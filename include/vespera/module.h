#ifndef VESPERA_MODULE_H
#define VESPERA_MODULE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VS_MODULE_EXPORT __declspec(dllexport)
#else
#define VS_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MODULE_ABI_VERSION 2u
/* Oldest module ABI whose saved module settings (module ID and version, state and parameter IDs)
 * mean the same under VS_MODULE_ABI_VERSION. Hosts restore settings saved with any ABI from this
 * one through VS_MODULE_ABI_VERSION and record VS_MODULE_ABI_VERSION when saving again. */
#define VS_MODULE_SETTINGS_ABI_MINIMUM 1u
#define VS_MODULE_ENTRY_NAME "vespera_module_entry"
#define VS_MODULE_STATE_ABI_VERSION 1u
#define VS_MODULE_STATE_ENTRY_NAME "vespera_module_state_entry"
#define VS_MODULE_PARAMETER_ABI_VERSION 1u
#define VS_MODULE_PARAMETER_ENTRY_NAME "vespera_module_parameter_entry"

/*
 * ABI growth rules. A struct that begins with struct_size grows only by appending fields: readers
 * accept a struct_size at least as large as the fields they use and ignore the remainder, and they
 * never read an appended field that struct_size does not cover. vs_parameter_descriptor and
 * vs_parameter_event carry no size, so their array element layout is frozen.
 * Richer parameter metadata and new event kinds are added through separately versioned extension
 * entry points or appended vs_process_block fields, and any array they introduce must state its
 * element size so its elements can grow in turn.
 *
 * Strings are NUL-terminated UTF-8 shorter than 256 bytes unless a field states otherwise,
 * and are compared byte for byte.
 */

typedef int32_t vs_result;
enum {
  VS_OK = 0,
  VS_ERROR_ARGUMENT = 1,
  VS_ERROR_STATE = 2,
  VS_ERROR_UNSUPPORTED = 3,
  VS_ERROR_PROCESS = 4
};

enum { VS_LAYOUT_MONO = 1u, VS_LAYOUT_STEREO = 2u };
enum { VS_PROCESS_DISCONTINUITY = 1u, VS_PROCESS_OFFLINE = 2u, VS_PROCESS_MIDI_PANIC = 4u };

/* MIDI 1 channel messages only. Events are ordered by frame_offset; ties preserve input order.
 * Parameter changes at the same offset precede MIDI. SysEx and system messages are not accepted.
 */
typedef struct vs_midi_event {
  uint32_t frame_offset;
  uint8_t status;
  uint8_t data1;
  uint8_t data2;
  uint8_t reserved;
} vs_midi_event;

static inline int vs_midi_event_valid(const vs_midi_event *event) {
  return event && event->status >= 0x80 && event->status < 0xf0 && event->data1 < 128 &&
         event->data2 < 128 && event->reserved == 0 &&
         ((event->status & 0xe0) != 0xc0 || event->data2 == 0);
}

typedef struct vs_parameter_descriptor {
  uint32_t id;
  const char *name;
  const char *unit;
  float minimum;
  float maximum;
  float default_value;
} vs_parameter_descriptor;

/* Disposes an instance and payload that a module released to vs_host_instance_cache. */
typedef void (*vs_cached_instance_dispose_fn)(void *instance, void *payload);

/*
 * Host-owned store of idle, module-created instances, such as hosted plugin instances that are
 * expensive to create, shared by every module the host loads. The host never inspects instances
 * or payloads. Modules choose keys so that every instance stored under one key is usable by every
 * module releasing under it, whichever library released it. Both functions run on the control
 * thread.
 *
 * acquire returns an idle instance stored under key, preferring the most recent one released with
 * the same signature, or NULL; *payload receives the payload stored with it. The caller then owns
 * both. acquire never disposes anything.
 *
 * release transfers ownership of instance and payload to the host, together with the function that
 * disposes them, which must not be NULL; without one the host neither stores nor disposes them.
 * The host either returns them from a later acquire with the same key or calls
 * dispose(instance, payload) exactly once, outside any host lock, on the control thread or while
 * the host shuts down; it keeps the library containing dispose loaded until then. dispose may run
 * before release returns, for example when the host evicts the oldest idle instance of the key, so
 * callers must not hold a lock that dispose functions take.
 */
typedef struct vs_host_instance_cache {
  uint32_t struct_size;
  void *context;
  void *(*acquire)(void *context, const char *key, const char *signature, void **payload);
  void (*release)(void *context, const char *key, const char *signature, void *instance,
                  void *payload, vs_cached_instance_dispose_fn dispose);
} vs_host_instance_cache;

typedef struct vs_prepare_info {
  uint32_t struct_size;
  double sample_rate;
  uint32_t max_frames;
  uint32_t channels;
} vs_prepare_info;

typedef struct vs_module_timing {
  uint32_t struct_size;
  uint32_t latency_frames;
  uint64_t tail_frames;
} vs_module_timing;

typedef struct vs_parameter_event {
  uint32_t parameter_id;
  uint32_t frame_offset;
  float value;
} vs_parameter_event;

typedef struct vs_process_block {
  uint32_t struct_size;
  const float *const *inputs;
  float *const *outputs;
  uint32_t channels;
  uint32_t frames;
  uint64_t frame_position;
  uint32_t flags;
  const vs_parameter_event *events;
  uint32_t event_count;
  const struct vs_tempo_context *tempo;
  const void *midi_events;
  uint32_t midi_event_count;
  uint32_t midi_event_size;
} vs_process_block;

#define VS_PROCESS_BLOCK_V1_SIZE offsetof(vs_process_block, tempo)

enum { VS_TEMPO_VALID = 1u, VS_TEMPO_DISCONTINUITY = 2u };

/* Quarter-note position at block start; tempo is constant within a block. Borrowed for process().
 */
typedef struct vs_tempo_context {
  uint32_t struct_size;
  uint32_t flags;
  double bpm;
  double beat_position;
} vs_tempo_context;

enum {
  /* bypass_parameter names a declared parameter ranging from 0 to 1. Values of at least 0.5
   * process; lower values bypass the module, which then passes its input through. A host may
   * bypass the module by setting the parameter, or by removing the module from its graph. */
  VS_MODULE_BYPASS_PARAMETER = 1u,
  /* The module reads vs_process_block.tempo. Hosts pass no tempo to modules without it. */
  VS_MODULE_HOST_TEMPO = 2u,
  /* The module accepts MIDI channel input through vs_process_block.midi_events and supports
   * VS_PROCESS_MIDI_PANIC, which releases held voices before the block's events and may preserve
   * audio tails. Hosts send neither MIDI nor the panic flag to modules without it, and use
   * midi_event_size to permit future appended event fields. */
  VS_MODULE_MIDI_INPUT = 4u
};

// Lifecycle calls on one instance may overlap processing of another instance from the same
// library. Shared module state must not introduce data races or blocking in process().
typedef struct vs_module_api {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *id;
  const char *name;
  const char *version;
  uint32_t supported_layouts;
  const vs_parameter_descriptor *parameters;
  uint32_t parameter_count;
  vs_result (*create)(void **instance);
  void (*destroy)(void *instance);
  vs_result (*prepare)(void *instance, const vs_prepare_info *info, vs_module_timing *timing);
  void (*release)(void *instance);
  void (*reset)(void *instance);
  vs_result (*process)(void *instance, const vs_process_block *block);
  /* VS_MODULE_* capability flags. Hosts reject unknown flags. */
  uint32_t capabilities;
  /* Meaningful only with VS_MODULE_BYPASS_PARAMETER; zero otherwise. */
  uint32_t bypass_parameter;
} vs_module_api;

typedef const vs_module_api *(*vs_module_entry_fn)(uint32_t host_abi_version);

/*
 * Saved state outlives the module version that wrote it: hosts load it into later versions of
 * the same module whenever load accepts its schema_version. Change schema_version whenever the
 * meaning of saved bytes changes, and make load reject or migrate schemas it cannot reproduce;
 * adding a parameter whose default preserves the old sound may keep the schema. save must be
 * deterministic: identical settings produce identical bytes, with no timestamps, counters or
 * uninitialized padding, because hosts compare saved state to detect changes.
 */
typedef struct vs_module_state_api {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t schema_version;
  vs_result (*size)(void *instance, uint64_t *bytes);
  vs_result (*save)(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written);
  vs_result (*load)(void *instance, uint32_t schema_version, const uint8_t *buffer, uint64_t bytes);
} vs_module_state_api;

typedef const vs_module_state_api *(*vs_module_state_entry_fn)(uint32_t host_state_abi_version);

/*
 * Optional access to current parameter values, negotiated independently of the processing ABI.
 * Both functions run on the control thread only while processing is quiescent, with the instance
 * either prepared or not. The host passes only declared parameter IDs and, for set_value, finite
 * values within the descriptor bounds.
 */
typedef struct vs_module_parameter_api {
  uint32_t struct_size;
  uint32_t abi_version;
  /* Reports the native-unit value the instance currently applies. */
  vs_result (*get_value)(void *instance, uint32_t parameter_id, float *value);
  /* Makes the value current for subsequent processing and state capture. */
  vs_result (*set_value)(void *instance, uint32_t parameter_id, float value);
} vs_module_parameter_api;

typedef const vs_module_parameter_api *(*vs_module_parameter_entry_fn)(
    uint32_t host_parameter_abi_version);

#define VS_MODULE_METADATA_ABI_VERSION 1u
#define VS_MODULE_METADATA_ENTRY_NAME "vespera_module_metadata_entry"

/*
 * Optional parameter metadata: what kind of control each parameter is and how hosts show its
 * value, so that every host view (a layout's readouts, accessibility, MIDI mapping) shows and
 * reads it the same way. Everything is static data that stays valid while the library is loaded.
 * A module describes any subset of its declared parameters, each at most once; hosts treat an
 * undescribed parameter as continuous with no display mapping.
 */
enum {
  /* Any value in the descriptor range. */
  VS_PARAMETER_CONTINUOUS = 0u,
  /* Off at the descriptor minimum, on at the maximum; values in the upper half of the range count
   * as on. The bypass parameter, when described, is a toggle. steps is 2. */
  VS_PARAMETER_TOGGLE = 1u,
  /* steps evenly spaced values from the descriptor minimum to its maximum, both included. The
   * default is one of them. */
  VS_PARAMETER_STEPPED = 2u,
  /* As VS_PARAMETER_STEPPED, with labels[i] naming the i-th value from the minimum. */
  VS_PARAMETER_ENUMERATED = 3u
};

enum {
  /* The display fields of vs_parameter_metadata describe the number shown for a value. Only
   * continuous and stepped parameters may set it; without it those fields are ignored. */
  VS_PARAMETER_DISPLAY_MAPPED = 1u,
  /* Shown numbers above zero carry a plus sign. Needs VS_PARAMETER_DISPLAY_MAPPED. */
  VS_PARAMETER_DISPLAY_SIGNED = 2u
};

enum { VS_PARAMETER_OFF_NONE = 0u, VS_PARAMETER_OFF_MINIMUM = 1u, VS_PARAMETER_OFF_MAXIMUM = 2u };

/*
 * With VS_PARAMETER_DISPLAY_MAPPED, a value v of a parameter ranging over [minimum, maximum] is
 * at position p = (v - minimum) / (maximum - minimum) and is shown as the number
 *   shown_minimum + p^(1 / skew) * (shown_maximum - shown_minimum)
 * with decimals digits after the point (0 rounds half away from zero), followed by unit. A
 * non-empty kilo_unit instead shows magnitudes of 1000 and above divided by 1000, with three
 * significant digits, followed by kilo_unit. A shown number that rounds to zero carries no sign.
 * off names the end of the range shown as OFF; values whose shown number is less than
 * off_tolerance away from that end's shown number are shown as OFF too.
 */
typedef struct vs_parameter_metadata {
  uint32_t parameter_id;
  uint32_t kind;
  /* 2 for toggles, the number of values for stepped and enumerated parameters, 0 otherwise. */
  uint32_t steps;
  /* VS_PARAMETER_DISPLAY_* flags. */
  uint32_t flags;
  /* steps non-empty names for an enumerated parameter; NULL otherwise. */
  const char *const *labels;
  /* Shown at the descriptor minimum and maximum respectively: different, finite, and at most 1e9
   * in magnitude. */
  double shown_minimum;
  double shown_maximum;
  /* Above 0 and at most 1000; 1 is linear, below 1 spends more of the range near the minimum. */
  double skew;
  /* Appended to shown numbers; NULL or "" for none. At most 64 bytes, as is kilo_unit. */
  const char *unit;
  /* NULL or "" when numbers are never shown in thousands. */
  const char *kilo_unit;
  /* 0 to 6. */
  uint32_t decimals;
  /* VS_PARAMETER_OFF_*. */
  uint32_t off;
  /* Finite, at least 0; non-zero only with an off end. */
  double off_tolerance;
} vs_parameter_metadata;

typedef struct vs_module_metadata_api {
  uint32_t struct_size;
  uint32_t abi_version;
  /* parameter_count elements, parameter_size bytes apart. */
  const vs_parameter_metadata *parameters;
  uint32_t parameter_count;
  uint32_t parameter_size;
} vs_module_metadata_api;

typedef const vs_module_metadata_api *(*vs_module_metadata_entry_fn)(
    uint32_t host_metadata_abi_version);

#define VS_MODULE_SHARED_ABI_VERSION 2u
#define VS_MODULE_SHARED_ENTRY_NAME "vespera_module_shared_entry"

/*
 * Shared processing engines. A module declaring this extension implements one section of a
 * fixed-order multi-section processor, such as one stage of a plugin's internal chain. A host may
 * run a contiguous run of such modules on one engine instance hosted by the run's first module,
 * the leader, instead of preparing each module separately. Hosted modules stay created and keep
 * their own parameter values and state; they are neither prepared nor processed while hosted.
 *
 * Modules may share an engine only when all of these hold:
 *  - their engine strings are byte-equal; the string identifies the engine implementation, its
 *    version and the meaning of stages and targets, and is not a display or catalog family;
 *  - their stages strictly increase in processing order, so the engine's internal order equals
 *    the host's order;
 *  - no module other than the leader is VS_SHARED_LEADING_ONLY or declares VS_MODULE_MIDI_INPUT;
 *  - no target is owned by more than one module, or owned by one and required by another;
 *  - every module requiring a target requires the same value;
 *  - targets with the same id carry identical names and ranges.
 * The host forms the engine state from, in order: the neutral value of each remaining target
 * declared by the earliest module that declares it, every required value, and every module's
 * owned assignments.
 *
 * A shared module's bypass parameter (VS_MODULE_BYPASS_PARAMETER) bypasses exactly its own section,
 * so hosts may toggle it while the module stays in its run instead of removing the module.
 */
enum { VS_SHARED_TARGET_OWNED = 1u, VS_SHARED_TARGET_REQUIRED = 2u, VS_SHARED_TARGET_NEUTRAL = 3u };
enum {
  /* The section may only begin an engine run. */
  VS_SHARED_LEADING_ONLY = 1u,
  /* Not declared by modules. The host adds it to engine_prepare's flags when any member of the run
   * declares VS_MODULE_HOST_TEMPO; the engine then reads the leader's block tempo for all of
   * them. */
  VS_SHARED_HOST_TEMPO = 2u,
  /* The section keeps audio state that resetting the engine does not clear, so an engine running
   * it must not be reused for later preparations. */
  VS_SHARED_RETAINS_AUDIO = 4u
};

typedef struct vs_shared_assignment {
  uint32_t target;
  float value;
} vs_shared_assignment;

typedef struct vs_shared_target {
  uint32_t id;
  const char *name;
  float minimum;
  float maximum;
  uint32_t role;
  /* Static value of a required or neutral target; unused for owned targets. */
  float value;
} vs_shared_target;

typedef struct vs_module_shared_api {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *engine;
  uint32_t stage;
  /* VS_SHARED_LEADING_ONLY and VS_SHARED_RETAINS_AUDIO. */
  uint32_t flags;
  const vs_shared_target *targets;
  uint32_t target_count;
  uint32_t target_size;
  /* Writes every owned target with its current value. Control thread, quiescent. */
  vs_result (*assignments)(void *instance, vs_shared_assignment *out, uint32_t capacity,
                           uint32_t *written);
  /* Makes a declared parameter's value current, as set_value does, and writes the owned
   * targets it changes. Real-time safe; runs on the processing thread while hosted. */
  vs_result (*translate)(void *instance, uint32_t parameter_id, float value,
                         vs_shared_assignment *out, uint32_t capacity, uint32_t *written);
  /* Leader only. Prepares one engine for a run of one or more members. cache may be NULL; the
   * leader may use it until its release returns.
   * signature names the run's modules in processing order, flags is the union of their flags plus
   * VS_SHARED_HOST_TEMPO as described above, targets is the union of their targets with elements
   * target_size bytes apart, and state is the formed engine state. */
  vs_result (*engine_prepare)(void *leader, const vs_prepare_info *info,
                              const vs_host_instance_cache *cache, const char *signature,
                              uint32_t flags, const vs_shared_target *targets,
                              uint32_t target_count, uint32_t target_size,
                              const vs_shared_assignment *state, uint32_t state_count,
                              vs_module_timing *timing);
  /* Processes the engine. block carries the leader's own parameter and MIDI events; targets
   * carries the other members' translated changes as parameter events whose parameter_id is the
   * target, ordered by frame offset. The leader's release and reset apply to the engine. */
  vs_result (*engine_process)(void *leader, const vs_process_block *block,
                              const vs_parameter_event *targets, uint32_t target_count);
  /* Applies translated member changes to a prepared engine. Control thread, quiescent. */
  vs_result (*engine_apply)(void *leader, const vs_shared_assignment *assignments, uint32_t count);
} vs_module_shared_api;

typedef const vs_module_shared_api *(*vs_module_shared_entry_fn)(uint32_t host_shared_abi_version);

#define VS_MODULE_SHARED_BRANCH_ABI_VERSION 1u
#define VS_MODULE_SHARED_BRANCH_ENTRY_NAME "vespera_module_shared_branch_entry"

enum { VS_SHARED_BRANCH_CROSSFADE = 0u };
enum { VS_SHARED_BRANCH_HAS_SELECTOR = 1u };

/*
 * Optional addendum to shared ABI v2. A compatible whole-graph run may realize a declared
 * graph-input branch through the engine, with no host merge. The branch is never the leader.
 * Its internal stage must be unique and below first_after_stage; that fence separates all
 * preceding serial members from all following serial members.
 * The selector, if present, must be a distinct discrete parameter with a canonical selector_value.
 * Hosts retain the normal shared run checks, except that the branch does not advance the serial
 * stage cursor. The leader receives no instance cache for this run and reports its full timing.
 */
typedef struct vs_module_shared_branch_api {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t mix_parameter;
  uint32_t mix_law;
  uint32_t first_after_stage;
  uint32_t flags;
  /* Read only with VS_SHARED_BRANCH_HAS_SELECTOR; parameter ID zero is valid. */
  uint32_t selector_parameter;
  float selector_value;
  /* Same contract as shared v2 assignments, with native branch settings. Write each owned
   * target exactly once. */
  vs_result (*native_assignments)(void *instance, vs_shared_assignment *out, uint32_t capacity,
                                  uint32_t *written);
  /* Same contract as shared v2 translate. Emit only distinct owned targets with in-range values.
   * Reject a selector route change before mutating state. Neither callback may leave a
   * persistent native mode on the member instance. */
  vs_result (*native_translate)(void *instance, uint32_t parameter_id, float value,
                                vs_shared_assignment *out, uint32_t capacity, uint32_t *written);
} vs_module_shared_branch_api;

typedef const vs_module_shared_branch_api *(*vs_module_shared_branch_entry_fn)(
    uint32_t host_shared_branch_abi_version);

#define VS_MODULE_FILES_ABI_VERSION 1u
#define VS_MODULE_FILES_ENTRY_NAME "vespera_module_files_entry"

enum { VS_FILE_SLOT_OPTIONAL = 1u };
enum { VS_FILE_NONE = 0, VS_FILE_PRESENT = 1, VS_FILE_MISSING = 2 };

typedef struct vs_file_slot {
  uint32_t id;
  const char *name;
  const char *types;
  const char *bundled; /* Relative resource directory, at most 4096 UTF-8 bytes, or NULL. */
  uint64_t max_bytes;
  uint32_t flags;
} vs_file_slot;

typedef struct vs_file_ref {
  uint32_t struct_size;
  uint32_t state;
  /* Absolute readable UTF-8 path of at most 4096 bytes, only for PRESENT. Host access lasts
   * until the next set_file or destroy; the module copies strings it keeps. */
  const char *path;
  const char *name;
  uint64_t bytes;
  uint8_t sha256[32];
} vs_file_ref;

typedef struct vs_module_files_api {
  uint32_t struct_size;
  uint32_t abi_version;
  const vs_file_slot *slots;
  uint32_t slot_count;
  uint32_t slot_size;
  /* Control thread, unprepared instance. No I/O. Missing/empty references are not errors.
   * References are host-owned settings, never part of opaque module state. */
  vs_result (*set_file)(void *instance, uint32_t slot, const vs_file_ref *file);
} vs_module_files_api;

typedef const vs_module_files_api *(*vs_module_files_entry_fn)(uint32_t host_files_abi_version);

#define VS_MODULE_SERVICES_ABI_VERSION 1u
#define VS_MODULE_SERVICES_ENTRY_NAME "vespera_module_services_entry"

/*
 * Host services: background work with a real-time safe hand-off of its results into process(),
 * notifications, a status query and logging. A host that offers them attaches a vs_host_services
 * table to each instance right after create. A module must also work when no host attaches one.
 */
enum {
  /* Work runs synchronously inside request(), and its result can be adopted as soon as request
   * returns; every service may then run module code on the calling thread. Set by deterministic
   * offline hosts. Without it, request(), adopt() and retire() are real-time safe. */
  VS_HOST_WORK_INLINE = 1u
};

/* vs_host_services.notify bits. */
enum {
  /* vs_module_services_api.status would now report something different. */
  VS_NOTIFY_STATUS = 1u,
  /* The latency or tail a new prepare would report differs from what the current preparation
   * reported. The host prepares the instance again when it chooses; that prepare must report the
   * new timing without waiting for work. */
  VS_NOTIFY_TIMING = 2u
};

enum { VS_LOG_DEBUG = 0, VS_LOG_INFO = 1, VS_LOG_WARNING = 2, VS_LOG_ERROR = 3 };

/* Host-owned handle of one stream of background work belonging to one prepared instance. */
typedef struct vs_work vs_work;

/* Passed to a running work function; valid only during that call. */
typedef struct vs_work_run {
  uint32_t struct_size;
  /* The token of the request this run serves. */
  uint64_t token;
  void *host;
  /* Nonzero once the work was closed or a newer request arrived. The run should then return
   * promptly; checking at least every 10 ms keeps release responsive. */
  int (*cancelled)(const struct vs_work_run *run);
  /* Optional progress in [0, 1] for status displays. */
  void (*progress)(const struct vs_work_run *run, float fraction);
} vs_work_run;

typedef struct vs_work_desc {
  uint32_t struct_size;
  void *context;
  /* Runs on a host worker thread, or with VS_HOST_WORK_INLINE on the requesting thread, never
   * concurrently with itself for the same work. It may allocate, block and perform I/O. On VS_OK
   * *result (possibly NULL) is delivered for adoption and ownership passes to the host; any other
   * return value delivers nothing. */
  vs_result (*run)(void *context, const vs_work_run *run, void **result);
  /* Frees a result. Only the host calls it: exactly once for every non-NULL result run returned
   * with VS_OK, on a thread other than the processing thread (with VS_HOST_WORK_INLINE possibly
   * the calling thread), outside any host lock, and never after close_work returns. */
  void (*dispose)(void *context, void *result);
} vs_work_desc;

/*
 * Ownership of results: run creates a result, the host owns it until adopt hands it to the
 * module, the module owns it while it uses it, and adopt, retire or close_work hand it back. A
 * module never frees a result itself.
 */
typedef struct vs_host_services {
  uint32_t struct_size;
  uint32_t abi_version; /* VS_MODULE_SERVICES_ABI_VERSION */
  uint32_t flags;       /* VS_HOST_* */
  void *context;
  /* Absolute UTF-8 path of the package's resources directory, or NULL when the host has none,
   * for example for a module loaded by binary path. Valid as long as the table. */
  const char *resource_directory;

  /* Control thread, only inside prepare or engine_prepare. Returns NULL when the host declines,
   * and at most 8 works are open per instance. desc is copied. */
  vs_work *(*open_work)(void *context, const vs_work_desc *desc);
  /* Asks for run(token). Any thread inside a call on this instance, including process and a run
   * of another work. A newer request supersedes an older one that has not run yet and flags a
   * running one as cancelled. With VS_HOST_WORK_INLINE every request runs before this returns;
   * otherwise it never allocates or blocks. VS_ERROR_STATE once the work is closed. */
  vs_result (*request)(void *context, vs_work *work, uint64_t token);
  /* Processing thread inside process, or control thread while quiescent. If a result newer than
   * the last adopted one is ready, stores it in *current, hands the previous *current (when not
   * NULL) back to the host, writes the result's token to *token and returns 1. Otherwise returns
   * 0 and changes nothing, including while an earlier hand-back is still being disposed; the
   * result then stays ready for a later call. Lock-free and allocation-free. */
  int (*adopt)(void *context, vs_work *work, void **current, uint64_t *token);
  /* Same threads as adopt. Hands a result the module no longer uses back to the host without
   * adopting another, such as one a crossfade has finished fading out. Returns 1 when the host
   * took it (NULL is accepted) and 0 while an earlier hand-back is still being disposed; the
   * module then keeps the result and tries again later. Lock-free and allocation-free. */
  int (*retire)(void *context, vs_work *work, void *result);
  /* Control thread, inside release or a failing prepare. Cancels pending and running work, waits
   * for a running run to return, then disposes every result the host still holds and the
   * held_count results in held that the module holds (its current result and any it keeps for a
   * crossfade; NULL entries are ignored). The handle is invalid afterwards. The host closes works
   * still open when release returns. */
  void (*close_work)(void *context, vs_work *work, void *const *held, uint32_t held_count);

  /* Any thread, real-time safe. ORs VS_NOTIFY_* bits, which the host reads and clears on its
   * control thread. */
  void (*notify)(void *context, uint32_t what);
  /* Any thread except inside process. message is UTF-8 and copied; the host may shorten or
   * rate-limit it. */
  void (*log)(void *context, uint32_t level, const char *message);
} vs_host_services;

enum {
  VS_STATUS_READY = 0,   /* nothing to report */
  VS_STATUS_BUSY = 1,    /* loading or rebuilding; progress may be valid */
  VS_STATUS_WARNING = 2, /* processing as configured, with a remark */
  VS_STATUS_ERROR = 3    /* not processing as configured, such as a missing or unreadable file */
};

typedef struct vs_module_status {
  uint32_t struct_size; /* set by the host; the module writes only fields it covers */
  uint32_t kind;        /* VS_STATUS_* */
  float progress;       /* [0, 1], or negative when unknown */
  uint32_t file_slot;   /* reserved for file references; 0 */
  char message[256];    /* NUL-terminated UTF-8, empty for VS_STATUS_READY */
} vs_module_status;

typedef struct vs_module_services_api {
  uint32_t struct_size;
  uint32_t abi_version;
  /* Control thread, exactly once, after create and before any other call on the instance except
   * destroy. services stays valid until destroy returns. A host without services never calls it.
   * An error makes the host destroy the instance and fail to load it. */
  vs_result (*attach)(void *instance, const vs_host_services *services);
  /* Control thread, at any time after attach, including while another thread is inside process.
   * Copies a snapshot of the instance's status into status, whose struct_size the host set, and
   * must not wait for the processing thread. */
  vs_result (*status)(void *instance, vs_module_status *status);
} vs_module_services_api;

typedef const vs_module_services_api *(*vs_module_services_entry_fn)(
    uint32_t host_services_abi_version);

#ifdef __cplusplus
}
#endif

#endif
