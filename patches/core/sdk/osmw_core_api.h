/* osmw_core_api.h -- public C ABI between osmw_core.dll and feature modules.
 *
 * ABI 1 is x86 Windows only.  The core passes this table to a module; modules
 * do not link an import library and must not reach into core-private symbols.
 *
 * ABI RULES
 * ---------
 * - Every cross-DLL function uses __cdecl.  Raw MythWar hooks keep their own
 *   verified calling convention inside the core and never cross this ABI.
 * - Every public structure starts with struct_size.  Stand-alone structures
 *   also carry enough version/state information to reject incompatible data.
 * - ABI 1 minor releases append fields only.  Existing fields, constants and
 *   semantics never move or change.
 * - No allocation changes owner across this boundary.  Strings and buffers
 *   are either borrowed for the duration stated below or are caller-owned.
 * - OSMW_OWNER and OSMW_TOKEN are opaque values.  Zero is always invalid.
 */
#ifndef OSMW_CORE_API_H
#define OSMW_CORE_API_H

#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This ABI intentionally describes the 32-bit client.  Failing at compile
 * time is safer than silently producing an x64-compatible-looking module. */
typedef char osmw_abi_requires_32_bit_pointers[(sizeof(void *) == 4) ? 1 : -1];

#define OSMW_ABI_MAKE(major, minor) \
    ((((DWORD)(major)) << 16) | ((DWORD)(minor) & 0xFFFFu))
#define OSMW_ABI_MAJOR(version) ((DWORD)(version) >> 16)
#define OSMW_ABI_MINOR(version) ((DWORD)(version) & 0xFFFFu)

#define OSMW_CORE_ABI_V1_0 OSMW_ABI_MAKE(1, 0)
/* 1.1 appends seal_region.  Nothing before it moved, so a 1.0 module keeps
 * working untouched; a module that wants the new call tests for it with
 * OSMW_STRUCT_HAS rather than comparing versions. */
#define OSMW_CORE_ABI_V1_1 OSMW_ABI_MAKE(1, 1)
#define OSMW_CORE_ABI_CURRENT OSMW_CORE_ABI_V1_1

/* Feature versions use the same mechanical packing but are not core ABI
 * versions.  They are diagnostic/status values owned by each module. */
#define OSMW_MODULE_VERSION(major, minor) OSMW_ABI_MAKE((major), (minor))

/* Test whether a size-prefixed object includes a field appended at the tail. */
#define OSMW_FIELD_END(type, field) \
    (offsetof(type, field) + sizeof(((type *)0)->field))
#define OSMW_STRUCT_HAS(object, type, field) \
    ((object) != NULL && (object)->struct_size >= OSMW_FIELD_END(type, field))

typedef DWORD OSMW_OWNER;
typedef DWORD OSMW_TOKEN;
typedef LONG  OSMW_RESULT;

#define OSMW_INVALID_OWNER ((OSMW_OWNER)0u)
#define OSMW_INVALID_TOKEN ((OSMW_TOKEN)0u)

/* Results are fixed LONG values rather than a compiler-sized enum. */
#define OSMW_OK                       ((OSMW_RESULT)0)
#define OSMW_MOD_DISABLED             ((OSMW_RESULT)1)
#define OSMW_E_INVALID_ARGUMENT       ((OSMW_RESULT)-1)
#define OSMW_E_INCOMPATIBLE_ABI       ((OSMW_RESULT)-2)
#define OSMW_E_UNAVAILABLE            ((OSMW_RESULT)-3)
#define OSMW_E_NOT_FOUND              ((OSMW_RESULT)-4)
#define OSMW_E_MULTIPLE_MATCHES       ((OSMW_RESULT)-5)
#define OSMW_E_ALREADY_CLAIMED        ((OSMW_RESULT)-6)
#define OSMW_E_WRONG_OWNER            ((OSMW_RESULT)-7)
#define OSMW_E_BUSY                   ((OSMW_RESULT)-8)
#define OSMW_E_PATCH_STILL_ACTIVE     ((OSMW_RESULT)-9)
#define OSMW_E_INTERNAL               ((OSMW_RESULT)-10)
#define OSMW_E_BUFFER_TOO_SMALL       ((OSMW_RESULT)-11)
#define OSMW_E_QUEUE_FULL             ((OSMW_RESULT)-12)
#define OSMW_E_DUPLICATE_ID           ((OSMW_RESULT)-13)
#define OSMW_E_STOPPING               ((OSMW_RESULT)-14)

/* Log levels.  A module formats its message locally and passes one completed,
 * NUL-terminated string; the core never receives cross-CRT varargs. */
typedef DWORD OSMW_LOG_LEVEL;
#define OSMW_LOG_DEBUG ((OSMW_LOG_LEVEL)0u)
#define OSMW_LOG_INFO  ((OSMW_LOG_LEVEL)1u)
#define OSMW_LOG_WARN  ((OSMW_LOG_LEVEL)2u)
#define OSMW_LOG_ERROR ((OSMW_LOG_LEVEL)3u)

/* Capability bits describe services that are operational in this process.
 * Function pointers remain present in the ABI table and return
 * OSMW_E_UNAVAILABLE when their capability is absent. */
#define OSMW_CAP_CONFIG          0x00000001u
#define OSMW_CAP_SIGNATURES      0x00000002u
#define OSMW_CAP_REGION_CLAIMS   0x00000004u
#define OSMW_CAP_UI_TICK         0x00000008u
#define OSMW_CAP_GAME_PACKETS    0x00000010u
#define OSMW_CAP_SEND_QUEUE      0x00000020u
#define OSMW_CAP_MODULE_STATUS   0x00000040u

/* Runtime region kinds are diagnostic, but explicit values keep status/log
 * tooling stable.  Every kind is exclusive in ABI 1. */
typedef DWORD OSMW_REGION_KIND;
#define OSMW_REGION_CODE      ((OSMW_REGION_KIND)1u)
#define OSMW_REGION_CALL_SITE ((OSMW_REGION_KIND)2u)
#define OSMW_REGION_IAT       ((OSMW_REGION_KIND)3u)
#define OSMW_REGION_DATA      ((OSMW_REGION_KIND)4u)

/* Module states returned by module_status. */
typedef DWORD OSMW_MODULE_STATE;
#define OSMW_MODULE_UNKNOWN          ((OSMW_MODULE_STATE)0u)
#define OSMW_MODULE_MISSING          ((OSMW_MODULE_STATE)1u)
#define OSMW_MODULE_LEGACY           ((OSMW_MODULE_STATE)2u)
#define OSMW_MODULE_INCOMPATIBLE     ((OSMW_MODULE_STATE)3u)
#define OSMW_MODULE_LOADED           ((OSMW_MODULE_STATE)4u)
#define OSMW_MODULE_ACTIVE           ((OSMW_MODULE_STATE)5u)
#define OSMW_MODULE_DISABLED         ((OSMW_MODULE_STATE)6u)
#define OSMW_MODULE_FAILED           ((OSMW_MODULE_STATE)7u)
#define OSMW_MODULE_STOPPING         ((OSMW_MODULE_STATE)8u)
#define OSMW_MODULE_UNSAFE_TO_UNLOAD ((OSMW_MODULE_STATE)9u)
#define OSMW_MODULE_UNLOADED         ((OSMW_MODULE_STATE)10u)

#define OSMW_MODULE_ID_CAP           32u
#define OSMW_MODULE_FILENAME_CAP     64u
#define OSMW_MODULE_DISPLAY_NAME_CAP 64u

/* Callback order is ascending.  Negative priorities are reserved for core
 * internals; ordinary feature modules register at zero unless their own spec
 * documents a cross-feature ordering requirement. */
#define OSMW_PRIORITY_DEFAULT ((LONG)0)

#pragma pack(push, 4)

typedef struct OSMW_SIGNATURE_V1 {
    DWORD       struct_size;
    const char *name;             /* borrowed only during resolve_signature */
    const BYTE *pattern;          /* borrowed only during resolve_signature */
    const char *mask;             /* length bytes: 'x' exact, '?' wildcard */
    DWORD       length;
    LONG        operand_offset;   /* -1 match VA; >=0 absolute DWORD operand */
    DWORD       reference_rva;    /* expected resolved RVA; diagnostic only */
    DWORD       flags;            /* must be zero for ABI 1.0 */
} OSMW_SIGNATURE_V1;

typedef struct OSMW_RESOLVED_ADDRESS_V1 {
    DWORD       struct_size;
    DWORD       match_count;      /* normally 0, 1 or 2 (2 means 2-or-more) */
    void       *address;          /* borrowed process address; never free */
    DWORD       match_rva;        /* RVA of the pattern, not operand target */
    DWORD       reference_rva;    /* copied from the request */
    LONG        rva_delta;        /* resolved-address RVA - reference_rva */
} OSMW_RESOLVED_ADDRESS_V1;

typedef struct OSMW_REGION_CLAIM_V1 {
    DWORD       struct_size;
    void       *address;
    DWORD       length;
    OSMW_REGION_KIND kind;
    const char *label;            /* borrowed only during claim_region */
    DWORD       flags;            /* must be zero for ABI 1.0 */
} OSMW_REGION_CLAIM_V1;

/* Receive payload storage belongs to the client and remains valid only for the
 * current callback.  Copy anything needed after return. */
typedef struct OSMW_GAME_PACKET_V1 {
    DWORD       struct_size;
    DWORD       type;
    DWORD       field12;
    const void *extra;
    DWORD       extra_size;
} OSMW_GAME_PACKET_V1;

typedef DWORD OSMW_PACKET_ACTION;
#define OSMW_PACKET_PASS    ((OSMW_PACKET_ACTION)0u)
#define OSMW_PACKET_CONSUME ((OSMW_PACKET_ACTION)1u)

/* ABI 1 send-queue bounds.  Payload bytes are copied before the API call
 * returns; zero-length packets may pass NULL. */
#define OSMW_SEND_QUEUE_CAPACITY       32u
#define OSMW_SEND_PACKET_MAX_EXTRA   4096u

typedef struct OSMW_MODULE_STATUS_V1 {
    DWORD             struct_size;
    OSMW_MODULE_STATE state;
    DWORD             module_version;
    DWORD             required_core_abi;
    DWORD             owned_claim_count;
    DWORD             subscription_count;
    OSMW_RESULT       last_result;
    DWORD             reserved;
    char              module_id[OSMW_MODULE_ID_CAP];
    char              filename[OSMW_MODULE_FILENAME_CAP];
    char              display_name[OSMW_MODULE_DISPLAY_NAME_CAP];
} OSMW_MODULE_STATUS_V1;

struct OSMW_CORE_API_V1;

/* The API pointer is core-owned and valid from init entry until shutdown
 * returns.  The owner is issued by core and must accompany every resource. */
typedef OSMW_RESULT (__cdecl *OSMW_MOD_INIT_V1)(
    const struct OSMW_CORE_API_V1 *api,
    OSMW_OWNER owner);

/* Shutdown must restore/release every module-owned runtime claim.  Returning
 * an error makes the core keep both the module and itself mapped. */
typedef OSMW_RESULT (__cdecl *OSMW_MOD_SHUTDOWN_V1)(
    const struct OSMW_CORE_API_V1 *api,
    OSMW_OWNER owner);

typedef struct OSMW_MOD_DESCRIPTOR_V1 {
    DWORD                struct_size;
    DWORD                required_core_abi;
    DWORD                module_version;
    DWORD                flags;          /* must be zero for ABI 1.0 */
    const char          *module_id;      /* module-owned until DLL unload */
    const char          *display_name;   /* module-owned until DLL unload */
    OSMW_MOD_INIT_V1     init;
    OSMW_MOD_SHUTDOWN_V1 shutdown;
} OSMW_MOD_DESCRIPTOR_V1;

/* ABI 1 query requires this permanent prefix.  Future ABI 1 minors may append
 * descriptor fields while still accepting this much output space. */
#define OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE \
    ((DWORD)OSMW_FIELD_END(OSMW_MOD_DESCRIPTOR_V1, shutdown))

typedef void (__cdecl *OSMW_UI_TICK_CALLBACK_V1)(
    OSMW_OWNER owner,
    void *context,                     /* module-owned until unsubscribe */
    LONG list_selector);

typedef OSMW_PACKET_ACTION (__cdecl *OSMW_PACKET_HANDLER_V1)(
    OSMW_OWNER owner,
    void *context,                     /* module-owned until unsubscribe */
    const OSMW_GAME_PACKET_V1 *packet);

typedef void (__cdecl *OSMW_PACKET_OBSERVER_V1)(
    OSMW_OWNER owner,
    void *context,                     /* module-owned until unsubscribe */
    const OSMW_GAME_PACKET_V1 *packet);

/* Function typedefs keep __cdecl visible at every cross-DLL boundary and make
 * the function-table layout straightforward to assert. */
typedef const char *(__cdecl *OSMW_GAME_DIR_FN)(void);
typedef void (__cdecl *OSMW_LOG_FN)(
    OSMW_OWNER owner, OSMW_LOG_LEVEL level, const char *message);
typedef int (__cdecl *OSMW_CONFIG_READ_INT_FN)(
    OSMW_OWNER owner, const char *section, const char *key, int fallback);
typedef OSMW_RESULT (__cdecl *OSMW_CONFIG_READ_STRING_FN)(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size);
typedef OSMW_RESULT (__cdecl *OSMW_CONFIG_WRITE_STRING_FN)(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *value);
typedef OSMW_RESULT (__cdecl *OSMW_RESOLVE_SIGNATURE_FN)(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output);
typedef OSMW_RESULT (__cdecl *OSMW_CLAIM_REGION_FN)(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim,
    OSMW_TOKEN *token);
typedef OSMW_RESULT (__cdecl *OSMW_RELEASE_REGION_FN)(
    OSMW_OWNER owner, OSMW_TOKEN token);
/* Declare that the bytes now at a claim are the ones you intend.
 *
 * A claim snapshots the bytes you are about to overwrite, so that snapshot is
 * the pre-patch original and stays that way - it is what release_region makes
 * you restore.  It cannot also serve as "what should be here now": compared
 * against live memory it differs for every patch that is applied, which is the
 * normal state of a working mod.
 *
 * Sealing supplies the missing half.  Call it after every write you make to
 * the region: the first patch, any later change, and the restore on the way
 * out.  From then on the core can say that a difference was written by
 * something that does not own the range, and reports it in its attestation.
 *
 * A claim you never seal is simply not watched.  That is deliberate - the
 * alternative is treating your own correct patch as an intrusion, which is
 * exactly the bug this call exists to fix.
 *
 * Pass OSMW_INVALID_TOKEN to seal every claim you own.  The natural place to
 * call this is whatever helper your module writes memory through, and that
 * helper knows the address but rarely the token covering it, so the bulk form is
 * what makes sealing a one-line adoption instead of a token-threading exercise.
 * It reaches only your own claims either way.
 */
typedef OSMW_RESULT (__cdecl *OSMW_SEAL_REGION_FN)(
    OSMW_OWNER owner, OSMW_TOKEN token);
typedef OSMW_RESULT (__cdecl *OSMW_SUBSCRIBE_POST_UI_TICK_FN)(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token);
typedef OSMW_RESULT (__cdecl *OSMW_UNSUBSCRIBE_FN)(
    OSMW_OWNER owner, OSMW_TOKEN token);
typedef OSMW_RESULT (__cdecl *OSMW_CLAIM_GAME_PACKET_FN)(
    OSMW_OWNER owner, DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback,
    void *context, OSMW_TOKEN *token);
typedef OSMW_RESULT (__cdecl *OSMW_OBSERVE_GAME_PACKET_AFTER_FN)(
    OSMW_OWNER owner, DWORD packet_type, LONG priority,
    OSMW_PACKET_OBSERVER_V1 callback, void *context, OSMW_TOKEN *token);
typedef OSMW_RESULT (__cdecl *OSMW_QUEUE_GAME_PACKET_FN)(
    OSMW_OWNER owner, DWORD packet_type, DWORD field12,
    const void *extra, DWORD extra_size);
typedef OSMW_RESULT (__cdecl *OSMW_MODULE_STATUS_FN)(
    const char *module_id_or_filename, OSMW_MODULE_STATUS_V1 *output);

typedef struct OSMW_CORE_API_V1 {
    DWORD                             struct_size;
    DWORD                             abi_version;
    DWORD                             capabilities;
    DWORD                             reserved;
    OSMW_GAME_DIR_FN                  game_dir;
    OSMW_LOG_FN                       log;
    OSMW_CONFIG_READ_INT_FN           config_read_int;
    OSMW_CONFIG_READ_STRING_FN        config_read_string;
    OSMW_CONFIG_WRITE_STRING_FN       config_write_string;
    OSMW_RESOLVE_SIGNATURE_FN         resolve_signature;
    OSMW_CLAIM_REGION_FN              claim_region;
    OSMW_RELEASE_REGION_FN            release_region;
    OSMW_SUBSCRIBE_POST_UI_TICK_FN    subscribe_post_ui_tick;
    OSMW_UNSUBSCRIBE_FN               unsubscribe;
    OSMW_CLAIM_GAME_PACKET_FN         claim_game_packet;
    OSMW_OBSERVE_GAME_PACKET_AFTER_FN observe_game_packet_after;
    OSMW_QUEUE_GAME_PACKET_FN         queue_game_packet;
    OSMW_MODULE_STATUS_FN             module_status;
    /* ABI 1.1.  Test with OSMW_STRUCT_HAS before calling. */
    OSMW_SEAL_REGION_FN               seal_region;
} OSMW_CORE_API_V1;

#define OSMW_CORE_API_V1_0_SIZE \
    ((DWORD)OSMW_FIELD_END(OSMW_CORE_API_V1, module_status))
#define OSMW_CORE_API_V1_1_SIZE \
    ((DWORD)OSMW_FIELD_END(OSMW_CORE_API_V1, seal_region))

#pragma pack(pop)

/* This exact export name is preflighted from the DLL's PE export directory
 * before LoadLibrary is allowed to run its passive DllMain. */
#define OSMW_MOD_QUERY_EXPORT_NAME "osmw_mod_query"

typedef OSMW_RESULT (__cdecl *OSMW_MOD_QUERY_V1)(
    DWORD core_abi_version,
    DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output);

/* Feature DLLs implement this and list it undecorated in their .def file.
 * Query is side-effect free.  It writes no more than output_size bytes. */
OSMW_RESULT __cdecl osmw_mod_query(
    DWORD core_abi_version,
    DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OSMW_CORE_API_H */
