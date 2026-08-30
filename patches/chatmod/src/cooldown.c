/* Replace "Please do not flood !" with the time actually left.
 *
 * The cooldown is enforced server side, so nothing here changes what is
 * allowed -- the client already knows the numbers and simply declines to say
 * them. All this does is answer the question the stock message raises.
 *
 * THE CLIENT'S OWN GATE, at 0x00409176 in the chat-send path:
 *
 *     mov edx, [lastSend]     ; when the last message went out, in ms
 *     add edx, eax            ; + the cooldown for this channel
 *     mov eax, [now]          ; the client's millisecond clock
 *     cmp eax, edx
 *     jbe  ...                ; too soon -> "#R#S Please do not flood !#N"
 *     mov [lastSend], eax     ; allowed -> remember when
 *
 * and the cooldown in eax comes from a ten-entry jump table indexed by the
 * current channel, each entry landing on a `mov eax, imm32`:
 *
 *     Local Party PM System Info FD   2s
 *     Guild                          3s
 *     Market GM                     30s
 *     World          15s, or 10s when a flag is set and a counter is >= 50
 *
 * Everything is read out of the client rather than copied here: the table is
 * followed at runtime and the constants taken from the instructions it points
 * at, so if a future build rebalances a cooldown this follows it. Only the
 * SHAPE of those instructions is assumed, and an unrecognised shape falls back
 * to the largest constant in the block -- erring towards waiting too long
 * rather than promising too soon.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

/* Offsets into the two matched instruction sequences. Each signature pins
 * these exact bytes, so the operands sit at fixed positions. */
#define DISPATCH_CHANNEL_AT 2       /* mov esi, [channel]        */
#define DISPATCH_TABLE_AT   17      /* jmp [esi*4 + table]       */
#define GATE_LASTSEND_AT    2       /* mov edx, [lastSend]       */
#define GATE_NOW_AT         9       /* mov eax, [now]            */

/* The call the client makes to put the flood message in the chat. */
#define FLOOD_CALL_DELTA    14

#define CHANNEL_MAX 9

typedef void (__cdecl *add_message_fn)(int channel, int flags,
                                       const char *name, const char *text);

static mw_hook_t      g_hook;
static add_message_fn g_real_add;
static int            g_hooked;

static const DWORD *g_channel;      /* current chat channel, 0..9 */
static const DWORD *g_last_send;    /* ms timestamp of the last accepted send */
static const DWORD *g_now;          /* the client's millisecond clock */
static const DWORD *g_table;        /* ten code addresses, one per channel */

static int   g_enabled = 1;
static char  g_format[128];

static const char *const CHANNEL_LABEL[CHANNEL_MAX + 1] = {
    "Local", "Party", "PM", "Guild", "Market",
    "World", "System", "Info", "FD", "GM"
};

void mw_cooldown_refresh(void)
{
    g_enabled = mw_cfg_int("CooldownMessage", 1) != 0;
    _snprintf(g_format, sizeof(g_format), "%s",
              mw_cfg_str("CooldownFormat",
                         "#R#S Wait %d more second%s before posting in %s "
                         "again!#N"));
    g_format[sizeof(g_format) - 1] = '\0';
}

/* Largest `mov eax, imm32` in a short block -- the conservative reading when
 * the block is not a shape we recognise. */
static DWORD largest_constant(const unsigned char *p)
{
    DWORD best = 0;
    int   i;

    for (i = 0; i < 0x20; i++) {
        if (p[i] == 0xB8) {
            DWORD v = *(const DWORD *)(p + i + 1);
            if (v > best && v <= 5 * 60 * 1000)
                best = v;
        }
    }
    return best;
}

/* The cooldown the client would apply to this channel, in milliseconds. */
static DWORD cooldown_for(int channel)
{
    const unsigned char *p;

    if (!g_table || channel < 0 || channel > CHANNEL_MAX)
        return 0;

    p = (const unsigned char *)g_table[channel];
    if (!p)
        return 0;

    /* mov eax, imm32 -- the plain case, six channels of ten. */
    if (p[0] == 0xB8)
        return *(const DWORD *)(p + 1);

    /* The World shape:
     *   8A 0D <byte flag>  84 C9  B8 <short>  74 ..  83 3D <counter> <n>
     *   73 ..  B8 <long>
     * i.e. short cooldown only when the flag is set AND the counter reaches n. */
    if (p[0] == 0x8A && p[1] == 0x0D && p[6] == 0x84 && p[8] == 0xB8 &&
        p[15] == 0x83 && p[16] == 0x3D) {
        const unsigned char *flag    = *(const unsigned char *const *)(p + 2);
        const DWORD         *counter = *(const DWORD *const *)(p + 17);
        DWORD                shortms = *(const DWORD *)(p + 9);
        DWORD                longms  = (p[24] == 0xB8) ? *(const DWORD *)(p + 25)
                                                       : largest_constant(p);
        unsigned char        threshold = p[21];

        if (flag && *flag && counter && *counter >= threshold)
            return shortms;
        return longms;
    }

    return largest_constant(p);
}

/* Seconds still to wait, or 0 if the gate would not actually be closed. */
static int seconds_remaining(int channel)
{
    DWORD due, now;

    if (!g_last_send || !g_now)
        return 0;

    due = *g_last_send + cooldown_for(channel);
    now = *g_now;
    if (now > due)
        return 0;

    /* Round up: "wait 0 seconds" helps nobody, and the gate is `now <= due`,
     * so a fractional second still counts. */
    return (int)((due - now) / 1000u) + 1;
}

static void __cdecl flood_detour(int channel, int flags, const char *name,
                                 const char *text)
{
    char message[256];
    int  active, seconds;

    if (!g_enabled || !g_channel) {
        g_real_add(channel, flags, name, text);
        return;
    }

    active = (int)*g_channel;
    seconds = seconds_remaining(active);
    if (seconds <= 0) {
        /* The gate closed for a reason we could not reconstruct. Say the
         * client's own thing rather than something made up. */
        g_real_add(channel, flags, name, text);
        return;
    }

    _snprintf(message, sizeof(message), g_format, seconds,
              (seconds == 1) ? "" : "s",
              (active >= 0 && active <= CHANNEL_MAX) ? CHANNEL_LABEL[active]
                                                     : "this channel");
    message[sizeof(message) - 1] = '\0';
    g_real_add(channel, flags, name, message);
}

int mw_cooldown_install(void)
{
    const unsigned char *dispatch = mw_addr[MW_CHATDISPATCH];
    const unsigned char *gate     = mw_addr[MW_FLOODGATE];
    unsigned char       *site     = mw_addr[MW_FLOODMESSAGE];
    void                *original = NULL;

    if (g_hooked)
        return 1;
    if (!dispatch || !gate || !site) {
        mw_log("cooldown: send path unresolved; leaving the stock message");
        return 0;
    }

    g_channel   = *(const DWORD *const *)(dispatch + DISPATCH_CHANNEL_AT);
    g_table     = *(const DWORD *const *)(dispatch + DISPATCH_TABLE_AT);
    g_last_send = *(const DWORD *const *)(gate + GATE_LASTSEND_AT);
    g_now       = *(const DWORD *const *)(gate + GATE_NOW_AT);

    mw_cooldown_refresh();

    site += FLOOD_CALL_DELTA;
    if (!mw_patch_direct_call(&g_hook, site,
                              (void *)(UINT_PTR)flood_detour, &original)) {
        mw_log("cooldown: could not patch the flood message call");
        return 0;
    }
    memcpy(&g_real_add, &original, sizeof(g_real_add));

    g_hooked = 1;
    mw_log("cooldown: channel %p, lastSend %p, clock %p, table %p",
           g_channel, g_last_send, g_now, g_table);
    {
        int i;
        for (i = 0; i <= CHANNEL_MAX; i++)
            mw_log("cooldown:   %-6s %u ms", CHANNEL_LABEL[i],
                   (unsigned)cooldown_for(i));
    }
    return 1;
}

void mw_cooldown_remove(void)
{
    if (!g_hooked)
        return;
    mw_hook_remove(&g_hook);
    g_hooked = 0;
}
