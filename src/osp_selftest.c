/* osp_selftest.c -- prove the host runs 68000 code, with no engine in sight.
 *
 * Every real test of this project needs an engine, and no engine is ever in
 * the repository or on a build server.  This is the check that carries its
 * own inputs: a handful of hand-assembled instructions that exercise the
 * things every engine here leans on -- a call that returns to the sentinel,
 * an OS trap that allocates, a Toolbox trap that serves a resource, an
 * exception named by its vector, a spent instruction budget, and the CPU
 * choice that MacinTalk Pro and 3 depend on.
 *
 * It is what a Linux build runs in CI, and what a Windows build runs to say
 * the DLL it just linked is not merely present.  Exit status is the number of
 * failed checks, so either can gate on it.
 *
 *     build/linux/osp_selftest        build/osp_selftest.exe
 */
#include <stdio.h>
#include <string.h>

#include "osp_host.h"

#define RAM        0x01000000u          /* 16 MB, as the Python driver asks */
#define HEAP       0x00080000u
#define HEAP_SIZE  0x00080000u
#define STACK      0x00200000u
#define CODE       0x00040000u          /* where `.sp` is loaded, as it happens */

static int g_failed;

static void check(int ok, const char *what, const char *detail)
{
    if (ok) {
        printf("ok    %s\n", what);
    } else {
        g_failed++;
        printf("FAIL  %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    }
}

/* A fresh machine: RAM, heap, supervisor mode, a stack. */
static void fresh(int cpu)
{
    if (osp_init(RAM) != 0) { puts("osp_init failed"); g_failed += 100; return; }
    if (cpu != OSP_CPU_68000) osp_set_cpu(cpu);
    osp_heap_init(HEAP, HEAP_SIZE);
    /* Every engine module does this before its first call; a caller that
     * forgets gets noErr and no handle from _NewHandle, which is exactly the
     * plausible success this file exists to catch.  It caught its author. */
    osp_enable_mem_traps(1);
    osp_set_reg(OSP_REG_SR, 0x2700);
    osp_set_reg(OSP_REG_A7, STACK);
}

/* Assemble `n` big-endian words at CODE and run them as a subroutine. */
static int run(const unsigned short *words, int n, long long budget)
{
    int i;
    for (i = 0; i < n; i++) osp_w16(CODE + 2u * (unsigned)i, words[i]);
    return osp_call(CODE, osp_magic_sentinel(), budget);
}

static void t_call_and_return(void)
{
    /* moveq #42,d0 ; rts */
    static const unsigned short code[] = { 0x702A, 0x4E75 };
    char d[96];
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, 2, 1000);
    sprintf(d, "stop %d, D0 %u, %lld instructions", r,
            osp_get_reg(OSP_REG_D0), osp_instr_count());
    check(r == OSP_STOP_SENTINEL && osp_get_reg(OSP_REG_D0) == 42
          && osp_instr_count() > 0 && osp_instr_count() < 10,
          "a subroutine runs and returns to the sentinel", d);
    /* The pushed return address must have been popped: SP is back where it
     * started.  A frame-size mistake shows up here first. */
    sprintf(d, "SP 0x%X", osp_get_reg(OSP_REG_SP));
    check(osp_get_reg(OSP_REG_SP) == STACK, "the stack is balanced afterwards", d);
}

static void t_new_handle(void)
{
    /* move.l #256,d0 ; _NewHandle ; rts */
    static const unsigned short code[] = { 0x203C, 0x0000, 0x0100, 0xA122, 0x4E75 };
    char d[128];
    unsigned h, p;
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, 5, 10000);
    h = osp_get_reg(OSP_REG_A0);
    p = h ? osp_r32(h) : 0;
    sprintf(d, "stop %d, D0 %d, A0 0x%X -> 0x%X, heap used %u", r,
            (int)osp_get_reg(OSP_REG_D0), h, p, osp_heap_used());
    check(r == OSP_STOP_SENTINEL && osp_get_reg(OSP_REG_D0) == 0
          && h >= HEAP && h < HEAP + HEAP_SIZE
          && p >= HEAP && p < HEAP + HEAP_SIZE
          && osp_heap_used() >= 256,
          "_NewHandle allocates in the heap and returns noErr", d);
}

static void t_get_resource(void)
{
    /* clr.l -(a7) ; move.l #'TEST',-(a7) ; move.w #ID,-(a7) ; _GetResource ;
     * movea.l (a7)+,a0 ; rts                                  -- ID patched */
    static unsigned short code[] = { 0x42A7, 0x2F3C, 0x5445, 0x5354,
                                     0x3F3C, 0x0007, 0xA9A0, 0x205F, 0x4E75 };
    static const unsigned char payload[] = "outSPOKEN selftest resource";
    unsigned char back[sizeof payload];
    char d[128];
    unsigned h, p, reg;
    int r;

    fresh(OSP_CPU_68000);
    reg = osp_add_resource(0x54455354u, 7, payload, (int)sizeof payload, -1);
    check(reg != 0, "a resource can be registered", NULL);

    code[5] = 7;
    r = run(code, 9, 10000);
    h = osp_get_reg(OSP_REG_A0);
    p = h ? osp_r32(h) : 0;
    memset(back, 0, sizeof back);
    if (p) osp_read_block(p, back, (int)sizeof back);
    sprintf(d, "stop %d, A0 0x%X -> 0x%X", r, h, p);
    check(r == OSP_STOP_SENTINEL && h == reg
          && memcmp(back, payload, sizeof payload) == 0,
          "_GetResource hands back the registered bytes", d);

    /* The same call for an id nobody registered must answer NIL, not a
     * plausible handle: a guess that looks like a success is the expensive
     * kind of failure, and outSPOKEN's dead RULZ probe would take it. */
    osp_set_reg(OSP_REG_A7, STACK);
    code[5] = 8;
    r = run(code, 9, 10000);
    sprintf(d, "stop %d, A0 0x%X", r, osp_get_reg(OSP_REG_A0));
    check(r == OSP_STOP_SENTINEL && osp_get_reg(OSP_REG_A0) == 0,
          "_GetResource answers NIL for a missing resource", d);
}

static void t_exception_is_named(void)
{
    /* illegal */
    static const unsigned short code[] = { 0x4AFC };
    char d[96];
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, 1, 1000);
    sprintf(d, "stop %d, vector %d, pc 0x%X", r, osp_stop_vector(), osp_stop_pc());
    check(r == OSP_STOP_EXCEPTION && osp_stop_vector() == 4,
          "an illegal instruction stops on vector 4", d);
}

static void t_budget_is_a_stop(void)
{
    /* bra.s * -- spins forever */
    static const unsigned short code[] = { 0x60FE };
    char d[96];
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, 1, 1000);
    sprintf(d, "stop %d after %lld instructions", r, osp_instr_count());
    check(r == OSP_STOP_BUDGET, "a spent instruction budget is reported, not hung", d);
}

static void t_cpu_choice(void)
{
    /* rtd #0 -- 68010 and later only; MacinTalk 3 opens with one. */
    static const unsigned short code[] = { 0x4E74, 0x0000 };
    char d[96];
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, 2, 1000);
    sprintf(d, "stop %d, vector %d", r, osp_stop_vector());
    check(r == OSP_STOP_EXCEPTION && osp_stop_vector() == 4,
          "a 68000 refuses rtd", d);

    fresh(OSP_CPU_68040);
    r = run(code, 2, 1000);
    sprintf(d, "stop %d, SP 0x%X", r, osp_get_reg(OSP_REG_SP));
    check(r == OSP_STOP_SENTINEL && osp_get_reg(OSP_REG_SP) == STACK,
          "a 68040 runs rtd and pops the eight-byte frame correctly", d);
}

static void t_dispose_gives_memory_back(void)
{
    /* move.l #4096,d0 ; _NewPtr ; movea.l a0,a2 ; move.l #64,d0 ; _NewHandle ;
     * movea.l a0,a3 ; movea.l a3,a0 ; _DisposeHandle ; movea.l a2,a0 ;
     * _DisposePtr ; move.l #4096,d0 ; _NewPtr ; rts
     *
     * The engines allocate per utterance and dispose afterwards; with dispose
     * a no-op the heap filled in a hundred utterances and the engine spun.
     * The second _NewPtr must land where the first did, and the heap must be
     * back to one block. */
    static const unsigned short code[] = {
        0x203C, 0x0000, 0x1000, 0xA11E, 0x2448,
        0x203C, 0x0000, 0x0040, 0xA122, 0x2648,
        0x204B, 0xA023, 0x204A, 0xA01F,
        0x203C, 0x0000, 0x1000, 0xA11E, 0x4E75
    };
    char d[128];
    unsigned first, second;
    int r;
    fresh(OSP_CPU_68000);
    r = run(code, (int)(sizeof code / sizeof code[0]), 100000);
    first = osp_get_reg(OSP_REG_A2);
    second = osp_get_reg(OSP_REG_A0);
    sprintf(d, "stop %d, first 0x%X, second 0x%X, heap used %u", r, first, second, osp_heap_used());
    check(r == OSP_STOP_SENTINEL && first && second == first && osp_heap_used() == 4096,
          "_DisposePtr and _DisposeHandle give the heap back", d);
}

static void t_memory_bounds(void)
{
    unsigned char b[4] = { 1, 2, 3, 4 };
    fresh(OSP_CPU_68000);
    check(osp_write_block(RAM, b, 4) != 0,
          "a write past the end of RAM is refused", NULL);
    check(osp_write_block(0x1000, b, 4) == 0 && osp_r32(0x1000) == 0x01020304u,
          "a write inside RAM lands big-endian", NULL);
}

int main(void)
{
    t_call_and_return();
    t_new_handle();
    t_get_resource();
    t_exception_is_named();
    t_budget_is_a_stop();
    t_cpu_choice();
    t_dispose_gives_memory_back();
    t_memory_bounds();
    osp_shutdown();
    printf("%s: %d failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed;
}
