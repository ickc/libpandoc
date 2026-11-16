/* A fuzzer of libpandoc's C interface: random calls, checked against what
 * the header promises. fuzz/run.sh runs it in many child processes; see
 * fuzz/README.md.
 *
 *   fuzz [-u] [-D] SEED COUNT [FIRST]
 *
 * Makes iterations FIRST .. FIRST+COUNT-1 (FIRST defaults to 0). Each has
 * its own random state, made from SEED and its number, so that it can be
 * made again alone: `fuzz SEED 1 K`. An iteration is a call or a few, or a
 * few threads making calls at once.
 *   -u  every call is untrusted (fuzz/run.sh traces such children's opens)
 *   -D  write iteration FIRST's inputs into the working directory instead
 *       of making its calls
 * Standard error: "iter K" before each iteration, "calls N" at the end.
 * Exit 0; 3 after "VIOLATION ...", when an invariant fails; 4 after "HANG",
 * when an iteration runs over 90 s.
 *
 * What pandoc offers is asked of pandoc at start: its readers and writers
 * (pandoc_query), its options and their types (pandoc.cli.default_options,
 * through a Lua filter), small valid documents for each reader (a markdown
 * document written by each writer of the same name). It makes, in its own
 * temporary directory, a canary: files and a user data directory whose
 * content (CANARY) must never come out of an untrusted call. */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libpandoc.h>

#define CANARY "XYZZY-CANARY-71c3"
#define PATH_MAX_ROOT 4096

/* ---- randomness: splitmix64, a state per iteration and thread ---- */

typedef struct { uint64_t s; } rng;

static uint64_t rnext(rng *r)
{
    uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static size_t below(rng *r, size_t n) { return n ? rnext(r) % n : 0; }
static int chance(rng *r, int percent) { return (int)below(r, 100) < percent; }
/* mostly small, now and then up to max (log-uniform) */
static size_t upto(rng *r, size_t max)
{
    unsigned bits = 0;
    while (bits < 40 && ((size_t)1 << bits) < max) bits++;
    size_t n = below(r, ((size_t)1 << below(r, bits + 1)) + 1);
    return n > max ? max : n;
}
#define PICK(r, a) ((a)[below((r), sizeof(a) / sizeof *(a))])

/* ---- growing buffers ---- */

typedef struct { char *p; size_t n, cap; } buf;

/* under the runner's memory cap, the fuzzer's own allocations fail too:
 * counted as running out of memory */
static void *need(void *p)
{
    if (!p) {
        fputs("fuzz: out of memory\n", stderr);
        _exit(251);
    }
    return p;
}

static void grow(buf *b, size_t n)
{
    if (b->n + n + 1 <= b->cap) return;
    b->cap = (b->n + n + 1) * 2;
    b->p = need(realloc(b->p, b->cap));
}
static void bput(buf *b, const void *s, size_t n)
{
    grow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfill(buf *b, char c, size_t n)
{
    grow(b, n);
    memset(b->p + b->n, c, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void brep(buf *b, const char *s, size_t times)
{
    for (size_t i = 0; i < times; i++) bstr(b, s);
}
static void bfmt(buf *b, const char *fmt, ...)
{
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    bput(b, t, k < (int)sizeof t ? (size_t)k : sizeof t - 1);
}
static const char *bget(buf *b) { return b->p ? b->p : ""; }

/* ---- checks ---- */

static __thread long iteration;
static int violations_fatal = 1;

static void violation(const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    fprintf(stderr, "VIOLATION iter %ld: %s\n", iteration, t);
    fflush(stderr);
    if (violations_fatal) _exit(3);
}

static int utf8_ok(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i], k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
        if (k == 9 || i + k >= n + (k == 0)) return 0;
        for (unsigned j = 1; j <= k; j++)
            if ((s[i + j] & 0xc0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

/* One JSON value (RFC 8259), iteratively: pandoc's JSON nests deeply. */
static int json_str(const char *s, size_t n, size_t *i)
{
    if (*i >= n || s[*i] != '"') return 0;
    for ((*i)++; *i < n; (*i)++) {
        unsigned char c = s[*i];
        if (c == '"') { (*i)++; return 1; }
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (++*i >= n) return 0;
            if (s[*i] == 'u') {
                for (int k = 0; k < 4; k++)
                    if (++*i >= n || !strchr("0123456789abcdefABCDEF", s[*i]) || !s[*i]) return 0;
            } else if (!s[*i] || !strchr("\"\\/bfnrt", s[*i])) return 0;
        }
    }
    return 0;
}
static int json_ok(const char *s, size_t n)
{
    char *st = need(malloc(n + 1));
    size_t d = 0, i = 0;
    int ok = 0;
#define WS while (i < n && strchr(" \t\r\n", s[i]) && s[i]) i++
#define DIGITS(min) do { size_t k = i; while (i < n && s[i] >= '0' && s[i] <= '9') i++; if (i - k < (min)) goto out; } while (0)
    for (;;) {
        WS;
        if (i >= n) goto out;
        char c = s[i];
        if (c == '{' || c == '[') {
            st[d++] = c;
            i++;
            WS;
            if (i < n && s[i] == (c == '{' ? '}' : ']')) { d--; i++; goto after; }
            if (c == '{') {
                if (!json_str(s, n, &i)) goto out;
                WS;
                if (i >= n || s[i] != ':') goto out;
                i++;
            }
            continue;
        } else if (c == '"') {
            if (!json_str(s, n, &i)) goto out;
        } else if (n - i >= 4 && (!memcmp(s + i, "true", 4) || !memcmp(s + i, "null", 4))) {
            i += 4;
        } else if (n - i >= 5 && !memcmp(s + i, "false", 5)) {
            i += 5;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            if (c == '-') i++;
            if (i < n && s[i] == '0') i++; else DIGITS(1);
            if (i < n && s[i] == '.') { i++; DIGITS(1); }
            if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                i++;
                if (i < n && (s[i] == '+' || s[i] == '-')) i++;
                DIGITS(1);
            }
        } else goto out;
    after:
        WS;
        if (d == 0) { ok = i == n; goto out; }
        if (i >= n) goto out;
        if (s[i] == ',') {
            i++;
            if (st[d - 1] == '{') {
                WS;
                if (!json_str(s, n, &i)) goto out;
                WS;
                if (i >= n || s[i] != ':') goto out;
                i++;
            }
            continue;
        }
        if (s[i] == (st[d - 1] == '{' ? '}' : ']')) { d--; i++; goto after; }
        goto out;
    }
out:
    free(st);
    return ok;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static long calls;

/* What the header promises of every result; and, for an untrusted call,
 * no canary in it. */
static void check(pandoc_result *r, const char *call, int untrusted, int json_output)
{
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (!r) { violation("%s returned NULL", call); return; }
    if ((r->status == 0) != (r->error_kind == NULL))
        violation("%s: status %d with error_kind %s", call, r->status, r->error_kind ? r->error_kind : "NULL");
    if (!r->output) violation("%s: output is NULL", call);
    else if (r->output[r->output_len] != 0) violation("%s: output not NUL-terminated at output_len", call);
    if (!r->log) violation("%s: log is NULL", call);
    else if (!json_ok(r->log, strlen(r->log))) violation("%s: log isn't JSON: %.300s", call, r->log);
    else if (!utf8_ok((const unsigned char *)r->log, strlen(r->log))) violation("%s: log isn't UTF-8", call);
    if (r->status) {
        if (r->error_kind && !r->error_kind[0]) violation("%s: empty error_kind", call);
        if (!r->error_message || !r->error_message[0])
            violation("%s: empty error_message (kind %s)", call, r->error_kind ? r->error_kind : "NULL");
        else if (!utf8_ok((const unsigned char *)r->error_message, strlen(r->error_message)))
            violation("%s: error_message isn't UTF-8 (kind %s)", call, r->error_kind);
    } else {
        if (r->error_message) violation("%s: success with an error_message", call);
        if (json_output && r->output && !json_ok(r->output, r->output_len))
            violation("%s: output isn't JSON: %.300s", call, r->output);
    }
    if (untrusted && ((r->output && memmem(r->output, r->output_len, CANARY, strlen(CANARY))) ||
                      (r->error_message && strstr(r->error_message, CANARY)) ||
                      (r->log && strstr(r->log, CANARY))))
        violation("%s: untrusted, and the canary came out", call);
}

/* ---- what pandoc told us ---- */

typedef struct { char *name; char type[3]; } key;
static char **readers, **writers;
static size_t nreaders, nwriters;
static key keys[256];
static size_t nkeys;
typedef struct { char *p; size_t n; } bytes;
static bytes *seeds; /* one per reader */
static char root[PATH_MAX_ROOT], canary[PATH_MAX_ROOT + 8];
static char *paths_in[32];
static size_t npaths_in;
static int untrusted_only, dump;
static long ncpu;

/* Keys whose default is null, which pandoc.cli.default_options (a Lua
 * table) can't show, and keys that aren't fields (aliases, "defaults"):
 * types f format, p path read, w path written, P paths read. */
static const key extra_keys[] = {
    {"from", "f"}, {"to", "f"}, {"reader", "f"}, {"writer", "f"},
    {"output-file", "w"}, {"extract-media", "w"}, {"log-file", "w"},
    {"input-files", "P"}, {"input-file", "p"}, {"template", "p"}, {"data-dir", "p"},
    {"reference-doc", "p"}, {"csl", "p"}, {"citation-abbreviations", "p"},
    {"abbreviations", "p"}, {"epub-cover-image", "p"}, {"epub-metadata", "p"},
    {"metadata-file", "p"}, {"defaults", "p"}, {"pdf-engine", "s"},
    {"highlight-style", "s"}, {"title-prefix", "s"}, {"citeproc", "b"},
    {"html-math-method", "o"}, {"toc", "b"}, {"number-sections", "b"},
};

static const char *paths_out[] = {"out.txt", "out", "sub/out.html", "-", ""};
static const char *words[] = {
    "auto", "none", "preserve", "true", "yes", "native", "lf", "crlf", "block",
    "section", "chapter", "part", "default", "mathjax", "katex", "webtex",
    "mathml", "plain", "gladtex", "javascript", "references", "end", "document",
    "above", "below", "all", "info", "warning", "error", "html", "pygments",
    "x", "en-US", "1", "0", "-1", "1e9", "null", "*", "\\", "%", "$", "{",
};
static const char *meta_keys[] = {
    "title", "author", "lang", "bibliography", "csl", "references", "nocite",
    "link-citations", "header-includes", "include-before", "abstract", "date",
    "citation-abbreviations", "toc-title", "x",
};
static const char *markup[] = {
    "*", "_", "[", "]", "(", ")", "#", ">", "-", "+", "|", "`", "```", "{", "}",
    "\\", "<", ">", "&", ";", ":", "=", "~", "^", "$", "$$", "@", "!", "%", "\"",
    "'", "\n", "\n\n", "    ", "\t", "<div>", "</div>", "<!--", "-->", "\\begin{",
    "\\end{", "..", "::", "#+", "[@", "](", "](x)", "<table>", "<td>", "\\x",
    "\xc3\xa9", "\xff", "\r", "\x00", "\xe2\x80\x8b", "&#0;", "&#x10ffff;", "|---|",
};

static pandoc_result *convert_str(const char *opts, const char *in, size_t n)
{
    return pandoc_convert(opts, strlen(opts), in, n);
}

static void write_file(const char *dir, const char *name, const char *text)
{
    char p[8192];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    mkdir(dir, 0700);
    for (char *s = p + strlen(dir) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    FILE *f = fopen(p, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f)) { perror(p); exit(2); }
}

/* the strings of a JSON array of plain strings (format names) */
static char **strings(const char *s, size_t *n)
{
    char **v = calloc(256, sizeof *v);
    *n = 0;
    for (const char *p = strchr(s, '"'); p && *n < 255; p = strchr(p, '"')) {
        const char *e = strchr(p + 1, '"');
        if (!e) break;
        v[(*n)++] = strndup(p + 1, e - p - 1);
        p = e + 1;
    }
    return v;
}

static int is_writer(const char *f)
{
    for (size_t i = 0; i < nwriters; i++)
        if (!strcmp(writers[i], f)) return 1;
    return 0;
}

static const char sample[] =
    "---\ntitle: A *title*\nauthor: [A, B]\nlang: en\n---\n\n"
    "# Head {#h .c}\n\nSome *emph*, **strong**, `code`, $x^2$, [link](#h), ![img](img.png){width=1},\n"
    "a note[^1], [a span]{.s}, ~~del~~, H~2~O, 2^10^, \"quotes\" -- dashes...\n\n"
    "[^1]: The note.\n\n> quote\n>\n> - item\n\n1. one\n2. two\n   - nested\n\n"
    "Term\n:   Definition\n\n| a | b |\n|---|:-:|\n| 1 | 2 |\n\n```python\nprint(1)\n```\n\n"
    "::: {.div}\n<b>raw</b>\n:::\n\n$$\\int_0^1 f$$\n\nLine  \nbreak [@key, p. 1].\n\n- [ ] task\n";

static void setup(void)
{
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    char t[] = "fuzz-XXXXXX";
    if (!mkdtemp(t) || !realpath(t, root)) { perror("mkdtemp"); exit(2); }
    snprintf(canary, sizeof canary, "%s/c", root);
    /* the canary: files a document or options may name, and the user data
     * directory (HOME and XDG_DATA_HOME point into it) */
    static const char *files[] = {"doc.md", "doc.tex", "doc.typ", "inc.rst", "x.csv",
        "header.html", "abbrevs", "img.png", "x.css", "meta.yaml",
        "data/pandoc/abbreviations", "data/pandoc/translations/en.yaml"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char text[256];
        snprintf(text, sizeof text, strstr(files[i], "yaml") ? "title: %s\nFigure: %s\n" : "%s\n\n%s\n", CANARY, CANARY);
        write_file(canary, files[i], text);
    }
    for (size_t i = 0; i < 10; i++) {
        char p[8192];
        snprintf(p, sizeof p, "%s/%s", canary, files[i]);
        paths_in[npaths_in++] = strdup(p);
    }
    write_file(canary, "bib.bib", "@book{key, title={" CANARY "}, author={A}, year={2000}}\n");
    write_file(canary, "style.csl", "<style>" CANARY "</style>\n");
    static const char *tpl[] = {"html", "html5", "latex", "markdown", "plain", "typst", "rst",
        "org", "commonmark", "docbook5", "jats", "context", "ms", "man", "asciidoc", "revealjs"};
    for (size_t i = 0; i < sizeof tpl / sizeof *tpl; i++) {
        char n[64];
        snprintf(n, sizeof n, "data/pandoc/templates/default.%s", tpl[i]);
        write_file(canary, n, CANARY " $body$\n");
    }
    char p[8192];
    snprintf(p, sizeof p, "%s/bib.bib", canary); paths_in[npaths_in++] = strdup(p);
    snprintf(p, sizeof p, "%s/style.csl", canary); paths_in[npaths_in++] = strdup(p);
    snprintf(p, sizeof p, "%s/data", canary); paths_in[npaths_in++] = strdup(p);
    snprintf(p, sizeof p, "%s/home", canary); mkdir(p, 0700); setenv("HOME", p, 1);
    snprintf(p, sizeof p, "%s/data", canary); setenv("XDG_DATA_HOME", p, 1);
    /* Lua filters: none loops (a time limit doesn't stop Lua) */
    char lua[PATH_MAX_ROOT + 8];
    snprintf(lua, sizeof lua, "%s/lua", root);
    write_file(lua, "keys.lua",
        "local function t(v)\n"
        "  local ty = pandoc.utils.type(v)\n"
        "  if ty == 'boolean' then return 'b' elseif ty == 'number' then return 'n'\n"
        "  elseif ty == 'string' then return 's'\n"
        "  elseif ty:match('array') or (type(v) == 'table' and v[1] ~= nil) then\n"
        "    return 'a' .. (v[1] ~= nil and t(v[1]):sub(1, 1) or '')\n"
        "  else return 'o' end\nend\n"
        "function Pandoc(d)\n  local ks = {}\n"
        "  for k, v in pairs(pandoc.cli.default_options) do ks[#ks + 1] = k .. ':' .. t(v) end\n"
        "  table.sort(ks)\n  return pandoc.Pandoc({pandoc.Plain(table.concat(ks, ' '))})\nend\n");
    write_file(lua, "id.lua", "return {}\n");
    write_file(lua, "err.lua", "error('lua error \\255')\n");
    write_file(lua, "bad.lua", "function Pandoc(d) return 42 end\n");
    write_file(lua, "big.lua", "function Pandoc(d) return pandoc.Pandoc({pandoc.Para(pandoc.Str(string.rep('x', 1000000)))}) end\n");
    write_file(lua, "read.lua", "function Para(p) return pandoc.read(pandoc.utils.stringify(p), 'markdown').blocks end\n");
    snprintf(p, sizeof p, "%s/lua/id.lua", root); paths_in[npaths_in++] = strdup(p);
    paths_in[npaths_in++] = strdup("nonexistent.md");
    paths_in[npaths_in++] = strdup("-");

    if (pandoc_init() != 0) { fputs("pandoc_init failed\n", stderr); exit(2); }
    if (pandoc_abi_version() / 1000 != LIBPANDOC_ABI_VERSION_MAJOR ||
        pandoc_abi_version() < LIBPANDOC_ABI_VERSION) violation("pandoc_abi_version %d", pandoc_abi_version());
    const char *q[2] = {"{\"query\":\"input-formats\"}", "{\"query\":\"output-formats\"}"};
    for (int i = 0; i < 2; i++) {
        pandoc_result *r = pandoc_query(q[i], strlen(q[i]));
        check(r, "setup query", 0, 1);
        if (!r || r->status) { fputs("format query failed\n", stderr); exit(2); }
        if (i == 0) readers = strings(r->output, &nreaders); else writers = strings(r->output, &nwriters);
        pandoc_result_free(r);
    }
    char opts[8192];
    snprintf(opts, sizeof opts, "{\"from\":\"markdown\",\"to\":\"plain\",\"wrap\":\"none\",\"filters\":[\"%s/lua/keys.lua\"]}", root);
    pandoc_result *r = convert_str(opts, "x", 1);
    check(r, "setup keys", 0, 0);
    if (!r || r->status) { fprintf(stderr, "listing the options failed: %s\n", r ? r->error_message : "NULL"); exit(2); }
    for (char *s = strtok(r->output, " \n"); s && nkeys < 200; s = strtok(NULL, " \n")) {
        char *c = strrchr(s, ':');
        if (!c) continue;
        *c = 0;
        keys[nkeys].name = strdup(s);
        snprintf(keys[nkeys++].type, 3, "%s", c + 1);
    }
    pandoc_result_free(r);
    if (nkeys < 30) { fprintf(stderr, "only %zu options found\n", nkeys); exit(2); }
    for (size_t i = 0; i < sizeof extra_keys / sizeof *extra_keys; i++) keys[nkeys++] = extra_keys[i];
    /* a small valid document per reader: the sample as that format's writer
     * writes it (else the sample itself) */
    seeds = calloc(nreaders, sizeof *seeds);
    for (size_t i = 0; i < nreaders; i++) {
        seeds[i].p = (char *)sample;
        seeds[i].n = strlen(sample);
        if (!is_writer(readers[i]) || !strcmp(readers[i], "markdown")) continue;
        snprintf(opts, sizeof opts, "{\"from\":\"markdown\",\"to\":\"%s\"%s}", readers[i],
                 untrusted_only ? ",\"untrusted\":true" : "");
        r = convert_str(opts, sample, strlen(sample));
        check(r, "setup seed", untrusted_only, 0);
        if (r && !r->status && r->output_len) {
            seeds[i].p = malloc(r->output_len);
            memcpy(seeds[i].p, r->output, r->output_len);
            seeds[i].n = r->output_len;
        }
        pandoc_result_free(r);
    }
    /* for fuzz/run.sh's trace: what follows is the fuzzing */
    access("fuzz-setup-done", F_OK);
}

/* ---- inputs ---- */

static void gen_bytes(rng *r, buf *b, size_t n)
{
    int printable = chance(r, 60);
    grow(b, n);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = rnext(r);
        if (printable && c >= 0x80) c = " \n#*_[]()<>-`$\\{}:|"[c % 19];
        b->p[b->n++] = c;
    }
    b->p[b->n] = 0;
}

/* a JSON string: valid UTF-8 as it is, else (mostly) its high bytes as '?' */
static void jstr(rng *r, buf *b, const char *s, size_t n)
{
    int raw = utf8_ok((const unsigned char *)s, n) || chance(r, 10);
    bstr(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') { bput(b, "\\", 1); bput(b, &c, 1); }
        else if (c < 0x20) bfmt(b, "\\u%04x", c);
        else if (c >= 0x80 && !raw) bstr(b, "?");
        else bput(b, &c, 1);
    }
    if (chance(r, 1)) bstr(b, PICK(r, ((const char *[]){"\\ud800", "\\u0000", "\\udfff\\ud800", "\\uffff"})));
    bstr(b, "\"");
}

static void gen_format(rng *r, buf *b, int writer)
{
    const char *f;
    if (writer) {
        do f = writers[below(r, nwriters)]; while (!strcmp(f, "pdf") && !untrusted_only);
    } else f = readers[below(r, nreaders)];
    buf t = {0};
    bstr(&t, f);
    if (chance(r, 25)) {
        static const char *ext[] = {"smart", "raw_html", "raw_tex", "citations", "yaml_metadata_block",
            "sourcepos", "east_asian_line_breaks", "native_numbering", "empty_paragraphs",
            "tex_math_dollars", "auto_identifiers", "styles", "xrefs_number", "nonexistent"};
        for (size_t k = 1 + below(r, 3); k; k--) bfmt(&t, "%c%s", chance(r, 50) ? '+' : '-', PICK(r, ext));
    }
    if (chance(r, 2)) bstr(&t, ".lua");
    jstr(r, b, bget(&t), t.n);
    free(t.p);
}

static void gen_string(rng *r, buf *b, char kind)
{
    if (kind == 'f') { gen_format(r, b, chance(r, 50)); return; }
    if (kind == 'w') { const char *w = PICK(r, paths_out); jstr(r, b, w, strlen(w)); return; }
    const char *s = NULL;
    buf t = {0};
    switch (kind == 'p' ? 0 : below(r, 10)) {
    case 0: case 1: s = paths_in[below(r, npaths_in)]; break;
    case 2: case 3: case 4: s = PICK(r, words); break;
    case 5: gen_format(r, b, chance(r, 50)); return;
    case 6: s = ""; break;
    case 7: gen_bytes(r, &t, upto(r, 64)); break;
    case 8: bfill(&t, 'x', upto(r, 1 << 20)); break;
    default: bfmt(&t, "%d", (int)rnext(r)); break;
    }
    if (s) jstr(r, b, s, strlen(s)); else jstr(r, b, bget(&t), t.n);
    free(t.p);
}

static void gen_value(rng *r, buf *b, const char *type, int depth);

static void gen_object(rng *r, buf *b, int depth)
{
    bstr(b, "{");
    for (size_t k = below(r, 5), i = 0; i < k; i++) {
        if (i) bstr(b, ",");
        const char *mk = PICK(r, meta_keys);
        jstr(r, b, mk, strlen(mk));
        bstr(b, ":");
        gen_value(r, b, depth > 2 ? "s" : PICK(r, ((const char *[]){"s", "b", "as", "o", "n", "p"})), depth + 1);
    }
    bstr(b, "}");
}

static void gen_value(rng *r, buf *b, const char *type, int depth)
{
    size_t roll = below(r, 100);
    if (roll >= 75) {
        if (roll < 80) { bstr(b, "null"); return; }
        if (roll < 90) { type = PICK(r, ((const char *[]){"b", "n", "s", "as", "an", "o"})); }
        else if (roll < 93) {
            buf t = {0};
            bfill(&t, 'y', upto(r, 1 << 22));
            jstr(r, b, bget(&t), t.n);
            free(t.p);
            return;
        } else if (roll < 96) {
            size_t n = upto(r, 1 << 17);
            bfill(b, '[', n);
            bfill(b, ']', n);
            return;
        } else {
            bstr(b, PICK(r, ((const char *[]){"1e400", "-0", "1e-400", "99999999999999999999999",
                "-9223372036854775809", "0.5", "-1", "1E+2", "[]", "{}", "\"\"", "nan", "01", "1."})));
            return;
        }
    }
    switch (type[0]) {
    case 'b': bstr(b, chance(r, 50) ? "true" : "false"); break;
    case 'n':
        if (chance(r, 70)) bfmt(b, "%d", (int)below(r, 12) - 2);
        else bfmt(b, "%g", (double)(int64_t)rnext(r) / (double)(1 + below(r, 1 << 20)));
        break;
    case 'a': case 'P':
        bstr(b, "[");
        for (size_t k = below(r, 4), i = 0; i < k; i++) {
            if (i) bstr(b, ",");
            gen_value(r, b, type[0] == 'P' ? "p" : type[1] ? type + 1 : "s", depth + 1);
        }
        bstr(b, "]");
        break;
    case 'o': gen_object(r, b, depth); break;
    default: gen_string(r, b, type[0]);
    }
}

/* documents that are hard on a reader (the review's, smaller) */
static const char *gen_hostile(rng *r, buf *b)
{
    size_t n = 1 + upto(r, 20000);
    switch (below(r, 15)) {
    case 0: bfill(b, '>', n); bstr(b, " x\n"); return "markdown";
    case 1: brep(b, "[", n); bstr(b, "x"); brep(b, "](u)", n); return "markdown";
    case 2: bfill(b, '[', n * 5); return "markdown";
    case 3: for (size_t i = 1; i < n / 10 + 2; i++) { bfill(b, '`', i); bstr(b, " "); } return chance(r, 50) ? "markdown" : "gfm";
    case 4: for (size_t i = 0; i < n / 20 + 1; i++) { bfill(b, ' ', 2 * i); bstr(b, "- x\n"); } return "markdown";
    case 5: brep(b, "<div>", n); return "html";
    case 6: brep(b, "<table><tr><td>", n / 4 + 1); return "html";
    case 7: brep(b, "\\begin{itemize}\\item ", n / 4 + 1); return "latex";
    case 8: for (size_t i = 0; i < n / 20 + 1; i++) { bfill(b, ' ', i); bstr(b, "x\n\n"); } return "rst";
    case 9: for (size_t i = 0; i < n / 20 + 1; i++) { bfill(b, ' ', i); bstr(b, "- x\n"); } return "org";
    case 10: bstr(b, PICK(r, ((const char *[]){"#while true {}\n", "#let f(x) = f(x)\n#f(1)\n",
                 "#let s = \"x\"\n#while true { s = s + s }\n"}))); return "typst";
    case 11: bfill(b, '[', n * 5); bfill(b, ']', n * 5); return "json";
    case 12: brep(b, "{{", n); return "mediawiki";
    case 13: bfill(b, '{', n * 5); return "rtf";
    default: brep(b, "\\def\\a{\\a\\a}", 1); bstr(b, "\\a"); return "latex";
    }
}

/* documents that name the canary's files: an untrusted call reads none */
static const char *gen_canary(rng *r, buf *b)
{
    const char *c = canary;
    switch (below(r, 10)) {
    case 0:
        bfmt(b, "![x](%s/img.png)\n\n\\input{%s/doc.tex}\n\n[@key]\n\n", c, c);
        bfmt(b, "---\nbibliography: %s/bib.bib\ncsl: %s/style.csl\n", c, c);
        bfmt(b, "header-includes: \\input{%s/doc.tex}\n---\n", c);
        return "markdown";
    case 1:
        bfmt(b, "\\input{%s/doc.tex}\n\\include{%s/doc}\n\\bibliography{%s/bib}\n", c, c, c);
        bfmt(b, "\\includegraphics{%s/img.png}\n\\lstinputlisting{%s/doc.md}\n\\cite{key}\n", c, c);
        return "latex";
    case 2:
        bfmt(b, ".. include:: %s/inc.rst\n\n.. include:: %s/doc.md\n   :literal:\n\n", c, c);
        bfmt(b, ".. raw:: html\n   :file: %s/header.html\n\n.. csv-table::\n   :file: %s/x.csv\n", c, c);
        return "rst";
    case 3:
        bfmt(b, "#+INCLUDE: \"%s/doc.md\"\n#+INCLUDE: \"%s/doc.md\" src markdown\n", c, c);
        bfmt(b, "#+SETUPFILE: %s/doc.md\n#+bibliography: %s/bib.bib\n[cite:@key]\n", c, c);
        return "org";
    case 4:
        bfmt(b, "#include \"%s/doc.typ\"\n#read(\"%s/doc.md\")\n#image(\"%s/img.png\")\n", c, c, c);
        bfmt(b, "#csv(\"%s/x.csv\")\n#bibliography(\"%s/bib.bib\")\n", c, c);
        return "typst";
    case 5:
        bfmt(b, "<img src=\"%s/img.png\"><link rel=\"stylesheet\" href=\"%s/x.css\">", c, c);
        bfmt(b, "<script src=\"%s/doc.md\"></script><iframe src=\"%s/doc.md\"></iframe>", c, c);
        return "html";
    case 6:
        bfmt(b, "<article xmlns:xi=\"http://www.w3.org/2001/XInclude\"><xi:include href=\"%s/doc.md\" parse=\"text\"/>", c);
        bfmt(b, "<mediaobject><imageobject><imagedata fileref=\"%s/img.png\"/></imageobject></mediaobject></article>", c);
        return "docbook";
    case 7: bfmt(b, "include::%s/doc.md[]\n\nimage::%s/img.png[]\n", c, c); return "asciidoc";
    case 8: bfmt(b, "%%!include: %s/doc.md\n", c); return "t2t";
    default:
        bfmt(b, "---\nreferences: []\nbibliography: [%s/bib.bib]\nnocite: '@*'\n---\n\n![](%s/img.png)\n", c, c);
        return chance(r, 50) ? "commonmark_x" : "djot";
    }
}

static void mutate(rng *r, buf *b, const bytes *s)
{
    bput(b, s->p, s->n);
    for (size_t k = 1 + below(r, 8); k; k--) {
        size_t at = below(r, b->n + 1), len = upto(r, b->n - at);
        switch (below(r, 6)) {
        case 0: if (b->n) b->p[below(r, b->n)] ^= 1 << below(r, 8); break;
        case 1: case 2: { /* insert markup */
            const char *m = PICK(r, markup);
            size_t ml = *m ? strlen(m) : 1;
            grow(b, ml);
            memmove(b->p + at + ml, b->p + at, b->n - at);
            memcpy(b->p + at, m, ml);
            b->n += ml;
            break; }
        case 3: memmove(b->p + at, b->p + at + len, b->n - at - len); b->n -= len; break;
        case 4: { /* repeat a piece */
            size_t times = upto(r, 2000);
            len = len > 64 ? 64 : len;
            char *piece = need(strndup(b->p + at, len));
            buf t = {0};
            for (size_t i = 0; i < times && len; i++) bput(&t, piece, len);
            grow(b, t.n);
            memmove(b->p + at + t.n, b->p + at, b->n - at);
            if (t.n) memcpy(b->p + at, t.p, t.n);
            b->n += t.n;
            free(piece);
            free(t.p);
            break; }
        default: b->n = at; break;
        }
        b->p[b->n] = 0;
    }
}

/* a document; *from: the reader it is meant for; *hostile: it may take long */
static void gen_doc(rng *r, buf *b, const char **from, int *hostile)
{
    size_t i = below(r, nreaders);
    *from = readers[i];
    *hostile = 0;
    size_t roll = below(r, 100);
    if (roll < 35) mutate(r, b, &seeds[i]);
    else if (roll < 50) bput(b, seeds[i].p, seeds[i].n);
    else if (roll < 65) gen_bytes(r, b, upto(r, 4096));
    else if (roll < 71) { *from = gen_hostile(r, b); *hostile = 1; }
    else if (roll < 81) *from = gen_canary(r, b);
    else if (roll < 91) { mutate(r, b, &seeds[i]); const bytes *s2 = &seeds[below(r, nreaders)]; bput(b, s2->p, s2->n); }
    else if (roll < 95) bstr(b, "");
    else mutate(r, b, &seeds[0]);
    if (!b->p) bstr(b, "");
}

/* ---- options ---- */

typedef struct {
    int untrusted;  /* "untrusted": true, alone */
    double timeout; /* a valid time limit, else 0 */
    int slow_ok;    /* Lua or callbacks: no time limit applies */
    int json;       /* the output must be JSON */
} optinfo;

static void gen_filters(rng *r, buf *b, size_t nfilters, optinfo *oi)
{
    bstr(b, "[");
    for (size_t k = 1 + below(r, 3), i = 0; i < k; i++) {
        if (i) bstr(b, ",");
        size_t roll = below(r, 10);
        if (roll < 4 && nfilters) {
            bfmt(b, "{\"type\":\"callback\",\"index\":%d}", chance(r, 90) ? (int)below(r, nfilters) : (int)below(r, 5) - 2);
            oi->slow_ok = 1;
        } else if (roll < 5) bstr(b, "\"citeproc\"");
        else if (roll < 7) {
            bfmt(b, "\"%s/lua/%s\"", root, PICK(r, ((const char *[]){"id.lua", "err.lua", "bad.lua", "big.lua", "read.lua"})));
            oi->slow_ok = 1;
        } else if (roll < 8) bstr(b, PICK(r, ((const char *[]){"\"/bin/cat\"", "\"/bin/false\"", "{\"type\":\"json\",\"path\":\"/bin/cat\"}"})));
        else if (roll < 9) bstr(b, PICK(r, ((const char *[]){"\"nonexistent.lua\"", "\"nonexistent\"", "{\"type\":\"callback\"}",
            "{\"type\":\"callback\",\"index\":\"0\"}", "{\"type\":\"lua\"}", "42", "null", "[]"})));
        else gen_value(r, b, "s", 0);
    }
    bstr(b, "]");
}

/* call: 0 pandoc_convert, 1 pandoc_read_many */
static void gen_options(rng *r, buf *b, int call, const char *from, int hostile, size_t nfilters, optinfo *oi)
{
    memset(oi, 0, sizeof *oi);
    if (chance(r, 2)) {
        if (chance(r, 50)) gen_bytes(r, b, upto(r, 64));
        else bstr(b, PICK(r, ((const char *[]){"", "[]", "null", "\"x\"", "{", "{\"from\":", "{}}", "{\"from\":\"markdown\" \"to\":1}"})));
        return;
    }
    const char *seen[64];
    size_t nseen = 0;
    int to_json = 0, dup = 0;
    bstr(b, "{");
#define KEY(k) do { if (b->n > 1) bstr(b, ","); bfmt(b, "\"%s\":", (k)); seen[nseen++] = (k); } while (0)
    if (chance(r, 90)) {
        KEY("from");
        if (chance(r, 80)) { buf t = {0}; jstr(r, &t, from, strlen(from)); bstr(b, bget(&t)); free(t.p); }
        else gen_format(r, b, 0);
    }
    if (call == 0 && chance(r, 90)) {
        KEY("to");
        const char *to = writers[below(r, nwriters)];
        if (!strcmp(to, "pdf") && !untrusted_only) to = "json";
        if (chance(r, 80)) { bfmt(b, "\"%s\"", to); to_json = !strcmp(to, "json"); }
        else gen_format(r, b, 1);
    }
    for (size_t k = upto(r, 8); k; k--) {
        if (chance(r, 4)) { KEY("zz-unknown"); gen_value(r, b, "s", 0); continue; }
        const key *kk = &keys[below(r, nkeys)];
        int again = 0;
        for (size_t j = 0; j < nseen; j++) again |= !strcmp(seen[j], kk->name);
        if (again && !chance(r, 5)) continue;
        if (!strcmp(kk->name, "filters") && call == 0 && !hostile && !untrusted_only) {
            KEY("filters");
            gen_filters(r, b, nfilters, oi);
            continue;
        }
        if (!strcmp(kk->name, "filters") || nseen >= 60) continue;
        dup |= again;
        KEY(kk->name);
        /* (writer: another name for to) */
        if (!strcmp(kk->name, "output-file") || !strcmp(kk->name, "dump-args") || !strcmp(kk->name, "writer"))
            to_json = 0;
        gen_value(r, b, kk->type, 0);
    }
    if (call == 0 && hostile && chance(r, 30)) { KEY("filters"); bstr(b, "[\"citeproc\"]"); }
    if (hostile || chance(r, 90)) {
        oi->timeout = (1 + below(r, 30)) / 10.0;
        KEY("timeout");
        bfmt(b, "%g", oi->timeout);
    } else if (chance(r, 50)) {
        KEY("timeout");
        gen_value(r, b, "n", 0);
    }
    if (untrusted_only || chance(r, 30)) {
        KEY("untrusted");
        bstr(b, "true");
        oi->untrusted = !dup;
    } else if (chance(r, 5)) {
        KEY("untrusted");
        gen_value(r, b, "b", 0);
    }
    bstr(b, "}");
    oi->json = to_json && !dup;
}

/* ---- calls ---- */

typedef struct { rng r; int depth; } cbstate;

static int callback(void *ud, const char *doc, size_t dl, const char *ctx, size_t cl, pandoc_buffer *out)
{
    cbstate *c = ud;
    rng *r = &c->r;
    if (!json_ok(doc, dl)) violation("callback: the document isn't JSON");
    if (!json_ok(ctx, cl)) violation("callback: the context isn't JSON: %.*s", (int)(cl > 300 ? 300 : cl), ctx);
    buf b = {0};
    int ret = 0;
    switch (below(r, 14)) {
    case 0: case 1: case 2: case 3: pandoc_buffer_set(out, doc, dl); break;
    case 4: pandoc_buffer_set(out, doc, below(r, dl + 1)); break;
    case 5: gen_bytes(r, &b, upto(r, 1 << 16)); pandoc_buffer_set(out, bget(&b), b.n); ret = chance(r, 50); break;
    case 6:
        bstr(&b, "{\"pandoc-api-version\":[1,23,1],\"meta\":{},\"blocks\":[{\"t\":\"Para\",\"c\":[{\"t\":\"Str\",\"c\":\"");
        bfill(&b, 'x', upto(r, 1 << 24));
        bstr(&b, "\"}]}]}");
        pandoc_buffer_set(out, b.p, b.n);
        break;
    case 7: { size_t n = upto(r, 1 << 20); bfill(&b, '[', n); bfill(&b, ']', n); pandoc_buffer_set(out, b.p, b.n); break; }
    case 8: pandoc_buffer_set(out, "error \xff\xfe message", 17); ret = 1; break;
    case 9: ret = (int)below(r, 3) - 1; if (!ret) ret = 1; break;
    case 10: break; /* success, and nothing set */
    case 11: {
        const char *s = PICK(r, ((const char *[]){"null", "[]", "{}", "{\"pandoc-api-version\":[9,9],\"meta\":{},\"blocks\":[]}",
            "{\"pandoc-api-version\":[1,23,1],\"meta\":{},\"blocks\":[{\"t\":\"Nope\"}]}"}));
        pandoc_buffer_set(out, s, strlen(s));
        break; }
    default:
        /* calls pandoc again, then answers (twice: the last wins) */
        pandoc_buffer_set(out, "garbage", 7);
        if (c->depth < 1) {
            const char *o = chance(r, 50) ? "{\"from\":\"markdown\",\"to\":\"json\",\"timeout\":5}" : "{\"query\":\"version\"}";
            pandoc_result *n = o[2] == 'f' ? convert_str(o, "*nested*", 8) : pandoc_query(o, strlen(o));
            check(n, "nested", 0, 1);
            pandoc_result_free(n);
        }
        pandoc_buffer_set(out, doc, dl);
    }
    free(b.p);
    return ret;
}

static void dumpfile(const char *what, int n, const char *s, size_t len)
{
    char p[64];
    snprintf(p, sizeof p, "%ld-%d.%s", iteration, n, what);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(s, 1, len, f); fclose(f); }
}

static __thread int ncall;

static void do_convert(rng *r)
{
    buf doc = {0}, opts = {0};
    const char *from;
    int hostile;
    gen_doc(r, &doc, &from, &hostile);
    size_t nf = untrusted_only || hostile ? 0 : below(r, 4);
    optinfo oi;
    gen_options(r, &opts, 0, from, hostile, nf, &oi);
    int no_input = !hostile && chance(r, 3);
    cbstate cb[4];
    pandoc_filter filters[4];
    for (size_t i = 0; i < nf; i++) {
        cb[i] = (cbstate){{rnext(r)}, 0};
        filters[i] = (pandoc_filter){chance(r, 3) ? NULL : callback, &cb[i]};
    }
    int with_filters = nf || chance(r, 20);
    if (dump) {
        dumpfile("options.json", ncall, bget(&opts), opts.n);
        if (!no_input) dumpfile("input", ncall, bget(&doc), doc.n);
        ncall++;
        free(doc.p); free(opts.p);
        return;
    }
    double t0 = now();
    pandoc_result *res = with_filters
        ? pandoc_convert_filters(bget(&opts), opts.n, no_input ? NULL : bget(&doc), doc.n, filters, nf)
        : pandoc_convert(bget(&opts), opts.n, no_input ? NULL : bget(&doc), doc.n);
    double dt = now() - t0;
    check(res, "pandoc_convert", oi.untrusted, oi.json);
    if (oi.timeout && !oi.slow_ok && dt > oi.timeout + 30)
        violation("pandoc_convert took %.1f s with a time limit of %g s", dt, oi.timeout);
    pandoc_result_free(res);
    free(doc.p);
    free(opts.p);
}

static void do_read_many(rng *r)
{
    buf req = {0}, opts = {0};
    const char *from;
    int hostile;
    buf first = {0};
    gen_doc(r, &first, &from, &hostile);
    optinfo oi;
    gen_options(r, &opts, 1, from, hostile, 0, &oi);
    size_t n = chance(r, 5) ? 1000 : upto(r, 64);
    if (chance(r, 3)) bstr(&req, PICK(r, ((const char *[]){"[]", "{}", "{\"inputs\":3}", "{\"inputs\":[]}", "{\"options\":[]}", "{"})));
    else {
        bstr(&req, "{");
        if (untrusted_only || chance(r, 95)) { bstr(&req, "\"options\":"); bput(&req, bget(&opts), opts.n); bstr(&req, ","); }
        else oi.untrusted = 0, oi.timeout = 0;
        bstr(&req, "\"inputs\":[");
        for (size_t i = 0; i < n; i++) {
            if (i) bstr(&req, ",");
            if (chance(r, 3)) { gen_value(r, &req, "o", 2); continue; }
            buf d = {0};
            const char *f2;
            int h2;
            if (i == 0) bput(&d, bget(&first), hostile && !oi.timeout ? 0 : first.n);
            else if (n > 100 || chance(r, 50)) gen_bytes(r, &d, upto(r, 40));
            else { gen_doc(r, &d, &f2, &h2); if (h2 && !oi.timeout) d.n = 0; }
            if (!oi.timeout && d.n > 512) d.n = 512; /* (as in do_args) */
            jstr(r, &req, bget(&d), d.n);
            free(d.p);
        }
        bstr(&req, "]}");
    }
    if (dump) { dumpfile("read_many.json", ncall++, bget(&req), req.n); goto out; }
    double t0 = now();
    pandoc_result *res = pandoc_read_many(bget(&req), req.n);
    double dt = now() - t0;
    check(res, "pandoc_read_many", oi.untrusted, 1);
    if (oi.timeout && dt > oi.timeout + 30)
        violation("pandoc_read_many took %.1f s with a time limit of %g s", dt, oi.timeout);
    pandoc_result_free(res);
out:
    free(req.p); free(opts.p); free(first.p);
}

static void do_query(rng *r)
{
    static const char *names[] = {"version", "api-version", "input-formats", "output-formats",
        "highlight-languages", "highlight-styles", "extensions-for-format", "default-template",
        "num-threads", "parse-args"};
    buf q = {0};
    int untrusted = 0;
    if (chance(r, 3)) gen_bytes(r, &q, upto(r, 32));
    else {
        bstr(&q, "{\"query\":");
        if (chance(r, 90)) bfmt(&q, "\"%s\"", PICK(r, names)); else gen_value(r, &q, "s", 0);
        if (chance(r, 50)) { bstr(&q, ",\"format\":"); gen_value(r, &q, "f", 0); }
        if (chance(r, 30)) {
            bstr(&q, ",\"args\":[");
            for (size_t k = below(r, 5), i = 0; i < k; i++) {
                if (i) bstr(&q, ",");
                if (chance(r, 50)) bfmt(&q, "\"--%s\"", keys[below(r, nkeys)].name); else gen_value(r, &q, "s", 0);
            }
            bstr(&q, "]");
        }
        if (chance(r, 5)) { bstr(&q, ",\"zz\":"); gen_value(r, &q, "o", 0); }
        if (untrusted_only || chance(r, 30)) { bstr(&q, ",\"untrusted\":true"); untrusted = 1; }
        bstr(&q, "}");
    }
    if (dump) { dumpfile("query.json", ncall++, bget(&q), q.n); free(q.p); return; }
    pandoc_result *res = pandoc_query(bget(&q), q.n);
    check(res, "pandoc_query", untrusted, 1);
    pandoc_result_free(res);
    free(q.p);
}

/* a command line: pandoc_convert_args (main = 0) or pandoc_main */
static void do_args(rng *r, int main)
{
    char *argv[16];
    int argc = 0;
    buf t = {0};
    if (main) argv[argc++] = need(strdup(chance(r, 95) ? "pandoc" : "pandoc-lua"));
    if (main && chance(r, 5)) argv[argc++] = need(strdup(chance(r, 70) ? "lua" : "server"));
    for (size_t k = below(r, 8); k && argc < 14; k--) {
        t.n = 0;
        switch (below(r, 10)) {
        case 0: bfmt(&t, "--from=%s", readers[below(r, nreaders)]); break;
        case 1: { const char *w = writers[below(r, nwriters)]; bfmt(&t, "--to=%s", strcmp(w, "pdf") ? w : "html"); break; }
        case 2: bfmt(&t, "--%s", keys[below(r, nkeys)].name); break;
        case 3: case 4: {
            const key *kk = &keys[below(r, nkeys)];
            if (kk->type[0] == 'w' || !strcmp(kk->name, "to") || !strcmp(kk->name, "pdf-engine")) { bfmt(&t, "--%s=out.txt", kk->name); break; }
            buf v = {0};
            gen_string(r, &v, kk->type[0] == 'f' ? 'f' : 's');
            /* the JSON string, unquoted (escapes and all) */
            bfmt(&t, "--%s=%.*s", kk->name, (int)(v.n > 2 ? v.n - 2 : 0), v.n > 2 ? v.p + 1 : "");
            free(v.p);
            break; }
        case 5: bstr(&t, paths_in[below(r, npaths_in)]); break;
        case 6: bstr(&t, PICK(r, ((const char *[]){"-o", "out.txt", "-s", "--toc", "--sandbox", "--", "-", "--version",
            "--help", "--list-input-formats", "-D", "html", "--print-default-data-file=reference.docx", "--citeproc",
            "--verbose", "--fail-if-warnings", "--wrap", "--wrap=none"}))); break;
        case 7: gen_bytes(r, &t, 1 + upto(r, 16)); for (size_t i = 0; i < t.n; i++) if (!t.p[i]) t.p[i] = 'x'; break;
        case 8: bfmt(&t, "--lua-filter=%s/lua/%s", root, PICK(r, ((const char *[]){"id.lua", "err.lua", "bad.lua", "big.lua"}))); break;
        default: bfmt(&t, "--lua-filter=libpandoc:callback/%d", (int)below(r, 3)); break;
        }
        argv[argc++] = need(strdup(bget(&t)));
    }
    argv[argc] = NULL;
    buf doc = {0};
    const char *from;
    int hostile;
    gen_doc(r, &doc, &from, &hostile);
    /* no time limit here: pandoc's markdown reader can take minutes on a
     * few kB of random bytes */
    if (hostile) doc.n = 0;
    if (doc.n > 512) doc.n = 512;
    cbstate cb[2] = {{{rnext(r)}, 0}, {{rnext(r)}, 0}};
    pandoc_filter filters[2] = {{callback, &cb[0]}, {callback, &cb[1]}};
    size_t nf = below(r, 3);
    int no_input = chance(r, 10);
    if (dump) {
        buf a = {0};
        for (int i = 0; i < argc; i++) { bstr(&a, argv[i]); bstr(&a, "\n"); }
        dumpfile(main ? "main.args" : "args", ncall, bget(&a), a.n);
        if (!main) dumpfile("input", ncall, bget(&doc), doc.n);
        ncall++;
        free(a.p);
    } else {
        if (main) {
            int st = pandoc_main(argc, (const char *const *)argv, NULL, 0, filters, nf);
            __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
            if (st < 0 || st > 255) violation("pandoc_main returned %d", st);
        } else {
            pandoc_result *res = pandoc_convert_args_filters(argc, (const char *const *)argv,
                no_input ? NULL : bget(&doc), doc.n, filters, nf);
            check(res, "pandoc_convert_args", 0, 0);
            pandoc_result_free(res);
        }
    }
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(t.p);
    free(doc.p);
}

static void do_threads_setting(rng *r)
{
    int n = chance(r, 80) ? 1 + (int)below(r, 4) : (int)below(r, 2000) - 1000;
    if (dump) return;
    int got = pandoc_set_num_threads(n);
    if (got < 1 || got > ncpu) violation("pandoc_set_num_threads(%d) returned %d", n, got);
    const char *q = "{\"query\":\"num-threads\"}";
    pandoc_result *res = pandoc_query(q, strlen(q));
    check(res, "num-threads", 0, 1);
    if (res && !res->status && (atoi(res->output) < 1 || atoi(res->output) > ncpu))
        violation("num-threads is %s", res->output);
    pandoc_result_free(res);
}

static void one_call(rng *r, int threaded)
{
    size_t roll = below(r, 100);
    if (roll < 55) do_convert(r);
    else if (roll < 72) do_read_many(r);
    else if (roll < 85) do_query(r);
    else if (roll < 89 || untrusted_only) do_threads_setting(r);
    else if (roll < 96) do_args(r, 0);
    else if (!threaded) do_args(r, 1);
}

typedef struct { uint64_t seed; long iter; } thread_arg;

static void *thread_main(void *p)
{
    thread_arg *a = p;
    iteration = a->iter;
    rng r = {a->seed};
    for (size_t k = 1 + below(&r, 3); k; k--) one_call(&r, 1);
    return NULL;
}

static void run(uint64_t seed, long k)
{
    iteration = k;
    ncall = 0;
    rng r = {seed * 0x2545f4914f6cdd1dULL ^ (uint64_t)k * 0x9e3779b97f4a7c15ULL};
    rnext(&r);
    if (!chance(&r, 10)) { one_call(&r, 0); return; }
    size_t n = 2 + below(&r, 5);
    pthread_t t[8];
    thread_arg a[8];
    for (size_t i = 0; i < n; i++) a[i] = (thread_arg){rnext(&r), k};
    if (dump) { /* the threads' calls, one after another */
        for (size_t i = 0; i < n; i++) thread_main(&a[i]);
        return;
    }
    for (size_t i = 0; i < n; i++)
        if (pthread_create(&t[i], NULL, thread_main, &a[i])) { n = i; break; }
    for (size_t i = 0; i < n; i++) pthread_join(t[i], NULL);
}

/* An iteration taking this long is a hang: its number is the last "iter"
 * on standard error. */
#define HANG_SECONDS 90
static volatile double iteration_start;

static void *watchdog(void *p)
{
    (void)p;
    for (;;) {
        sleep(1);
        if (iteration_start && now() - iteration_start > HANG_SECONDS) {
            fprintf(stderr, "HANG: an iteration ran over %d s\n", HANG_SECONDS);
            _exit(4);
        }
    }
    return NULL;
}

static size_t count_dir(const char *path, const char *prefix)
{
    DIR *d = opendir(path);
    size_t n = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.' && !strncmp(e->d_name, prefix, strlen(prefix))) n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u")) untrusted_only = 1;
        else if (!strcmp(argv[i], "-D")) dump = 1;
        else break;
    }
    if (argc - i < 2) {
        fputs("usage: fuzz [-u] [-D] SEED COUNT [FIRST]\n", stderr);
        return 2;
    }
    uint64_t seed = strtoull(argv[i], NULL, 10);
    long count = atol(argv[i + 1]), first = argc - i > 2 ? atol(argv[i + 2]) : 0;
    setup();
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    size_t tmp0 = count_dir(tmp, "libpandoc-");
    pthread_t w;
    if (!dump) pthread_create(&w, NULL, watchdog, NULL);
    for (long k = first; k < first + count; k++) {
        fprintf(stderr, "iter %ld\n", k);
        if (untrusted_only) { /* for fuzz/run.sh's trace */
            char m[64];
            snprintf(m, sizeof m, "fuzz-iter-%ld", k);
            access(m, F_OK);
        }
        iteration_start = now();
        run(seed, k);
        iteration_start = 0;
        if (dump) break;
    }
    iteration = -1;
    /* every call's temporary directory removed; descriptors not leaking (the
     * runtime opens some per thread it grows to) */
    if (!dump && count_dir(tmp, "libpandoc-") > tmp0)
        violation("%zu temporary directories left in %s", count_dir(tmp, "libpandoc-") - tmp0, tmp);
    size_t fds = count_dir("/proc/self/fd", "");
    if (fds > (size_t)(64 + 4 * ncpu)) violation("%zu file descriptors open at the end", fds);
    fprintf(stderr, "calls %ld\n", calls);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) return 2;
    return 0;
}
