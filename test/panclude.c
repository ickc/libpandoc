/* The include step built in ("panclude" among the filters, 1.15): what
 * panclude's conformance cases (test/panclude.mjs) don't cover. The
 * options: the entries' spellings and their errors, the host's roots for
 * untrusted options; the step's place among the filters; and each guarantee
 * the step could have broken: nothing read outside its roots (through ..,
 * an absolute path, a symbolic link), nothing read under pandoc's sandbox
 * or for untrusted options without roots given, nothing read by an
 * included file's own includes (LaTeX's \input), nothing of a file in a
 * message, and a program named panclude not run. The canaries are files
 * this test writes (and removes) under pcl/ in the current directory.
 *
 *   cc test/panclude.c -Idist/include -Ldist/lib -lpandoc && ./a.out
 *
 * Not on Windows: the symbolic links, and the program named panclude. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpandoc.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(d) _mkdir(d)
#define get_cwd _getcwd
#define remove_dir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(d) mkdir(d, 0755)
#define get_cwd getcwd
#define remove_dir rmdir
#endif

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int has(const char *s, const char *what) { return s && strstr(s, what); }

static void show(const pandoc_result *r)
{
    if (r && r->error_kind) printf("      %s: %.300s\n", r->error_kind, r->error_message);
}

/* The result shows nothing of the canaries: output, error, log. */
static int clean(const pandoc_result *r)
{
    return r && !has(r->output, "CANARY") && !has(r->error_message, "CANARY") && !has(r->log, "CANARY");
}

static pandoc_result *convert(const char *opts, const char *input)
{
    return pandoc_convert(opts, strlen(opts), input, input ? strlen(input) : 0);
}

static int option_error(const pandoc_result *r, const char *what)
{
    return r && r->status != 0 && has(r->error_kind, "PandocOptionError") && has(r->error_message, what);
}

/* A callback filter that notes whether the document it is given still has
 * the include attribute, and whether it has the file's text. */
static int seen_include[2], seen_content[2];

static int spy(void *data, const char *doc, size_t len, const char *ctx, size_t ctx_len, pandoc_buffer *out)
{
    (void)ctx; (void)ctx_len;
    int i = *(int *)data;
    char *s = malloc(len + 1);
    if (!s) return 1;
    memcpy(s, doc, len);
    s[len] = 0;
    seen_include[i] = has(s, "\"include\"");
    seen_content[i] = has(s, "INSIDE-X");
    free(s);
    pandoc_buffer_set(out, doc, len);
    return 0;
}

#define INC(path) "```{include=\"" path "\"}\nplaceholder\n```\n\n"
#define DIV(path) "::: {include=\"" path "\"}\nplaceholder\n:::\n\n"

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pandoc_result *r;
    make_dir("pcl");
    make_dir("pcl/in");
    write_file("pcl/secret.txt", "CANARY-SECRET\n");
    write_file("pcl/in/x.txt", "INSIDE-X\n");
    write_file("pcl/in/doc.md", INC("x.txt") INC("../secret.txt"));
    write_file("pcl/in/part.tex", "\\input{pcl/secret.txt}\n\nINSIDE-TEX\n");
    write_file("pcl/in/bad.json", "{\"CANARY-JSON\": [\n");
    char cwd[2048], abs_doc[2600];
    if (!get_cwd(cwd, sizeof cwd)) return 1;
    for (char *p = cwd; *p; p++) if (*p == '\\') *p = '/';
    snprintf(abs_doc, sizeof abs_doc, INC("%s/pcl/secret.txt"), cwd);
#ifndef _WIN32
    int linked = symlink("../secret.txt", "pcl/in/link.txt") == 0 && symlink("..", "pcl/in/up") == 0;
    check(linked, "(the symbolic links made)");
#endif

    /* "panclude" with the input given: its root is the working directory,
     * and the input is "standard input" (not the temporary file holding it) */
    r = convert("{\"to\": \"plain\", \"filters\": [\"panclude\"]}", INC("pcl/in/x.txt") INC("pcl/missing.txt"));
    show(r);
    check(r && r->status == 0 && has(r->output, "INSIDE-X"), "\"panclude\": the file's text, from the working directory");
    check(r && has(r->log, "\"type\": \"ScriptingWarning\"")
          && has(r->log, "panclude: cannot include \\\"pcl/missing.txt\\\" in standard input: not found"),
          "a failure is a ScriptingWarning in the log, naming standard input");
    pandoc_result_free(r);
    r = convert("{\"to\": \"plain\", \"filters\": [{\"type\": \"panclude\"}]}", INC("pcl/in/x.txt"));
    check(r && r->status == 0 && has(r->output, "INSIDE-X"), "{\"type\": \"panclude\"}: the same");
    pandoc_result_free(r);

    /* the main document's directory, as its first input file */
    r = convert("{\"to\": \"plain\", \"input-files\": [\"pcl/in/doc.md\"], \"filters\": [\"panclude\"]}", NULL);
    show(r);
    check(r && r->status == 0 && has(r->output, "INSIDE-X") && clean(r)
          && has(r->log, "cannot include \\\"../secret.txt\\\" in pcl/in/doc.md: outside the roots"),
          "the main document's directory is the root: .. leaving it is outside");
    pandoc_result_free(r);

    /* roots given: nothing outside them, however it is named */
    {
        char doc[8192];
        snprintf(doc, sizeof doc, "%s%s%s%s%s%s%s%s", INC("pcl/in/x.txt"), INC("pcl/secret.txt"),
                 INC("pcl/in/../secret.txt"), abs_doc, INC("pcl/in/up/secret.txt"), INC("pcl/in/link.txt"),
                 DIV("pcl/in/part.tex"), DIV("pcl/in/bad.json"));
        r = convert("{\"to\": \"plain\", \"filters\": [{\"type\": \"panclude\", \"roots\": [\"pcl/in\"]}]}", doc);
        show(r);
        check(r && r->status == 0 && has(r->output, "INSIDE-X"), "roots given: inside, read");
        check(clean(r), "roots given: nothing outside read (a path, .., an absolute path, links), nor shown");
        check(has(r ? r->log : NULL, "\\\"pcl/secret.txt\\\" in standard input: outside the roots")
              && has(r->log, "\\\"pcl/in/../secret.txt\\\" in standard input: outside the roots")
              && has(r->log, "/pcl/secret.txt\\\" in standard input: outside the roots"),
              "roots given: outside the roots, as written");
#ifndef _WIN32
        check(has(r ? r->log : NULL, "\\\"pcl/in/up/secret.txt\\\" in standard input: a symbolic link leads outside the roots")
              && has(r->log, "\\\"pcl/in/link.txt\\\" in standard input: a symbolic link leads outside the roots"),
              "roots given: a symbolic link leading out of them");
#endif
        check(r && has(r->output, "INSIDE-TEX") && has(r->log, "Could not load include file"),
              "an included file's own includes (LaTeX's \\input) read nothing");
        check(has(r ? r->log : NULL, "\\\"pcl/in/bad.json\\\" in standard input: pandoc cannot read it as \\\"json\\\""),
              "a file that doesn't parse: the message doesn't quote it");
        pandoc_result_free(r);
    }

    /* options: the entries' errors */
    struct { const char *what, *opts, *error; } bad[] = {
        {"roots not a list", "{\"filters\": [{\"type\": \"panclude\", \"roots\": \"pcl/in\"}]}",
         "panclude: \"roots\" is a list of directories"},
        {"roots not strings", "{\"filters\": [{\"type\": \"panclude\", \"roots\": [1]}]}",
         "panclude: \"roots\" is a list of directories"},
        {"an unknown field", "{\"filters\": [{\"type\": \"panclude\", \"root\": [\"pcl/in\"]}]}",
         "panclude: unknown field \"root\""},
        {"a root that doesn't exist (also with nothing to include)",
         "{\"filters\": [{\"type\": \"panclude\", \"roots\": [\"pcl/in\", \"pcl/no-such\"]}]}",
         "panclude: the root \"pcl/no-such\" is not a directory"},
        {"a root that is a file", "{\"filters\": [{\"type\": \"panclude\", \"roots\": [\"pcl/secret.txt\"]}]}",
         "is not a directory"},
        {"an empty root", "{\"filters\": [{\"type\": \"panclude\", \"roots\": [\"\"]}]}", "is not a directory"},
        {"\"panclude-roots\" in trusted options", "{\"filters\": [\"panclude\"], \"panclude-roots\": [\"pcl/in\"]}",
         "\"panclude-roots\" is the host's"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r = convert(bad[i].opts, "nothing to include");
        if (!option_error(r, bad[i].error)) show(r);
        char what[256];
        snprintf(what, sizeof what, "an option error: %s", bad[i].what);
        check(option_error(r, bad[i].error), what);
        pandoc_result_free(r);
    }

    /* pandoc's sandbox: no roots unless named */
    r = convert("{\"to\": \"plain\", \"sandbox\": true, \"filters\": [\"panclude\"]}", INC("pcl/in/x.txt"));
    check(r && r->status == 0 && !has(r->output, "INSIDE-X")
          && has(r->log, "there are no roots, so no file is included"),
          "sandbox: \"panclude\" has no roots, reads nothing");
    pandoc_result_free(r);
    r = convert("{\"to\": \"plain\", \"sandbox\": true, \"filters\": [{\"type\": \"panclude\", \"roots\": [\"pcl/in\"]}]}",
                INC("pcl/in/x.txt") INC("pcl/secret.txt"));
    check(r && r->status == 0 && has(r->output, "INSIDE-X") && clean(r), "sandbox: roots named are read, only they");
    pandoc_result_free(r);

    /* untrusted options: the step by name, with the host's roots only */
    r = convert("{\"to\": \"plain\", \"untrusted\": true, \"filters\": [\"panclude\"]}", INC("pcl/in/x.txt"));
    show(r);
    check(r && r->status == 0 && !has(r->output, "INSIDE-X")
          && has(r->log, "there are no roots, so no file is included"),
          "untrusted: without the host's roots, nothing read");
    pandoc_result_free(r);
    r = convert("{\"to\": \"plain\", \"untrusted\": {\"panclude-roots\": [\"pcl/in\"]}, \"filters\": [\"panclude\"]}",
                INC("pcl/in/x.txt") INC("pcl/secret.txt") INC("pcl/in/../secret.txt"));
    show(r);
    check(r && r->status == 0 && has(r->output, "INSIDE-X") && clean(r),
          "untrusted: the host's roots read, nothing outside them");
    pandoc_result_free(r);
    r = convert("{\"to\": \"plain\", \"untrusted\": {\"panclude-roots\": [\"pcl/in\"], \"cancel-token\": 0},"
                " \"filters\": [\"panclude\", \"citeproc\"]}", "x");
    check(r && (r->status == 0 || !has(r->error_message, "not allowed")),
          "untrusted: \"filters\" may name \"panclude\" and \"citeproc\"");
    pandoc_result_free(r);
    struct { const char *what, *opts, *error; } refused[] = {
        {"another filter beside it", "{\"untrusted\": true, \"filters\": [\"panclude\", \"x.lua\"]}",
         "not allowed for untrusted code: filters"},
        {"the step with roots of its own",
         "{\"untrusted\": true, \"filters\": [{\"type\": \"panclude\", \"roots\": [\"pcl\"]}]}",
         "not allowed for untrusted code: filters"},
        {"roots of the code's own", "{\"untrusted\": true, \"filters\": [\"panclude\"], \"panclude-roots\": [\"pcl\"]}",
         "not allowed for untrusted code: panclude-roots"},
        {"the host's roots not a list", "{\"untrusted\": {\"panclude-roots\": \"pcl\"}, \"filters\": [\"panclude\"]}",
         "\"panclude-roots\" is a list of directories"},
        {"the host's root not a directory",
         "{\"untrusted\": {\"panclude-roots\": [\"pcl/no-such\"]}, \"filters\": [\"panclude\"]}",
         "the root \"pcl/no-such\" is not a directory"},
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        r = convert(refused[i].opts, "x");
        if (!option_error(r, refused[i].error)) show(r);
        char what[256];
        snprintf(what, sizeof what, "untrusted, refused: %s", refused[i].what);
        check(option_error(r, refused[i].error), what);
        pandoc_result_free(r);
    }
    {
        const char *rm = "{\"options\": {\"untrusted\": {\"panclude-roots\": [\"pcl\"]}}, \"inputs\": [\"x\"]}";
        r = pandoc_read_many(rm, strlen(rm));
        check(option_error(r, "is for pandoc_convert"), "untrusted: pandoc_read_many takes no \"panclude-roots\"");
        pandoc_result_free(r);
        const char *q = "{\"query\": \"version\", \"untrusted\": {\"panclude-roots\": [\"pcl\"]}}";
        r = pandoc_query(q, strlen(q));
        check(option_error(r, "a query takes no \"panclude-roots\""), "untrusted: a query takes no \"panclude-roots\"");
        pandoc_result_free(r);
    }

    /* its place among the filters: where it is put, not moved */
    {
        int ids[2] = {0, 1};
        pandoc_filter fs[] = {{spy, &ids[0]}, {spy, &ids[1]}};
        const char *o = "{\"to\": \"plain\", \"filters\": [{\"type\": \"callback\", \"index\": 0}, \"panclude\","
                        " {\"type\": \"callback\", \"index\": 1}]}";
        const char *in = INC("pcl/in/x.txt");
        r = pandoc_convert_filters(o, strlen(o), in, strlen(in), fs, 2);
        show(r);
        check(r && r->status == 0 && seen_include[0] && !seen_content[0],
              "a filter before the step sees the element as the reader made it");
        check(r && seen_content[1] && !seen_include[1], "a filter after the step sees the file's text");
        pandoc_result_free(r);
    }

    /* --fail-if-warnings: a failure fails the conversion */
    r = convert("{\"to\": \"plain\", \"fail-if-warnings\": true, \"filters\": [\"panclude\"]}", INC("pcl/missing.txt"));
    check(r && r->status != 0 && has(r->log, "pcl/missing.txt\\\" in standard input: not found"),
          "fail-if-warnings: an include that fails fails the conversion");
    pandoc_result_free(r);

    /* the command line: -F panclude, the step, also with a program of that
     * name in the working directory (not run); --sandbox: no roots */
    {
#ifndef _WIN32
        write_file("panclude", "#!/bin/sh\necho ran > pcl/ran\ncat\n");
        chmod("panclude", 0755);
#endif
        const char *args[] = {"-t", "plain", "-F", "panclude"};
        const char *in = INC("pcl/in/x.txt");
        r = pandoc_convert_args(4, args, in, strlen(in));
        show(r);
        check(r && r->status == 0 && has(r->output, "INSIDE-X"), "-F panclude: the step");
#ifndef _WIN32
        FILE *ran = fopen("pcl/ran", "rb");
        check(!ran, "-F panclude: not the program named panclude in the working directory");
        if (ran) fclose(ran);
        remove("pcl/ran");
        remove("panclude");
#endif
        pandoc_result_free(r);
        const char *sargs[] = {"-t", "plain", "--sandbox", "-F", "panclude"};
        r = pandoc_convert_args(5, sargs, in, strlen(in));
        check(r && r->status == 0 && !has(r->output, "INSIDE-X") && has(r->log, "there are no roots"),
              "--sandbox -F panclude: no roots");
        pandoc_result_free(r);
    }

#ifndef _WIN32
    remove("pcl/in/link.txt");
    remove("pcl/in/up");
#endif
    remove("pcl/in/x.txt");
    remove("pcl/in/doc.md");
    remove("pcl/in/part.tex");
    remove("pcl/in/bad.json");
    remove("pcl/secret.txt");
    remove_dir("pcl/in");
    remove_dir("pcl");
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
