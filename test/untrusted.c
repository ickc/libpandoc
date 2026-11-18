/* "untrusted": true reads nothing of the user data directory: templates
 * and their partials, reference documents, abbreviations, translations,
 * CSL styles, dzslides' template come from pandoc's own data. Canaries in
 * the user data directory (U) and in a directory of PANDOC_DATA_DIRS (P),
 * files this test writes (and removes) under the current directory.
 *
 * Also (1.14): "untrusted" as an object, which is untrusted too, and
 * malformed values, which are errors (never trusted); and citeproc's files
 * given as their contents ("citeproc-files"), with canary files of the
 * same and other names on the disk (ut-cf/), which are never read.
 *
 *   cc test/untrusted.c -Idist/include -Ldist/lib -lpandoc && ./a.out
 *
 * PANDOC_DATA_DIRS (pandoc 3.14) is honoured by design: it is the host's
 * own configuration, not the untrusted options'. pandoc reads from it the
 * template, its partials, dzslides' template and the abbreviations (before
 * its sandbox), not reference documents or translations (in its sandbox;
 * citeproc in libpandoc's). The test shows which, failing on neither (the
 * header says so).
 *
 * Not on Windows: the user data directory, which is the system's
 * %APPDATA%\pandoc there (XDG_DATA_HOME, which the test points elsewhere,
 * is for the other systems). PANDOC_DATA_DIRS everywhere. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpandoc.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(d) _mkdir(d)
#define get_cwd _getcwd
#define remove_dir _rmdir
#define SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(d) mkdir(d, 0755)
#define get_cwd getcwd
#define remove_dir rmdir
#define SEP "/"
#endif

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void note(int leaked, const char *what)
{
    printf("      PANDOC_DATA_DIRS's %s: %s\n", what, leaked ? "read (honoured by design)" : "not read");
}

static int has(const char *s, const char *what) { return s && strstr(s, what); }

static void show(const pandoc_result *r)
{
    if (r && r->error_kind) printf("      %s: %.200s\n", r->error_kind, r->error_message);
}

/* the data directories, U (if any) and P */
static char udir[1024], pdir[1024];
static int with_u;

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* dir/name := text, for each data directory, with "U" or "P" for '@' */
static void put(const char *name, const char *text)
{
    for (int k = 0; k < 2; k++) {
        if (k == 0 && !with_u) continue;
        char path[2048], t[512];
        snprintf(path, sizeof path, "%s" SEP "%s", k == 0 ? udir : pdir, name);
        snprintf(t, sizeof t, "%s", text);
        for (char *c = t; *c; c++) if (*c == '@') *c = k == 0 ? 'U' : 'P';
        write_file(path, t);
    }
}

static void drop(const char *name)
{
    for (int k = 0; k < 2; k++) {
        if (k == 0 && !with_u) continue;
        char path[2048];
        snprintf(path, sizeof path, "%s" SEP "%s", k == 0 ? udir : pdir, name);
        remove(path);
    }
}

static pandoc_result *convert(const char *o, const char *in)
{
    return pandoc_convert(o, strlen(o), in, strlen(in));
}

static const char *subdirs[] = {"templates", "csl", "translations", "dzslides", "filters", "defaults"};
#define NSUB (sizeof subdirs / sizeof subdirs[0])

static void tree(int make)
{
    for (int k = 0; k < 2; k++) {
        if (k == 0 && !with_u) continue;
        const char *base = k == 0 ? udir : pdir;
        if (make) make_dir(base);
        for (size_t i = 0; i < NSUB; i++) {
            char path[2048];
            snprintf(path, sizeof path, "%s" SEP "%s", base, subdirs[i]);
            if (make) make_dir(path); else remove_dir(path);
        }
        if (!make) remove_dir(base);
    }
}

#ifdef _WIN32
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

/* "untrusted" (1.14): an object is untrusted too; anything but true, false
 * or an object is an error, never trusted. (Its "cancel-token": cancel.c.) */
static void untrusted_values(void)
{
    pandoc_result *r = convert("{\"untrusted\": {}, \"to\": \"html\"}", "*x*");
    check(r && r->status == 0 && strcmp(r->output, "<p><em>x</em></p>\n") == 0,
          "\"untrusted\": {} converts");
    pandoc_result_free(r);
    r = convert("{\"untrusted\": {}, \"template\": \"t.html\"}", "x");
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
          && has(r->error_message, "not allowed for untrusted code: template"),
          "\"untrusted\": {} is untrusted: it refuses a template");
    show(r);
    pandoc_result_free(r);
    r = convert("{\"untrusted\": {\"sandbox\": false}}", "x");
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
          && has(r->error_message, "unknown field \"sandbox\""),
          "\"untrusted\": an unknown field is an error naming it");
    show(r);
    pandoc_result_free(r);
    const char *bad[] = {"\"yes\"", "\"true\"", "1", "0", "null", "[]", "[true]"};
    int all = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char o[128];
        snprintf(o, sizeof o, "{\"untrusted\": %s, \"template\": \"t.html\"}", bad[i]);
        r = convert(o, "x");
        all = all && r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
              && has(r->error_message, "\"untrusted\" is true, false or an object");
        pandoc_result_free(r);
        snprintf(o, sizeof o, "{\"options\": {\"untrusted\": %s}, \"inputs\": [\"a\"]}", bad[i]);
        r = pandoc_read_many(o, strlen(o));
        all = all && r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0;
        pandoc_result_free(r);
        snprintf(o, sizeof o, "{\"query\": \"default-template\", \"format\": \"html\", \"untrusted\": %s}", bad[i]);
        r = pandoc_query(o, strlen(o));
        all = all && r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0;
        pandoc_result_free(r);
    }
    check(all, "\"untrusted\" of another type: an error in convert, read_many and query");
    const char *rq = "{\"options\": {\"untrusted\": {}, \"data-dir\": \".\"}, \"inputs\": [\"a\"]}";
    r = pandoc_read_many(rq, strlen(rq));
    check(r && r->status != 0 && has(r->error_message, "not allowed for untrusted code: data-dir"),
          "read_many: \"untrusted\": {} refuses data-dir");
    pandoc_result_free(r);
    struct { const char *q; int ok; const char *message; } qs[] = {
        {"{\"query\": \"version\", \"untrusted\": {}}", 1, NULL},
        {"{\"query\": \"default-template\", \"format\": \"html\", \"untrusted\": {}}", 0,
         "query not allowed for untrusted code"},
        {"{\"query\": \"version\", \"untrusted\": {\"cancel-token\": 1}}", 0, "takes no \"cancel-token\""},
        {"{\"query\": \"version\", \"untrusted\": {\"x\": 1}}", 0, "unknown field \"x\""},
        {"{\"query\": \"default-template\", \"format\": \"html\", \"untrusted\": false}", 1, NULL},
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        r = pandoc_query(qs[i].q, strlen(qs[i].q));
        check(r && (qs[i].ok ? r->status == 0
                             : r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
                               && has(r->error_message, qs[i].message)), qs[i].q);
        show(r);
        pandoc_result_free(r);
    }
}

/* citeproc's files given as their contents ("citeproc-files", 1.14) */
#define CF_BIB "@article{k, title={Givenness}, author={Au, Thor}, year={1999}, journal={Long Journal Name}}"
#define CF_CSL "<?xml version='1.0' encoding='utf-8'?><style xmlns='http://purl.org/net/xbiblio/csl'" \
    " class='in-text' version='1.0'><info><title>T</title><id>t</id>" \
    "<updated>2020-01-01T00:00:00+00:00</updated></info><citation><layout prefix='[STYLE ' suffix=']'>" \
    "<group delimiter=', '><text variable='title'/><text variable='container-title' form='short'/>" \
    "</group></layout></citation></style>"
#define CF_ABBR "{\\\"default\\\": {\\\"container-title\\\": {\\\"Long Journal Name\\\": \\\"LJN\\\"}}}"
#define CF_OPTS "{\"untrusted\": true, \"to\": \"plain\", \"citeproc\": true, \"citeproc-files\": "
/* the canary: files on the disk, of names a document may give */
#define CF_MARK "CANARY-CITEPROC-FILES-5e1b"

static int clean(const pandoc_result *r)
{
    return r && !has(r->output, CF_MARK) && !has(r->error_message, CF_MARK) && !has(r->log, CF_MARK);
}

static void citeproc_files(const char *cwd)
{
    make_dir("ut-cf");
    write_file("ut-cf" SEP "refs.bib", "@article{k, title={" CF_MARK "-BIB}, author={Canary, C}, year={1900},"
               " journal={Long Journal Name}}\n");
    write_file("ut-cf" SEP "style.csl", "<?xml version='1.0' encoding='utf-8'?><style"
               " xmlns='http://purl.org/net/xbiblio/csl' class='in-text' version='1.0'><info><title>C</title>"
               "<id>c</id><updated>2020-01-01T00:00:00+00:00</updated></info><citation><layout>"
               "<text value='" CF_MARK "-CSL'/></layout></citation></style>\n");
    write_file("ut-cf" SEP "abbr.json", "{\"default\": {\"container-title\": {\"Long Journal Name\": \""
               CF_MARK "-ABBR\"}}}\n");
    char abs_doc[1200];
    snprintf(abs_doc, sizeof abs_doc, "---\nbibliography: \"%s/ut-cf/refs.bib\"\n---\n[@k]\n", cwd);
    for (char *p = abs_doc; *p; p++) if (*p == '\\') *p = '/';

    struct { const char *what, *given, *doc; const char *from; const char *expect; } cases[] = {
        /* given and named: rendered */
        {"bibliography, CSL style and abbreviations given, named in the metadata: rendered",
         "{\"refs.bib\": \"" CF_BIB "\", \"style.csl\": \"" CF_CSL "\", \"abbr.json\": \"" CF_ABBR "\"}",
         "---\nbibliography: ./refs.bib\ncsl: style\ncitation-abbreviations: abbr\n---\n[@k]\n", NULL,
         "[STYLE Givenness, LJN]"},
        {"... csl and citation-abbreviations named with their extensions",
         "{\"refs.bib\": \"" CF_BIB "\", \"style.csl\": \"" CF_CSL "\", \"abbr.json\": \"" CF_ABBR "\"}",
         "---\nbibliography: refs.bib\ncsl: style.csl\ncitation-abbreviations: abbr.json\n---\n[@k]\n", NULL,
         "[STYLE Givenness, LJN]"},
        {"a list of bibliographies, both given",
         "{\"a.bib\": \"" CF_BIB "\", \"b.bib\": \"@book{j, title={Second}, year={2001}}\"}",
         "---\nbibliography: [a.bib, b.bib]\n---\n[@k; @j]\n", NULL, "Second"},
        {"LaTeX's \\bibliography{refs}: refs.bib", "{\"refs.bib\": \"" CF_BIB "\"}",
         "\\bibliography{refs}\\cite{k}", "latex", "Givenness"},
        {"a subdirectory's name, as the document gives it", "{\"sub/refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: sub/refs.bib\n---\n[@k]\n", NULL, "Givenness"},
        /* a name the disk has (the canary) given: the given contents */
        {"a name the disk has too: the given contents", "{\"ut-cf/refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: ut-cf/refs.bib\n---\n[@k]\n", NULL, "Givenness"},
        /* names not given: missing, the files on the disk not read */
        {"a bibliography not given, on the disk: missing", "{\"refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: ut-cf/refs.bib\n---\n[@k]\n", NULL, NULL},
        {"... named by its absolute path: missing", "{\"refs.bib\": \"" CF_BIB "\"}", abs_doc, NULL, NULL},
        {"... through ..: missing", "{\"refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: ut-cf/../ut-cf/refs.bib\n---\n[@k]\n", NULL, NULL},
        {"... one of a list: missing", "{\"refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: [refs.bib, ut-cf/refs.bib]\n---\n[@k]\n", NULL, NULL},
        {"... LaTeX's \\bibliography: missing", "{\"refs.bib\": \"" CF_BIB "\"}",
         "\\bibliography{ut-cf/refs}\\cite{k}", "latex", NULL},
        {"a CSL style not given, on the disk: missing", "{\"refs.bib\": \"" CF_BIB "\"}",
         "---\nbibliography: refs.bib\ncsl: ut-cf/style\n---\n[@k]\n", NULL, NULL},
        {"abbreviations not given, on the disk: missing",
         "{\"refs.bib\": \"" CF_BIB "\", \"style.csl\": \"" CF_CSL "\"}",
         "---\nbibliography: refs.bib\ncsl: style.csl\ncitation-abbreviations: ut-cf/abbr.json\n---\n[@k]\n",
         NULL, NULL},
        {"a name given only without its extension: missing (pandoc looks for style.csl)",
         "{\"refs.bib\": \"" CF_BIB "\", \"style\": \"" CF_CSL "\"}",
         "---\nbibliography: refs.bib\ncsl: style\n---\n[@k]\n", NULL, NULL},
        {"a name on neither: missing", "{}", "---\nbibliography: nowhere.bib\n---\n[@k]\n", NULL, NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char o[4096];
        snprintf(o, sizeof o, CF_OPTS "%s%s%s%s}", cases[i].given, cases[i].from ? ", \"from\": \"" : "",
                 cases[i].from ? cases[i].from : "", cases[i].from ? "\"" : "");
        pandoc_result *r = convert(o, cases[i].doc);
        int ok = clean(r) && (cases[i].expect
            ? r->status == 0 && has(r->output, cases[i].expect)
            : r->status != 0 && strcmp(r->error_kind, "PandocResourceNotFound") == 0);
        char what[300];
        snprintf(what, sizeof what, "citeproc-files: %s", cases[i].what);
        check(ok, what);
        if (!ok && r) printf("      %s\n", r->status == 0 ? r->output : r->error_message);
        pandoc_result_free(r);
    }

    /* the canary shows when it is read: trusted, without the sandbox */
    {
        pandoc_result *r = convert("{\"to\": \"plain\", \"citeproc\": true}",
                                   "---\nbibliography: ut-cf/refs.bib\n---\n[@k]\n");
        check(r && r->status == 0 && has(r->output, CF_MARK "-BIB"),
              "citeproc-files: (the canary shows when read: trusted, without the sandbox)");
        pandoc_result_free(r);
    }

    /* malformed: errors */
    const char *bad[] = {
        CF_OPTS "[]}", CF_OPTS "\"refs.bib\"}", CF_OPTS "{\"refs.bib\": 1}}", CF_OPTS "{\"refs.bib\": null}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        pandoc_result *r = convert(bad[i], "x");
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
              && has(r->error_message, "citeproc-files"), bad[i]);
        pandoc_result_free(r);
    }
    /* not in read_many (readers run no citeproc) */
    const char *rq = "{\"options\": {\"untrusted\": true, \"citeproc-files\": {}}, \"inputs\": [\"a\"]}";
    pandoc_result *r = pandoc_read_many(rq, strlen(rq));
    check(r && r->status != 0 && has(r->error_message, "not allowed for untrusted code: citeproc-files"),
          "citeproc-files: read_many refuses it");
    pandoc_result_free(r);
    /* the options naming files stay refused */
    r = convert(CF_OPTS "{}, \"bibliography\": [\"refs.bib\"]}", "x");
    check(r && r->status != 0 && has(r->error_message, "not allowed for untrusted code: bibliography"),
          "citeproc-files: the option bibliography is still refused");
    pandoc_result_free(r);

    remove("ut-cf" SEP "refs.bib");
    remove("ut-cf" SEP "style.csl");
    remove("ut-cf" SEP "abbr.json");
    remove_dir("ut-cf");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pandoc_result *r;
    char cwd[900];
    if (!get_cwd(cwd, sizeof cwd)) return 2;
    /* absolute: pandoc ignores relative ones */
    snprintf(pdir, sizeof pdir, "%s" SEP "ut-pdd", cwd);
    set_env("PANDOC_DATA_DIRS", pdir);
#ifndef _WIN32
    with_u = 1;
    char xdg[960];
    snprintf(xdg, sizeof xdg, "%s/ut-xdg", cwd);
    snprintf(udir, sizeof udir, "%s/pandoc", xdg);
    set_env("XDG_DATA_HOME", xdg);
    make_dir(xdg);
#endif
    tree(1);

    const char *html = "{\"untrusted\": true, \"to\": \"html5\", \"standalone\": true}";

    /* the template */
    put("templates/default.html5", "CANARY-@-TEMPLATE $body$");
    r = convert(html, "x");
    check(r && r->status == 0 && !has(r->output, "CANARY-U")
          && (has(r->output, "<html") || has(r->output, "CANARY-P")),
          "the template: pandoc's own");
    note(r && has(r->output, "CANARY-P"), "template");
    pandoc_result_free(r);
    drop("templates/default.html5");

    /* the template's partials */
    put("templates/styles.html", "CANARY-@-PARTIAL");
    r = convert(html, "x");
    check(r && r->status == 0 && !has(r->output, "CANARY-U"), "the template's partials: pandoc's own");
    note(r && has(r->output, "CANARY-P"), "partial");
    pandoc_result_free(r);
    drop("templates/styles.html");

    /* reference documents: not zip files, so reading one fails */
    put("reference.docx", "CANARY-@-NOT-A-DOCX");
    put("reference.odt", "CANARY-@-NOT-AN-ODT");
    r = convert("{\"untrusted\": true, \"to\": \"docx\"}", "x");
    check(r && r->status == 0, "reference.docx: pandoc's own");
    show(r);
    pandoc_result_free(r);
    r = convert("{\"untrusted\": true, \"to\": \"odt\"}", "x");
    check(r && r->status == 0, "reference.odt: pandoc's own");
    show(r);
    pandoc_result_free(r);
    drop("reference.docx");
    drop("reference.odt");

    /* abbreviations: after one, the markdown reader writes a no-break space */
    put("abbreviations", "@canary.\n");
    const char *abbr = "Ucanary. Bird and Pcanary. Bird";
    r = convert("{\"untrusted\": true, \"to\": \"plain\"}", abbr);
    check(r && r->status == 0 && has(r->output, "Ucanary. Bird"), "abbreviations: pandoc's own");
    note(r && has(r->output, "Pcanary.\xc2\xa0" "Bird"), "abbreviations");
    pandoc_result_free(r);
    {
        const char *rq = "{\"options\": {\"untrusted\": true}, "
                         "\"inputs\": [\"Ucanary. Bird and Pcanary. Bird\"]}";
        r = pandoc_read_many(rq, strlen(rq));
        check(r && r->status == 0 && has(r->output, "\"Ucanary.\"},{\"t\":\"Space\""),
              "read_many: abbreviations: pandoc's own");
        note(r && has(r->output, "Pcanary.\xc2\xa0"), "abbreviations, in read_many");
        pandoc_result_free(r);
    }
    drop("abbreviations");

    /* translations */
    put("translations/en.yaml", "Abstract: CANARY-@-ABSTRACT\n");
    put("translations/en-US.yaml", "Abstract: CANARY-@-ABSTRACT\n");
    r = convert(html, "---\ntitle: T\nabstract: summary\n---\nx\n");
    check(r && r->status == 0 && has(r->output, "Abstract") && !has(r->output, "CANARY"),
          "translations: pandoc's own (P's too)");
    if (r && has(r->output, "CANARY")) printf("      %.80s\n", strstr(r->output, "CANARY"));
    pandoc_result_free(r);
    drop("translations/en.yaml");
    drop("translations/en-US.yaml");

    /* CSL styles: not CSL, so reading one fails */
    put("default.csl", "CANARY-@-NOT-CSL");
    put("csl/canary.csl", "CANARY-@-NOT-CSL");
    {
        const char *refs = "---\nreferences:\n- id: k\n  type: book\n  title: T\n"
                           "  author: [{family: F, given: G}]\n  issued: 2000\n---\n[@k]\n";
        const char *cite = "{\"untrusted\": true, \"to\": \"plain\", \"citeproc\": true}";
        r = convert(cite, refs);
        check(r && r->status == 0 && has(r->output, "(F 2000)"), "default.csl: pandoc's own (P's too)");
        show(r);
        pandoc_result_free(r);
        char named[512];
        snprintf(named, sizeof named, "---\ncsl: canary\n%s", refs + 4);
        r = convert(cite, named);
        check(r && r->status != 0 && !has(r->error_message, "CANARY")
              && (strcmp(r->error_kind, "PandocResourceNotFound") == 0
                  || strcmp(r->error_kind, "PandocCouldNotFindDataFileError") == 0),
              "a style named in csl/: not found (P's neither)");
        show(r);
        pandoc_result_free(r);
    }
    drop("default.csl");
    drop("csl/canary.csl");

    /* dzslides' template */
    put("dzslides/template.html", "<!-- {{{{ dzslides core\nCANARY-@-DZSLIDES\n");
    r = convert("{\"untrusted\": true, \"to\": \"dzslides\", \"standalone\": true}", "# x\n\ny\n");
    check(r && r->status == 0 && !has(r->output, "CANARY-U"), "dzslides' template: pandoc's own");
    note(r && has(r->output, "CANARY-P"), "dzslides template");
    pandoc_result_free(r);
    drop("dzslides/template.html");

    /* filters and defaults files: refused, wherever they are */
    put("filters/canary.lua", "error('CANARY')\n");
    put("defaults/canary.yaml", "to: plain\n");
    r = convert("{\"untrusted\": true, \"filters\": [\"canary.lua\"]}", "x");
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0, "filters: refused");
    pandoc_result_free(r);
    r = convert("{\"untrusted\": true, \"defaults\": \"canary\"}", "x");
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
          "defaults files: refused");
    pandoc_result_free(r);
    drop("filters/canary.lua");
    drop("defaults/canary.yaml");

    /* trusted, as pandoc: the user data directory's template is used */
    if (with_u) {
        put("templates/default.html5", "CANARY-@-TEMPLATE $body$");
        r = convert("{\"to\": \"html5\", \"standalone\": true}", "x");
        check(r && r->status == 0 && has(r->output, "CANARY-U-TEMPLATE"),
              "without \"untrusted\": the user data directory, as pandoc");
        pandoc_result_free(r);
        drop("templates/default.html5");
    }

    tree(0);
#ifndef _WIN32
    remove_dir(xdg);
#endif
    untrusted_values();
    citeproc_files(cwd);
    return failures != 0;
}
