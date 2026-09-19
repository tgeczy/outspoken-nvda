/* osp_numbers.c -- numbers as words, in English and Mexican Spanish.
 *
 * A port of `numwords.py`, which is the reference and stays the reference:
 * `tools/numbers_oracle.py` diffs this against it over a corpus in every mode,
 * and a disagreement is a failed build.  Where the Python has a quirk, this
 * has the same quirk -- "the 1,234,567th" backtracks to "1,234" here exactly
 * as it does there -- because one behaviour everywhere beats a port that
 * quietly improves on its specification.  Fix both in one commit or neither.
 *
 * **Why it is here.**  None of these engines can count: `RULZ` bucket 26 is
 * the ten digit names and nothing else, so `30` is "three zero".  The Python
 * put a number reader in front of the rules in 0.9, and Spanish came with
 * Carlos in 1.2.0.  Until 2.0 it ran only where Python ran, which meant NVDA
 * and the SAPI bridge's embedded interpreter; the host runs it now so that
 * Linux and Android reach the same rules through the same code.
 *
 * **Text is MacRoman bytes, in and out.**  The Python works on Unicode text
 * and the driver encodes afterwards; here the encoding has already happened.
 * The one place that shows is the `\w` class -- Python's `re` decides it by
 * `str.isalnum()`, so `NUM_WORD_HI` below is that predicate evaluated over
 * MacRoman 0x80-0xFF -- and in a letter the encoding cannot carry, which is
 * `?` by the time it arrives and no longer a letter.  The oracle compares on
 * MacRoman-representable text for that reason, and says so.
 *
 * The scanner is `_NUMBER` from the Python, written out by hand:
 *
 *     (?P<ord>  (?<![\w.])  \d+ (?:st|nd|rd|th)  \b )
 *   | (?P<num>  (?<![\w.])  -? \d{1,3}(?:,\d{3})+ (?:\.\d+)? (?![\w.])
 *             | (?<![\w.])  -? \d+  \.\d+                    (?![\w.])
 *             | (?<![\w.])  -? \d+                           (?![\w])  (?!\.\d) )
 *
 * with IGNORECASE.  Alternatives are tried in that order at each position and
 * the first to match wins, as in `re`; the only backtracking that can change
 * an answer is inside the grouped alternative, where a failed lookahead drops
 * the fraction and then the last `,ddd` group, and that is reproduced.  The
 * other two alternatives cannot backtrack into a different match: a shorter
 * digit run is always followed by a digit, which is `\w`.
 */

/* ---- tables --------------------------------------------------------------- */

static const char *const NUM_ONES_EN[20] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen"
};
static const char *const NUM_TENS_EN[10] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
    "eighty", "ninety"
};
/* Short scale, which is what English screen readers expect. */
static const struct { unsigned long long value; const char *name; }
NUM_SCALES_EN[5] = {
    { 1000000000000000ULL, "quadrillion" }, { 1000000000000ULL, "trillion" },
    { 1000000000ULL, "billion" },           { 1000000ULL, "million" },
    { 1000ULL, "thousand" }
};
/* Where the scale names stop: from 10^18 up the whole part is "large number".
 * Decided on the digit string, never after conversion -- a screen reader
 * crashing on a long digit run is a far worse answer. */
#define NUM_LIMIT         1000000000000000000ULL   /* 10^18 */
#define NUM_LIMIT_DIGITS  18                       /* significant digits */
#define NUM_MINUS_EN  "minus"
#define NUM_POINT_EN  "point"
#define NUM_LARGE_EN  "large number"

/* Mexican Spanish, for the cami voices.  MacRoman: é 8E, ó 97, ú 9C. */
static const char *const NUM_ONES_ES[30] = {
    "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho",
    "nueve", "diez", "once", "doce", "trece", "catorce", "quince",
    "diecis" "\x8E" "is", "diecisiete", "dieciocho", "diecinueve", "veinte",
    "veintiuno", "veintid" "\x97" "s", "veintitr" "\x8E" "s", "veinticuatro",
    "veinticinco", "veintis" "\x8E" "is", "veintisiete", "veintiocho",
    "veintinueve"
};
static const char *const NUM_TENS_ES[10] = {
    "", "", "", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta",
    "ochenta", "noventa"
};
static const char *const NUM_HUNDREDS_ES[10] = {
    "", "ciento", "doscientos", "trescientos", "cuatrocientos", "quinientos",
    "seiscientos", "setecientos", "ochocientos", "novecientos"
};
/* Long scale.  10^9 and 10^15 need no names of their own: the quotient a
 * scale takes runs to 999999, so "dos mil quinientos millones" falls out of
 * the same composition that makes "dos mil quinientos". */
static const struct { unsigned long long value; const char *one, *many; }
NUM_SCALES_ES[2] = {
    { 1000000000000ULL, "bill" "\x97" "n", "billones" },
    { 1000000ULL,       "mill" "\x97" "n", "millones" }
};
#define NUM_MINUS_ES  "menos"
#define NUM_POINT_ES  "punto"
#define NUM_LARGE_ES  "n" "\x9C" "mero grande"

/* Python's `\w` over MacRoman 0x80-0xFF: `str.isalnum()` of the decoded
 * character.  Generated, not judged -- see the file comment. */
static const unsigned char NUM_WORD_HI[128] = {
    /* 80 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 90 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* A0 */ 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1,
    /* B0 */ 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1,
    /* C0 */ 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
    /* D0 */ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1,
    /* E0 */ 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* F0 */ 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1
};

static int num_is_word(unsigned char c)
{
    if (c >= 0x80) return NUM_WORD_HI[c - 0x80];
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '_';
}
static int num_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* ---- a growable byte string ---------------------------------------------- */

typedef struct { unsigned char *p; size_t len, cap; int oom; } NumBuf;

static void nb_init(NumBuf *b) { b->p = NULL; b->len = b->cap = 0; b->oom = 0; }
static void nb_free(NumBuf *b) { if (b->p) free(b->p); nb_init(b); }
/* Make room for `extra` more bytes after `len`. -> 0, or -1 with oom set. */
static int nb_reserve(NumBuf *b, size_t extra)
{
    size_t want;
    unsigned char *q;
    if (b->oom) return -1;
    if (b->len + extra <= b->cap) return 0;
    want = b->cap ? b->cap : 256;
    while (want < b->len + extra) want *= 2;
    q = (unsigned char *)realloc(b->p, want);
    if (!q) { b->oom = 1; return -1; }
    b->p = q; b->cap = want;
    return 0;
}
static void nb_put(NumBuf *b, const void *s, size_t n)
{
    if (b->oom || !n) return;
    if (nb_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}
static void nb_puts(NumBuf *b, const char *s) { nb_put(b, s, strlen(s)); }
static void nb_putc(NumBuf *b, char c) { nb_put(b, &c, 1); }
static int nb_ends_with(const NumBuf *b, const char *s)
{
    size_t n = strlen(s);
    return b->len >= n && memcmp(b->p + b->len - n, s, n) == 0;
}

/* ---- cardinals ------------------------------------------------------------ */

static void num_cardinal_en(NumBuf *b, unsigned long long n)
{
    int i;
    if (n >= NUM_LIMIT) { nb_puts(b, NUM_LARGE_EN); return; }
    if (n < 20) { nb_puts(b, NUM_ONES_EN[n]); return; }
    if (n < 100) {
        nb_puts(b, NUM_TENS_EN[n / 10]);
        if (n % 10) { nb_putc(b, ' '); nb_puts(b, NUM_ONES_EN[n % 10]); }
        return;
    }
    if (n < 1000) {
        nb_puts(b, NUM_ONES_EN[n / 100]);
        nb_puts(b, " hundred");
        if (n % 100) { nb_putc(b, ' '); num_cardinal_en(b, n % 100); }
        return;
    }
    for (i = 0; i < 5; i++) {
        if (n >= NUM_SCALES_EN[i].value) {
            unsigned long long q = n / NUM_SCALES_EN[i].value;
            unsigned long long r = n % NUM_SCALES_EN[i].value;
            num_cardinal_en(b, q);
            nb_putc(b, ' ');
            nb_puts(b, NUM_SCALES_EN[i].name);
            if (r) { nb_putc(b, ' '); num_cardinal_en(b, r); }
            return;
        }
    }
}

static void num_cardinal_es(NumBuf *b, unsigned long long n);

/* 'veintiuno' -> 'veintiún', 'treinta y uno' -> 'treinta y un'.  Only ever
 * before a scale word, which is the only place this is called.  The longer
 * suffix is tested first because "veintiuno" also ends in "uno". */
static void num_apocope_es(NumBuf *b, unsigned long long q)
{
    NumBuf t;
    nb_init(&t);
    num_cardinal_es(&t, q);
    if (t.oom) { b->oom = 1; nb_free(&t); return; }
    if (nb_ends_with(&t, "veintiuno")) {
        t.len -= strlen("veintiuno");
        nb_puts(&t, "veinti" "\x9C" "n");
    } else if (nb_ends_with(&t, "uno")) {
        t.len -= strlen("uno");
        nb_puts(&t, "un");
    }
    nb_put(b, t.p, t.len);
    nb_free(&t);
}

static void num_cardinal_es(NumBuf *b, unsigned long long n)
{
    unsigned long long q, r;
    int i;
    if (n >= NUM_LIMIT) { nb_puts(b, NUM_LARGE_ES); return; }
    if (n < 30) { nb_puts(b, NUM_ONES_ES[n]); return; }
    if (n < 100) {
        nb_puts(b, NUM_TENS_ES[n / 10]);
        if (n % 10) { nb_puts(b, " y "); nb_puts(b, NUM_ONES_ES[n % 10]); }
        return;
    }
    if (n == 100) { nb_puts(b, "cien"); return; }
    if (n < 1000) {
        nb_puts(b, NUM_HUNDREDS_ES[n / 100]);
        if (n % 100) { nb_putc(b, ' '); num_cardinal_es(b, n % 100); }
        return;
    }
    for (i = 0; i < 2; i++) {
        if (n >= NUM_SCALES_ES[i].value) {
            q = n / NUM_SCALES_ES[i].value;
            r = n % NUM_SCALES_ES[i].value;
            if (q == 1) {
                nb_puts(b, "un ");
                nb_puts(b, NUM_SCALES_ES[i].one);
            } else {
                num_apocope_es(b, q);
                nb_putc(b, ' ');
                nb_puts(b, NUM_SCALES_ES[i].many);
            }
            if (r) { nb_putc(b, ' '); num_cardinal_es(b, r); }
            return;
        }
    }
    /* Thousands: bare "mil" for exactly one of them, never "uno mil". */
    q = n / 1000; r = n % 1000;
    if (q == 1) nb_puts(b, "mil");
    else { num_apocope_es(b, q); nb_puts(b, " mil"); }
    if (r) { nb_putc(b, ' '); num_cardinal_es(b, r); }
}

/* 1 -> 'first', 22 -> 'twenty second'.  The irregulars are a table; the rest
 * are the cardinal with its last word suffixed. */
static void num_ordinal_en(NumBuf *b, unsigned long long n)
{
    static const char *const irregular[][2] = {
        { "one", "first" }, { "two", "second" }, { "three", "third" },
        { "five", "fifth" }, { "eight", "eighth" }, { "nine", "ninth" },
        { "twelve", "twelfth" }
    };
    NumBuf t;
    size_t last;
    int i;
    if (n >= NUM_LIMIT) { nb_puts(b, NUM_LARGE_EN); return; }   /* never "large numberth" */
    nb_init(&t);
    num_cardinal_en(&t, n);
    if (t.oom) { b->oom = 1; nb_free(&t); return; }
    last = t.len;
    while (last > 0 && t.p[last - 1] != ' ') last--;
    nb_put(b, t.p, last);                              /* every word but the last */
    for (i = 0; i < 7; i++) {
        size_t n1 = strlen(irregular[i][0]);
        if (t.len - last == n1 && memcmp(t.p + last, irregular[i][0], n1) == 0) {
            nb_puts(b, irregular[i][1]);
            nb_free(&t);
            return;
        }
    }
    if (t.p[t.len - 1] == 'y') {
        nb_put(b, t.p + last, t.len - last - 1);
        nb_puts(b, "ieth");
    } else {
        nb_put(b, t.p + last, t.len - last);
        nb_puts(b, "th");
    }
    nb_free(&t);
}

/* '2024' -> 'two zero two four'.  Anything that is not a digit passes through
 * as itself, spaced like the rest, which is what the Python does too. */
static void num_digits(NumBuf *b, const unsigned char *s, size_t n, int es)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (i) nb_putc(b, ' ');
        if (num_is_digit(s[i]))
            nb_puts(b, es ? NUM_ONES_ES[s[i] - '0'] : NUM_ONES_EN[s[i] - '0']);
        else
            nb_put(b, s + i, 1);
    }
}

/* ---- the scanner ---------------------------------------------------------- */

typedef struct {
    int kind;                 /* 1 = ordinal, 2 = number */
    size_t end;               /* one past the match */
    int neg;
    size_t whole_s, whole_e;  /* digits, possibly with commas; the ordinal's head */
    size_t frac_s, frac_e;    /* empty when there is none */
} NumMatch;

static size_t num_digit_run(const unsigned char *t, size_t n, size_t p)
{
    while (p < n && num_is_digit(t[p])) p++;
    return p;
}
/* (?![\w.]) */
static int num_after_ok(const unsigned char *t, size_t n, size_t p)
{
    return p >= n || !(num_is_word(t[p]) || t[p] == '.');
}
static int num_suffix_at(const unsigned char *t, size_t n, size_t p)
{
    unsigned char a, b;
    if (p + 2 > n) return 0;
    a = (unsigned char)(t[p] | 0x20); b = (unsigned char)(t[p + 1] | 0x20);
    return (a == 's' && b == 't') || (a == 'n' && b == 'd')
        || (a == 'r' && b == 'd') || (a == 't' && b == 'h');
}

static int num_match_at(const unsigned char *t, size_t n, size_t p, NumMatch *m)
{
    size_t q, e1;

    /* (?<![\w.]) -- shared by every alternative */
    if (p > 0 && (num_is_word(t[p - 1]) || t[p - 1] == '.')) return 0;

    memset(m, 0, sizeof *m);

    /* ord: \d+ (st|nd|rd|th) \b */
    if (num_is_digit(t[p])) {
        e1 = num_digit_run(t, n, p);
        if (num_suffix_at(t, n, e1) && (e1 + 2 >= n || !num_is_word(t[e1 + 2]))) {
            m->kind = 1; m->whole_s = p; m->whole_e = e1; m->end = e1 + 2;
            return 1;
        }
    }

    /* num: -? then digits */
    q = p;
    if (t[q] == '-') { m->neg = 1; q++; }
    if (q >= n || !num_is_digit(t[q])) return 0;
    e1 = num_digit_run(t, n, q);
    m->kind = 2;
    m->whole_s = q;

    /* (a)  \d{1,3}(?:,\d{3})+ (?:\.\d+)? (?![\w.]) */
    if (e1 - q <= 3 && e1 < n && t[e1] == ',') {
        size_t pos = e1, groups = 0;
        while (pos + 4 <= n && t[pos] == ',' && num_is_digit(t[pos + 1])
               && num_is_digit(t[pos + 2]) && num_is_digit(t[pos + 3])) {
            pos += 4; groups++;
        }
        if (groups >= 1) {
            /* with the fraction */
            if (pos + 1 < n && t[pos] == '.' && num_is_digit(t[pos + 1])) {
                size_t e2 = num_digit_run(t, n, pos + 1);
                if (num_after_ok(t, n, e2)) {
                    m->whole_e = pos; m->frac_s = pos + 1; m->frac_e = e2; m->end = e2;
                    return 1;
                }
            }
            /* without it */
            if (num_after_ok(t, n, pos)) { m->whole_e = pos; m->end = pos; return 1; }
            /* one group fewer: the next character is then the dropped comma,
             * which is neither \w nor '.', so this always succeeds */
            if (groups >= 2) { m->whole_e = pos - 4; m->end = pos - 4; return 1; }
        }
    }

    /* (b)  \d+ \. \d+ (?![\w.]) */
    if (e1 + 1 < n && t[e1] == '.' && num_is_digit(t[e1 + 1])) {
        size_t e2 = num_digit_run(t, n, e1 + 1);
        if (num_after_ok(t, n, e2)) {
            m->whole_e = e1; m->frac_s = e1 + 1; m->frac_e = e2; m->end = e2;
            return 1;
        }
    }

    /* (c)  \d+ (?![\w]) (?!\.\d) */
    if ((e1 >= n || !num_is_word(t[e1]))
        && !(e1 + 1 < n && t[e1] == '.' && num_is_digit(t[e1 + 1]))) {
        m->whole_e = e1; m->end = e1;
        return 1;
    }
    return 0;
}

/* Significant digits, ignoring commas and leading zeros; stops counting at
 * one past the limit, so a five-thousand-digit run costs nothing. */
static int num_too_big(const unsigned char *s, size_t n)
{
    size_t i; int sig = 0, started = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == ',') continue;
        if (!started && s[i] == '0') continue;
        started = 1;
        if (++sig > NUM_LIMIT_DIGITS) return 1;
    }
    return 0;
}
static unsigned long long num_value(const unsigned char *s, size_t n)
{
    unsigned long long v = 0; size_t i;
    for (i = 0; i < n; i++)
        if (num_is_digit(s[i])) v = v * 10 + (unsigned)(s[i] - '0');
    return v;
}

static void num_substitute(NumBuf *b, const unsigned char *t, const NumMatch *m,
                           int spell_out, int es)
{
    const char *large = es ? NUM_LARGE_ES : NUM_LARGE_EN;
    size_t wn = m->whole_e - m->whole_s;

    if (m->kind == 1) {
        /* An ordinal always becomes a word, whatever `spell_out` says:
         * "three r d" helps nobody.  In Spanish the bare cardinal, because an
         * English suffix in Spanish text is already foreign and "tercero"
         * would be a guess about gender and position. */
        if (num_too_big(t + m->whole_s, wn)) nb_puts(b, large);
        else if (es) num_cardinal_es(b, num_value(t + m->whole_s, wn));
        else num_ordinal_en(b, num_value(t + m->whole_s, wn));
        return;
    }
    if (m->neg) { nb_puts(b, es ? NUM_MINUS_ES : NUM_MINUS_EN); nb_putc(b, ' '); }
    if (spell_out) {
        /* Digit by digit, commas dropped, every digit kept: a user who asked
         * for digits gets all of them, however many. */
        size_t i; int first = 1;
        for (i = m->whole_s; i < m->whole_e; i++) {
            if (t[i] == ',') continue;
            if (!first) nb_putc(b, ' ');
            first = 0;
            nb_puts(b, es ? NUM_ONES_ES[t[i] - '0'] : NUM_ONES_EN[t[i] - '0']);
        }
    } else if (num_too_big(t + m->whole_s, wn)) {
        nb_puts(b, large);
    } else if (es) {
        num_cardinal_es(b, num_value(t + m->whole_s, wn));
    } else {
        num_cardinal_en(b, num_value(t + m->whole_s, wn));
    }
    if (m->frac_e > m->frac_s) {
        /* The fractional part is digit by digit either way: nobody reads
         * .50 as "fifty". */
        nb_putc(b, ' ');
        nb_puts(b, es ? NUM_POINT_ES : NUM_POINT_EN);
        nb_putc(b, ' ');
        num_digits(b, t + m->frac_s, m->frac_e - m->frac_s, es);
    }
}

/* Rewrite every number in `text` as words.
 *
 * `text` is MacRoman, `len` bytes, not necessarily NUL-terminated.
 * `spell_out` gives digit by digit, the engines' own behaviour, for a user who
 * prefers it; `spanish` selects the cami voices' language.  The result is
 * written to `out`, at most `cap` bytes, and the return value is how many
 * bytes the whole result needs -- larger than `cap` means it was cut and the
 * caller should come back with a bigger buffer.  -1 means no memory. */
OSP_API int osp_numbers(const unsigned char *text, int len, int spell_out,
                        int spanish, unsigned char *out, int cap)
{
    NumBuf b;
    size_t p = 0, n = len < 0 ? 0 : (size_t)len;
    NumMatch m;
    int need;

    nb_init(&b);
    while (p < n) {
        if (num_match_at(text, n, p, &m)) {
            num_substitute(&b, text, &m, spell_out, spanish);
            p = m.end;
        } else {
            nb_put(&b, text + p, 1);
            p++;
        }
    }
    if (b.oom) { nb_free(&b); return -1; }
    need = (int)b.len;
    if (out && cap > 0) memcpy(out, b.p, b.len < (size_t)cap ? b.len : (size_t)cap);
    nb_free(&b);
    return need;
}
