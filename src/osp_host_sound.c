/* ------------------------------------------------------- the Sound Manager -
 *
 * The whole audio path, per docs/sound-model.md.  MacinTalk fills a buffer,
 * hands it over with `bufferCmd`, and queues a `callBackCmd` behind it to learn
 * when that buffer has drained.  We take the samples immediately and fire the
 * callback, so the engine runs flat out and never waits on a clock.
 */
#define quietCmd     3
#define flushCmd     4
#define callBackCmd  13
#define bufferCmd    81

#define PCM_CAP       (8u * 1024u * 1024u)
#define MAGIC_CB_RET  0x00F11000u
#define MAGIC_DT_RET  0x00F11200u   /* a deferred task has returned */
#define MAGIC_IOC_RET 0x00F11300u   /* an I/O completion has returned */
#define CB_SCRATCH    0x00F20000u   /* the SndCommand, copied out of the
                                     * caller's stack frame -- by the time the
                                     * callback runs, that frame is gone */

static unsigned char *g_pcm;
static unsigned  g_pcm_len;
static int       g_pcm_overflow;
static int       g_buffers_taken;
static unsigned  g_sample_rate;     /* Fixed, from the last SoundHeader */
static int       g_short_buffers;   /* headers whose length was rewritten */

/* Which physical buffer each bufferCmd named, and the length it declared.
 * Double buffering is only correct if these alternate; a duplicate or a skip
 * is audible as a chop at the buffer rate -- 3870 samples at 22254 Hz is
 * 174 ms, so about 6 Hz. */
#define BUFLOG_CAP 8192
static unsigned  g_buflog_addr[BUFLOG_CAP];
static unsigned  g_buflog_len[BUFLOG_CAP];
static int       g_buflog_n;

static int      g_cb_pending;
static int      g_in_callback;
static unsigned g_cb_chan;
static int      g_cb_runs;        /* callbacks actually executed */
static long long g_cb_queued_instr;


/* Whether a sound callback may run while the engine is still mid-call.
 *
 * `.sp` wanted it to: its Prime rendered a whole utterance synchronously, so
 * answering the callback the instant the buffer was offered kept it running
 * flat out.  MacinTalk 2 is double-buffered and asynchronous, and its callback
 * (Cecy 1 +$3B1E) only refills on its *second* invocation -- the first just
 * sets a flag.  Firing early therefore burns that first callback before the
 * engine has queued anything for it to be about.
 *
 * So this is per-engine policy, set from Python, not a global truth. */
static int      g_defer_cb;
#define IN_CALL_CB_WAIT 1000000LL

/* How long to wait, as a per-engine setting rather than a compile-time one.
 *
 * The constant above suits MacinTalk 2, whose first callback only sets a flag.
 * **MacinTalk 3 needs a much shorter wait**: its whole SpeakBuffer is about
 * 636,000 instructions and it disposes of the record its callback walks into
 * at ~620,000, so a million-instruction wait means the callback can never fire
 * in-call at all -- the speak ends first and the callback then runs against
 * freed memory. */
static long long g_cb_wait = IN_CALL_CB_WAIT;

/* Every Sound Manager command, in order.  MacinTalk 2 drives audio
 * asynchronously, so "why did it stop after one buffer" is a question about
 * the *sequence* of commands, which no single counter can answer. */
#define MAX_SNDLOG 512
static unsigned short g_sndlog[MAX_SNDLOG];
static int            g_sndlog_n;

static void queue_synthetic_callback(unsigned chan, unsigned hdr)
{
    m68k_write_memory_16(CB_SCRATCH + 0, callBackCmd);
    m68k_write_memory_16(CB_SCRATCH + 2, 4);
    m68k_write_memory_32(CB_SCRATCH + 4, hdr);
    g_cb_chan = chan;
    g_cb_pending = 1;
    g_cb_queued_instr = g_instr_count;
}

/* Take one buffer's worth of samples.
 *
 * `length` is read from the header every time and never assumed.  The driver
 * rewrites it (SetBufLength, +$4C36) so the last buffer of an utterance is
 * short; taking a fixed 3870 would append stale bytes to every phrase and show
 * up as a ~6 Hz chop under the voice. */
static void take_std_buffer(unsigned hdr)
{
    unsigned len = m68k_read_memory_32(hdr + 4);
    unsigned ptr = m68k_read_memory_32(hdr + 0);
    unsigned area = ptr ? ptr : (hdr + 0x16);
    unsigned i;

    g_sample_rate = m68k_read_memory_32(hdr + 8);
    if (len != 0x0F1E) g_short_buffers++;
    if (len > 0x10000u) {           /* nothing legitimate is this big */
        note_fault(hdr + 4, 0, 4);
        return;
    }
    for (i = 0; i < len; i++) {
        if (g_pcm_len >= PCM_CAP) { g_pcm_overflow++; return; }
        g_pcm[g_pcm_len++] = (unsigned char)m68k_read_memory_8(area + i);
    }
    if (g_buflog_n < BUFLOG_CAP) {
        g_buflog_addr[g_buflog_n] = hdr;
        g_buflog_len[g_buflog_n] = len;
        g_buflog_n++;
    }
    g_buffers_taken++;
}

static int take_buffer(unsigned hdr)
{
    unsigned encode = m68k_read_memory_8(hdr + 20);
    unsigned len, ptr, area, channels, frames, bits, bytes_per_sample, i;

    if (encode == 0x00u) {
        take_std_buffer(hdr);
        return 1;
    }
    if (encode == 0xFEu) {
        fprintf(stderr, "compressed SoundHeader at 0x%X is not supported\n", hdr);
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = 10;
        g_stop_pc = m68k_get_reg(NULL, M68K_REG_PPC);
        m68k_end_timeslice();
        return 0;
    }
    if (encode != 0xFFu) {
        fprintf(stderr, "unknown SoundHeader encode 0x%02X at 0x%X\n",
                encode, hdr);
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = 10;
        g_stop_pc = m68k_get_reg(NULL, M68K_REG_PPC);
        m68k_end_timeslice();
        return 0;
    }

    ptr = m68k_read_memory_32(hdr + 0);
    channels = m68k_read_memory_32(hdr + 4);
    frames = m68k_read_memory_32(hdr + 22);
    bits = m68k_read_memory_16(hdr + 48);
    bytes_per_sample = (bits + 7u) / 8u;
    if (!channels || !bytes_per_sample || frames > 0x10000u
        || channels > 64u || bytes_per_sample > 16u) {
        note_fault(hdr, 0, 0);
        return 0;
    }
    len = frames * channels * bytes_per_sample;
    if (frames && len / frames != channels * bytes_per_sample) {
        note_fault(hdr + 22, 0, 4);
        return 0;
    }
    area = ptr ? ptr : (hdr + 64u);

    g_sample_rate = m68k_read_memory_32(hdr + 8);
    if (len != 0x0F1E) g_short_buffers++;
    if (len > 0x10000u) {
        note_fault(hdr + 22, 0, 4);
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (g_pcm_len >= PCM_CAP) { g_pcm_overflow++; return 1; }
        g_pcm[g_pcm_len++] = (unsigned char)m68k_read_memory_8(area + i);
    }
    if (g_buflog_n < BUFLOG_CAP) {
        g_buflog_addr[g_buflog_n] = hdr;
        g_buflog_len[g_buflog_n] = len;
        g_buflog_n++;
    }
    g_buffers_taken++;
    return 1;
}

static int serve_sound_trap(unsigned base, unsigned exc_sp, unsigned csp)
{
    switch (base) {
    case 0xA803:                        /* _SndDoCommand   */
    case 0xA804: {                      /* _SndDoImmediate */
        /* These two do NOT have the same signature:
         *
         *     SndDoCommand  (chan, cmd, noWait)   -- 10 bytes of arguments
         *     SndDoImmediate(chan, cmd)           --  8
         *
         * Treating them alike reads the command pointer two bytes off (giving
         * addresses like $FFDC001D) and, worse, pops two bytes too many, which
         * silently corrupts the caller's frame.  That is how MACSTARTSOUND came
         * back "cleanly" having written none of its SoundHeader fields. */
        int nowait = (base == 0xA803);
        unsigned pbytes = nowait ? 10u : 8u;
        unsigned cmdp = m68k_read_memory_32(csp + (nowait ? 2 : 0));
        unsigned chan = m68k_read_memory_32(csp + (nowait ? 6 : 4));
        unsigned cmd = m68k_read_memory_16(cmdp);
        unsigned param2 = m68k_read_memory_32(cmdp + 4);
        int i;

        if (g_sndlog_n < MAX_SNDLOG) g_sndlog[g_sndlog_n++] = (unsigned short)cmd;
        if (cmd == bufferCmd) {
            if (!take_buffer(param2)) return 1;
            /* EXPERIMENT: seed the cycle so Pro's own callBackCmds can be
             * observed. See the note below; this is not the final answer. */
            if (g_seed_cb && m68k_read_memory_32(chan + 8))
                queue_synthetic_callback(chan, param2);
            /* No synthetic callback. Both engines queue their OWN
             * callBackCmd behind every bufferCmd -- MacinTalk 2 via
             * _SndDoCommand at Cecy 1 +$391A, MacinTalk Pro 199 times from
             * module code at 0x1D18F8 -- and the callback reads `param2` out
             * of the command as its own bookkeeping pointer.
             *
             * Handing it the SOUND HEADER there is not a harmless extra
             * notification: MacinTalk Pro's callback does `movea.l $4(a0),a4`
             * and then writes `$24(a4)`, so a synthetic one makes it scribble
             * over the header it is rendering from. */
        } else if (cmd == callBackCmd) {
            /* Copy the command somewhere that outlives the caller's frame,
             * then run the callback once we are safely outside m68k_execute. */
            for (i = 0; i < 8; i++)
                m68k_write_memory_8(CB_SCRATCH + i,
                                    m68k_read_memory_8(cmdp + i));
            g_cb_chan = chan;
            g_cb_pending = 1;
            g_cb_queued_instr = g_instr_count;
        }
        /* quietCmd / flushCmd: there is no queue to drop, we already took it */
        tb_return(exc_sp, pbytes, 0, 2);
        if (g_cb_pending) m68k_end_timeslice();
        return 1;
    }
    case 0xA807: {                      /* _SndNewChannel                 */
        /* SndNewChannel(SndChannelPtr *chan, short synth, long init,
         *               SndCallBackUPP userRoutine)
         *
         * Read off the call site at Cecy 1 +$A64: two bytes of result space,
         * then chan, synth (a *word*), init, userRoutine -- 14 bytes.
         *
         * `.sp` never needed this because it was handed a channel; MacinTalk 2
         * makes its own, and the callback it registers here is the one the
         * existing sound model already looks for at SndChannel+8.  So filling
         * that field in is what wires MacinTalk 2's audio into the path `.sp`
         * already uses. */
        unsigned user_routine = m68k_read_memory_32(csp + 0);
        unsigned chanpp = m68k_read_memory_32(csp + 10);
        unsigned chan = heap_alloc(1064);       /* SndChannel + its queue */
        if (chan) {
            m68k_write_memory_32(chan + 8, user_routine);   /* callBack */
            m68k_write_memory_32(chanpp, chan);
        }
        tb_return(exc_sp, 14, chan ? 0u : 0xFF94u, 2);
        return 1;
    }
    case 0xA801:                        /* _SndDisposeChannel(chan, quiet) */
        /* Nothing to tear down: the bump allocator does not free, and we take
         * every buffer the moment it is offered, so no queue can be left. */
        tb_return(exc_sp, 6, 0, 2);
        return 1;
    case 0xA800: {                      /* _SoundDispatch, selector in D0 */
        unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
        /* **D0 is (selector << 16) | argument byte count**, which the
         * GetVoiceInfo case below already depends on: $0614000C is selector
         * $0614 with twelve bytes, and GetVoiceInfo does take twelve.
         *
         * Reading the low word as the selector -- as this did -- makes the
         * two indistinguishable, and MacinTalk Pro calls selector $000C with
         * eight bytes, whose low word is 8. That ran the SndChannelStatus
         * branch against a completely different argument layout, took a
         * caller-supplied long as a pointer, and wrote through 0x1C380028,
         * which is not even inside the emulator's 16 MB. It was the wild
         * write that sent Pro branching into a wave table. */
        unsigned bytes = d0 & 0xFFFFu;
        unsigned selector = d0 >> 16;

        /* The Speech Manager rides on the Sound Manager's trap, so this is not
         * a sound call at all:
         *
         *     GetVoiceInfo(VoiceSpec *voice, OSType 'fref', void *info)
         *
         * read off Cecy 1 +$7FC, where D0 is $0614000C -- twelve bytes of
         * arguments in the low word -- and the selector pushed is 'fref',
         * which Apple's Speech.h calls soVoiceFile.
         *
         * `info` is a VoiceFileInfo: an FSSpec (vRefNum, parID, Str63 name =
         * 70 bytes) followed by **resID at +70**.  That resID is the whole
         * point: the engine opens the file and immediately does
         * _Get1Resource('ttvd', resID).  Our Resource Manager keeps one flat
         * table and no files, so the FSSpec can be nominal, but the resID has
         * to be right. */
        if (d0 == 0x0614000Cu) {
            unsigned info    = m68k_read_memory_32(csp + 0);
            unsigned vspec   = m68k_read_memory_32(csp + 8);
            unsigned creator = m68k_read_memory_32(vspec + 0);
            unsigned vid     = m68k_read_memory_32(vspec + 4);
            int k, found = 0, vfile = -1;
            short res_id = 0;
            for (k = 0; k < g_voice_count; k++) {
                if (g_voices[k].creator == creator && g_voices[k].id == vid) {
                    res_id = g_voices[k].res_id;
                    vfile = g_voices[k].file;
                    found = 1; break;
                }
            }
            if (found) {
                /* **The name is not decoration for MacinTalk Pro.** It takes
                 * this FSSpec and calls _OpenRFPerm on it, then walks that
                 * fork's resource map. An empty name sent it to the engine's
                 * own file instead of the voice's, where the units it wanted
                 * were not -- reported as resFNotFound, -193, from Pro's own
                 * lookup rather than from any trap.
                 *
                 * A voice registered without a file keeps the nominal FSSpec
                 * MacinTalk 2 has always been given. */
                if (vfile >= 0 && vfile < g_file_count) {
                    m68k_write_memory_16(info + 0, FAKE_VREFNUM);
                    m68k_write_memory_32(info + 2, FAKE_DIRID);
                    file_put_name(vfile, info + 6);
                } else {
                    m68k_write_memory_16(info + 0, 0);  /* FSSpec.vRefNum   */
                    m68k_write_memory_32(info + 2, 0);  /* FSSpec.parID     */
                    m68k_write_memory_8 (info + 6, 0);  /* FSSpec.name, ""  */
                }
                m68k_write_memory_16(info + 70,
                                     (unsigned)(unsigned short)res_id);
            }
            /* -244 is voiceNotFound, which is what the caller expects when a
             * VoiceSpec names something that is not installed. */
            tb_return(exc_sp, 12, found ? 0u : 0xFF0Cu, 2);
            return 1;
        }
        if (selector == 8 && bytes == 10) {  /* SndChannelStatus(chan,len,stat) */
            unsigned stat = m68k_read_memory_32(csp + 0);
            unsigned len = m68k_read_memory_16(csp + 4);
            unsigned i;
            for (i = 0; i < len && i < 64; i++)
                m68k_write_memory_8(stat + i, 0);
            /* scChannelBusy at +12.  We consume buffers instantly, so the
             * channel is never busy and every wait loop exits at once. */
            m68k_write_memory_8(stat + 12, 0);
            tb_return(exc_sp, 10, 0, 2);
            return 1;
        }
        if (d0 == 0x000C0008u) {
            /* SndSoundManagerVersion: **no arguments at all**, and a
             * four-byte NumVersion result. Read off the call site in the
             * loaded module -- `subq.l #$4,a7` reserves the long, nothing is
             * pushed, then `move.l (a7)+` pops it and `cmpi.b #$3` tests the
             * major version.
             *
             * So D0's low word is NOT an argument byte count here, and
             * deriving one from it removes eight bytes that were never
             * pushed. Answer 2.0.0: below 3, which selects the classic
             * channel-and-bufferCmd path this host actually models. */
            tb_return(exc_sp, 0, 0x02008000u, 4);
            return 1;
        }
        /* Anything else: answer noErr but **remove exactly what was pushed**.
         * The byte count is in D0 and guessing it is how a stack gets one
         * argument out of step, which does not fail here -- it fails later,
         * somewhere unrelated, as a wild pointer. */
        tb_return(exc_sp, bytes <= 64 ? bytes : 10, 0, 2);
        return 1;
    }
    default:
        return 0;
    }
}

