// Translations (Thai) for Uplink, applied where the game draws, measures and wraps text.
//
// Uplink's gucci layer draws every string through GciDrawText(x, y, text, font) (0x4ea0b0), measures it with
// GciTextWidth(text, font) (0x4ea120), and wraps long text with wordwraptext(text, linesize) (0x490690), which
// counts bytes. All three are "wraps" in recomp_config.json:
//   - the English text is looked up in lang/<lang>/strings.tsv: the whole string, else line by line, each line
//     exactly or against templates made from the game's own format strings ("Connected to %s"); captured
//     arguments are translated too when they are table entries;
//   - ASCII runs of the result are still drawn and measured by the game's FTGL font (Dungeon), Thai runs are
//     rasterised here with stb_truetype from an OFL Thai font, baseline at the same raster position;
//   - wordwraptext on translated text wraps by pixel width (Thai has no spaces between words: it breaks at
//     spaces when it can and between characters otherwise) and builds the game's own LList of lines.
// strings.tsv: "english<TAB>translation" per line, \n \t \\ escaped; templates use %s %d %u %c %x %f (with flags,
// width, precision) and the translation may reorder them as %1$s %2$d.
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define GCI_DRAW_TEXT   0x004ea0b0u     // (x, y, text, font)
#define GCI_TEXT_WIDTH  0x004ea120u     // (text, font) -> pixels
#define GCI_FONT        0x005e95c4u     // current default font index
#define OP_NEW          0x00541ebfu     // operator new(size)
#define OP_NEW_ARRAY    0x00540377u     // operator new[](size), freed by the callers with delete[]
#define LLIST_PUTDATA   0x0041ccd0u     // LList<char*>::PutData(const T&), thiscall

void f_4ea0b0(void);
void f_4ea120(void);
void f_490690(void);
void gl1_draw_text_alpha(int gx, int gy, int w, int h, int top, const uint8_t *alpha, uint64_t key);

extern char g_lang[8];
static int lang_on;
static int is_ascii_str(const char *s);

// ---------------------------------------------------------------- table
typedef struct { char *en, *tr; } Pair;
static Pair *exact; static int nexact, cap_exact;
typedef struct { char *en, *tr; int prefix; } Pat;
static Pat *pats; static int npats;
static int *htab; static int hsize;

static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static const char *exact_get(const char *s) {
    if (!hsize) return NULL;
    for (uint32_t i = hstr(s) & (hsize - 1);; i = (i + 1) & (hsize - 1)) {
        int k = htab[i];
        if (k < 0) return NULL;
        if (!strcmp(exact[k].en, s)) return exact[k].tr;
    }
}
static void unescape(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
        } else *o++ = *p;
    }
    *o = 0;
}
static int is_template(const char *s) {
    for (const char *p = s; *p; p++)
        if (*p == '%') {
            const char *q = p + 1;
            while (*q && strchr("-+ #0123456789.l", *q)) q++;
            if (*q && strchr("sducixXf", *q)) return 1;
            if (*q == '%') p = q;
        }
    return 0;
}
static int load_table(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *line = malloc(1 << 16);
    while (fgets(line, 1 << 16, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab || !tab[1]) continue;
        *tab = 0;
        char *en = strdup(line), *tr = strdup(tab + 1);
        unescape(en); unescape(tr);
        if (is_template(en)) {
            pats = realloc(pats, (npats + 1) * sizeof *pats);
            int pre = 0; while (en[pre] && en[pre] != '%') pre++;
            pats[npats].en = en; pats[npats].tr = tr; pats[npats].prefix = pre;
            npats++;
        } else {
            if (nexact == cap_exact) { cap_exact = cap_exact ? cap_exact * 2 : 1024; exact = realloc(exact, cap_exact * sizeof *exact); }
            exact[nexact].en = en; exact[nexact].tr = tr; nexact++;
        }
    }
    free(line);
    fclose(f);
    hsize = 1; while (hsize < nexact * 2 + 16) hsize <<= 1;
    htab = malloc(hsize * sizeof *htab);
    for (int i = 0; i < hsize; i++) htab[i] = -1;
    for (int k = 0; k < nexact; k++) {
        uint32_t i = hstr(exact[k].en) & (hsize - 1);
        while (htab[i] >= 0) i = (i + 1) & (hsize - 1);
        htab[i] = k;
    }
    // longer literal prefixes first: the most specific template wins
    for (int i = 1; i < npats; i++) {
        Pat p = pats[i]; int j = i - 1;
        while (j >= 0 && (int)strlen(pats[j].en) < (int)strlen(p.en)) { pats[j + 1] = pats[j]; j--; }
        pats[j + 1] = p;
    }
    return 1;
}

// ---------------------------------------------------------------- templates
typedef struct { char *buf; int n, cap; } Str;
static void sput(Str *s, const char *p, int n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 64; s->buf = realloc(s->buf, s->cap); }
    memcpy(s->buf + s->n, p, n); s->n += n; s->buf[s->n] = 0;
}
#define MAXCAP 12
static const char *cap_p[MAXCAP]; static int cap_n[MAXCAP]; static int ncap;
// match text t against template p (from position), capturing conversions
static int pmatch(const char *p, const char *t) {
    while (*p) {
        if (*p == '%') {
            if (p[1] == '%') { if (*t != '%') return 0; p += 2; t++; continue; }
            const char *q = p + 1;
            while (*q && strchr("-+ #0123456789.l", *q)) q++;
            char conv = *q;
            if (!conv || !strchr("sducixXf", conv)) { if (*t != '%') return 0; p++; t++; continue; }
            q++;
            if (ncap >= MAXCAP) return 0;
            int slot = ncap++;
            cap_p[slot] = t;
            if (conv == 'c') { if (!*t) { ncap--; return 0; } cap_n[slot] = 1; if (pmatch(q, t + 1)) return 1; ncap = slot; return 0; }
            if (conv == 's') {
                // shortest first, so a template with following literals finds its way
                for (int n = 0;; n++) {
                    cap_n[slot] = n; ncap = slot + 1;
                    if (pmatch(q, t + n)) return 1;
                    if (!t[n]) break;
                }
                ncap = slot; return 0;
            }
            // numbers
            int n = 0;
            if (t[n] == '-' || t[n] == '+') n++;
            int digits = 0;
            while (isdigit((unsigned char)t[n]) || ((conv == 'x' || conv == 'X') && isxdigit((unsigned char)t[n])) ||
                   (conv == 'f' && t[n] == '.')) { n++; digits++; }
            if (!digits) { ncap = slot; return 0; }
            for (int k = n; k >= 1; k--) {        // greedy, then shorter
                cap_n[slot] = k; ncap = slot + 1;
                if (pmatch(q, t + k)) return 1;
            }
            ncap = slot; return 0;
        }
        if (*p != *t) return 0;
        p++; t++;
    }
    return *t == 0;
}
static const char *translate_line(const char *s, Str *tmp);
static char *fill(const char *tr) {
    Str o = {0};
    int seq = 0;
    for (const char *p = tr; *p; p++) {
        if (*p == '%' && p[1] == '%') { sput(&o, "%", 1); p++; continue; }
        if (*p == '%') {
            const char *q = p + 1; int idx = -1;
            if (isdigit((unsigned char)*q) && q[1] == '$') { idx = *q - '1'; q += 2; }
            while (*q && strchr("-+ #0123456789.l", *q)) q++;
            if (*q && strchr("sducixXf", *q)) {
                if (idx < 0) idx = seq++;
                if (idx >= 0 && idx < ncap) {
                    char arg[1024]; int n = cap_n[idx] < 1023 ? cap_n[idx] : 1023;
                    memcpy(arg, cap_p[idx], n); arg[n] = 0;
                    const char *ta = (*q == 's' && n > 1) ? exact_get(arg) : NULL;
                    if (ta) sput(&o, ta, (int)strlen(ta)); else sput(&o, arg, n);
                }
                p = q;
                continue;
            }
        }
        sput(&o, p, 1);
    }
    if (!o.buf) sput(&o, "", 0);
    return o.buf;
}
static const char *pattern_get(const char *s) {
    size_t ls = strlen(s);
    for (int i = 0; i < npats; i++) {
        Pat *p = &pats[i];
        if ((size_t)p->prefix > ls || strncmp(p->en, s, p->prefix)) continue;
        ncap = 0;
        if (pmatch(p->en, s)) return fill(p->tr);       // caller frees
    }
    return NULL;
}
static int *sorted; static int nsorted;
static int used_prefix;       // the last translation used a prefix match (typewriter text)
static int cmp_key(const void *a, const void *b) { return strcmp(exact[*(const int *)a].en, exact[*(const int *)b].en); }
static uint32_t utf8(const char **ps);
static int is_mark_cp(uint32_t c) { return c == 0x0E31 || (c >= 0x0E33 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E); }
static const char *prefix_get(const char *s) {
    size_t ls = strlen(s);
    if (ls < 3 || !nexact) return NULL;
    if (!sorted) {
        sorted = malloc(nexact * sizeof *sorted);
        for (int i = 0; i < nexact; i++) sorted[i] = i;
        qsort(sorted, nexact, sizeof *sorted, cmp_key);
        nsorted = nexact;
    }
    int lo = 0, hi = nsorted;
    while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(exact[sorted[m]].en, s) < 0) lo = m + 1; else hi = m; }
    if (lo >= nsorted) return NULL;
    Pair *p = &exact[sorted[lo]];
    size_t le = strlen(p->en);
    if (le <= ls || strncmp(p->en, s, ls)) return NULL;
    // cut the translation at the same fraction of its characters, never before a combining mark
    int total = 0;
    for (const char *q = p->tr; *q;) { utf8(&q); total++; }
    int want = (int)((double)total * ls / le + 0.5), k = 0;
    const char *q = p->tr;
    while (*q && k < want) { utf8(&q); k++; }
    while (*q) { const char *r = q; uint32_t c = utf8(&r); if (!is_mark_cp(c)) break; q = r; }
    static char buf[8192];
    size_t n = (size_t)(q - p->tr); if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, p->tr, n); buf[n] = 0;
    used_prefix = 1;
    return buf;
}
#define FRAG_MIN 6
#define FBUCKETS 4096
static int *fhead, *fnext;      // buckets by the first FRAG_MIN characters
static uint32_t fkey(const char *s) { uint32_t h = 2166136261u; for (int i = 0; i < FRAG_MIN; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; } return h & (FBUCKETS - 1); }
static int word_char(char c) { return isalnum((unsigned char)c) || c == '\''; }
static int fragments_get(const char *s, Str *out) {
    if (!nexact) return 0;
    if (!fhead) {
        fhead = malloc(FBUCKETS * sizeof *fhead); fnext = malloc(nexact * sizeof *fnext);
        for (int i = 0; i < FBUCKETS; i++) fhead[i] = -1;
        for (int k = 0; k < nexact; k++) {
            fnext[k] = -1;
            const char *e = exact[k].en;
            if ((int)strlen(e) < FRAG_MIN || !strchr(e, ' ') || !*exact[k].tr) continue;
            uint32_t b = fkey(e); fnext[k] = fhead[b]; fhead[b] = k;
        }
    }
    int n = (int)strlen(s), letters = 0, done = 0, changed = 0;
    for (int i = 0; i < n; i++) if (isalpha((unsigned char)s[i])) letters++;
    Str o = {0};
    for (int i = 0; i < n;) {
        int best = -1, blen = 0;
        if ((i == 0 || !word_char(s[i - 1])) && n - i >= FRAG_MIN) {
            for (int k = fhead[fkey(s + i)]; k >= 0; k = fnext[k]) {
                int l = (int)strlen(exact[k].en);
                if (l > blen && l <= n - i && !strncmp(s + i, exact[k].en, l) && (i + l == n || !word_char(s[i + l]) || !word_char(s[i + l - 1]))) { best = k; blen = l; }
            }
        }
        if (best >= 0) {
            const char *tr = exact[best].tr;
            sput(&o, tr, (int)strlen(tr));
            for (int j = 0; j < blen; j++) if (isalpha((unsigned char)s[i + j])) done++;
            i += blen; changed = 1;
        } else { sput(&o, s + i, 1); i++; }
    }
    int ok = changed && done * 10 >= letters * 4;
    if (ok) sput(out, o.buf, o.n);
    free(o.buf);
    return ok;
}
// one line: exact, then trimmed of surrounding blanks, then templates, then fragments, then a typewriter prefix
static const char *translate_line(const char *s, Str *out) {
    const char *t = exact_get(s);
    if (t) { sput(out, t, (int)strlen(t)); return t; }
    int a = 0, b = (int)strlen(s);
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    if (b - a >= 2 && (a > 0 || b < (int)strlen(s))) {
        char *core = malloc(b - a + 1); memcpy(core, s + a, b - a); core[b - a] = 0;
        t = exact_get(core);
        char *pt = t ? NULL : (char *)pattern_get(core);
        free(core);
        if (t || pt) {
            sput(out, s, a);
            sput(out, t ? t : pt, (int)strlen(t ? t : pt));
            sput(out, s + b, (int)strlen(s + b));
            free(pt);
            return out->buf;
        }
    }
    char *pt = (char *)pattern_get(s);
    if (pt) { sput(out, pt, (int)strlen(pt)); free(pt); return out->buf; }
    // sentences the game glues together around names ("Earlier today the" + company + "system was ..."):
    // translate every known fragment inside the line, longest first, and keep the rest
    if (fragments_get(s, out)) return out->buf;
    // typewriter effects draw growing prefixes of a known line: show the same share of the translation
    t = prefix_get(s);
    if (t) { sput(out, t, (int)strlen(t)); return out->buf; }
    return NULL;
}

// translation cache: the same strings are drawn every frame
// A prefix match only stands while the text keeps changing: a string drawn unchanged for more than ~1.5 s is a
// label of its own ("Uplink" is also the start of "Uplink Corporation ..."), so it goes back to English.
typedef struct { char *en, *tr; int prefix, hits; } CacheEnt;    // tr == NULL: no translation
#define CACHE 8192
static CacheEnt cache[CACHE];
static int cache_used;
static const char *translate(const char *s) {
    if (!lang_on || !s || !*s) return NULL;
    uint32_t h = hstr(s) & (CACHE - 1);
    for (uint32_t i = h; cache[i].en; i = (i + 1) & (CACHE - 1)) {
        if (!strcmp(cache[i].en, s)) {
            if (cache[i].prefix && ++cache[i].hits > 90) { free(cache[i].tr); cache[i].tr = NULL; cache[i].prefix = 0; }
            return cache[i].tr;
        }
    }
    used_prefix = 0;
    const char *r = exact_get(s);
    char *res = NULL;
    if (r) res = strdup(r);
    else {
        // line by line
        Str o = {0}; int changed = 0;
        const char *p = s;
        while (1) {
            const char *e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            char *ln = malloc(n + 1); memcpy(ln, p, n); ln[n] = 0;
            Str t = {0};
            if (n && translate_line(ln, &t)) { sput(&o, t.buf, t.n); changed = 1; }
            else sput(&o, ln, n);
            free(t.buf); free(ln);
            if (!e) break;
            sput(&o, "\n", 1);
            p = e + 1;
        }
        if (changed) res = o.buf; else free(o.buf);
    }
    if (cache_used > CACHE * 3 / 4) {
        for (int i = 0; i < CACHE; i++) { free(cache[i].en); free(cache[i].tr); cache[i].en = cache[i].tr = NULL; }
        cache_used = 0;
    }
    uint32_t i = h;
    while (cache[i].en) i = (i + 1) & (CACHE - 1);
    cache[i].en = strdup(s); cache[i].tr = res; cache[i].prefix = used_prefix && res; cache[i].hits = 0; cache_used++;
    if (getenv("PORT_LANGLOG") && !res && is_ascii_str(s)) {
        int letters = 0;
        for (const char *p = s; *p; p++) letters += isalpha((unsigned char)*p) != 0;
        if (letters >= 3) host_log("untranslated: %s\n", s);
    }
    return res;
}
const char *lang_translate(const char *s) { return translate(s); }

// ---------------------------------------------------------------- Thai rendering
static stbtt_fontinfo thai;
static uint8_t *thai_data;
static int is_thai(uint32_t cp) { return cp >= 0x0E00 && cp <= 0x0E7F; }
static int is_ascii_str(const char *s) { for (; *s; s++) if ((uint8_t)*s >= 0x80) return 0; return 1; }
static uint32_t utf8(const char **ps) {
    const uint8_t *p = (const uint8_t *)*ps; uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    *ps = (const char *)p;
    return c;
}
static int above(uint32_t c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || c == 0x0E47 || c == 0x0E4D || c == 0x0E4E; }
static int tone(uint32_t c) { return c >= 0x0E48 && c <= 0x0E4C; }
static int below(uint32_t c) { return c >= 0x0E38 && c <= 0x0E3A; }
static int tall(uint32_t c) { return c == 0x0E1B || c == 0x0E1D || c == 0x0E1F || c == 0x0E2C; }
static int desc(uint32_t c) { return c == 0x0E0E || c == 0x0E0F || c == 0x0E24 || c == 0x0E26; }

static float font_px[16];      // Thai pixel size per game font index, from the Latin font's measured width
static float thai_px(int font) {
    if (font < 0 || font > 15) font = 5;
    if (!font_px[font]) {
        // the game's own width of a reference string: 26 lower-case letters
        uint32_t ref = guest_strdup("abcdefghijklmnopqrstuvwxyz");
        uint32_t w = guest_call(GCI_TEXT_WIDTH, 2, ref, (uint32_t)font);
        heap_free(ref);
        float avg = w ? w / 26.0f : 6.0f;
        float k = getenv("PORT_THAI_SCALE") ? (float)atof(getenv("PORT_THAI_SCALE")) : 2.45f;
        font_px[font] = avg * k;
        host_log("lang: font %d latin avg %.2f px -> thai %.1f px\n", font, avg, font_px[font]);
    }
    return font_px[font];
}
// layout of one Thai run: glyph positions in pixels from the pen start
typedef struct { int g; float x, y; } Placed;
static int layout(const char *s, float px, Placed *out, int max, float *adv_out) {
    float sc = stbtt_ScaleForPixelHeight(&thai, px);
    float em = sc * 1000.0f;           // the font has 1000 units per em
    float pen = 0; int n = 0;
    uint32_t base = 0, prev = 0; float base_x = 0;
    int stacked_above = 0;
    for (const char *p = s; *p && n < max;) {
        uint32_t c = utf8(&p);
        int g = stbtt_FindGlyphIndex(&thai, c);
        int aw, lsb; stbtt_GetGlyphHMetrics(&thai, g, &aw, &lsb);
        float x = pen, y = 0;
        if (aw == 0 && is_thai(c)) {
            // combining mark: drawn by its own (negative) bearing over the previous base
            float shift = tall(base) ? -0.10f * em : 0;
            if (tone(c) && (stacked_above || (*p && (uint8_t)p[0] == 0xE0 && utf8(&(const char *){p}) == 0x0E33)))
                y -= 0.17f * em;                                        // above an upper vowel or before sara am
            if (above(c)) stacked_above = 1;
            if (below(c) && desc(base)) y += 0.15f * em;
            x = pen + shift; (void)base_x;
        } else {
            base = c; base_x = pen; stacked_above = 0;
        }
        out[n].g = g; out[n].x = x; out[n].y = y; n++;
        pen += aw * sc;
        if (*p) { const char *q = p; uint32_t nx = utf8(&q); pen += stbtt_GetCodepointKernAdvance(&thai, c, nx) * sc; }
        prev = c;
    }
    (void)prev;
    *adv_out = pen;
    return n;
}
static float thai_width(const char *s, float px) {
    static Placed pl[2048]; float adv;
    layout(s, px, pl, 2048, &adv);
    return adv;
}
static uint64_t fnv64(const char *s, uint64_t h) { while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; } return h; }
static void thai_draw(int x, int y, const char *s, float px) {
    static Placed pl[2048]; float adv;
    int n = layout(s, px, pl, 2048, &adv);
    float sc = stbtt_ScaleForPixelHeight(&thai, px);
    int asc, dsc, gap; stbtt_GetFontVMetrics(&thai, &asc, &dsc, &gap);
    int top = (int)ceil(asc * sc * 1.05f), bot = (int)ceil(-dsc * sc);
    int w = (int)ceil(adv) + (int)(px) + 2, h = top + bot + 2;
    int ox = (int)px / 2;               // room for marks with negative bearing at the start
    uint8_t *a = calloc((size_t)w * h, 1);
    for (int i = 0; i < n; i++) {
        int x0, y0, x1, y1;
        float fx = pl[i].x + ox, fy = pl[i].y + top;
        stbtt_GetGlyphBitmapBoxSubpixel(&thai, pl[i].g, sc, sc, fx - floorf(fx), 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        uint8_t *gb = calloc((size_t)gw * gh, 1);
        stbtt_MakeGlyphBitmapSubpixel(&thai, gb, gw, gh, gw, sc, sc, fx - floorf(fx), 0, pl[i].g);
        int bx = (int)floorf(fx) + x0, by = (int)floorf(fy) + y0;
        for (int yy = 0; yy < gh; yy++)
            for (int xx = 0; xx < gw; xx++) {
                int X = bx + xx, Y = by + yy;
                if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                int v = a[Y * w + X] + gb[yy * gw + xx];
                a[Y * w + X] = (uint8_t)(v > 255 ? 255 : v);
            }
        free(gb);
    }
    // a little gamma so small text stays readable on the dark screens
    for (int i = 0; i < w * h; i++) a[i] = (uint8_t)(255.0f * powf(a[i] / 255.0f, 0.8f));
    gl1_draw_text_alpha(x - ox, y, w, h, top, a, fnv64(s, 1469598103934665603ull) ^ (uint64_t)(px * 100));
    free(a);
}

// split text into ASCII and non-ASCII runs and call back for each
typedef void (*RunFn)(const char *run, int len, int thai_run, void *ud);
static void runs(const char *s, RunFn fn, void *ud) {
    const char *p = s;
    while (*p) {
        int t = (uint8_t)*p >= 0x80;
        const char *q = p;
        // spaces and punctuation between Thai stay with the Thai run
        while (*q && (((uint8_t)*q >= 0x80) == t || (t && (*q == ' ') && q[1] && (uint8_t)q[1] >= 0x80))) q++;
        fn(p, (int)(q - p), t, ud);
        p = q;
    }
}
typedef struct { int x, y, font; float pen; int measure_only; } RunCtx;
static void run_cb(const char *run, int len, int thai_run, void *ud) {
    RunCtx *c = ud;
    char *tmp = malloc(len + 1); memcpy(tmp, run, len); tmp[len] = 0;
    if (thai_run) {
        float px = thai_px(c->font);
        if (!c->measure_only) thai_draw(c->x + (int)(c->pen + 0.5f), c->y, tmp, px);
        c->pen += thai_width(tmp, px);
    } else {
        uint32_t g = guest_strdup(tmp);
        if (!c->measure_only) guest_call(GCI_DRAW_TEXT, 4, (uint32_t)(c->x + (int)(c->pen + 0.5f)), (uint32_t)c->y, g, (uint32_t)c->font);
        c->pen += (float)(int32_t)guest_call(GCI_TEXT_WIDTH, 2, g, (uint32_t)c->font);
        heap_free(g);
    }
    free(tmp);
}
static int text_width(const char *s, int font) {
    RunCtx c = {0, 0, font, 0, 1};
    runs(s, run_cb, &c);
    return (int)(c.pen + 0.5f);
}

// ---------------------------------------------------------------- the wraps (all cdecl)
// GciDrawText(x, y, text, font)
void host_draw_text(void) {
    uint32_t text = ARG(2);
    const char *t = text ? translate(GSTR(text)) : NULL;
    if (!t && lang_on && text && !is_ascii_str(GSTR(text))) t = GSTR(text);    // already translated (wrapped lines)
    if (!t) { f_4ea0b0(); return; }
    int x = (int32_t)ARG(0), y = (int32_t)ARG(1), font = (int32_t)ARG(3);
    uint32_t ret = R32(R.esp);
    if (is_ascii_str(t)) {
        uint32_t g = guest_strdup(t);
        guest_call(GCI_DRAW_TEXT, 4, (uint32_t)x, (uint32_t)y, g, (uint32_t)font);
        heap_free(g);
    } else {
        RunCtx c = {x, y, font, 0, 0};
        runs(t, run_cb, &c);
    }
    (void)ret;
    R.esp += 4;
}
// GciTextWidth(text, font)
void host_text_width(void) {
    uint32_t text = ARG(0);
    const char *t = text ? translate(GSTR(text)) : NULL;
    if (!t && lang_on && text && !is_ascii_str(GSTR(text))) t = GSTR(text);
    if (!t) { f_4ea120(); return; }
    int font = (int32_t)ARG(1);
    R.eax = (uint32_t)text_width(t, font);
    R.esp += 4;
}
// wordwraptext(text, linesize) -> LList<char *> * (one buffer of NUL-separated lines; the first entry owns it)
void host_word_wrap(void) {
    uint32_t text = ARG(0), linesize = ARG(1);
    const char *t = text ? translate(GSTR(text)) : NULL;
    if (!t) { f_490690(); return; }
    if (is_ascii_str(t)) {
        uint32_t g = guest_strdup(t);
        R.eax = guest_call(0x490690u, 2, g, linesize);
        heap_free(g);
        R.esp += 4;
        return;
    }
    int font = (int32_t)R32(GCI_FONT);
    // callers draw the lines 10 px inside a box `linesize` wide (text_draw: x + 10); keep the same margin right
    int width = (int32_t)linesize - 20;
    if (width < 40) width = (int32_t)linesize;
    // lines: break at '\n'; within a line, at the last space that fits, else before the character that overflows
    size_t L = strlen(t);
    char *buf = malloc(L + 2);
    int nl = 0; int *starts = malloc(sizeof(int) * (L + 2));
    size_t o = 0;
    const char *p = t;
    while (1) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        // wrap [p, p+n)
        size_t a = 0;
        do {
            size_t best = 0, sp = 0, i = a;
            char tmp[4096];
            while (i < n) {
                const char *q = p + i; utf8(&q);
                size_t ni = (size_t)(q - p);
                // never split before a combining mark or a following vowel
                while (ni < n) { const char *r = p + ni; uint32_t c2 = utf8(&r); int aw, lsb;
                    if (is_thai(c2)) { stbtt_GetGlyphHMetrics(&thai, stbtt_FindGlyphIndex(&thai, c2), &aw, &lsb); if (aw == 0 || c2 == 0x0E30 || c2 == 0x0E32 || c2 == 0x0E33 || c2 == 0x0E45) { ni = (size_t)(r - p); continue; } }
                    break; }
                size_t len = ni - a; if (len > sizeof tmp - 1) len = sizeof tmp - 1;
                memcpy(tmp, p + a, len); tmp[len] = 0;
                if (best && text_width(tmp, font) > width) break;
                best = ni;
                if (p[i] == ' ') sp = i;
                i = ni;
            }
            size_t cut = best;
            if (best < n && sp > a) cut = sp;              // back to the last space in the line
            if (cut <= a) cut = best > a ? best : n;
            starts[nl++] = (int)o;
            memcpy(buf + o, p + a, cut - a); o += cut - a; buf[o++] = 0;
            a = cut;
            while (a < n && p[a] == ' ') a++;
        } while (a < n);
        if (!e) break;
        p = e + 1;
    }
    // the guest list
    uint32_t list = guest_call(OP_NEW, 1, 0x14u);
    W32(list, 0); W32(list + 4, 0); W32(list + 16, 0); W32(list + 8, 0); W32(list + 12, 0xffffffffu);
    uint32_t gbuf = guest_call(OP_NEW_ARRAY, 1, (uint32_t)o + 1);
    memcpy(G + gbuf, buf, o); G[gbuf + o] = 0;
    uint32_t slot = heap_alloc(4);
    for (int i = 0; i < nl; i++) {
        W32(slot, gbuf + starts[i]);
        guest_call_this(LLIST_PUTDATA, list, 1, slot);
    }
    heap_free(slot);
    free(buf); free(starts);
    R.eax = list;
    R.esp += 4;
}

// ---------------------------------------------------------------- start-up
static int read_file(const char *path, uint8_t **out, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc(*n);
    int ok = fread(*out, 1, *n, f) == (size_t)*n;
    fclose(f);
    return ok;
}
void lang_init(void) {
    if (!strcmp(g_lang, "en") || !g_lang[0]) return;
    const char *roots[] = {getenv("PORT_LANGDIR"), "/lang", "lang", "../lang", "../../lang", "port/lang"};
    char dir[512] = "", path[600];
    for (size_t i = 0; i < sizeof roots / sizeof *roots; i++) {
        if (!roots[i]) continue;
        snprintf(path, sizeof path, "%s/%s/strings.tsv", roots[i], g_lang);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); snprintf(dir, sizeof dir, "%s/%s", roots[i], g_lang); break; }
    }
    if (!dir[0]) { host_log("lang: no strings.tsv for '%s'\n", g_lang); return; }
    snprintf(path, sizeof path, "%s/strings.tsv", dir);
    load_table(path);
    long n;
    snprintf(path, sizeof path, "%s/NotoSansThai-Regular.ttf", dir);
    if (!read_file(path, &thai_data, &n) || !stbtt_InitFont(&thai, thai_data, stbtt_GetFontOffsetForIndex(thai_data, 0))) {
        host_log("lang: cannot load %s\n", path);
        return;
    }
    lang_on = 1;
    host_log("lang: %s, %d strings + %d templates from %s\n", g_lang, nexact, npats, dir);
}
