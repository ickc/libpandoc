/* "untrusted": true reads nothing of the user data directory: templates
 * and their partials, reference documents, abbreviations, translations,
 * CSL styles, dzslides' template come from pandoc's own data. Canaries in
 * the user data directory (U) and in a directory of PANDOC_DATA_DIRS (P),
 * files this test writes (and removes) under the current directory.
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
    return failures != 0;
}
