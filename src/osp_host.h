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

/* ---- the text front end ------------------------------------------------- */
/* Numbers as words, ported from numwords.py (see osp_numbers.c).  MacRoman
 * in, MacRoman out.  `spell_out` is digit by digit; `spanish` is the cami
 * voices' language.  Writes at most `cap` bytes to `out` and returns how
 * many the whole result needs; more than `cap` means come back with a
 * bigger buffer.  -1 means no memory. */
OSP_API int      osp_numbers(const unsigned char *text, int len, int spell_out,
                             int spanish, unsigned char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* OSP_HOST_H */
