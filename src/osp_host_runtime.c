/* ------------------------------------------------------------ the A-trap - */

static void service_atrap(void)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    unsigned stacked_pc = m68k_read_memory_32(sp + 2);
    unsigned trap_pc = stacked_pc;
    unsigned short word = (unsigned short)m68k_read_memory_16(trap_pc);
    unsigned d0, a0;
    int served = 0;

    if ((word & 0xF000u) != 0xA000u) {
        /* Not at the stacked address -- the CPU pointed past the word. */
        unsigned short w2 = (unsigned short)m68k_read_memory_16(stacked_pc - 2);
        if ((w2 & 0xF000u) == 0xA000u) {
            trap_pc = stacked_pc - 2;
            word = w2;
            if (g_stackpc_is_instruction < 0) g_stackpc_is_instruction = 0;
        }
    } else if (g_stackpc_is_instruction < 0) {
        g_stackpc_is_instruction = 1;
    }

    d0 = m68k_get_reg(NULL, M68K_REG_D0);
    a0 = m68k_get_reg(NULL, M68K_REG_A0);

    /* Patch the resume address first, so a Toolbox handler that relocates the
     * exception frame copies an already-correct PC. */
    m68k_write_memory_32(sp + 2, trap_pc + 2);

    if (word & 0x0800u) {
        /* Toolbox: arguments and result live on the stack.  A stub here is
         * worse than useless -- it leaves the stack unbalanced and the caller
         * returns into rubbish, which is how this first showed up (vector 4,
         * illegal instruction, four instructions after a stubbed
         * _GetResource). */
        served = serve_toolbox_trap(word, sp, d0, trap_pc + 2);
        if (!served) g_stub_count++;
    } else if (g_mem_traps && serve_memory_trap(word, &d0, &a0)) {
        served = 1;
    } else {
        TrapPolicy *p = &g_policy[word & 0x0FFFu];
        if (p->have) { d0 = p->d0; a0 = p->a0; served = 1; }
        else         { d0 = 0; g_stub_count++; }
    }

    /* An asynchronous call is not finished when the data has been copied. */
    if (served) queue_io_completion(word, a0, d0);

    if (g_trap_count < MAX_TRAPS) {
        TrapRec *t = &g_traps[g_trap_count];
        t->pc = trap_pc; t->word = word; t->served = served;
        t->d0 = d0; t->a0 = a0;
        t->d0_in = m68k_get_reg(NULL, M68K_REG_D0);
        t->a1 = m68k_get_reg(NULL, M68K_REG_A1);
        t->sp = sp;
        g_trap_count++;
    } else {
        g_trap_overflow++;
    }

    if (word & 0x0800u) return;      /* Toolbox already adjusted the frame */

    m68k_set_reg(M68K_REG_D0, d0);
    m68k_set_reg(M68K_REG_A0, a0);

    /* An OS trap returns its error in D0 *and* sets the condition codes from
     * it -- callers branch on the flags, not on D0.  Because the `rte` below
     * restores SR from the stack, setting the live CCR would be thrown away;
     * the stacked copy is the one that survives.
     *
     * Missing this made DriverOpen "succeed" in 14 instructions: _NewHandle
     * returned 0, the following `bne` saw a stale Z flag, and the driver took
     * its error exit and returned noErr.  A clean return that did nothing.
     * Toolbox traps ($A800+) pass results on the stack, so they are left
     * alone. */
    if (!(word & 0x0800u)) {
        unsigned sr = m68k_read_memory_16(sp);
        sr &= ~0x000Fu;                      /* N Z V C */
        if ((d0 & 0xFFFFu) == 0)  sr |= 0x04u;   /* Z */
        if (d0 & 0x8000u)         sr |= 0x08u;   /* N */
        m68k_write_memory_16(sp, sr);
    }
}

/* Run the driver's own callback proc, outside m68k_execute.
 *
 * Doing this from inside the trap handler would mean re-entering the CPU while
 * it is already running.  Instead the handler ends the timeslice, and we get
 * here with the machine stopped: push the two Pascal arguments and a magic
 * return address, jump to the proc, let it run to that address, then put PC and
 * SP back exactly as they were.  Register state is restored; the flag word the
 * callback set is a memory write, so it survives -- which is the whole point.
 */
static void run_pending_callback(void)
{
    unsigned proc = m68k_read_memory_32(g_cb_chan + 8);   /* SndChannel.callBack */
    unsigned save_pc, save_sp, sp;
    static const int R[15] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
        M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
        M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6 };
    unsigned save_reg[15];
    int k;

    g_cb_pending = 0;
    if (!proc) return;
    g_cb_runs++;

    save_pc = m68k_get_reg(NULL, M68K_REG_PC);
    save_sp = m68k_get_reg(NULL, M68K_REG_SP);
    for (k = 0; k < 15; k++)
        save_reg[k] = m68k_get_reg(NULL, (m68k_register_t)R[k]);

    sp = save_sp;
    sp -= 4; m68k_write_memory_32(sp, g_cb_chan);    /* chan, pushed first  */
    sp -= 4; m68k_write_memory_32(sp, CB_SCRATCH);   /* the SndCommand      */
    sp -= 4; m68k_write_memory_32(sp, MAGIC_CB_RET); /* return address      */
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, proc);

    g_in_callback = 1;
    while (g_in_callback && g_stop_reason == STOP_RUNNING)
        m68k_execute(100000);
    g_in_callback = 0;

    for (k = 0; k < 15; k++)
        m68k_set_reg((m68k_register_t)R[k], save_reg[k]);
    m68k_set_reg(M68K_REG_PC, save_pc);
    m68k_set_reg(M68K_REG_SP, save_sp);
}

/* The second half of OpenComponent, run with the machine stopped.
 *
 * The component's Open handler has just returned to the magic address.  Its
 * own result says whether it succeeded; the caller wants an instance, or nil
 * if it did not.  Then put the caller back exactly where the tail-call
 * convention would have left it: SP on its result slot, PC past the trap. */
static void finish_open_component(void)
{
    PendingOpen *p;
    unsigned res;

    g_copen_ret = 0;
    if (g_pending_n <= 0) return;
    p = &g_pending[--g_pending_n];

    res = m68k_read_memory_32(p->callee_result);
    m68k_write_memory_32(p->result_slot, res == 0 ? p->inst_token : 0);
    m68k_set_reg(M68K_REG_SP, p->result_slot);
    m68k_set_reg(M68K_REG_PC, p->resume_pc);
}

/* Run one deferred task, outside m68k_execute.
 *
 * Same construction as run_pending_callback, with two differences: the task
 * takes its argument in A1 rather than on the stack, and it ends in `rts`, so
 * a magic return address on the stack is all that is needed to catch it. */
static void run_pending_deferred(void)
{
    unsigned proc = g_dt_proc, parm = g_dt_parm;
    unsigned save_pc, save_sp, save_a1, sp;
    static const int R[15] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
        M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
        M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6 };
    unsigned save_reg[15];
    int k;

    g_dt_pending = 0;
    if (!proc) return;
    g_dt_runs++;

    save_pc = m68k_get_reg(NULL, M68K_REG_PC);
    save_sp = m68k_get_reg(NULL, M68K_REG_SP);
    save_a1 = m68k_get_reg(NULL, M68K_REG_A1);
    for (k = 0; k < 15; k++)
        save_reg[k] = m68k_get_reg(NULL, (m68k_register_t)R[k]);

    sp = save_sp - 4;
    m68k_write_memory_32(sp, MAGIC_DT_RET);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_A1, parm);
    m68k_set_reg(M68K_REG_PC, proc);

    g_in_deferred = 1;
    while (g_in_deferred && g_stop_reason == STOP_RUNNING)
        m68k_execute(100000);
    g_in_deferred = 0;

    for (k = 0; k < 15; k++)
        m68k_set_reg((m68k_register_t)R[k], save_reg[k]);
    m68k_set_reg(M68K_REG_PC, save_pc);
    m68k_set_reg(M68K_REG_SP, save_sp);
    m68k_set_reg(M68K_REG_A1, save_a1);
}

/* Run one File Manager completion routine, outside m68k_execute.
 *
 * The documented convention is **A0 = ParmBlkPtr and D0 = the result code**,
 * and MacinTalk Pro's routine depends on both: it takes its whole context from
 * A0, and brackets its body with `exg.l d0,a5` to install its own A5 world and
 * put the caller's back -- which is what a routine written to be entered at
 * interrupt time looks like.  It ends in `rts`, so a magic return address on
 * the stack is enough to catch it.
 *
 * FIFO, because requests complete in the order the caller made them and the
 * caller keeps its own state per request. */
static void run_pending_completion(void)
{
    unsigned proc, pb, result;
    unsigned save_pc, save_sp, sp;
    static const int R[15] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
        M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
        M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6 };
    unsigned save_reg[15];
    int k;

    if (g_ioc_n <= 0) return;
    proc = g_ioc[0].proc; pb = g_ioc[0].pb; result = g_ioc[0].result;
    for (k = 1; k < g_ioc_n; k++) g_ioc[k - 1] = g_ioc[k];
    g_ioc_n--;
    if (!proc) return;
    g_ioc_runs++;

    save_pc = m68k_get_reg(NULL, M68K_REG_PC);
    save_sp = m68k_get_reg(NULL, M68K_REG_SP);
    for (k = 0; k < 15; k++)
        save_reg[k] = m68k_get_reg(NULL, (m68k_register_t)R[k]);

    sp = save_sp - 4;
    m68k_write_memory_32(sp, MAGIC_IOC_RET);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_A0, pb);
    m68k_set_reg(M68K_REG_D0, result);
    m68k_set_reg(M68K_REG_PC, proc);

    g_in_ioc = 1;
    while (g_in_ioc && g_stop_reason == STOP_RUNNING)
        m68k_execute(100000);
    g_in_ioc = 0;

    for (k = 0; k < 15; k++)
        m68k_set_reg((m68k_register_t)R[k], save_reg[k]);
    m68k_set_reg(M68K_REG_PC, save_pc);
    m68k_set_reg(M68K_REG_SP, save_sp);
}

static int callback_due_in_call(void)
{
    return g_cb_pending
        && (!g_defer_cb || g_instr_count - g_cb_queued_instr >= g_cb_wait);
}

static void instr_hook(unsigned int pc)
{
    if (g_tick_auto
            && g_instr_count - g_tick_instr >= (long long)INSTR_PER_TICK) {
        g_tick_instr = g_instr_count;
        g_ticks++;
        m68k_write_memory_32(TICKS_ADDR, g_ticks);
    }
    if (g_trace_on) g_trace[g_trace_pos++ & (TRACE_CAP - 1u)] = pc;
    if (g_snap_pc && pc == g_snap_pc && g_snap_n < SNAP_CAP) {
        static const int R[17] = {
            M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
            M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
            M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
            M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
            M68K_REG_PC };
        int k;
        for (k = 0; k < 17; k++)
            g_snap[g_snap_n][k] = m68k_get_reg(NULL, (m68k_register_t)R[k]);
        g_snap_n++;
        if (g_snap_halt && g_snap_n >= g_snap_halt) {
            g_stop_reason = STOP_BREAK;
            g_stop_pc = pc;
            m68k_end_timeslice();
            return;
        }
    }
    if (pc == MAGIC_CB_RET) {
        g_in_callback = 0;
        m68k_end_timeslice();
        return;
    }
    /* A component's Open handler has returned.  Finish the OpenComponent that
     * started it -- outside m68k_execute, the way the sound callback does it,
     * rather than redirecting PC from inside the hook. */
    if (pc == MAGIC_DT_RET) {
        g_in_deferred = 0;
        m68k_end_timeslice();
        return;
    }
    if (pc == MAGIC_IOC_RET) {
        g_in_ioc = 0;
        m68k_end_timeslice();
        return;
    }
    if (pc == MAGIC_COPEN_RET) {
        g_copen_ret = 1;
        m68k_end_timeslice();
        return;
    }
    if (pc >= MAGIC_EXC_BASE && pc < MAGIC_EXC_BASE + 64u * MAGIC_EXC_SLOT) {
        int vec = (int)((pc - MAGIC_EXC_BASE) / MAGIC_EXC_SLOT);
        if (vec == 10) { service_atrap(); return; }   /* A-line: expected   */
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = vec;
        g_stop_pc = m68k_read_memory_32(m68k_get_reg(NULL, M68K_REG_SP) + 2);
        m68k_end_timeslice();
        return;
    }
    if (pc == g_sentinel) {
        g_stop_reason = STOP_SENTINEL;
        g_stop_pc = pc;
        m68k_end_timeslice();
        return;
    }
    g_instr_total++;
    if (++g_instr_count > g_instr_budget) {
        g_stop_reason = STOP_BUDGET;      /* counted, never silent */
        g_stop_pc = pc;
        m68k_end_timeslice();
    }
}
