/* osp_host.h -- the host's C API, as a caller sees it.
 *
 * Until 2.0 the only caller was Python over ctypes, which declares its own
 * view of every function in osp.py and would never notice a drift.  A C
 * caller -- osp_selftest today, the serve loop and the JNI layer later --
 * needs the declarations in one place, and the host includes this header
 * itself so a definition that disagrees with it fails to compile instead of
 * failing at the first call.
 *
 * Grown deliberately: only what a C caller uses so far is declared here.  The
 * rest of the exported surface -- the probes' registers-and-memory view,
 * the component calls, the logs -- is still reached from osp.py and will be
 * declared as each engine's driving moves into C.
 *
 * Every address is a guest address, an unsigned 32-bit number into the flat
 * RAM osp_init allocates.  Nothing here is a host pointer, which is what
 * makes the same code correct on a 32-bit DLL and a 64-bit .so.
 */
#ifndef OSP_HOST_H
#define OSP_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(OSP_HOST_BUILD)
#  define OSP_API __declspec(dllexport)
#elif defined(_WIN32)
#  define OSP_API __declspec(dllimport)
#else
#  define OSP_API
#endif

/* ---- registers, in Musashi's m68k_register_t order ---------------------- */
enum {
    OSP_REG_D0 = 0, OSP_REG_D1, OSP_REG_D2, OSP_REG_D3,
    OSP_REG_D4, OSP_REG_D5, OSP_REG_D6, OSP_REG_D7,
    OSP_REG_A0 = 8, OSP_REG_A1, OSP_REG_A2, OSP_REG_A3,
    OSP_REG_A4, OSP_REG_A5, OSP_REG_A6, OSP_REG_A7,
    OSP_REG_PC = 16, OSP_REG_SR = 17, OSP_REG_SP = 18
};

/* ---- why a call stopped ------------------------------------------------- */
#define OSP_STOP_RUNNING    0
#define OSP_STOP_SENTINEL   1   /* returned to the address osp_call pushed  */
#define OSP_STOP_BUDGET     2   /* ran out of instructions: a stall         */
#define OSP_STOP_EXCEPTION  3   /* took a vector; osp_stop_vector names it  */
#define OSP_STOP_FAULT      4   /* the host refused a memory access         */
#define OSP_STOP_BREAK      5   /* a snapshot breakpoint                    */

/* ---- Gestalt processor values, for osp_set_cpu -------------------------- */
#define OSP_CPU_68000 1
#define OSP_CPU_68010 2
#define OSP_CPU_68020 3
#define OSP_CPU_68030 4
#define OSP_CPU_68040 5

/* ---- lifecycle ---------------------------------------------------------- */
/* Allocate `ram_size` bytes of guest RAM (raised to fit the host's own pages
 * if smaller), reset the CPU, and forget every resource, file and component.
 * Doubles as "start over".  -> 0, or -1 when memory could not be had. */
OSP_API int      osp_init(unsigned ram_size);
OSP_API void     osp_shutdown(void);
/* 1..5, the Gestalt('proc') value; call straight after osp_init. -> 0 or -1 */
OSP_API int      osp_set_cpu(int proc);
/* The one Memory Manager zone: a flat bump allocator over [base, base+size). */
OSP_API void     osp_heap_init(unsigned base, unsigned size);
OSP_API unsigned osp_heap_used(void);
OSP_API int      osp_heap_blocks(void);      /* blocks tracked; diagnosis */
/* Serve the Memory Manager traps (_NewHandle, _HLock, the zone queries...)
 * from that heap.  Off after osp_init, and off means an unanswered trap is a
 * stub that returns noErr and allocates nothing -- so every engine turns it
 * on before its first call, and so must any C caller. */
OSP_API void     osp_enable_mem_traps(int on);

/* ---- guest memory ------------------------------------------------------- */
/* -> 0, or nonzero when the range is not inside RAM. */
OSP_API int      osp_write_block(unsigned addr, const unsigned char *data, int len);
OSP_API int      osp_read_block(unsigned addr, unsigned char *out, int len);
OSP_API void     osp_w8 (unsigned a, unsigned v);
OSP_API void     osp_w16(unsigned a, unsigned v);
OSP_API void     osp_w32(unsigned a, unsigned v);
OSP_API unsigned osp_r8 (unsigned a);
OSP_API unsigned osp_r16(unsigned a);
OSP_API unsigned osp_r32(unsigned a);

/* ---- resources ---------------------------------------------------------- */
/* Copy `data` into the heap and register it under (type, id) for the Resource
 * Manager traps.  `file_index` is -1 for a resource that belongs to no file.
 * -> the Handle, or 0 when the heap or the table is full. */
OSP_API unsigned osp_add_resource(unsigned type, int id,
                                  const unsigned char *data, int len,
                                  int file_index);

/* ---- running the CPU ---------------------------------------------------- */
OSP_API void     osp_set_reg(int reg, unsigned v);
OSP_API unsigned osp_get_reg(int reg);
/* The address an `rts` may land on to stop a call cleanly. */
OSP_API unsigned osp_magic_sentinel(void);
/* Push `sentinel` as the return address and run from `entry` until the
 * routine returns to it, takes an exception, faults, or spends `max_instr`
 * instructions.  -> one of OSP_STOP_*. */
OSP_API int      osp_call(unsigned entry, unsigned sentinel, long long max_instr);
OSP_API int      osp_stop_reason(void);
OSP_API int      osp_stop_vector(void);
OSP_API unsigned osp_stop_pc(void);
OSP_API long long osp_instr_count(void);
/* Push `args` (in declared order) and call `entry` to the sentinel. */
OSP_API int      osp_call_with_args(unsigned entry, const unsigned *args, int nargs,
                                    long long max_instr);

/* ---- the PCM the Sound Manager model collects ---------------------------- */
OSP_API void     osp_pcm_reset(void);
OSP_API unsigned osp_pcm_len(void);
OSP_API int      osp_pcm_get(unsigned char *out, int max);

/* ---- the engines: text in, PCM out (see osp_engine.c) --------------------- */
/* One engine at a time.  `manifest` names the engine and every file it needs
 * by explicit path, one key=value a line; the format is documented at the
 * top of osp_engine.c.  -> 0; -100 when that engine is not ported yet; another
 * negative with osp_engine_error() set. */
OSP_API int         osp_engine_open(const char *manifest);
OSP_API const char *osp_engine_error(void);
OSP_API void        osp_engine_close(void);
/* Voice switch within the open engine: -> 1 took it, 0 refused (the previous
 * voice stays).  Engines with one voice per instance answer 1. */
OSP_API int         osp_engine_select(const char *creator, int id);
/* Settings on the engines' own scales -- words per minute, tenths of a
 * semitone from the voice's own pitch, hertz for `.sp` only, percent with 50
 * as recorded -- and the number style: 0 leaves numbers to the engine, 1
 * words, 2 digit by digit.  A setting an engine lacks is a no-op. */
OSP_API void        osp_engine_set_rate(int wpm);
OSP_API void        osp_engine_set_pitch(int tenths);
OSP_API void        osp_engine_set_voice_hz(double hz);
OSP_API void        osp_engine_set_inflection(int percent);
OSP_API void        osp_engine_set_numbers(int mode);
/* The engine's own text preparation (numbers, punctuation, the 1984 rules),
 * MacRoman in and out, size-needed contract as osp_numbers.  What it returns
 * is what osp_engine_speak is handed. */
OSP_API int         osp_engine_translate(const unsigned char *text, int len,
                                         unsigned char *out, int cap);
/* Render, blocking; -> bytes of 8-bit unsigned PCM at 22254 Hz after the
 * engine's own tidying, fetched with osp_engine_pcm; negative on failure. */
OSP_API int         osp_engine_speak(const unsigned char *prepared, int len);
OSP_API int         osp_engine_pcm(unsigned char *out, int cap);
/* Ask the utterance in flight to stop, where the engine allows it. */
OSP_API void        osp_engine_stop(void);
/* Streaming: begin an utterance (-> 0 accepted, 1 nothing to say, negative
 * on failure), then pull 8-bit PCM as the engine renders it -- each pull
 * runs one round of the engine on the calling thread and answers the bytes
 * written, 0 when the utterance is over.  The pieces concatenate to exactly
 * what osp_engine_speak returns.  osp_engine_cancel abandons the utterance
 * being pulled: the next pull stops the engine and answers 0. */
OSP_API int         osp_engine_speak_start(const unsigned char *prepared, int len);
OSP_API int         osp_engine_pull(unsigned char *out, int cap);
OSP_API void        osp_engine_cancel(void);

/* ---- the driver's settings, 0-100 (see osp_settings.c) -------------------- */
/* Rate, pitch, volume and inflection on the sliders' own 0-100 scales;
 * osp_apply_settings pushes them at the open engine before an utterance,
 * with the RateCommand and PitchCommand offsets in force (0 = none), and
 * osp_engine_pcm16 widens the last utterance to signed 16-bit with the
 * volume, plus any VolumeCommand offset, folded in.  Pure arithmetic apart
 * from the two that reach the engine; osp_settings_preview shows what the
 * mapping would apply, for the oracle. */
OSP_API void        osp_set_rate(int percent);
OSP_API void        osp_set_pitch(int percent);
OSP_API void        osp_set_volume(int percent);
OSP_API void        osp_set_volume_offset(int adj);
OSP_API void        osp_set_inflection(int percent);
OSP_API int         osp_get_rate(void);
OSP_API int         osp_get_pitch(void);
OSP_API int         osp_get_volume(void);
OSP_API int         osp_get_inflection(void);
OSP_API void        osp_apply_settings(int radj, int padj);
OSP_API int         osp_pcm_widen(const unsigned char *pcm8, int n, short *out, int cap);
OSP_API int         osp_engine_pcm16(short *out, int cap);
OSP_API void        osp_settings_preview(int rate, int radj, int pitch, int padj, double base_hz,
                                         int *engine_rate, int *tenths, double *hz);

/* ---- the catalogue: what the user has (see osp_voices.c) ------------------ */
/* Scan the search roots, one per line, and build the list of voices that can
 * actually speak: the 1984 pair when its three files are present, and every
 * voice folder whose engine and whose own parts are.  -> entries, or -1. */
OSP_API int         osp_catalogue_scan(const char *roots);
OSP_API int         osp_catalogue_count(void);
/* Entry `i`, tab-separated UTF-8: id, label, kind, creator, voice id, name,
 * language, gender, folder.  Size-needed contract; -2 for no such entry. */
OSP_API int         osp_catalogue_entry(int i, char *out, int cap);
/* The manifest that opens entry `i`'s engine with that voice selected. */
OSP_API int         osp_catalogue_manifest(int i, char *out, int cap);
/* Voice folders that were passed over, and why: "folder\treason". */
OSP_API int         osp_catalogue_skipped_count(void);
OSP_API int         osp_catalogue_skipped(int i, char *out, int cap);

/* ---- the text front end ------------------------------------------------- */
/* Numbers as words, ported from numwords.py (see osp_numbers.c).  MacRoman
 * in, MacRoman out.  `spell_out` is digit by digit; `spanish` is the cami
 * voices' language.  Writes at most `cap` bytes to `out` and returns how
 * many the whole result needs; more than `cap` means come back with a
 * bigger buffer.  -1 means no memory. */
OSP_API int      osp_numbers(const unsigned char *text, int len, int spell_out,
                             int spanish, unsigned char *out, int cap);

/* The 1984 engine's English front end, ported from nrl.py (see osp_nrl.c).
 * The rule tables are the user's own `RULZ` and `DICT` resources, handed in
 * whole; the interpreter is ours.  Load returns 0, or -1 for a table that
 * does not parse.  The text calls follow osp_numbers' contract -- MacRoman
 * in and out, return the size needed -- and answer -2 when the table they
 * need is not loaded, -1 for no memory. */
OSP_API int      osp_nrl_load(const unsigned char *data, int len);            /* RULZ */
OSP_API int      osp_nrl_load_dictionary(const unsigned char *data, int len); /* DICT */
OSP_API void     osp_nrl_unload(void);
OSP_API int      osp_nrl_rule_count(int dictionary);
/* English -> phonemes, stripped. */
OSP_API int      osp_nrl_translate(const unsigned char *text, int len, unsigned char *out, int cap);
/* Berkeley's respellings applied; English out, unmatched text kept. */
OSP_API int      osp_nrl_respell(const unsigned char *text, int len, unsigned char *out, int cap);
/* How a lone letter is announced. */
OSP_API int      osp_nrl_letter_name(const unsigned char *text, int len, unsigned char *out, int cap);

/* ---- the search roots (see osp_roots.c) ------------------------------------ */
/* rom.search_roots() minus migrate(): every folder the driver looks in for a
 * configuration path and an add-on folder, one per line, deduplicated, in
 * the order that decides which copy of a file wins.  Size-needed contract. */
OSP_API int      osp_roots_default(const char *config_path, const char *addon_root,
                                   char *out, int cap);

/* ---- serve mode and the text front door (see osp_serve.c) ----------------- */
/* UTF-8 to MacRoman as the engines want it, with `?` for what the encoding
 * cannot carry.  Size-needed contract. */
OSP_API int      osp_text_macroman(const char *utf8, unsigned char *out, int cap);
/* "id<TAB>label" per voice on `out_fd`, for token registration. -> exit status */
OSP_API int      osp_serve_list(const char *roots, int out_fd);
/* The SAPI bridge's protocol: requests on `in_fd`, responses on `out_fd`,
 * until the input closes. -> exit status */
OSP_API int      osp_serve_run(const char *roots, int in_fd, int out_fd);

#ifdef __cplusplus
}
#endif
#endif /* OSP_HOST_H */
