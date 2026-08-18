/* --------------------------------------------------- the Component Manager -
 *
 * MacinTalk 2 and MacinTalk Pro are Component Manager components, not DRVRs,
 * so the host plays a second role it did not need for `.sp`: the switchboard
 * that `$A82A` calls into.  See docs/macintalk2-components.md for how the
 * frame layouts below were measured; none of it is recalled.
 *
 * The one idea that makes this cheap: **a component call is a tail call.**  A
 * naive implementation would re-enter the CPU (the way run_pending_callback
 * does) and copy the result back, but that cannot recurse, and the front end
 * calling the back end which calls its own handler is three deep immediately.
 * Instead we rewrite the stack so the callee's Pascal frame lands exactly
 * where the caller's `$A82A` result is expected, give the callee the address
 * just past the trap word as its return address, and let it return straight to
 * the caller.  The host never sees the call end, and never needs to.
 *
 * That works because every one of these leaves SP at the caller's result slot:
 *
 *     D0 = -1  handler frame at csp+4-paramSize; the handler pops its return
 *              address, paramSize bytes of arguments and the storage Handle,
 *              landing on csp+12 -- the slot the glue's caller pushed.
 *     D0 =  0  component main's frame at csp+paramSize-4; main pops 8 bytes
 *              (measured at Cecy 3 +$110: unlk / movea.l (a7)+,a0 / addq #8),
 *              landing on csp+8+paramSize.
 *
 * Both overwrite stack the caller still owns, so every field is read out
 * before anything is written.
 */

#define MAX_COMPONENTS 8
#define MAX_INSTANCES  16

/* A ComponentInstance is an opaque token to the engine, so it can be anything
 * non-zero that we recognise on the way back in.  Keeping them in their own
 * magic page means a stale or invented one is obvious rather than plausible. */
#define INSTANCE_BASE  0x00F2C000u
#define INSTANCE_TOK(i) (INSTANCE_BASE + (unsigned)(i) * 4u)

/* A `Component` and a `ComponentInstance` are different things and the engine
 * holds both at once -- FindNextComponent hands back the former, OpenComponent
 * turns it into the latter.  Separate token ranges so mixing them up is caught
 * here rather than becoming a plausible-looking wrong answer. */
#define COMPONENT_BASE 0x00F2C100u
#define COMPONENT_TOK(i) (COMPONENT_BASE + (unsigned)(i) * 4u)

/* Where a `D0 = 0` call's ComponentParameters is copied so the frame we build
 * on top of it cannot clobber the arguments it describes.
 *
 * A ring, not a stack, because tail-calling means the host is never told that
 * a call finished -- slots come back by wrap-around alone.  64 is far past the
 * nesting these two components can reach; g_cp_wraps gives a corruption a
 * signature instead of leaving it to look like an engine bug. */
#define CP_SCRATCH   0x00F22000u
#define CP_SLOTS     64
#define CP_SLOT_SZ   264u          /* 4 + a paramSize that cannot exceed 255 */

/* The host's own top-level call lives for a whole utterance, so it must never
 * come from the ring. */
#define CP_TOPLEVEL  0x00F26400u

typedef struct {
    unsigned type, subtype, manuf;
    unsigned entry;                 /* code resource, already loaded into RAM */
} Component;

typedef struct {
    int      component;             /* index into g_comp; -1 when free */
    unsigned storage;               /* the Handle set through selector $11 */
    unsigned refcon;                /* a long the component parks on itself */
} Instance;

static Component g_comp[MAX_COMPONENTS];
static int       g_comp_count;
static Instance  g_inst[MAX_INSTANCES];
static int       g_inst_count;
static int       g_cp_slot, g_cp_wraps;

/* OpenComponent is the one call a tail call cannot express.
 *
 * Every other selector either answers immediately or hands control to exactly
 * one callee, so the callee can return straight to the original caller.  Open
 * has to do both: run the component's own Open handler, *and then* give the
 * caller an instance rather than whatever that handler returned.  Two returns,
 * one of which is ours.
 *
 * So the handler is sent back to a magic address instead, and the pending
 * record here says what to do when execution reaches it.  A stack rather than
 * a single slot, because a component's Open may open another component --
 * which is exactly what the front end does to the back end. */
#define MAGIC_COPEN_RET 0x00F11100u

#define MAX_PENDING 8
typedef struct {
    unsigned result_slot;   /* the caller's, where the instance belongs */
    unsigned resume_pc;     /* where the caller carries on              */
    unsigned inst_token;    /* handed back if the handler said noErr    */
    unsigned callee_result; /* where the handler left its own result    */
} PendingOpen;
static PendingOpen g_pending[MAX_PENDING];
static int         g_pending_n;
/* The frame arithmetic for each CallComponentFunctionWithStorage.
 *
 * Kept because it earned its place: the SR-clobber above was invisible from
 * the 68000 side -- the trace showed the right instructions in the right order
 * and every register looked plausible -- and one look at csp, paramSize and
 * the computed newsp side by side made it obvious. */
#define MAX_FRAMELOG 16
static unsigned    g_framelog[MAX_FRAMELOG][6];
static int         g_framelog_n;
static int         g_copen_ret;   /* set by instr_hook, acted on outside */

/* Every `$A82A` selector seen, whether or not we knew it.  The first run's log
 * is what settles this engine's surface, exactly as the trap log settled
 * `.sp`'s -- so record the unknown ones with enough stack to decode them. */
#define MAX_CMLOG 512
typedef struct { unsigned d0, pc, csp, w0, w1, w2, w3; int served; } CmRec;
static CmRec g_cmlog[MAX_CMLOG];
static int   g_cmlog_n;

static int comp_of(unsigned token)
{
    unsigned i;
    if (token < COMPONENT_BASE) return -1;
    i = (token - COMPONENT_BASE) / 4u;
    return i < (unsigned)g_comp_count ? (int)i : -1;
}

static int inst_of(unsigned token)
{
    unsigned i;
    if (token < INSTANCE_BASE || token >= COMPONENT_BASE) return -1;
    i = (token - INSTANCE_BASE) / 4u;
    if (i >= (unsigned)g_inst_count || g_inst[i].component < 0) return -1;
    return (int)i;
}

/* Put the exception frame where `rte` will land on `entry` with SP == newsp.
 * The same trick tb_return uses, aimed into a callee instead of past a trap.
 *
 * `sr` is passed in, and must have been read before the caller wrote anything.
 * This is not defensiveness -- it is a bug that already happened.  With twelve
 * bytes of arguments the new frame lands *below* the old one, and the return
 * address written at `newsp` covers `exc_sp` and `exc_sp+1`, which is exactly
 * where the saved SR lives.  Reading SR here, after that write, picked up the
 * low half of a return address instead: supervisor bit clear, so `rte` dropped
 * to user mode, A7 became the (zero) USP, and the callee ran with SP = 0
 * scribbling on low memory.  Every earlier call happened to have four bytes of
 * arguments, which puts the new frame exactly on top of the old one and hides
 * this completely. */
static void enter_callee(unsigned sr, unsigned newsp, unsigned entry)
{
    unsigned frame = newsp - EXC_FRAME;
    m68k_write_memory_16(frame, sr);
    m68k_write_memory_32(frame + 2, entry);
    /* Built by hand rather than by the CPU, so the format word has to be
     * supplied too -- `rte` pops by format, not by CPU type. */
    if (EXC_FRAME > 6u) m68k_write_memory_16(frame + 6, EXC_FORMAT_ALINE);
    m68k_set_reg(M68K_REG_SP, frame);
}

/* Returns 1 if we served it.  `resume_pc` is the address just past the trap
 * word, which doubles as the callee's return address. */
static int serve_component_trap(unsigned d0, unsigned exc_sp, unsigned csp,
                                unsigned resume_pc)
{
    unsigned char tmp[CP_SLOT_SZ];
    /* Read before any case writes: several of them build a frame that overlaps
     * the exception frame this comes from.  See enter_callee. */
    unsigned sr = m68k_read_memory_16(exc_sp);
    /* Snapshot the caller's stack *now*.  Every branch below rewrites it to
     * build the callee's frame, so reading it at the end of this function --
     * which is what the log used to do -- described our own output rather
     * than the engine's input, and reported a 'stat' call as `what=0
     * paramSize=6`. */
    unsigned w0 = m68k_read_memory_32(csp + 0);
    unsigned w1 = m68k_read_memory_32(csp + 4);
    unsigned w2 = m68k_read_memory_32(csp + 8);
    unsigned w3 = m68k_read_memory_32(csp + 12);
    unsigned i;
    int served = 0;

    switch (d0) {

    case 0xFFFFFFFFu: {         /* CallComponentFunctionWithStorage           */
        /* `moveq #$FF,d0` sign-extends, so the selector arrives as -1, not
         * $FF.  Comparing against $FF here would silently miss every call. */
        unsigned handler = w0, params = w1, storage = w2;
        unsigned psize   = m68k_read_memory_8(params + 1);
        unsigned newsp;

        if (psize > 255u) psize = 255u;
        if (g_framelog_n < MAX_FRAMELOG) {
            unsigned *f = g_framelog[g_framelog_n++];
            f[0] = csp;   f[1] = params;         f[2] = psize;
            f[3] = handler; f[4] = csp + 4u - psize; f[5] = exc_sp;
        }
        /* Bounce through C: params may itself live on the stack we are about
         * to rewrite, and a straight copy would then read its own output. */
        for (i = 0; i < psize; i++)
            tmp[i] = (unsigned char)m68k_read_memory_8(params + 4 + i);

        newsp = csp + 4u - psize;
        for (i = 0; i < psize; i++)
            m68k_write_memory_8(newsp + 4u + i, tmp[i]);
        /* Storage is the handler's *first* declared argument, so it sits above
         * every unpacked one -- four handlers across both components agree. */
        m68k_write_memory_32(newsp + 4u + psize, storage);
        m68k_write_memory_32(newsp, resume_pc);
        enter_callee(sr, newsp, handler);
        served = 1;
        break;
    }

    case 0x00000000u: {         /* call another component instance            */
        unsigned hdr   = w0;
        unsigned psize = (hdr >> 16) & 0xFFu;
        unsigned token = m68k_read_memory_32(csp + 4u + psize);
        unsigned cp, newsp;
        int ii = inst_of(token);

        if (ii < 0) break;      /* unknown instance -- fall through and halt */

        for (i = 0; i < 4u + psize; i++)
            tmp[i] = (unsigned char)m68k_read_memory_8(csp + i);
        cp = CP_SCRATCH + (unsigned)(g_cp_slot % CP_SLOTS) * CP_SLOT_SZ;
        if (++g_cp_slot % CP_SLOTS == 0) g_cp_wraps++;
        for (i = 0; i < 4u + psize; i++)
            m68k_write_memory_8(cp + i, tmp[i]);

        newsp = csp + psize - 4u;
        m68k_write_memory_32(newsp + 8u, cp);                  /* params     */
        m68k_write_memory_32(newsp + 4u, g_inst[ii].storage);  /* storage    */
        m68k_write_memory_32(newsp, resume_pc);
        enter_callee(sr, newsp, g_comp[g_inst[ii].component].entry);
        served = 1;
        break;
    }

    case 0x0000000Eu: {         /* one argument, one result; see below        */
        /* Measured at Cecy 3 +$134: the front end asks this before allocating
         * storage and uses the answer only to choose between _ResrvMem then
         * _NewHandle, and a plain _NewHandle -- then records the inverse at
         * storage+$21A.  Both branches allocate the same $21C bytes and both
         * are fine against a bump allocator, so 0 is safe.  storage+$21A is
         * the first knob to try if MacinTalk 2 later misbehaves in a way that
         * smells like a heap assumption. */
        tb_return(exc_sp, 4, 0, 4);
        served = 1;
        break;
    }

    case 0x00000004u: {         /* FindNextComponent(prev, desc) -> Component */
        /* Cecy 3 +$17E2 builds a 20-byte ComponentDescription on its own
         * frame -- type 't2be', subType 0, manufacturer 'mtk2', flags 0 -- and
         * returns -240 (`noSynthFound`, straight out of Apple's Speech.h) when
         * this comes back nil.  A zero field is a wildcard, which is why the
         * subType of 0 still has to match our back end's 't2be'. */
        unsigned desc = m68k_read_memory_32(csp + 0);
        unsigned prev = m68k_read_memory_32(csp + 4);
        unsigned wt = m68k_read_memory_32(desc + 0);
        unsigned ws = m68k_read_memory_32(desc + 4);
        unsigned wm = m68k_read_memory_32(desc + 8);
        unsigned found = 0;
        int k, start = 0, pi = comp_of(prev);

        if (pi >= 0) start = pi + 1;        /* "next", so resume past it */
        for (k = start; k < g_comp_count; k++) {
            if (wt && g_comp[k].type != wt) continue;
            if (ws && g_comp[k].subtype != ws) continue;
            if (wm && g_comp[k].manuf != wm) continue;
            found = COMPONENT_TOK(k);
            break;
        }
        tb_return(exc_sp, 8, found, 4);
        served = 1;
        break;
    }

    case 0x00000007u: {         /* OpenComponent(Component) -> instance       */
        /* Cecy 3 +$183E returns -241 (`synthOpenFailed`) when this comes back
         * nil, so a nil answer here is the front end giving up on the back end
         * entirely.  Creating the instance is the easy half; the component's
         * own Open handler has to run before the caller may use it. */
        unsigned ctok = m68k_read_memory_32(csp + 0);
        unsigned tok, cp, newsp;
        int ci = comp_of(ctok), ii;

        if (ci < 0) break;                        /* unknown Component */
        if (g_inst_count >= MAX_INSTANCES) break;
        if (g_pending_n >= MAX_PENDING) break;

        ii = g_inst_count++;
        g_inst[ii].component = ci;
        g_inst[ii].storage = 0;                   /* the handler will set it */
        g_inst[ii].refcon = 0;
        tok = INSTANCE_TOK(ii);

        cp = CP_SCRATCH + (unsigned)(g_cp_slot % CP_SLOTS) * CP_SLOT_SZ;
        if (++g_cp_slot % CP_SLOTS == 0) g_cp_wraps++;
        m68k_write_memory_8(cp + 0, 0);           /* flags                  */
        m68k_write_memory_8(cp + 1, 4);           /* paramSize              */
        m68k_write_memory_16(cp + 2, 0xFFFFu);    /* what = -1, Open        */
        m68k_write_memory_32(cp + 4, tok);        /* params[0] = self       */

        /* Well clear of the caller's frame; the callee grows downward. */
        newsp = csp - 32u;
        m68k_write_memory_32(newsp + 12, 0);      /* the callee's result    */
        m68k_write_memory_32(newsp + 8, cp);      /* params, first declared */
        m68k_write_memory_32(newsp + 4, 0);       /* storage: nil at open   */
        m68k_write_memory_32(newsp + 0, MAGIC_COPEN_RET);

        {
            PendingOpen *p = &g_pending[g_pending_n++];
            p->result_slot = csp + 4;
            p->resume_pc = resume_pc;
            p->inst_token = tok;
            p->callee_result = newsp + 12;
        }
        enter_callee(sr, newsp, g_comp[ci].entry);
        served = 1;
        break;
    }

    case 0x00000015u: {         /* one argument, a *word* result              */
        /* Cecy 3 +$14FC reserves two bytes, passes self, and treats a result
         * <= 0 as failure; on success it immediately fills three pointer
         * slots from three subroutines.  That is a component opening its own
         * resource file and then reading its tables out of it -- Apple most
         * likely calls this OpenComponentResFile.
         *
         * Named here by what it does rather than by what it is probably
         * called, because the evidence supports the behaviour and only
         * suggests the name.  Our _GetResource searches one flat table and
         * ignores files entirely, so any positive refNum will do; the proof
         * that this reading is right is whether the resource calls that
         * follow ask for ttsr/ttsd/ttss. */
        tb_return(exc_sp, 4, 1, 2);
        served = 1;
        break;
    }

    case 0x00000018u: {         /* the close that matches $15                 */
        /* Cecy 3 +$1578 hands back the same *word* refNum $15 returned, once
         * the three tables are loaded and detached, and ignores the result.
         * CloseComponentResFile in all but the name.  Note the argument is a
         * word, not a long -- two bytes of arguments, not four. */
        tb_return(exc_sp, 2, 0, 2);
        served = 1;
        break;
    }

    case 0x0000000Du: {         /* SetComponentInstanceStorage(self, storage) */
        /* $D and $11 have the same shape -- (self, a long), no result -- and
         * both are called once during each component's Open, so the shape
         * cannot tell them apart.  The engine's own later use does.
         *
         * The front end allocates two blocks: $21C, handed to $11 early, and
         * $322, handed to $D at the very end of Open.  It writes the back-end
         * ComponentInstance into **the $322 block at +$4**, and its handlers
         * then reach that instance as `$4(deref(storage))` -- at Cecy 3 +$171E
         * and +$5F8.  Only the $322 block satisfies that, so $D is the one
         * setting storage and $11 is parking something else.
         *
         * The back end agrees: it gives $11 a `_NewPtr` block and $D a real
         * Handle, and its handlers dereference storage once, which is only
         * meaningful for the Handle. */
        unsigned storage = m68k_read_memory_32(csp + 0);
        int ii = inst_of(m68k_read_memory_32(csp + 4));
        if (ii >= 0) g_inst[ii].storage = storage;
        tb_return(exc_sp, 8, 0, 0);
        served = 1;
        break;
    }

    case 0x0000000Bu: {         /* SetComponentInstanceError(self, OSErr)     */
        /* gtse 1 +$1E0 reaches this only on the way out of a FAILED selector:
         * it tests its error, checks it has storage, then pushes the instance
         * and the error as a word.  Six bytes of arguments, no result.
         *
         * Worth serving for the diagnosis alone -- this is the engine saying
         * out loud which error it is about to return, and osp_instance_error
         * hands it to the probe.  Unserved it halted, which turned "the rate
         * call failed" into "unhandled exception" and hid the reason. */
        g_instance_error = (short)m68k_read_memory_16(csp);
        tb_return(exc_sp, 6, 0, 0);
        served = 1;
        break;
    }

    case 0x00000010u: {         /* GetComponentInstanceStorage(self)          */
        int ii = inst_of(m68k_read_memory_32(csp));
        tb_return(exc_sp, 4, ii < 0 ? 0 : g_inst[ii].storage, 4);
        served = 1;
        break;
    }

    case 0x00000011u: {         /* park a long on the instance -- see $D      */
        /* Two arguments and no result slot: the call site at Cecy 3 +$17E
         * never reserves one, so writing four bytes there would land in the
         * caller's locals. */
        unsigned value = m68k_read_memory_32(csp + 0);
        int ii = inst_of(m68k_read_memory_32(csp + 4));
        if (ii >= 0) g_inst[ii].refcon = value;
        tb_return(exc_sp, 8, 0, 0);
        served = 1;
        break;
    }

    default:
        break;
    }

    if (g_cmlog_n < MAX_CMLOG) {
        CmRec *r = &g_cmlog[g_cmlog_n++];
        r->d0 = d0; r->pc = resume_pc - 2u; r->csp = csp; r->served = served;
        r->w0 = w0; r->w1 = w1; r->w2 = w2; r->w3 = w3;
    }

    /* An unknown selector must never be stubbed.  A Toolbox stub leaves the
     * stack unbalanced and the caller returns into rubbish several
     * instructions later, which is a much harder bug than stopping here with
     * the stack still intact and readable. */
    if (!served) {
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = 10;
        g_stop_pc = resume_pc - 2u;
        m68k_end_timeslice();
    }
    return 1;
}
