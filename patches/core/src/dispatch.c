/* Post-UiTick/packet registries, copied send queue, and shutdown quiescence.
 *
 * hook.c owns the one live client detour.  This file owns its concurrency
 * rules: callbacks run from a stable snapshot without the registry lock,
 * mutations during dispatch affect the next pass, and stopping prevents new
 * callback passes while retaining the stock original call.
 */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define OSMW_UI_SUBSCRIPTION_CAP 64u
#define OSMW_PACKET_HANDLER_CAP  64u
#define OSMW_PACKET_OBSERVER_CAP 128u

typedef struct OSMW_UI_SUBSCRIPTION {
    OSMW_TOKEN token;
    OSMW_OWNER owner;
    LONG priority;
    DWORD order;
    OSMW_UI_TICK_CALLBACK_V1 callback;
    void *context;
    BOOL pending_remove;
} OSMW_UI_SUBSCRIPTION;

typedef struct OSMW_UI_SNAPSHOT {
    OSMW_OWNER owner;
    LONG priority;
    DWORD order;
    OSMW_UI_TICK_CALLBACK_V1 callback;
    void *context;
} OSMW_UI_SNAPSHOT;

typedef struct OSMW_PACKET_HANDLER_SUBSCRIPTION {
    OSMW_TOKEN token;
    OSMW_OWNER owner;
    DWORD packet_type;
    OSMW_PACKET_HANDLER_V1 callback;
    void *context;
    BOOL pending_remove;
} OSMW_PACKET_HANDLER_SUBSCRIPTION;

typedef struct OSMW_PACKET_OBSERVER_SUBSCRIPTION {
    OSMW_TOKEN token;
    OSMW_OWNER owner;
    DWORD packet_type;
    LONG priority;
    DWORD order;
    OSMW_PACKET_OBSERVER_V1 callback;
    void *context;
    BOOL pending_remove;
} OSMW_PACKET_OBSERVER_SUBSCRIPTION;

typedef struct OSMW_PACKET_OBSERVER_SNAPSHOT {
    OSMW_OWNER owner;
    LONG priority;
    DWORD order;
    OSMW_PACKET_OBSERVER_V1 callback;
    void *context;
} OSMW_PACKET_OBSERVER_SNAPSHOT;

typedef struct OSMW_SEND_QUEUE_ENTRY {
    DWORD type;
    DWORD field12;
    DWORD extra_size;
    BYTE extra[OSMW_SEND_PACKET_MAX_EXTRA];
} OSMW_SEND_QUEUE_ENTRY;

static BOOL g_dispatch_ready;
static BOOL g_dispatch_stopping;
static CRITICAL_SECTION g_dispatch_lock;
static HANDLE g_quiescent_event;
static DWORD g_in_flight;
static DWORD g_next_order;
static OSMW_TOKEN g_next_subscription_token;
static OSMW_UI_SUBSCRIPTION g_ui_subscriptions[OSMW_UI_SUBSCRIPTION_CAP];
static OSMW_PACKET_HANDLER_SUBSCRIPTION
    g_packet_handlers[OSMW_PACKET_HANDLER_CAP];
static OSMW_PACKET_OBSERVER_SUBSCRIPTION
    g_packet_observers[OSMW_PACKET_OBSERVER_CAP];
static OSMW_SEND_QUEUE_ENTRY g_send_queue[OSMW_SEND_QUEUE_CAPACITY];
static DWORD g_send_head;
static DWORD g_send_count;
static BOOL g_send_flushing;
static PVOID g_send_target;

static BOOL valid_owner(OSMW_OWNER owner)
{
    char module_id[OSMW_MODULE_ID_CAP];
    return osmw_core_owner_copy_id(owner, module_id, sizeof(module_id));
}

static OSMW_TOKEN next_subscription_token(void)
{
    do {
        ++g_next_subscription_token;
    } while (g_next_subscription_token == OSMW_INVALID_TOKEN);
    return g_next_subscription_token;
}

static void apply_pending_removals(void)
{
    DWORD index;
    for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
        if (g_ui_subscriptions[index].pending_remove) {
            ZeroMemory(&g_ui_subscriptions[index],
                       sizeof(g_ui_subscriptions[index]));
        }
    }
    for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
        if (g_packet_handlers[index].pending_remove) {
            ZeroMemory(&g_packet_handlers[index],
                       sizeof(g_packet_handlers[index]));
        }
    }
    for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
        if (g_packet_observers[index].pending_remove) {
            ZeroMemory(&g_packet_observers[index],
                       sizeof(g_packet_observers[index]));
        }
    }
}

static void sort_ui_snapshot(OSMW_UI_SNAPSHOT *snapshot, DWORD count)
{
    DWORD index;
    for (index = 1; index < count; ++index) {
        OSMW_UI_SNAPSHOT value = snapshot[index];
        DWORD position = index;
        while (position != 0 &&
                (snapshot[position - 1].priority > value.priority ||
                 (snapshot[position - 1].priority == value.priority &&
                  snapshot[position - 1].order > value.order))) {
            snapshot[position] = snapshot[position - 1];
            --position;
        }
        snapshot[position] = value;
    }
}

static void sort_packet_snapshot(
    OSMW_PACKET_OBSERVER_SNAPSHOT *snapshot, DWORD count)
{
    DWORD index;
    for (index = 1; index < count; ++index) {
        OSMW_PACKET_OBSERVER_SNAPSHOT value = snapshot[index];
        DWORD position = index;
        while (position != 0 &&
                (snapshot[position - 1].priority > value.priority ||
                 (snapshot[position - 1].priority == value.priority &&
                  snapshot[position - 1].order > value.order))) {
            snapshot[position] = snapshot[position - 1];
            --position;
        }
        snapshot[position] = value;
    }
}

static void begin_in_flight(void)
{
    if (g_in_flight++ == 0) {
        ResetEvent(g_quiescent_event);
    }
}

static void end_in_flight(void)
{
    --g_in_flight;
    if (g_in_flight == 0) {
        apply_pending_removals();
        SetEvent(g_quiescent_event);
    }
}

BOOL osmw_dispatch_init(void)
{
    if (g_dispatch_ready) {
        return TRUE;
    }
    ZeroMemory(g_ui_subscriptions, sizeof(g_ui_subscriptions));
    ZeroMemory(g_packet_handlers, sizeof(g_packet_handlers));
    ZeroMemory(g_packet_observers, sizeof(g_packet_observers));
    ZeroMemory(g_send_queue, sizeof(g_send_queue));
    g_dispatch_stopping = FALSE;
    g_in_flight = 0;
    g_next_order = 0;
    g_next_subscription_token = OSMW_INVALID_TOKEN;
    g_send_head = 0;
    g_send_count = 0;
    g_send_flushing = FALSE;
    g_send_target = NULL;
    InitializeCriticalSection(&g_dispatch_lock);
    g_quiescent_event = CreateEventA(NULL, TRUE, TRUE, NULL);
    if (g_quiescent_event == NULL) {
        DeleteCriticalSection(&g_dispatch_lock);
        return FALSE;
    }
    g_dispatch_ready = TRUE;
    return TRUE;
}

void osmw_dispatch_reset(void)
{
    if (!g_dispatch_ready) {
        return;
    }
    EnterCriticalSection(&g_dispatch_lock);
    ZeroMemory(g_ui_subscriptions, sizeof(g_ui_subscriptions));
    ZeroMemory(g_packet_handlers, sizeof(g_packet_handlers));
    ZeroMemory(g_packet_observers, sizeof(g_packet_observers));
    ZeroMemory(g_send_queue, sizeof(g_send_queue));
    g_dispatch_stopping = TRUE;
    g_in_flight = 0;
    g_send_head = 0;
    g_send_count = 0;
    g_send_flushing = FALSE;
    g_send_target = NULL;
    SetEvent(g_quiescent_event);
    LeaveCriticalSection(&g_dispatch_lock);
    CloseHandle(g_quiescent_event);
    g_quiescent_event = NULL;
    DeleteCriticalSection(&g_dispatch_lock);
    g_dispatch_ready = FALSE;
}

OSMW_RESULT osmw_dispatch_subscribe_ui(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    DWORD index;

    if (token != NULL) {
        *token = OSMW_INVALID_TOKEN;
    }
    if (!g_dispatch_ready || !valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (callback == NULL || token == NULL || priority < 0) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_dispatch_lock);
    if (g_dispatch_stopping) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_STOPPING;
    }
    for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
        OSMW_UI_SUBSCRIPTION *subscription = &g_ui_subscriptions[index];
        if (subscription->token != OSMW_INVALID_TOKEN ||
                subscription->pending_remove) {
            continue;
        }
        subscription->token = next_subscription_token();
        subscription->owner = owner;
        subscription->priority = priority;
        subscription->order = ++g_next_order;
        subscription->callback = callback;
        subscription->context = context;
        *token = subscription->token;
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_OK;
    }
    LeaveCriticalSection(&g_dispatch_lock);
    return OSMW_E_BUSY;
}

OSMW_RESULT osmw_dispatch_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    DWORD index;

    if (!g_dispatch_ready || !valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (token == OSMW_INVALID_TOKEN) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_dispatch_lock);
    for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
        OSMW_UI_SUBSCRIPTION *subscription = &g_ui_subscriptions[index];
        if (subscription->token != token) {
            continue;
        }
        if (subscription->owner != owner) {
            LeaveCriticalSection(&g_dispatch_lock);
            return OSMW_E_WRONG_OWNER;
        }
        if (g_in_flight != 0) {
            subscription->pending_remove = TRUE;
        } else {
            ZeroMemory(subscription, sizeof(*subscription));
        }
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_OK;
    }
    for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
        OSMW_PACKET_HANDLER_SUBSCRIPTION *subscription =
            &g_packet_handlers[index];
        if (subscription->token != token) {
            continue;
        }
        if (subscription->owner != owner) {
            LeaveCriticalSection(&g_dispatch_lock);
            return OSMW_E_WRONG_OWNER;
        }
        if (g_in_flight != 0) {
            subscription->pending_remove = TRUE;
        } else {
            ZeroMemory(subscription, sizeof(*subscription));
        }
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_OK;
    }
    for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
        OSMW_PACKET_OBSERVER_SUBSCRIPTION *subscription =
            &g_packet_observers[index];
        if (subscription->token != token) {
            continue;
        }
        if (subscription->owner != owner) {
            LeaveCriticalSection(&g_dispatch_lock);
            return OSMW_E_WRONG_OWNER;
        }
        if (g_in_flight != 0) {
            subscription->pending_remove = TRUE;
        } else {
            ZeroMemory(subscription, sizeof(*subscription));
        }
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_OK;
    }
    LeaveCriticalSection(&g_dispatch_lock);
    return OSMW_E_NOT_FOUND;
}

OSMW_RESULT osmw_dispatch_claim_packet(
    OSMW_OWNER owner, DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    DWORD index;
    DWORD empty = OSMW_PACKET_HANDLER_CAP;

    if (token != NULL) {
        *token = OSMW_INVALID_TOKEN;
    }
    if (!g_dispatch_ready || !valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (callback == NULL || token == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_dispatch_lock);
    if (g_dispatch_stopping) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_STOPPING;
    }
    for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
        OSMW_PACKET_HANDLER_SUBSCRIPTION *subscription =
            &g_packet_handlers[index];
        if (subscription->token == OSMW_INVALID_TOKEN &&
                !subscription->pending_remove &&
                empty == OSMW_PACKET_HANDLER_CAP) {
            empty = index;
        } else if (subscription->token != OSMW_INVALID_TOKEN &&
                !subscription->pending_remove &&
                subscription->packet_type == packet_type) {
            LeaveCriticalSection(&g_dispatch_lock);
            return OSMW_E_ALREADY_CLAIMED;
        }
    }
    if (empty == OSMW_PACKET_HANDLER_CAP) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_BUSY;
    }
    g_packet_handlers[empty].token = next_subscription_token();
    g_packet_handlers[empty].owner = owner;
    g_packet_handlers[empty].packet_type = packet_type;
    g_packet_handlers[empty].callback = callback;
    g_packet_handlers[empty].context = context;
    *token = g_packet_handlers[empty].token;
    LeaveCriticalSection(&g_dispatch_lock);
    return OSMW_OK;
}

OSMW_RESULT osmw_dispatch_observe_packet(
    OSMW_OWNER owner, DWORD packet_type, LONG priority,
    OSMW_PACKET_OBSERVER_V1 callback, void *context, OSMW_TOKEN *token)
{
    DWORD index;

    if (token != NULL) {
        *token = OSMW_INVALID_TOKEN;
    }
    if (!g_dispatch_ready || !valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (callback == NULL || token == NULL || priority < 0) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_dispatch_lock);
    if (g_dispatch_stopping) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_STOPPING;
    }
    for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
        OSMW_PACKET_OBSERVER_SUBSCRIPTION *subscription =
            &g_packet_observers[index];
        if (subscription->token != OSMW_INVALID_TOKEN ||
                subscription->pending_remove) {
            continue;
        }
        subscription->token = next_subscription_token();
        subscription->owner = owner;
        subscription->packet_type = packet_type;
        subscription->priority = priority;
        subscription->order = ++g_next_order;
        subscription->callback = callback;
        subscription->context = context;
        *token = subscription->token;
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_OK;
    }
    LeaveCriticalSection(&g_dispatch_lock);
    return OSMW_E_BUSY;
}

OSMW_RESULT osmw_dispatch_queue_packet(
    OSMW_OWNER owner, DWORD packet_type, DWORD field12,
    const void *extra, DWORD extra_size)
{
    DWORD tail;
    OSMW_SEND_QUEUE_ENTRY *entry;

    if (!g_dispatch_ready || !valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (extra_size > OSMW_SEND_PACKET_MAX_EXTRA) {
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    if (extra_size != 0 && extra == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_dispatch_lock);
    if (g_dispatch_stopping) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_STOPPING;
    }
    if (g_send_target == NULL) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_UNAVAILABLE;
    }
    if (g_send_count == OSMW_SEND_QUEUE_CAPACITY) {
        LeaveCriticalSection(&g_dispatch_lock);
        return OSMW_E_QUEUE_FULL;
    }
    tail = (g_send_head + g_send_count) % OSMW_SEND_QUEUE_CAPACITY;
    entry = &g_send_queue[tail];
    entry->type = packet_type;
    entry->field12 = field12;
    entry->extra_size = extra_size;
    if (extra_size != 0) {
        memcpy(entry->extra, extra, extra_size);
    }
    ++g_send_count;
    LeaveCriticalSection(&g_dispatch_lock);
    return OSMW_OK;
}

DWORD osmw_dispatch_owner_count(OSMW_OWNER owner)
{
    DWORD index;
    DWORD count = 0;

    if (!g_dispatch_ready || owner == OSMW_INVALID_OWNER) {
        return 0;
    }
    EnterCriticalSection(&g_dispatch_lock);
    for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
        if (g_ui_subscriptions[index].token != OSMW_INVALID_TOKEN &&
                g_ui_subscriptions[index].owner == owner) {
            ++count;
        }
    }
    for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
        if (g_packet_handlers[index].token != OSMW_INVALID_TOKEN &&
                g_packet_handlers[index].owner == owner) {
            ++count;
        }
    }
    for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
        if (g_packet_observers[index].token != OSMW_INVALID_TOKEN &&
                g_packet_observers[index].owner == owner) {
            ++count;
        }
    }
    LeaveCriticalSection(&g_dispatch_lock);
    return count;
}

DWORD osmw_dispatch_total_count(void)
{
    DWORD index;
    DWORD count = 0;

    if (!g_dispatch_ready) {
        return 0;
    }
    EnterCriticalSection(&g_dispatch_lock);
    for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
        if (g_ui_subscriptions[index].token != OSMW_INVALID_TOKEN) {
            ++count;
        }
    }
    for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
        if (g_packet_handlers[index].token != OSMW_INVALID_TOKEN) {
            ++count;
        }
    }
    for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
        if (g_packet_observers[index].token != OSMW_INVALID_TOKEN) {
            ++count;
        }
    }
    LeaveCriticalSection(&g_dispatch_lock);
    return count;
}

void osmw_dispatch_set_send_target(void *target)
{
    if (!g_dispatch_ready) {
        return;
    }
    EnterCriticalSection(&g_dispatch_lock);
    g_send_target = target;
    LeaveCriticalSection(&g_dispatch_lock);
}

BOOL osmw_dispatch_send_available(void)
{
    BOOL available;

    if (!g_dispatch_ready) {
        return FALSE;
    }
    EnterCriticalSection(&g_dispatch_lock);
    available = g_send_target != NULL;
    LeaveCriticalSection(&g_dispatch_lock);
    return available;
}

static void flush_send_queue(void)
{
    DWORD budget;
    DWORD index;

    EnterCriticalSection(&g_dispatch_lock);
    if (g_dispatch_stopping || g_send_flushing || g_send_target == NULL) {
        LeaveCriticalSection(&g_dispatch_lock);
        return;
    }
    g_send_flushing = TRUE;
    budget = g_send_count;
    LeaveCriticalSection(&g_dispatch_lock);

    for (index = 0; index < budget; ++index) {
        OSMW_SEND_QUEUE_ENTRY *entry;
        OSMW_SEND_PACKET_FN send;
        DWORD type;
        DWORD field12;
        DWORD extra_size;
        const void *extra;

        EnterCriticalSection(&g_dispatch_lock);
        if (g_send_count == 0 || g_send_target == NULL) {
            LeaveCriticalSection(&g_dispatch_lock);
            break;
        }
        entry = &g_send_queue[g_send_head];
        send = (OSMW_SEND_PACKET_FN)g_send_target;
        type = entry->type;
        field12 = entry->field12;
        extra_size = entry->extra_size;
        extra = extra_size != 0 ? entry->extra : NULL;
        LeaveCriticalSection(&g_dispatch_lock);

        (void)send(type, field12, extra, extra_size);

        EnterCriticalSection(&g_dispatch_lock);
        ZeroMemory(entry, sizeof(*entry));
        g_send_head = (g_send_head + 1u) % OSMW_SEND_QUEUE_CAPACITY;
        --g_send_count;
        LeaveCriticalSection(&g_dispatch_lock);
    }

    EnterCriticalSection(&g_dispatch_lock);
    g_send_flushing = FALSE;
    LeaveCriticalSection(&g_dispatch_lock);
}

void osmw_dispatch_ui_invoke(
    OSMW_UI_ORIGINAL_FN original, LONG list_selector)
{
    OSMW_UI_SNAPSHOT snapshot[OSMW_UI_SUBSCRIPTION_CAP];
    DWORD snapshot_count = 0;
    DWORD index;
    BOOL allow_module_work = FALSE;

    if (!g_dispatch_ready) {
        if (original != NULL) {
            original(list_selector);
        }
        return;
    }
    EnterCriticalSection(&g_dispatch_lock);
    begin_in_flight();
    if (!g_dispatch_stopping) {
        allow_module_work = TRUE;
        for (index = 0; index < OSMW_UI_SUBSCRIPTION_CAP; ++index) {
            OSMW_UI_SUBSCRIPTION *subscription =
                &g_ui_subscriptions[index];
            if (subscription->token == OSMW_INVALID_TOKEN ||
                    subscription->pending_remove) {
                continue;
            }
            snapshot[snapshot_count].owner = subscription->owner;
            snapshot[snapshot_count].priority = subscription->priority;
            snapshot[snapshot_count].order = subscription->order;
            snapshot[snapshot_count].callback = subscription->callback;
            snapshot[snapshot_count].context = subscription->context;
            ++snapshot_count;
        }
    }
    LeaveCriticalSection(&g_dispatch_lock);

    if (original != NULL) {
        original(list_selector);
    }
    sort_ui_snapshot(snapshot, snapshot_count);
    for (index = 0; index < snapshot_count; ++index) {
        snapshot[index].callback(snapshot[index].owner,
                                 snapshot[index].context, list_selector);
    }
    if (allow_module_work) {
        flush_send_queue();
    }

    EnterCriticalSection(&g_dispatch_lock);
    end_in_flight();
    LeaveCriticalSection(&g_dispatch_lock);
}

void osmw_dispatch_packet_invoke(
    OSMW_PACKET_ORIGINAL_FN original, DWORD type, DWORD field12,
    const void *extra, DWORD extra_size)
{
    OSMW_PACKET_HANDLER_SUBSCRIPTION handler;
    OSMW_PACKET_OBSERVER_SNAPSHOT
        observers[OSMW_PACKET_OBSERVER_CAP];
    OSMW_GAME_PACKET_V1 packet;
    OSMW_PACKET_ACTION action = OSMW_PACKET_PASS;
    DWORD observer_count = 0;
    DWORD index;
    BOOL have_handler = FALSE;
    BOOL stock_called = FALSE;

    if (!g_dispatch_ready) {
        if (original != NULL) {
            original(type, field12, extra, extra_size);
        }
        return;
    }
    ZeroMemory(&handler, sizeof(handler));
    ZeroMemory(&packet, sizeof(packet));
    packet.struct_size = sizeof(packet);
    packet.type = type;
    packet.field12 = field12;
    packet.extra = extra;
    packet.extra_size = extra_size;

    EnterCriticalSection(&g_dispatch_lock);
    begin_in_flight();
    if (!g_dispatch_stopping) {
        for (index = 0; index < OSMW_PACKET_HANDLER_CAP; ++index) {
            OSMW_PACKET_HANDLER_SUBSCRIPTION *subscription =
                &g_packet_handlers[index];
            if (subscription->token != OSMW_INVALID_TOKEN &&
                    !subscription->pending_remove &&
                    subscription->packet_type == type) {
                handler = *subscription;
                have_handler = TRUE;
                break;
            }
        }
        for (index = 0; index < OSMW_PACKET_OBSERVER_CAP; ++index) {
            OSMW_PACKET_OBSERVER_SUBSCRIPTION *subscription =
                &g_packet_observers[index];
            if (subscription->token == OSMW_INVALID_TOKEN ||
                    subscription->pending_remove ||
                    subscription->packet_type != type) {
                continue;
            }
            observers[observer_count].owner = subscription->owner;
            observers[observer_count].priority = subscription->priority;
            observers[observer_count].order = subscription->order;
            observers[observer_count].callback = subscription->callback;
            observers[observer_count].context = subscription->context;
            ++observer_count;
        }
    }
    LeaveCriticalSection(&g_dispatch_lock);

    if (have_handler) {
        action = handler.callback(handler.owner, handler.context, &packet);
        if (action != OSMW_PACKET_PASS &&
                action != OSMW_PACKET_CONSUME) {
            char message[128];
            int written = _snprintf(
                message, sizeof(message),
                "invalid packet action %lu for type 0x%08lX; treating as PASS",
                (unsigned long)action, (unsigned long)type);
            if (written < 0 || (DWORD)written >= sizeof(message)) {
                message[sizeof(message) - 1] = '\0';
            }
            osmw_api_log(handler.owner, OSMW_LOG_WARN, message);
            action = OSMW_PACKET_PASS;
        }
    }
    if (action != OSMW_PACKET_CONSUME && original != NULL) {
        original(type, field12, extra, extra_size);
        stock_called = TRUE;
    }
    if (stock_called) {
        sort_packet_snapshot(observers, observer_count);
        for (index = 0; index < observer_count; ++index) {
            observers[index].callback(
                observers[index].owner, observers[index].context, &packet);
        }
    }

    EnterCriticalSection(&g_dispatch_lock);
    end_in_flight();
    LeaveCriticalSection(&g_dispatch_lock);
}

void osmw_dispatch_begin_stop(void)
{
    if (!g_dispatch_ready) {
        return;
    }
    EnterCriticalSection(&g_dispatch_lock);
    g_dispatch_stopping = TRUE;
    LeaveCriticalSection(&g_dispatch_lock);
}

void osmw_dispatch_resume(void)
{
    if (!g_dispatch_ready) {
        return;
    }
    EnterCriticalSection(&g_dispatch_lock);
    g_dispatch_stopping = FALSE;
    LeaveCriticalSection(&g_dispatch_lock);
}

BOOL osmw_dispatch_wait_quiescent(DWORD timeout_ms)
{
    if (!g_dispatch_ready) {
        return TRUE;
    }
    return WaitForSingleObject(g_quiescent_event, timeout_ms) == WAIT_OBJECT_0;
}
