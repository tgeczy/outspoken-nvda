/* Returns 1 if we served the trap.  `exc_sp` is the exception frame address. */
static int serve_toolbox_trap(unsigned short word, unsigned exc_sp,
                              unsigned d0, unsigned resume_pc)
{
    unsigned csp = exc_sp + EXC_FRAME;
    /* A Toolbox trap number is ten bits; only bit 10 is a flag (auto-pop).
     * Masking bit 9 as well -- the OS-trap rule -- turns $A9A0 into $A8A0 and
     * every resource call falls through to "stubbed". */
    unsigned base = word & 0xFBFFu;

    /* _ComponentDispatch carries its selector in D0 rather than on the stack,
     * which is why this one trap needs a register the others do not. */
    if (base == 0xA82Au)
        return serve_component_trap(d0, exc_sp, csp, resume_pc);

    if (serve_sound_trap(base, exc_sp, csp)) return 1;

    switch (base) {
    /* _Get1Resource searches only the current file where _GetResource walks
     * the whole chain.  We keep one flat table and no files at all, so the
     * distinction cannot arise -- but the trap still has to be *served*, since
     * a Toolbox stub leaves six bytes on the stack and the caller carries on
     * with a corrupted frame.  That is precisely what happened on the first
     * run: the _DetachResource after it reported D0 = 0x00017474. */
    case 0xA81F:                         /* _Get1Resource(type, id)          */
    case 0xA9A0: {                       /* _GetResource(type, id) -> Handle */
        short id = (short)m68k_read_memory_16(csp);
        unsigned type = m68k_read_memory_32(csp + 2);
        unsigned h = res_find_scoped(type, id, base == 0xA81Fu);
        log_res(type, id, h != 0);
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 6, h, 4);
        return 1;
    }
    case 0xA992: {                       /* _DetachResource(h)               */
        /* The resource leaves the map and the block becomes the caller's, so
         * whatever it does to those bytes from here on is its own business --
         * and MacinTalk 2 patches its `ttvi` as it loads a voice.  Mark it, so
         * that asking for it again gets the original back.  See res_find. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        if (r) r->detached = 1;
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, 0, 0);
        return 1;
    }
    case 0xA9A2:                         /* _LoadResource(h) -- already in   */
    case 0xA9A3:                         /* _ReleaseResource(h)              */
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, 0, 0);
        return 1;
    case 0xA99B:                         /* _SetResLoad(Boolean)             */
        g_res_load = m68k_read_memory_8(csp + 1) ? 1 : 0;
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    case 0xA99C:                         /* _CountResources(type)  -> short   */
    case 0xA80D: {                       /* _Count1Resources(type) -> short   */
        /* The engine enumerates by type -- count them, then walk them by
         * index -- to find which voices and modules are present.
         *
         * Stubbed, the count was whatever the caller's reserved word already
         * held: it came back as thousands, and the walk that followed made
         * 3,954 _ReleaseResource calls and took 26,245 memory faults reading
         * from addresses like 0x70726F6B.  Nothing about that looks like a
         * missing trap until you count the calls. */
        unsigned type = m68k_read_memory_32(csp);
        int one = (base == 0xA80Du), i, n = 0;
        for (i = 0; i < g_res_count; i++)
            if (g_res[i].type == type && res_in_file(i, one)) n++;
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, (unsigned)n, 2);
        return 1;
    }
    case 0xA99D:                         /* _GetIndResource(type, i)          */
    case 0xA80E: {                       /* _Get1IndResource(type, i) -> Hdl  */
        /* One-based, as the Resource Manager is, and in the order they were
         * registered -- which is the order they came out of the file. */
        int idx = (int)(short)m68k_read_memory_16(csp);
        unsigned type = m68k_read_memory_32(csp + 2);
        int one = (base == 0xA80Eu), i, seen = 0;
        unsigned h = 0;
        for (i = 0; i < g_res_count && !h; i++)
            if (g_res[i].type == type && res_in_file(i, one) && ++seen == idx) {
                res_restore(&g_res[i]);
                h = g_res[i].handle;
            }
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 6, h, 4);
        return 1;
    }
    case 0xA9A1:                         /* _GetNamedResource(type, name)     */
    case 0xA820: {                       /* _Get1NamedResource(type, name)    */
        /* **This is how MacinTalk Pro finds everything.** It is a modular
         * engine addressed by name rather than by id: `*TTS`, `*Wave`, `*Snd`,
         * `*Lex`, `*Cmd`, `*XPh`, `*XAl`, `*PhX`, `*AlX`, `*WvX`, plus
         * `EnglPhon` and `EnglAllo`; a voice is `EnglMBruceData`,
         * `EnglMBruceCode`, `EnglMBruce` and `EnglMBruceWave`.
         *
         * Serving it matters even when the answer is nil: the caller reserves
         * four bytes for the Handle, and a stub never writes them, so the
         * engine carries on with whatever was on the stack. That is how a
         * stack address once reached _SizeResource as a Handle -- non-zero, so
         * it passed the nil test, and the failure surfaced two calls later
         * as memFullErr. */
        unsigned name = m68k_read_memory_32(csp);
        unsigned type = m68k_read_memory_32(csp + 4);
        unsigned char buf[64];
        unsigned h = 0;
        int n = 0, i;
        if (name) {
            n = (int)m68k_read_memory_8(name);
            if (n > 63) n = 63;
            for (i = 0; i < n; i++)
                buf[i] = (unsigned char)m68k_read_memory_8(name + 1u + (unsigned)i);
            h = res_find_named(type, buf, n);
        }
        log_res(type, (short)(h ? 0 : -1), h != 0);
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 8, h, 4);
        return 1;
    }
    case 0xA9C5: {                       /* _RsrcMapEntry(h) -> long         */
        /* Where this resource's twelve-byte reference entry sits, measured
         * from the start of the resource MAP.  Named from its call site at
         * gtse 1 +$5B16: one Handle in, a long out, zero treated as failure
         * with _ResError straight after.
         *
         * What Pro does with it is the reason the whole File Manager exists
         * here.  It adds the offset to its in-memory copy of the map, pulls
         * the 24-bit data offset out of the entry with a 68020 bitfield
         * instruction, adds the data-area base from the map's own header copy,
         * seeks to that plus four, and reads SizeResource bytes.  It never
         * loads the 800 KB database into a Handle at all -- which is how this
         * ran on a Mac with 8 MB of RAM. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        g_res_err = (r && r->map_entry) ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 4, (r && r->map_entry) ? (unsigned)r->map_entry : 0, 4);
        return 1;
    }
    /* ---- Fixed point, named from their call sites --------------------- *
     *
     * `$A84D` is handed #$FF0000 and #$10000 -- 255.0 and 1.0 -- and returns
     * one long, and `$A844` is handed a pointer to ten bytes the caller just
     * filled with a word and two longs, which is an extended, and returns a
     * long.  They sit in the documented contiguous block with Long2Fix,
     * Fix2X, X2Frac, FracMul, FixMul and FixRound, and every unserved trap in
     * this engine falls inside it.
     *
     * Toolbox convention: the caller reserves the result, then pushes
     * arguments left to right, so the LAST argument is on top. */
    case 0xA83F:                         /* _Long2Fix(long) -> Fixed          */
        tb_return(exc_sp, 4,
                  (unsigned)(m68k_read_memory_32(csp) << 16), 4);
        return 1;
    case 0xA840:                         /* _Fix2Long(Fixed) -> long          */
        tb_return(exc_sp, 4,
                  (unsigned)((fx_signed(m68k_read_memory_32(csp)) + 0x8000)
                             >> 16), 4);
        return 1;
    case 0xA868:                         /* _FixMul(Fixed, Fixed) -> Fixed    */
        tb_return(exc_sp, 8, fx_mul(m68k_read_memory_32(csp + 4),
                                    m68k_read_memory_32(csp)), 4);
        return 1;
    case 0xA869: {                       /* _FixRatio(short, short) -> Fixed  */
        /* Named from its two call sites in `*Wave`, which settle the shape
         * without any guessing.  At +$4BC4:
         *
         *     clr.l   -(a7)              ; the outer result slot
         *     move.l  -$e(a6), -(a7)     ; a Fixed
         *     clr.l   -(a7)              ; this call's result slot
         *     move.w  $2(a4,d0.w), -(a7) ; numerator, out of a table
         *     move.w  #$64, -(a7)        ; denominator: 100
         *     _FixRatio
         *     _FixMul
         *     move.l  (a7)+, d0
         *
         * Two calls back to back popping one long says this takes four bytes
         * of arguments and leaves four, nested inside a FixMul that takes
         * eight -- and x/100 as a Fixed is a percentage, which is what the
         * table holds.
         *
         * **Unserved it was worse than absent.** A Toolbox stub leaves the
         * arguments on the stack and the result slot unwritten, so `*Wave`
         * took a null for a table base and walked it in 82-byte steps: twenty
         * million out-of-range reads in one utterance, from two missing
         * traps.  See the rule at the top of macintalkpro-notes.
         *
         * Apple saturates rather than trapping, and so does this. */
        int denom = (int)(short)m68k_read_memory_16(csp);
        int numer = (int)(short)m68k_read_memory_16(csp + 2);
        unsigned r;
        if (denom == 0) {
            r = numer < 0 ? 0x80000000u : 0x7FFFFFFFu;
        } else {
            long long q = ((long long)numer << 16) / denom;
            r = q > 0x7FFFFFFFLL ? 0x7FFFFFFFu
              : q < -0x80000000LL ? 0x80000000u
              : (unsigned)(int)q;
        }
        tb_return(exc_sp, 4, r, 4);
        return 1;
    }
    case 0xA84D:                         /* _FixDiv(Fixed, Fixed) -> Fixed    */
        tb_return(exc_sp, 8, fx_div(m68k_read_memory_32(csp + 4),
                                    m68k_read_memory_32(csp)), 4);
        return 1;
    case 0xA86C:                         /* _FixRound(Fixed) -> short         */
        tb_return(exc_sp, 4,
                  (unsigned)((fx_signed(m68k_read_memory_32(csp)) + 0x8000)
                             >> 16), 2);
        return 1;
    case 0xA84A: {                       /* _FracMul(Fract, Fract) -> Fract   */
        /* Fract is 2.30, so the product shifts by 30 rather than 16. */
        long long r = (long long)fx_signed(m68k_read_memory_32(csp + 4))
                    * (long long)fx_signed(m68k_read_memory_32(csp));
        tb_return(exc_sp, 8, (unsigned)((r + (1 << 29)) >> 30), 4);
        return 1;
    }
    case 0xA843: {                       /* _Fix2X(extended *, Fixed)         */
        /* The pointer is a ten-byte local -- `-$a(a6)` at gtse 1 +$7C88 --
         * which is an extended exactly.  The reserved long is discarded by
         * the caller, so answering with the value costs nothing and satisfies
         * either reading of the result. */
        unsigned val = m68k_read_memory_32(csp);
        unsigned dst = m68k_read_memory_32(csp + 4);
        if (dst) ext80_write(dst, fx_signed(val) / 65536.0);
        tb_return(exc_sp, 8, val, 4);
        return 1;
    }
    /* _X2Fix / _X2Frac take a pointer to an extended and return the scalar in
     * place of that pointer -- every call site in every module (Fix2X's twin
     * `Fix2X` reserves a result slot; these do not) is
     *
     *     pea     -$a(a6)        push the extended pointer   (4 bytes)
     *     _X2Fix
     *     move.l  (a7)+, dst     pop the result over the same slot
     *
     * so the result overwrites the argument and NOTHING is popped: param_bytes
     * must be 0, not 4.  With 4 the result landed one slot high and SP came back
     * four bytes light per call; two calls in the 'pbas' setter left a function's
     * `movem` restore reading eight bytes into its own saved d6/d7.  gala never
     * noticed -- its voice-select ignores those registers -- but Spanish Pro's
     * reads d7 as a status and saw the pitch's low word as an error (20125). */
    case 0xA844: {                       /* _X2Fix(extended *) -> Fixed       */
        unsigned src = m68k_read_memory_32(csp);
        double v = src ? ext80_read(src) : 0.0;
        double f = v * 65536.0;
        long long r;
        if (f > 2147483647.0) r = 2147483647;
        else if (f < -2147483648.0) r = -2147483648LL;
        else r = (long long)(f < 0 ? f - 0.5 : f + 0.5);
        tb_return(exc_sp, 0, (unsigned)r, 4);
        return 1;
    }
    case 0xA846: {                       /* _X2Frac(extended *) -> Fract      */
        unsigned src = m68k_read_memory_32(csp);
        double v = src ? ext80_read(src) : 0.0;
        double f = v * 1073741824.0;               /* 2^30 */
        long long r;
        if (f > 2147483647.0) r = 2147483647;
        else if (f < -2147483648.0) r = -2147483648LL;
        else r = (long long)(f < 0 ? f - 0.5 : f + 0.5);
        tb_return(exc_sp, 0, (unsigned)r, 4);
        return 1;
    }
    case 0xA9EB:                         /* _Pack4 / FP68K                   */
    case 0xA9EC:                         /* _Pack5 / Elems68K                */
        return serve_sane_trap((unsigned short)base, exc_sp, csp, resume_pc);
    case 0xA9A5: {                       /* _SizeResource(h) -> long          */
        /* How big is this resource.  Answerable exactly, because the pristine
         * copy kept for _DetachResource carries the length -- and a wrong
         * answer here is expensive: the engine sizes an allocation from it,
         * and stubbed it read whatever was on the stack and asked for that
         * many bytes.  Open failed with memFullErr and an almost empty heap. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        g_res_err = r ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 4, r ? (unsigned)r->len : 0xFFFFFFFFu, 4);
        return 1;                        /* -1 is the documented failure     */
    }
    case 0xA9A6:                         /* _GetResAttrs(h) -> short          */
        /* Nothing is purgeable, preloaded, protected or changed here: the
         * resources come from a folder and never go back. */
        tb_return(exc_sp, 4, 0, 2);
        return 1;
    case 0xA906: {                       /* _NewString(str) -> StringHandle   */
        /* gtse 1 +$65EA reserves a long, pushes a pointer to a Pascal string
         * inside a table it is walking, and stores the result as a Handle --
         * treating nil as MemErr and failing.  47 of them during Open.
         *
         * Worth serving even though Open already returned noErr without it:
         * stubbed, the reserved long was never written, and the engine kept
         * whatever was on the stack.  It was non-zero often enough to pass the
         * nil test, so Open succeeded holding 47 handles to nothing. */
        unsigned src = m68k_read_memory_32(csp);
        unsigned n = src ? m68k_read_memory_8(src) : 0;
        unsigned h = heap_new_handle(n + 1);
        if (h) {
            unsigned blk = m68k_read_memory_32(h), i;
            for (i = 0; i <= n; i++)
                m68k_write_memory_8(blk + i, m68k_read_memory_8(src + i));
        }
        m68k_write_memory_16(MEM_ERR_ADDR, h ? 0u : 0xFF94u);
        tb_return(exc_sp, 4, h, 4);
        return 1;
    }
    case 0xAA52:                         /* _HighLevelFSDispatch, sel in D0   */
        /* Selector 1 is FSMakeFSSpec, read off its arguments rather than
         * recalled: gtse 1 +$F76 reserves a word for the result and pushes a
         * word, a long, a pointer and a pointer -- (vRefNum, dirID, fileName,
         * spec) -- and the three inputs are exactly what it just read out of
         * PBGetFCBInfo.  The 70-byte FSSpec it fills lands at +$1C of the
         * engine's globals, and +$1C plus 70 is +$62, which is the next field
         * the code writes. */
        if (d0 == 1u) {
            unsigned spec = m68k_read_memory_32(csp);
            unsigned name = m68k_read_memory_32(csp + 4);
            unsigned dir  = m68k_read_memory_32(csp + 8);
            unsigned vref = m68k_read_memory_16(csp + 12);
            m68k_write_memory_16(spec + 0, vref);
            m68k_write_memory_32(spec + 2, dir);
            /* The caller may pass the name it just got back, or nil to mean
             * "the file itself"; either way there is one file here. */
            (void)name;
            file_put_name(0, spec + 6);
            tb_return(exc_sp, 14, 0, 2);
            return 1;
        }
        return 0;
    case 0xA994:                         /* _CurResFile -> short             */
        tb_return(exc_sp, 0, 1, 2);
        return 1;
    case 0xA9A4:                         /* _HomeResFile(h) -> short         */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    case 0xA998: {                       /* _UseResFile(short)               */
        /* Selects which file Get1* searches. MacinTalk Pro switches between
         * its own file and a voice's, and they carry resources with the SAME
         * type and id -- `gtsg 0` is 1,032 bytes in the engine and 110 in a
         * voice -- so ignoring this hands back the wrong one. refNum 1 is the
         * flat table, which means "no particular file". */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(csp));
        g_cur_res_file = f ? f->file : -1;
        if (f) {
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(f->file));
            m68k_write_memory_16(CUR_MAP_ADDR,
                                 (unsigned)m68k_read_memory_16(csp));
        }
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    }
    case 0xA997: {                       /* _OpenResFile(name) -> short      */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    }
    case 0xA81A: {                       /* _HOpenResFile -> short refNum    */
        /* HOpenResFile(short vRefNum, long dirID, ConstStr255Param fileName,
         *              SignedByte permission)
         *
         * Twelve bytes of arguments, so the name is the long at csp+2.  Opens
         * the resource fork for real, same as _OpenRFPerm -- what mattered
         * before was only that the refNum was not -1, which is the value
         * Cecy 1 +$830 tests for, and that is still true. */
        int idx = no_files() ? -1 : file_by_name(m68k_read_memory_32(csp + 2));
        int ref = no_files() ? 1 : file_open(idx, FORK_RSRC);
        if (ref && idx >= 0) {
            g_cur_res_file = idx;
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(idx));
            m68k_write_memory_16(CUR_MAP_ADDR, (unsigned)ref);
        }
        g_res_err = ref ? 0 : (short)(-43);
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 12, (unsigned)(ref ? ref : 1), 2);
        return 1;
    }
    case 0xA9C4: {                       /* _OpenRFPerm -> short refNum      */
        /* OpenRFPerm(ConstStr255Param fileName, short vRefNum,
         *            SignedByte permission)
         *
         * Eight bytes of arguments -- the byte permission still costs two,
         * because `move.b <ea>,-(a7)` keeps A7 even -- so the name is the long
         * at csp+4, and the word result is kept as a refNum.
         *
         * A REAL open: MacinTalk Pro does not stop at the Resource Manager, it
         * walks the map and seeks to byte offsets in the fork, so the refNum
         * it gets back has to name a stream it can actually read. */
        int idx = no_files() ? -1 : file_by_name(m68k_read_memory_32(csp + 4));
        int ref = no_files() ? 1 : file_open(idx, FORK_RSRC);
        /* The Resource Manager makes a newly opened file the current one, and
         * that is what scopes every Get1* that follows. */
        if (ref && idx >= 0) {
            g_cur_res_file = idx;
            /* The Resource Manager points TopMapHndl at the file just opened,
             * and Pro reads it directly rather than asking. */
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(idx));
            m68k_write_memory_16(CUR_MAP_ADDR, (unsigned)ref);
        }
        g_res_err = ref ? 0 : (short)(-43);              /* fnfErr */
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 8, (unsigned)(ref ? ref : -1), 2);
        return 1;
    }
    case 0xA99A: {                       /* _CloseResFile(short)             */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(csp));
        if (f) {
            if (g_cur_res_file == f->file) g_cur_res_file = -1;
            f->used = 0;
        }
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    }
    case 0xA9AF:                         /* _ResError -> short               */
        /* Only ever reached on a failure path -- MacinTalk 2 calls it to turn
         * "the Handle came back nil" into an error code to return. */
        tb_return(exc_sp, 0, (unsigned)(unsigned short)g_res_err, 2);
        return 1;
    case 0xA975: {                       /* _TickCount -> long               */
        /* Only ever feeds `TickCount + 60` timeouts.  It must advance, or a
         * wait loop that checks it becomes infinite; it must not advance fast,
         * or a legitimate wait times out.  One tick per call is both. */
        g_ticks++;
        /* Only mirror into low memory when an engine has asked for a running
         * clock. `.sp` predates any of this and its own data lives across low
         * memory in our layout: writing $016A behind its back changed what it
         * rendered, same length, different samples. */
        if (g_tick_auto) m68k_write_memory_32(TICKS_ADDR, g_ticks);
        tb_return(exc_sp, 0, g_ticks, 4);
        return 1;
    }
    default:
        return 0;
    }
}
