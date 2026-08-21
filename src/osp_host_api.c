/* -------------------------------------------------------------- the API -- */

OSP_API int osp_init(unsigned ram_size)
{
    unsigned v;
    if (g_ram) free(g_ram);
    if (ram_size < 0x00F30000u) ram_size = 0x00F30000u;  /* magic pages fit */
    g_ram = (unsigned char *)calloc(ram_size, 1);
    if (!g_ram) return -1;
    g_ram_size = ram_size;
    if (!g_pcm) g_pcm = (unsigned char *)malloc(PCM_CAP);
    if (!g_pcm) return -1;
    if (!g_trace) g_trace = (unsigned *)malloc(TRACE_CAP * sizeof(unsigned));
    if (!g_trace) return -1;
    g_trace_pos = 0; g_trace_on = 0;
    g_snap_pc = 0; g_snap_n = 0; g_snap_halt = 0;

    /* 68000 unless an engine asks for more; see osp_set_cpu.  `.sp` and
     * MacinTalk 2 are 68000 code and stay that way. */
    g_gestalt_proc = 1;
    g_exc_frame = 6u;
    g_instance_error = 0;
    g_sane_env = 0;
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_instr_hook_callback(instr_hook);

    /* Vectors go in BEFORE the reset pulse, because reset loads SSP from $0 and
     * PC from $4.  osp_call sets both explicitly so the old order never bit us,
     * but anything that runs without osp_call first would have started from
     * zeroed memory -- and silently. */
    m68k_write_memory_32(0, 0x00200000u);      /* initial SSP */
    m68k_write_memory_32(4, MAGIC_SENTINEL);   /* initial PC  */
    /* Point every vector at its own identifiable slot, and give the A-line
     * slot a real `rte` so servicing can just fall through it. */
    for (v = 2; v < 64; v++)
        m68k_write_memory_32(v * 4u, MAGIC_EXC_BASE + v * MAGIC_EXC_SLOT);
    m68k_write_memory_16(MAGIC_EXC_BASE + 10u * MAGIC_EXC_SLOT, 0x4E73); /* rte */

    m68k_pulse_reset();

    memset(g_policy, 0, sizeof g_policy);
    g_trap_count = g_trap_overflow = g_stub_count = g_fault_count = 0;
    g_stackpc_is_instruction = -1;
    /* osp_init doubles as "start over", and the driver rebuilds the emulator
     * whenever the user crosses between engines, so the pristine copies from
     * the previous life have to go or every switch leaks a voice's worth. */
    for (v = 0; v < g_res_count; v++) {
        if (g_res[v].bytes) free(g_res[v].bytes);
        g_res[v].bytes = NULL; g_res[v].len = 0; g_res[v].detached = 0;
    }
    { int fi; for (fi = 0; fi < g_file_count; fi++) {
        if (g_files[fi].fork[0]) free(g_files[fi].fork[0]);
        if (g_files[fi].fork[1]) free(g_files[fi].fork[1]);
    } }
    memset(g_files, 0, sizeof(g_files));
    memset(g_open, 0, sizeof(g_open));
    g_file_count = 0;
    g_cur_res_file = -1;
    g_handle_count = 0;
    g_res_count = 0; g_reslog_n = 0; g_voice_count = 0; g_res_load = 1; g_res_err = 0; g_ticks = 0; g_tick_instr = 0; g_tick_auto = 0; g_seed_cb = 0;
    g_comp_count = 0; g_inst_count = 0;
    g_cmlog_n = 0; g_cp_slot = 0; g_cp_wraps = 0;
    g_pending_n = 0; g_copen_ret = 0; g_framelog_n = 0;
    g_pcm_len = 0; g_buffers_taken = 0; g_pcm_overflow = 0; g_short_buffers = 0;
    g_buflog_n = 0;
    g_cb_pending = 0; g_in_callback = 0; g_cb_queued_instr = 0;
    g_sample_rate = 0;
    g_cb_runs = 0; g_sndlog_n = 0; g_defer_cb = 0;
    g_cb_wait = IN_CALL_CB_WAIT;
    g_dt_pending = 0; g_in_deferred = 0; g_dt_runs = 0;
    g_dt_proc = g_dt_parm = 0;
    g_ioc_n = 0; g_in_ioc = 0; g_ioc_runs = 0; g_ioc_dropped = 0;
    g_heap_base = g_heap_end = g_heap_next = 0;
    g_mem_traps = 0;
    return 0;
}

OSP_API void osp_shutdown(void)
{
    int i;
    /* The DLL is unloaded after this, so these would leak into NVDA's heap
     * for the rest of its life -- a voice's worth per engine switch. */
    for (i = 0; i < g_res_count; i++) {
        if (g_res[i].bytes) free(g_res[i].bytes);
        g_res[i].bytes = NULL; g_res[i].len = 0; g_res[i].detached = 0;
    }
    g_res_count = 0;
    { int fi; for (fi = 0; fi < g_file_count; fi++) {
        if (g_files[fi].fork[0]) free(g_files[fi].fork[0]);
        if (g_files[fi].fork[1]) free(g_files[fi].fork[1]);
    } }
    memset(g_files, 0, sizeof(g_files));
    memset(g_open, 0, sizeof(g_open));
    g_file_count = 0;
    if (g_ram) free(g_ram);
    g_ram = NULL; g_ram_size = 0;
}

OSP_API int osp_write_block(unsigned addr, const unsigned char *data, int len)
{
    if (!in_ram(addr, (unsigned)len)) return -1;
    memcpy(g_ram + addr, data, (size_t)len);
    return 0;
}
OSP_API int osp_read_block(unsigned addr, unsigned char *out, int len)
{
    if (!in_ram(addr, (unsigned)len)) return -1;
    memcpy(out, g_ram + addr, (size_t)len);
    return 0;
}

OSP_API void osp_w8 (unsigned a, unsigned v) { m68k_write_memory_8(a, v); }
OSP_API void osp_w16(unsigned a, unsigned v) { m68k_write_memory_16(a, v); }
OSP_API void osp_w32(unsigned a, unsigned v) { m68k_write_memory_32(a, v); }
OSP_API unsigned osp_r8 (unsigned a) { return m68k_read_memory_8(a); }
OSP_API unsigned osp_r16(unsigned a) { return m68k_read_memory_16(a); }
OSP_API unsigned osp_r32(unsigned a) { return m68k_read_memory_32(a); }

/* Choose the CPU, because not every engine runs on the same one.
 *
 * MacinTalk Pro checks Gestalt('proc') during Open and refuses anything below
 * a 68020 -- see the _Gestalt case -- so "Pro" is a hardware requirement and
 * not just a name.  `.sp` and MacinTalk 2 are 68000 code and are left alone;
 * only one engine is live at a time, so this is per-engine rather than global.
 *
 * `proc` is the Gestalt processor value, 1..5, which keeps the CPU and the
 * answer we give about it in one place and impossible to disagree.  Call it
 * straight after osp_init, before loading any code. */
OSP_API int osp_instance_error(void) { return g_instance_error; }
OSP_API const char *osp_last_file_request(void) { return g_last_file_req; }

OSP_API int osp_set_cpu(int proc)
{
    unsigned type;
    switch (proc) {
        case 1: type = M68K_CPU_TYPE_68000; break;
        case 2: type = M68K_CPU_TYPE_68010; break;
        case 3: type = M68K_CPU_TYPE_68020; break;
        case 4: type = M68K_CPU_TYPE_68030; break;
        case 5: type = M68K_CPU_TYPE_68040; break;
        default: return -1;
    }
    g_gestalt_proc = proc;
    /* The 68010 introduced the format/vector word and everything after it kept
     * it, so only the 68000 has a six-byte frame. */
    g_exc_frame = (proc == 1) ? 6u : 8u;
    m68k_set_cpu_type(type);
    m68k_pulse_reset();
    return 0;
}

OSP_API void osp_heap_init(unsigned base, unsigned size)
{
    g_heap_base = g_heap_next = base;
    g_heap_end = base + size;
    /* Fill in the one zone (see MAGIC_ZONE).  Only the fields a client can
     * reasonably read are set; a caller that walks the free list would need
     * far more, and the trap log will say so if one ever does.  Written here
     * rather than at osp_init because the honest answers are the heap's. */
    m68k_write_memory_32(MAGIC_ZONE +  0, g_heap_end);        /* bkLim      */
    m68k_write_memory_32(MAGIC_ZONE +  4, 0);                 /* purgePtr   */
    m68k_write_memory_32(MAGIC_ZONE +  8, 0);                 /* hFstFree   */
    m68k_write_memory_32(MAGIC_ZONE + 12, size);              /* zcbFree    */
    m68k_write_memory_32(MAGIC_ZONE + 16, 0);                 /* gzProc     */
    m68k_write_memory_16(MAGIC_ZONE + 20, 64);                /* moreMast   */
    m68k_write_memory_32(MAGIC_ZONE + 48, base);              /* allocPtr   */
}
OSP_API void osp_enable_mem_traps(int on) { g_mem_traps = on ? 1 : 0; }
OSP_API unsigned osp_heap_used(void) { return g_heap_next - g_heap_base; }

/* Load a resource into emulated memory and register it under (type, id).
 * Returns the Handle, or 0 if the heap or the table is full.
 *
 * IDs are the driver's, not the file's: `.sp` asks for TALK 1 while
 * outspoken.bin stores TALK 1001.  The +1000 mapping is the caller's problem,
 * deliberately -- it is a fact about outSPOKEN, not about the Resource
 * Manager. */
OSP_API unsigned osp_add_resource(unsigned type, int id,
                                  const unsigned char *data, int len,
                                  int file_index)
{
    unsigned blk, mp;
    if (g_res_count >= MAX_RES) return 0;
    blk = heap_alloc((unsigned)len);
    if (!blk) return 0;
    memcpy(g_ram + blk, data, (size_t)len);
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    note_handle(mp, (unsigned)len);
    g_res[g_res_count].type = type;
    g_res[g_res_count].id = (short)id;
    g_res[g_res_count].handle = mp;
    /* Keep the resource as it arrived. A client that modifies what it is given
     * -- MacinTalk 2 patches a voice's `ttvi` -- must still get the original
     * the next time it asks. See res_find. Registration failing for want of a
     * few kilobytes is not worth failing the load over, so a null copy simply
     * means "cannot restore this one".*/
    g_res[g_res_count].bytes = (unsigned char *)malloc((size_t)len);
    g_res[g_res_count].len = len;
    g_res[g_res_count].detached = 0;
    g_res[g_res_count].map_entry = 0;
    g_res[g_res_count].file = file_index;
    memset(g_res[g_res_count].name, 0, sizeof(g_res[g_res_count].name));
    if (g_res[g_res_count].bytes)
        memcpy(g_res[g_res_count].bytes, data, (size_t)len);
    g_res_count++;
    return mp;
}

/* Give a registered resource its Mac name, so Get1NamedResource can find it.
 *
 * Separate from osp_add_resource rather than another argument to it, because
 * the engines that came first never needed names and their callers should not
 * have to say so.  -> 0, or -1 if that resource was never registered. */
OSP_API int osp_name_resource(unsigned handle, const char *name, int len)
{
    ResEntry *r = res_by_handle(handle);
    if (!r || len < 0 || len > 63) return -1;
    memset(r->name, 0, sizeof(r->name));
    memcpy(r->name, name, (size_t)len);
    return 0;
}

/* Tell the host where a resource's entry sits in its file's resource map.
 *
 * Computed on the Python side, where the fork is already parsed -- see
 * rsrc.Resource.map_entry -- rather than parsing the map again in C.
 * -> 0, or -1 if that resource was never registered. */
OSP_API int osp_map_entry(unsigned handle, int entry)
{
    ResEntry *r = res_by_handle(handle);
    if (!r) return -1;
    r->map_entry = entry;
    return 0;
}

/* Register the one file: its name, and its data fork.
 *
 * Kept host-side rather than copied into emulated RAM. MacinTalk Pro's lexicon
 * is 572,928 bytes and it reads it a piece at a time through the File Manager,
 * so there is no reason for all of it to sit in the 68000's address space --
 * unlike a resource, which the engine gets a Handle to and dereferences. */
OSP_API int osp_add_file(const unsigned char *name, int namelen,
                         const unsigned char *data, int dlen,
                         const unsigned char *rsrc, int rlen)
{
    FileEntry *f;
    if (g_file_count >= MAX_FILES) return -1;
    if (namelen < 0 || namelen > 63 || dlen < 0 || rlen < 0) return -1;
    f = &g_files[g_file_count];
    memset(f, 0, sizeof(*f));
    memset(f->name, 0, sizeof(f->name));
    f->name[0] = (unsigned char)namelen;
    memcpy(f->name + 1, name, (size_t)namelen);
    if (data && dlen) {
        f->fork[FORK_DATA] = (unsigned char *)malloc((size_t)dlen);
        if (!f->fork[FORK_DATA]) return -1;
        memcpy(f->fork[FORK_DATA], data, (size_t)dlen);
        f->len[FORK_DATA] = dlen;
    }
    if (rsrc && rlen) {
        f->fork[FORK_RSRC] = (unsigned char *)malloc((size_t)rlen);
        if (!f->fork[FORK_RSRC]) return -1;
        memcpy(f->fork[FORK_RSRC], rsrc, (size_t)rlen);
        f->len[FORK_RSRC] = rlen;
    }
    return g_file_count++;
}

/* --- the Component Manager, from Python ------------------------------- */

/* Defined further down; declared here so the call below keeps its linkage. */
OSP_API int osp_call(unsigned entry, unsigned sentinel, long long max_instr);

OSP_API int osp_add_component(unsigned type, unsigned subtype, unsigned manuf,
                              unsigned entry)
{
    Component *c;
    if (g_comp_count >= MAX_COMPONENTS) return -1;
    c = &g_comp[g_comp_count];
    c->type = type; c->subtype = subtype; c->manuf = manuf; c->entry = entry;
    return g_comp_count++;
}

OSP_API unsigned osp_open_instance(int component)
{
    int i;
    if (component < 0 || component >= g_comp_count) return 0;
    if (g_inst_count >= MAX_INSTANCES) return 0;
    i = g_inst_count++;
    g_inst[i].component = component;
    g_inst[i].storage = 0;
    g_inst[i].refcon = 0;
    return INSTANCE_TOK(i);
}

OSP_API unsigned osp_instance_storage(unsigned token)
{
    int i = inst_of(token);
    return i < 0 ? 0u : g_inst[i].storage;
}

/* Play Speech Manager: build a ComponentParameters and call the component's
 * own entry point, the way the real Component Manager would.
 *
 * `args` is in **declared** order -- (self) for Open, and so on.  They are
 * written reversed, because params[0] is the last declared argument: that is
 * how the block sits on the stack at a measured `D0 = 0` call site, and the
 * glue copies it into a handler frame verbatim rather than re-ordering it.
 */
OSP_API int osp_component_call(unsigned token, int what,
                               const unsigned *args, int nargs,
                               long long max_instr, unsigned *result)
{
    unsigned cp = CP_TOPLEVEL, sp, res_slot;
    int ii = inst_of(token), i, r;

    if (ii < 0) return -2;
    if (nargs < 0 || nargs * 4 > 255) return -3;

    m68k_write_memory_8(cp + 0, 0);                      /* flags     */
    m68k_write_memory_8(cp + 1, (unsigned)(nargs * 4));  /* paramSize */
    m68k_write_memory_16(cp + 2, (unsigned)what & 0xFFFFu);
    for (i = 0; i < nargs; i++)
        m68k_write_memory_32(cp + 4u + (unsigned)i * 4u, args[nargs - 1 - i]);

    /* pascal ComponentResult main(ComponentParameters *p, Handle storage) --
     * left to right, so params goes deeper than storage. */
    sp = m68k_get_reg(NULL, M68K_REG_SP);
    sp -= 4; res_slot = sp; m68k_write_memory_32(res_slot, 0);
    sp -= 4; m68k_write_memory_32(sp, cp);
    sp -= 4; m68k_write_memory_32(sp, g_inst[ii].storage);
    m68k_set_reg(M68K_REG_SP, sp);

    r = osp_call(g_comp[g_inst[ii].component].entry, MAGIC_SENTINEL, max_instr);
    if (result) *result = m68k_read_memory_32(res_slot);
    return r;
}

OSP_API int osp_cm_log_n(void) { return g_cmlog_n; }
OSP_API int osp_cm_log_get(int i, unsigned *d0, unsigned *pc, unsigned *csp,
                           unsigned *words, int *served)
{
    CmRec *r;
    if (i < 0 || i >= g_cmlog_n) return 0;
    r = &g_cmlog[i];
    *d0 = r->d0; *pc = r->pc; *csp = r->csp; *served = r->served;
    words[0] = r->w0; words[1] = r->w1; words[2] = r->w2; words[3] = r->w3;
    return 1;
}
OSP_API int osp_cp_wraps(void) { return g_cp_wraps; }
/* Tell the Speech Manager -- us -- that a voice exists, and which `ttvd` id it
 * lives under.  Both halves come from the voice's own ttvd. */
OSP_API int osp_add_voice(unsigned creator, unsigned id, int res_id,
                          int file_index)
{
    if (g_voice_count >= MAX_VOICES) return -1;
    g_voices[g_voice_count].creator = creator;
    g_voices[g_voice_count].id = id;
    g_voices[g_voice_count].res_id = (short)res_id;
    g_voices[g_voice_count].file = file_index;
    return g_voice_count++;
}

OSP_API int osp_reslog_n(void) { return g_reslog_n; }
OSP_API int osp_reslog_get(int i, unsigned *type, int *id, int *found)
{
    if (i < 0 || i >= g_reslog_n) return 0;
    *type = g_reslog[i].type; *id = g_reslog[i].id;
    *found = g_reslog[i].found;
    return 1;
}

OSP_API int osp_framelog_n(void) { return g_framelog_n; }
OSP_API unsigned osp_framelog_get(int i, int k)
{
    return (i >= 0 && i < g_framelog_n && k >= 0 && k < 6)
        ? g_framelog[i][k] : 0u;
}

OSP_API unsigned osp_tick_count(void) { return g_ticks; }
OSP_API void osp_auto_ticks(int on) { g_tick_auto = on ? 1 : 0; }
OSP_API void osp_seed_callback(int on) { g_seed_cb = on ? 1 : 0; }

OSP_API void osp_set_trap_policy(unsigned word, unsigned d0, unsigned a0)
{
    TrapPolicy *p = &g_policy[word & 0x0FFFu];
    p->have = 1; p->d0 = d0; p->a0 = a0;
}

OSP_API void osp_set_reg(int reg, unsigned v) { m68k_set_reg((m68k_register_t)reg, v); }
OSP_API unsigned osp_get_reg(int reg) { return m68k_get_reg(NULL, (m68k_register_t)reg); }

/* Call a subroutine.  `sentinel` is pushed as the return address, so an `rts`
 * out of the routine lands on it and stops us cleanly. */
OSP_API int osp_call(unsigned entry, unsigned sentinel, long long max_instr)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    sp -= 4;
    m68k_write_memory_32(sp, sentinel);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, entry);

    g_sentinel = sentinel;
    g_stop_reason = STOP_RUNNING;
    g_stop_vector = -1;
    g_instr_count = 0;
    g_instr_budget = max_instr;

    while (g_stop_reason == STOP_RUNNING) {
        m68k_execute(100000);
        if (callback_due_in_call() && g_stop_reason == STOP_RUNNING)
            run_pending_callback();
        if (g_dt_pending && g_stop_reason == STOP_RUNNING)
            run_pending_deferred();
        if (g_ioc_n && g_stop_reason == STOP_RUNNING)
            run_pending_completion();
        if (g_copen_ret && g_stop_reason == STOP_RUNNING)
            finish_open_component();
    }

    return g_stop_reason;
}

/* Call a routine that takes Pascal arguments already pushed by the caller.
 * MACSTARTSOUND is reached this way, through the export table at driver+$001E. */
/* Continue after a snapshot breakpoint, leaving PC, SP and every register
 * exactly as the break left them.  Needed to test what NVDA does constantly:
 * stop an utterance half way through and then start another one. */
/* Keep answering the sound callback until the engine stops asking.
 *
 * MacinTalk 2's SpeakBuffer is asynchronous: it renders one buffer, queues it
 * behind a callBackCmd and returns, and everything after that is driven by the
 * Sound Manager calling back.  `.sp` never needed this because its Prime
 * rendered a whole utterance synchronously -- here the host has to keep being
 * the Sound Manager after the speak call has returned.
 *
 * Returns how many callbacks ran.  Hitting `max_rounds` is a stall, not a
 * finish, and the caller should say so rather than treat the audio as
 * complete. */
OSP_API int osp_run_callbacks(int max_rounds, long long max_instr)
{
    int n = 0;
    while ((g_cb_pending || g_dt_pending || g_ioc_n) && n < max_rounds) {
        g_stop_reason = STOP_RUNNING;
        g_stop_vector = -1;
        g_instr_count = 0;
        g_instr_budget = max_instr;
        /* The callback installs the deferred task, and the deferred task is
         * what renders, so both have to be drained or the chain stops half
         * way with a buffer of silence already queued. */
        /* **The deferred task goes first.**
         *
         * A deferred task is the tail of the interrupt that installed it: on a
         * real Macintosh it runs when interrupt processing ends, before any
         * later interrupt is taken. Running callbacks first inverts that, and
         * the task then executes one or more callbacks too late -- against
         * state the engine has already moved past.
         *
         * MacinTalk 3 is where it shows. Its sound callback installs a task
         * AND queues the next callBackCmd, so both are pending at once; with
         * callbacks first the task fired two rounds late, walked into a record
         * the engine had disposed of -- guard word `0x12345678` and all -- and
         * jumped through freed memory. */
        if (g_dt_pending)      run_pending_deferred();
        else if (g_cb_pending) run_pending_callback();
        else                   run_pending_completion();
        n++;
        if (g_stop_reason != STOP_RUNNING) break;
    }
    return n;
}

OSP_API int osp_resume(long long max_instr)
{
    if (g_stop_reason != STOP_BREAK) return g_stop_reason;
    g_stop_reason = STOP_RUNNING;
    g_instr_budget = max_instr;
    g_instr_count = 0;
    while (g_stop_reason == STOP_RUNNING) {
        m68k_execute(100000);
        if (callback_due_in_call() && g_stop_reason == STOP_RUNNING)
            run_pending_callback();
    }
    return g_stop_reason;
}

OSP_API int osp_call_with_args(unsigned entry, const unsigned *args, int nargs,
                               long long max_instr)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    int i;
    for (i = 0; i < nargs; i++) {
        sp -= 4;
        m68k_write_memory_32(sp, args[i]);
    }
    m68k_set_reg(M68K_REG_SP, sp);
    return osp_call(entry, MAGIC_SENTINEL, max_instr);
}

OSP_API unsigned osp_pcm_len(void)      { return g_pcm_len; }
OSP_API int  osp_buffers_taken(void)    { return g_buffers_taken; }
OSP_API int  osp_pcm_overflow(void)     { return g_pcm_overflow; }
OSP_API int  osp_short_buffers(void)    { return g_short_buffers; }
OSP_API unsigned osp_sample_rate(void)  { return g_sample_rate; }
OSP_API void osp_pcm_reset(void)
{
    g_pcm_len = 0; g_buffers_taken = 0; g_pcm_overflow = 0; g_short_buffers = 0;
}
OSP_API int osp_pcm_get(unsigned char *out, int max)
{
    unsigned n = g_pcm_len;
    if ((int)n > max) n = (unsigned)max;
    memcpy(out, g_pcm, n);
    return (int)n;
}
OSP_API unsigned osp_cb_scratch(void)   { return CB_SCRATCH; }

OSP_API void osp_defer_callbacks(int on) { g_defer_cb = on ? 1 : 0; }
/* Instructions to wait before answering a callback in-call. Per engine:
 * see the note beside IN_CALL_CB_WAIT. */
OSP_API void osp_cb_wait(long long n) { g_cb_wait = n > 0 ? n : 1; }
OSP_API int osp_cb_runs(void) { return g_cb_runs; }
OSP_API int osp_dt_runs(void) { return g_dt_runs; }
/* Completion routines run, and any a full queue had to drop.  Dropped is a
 * fault: the caller that never gets its callback waits forever. */
OSP_API int osp_ioc_runs(void) { return g_ioc_runs; }
OSP_API int osp_ioc_dropped(void) { return g_ioc_dropped; }
OSP_API int osp_sndlog_n(void) { return g_sndlog_n; }
OSP_API int osp_sndlog_get(int i)
{
    return (i >= 0 && i < g_sndlog_n) ? (int)g_sndlog[i] : -1;
}

OSP_API int osp_buflog_n(void) { return g_buflog_n; }
OSP_API int osp_buflog_get(int i, unsigned *addr, unsigned *len)
{
    if (i < 0 || i >= g_buflog_n) return -1;
    *addr = g_buflog_addr[i]; *len = g_buflog_len[i];
    return 0;
}

OSP_API void osp_snap_set(unsigned pc) { g_snap_pc = pc; g_snap_n = 0; }
OSP_API void osp_snap_halt(int nth) { g_snap_halt = nth; }
OSP_API int  osp_snap_n(void) { return g_snap_n; }
OSP_API int  osp_snap_get(int i, unsigned *out)
{
    int k;
    if (i < 0 || i >= g_snap_n) return -1;
    for (k = 0; k < 17; k++) out[k] = g_snap[i][k];
    return 0;
}

OSP_API void osp_rwatch_set(unsigned lo, unsigned hi)
{ g_rwatch_lo = lo; g_rwatch_hi = hi; g_rwatch_n = 0; }
OSP_API int osp_rwatch_n(void) { return g_rwatch_n; }
/* Oldest-first over whatever the ring still holds. */
OSP_API int osp_rwatch_get(int i, unsigned *pc, unsigned *addr)
{
    int have = g_rwatch_n < RWATCH_CAP ? g_rwatch_n : RWATCH_CAP;
    int start = g_rwatch_n - have;
    if (i < 0 || i >= have) return -1;
    *pc = g_rwatch_pc[(start + i) % RWATCH_CAP];
    *addr = g_rwatch_addr[(start + i) % RWATCH_CAP];
    return 0;
}

OSP_API void osp_watch_set(unsigned lo, unsigned hi)
{ g_watch_lo = lo; g_watch_hi = hi; g_watch_n = 0; }
OSP_API int osp_watch_n(void) { return g_watch_n; }
OSP_API int osp_watch_get(int i, unsigned *pc, unsigned *addr, unsigned *val, int *size)
{
    if (i < 0 || i >= g_watch_n || i >= WATCH_CAP) return -1;
    *pc = g_watch[i].pc; *addr = g_watch[i].addr;
    *val = g_watch[i].val; *size = g_watch[i].size;
    return 0;
}

OSP_API void osp_trace_enable(int on) { g_trace_on = on ? 1 : 0; g_trace_pos = 0; }
OSP_API unsigned osp_trace_len(void)
{
    return g_trace_pos < TRACE_CAP ? g_trace_pos : TRACE_CAP;
}
/* Copy the trace out oldest-first. */
OSP_API int osp_trace_get(unsigned *out, int max)
{
    unsigned n = osp_trace_len(), i, start;
    if ((int)n > max) n = (unsigned)max;
    start = g_trace_pos - n;
    for (i = 0; i < n; i++)
        out[i] = g_trace[(start + i) & (TRACE_CAP - 1u)];
    return (int)n;
}

OSP_API unsigned osp_trap_d0in(int i)
{
    return (i >= 0 && i < g_trap_count) ? g_traps[i].d0_in : 0u;
}

OSP_API int  osp_stop_reason(void)  { return g_stop_reason; }
OSP_API int  osp_stop_vector(void)  { return g_stop_vector; }
OSP_API unsigned osp_stop_pc(void)  { return g_stop_pc; }
OSP_API long long osp_instr_count(void) { return g_instr_count; }
OSP_API int  osp_trap_count(void)   { return g_trap_count; }
OSP_API int  osp_trap_overflow(void){ return g_trap_overflow; }
OSP_API int  osp_stub_count(void)   { return g_stub_count; }
OSP_API int  osp_fault_count(void)  { return g_fault_count; }
OSP_API int  osp_stackpc_convention(void) { return g_stackpc_is_instruction; }

OSP_API int osp_trap_get(int i, unsigned *pc, unsigned *word, unsigned *d0,
                         unsigned *a0, unsigned *a1, int *served)
{
    if (i < 0 || i >= g_trap_count) return -1;
    *pc = g_traps[i].pc; *word = g_traps[i].word; *d0 = g_traps[i].d0;
    *a0 = g_traps[i].a0; *a1 = g_traps[i].a1; *served = g_traps[i].served;
    return 0;
}

OSP_API int osp_fault_get(int i, unsigned *addr, unsigned *pc, int *write, int *size)
{
    if (i < 0 || i >= g_fault_count || i >= MAX_FAULTS) return -1;
    *addr = g_faults[i].addr; *pc = g_faults[i].pc;
    *write = g_faults[i].write; *size = g_faults[i].size;
    return 0;
}

OSP_API unsigned osp_magic_sentinel(void) { return MAGIC_SENTINEL; }
