/* osp_nrl.c -- English text to MacinTalk phonemes, using the engine's own
 * rule table.  A port of `nrl.py`, which is the reference and stays the
 * reference; `tools/nrl_oracle.py` diffs the two.
 *
 * MacinTalk speaks phonemes and nothing else: its `Prime` rejects English at
 * +$338, and outSPOKEN's own Pascal ran these rules in front of it using the
 * `RULZ` resource.  **The rules are the user's data; only the interpreter is
 * ours.**  Nothing here embeds any part of the engine -- `RULZ` and Berkeley's
 * exception list `DICT` are handed in by the caller, exactly like `DRVR` and
 * `TALK`.
 *
 * `RULZ` is 28 big-endian 4-byte offsets followed by 28 buckets of rules
 * separated by backslashes.  Buckets 0-25 are A-Z, 26 is digits, 27 is
 * punctuation.  Each rule is
 *
 *     left [ focus ] right = phonemes
 *
 * in the notation of Elovitz et al., NRL Report 7948 (1976).  `DICT` has the
 * same shape with a backtick as separator and *respelled English* on the
 * right-hand side, which then goes through the rules like any other word;
 * that is how Berkeley fixed "sea-rch" without touching the 1984 rule set.
 *
 * Among the rules matching at the cursor **the longest focus wins**, with
 * file order breaking ties -- measured in the Python at 97.9% against the
 * file's own assertions where the textbook first-match reading scores 91.5%.
 * Do not sort the buckets: file order is load-bearing for the ties.
 *
 * **Text is MacRoman bytes.**  The Python uppercases Unicode text with
 * `str.upper()` before matching, and three MacRoman letters uppercase to two
 * ASCII ones -- ß to SS, and the ligatures ﬁ and ﬂ to FI and FL -- so they
 * reach the rules as letters where every other high byte reaches them as
 * nothing at all (no rule names one; the files are pure ASCII).  `NRL_UPPER`
 * is `str.upper()` evaluated per MacRoman byte and re-encoded, tabulated,
 * not judged; the two-byte cases are the three named above.  A byte whose
 * uppercase has no MacRoman spelling (µ becomes a Greek capital) is kept as
 * it is, which changes nothing a rule can see.
 */

/* ---- character classes --------------------------------------------------- */

#define NRL_VOWEL   0x01   /* AEIOUY                 '#'          */
#define NRL_CONS    0x02   /* BCDFGHJKLMNPQRSTVWXZ   '^' and ':'  */
#define NRL_FRONT   0x04   /* EIY                    '+'          */
#define NRL_VOICED  0x08   /* BDVGJLMNRWZ            '.'          */
#define NRL_SIBIL   0x10   /* SCGZXJ                 '&' (+CH SH) */
#define NRL_RULEU   0x20   /* TSRDLZNJ               '@' (+TH CH SH) */

static unsigned char g_nrl_class[256];
static int g_nrl_class_ready;

static void nrl_classes_init(void)
{
    const char *p;
    if (g_nrl_class_ready) return;
    memset(g_nrl_class, 0, sizeof g_nrl_class);
    for (p = "AEIOUY"; *p; p++)               g_nrl_class[(unsigned char)*p] |= NRL_VOWEL;
    for (p = "BCDFGHJKLMNPQRSTVWXZ"; *p; p++) g_nrl_class[(unsigned char)*p] |= NRL_CONS;
    for (p = "EIY"; *p; p++)                  g_nrl_class[(unsigned char)*p] |= NRL_FRONT;
    for (p = "BDVGJLMNRWZ"; *p; p++)          g_nrl_class[(unsigned char)*p] |= NRL_VOICED;
    for (p = "SCGZXJ"; *p; p++)               g_nrl_class[(unsigned char)*p] |= NRL_SIBIL;
    for (p = "TSRDLZNJ"; *p; p++)             g_nrl_class[(unsigned char)*p] |= NRL_RULEU;
    g_nrl_class_ready = 1;
}
#define NRL_IS(c, cls) ((g_nrl_class[(unsigned char)(c)] & (cls)) != 0)
static int nrl_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* str.upper() per MacRoman byte 0x80-0xFF, re-encoded.  0 marks the three
 * two-byte results, handled in nrl_upper. */
static const unsigned char NRL_UPPER[128] = {
    /* 80 */ 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xE7, 0xCB, 0xE5, 0x80, 0xCC, 0x81, 0x82, 0x83, 0xE9,
    /* 90 */ 0xE6, 0xE8, 0xEA, 0xED, 0xEB, 0xEC, 0x84, 0xEE, 0xF1, 0xEF, 0x85, 0xCD, 0xF2, 0xF4, 0xF3, 0x86,
    /* A0 */ 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0x00, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    /* B0 */ 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xAE, 0xAF,
    /* C0 */ 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCE,
    /* D0 */ 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD9, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0x00, 0x00,
    /* E0 */ 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
    /* F0 */ 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0x49, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
};

/* " " + text.upper() + " ", as the Python pads it: a space in a rule then
 * means a word boundary and needs no special case. */
static void nrl_upper_padded(NumBuf *b, const unsigned char *s, int n)
{
    int i;
    nb_putc(b, ' ');
    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c >= 'a' && c <= 'z') nb_putc(b, (char)(c - 32));
        else if (c < 0x80) nb_put(b, &c, 1);
        else if (c == 0xA7) nb_puts(b, "SS");       /* ß  */
        else if (c == 0xDE) nb_puts(b, "FI");       /* ﬁ  */
        else if (c == 0xDF) nb_puts(b, "FL");       /* ﬂ  */
        else nb_putc(b, (char)NRL_UPPER[c - 0x80]);
    }
    nb_putc(b, ' ');
}

/* ---- the tables ------------------------------------------------------------ */

typedef struct {
    const unsigned char *left, *focus, *right, *out;
    int nl, nf, nr, no;
} NrlRule;

typedef struct {
    unsigned char *blob;        /* our copy of the resource */
    NrlRule *rules;             /* every bucket's rules, in file order */
    int nrules;
    int start[29];              /* bucket i is rules[start[i] .. start[i+1]) */
    int loaded;
} NrlTable;

static NrlTable g_rulz, g_dict;

static void nrl_table_free(NrlTable *t)
{
    if (t->blob) free(t->blob);
    if (t->rules) free(t->rules);
    memset(t, 0, sizeof *t);
}

static const unsigned char *nrl_find(const unsigned char *p, const unsigned char *end, unsigned char c)
{
    while (p < end) { if (*p == c) return p; p++; }
    return NULL;
}

/* One bucket's blob, split on `sep`.  A piece is a rule when it holds a `[`,
 * a `]` after it and a `=` after that; anything else -- empty pieces between
 * separators, a trailing fragment -- is skipped, as the Python skips it.
 * (The Python raises on a piece whose brackets are out of order; the files
 * have none, and here such a piece is simply not a rule.) */
static int nrl_parse_bucket(NrlTable *t, const unsigned char *p, const unsigned char *end,
                            unsigned char sep, int count_only)
{
    int n = 0;
    while (p <= end) {
        const unsigned char *q = nrl_find(p, end, sep);
        const unsigned char *piece_end = q ? q : end;
        const unsigned char *lb = nrl_find(p, piece_end, '[');
        const unsigned char *rb = lb ? nrl_find(lb + 1, piece_end, ']') : NULL;
        const unsigned char *eq = rb ? nrl_find(rb + 1, piece_end, '=') : NULL;
        if (lb && rb && eq) {
            if (!count_only) {
                NrlRule *r = &t->rules[t->nrules];
                r->left = p;       r->nl = (int)(lb - p);
                r->focus = lb + 1; r->nf = (int)(rb - lb - 1);
                r->right = rb + 1; r->nr = (int)(eq - rb - 1);
                r->out = eq + 1;   r->no = (int)(piece_end - eq - 1);
                t->nrules++;
            }
            n++;
        }
        if (!q) break;
        p = q + 1;
    }
    return n;
}

static int nrl_table_load(NrlTable *t, const unsigned char *data, int len, unsigned char sep)
{
    unsigned offs[28];
    int i, total = 0;

    nrl_classes_init();
    nrl_table_free(t);
    if (len < 112) return -1;
    for (i = 0; i < 28; i++)
        offs[i] = ((unsigned)data[i * 4] << 24) | ((unsigned)data[i * 4 + 1] << 16)
                | ((unsigned)data[i * 4 + 2] << 8) | (unsigned)data[i * 4 + 3];
    /* Equal offsets are legal and mean an empty bucket. */
    if (offs[0] != 112) return -1;
    for (i = 0; i < 27; i++) if (offs[i] > offs[i + 1]) return -1;
    if (offs[27] > (unsigned)len) return -1;      /* equal is a trailing empty bucket */

    t->blob = (unsigned char *)malloc((size_t)len + 1);
    if (!t->blob) return -1;
    memcpy(t->blob, data, (size_t)len);
    t->blob[len] = 0;

    for (i = 0; i < 28; i++) {
        unsigned end = i + 1 < 28 ? offs[i + 1] : (unsigned)len;
        total += nrl_parse_bucket(t, t->blob + offs[i], t->blob + end, sep, 1);
    }
    t->rules = (NrlRule *)calloc((size_t)(total ? total : 1), sizeof(NrlRule));
    if (!t->rules) { nrl_table_free(t); return -1; }
    for (i = 0; i < 28; i++) {
        unsigned end = i + 1 < 28 ? offs[i + 1] : (unsigned)len;
        t->start[i] = t->nrules;
        nrl_parse_bucket(t, t->blob + offs[i], t->blob + end, sep, 0);
    }
    t->start[28] = t->nrules;
    t->loaded = 1;
    return 0;
}

static int nrl_bucket_for(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (nrl_is_digit(c)) return 26;
    return 27;
}

/* ---- context matching ------------------------------------------------------
 *
 * Left context is matched right-to-left from just before the focus; right
 * context left-to-right from just after it.  `text` is the padded, uppercased
 * buffer of `n` bytes. */

/* Length of the suffix at `i`, or 0.  `%` is one of ER, E, ES, ED, ING, ELY
 * -- and they are suffixes, so the match only counts when the word ends
 * there.  Longest first, in the Python's (stable) sort order. */
static int nrl_suffix_at(const unsigned char *text, int n, int i)
{
    static const char *const suf[6] = { "ING", "ELY", "ER", "ES", "ED", "E" };
    int k;
    for (k = 0; k < 6; k++) {
        int l = (int)strlen(suf[k]), j = i + l;
        if (j <= n && memcmp(text + i, suf[k], (size_t)l) == 0
            && (j >= n || text[j] == ' '))
            return l;
    }
    return 0;
}

static int nrl_pair_at(const unsigned char *text, int n, int i, const char *a, const char *b)
{
    if (i + 2 > n) return 0;
    if (text[i] == (unsigned char)a[0] && text[i + 1] == (unsigned char)a[1]) return 1;
    return b && text[i] == (unsigned char)b[0] && text[i + 1] == (unsigned char)b[1];
}

static int nrl_match_right(const unsigned char *text, int n, int i,
                           const unsigned char *pat, int np)
{
    int k;
    for (k = 0; k < np; k++) {
        unsigned char c = pat[k];
        if (c == '#') {                              /* one or more vowels */
            if (i >= n || !NRL_IS(text[i], NRL_VOWEL)) return 0;
            while (i < n && NRL_IS(text[i], NRL_VOWEL)) i++;
        } else if (c == ':') {                       /* zero or more consonants */
            while (i < n && NRL_IS(text[i], NRL_CONS)) i++;
        } else if (c == '^') {
            if (i >= n || !NRL_IS(text[i], NRL_CONS)) return 0;
            i++;
        } else if (c == '+') {
            if (i >= n || !NRL_IS(text[i], NRL_FRONT)) return 0;
            i++;
        } else if (c == '.') {
            if (i >= n || !NRL_IS(text[i], NRL_VOICED)) return 0;
            i++;
        } else if (c == '&') {                       /* sibilant, incl. CH and SH */
            if (nrl_pair_at(text, n, i, "CH", "SH")) i += 2;
            else if (i < n && NRL_IS(text[i], NRL_SIBIL)) i++;
            else return 0;
        } else if (c == '@') {                       /* consonant giving long U */
            if (nrl_pair_at(text, n, i, "TH", NULL) || nrl_pair_at(text, n, i, "CH", "SH")) i += 2;
            else if (i < n && NRL_IS(text[i], NRL_RULEU)) i++;
            else return 0;
        } else if (c == '%') {
            int l = nrl_suffix_at(text, n, i);
            if (!l) return 0;
            i += l;
        } else if (c == '?') {                       /* a digit */
            if (i >= n || !nrl_is_digit(text[i])) return 0;
            i++;
        } else {
            if (i >= n || text[i] != c) return 0;
            i++;
        }
    }
    return 1;
}

/* `i` is the index just past the last character of the left context. */
static int nrl_match_left(const unsigned char *text, int i,
                          const unsigned char *pat, int np)
{
    int k;
    for (k = np - 1; k >= 0; k--) {
        unsigned char c = pat[k];
        if (c == '#') {
            if (i <= 0 || !NRL_IS(text[i - 1], NRL_VOWEL)) return 0;
            while (i > 0 && NRL_IS(text[i - 1], NRL_VOWEL)) i--;
        } else if (c == ':') {
            while (i > 0 && NRL_IS(text[i - 1], NRL_CONS)) i--;
        } else if (c == '^') {
            if (i <= 0 || !NRL_IS(text[i - 1], NRL_CONS)) return 0;
            i--;
        } else if (c == '+') {
            if (i <= 0 || !NRL_IS(text[i - 1], NRL_FRONT)) return 0;
            i--;
        } else if (c == '.') {
            if (i <= 0 || !NRL_IS(text[i - 1], NRL_VOICED)) return 0;
            i--;
        } else if (c == '&') {
            if (i >= 2 && nrl_pair_at(text, i, i - 2, "CH", "SH")) i -= 2;
            else if (i > 0 && NRL_IS(text[i - 1], NRL_SIBIL)) i--;
            else return 0;
        } else if (c == '@') {
            if (i >= 2 && (nrl_pair_at(text, i, i - 2, "TH", NULL)
                           || nrl_pair_at(text, i, i - 2, "CH", "SH"))) i -= 2;
            else if (i > 0 && NRL_IS(text[i - 1], NRL_RULEU)) i--;
            else return 0;
        } else if (c == '%') {
            return 0;                                /* a right-context device only */
        } else if (c == '?') {
            if (i <= 0 || !nrl_is_digit(text[i - 1])) return 0;
            i--;
        } else {
            if (i <= 0 || text[i - 1] != c) return 0;
            i--;
        }
    }
    return 1;
}

/* The one loop both translate and respell share: at each cursor the longest
 * matching focus in the bucket wins, file order breaking ties.  On a match
 * the rule's right-hand side is emitted; with none, `keep_unmatched` says
 * whether the character passes through (respell) or is skipped (translate). */
static void nrl_apply(const NrlTable *t, const unsigned char *s, int n,
                      NumBuf *out, int keep_unmatched)
{
    NumBuf p;
    int i;
    nb_init(&p);
    nrl_upper_padded(&p, s, n);
    if (p.oom) { out->oom = 1; nb_free(&p); return; }
    for (i = 1; i < (int)p.len - 1; ) {
        int bk = nrl_bucket_for(p.p[i]);
        const NrlRule *best = NULL;
        int best_len = 0, r;
        for (r = t->start[bk]; r < t->start[bk + 1]; r++) {
            const NrlRule *rule = &t->rules[r];
            if (rule->nf <= best_len) continue;             /* cannot beat what we have */
            if (i + rule->nf > (int)p.len) continue;         /* the slice comes up short */
            if (memcmp(p.p + i, rule->focus, (size_t)rule->nf) != 0) continue;
            if (!nrl_match_left(p.p, i, rule->left, rule->nl)) continue;
            if (!nrl_match_right(p.p, (int)p.len, i + rule->nf, rule->right, rule->nr)) continue;
            best = rule; best_len = rule->nf;
        }
        if (best) {
            nb_put(out, best->out, (size_t)best->no);
            i += best->nf;
        } else {
            if (keep_unmatched) nb_put(out, p.p + i, 1);
            i++;
        }
    }
    nb_free(&p);
}

/* str.strip() over what the rules can emit: ASCII whitespace, including the
 * four separators 0x1C-0x1F that Python counts as space. */
static int nrl_is_space(unsigned char c)
{
    return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F);
}
static void nrl_strip(NumBuf *b, int left, const char *extra)
{
    size_t s = 0, e = b->len;
    while (e > s && (nrl_is_space(b->p[e - 1]) || (extra && strchr(extra, b->p[e - 1])))) e--;
    if (left) while (s < e && nrl_is_space(b->p[s])) s++;
    if (s) memmove(b->p, b->p + s, e - s);
    b->len = e - s;
}

static int nrl_deliver(NumBuf *b, unsigned char *out, int cap)
{
    int need;
    if (b->oom) { nb_free(b); return -1; }
    need = (int)b->len;
    if (out && cap > 0) memcpy(out, b->p, b->len < (size_t)cap ? b->len : (size_t)cap);
    nb_free(b);
    return need;
}

/* ---- the API ---------------------------------------------------------------- */

OSP_API int osp_nrl_load(const unsigned char *data, int len)
{
    return nrl_table_load(&g_rulz, data, len, '\\');
}

OSP_API int osp_nrl_load_dictionary(const unsigned char *data, int len)
{
    return nrl_table_load(&g_dict, data, len, '`');
}

OSP_API void osp_nrl_unload(void)
{
    nrl_table_free(&g_rulz);
    nrl_table_free(&g_dict);
}

OSP_API int osp_nrl_rule_count(int dictionary)
{
    const NrlTable *t = dictionary ? &g_dict : &g_rulz;
    return t->loaded ? t->nrules : -2;
}

/* English -> a MacinTalk phoneme string, stripped. */
OSP_API int osp_nrl_translate(const unsigned char *text, int len, unsigned char *out, int cap)
{
    NumBuf b;
    if (!g_rulz.loaded) return -2;
    nb_init(&b);
    nrl_apply(&g_rulz, text, len < 0 ? 0 : len, &b, 0);
    if (!b.oom) nrl_strip(&b, 1, NULL);
    return nrl_deliver(&b, out, cap);
}

/* Apply Berkeley's exception list, returning English, not phonemes.  Anything
 * not matched passes through unchanged, so this is safe over any text. */
OSP_API int osp_nrl_respell(const unsigned char *text, int len, unsigned char *out, int cap)
{
    NumBuf b;
    if (!g_dict.loaded) return -2;
    nb_init(&b);
    nrl_apply(&g_dict, text, len < 0 ? 0 : len, &b, 1);
    return nrl_deliver(&b, out, cap);
}

/* How a lone letter should be announced.  Twenty-five of the twenty-six
 * already come out as their names; a bare A matches the rule for the *word*
 * "a" and comes out a schwa, so for A alone the dotted reading ` [A. ]` is
 * taken from the user's own rules instead.  The dotted form is no good for
 * the rest -- consonants lose their vowel -- so it is used only there. */
OSP_API int osp_nrl_letter_name(const unsigned char *text, int len, unsigned char *out, int cap)
{
    NumBuf plain, dotted, up;
    int is_a;
    if (!g_rulz.loaded) return -2;
    if (len < 0) len = 0;

    nb_init(&plain);
    nrl_apply(&g_rulz, text, len, &plain, 0);
    if (!plain.oom) nrl_strip(&plain, 1, NULL);

    /* ch.upper() != "A" */
    nb_init(&up);
    nrl_upper_padded(&up, text, len);
    is_a = !up.oom && up.len == 3 && up.p[1] == 'A';
    nb_free(&up);
    if (!is_a) return nrl_deliver(&plain, out, cap);

    nb_init(&dotted);
    {
        NumBuf t2;
        nb_init(&t2);
        nb_put(&t2, text, (size_t)len);
        nb_putc(&t2, '.');
        if (t2.oom) dotted.oom = 1;
        else nrl_apply(&g_rulz, t2.p, (int)t2.len, &dotted, 0);
        nb_free(&t2);
    }
    if (!dotted.oom) {
        nrl_strip(&dotted, 1, NULL);            /* translate() strips both ends */
        nrl_strip(&dotted, 0, ". ");            /* .rstrip(". ") */
    }
    if (dotted.oom || plain.oom) { nb_free(&dotted); nb_free(&plain); return -1; }
    if (dotted.len) { nb_free(&plain); return nrl_deliver(&dotted, out, cap); }
    nb_free(&dotted);
    return nrl_deliver(&plain, out, cap);
}
