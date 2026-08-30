/* ------------------------------------------- Fixed point and extended 80 -
 *
 * MacinTalk Pro does its synthesis arithmetic in Apple's numeric types, and
 * calls the Toolbox to do it: FixMul and FixDiv by the hundred, and
 * conversions to and from the 80-bit extended format SANE works in.
 *
 * `Fixed` is signed 16.16, `Fract` is signed 2.30.  An extended is ten bytes:
 * a sign bit, fifteen exponent bits biased by 16383, and a 64-bit mantissa
 * **with an explicit integer bit** -- unlike IEEE double, nothing is implied.
 * We already write this layout into AIFF sample rates; this reads it too.
 */
static double ext80_read(unsigned addr)
{
    unsigned hi = m68k_read_memory_16(addr);
    unsigned m0 = m68k_read_memory_32(addr + 2);
    unsigned m1 = m68k_read_memory_32(addr + 6);
    double mant = (double)m0 * 4294967296.0 + (double)m1;
    int exp = (int)(hi & 0x7FFFu);
    double v;
    if (exp == 0 && mant == 0.0) return 0.0;
    v = ldexp(mant, exp - 16383 - 63);
    return (hi & 0x8000u) ? -v : v;
}

static void ext80_write(unsigned addr, double v)
{
    int sign = 0, e2;
    double m;
    unsigned hi, m0, m1;
    unsigned long long mant;
    if (v < 0.0) { sign = 1; v = -v; }
    if (!(v > 0.0)) {                        /* zero, and NaN lands here too */
        m68k_write_memory_16(addr, (unsigned)(sign << 15));
        m68k_write_memory_32(addr + 2, 0);
        m68k_write_memory_32(addr + 6, 0);
        return;
    }
    m = frexp(v, &e2);                       /* v = m * 2^e2, 0.5 <= m < 1   */
    mant = (unsigned long long)ldexp(m, 64); /* integer bit lands in bit 63  */
    hi = (unsigned)((sign << 15) | ((unsigned)(e2 - 1 + 16383) & 0x7FFFu));
    m0 = (unsigned)(mant >> 32);
    m1 = (unsigned)(mant & 0xFFFFFFFFu);
    m68k_write_memory_16(addr, hi);
    m68k_write_memory_32(addr + 2, m0);
    m68k_write_memory_32(addr + 6, m1);
}

static int fx_signed(unsigned v)
{
    return (v & 0x80000000u) ? (int)(v - 0x100000000ull) : (int)v;
}

/* Round-to-nearest, which is what the Toolbox does. */
static unsigned fx_mul(unsigned a, unsigned b)
{
    long long r = ((long long)fx_signed(a) * (long long)fx_signed(b));
    return (unsigned)((r + 0x8000) >> 16);
}

static unsigned fx_div(unsigned a, unsigned b)
{
    long long num = ((long long)fx_signed(a)) << 16;
    int den = fx_signed(b);
    if (den == 0) return (fx_signed(a) < 0) ? 0x80000000u : 0x7FFFFFFFu;
    return (unsigned)(num / den);
}

static void tb_return(unsigned exc_sp, unsigned param_bytes,
                      unsigned result, int result_size);

static unsigned short g_sane_env;

static double sane_read_float(unsigned addr, unsigned fmt)
{
    if (fmt == 0x0000u) {
        return ext80_read(addr);
    } else if (fmt == 0x0800u) {
        unsigned long long bits = ((unsigned long long)m68k_read_memory_32(addr) << 32)
                                | (unsigned long long)m68k_read_memory_32(addr + 4);
        double v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    } else if (fmt == 0x1000u) {
        unsigned bits = m68k_read_memory_32(addr);
        float v;
        memcpy(&v, &bits, sizeof(v));
        return (double)v;
    } else if (fmt == 0x2000u) {
        return (double)(short)m68k_read_memory_16(addr);
    } else if (fmt == 0x2800u) {
        return (double)fx_signed(m68k_read_memory_32(addr));
    } else if (fmt == 0x3000u) {
        unsigned long long bits = ((unsigned long long)m68k_read_memory_32(addr) << 32)
                                | (unsigned long long)m68k_read_memory_32(addr + 4);
        long long signed_bits = (long long)bits;
        return (double)signed_bits;
    }
    return 0.0;
}

static void sane_write_float(unsigned addr, unsigned fmt, double v)
{
    if (fmt == 0x0000u) {
        /* MSVC long double is just IEEE double, so this SANE subset trades the
         * last 11 mantissa bits of extended precision for a working synthesis
         * engine.  The audio path does not need bit-perfect 80-bit arithmetic. */
        ext80_write(addr, v);
    } else if (fmt == 0x0800u) {
        unsigned long long bits;
        memcpy(&bits, &v, sizeof(bits));
        m68k_write_memory_32(addr, (unsigned)(bits >> 32));
        m68k_write_memory_32(addr + 4, (unsigned)bits);
    } else if (fmt == 0x1000u) {
        float f = (float)v;
        unsigned bits;
        memcpy(&bits, &f, sizeof(bits));
        m68k_write_memory_32(addr, bits);
    } else if (fmt == 0x2000u) {
        long r = (long)(v < 0.0 ? v - 0.5 : v + 0.5);
        if (r > 32767L) r = 32767L;
        else if (r < -32768L) r = -32768L;
        m68k_write_memory_16(addr, (unsigned)(unsigned short)r);
    } else if (fmt == 0x2800u) {
        double r = v < 0.0 ? v - 0.5 : v + 0.5;
        if (r > 2147483647.0) r = 2147483647.0;
        else if (r < -2147483648.0) r = -2147483648.0;
        m68k_write_memory_32(addr, (unsigned)(long)r);
    } else if (fmt == 0x3000u) {
        double r = v < 0.0 ? v - 0.5 : v + 0.5;
        unsigned long long bits = (unsigned long long)(long long)r;
        m68k_write_memory_32(addr, (unsigned)(bits >> 32));
        m68k_write_memory_32(addr + 4, (unsigned)bits);
    }
}

static void sane_set_relation(unsigned exc_sp, double dst, double src)
{
    unsigned sr = m68k_read_memory_16(exc_sp);
    sr &= ~0x001Fu;                         /* X N Z V C */
    if (dst < src) sr |= 0x08u | 0x01u;     /* N and C */
    else if (dst == src) sr |= 0x04u;       /* Z */
    m68k_write_memory_16(exc_sp, sr);
}

static int sane_fail(unsigned short trap, unsigned short opword, unsigned pc)
{
    fprintf(stderr, "unimplemented SANE trap $%04X opword $%04X at 0x%X\n",
            trap, opword, pc);
    g_stop_reason = STOP_EXCEPTION;
    g_stop_vector = 10;
    g_stop_pc = pc;
    m68k_end_timeslice();
    return 1;
}

static int sane_return(unsigned exc_sp, unsigned param_bytes)
{
    tb_return(exc_sp, param_bytes, 0, 0);
    return 1;
}

static int serve_sane_trap(unsigned short trap, unsigned exc_sp,
                           unsigned csp, unsigned resume_pc)
{
    unsigned short opword = (unsigned short)m68k_read_memory_16(csp);
    unsigned fmt = (unsigned)(opword & 0x7800u);
    unsigned op = (unsigned)(opword & 0x07FFu);
    unsigned dst = m68k_read_memory_32(csp + 2);
    unsigned src = m68k_read_memory_32(csp + 6);
    double d, s, r;

    if (trap == 0xA9ECu) {                  /* Pack5 / Elems68K */
        if (opword == 0x8010u || opword == 0x8012u) {
            d = ext80_read(dst);
            s = ext80_read(src);
            ext80_write(dst, pow(d, s));
            return sane_return(exc_sp, 10);
        }
        if (fmt != 0x0000u) return sane_fail(trap, opword, resume_pc - 2u);
        d = ext80_read(dst);
        switch (op) {
        case 0x0000u: r = log(d); break;           /* FLNX    */
        case 0x0002u: r = log(d) / log(2.0); break;/* FLOG2X  */
        case 0x0004u: r = log1p(d); break;         /* FLN1X   */
        case 0x0006u: r = log1p(d) / log(2.0); break;
        case 0x0008u: r = exp(d); break;           /* FEXPX   */
        case 0x000Au: r = pow(2.0, d); break;      /* FEXP2X  */
        case 0x000Cu: r = exp(d) - 1.0; break;     /* FEXP1X  */
        case 0x000Eu: r = pow(2.0, d) - 1.0; break;
        case 0x0018u: r = sin(d); break;
        case 0x001Au: r = cos(d); break;
        case 0x001Cu: r = tan(d); break;
        case 0x001Eu: r = atan(d); break;
        default: return sane_fail(trap, opword, resume_pc - 2u);
        }
        ext80_write(dst, r);
        return sane_return(exc_sp, 6);
    }

    if (trap != 0xA9EBu) return 0;

    switch (op) {
    case 0x0000u: case 0x0002u: case 0x0004u: case 0x0006u:
    case 0x000Cu: case 0x0018u: {
        d = ext80_read(dst);
        s = sane_read_float(src, fmt);
        if (op == 0x0000u) r = d + s;
        else if (op == 0x0002u) r = d - s;
        else if (op == 0x0004u) r = d * s;
        else if (op == 0x0006u) r = d / s;
        else if (op == 0x000Cu) r = fmod(d, s);
        else r = ldexp(d, (int)s);
        ext80_write(dst, r);
        return sane_return(exc_sp, 10);
    }
    case 0x0008u: case 0x000Au:
        sane_set_relation(exc_sp, ext80_read(dst), sane_read_float(src, fmt));
        return sane_return(exc_sp, 10);
    case 0x000Eu:
        ext80_write(dst, sane_read_float(src, fmt));
        return sane_return(exc_sp, 10);
    case 0x0010u:
        sane_write_float(dst, fmt, ext80_read(src));
        return sane_return(exc_sp, 10);
    case 0x0012u:
        ext80_write(dst, sqrt(ext80_read(dst)));
        return sane_return(exc_sp, 6);
    case 0x0014u:
        ext80_write(dst, floor(ext80_read(dst) + 0.5));
        return sane_return(exc_sp, 6);
    case 0x0016u:
        d = ext80_read(dst);
        ext80_write(dst, d < 0.0 ? ceil(d) : floor(d));
        return sane_return(exc_sp, 6);
    case 0x001Au:
        ext80_write(dst, floor(log(fabs(ext80_read(dst))) / log(2.0)));
        return sane_return(exc_sp, 6);
    case 0x001Cu:
        m68k_write_memory_16(dst, ext80_read(src) == 0.0 ? 4u : 5u);
        return sane_return(exc_sp, 10);
    case 0x000Du:
        ext80_write(dst, -ext80_read(dst));
        return sane_return(exc_sp, 6);
    case 0x000Fu:
        ext80_write(dst, fabs(ext80_read(dst)));
        return sane_return(exc_sp, 6);
    case 0x0001u:                         /* FSETENV */
        g_sane_env = m68k_read_memory_16(dst);
        return sane_return(exc_sp, 6);
    case 0x0003u:                         /* FGETENV */
        m68k_write_memory_16(dst, g_sane_env);
        return sane_return(exc_sp, 6);
    case 0x0015u:                         /* exception flags, in or out    */
        /* One pointer operand and six bytes to pop -- read off the call site
         * rather than off a table, because a wrong pop size corrupts the
         * frame of an engine that is otherwise working:
         *
         *     clr.w   -$c(a6)        zero a 16-bit local
         *     pea     -$c(a6)        push its address        (4)
         *     move.w  #$0015,-(a7)   push the opword         (2)
         *     _FP68K
         *     bra     ...            -> unlk a6 / rts
         *
         * Whether the word travels in or out cannot be told apart from one
         * call site that clears it first and never reads it after, so it
         * travels both ways: zero goes back, and whatever came in is taken.
         * This host raises no SANE exceptions, so "none pending" is the true
         * answer to a read, and consuming the operand is the right answer to
         * a write.
         *
         * MacinTalk Pro reaches this only above roughly 'pbas' 69, and only
         * on longer text -- which is why wiring up a pitch slider found it
         * and every utterance before now did not. Unserved, it halted the
         * engine outright: `sane_fail` takes vector 10, so the symptom was
         * "unhandled exception" partway through a sentence.
         */
        m68k_write_memory_16(dst, 0);
        return sane_return(exc_sp, 6);
    case 0x0017u:                         /* FPROCENTRY */
        m68k_write_memory_16(dst, g_sane_env);
        g_sane_env = 0;
        return sane_return(exc_sp, 6);
    case 0x0019u:                         /* FPROCEXIT */
        g_sane_env = m68k_read_memory_16(dst);
        return sane_return(exc_sp, 6);
    default:
        return sane_fail(trap, opword, resume_pc - 2u);
    }
}

/* Is this one of the File Manager traps this host serves?
 *
 * The list matters because bit 10 only means "asynchronous" for the File
 * Manager.  On the Memory Manager it is the clear/sys flag and on
 * `_GetTrapAddress` it selects the trap table, and MacinTalk Pro sets it on
 * all three -- $A722, $A51E and $A746 all appear in one utterance.  Treating
 * those as async would call a completion routine that was never asked for. */
static int is_file_trap(unsigned base)
{
    switch (base) {
    case 0xA000u: case 0xA001u: case 0xA002u: case 0xA003u:   /* Open..Write */
    case 0xA008u: case 0xA009u: case 0xA00Au:      /* Create, Delete, OpenRF */
    case 0xA011u: case 0xA012u: case 0xA013u:      /* GetEOF, SetEOF, Flush  */
    case 0xA014u: case 0xA015u: case 0xA017u:      /* GetVol, SetVol, Eject  */
    case 0xA018u: case 0xA044u: case 0xA060u:      /* GetFPos, SetFPos, HFS  */
        return 1;
    default:
        return 0;
    }
}

/* Take the completion routine off an asynchronous File Manager call.
 *
 * The request itself has already been served by the time this runs, so
 * ioResult and ioActCount are correct; what is left is the callback, which is
 * the only way the caller can learn any of that happened.  It is queued rather
 * than called: running it here would re-enter the CPU mid-instruction.
 *
 * The timeslice ends so it runs promptly.  A real File Manager would complete
 * at interrupt time, but "promptly" is the safer end of that range -- the
 * caller tests ioResult on the very next instruction, and the scheduler pass
 * this belongs to can be over in a few thousand more. */
static void queue_io_completion(unsigned short word, unsigned a0, unsigned d0)
{
    unsigned proc;
    if (word & 0x0800u) return;              /* Toolbox: bit 10 is auto-pop */
    if (!(word & 0x0400u)) return;           /* not asynchronous            */
    if (!is_file_trap(word & 0xF9FFu)) return;
    proc = m68k_read_memory_32(a0 + 12);     /* ioCompletion                */
    if (!proc) return;
    if (g_ioc_n >= IOC_CAP) { g_ioc_dropped++; return; }
    g_ioc[g_ioc_n].proc = proc;
    g_ioc[g_ioc_n].pb = a0;
    g_ioc[g_ioc_n].result = d0;
    g_ioc_n++;
    m68k_end_timeslice();
}

/* Returns 1 if we served the trap. */
static int serve_memory_trap(unsigned short word, unsigned *d0_out, unsigned *a0_out)
{
    unsigned base = (word & 0x0800u) ? (word & 0xF8FFu) : (word & 0xF9FFu);
    unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
    unsigned a0 = m68k_get_reg(NULL, M68K_REG_A0);

    /* _CmpString and its MARKS/CASE variants ($A03C, $A23C, $A43C, $A63C).
     * A0 and A1 already point past the Pascal length bytes; D0 carries len1 in
     * the high word and len2 in the low.  Returns 0 for equal.
     *
     * This must be real, not stubbed.  Returning "equal" sends DriverOpen down
     * a path that stores a NULL where the TALK handle belongs and never calls
     * _GetResource at all -- a clean return that skipped the only thing Open
     * exists to do. */
    if ((word & 0xF0FFu) == 0xA03Cu) {
        unsigned a1 = m68k_get_reg(NULL, M68K_REG_A1);
        unsigned len1 = (d0 >> 16) & 0xFFFFu, len2 = d0 & 0xFFFFu;
        int ignore_case = (word & 0x0400u) ? 1 : 0;
        unsigned i;
        unsigned res = 1;
        if (len1 == len2) {
            res = 0;
            for (i = 0; i < len1; i++) {
                int c1 = fold(m68k_read_memory_8(a0 + i), ignore_case);
                int c2 = fold(m68k_read_memory_8(a1 + i), ignore_case);
                if (c1 != c2) { res = 1; break; }
            }
        }
        *d0_out = res; *a0_out = a0;
        return 1;
    }

    switch (base) {
    case 0xA11E: {                     /* _NewPtr  -- size in D0, ptr in A0 */
        /* Bit 9 is the Clear flag: _NewPtr leaves the block uninitialized,
         * _NewPtrClear zeros it.  See heap_alloc's g_alloc_dirty note. */
        unsigned p;
        g_alloc_dirty = !(word & 0x0200u);
        p = heap_alloc(d0);
        g_alloc_dirty = 0;
        *a0_out = p; *d0_out = p ? 0 : (unsigned)(-108) /* memFullErr */;
        m68k_write_memory_16(MEM_ERR_ADDR, p ? 0u : 0xFF94u);
        return 1;
    }
    case 0xA122: {                     /* _NewHandle -- size in D0, hdl A0 */
        unsigned h;
        g_alloc_dirty = !(word & 0x0200u);   /* _NewHandleClear zeros; this may not */
        h = heap_new_handle(d0);
        g_alloc_dirty = 0;
        *a0_out = h; *d0_out = h ? 0 : (unsigned)(-108);
        m68k_write_memory_16(MEM_ERR_ADDR, h ? 0u : 0xFF94u);
        return 1;
    }
    /* Every one of these answers with the single zone.  A client that wants
     * to know where a block lives, or which zone is current, or where the
     * system and application zones are, gets the same truthful answer: there
     * is one heap here.  MacinTalk Pro's Open compares two of them. */
    case 0xA11A:                       /* _GetZone         -> THz in A0     */
    case 0xA126:                       /* _HandleZone(h)   -> THz in A0     */
    case 0xA148:                       /* _PtrZone(p)      -> THz in A0     */
    case 0xA02C:                       /* _ApplicationZone -> THz in A0     */
        *a0_out = MAGIC_ZONE; *d0_out = 0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA01B:                       /* _SetZone(THz) -- there is only one */
        *a0_out = a0; *d0_out = 0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    /* ---- the File Manager, for one file ------------------------------- *
     *
     * All of these take a ParamBlockRec in A0 and return the error in D0, and
     * the HFS spellings are the flat ones with bit 9 set: $A20A is _HOpenRF,
     * $A214 is _HGetVol, $A215 is _HSetVol.  `base` has already masked that
     * off, so one case serves both.
     *
     * IOParam offsets used below: ioResult 16, ioNamePtr 18, ioVRefNum 22,
     * ioRefNum 24, ioMisc 28, ioBuffer 32, ioReqCount 36, ioActCount 40,
     * ioPosMode 44, ioPosOffset 46. */
    case 0xA000:                       /* _Open   -- the DATA fork          */
    case 0xA00A: {                     /* _OpenRF -- the RESOURCE fork      */
        /* Which fork is the whole point. Pro's lexicon is in its own data
         * fork; a voice's 800 KB of units is in that voice file's resource
         * fork, which it reads by walking the map and seeking. Serving one
         * where the other was asked for is silent corruption, not an error. */
        int idx, ref;
        if (no_files()) {                                  /* nominal open */
            m68k_write_memory_16(a0 + 24, (unsigned)FIRST_REFNUM);
            m68k_write_memory_16(a0 + 16, 0);
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        idx = file_by_name(m68k_read_memory_32(a0 + 18));
        ref = file_open(idx, base == 0xA00Au ? FORK_RSRC : FORK_DATA);
        if (!ref) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);        /* fnfErr, -43 */
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        m68k_write_memory_16(a0 + 24, (unsigned)ref);      /* ioRefNum    */
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA001: {                     /* _Close                            */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        if (f) f->used = 0;
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA002: {                     /* _Read                             */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        unsigned buf = m68k_read_memory_32(a0 + 32);
        int req  = (int)m68k_read_memory_32(a0 + 36);
        int mode = (int)(short)m68k_read_memory_16(a0 + 44);
        int off  = (int)m68k_read_memory_32(a0 + 46);
        const unsigned char *bytes;
        int len, pos, got, i, err = 0;
        if (!f) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);        /* fnfErr, -43 */
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        bytes = g_files[f->file].fork[f->fork];
        len = g_files[f->file].len[f->fork];
        if (!bytes) {
            m68k_write_memory_16(a0 + 16, 0xFFDCu);        /* ioErr, -36  */
            *d0_out = (unsigned)(-36); *a0_out = a0;
            return 1;
        }
        switch (mode & 3) {
            case 1:  pos = off; break;                     /* fsFromStart */
            case 2:  pos = len + off; break;               /* fsFromLEOF  */
            case 3:  pos = f->pos + off; break;            /* fsFromMark  */
            default: pos = f->pos; break;                  /* fsAtMark    */
        }
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        got = req < 0 ? 0 : req;
        if (got > len - pos) { got = len - pos; err = -39; }
        for (i = 0; i < got; i++)
            m68k_write_memory_8(buf + (unsigned)i, bytes[pos + i]);
        f->pos = pos + got;
        m68k_write_memory_32(a0 + 40, (unsigned)got);      /* ioActCount  */
        m68k_write_memory_16(a0 + 16, (unsigned)(unsigned short)err);
        *d0_out = (unsigned)err;                           /* eofErr, -39 */
        *a0_out = a0;
        return 1;
    }
    case 0xA044: {                     /* _SetFPos                          */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        int mode = (int)(short)m68k_read_memory_16(a0 + 44);
        int off  = (int)m68k_read_memory_32(a0 + 46);
        int len, pos;
        if (!f) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        len = g_files[f->file].len[f->fork];
        switch (mode & 3) {
            case 1:  pos = off; break;
            case 2:  pos = len + off; break;
            case 3:  pos = f->pos + off; break;
            default: pos = f->pos; break;
        }
        if (pos < 0) pos = 0;
        /* Seeking past the end is posErr, and saying so matters: the caller
         * that gets noErr for a bad seek reads the wrong bytes instead. */
        if (pos > len) {
            f->pos = len;
            m68k_write_memory_16(a0 + 16, 0xFFC0u);        /* posErr, -64 */
            *d0_out = (unsigned)(-64); *a0_out = a0;
            return 1;
        }
        f->pos = pos;
        m68k_write_memory_32(a0 + 46, (unsigned)pos);
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA018: {                     /* _GetFPos                          */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        m68k_write_memory_32(a0 + 36, 0);
        m68k_write_memory_32(a0 + 46, (unsigned)(f ? f->pos : 0));
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA011: {                     /* _GetEOF -- length in ioMisc       */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        int len = f ? g_files[f->file].len[f->fork] : 0;
        m68k_write_memory_32(a0 + 28, (unsigned)len);
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA014:                       /* _GetVol / _HGetVol                */
        file_put_name(0, m68k_read_memory_32(a0 + 18));
        m68k_write_memory_16(a0 + 22, FAKE_VREFNUM);
        m68k_write_memory_32(a0 + 48, FAKE_DIRID);         /* ioWDDirID     */
        m68k_write_memory_16(a0 + 16, 0);
    case 0xA015:                       /* _SetVol / _HSetVol -- only one    */
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA060:                       /* _HFSDispatch -- selector in D0    */
        /* Selector 8 is PBGetFCBInfo, and the param block proves it rather
         * than a table doing: MacinTalk Pro fills ioCompletion at +12,
         * ioNamePtr at +18, ioVRefNum at +22, ioRefNum at +24 and ioFCBIndx
         * at +28, then reads ioFCBVRefNum back from +52 and ioFCBParID from
         * +58 -- which is FCBPBRec, field for field.
         *
         * "Which file is this open refNum?" has one answer here. */
        if (d0 == 7u) {
            /* PBGetWDInfo(WDPBRec): ioWDVRefNum at +32 and ioWDDirID at +48,
             * which is where _GetVol above already puts them.  Pro asks for
             * this between setting the volume and opening a file, and stubbed
             * it concluded the file was not there -- resFNotFound, -193. */
            m68k_write_memory_16(a0 + 16, 0);            /* ioResult        */
            file_put_name(0, m68k_read_memory_32(a0 + 18)); /* ioNamePtr    */
            m68k_write_memory_16(a0 + 32, FAKE_VREFNUM); /* ioWDVRefNum     */
            m68k_write_memory_32(a0 + 48, FAKE_DIRID);   /* ioWDDirID       */
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        if (d0 == 8u) {
            m68k_write_memory_16(a0 + 16, 0);            /* ioResult        */
            /* Whichever file that refNum has open, or the engine's own
             * when the refNum is the flat table's. */
            {
                OpenFile *f = file_by_ref(
                    (int)(short)m68k_read_memory_16(a0 + 24));
                int idx = f ? f->file : 0;
                int len = f ? g_files[idx].len[f->fork]
                            : (g_file_count ? g_files[0].len[FORK_DATA] : 0);
                file_put_name(idx, m68k_read_memory_32(a0 + 18));
                m68k_write_memory_32(a0 + 40, (unsigned)len); /* ioFCBEOF  */
                m68k_write_memory_32(a0 + 44, (unsigned)len); /* ioFCBPLen */
            }
            m68k_write_memory_16(a0 + 52, FAKE_VREFNUM); /* ioFCBVRefNum    */
            m68k_write_memory_32(a0 + 58, FAKE_DIRID);   /* ioFCBParID      */
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        return 0;                      /* anything else is news; let it stub */
    case 0xA198:                       /* thin wrapper, result in D0        */
        /* gtse 1 +$82CE is `movea.l (a7)+,a1 / moveq #1,d0 / A198 / jmp (a1)`
         * -- no stack arguments, the result is D0, and it sits among the file
         * wrappers at +$82xx, called from the module loader at +$5E42 and
         * +$6286.  Stubbed it left D0 as the 1 that was moved in, and no Mac
         * OSErr is ever 1, so the loader read that as a failure.
         *
         * Answered as noErr, which is provisional: the trap is not identified
         * from a call site the way the rest here are, only its shape and its
         * neighbours.  If MacinTalk Pro ever behaves oddly around loading a
         * module, doubt this first. */
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA01C:                       /* _FreeMem -> free bytes in D0      */
        /* One contiguous free block, so free and largest-free are the same
         * number here. gtse 1 +$2E92 asks before deciding how much to keep. */
        *d0_out = (g_heap_end > g_heap_next) ? (g_heap_end - g_heap_next) : 0;
        *a0_out = a0;
        return 1;
    case 0xA025:                       /* _GetHandleSize(h) -> long in D0   */
        /* Answerable exactly now that every Handle records its size. Pro asks
         * fifteen times per utterance and sizes buffers from the answer. */
        *d0_out = handle_size(a0);
        *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA069:                       /* _HGetState(h) -> flags in D0      */
        /* Nothing here is locked, purgeable or a resource in the Memory
         * Manager's sense -- our blocks never move -- so the honest answer is
         * no flags set. `$A06A` used to be served as _SystemZone, which was
         * wrong: it is _HSetState, the other half of this pair, and MacinTalk
         * Pro calls them around anything it wants to hold still. */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA06A:                       /* _HSetState(h, flags)              */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA04C:                       /* _CompactMem(size) -> largest free */
        /* A bump allocator has one free block and it is contiguous, so the
         * largest it could give out is everything left. */
        *d0_out = (g_heap_end > g_heap_next) ? (g_heap_end - g_heap_next) : 0;
        *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA1AD:                       /* _Gestalt -- selector D0, resp A0 */
        /* Reached through the TrapAvailable() pattern at Cecy 1 +$58A4, and
         * the engine carries its own answer table for the case where Gestalt
         * is missing.  We report it present -- the selector it asks for is in
         * the trap log's D0-in column, and the answer below is chosen from
         * that rather than from what a Mac would have said.
         *
         * Zero for almost everything, which reads as "the feature is there and
         * it is off", and is what MacinTalk 2 has always been told.  Exactly
         * two selectors are answered, because exactly two are gates:
         *
         * MacinTalk Pro's Open builds a requirements record at gtse 1 +$1044
         * and tests it at +$282.  `sysv` below $0700 fails, and `proc` equal
         * to 1 or 2 -- a 68000 or a 68010 -- fails.  Either way it writes
         * #$ff0f into the error and disposes its own storage:
         * **synthOpenFailed, -241, before it reads a single resource.**
         *
         * So Pro genuinely requires a 68020. `proc` reports whatever CPU is
         * actually configured rather than a flattering constant -- claiming an
         * 020 while running the 68000 core would invite 020-only instructions
         * into an emulator that cannot execute them. */
        if (d0 == 0x73797376u)         /* 'sysv' */
            *a0_out = 0x0700u;         /* System 7, as _SysEnvirons says too */
        else if (d0 == 0x70726F63u)    /* 'proc' */
            *a0_out = g_gestalt_proc;
        else if (d0 == 0x66707520u)    /* 'fpu ' */
            /* 3 is the 68040's built-in FPU. The modules MacinTalk Pro loads
             * for synthesis contain F-line coprocessor instructions, so this
             * has to agree with the CPU the same way 'proc' does -- and on a
             * 68040 the answer is yes. */
            *a0_out = (g_gestalt_proc >= 5) ? 3u : 0u;
        else
            *a0_out = 0;
        *d0_out = 0;                   /* noErr */
        return 1;
    case 0xA090: {                     /* _SysEnvirons(version, SysEnvRec*) */
        /* Cecy 1 +$100A asks for version 2 and compares systemVersion with
         * $0700 to decide whether it is on System 7.  These resources came
         * off a System 7 disk, so saying so is the truthful answer. */
        unsigned rec = a0;
        m68k_write_memory_16(rec + 0, 2);        /* environsVersion         */
        m68k_write_memory_16(rec + 2, 9);        /* machineType             */
        m68k_write_memory_16(rec + 4, 0x0700);   /* systemVersion, System 7 */
        m68k_write_memory_16(rec + 6, 1);        /* processor: env68000     */
        m68k_write_memory_8 (rec + 8, 0);        /* hasFPU                  */
        m68k_write_memory_8 (rec + 9, 0);        /* hasColorQD              */
        m68k_write_memory_16(rec + 10, 0);       /* keyBoardType            */
        m68k_write_memory_16(rec + 12, 0);       /* atDrvrVersNum           */
        m68k_write_memory_16(rec + 14, 0);       /* sysVRefNum              */
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    /* The Time Manager.  MacinTalk 2's back end installs eight tasks at
     * Cecy 1 +$38F6, over 26-byte records whose filled fields -- long at +0,
     * word at +4, long at +$A, word at +$E -- are TMTask's qLink, qType,
     * tmCount and tmWakeUp.
     *
     * Accepting and ignoring them is right for this host rather than merely
     * convenient: we take every sound buffer the instant it is offered and
     * fire the callback immediately, so nothing here ever waits on a clock.
     * A timer that never fires cannot stall what never waits. */
    case 0xA082: {                     /* _DTInstall -- A0 = DeferredTask  */
        /* Named from the record, not from recall.  A snapshot at the call
         * site (Cecy 1 +$3BA2) reads:
         *
         *   +$00 qLink    00000000
         *   +$04 qType    0007         <- dtQType
         *   +$06 dtFlags  0000
         *   +$08 dtAddr   00063A5A     <- back end +$3A5A
         *   +$0C dtParam  00093BE8     <- the back end's storage
         *   +$10 dtReserved 00000000
         *
         * which is DeferredTask field for field.  The task is entered with
         * A1 = dtParam, and +$3A62 confirms it: `move.l a1,d0 / movea.l d0,a4`
         * with a4 used as storage from there on.
         *
         * Running it here would be re-entering the CPU mid-instruction, so
         * take the record now and run it once the machine is between calls --
         * the same shape the sound callback already uses. */
        g_dt_proc = m68k_read_memory_32(a0 + 8);
        g_dt_parm = m68k_read_memory_32(a0 + 12);
        g_dt_pending = 1;
        /* End the timeslice so it runs PROMPTLY.
         *
         * A deferred task fires at the end of interrupt processing on a real
         * Macintosh -- microseconds, not "whenever the caller next yields".
         * Setting the flag and waiting for the natural end of a 100,000
         * instruction slice is usually close enough, but it drops the task
         * entirely when the call reaches its sentinel first: `osp_call` only
         * runs pending work while the reason is still STOP_RUNNING.
         *
         * MacinTalk 3 is where that bites. It installs one task during
         * SpeakBuffer, the speak returns before the slice ends, and the task
         * runs on a later pump -- by which time the engine has disposed of the
         * record the task walks into, guard word `0x12345678` and all. The
         * result is a jump through freed memory, forty million faults, and it
         * looks exactly like an engine bug. */
        m68k_end_timeslice();
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA058:                       /* _InsTime                         */
    case 0xA059:                       /* _RmvTime                         */
    case 0xA05A:                       /* _PrimeTime                       */
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA193: {                     /* _Microseconds                     */
        /* **It returns the count in A0 and D0, and writes no memory.**
         *
         * This host used to treat A0 as a `UnsignedWide *` and store the
         * result through it, which is what the C prototype
         * `Microseconds(UnsignedWide *)` suggests -- and it is wrong. The
         * pointer belongs to Apple's *glue*, not to the trap. MacinTalk 3
         * carries that glue verbatim, and it says so plainly:
         *
         *     pea      $f6(a4)        push the destination
         *     _Microseconds
         *     movea.l  (a7)+,a1       pop it back -- the trap did not take it
         *     move.l   a0,(a1)+       high word from A0
         *     move.l   d0,(a1)        low  word from D0
         *
         * A0 on entry is whatever the caller happened to leave there, so
         * storing through it scribbles eight bytes into live data. In
         * MacinTalk 3 it landed exactly on the engine's own SndCommand and
         * rewrote its `param2`; the sound callback then walked that pointer
         * into freed memory and jumped through it, four hundred million bus
         * faults later. The clock was corrupting the thing it was timing.
         *
         * The count must also never go backwards. `g_instr_count` is reset at
         * every component call and every callback round, so it is a duration
         * rather than a clock; `g_instr_total` only ever climbs. An engine
         * subtracting two samples of a clock that restarts sees a huge
         * negative elapsed time, which is its own class of bug and not one
         * worth waiting to be bitten by.
         */
        unsigned long long us = (unsigned long long)g_instr_total;
        *a0_out = (unsigned)(us >> 32);
        *d0_out = (unsigned)(us & 0xFFFFFFFFu);
        return 1;
    }
    case 0xA146:                       /* _GetTrapAddress -- number in D0  */
        *a0_out = TRAPADDR_BASE + ((d0 & 0x0FFFu) * 4u);
        *d0_out = 0;
        return 1;
    case 0xA029:                       /* _HLock   -- nothing moves here   */
    case 0xA02A:                       /* _HUnlock                         */
    case 0xA023:                       /* _DisposeHandle                   */
    case 0xA01F:                       /* _DisposePtr                      */
    case 0xA049:                       /* _HPurge                          */
    case 0xA04A:                       /* _HNoPurge                        */
    case 0xA036:                       /* _MoreMasters                     */
    /* Both of these only matter to a real, compacting heap.  Serving them
     * explicitly rather than letting them stub keeps g_stub_count meaning
     * "something we have not implemented", which is the alarm it exists to
     * be. */
    case 0xA040:                       /* _ResrvMem                        */
    case 0xA064:                       /* _MoveHHi                         */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA055:                       /* _StripAddress -- 24-bit clean    */
        *d0_out = d0 & 0x00FFFFFFu; *a0_out = a0;
        return 1;
    case 0xA02E: {                     /* _BlockMove -- A0 src A1 dst D0 n */
        unsigned src = a0, dst = m68k_get_reg(NULL, M68K_REG_A1), n = d0, i;
        for (i = 0; i < n; i++)
            m68k_write_memory_8(dst + i, m68k_read_memory_8(src + i));
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    default:
        return 0;
    }
}
