/* --------------------------------------------- the Pascal stack convention -
 *
 * A Toolbox trap takes its arguments on the stack and leaves its result there,
 * in a slot the caller pushed *before* the arguments.  The callee removes the
 * arguments.  So after the call, SP points at the result.
 *
 * The complication is that we are standing inside an exception frame.  The
 * CPU pushed SR and PC onto the same stack, so the caller's SP is `exc_sp + 6`,
 * and we cannot simply adjust SP -- the `rte` will pop those six bytes from
 * wherever SP happens to be.  So we relocate the frame: put SR and PC six
 * bytes below where we want the caller's SP to end up, and let `rte` do the
 * arithmetic for us.
 */
/* Six on a 68000, eight on everything later. See g_exc_frame. */
#define EXC_FRAME g_exc_frame

static void tb_return(unsigned exc_sp, unsigned param_bytes,
                      unsigned result, int result_size)
{
    unsigned caller_sp = exc_sp + EXC_FRAME;
    unsigned result_slot = caller_sp + param_bytes;
    unsigned new_sp, sr, pc, fmt = 0;

    if (result_size == 4)      m68k_write_memory_32(result_slot, result);
    else if (result_size == 2) m68k_write_memory_16(result_slot, result);
    else                       result_slot = caller_sp + param_bytes;

    new_sp = result_slot - EXC_FRAME;
    sr = m68k_read_memory_16(exc_sp);          /* read them all before any  */
    pc = m68k_read_memory_32(exc_sp + 2);      /* write, the ranges overlap */
    if (EXC_FRAME > 6u) fmt = m68k_read_memory_16(exc_sp + 6);
    m68k_write_memory_16(new_sp, sr);
    m68k_write_memory_32(new_sp + 2, pc);
    /* The format word moves with the frame. `rte` reads it to decide how much
     * to pop, so leaving the old one behind desynchronises the stack. */
    if (EXC_FRAME > 6u) m68k_write_memory_16(new_sp + 6, fmt);
    m68k_set_reg(M68K_REG_SP, new_sp);
}
