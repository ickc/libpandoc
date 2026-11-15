/* Edge cases: what must neither end nor crash the host process, and the
 * messages it is told. The files are written (and removed) in the current
 * directory.
 *
 *   cc test/edge.c -Idist/include -Ldist/lib -lpandoc && ./a.out
 *
 * Not on Windows: standard output as a broken pipe, the command's --version
 * captured, and a missing temporary directory. Only on Linux: a file name
 * that isn't UTF-8 (macOS file systems refuse them). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpandoc.h>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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

#ifndef _WIN32
/* the file's first bytes, NUL-terminated, in buf */
static const char *read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, size - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
    return buf;
}
#endif

static int has(const char *s, const char *what) { return s && strstr(s, what); }

static void show(const pandoc_result *r)
{
    if (r && r->error_kind) printf("      %s: %.300s\n", r->error_kind, r->error_message);
}

#ifndef _WIN32
/* The child: pandoc_main with standard output a pipe nobody reads, SIGPIPE
 * ignored (as Python does): pandoc's status (0, as the command's), returned
 * rather than the process ended. 42 if both calls returned 0. */
static int broken_stdout(void)
{
    int fds[2];
    if (pipe(fds) != 0) return 2;
    close(fds[0]);
    dup2(fds[1], 1);
    signal(SIGPIPE, SIG_IGN);
    const char *version[] = {"pandoc", "--version"};
    int a = pandoc_main(2, version, NULL, 0, NULL, 0);
    const char *convert[] = {"pandoc", "-t", "html", "edge-in.md"};
    int b = pandoc_main(4, convert, NULL, 0, NULL, 0);
    fprintf(stderr, "      returned %d and %d\n", a, b);
    return a == 0 && b == 0 ? 42 : 1;
}

/* The child: SIGPIPE at its default (ending the process, as in a C
 * program), a JSON filter that exits without reading its input (1 MB, more
 * than a pipe holds): the filter's error, rather than the process ended by
 * SIGPIPE (before 1.12); and the thread's signal mask as it was. 42 if so.
 * (Found by fuzz/.) */
static int filter_exits(void)
{
    signal(SIGPIPE, SIG_DFL);
    size_t n = 1 << 20;
    char *doc = malloc(n);
    if (!doc) return 2;
    memset(doc, 'x', n);
    const char *o = "{\"from\":\"markdown\",\"to\":\"html\",\"filters\":[\"false\"]}";
    pandoc_result *r = pandoc_convert(o, strlen(o), doc, n);
    sigset_t mask;
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    int ok = r && r->status != 0 && has(r->error_kind, "PandocFilterError") && !sigismember(&mask, SIGPIPE);
    show(r);
    pandoc_result_free(r);
    free(doc);
    return ok ? 42 : 1;
}

/* what pandoc_main writes to standard output, in buf */
static int main_output(int argc, const char *const *argv, char *buf, size_t size)
{
    fflush(stdout);
    int saved = dup(1);
    FILE *f = fopen("edge-out.txt", "wb");
    if (!f || saved < 0) return -1;
    dup2(fileno(f), 1);
    int st = pandoc_main(argc, argv, NULL, 0, NULL, 0);
    dup2(saved, 1);
    close(saved);
    fclose(f);
    read_file("edge-out.txt", buf, size);
    remove("edge-out.txt");
    return st;
}
#endif

/* A native filter without a function. */
static int unused(void *userdata, const char *doc, size_t doc_len,
                  const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)doc; (void)doc_len; (void)context; (void)context_len; (void)out;
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    write_file("edge-in.md", "*x*\n");
#ifndef _WIN32
    if (argc > 1 && strcmp(argv[1], "broken-stdout") == 0) return broken_stdout();
    if (argc > 1 && strcmp(argv[1], "filter-exits") == 0) return filter_exits();
#endif
    pandoc_result *r;
    const char *md = "*x*";

    /* pandoc_main: arguments that aren't UTF-8 are decoded as the pandoc
     * command decodes its own (the file system's encoding), and nothing
     * ends the process */
    {
        const char *bad[] = {"pandoc", "--from=markdown\xff", "edge-in.md"};
        int st = pandoc_main(3, bad, NULL, 0, NULL, 0);
        check(st != 0, "pandoc_main: an argument that isn't UTF-8 is pandoc's error, returned");
        printf("      status %d\n", st);
#ifdef __linux__
        char out[256];
        write_file("caf\xe9.md", "*caf\xc3\xa9*\n");
        const char *latin1[] = {"pandoc", "-t", "html", "-o", "edge-latin1.html", "caf\xe9.md"};
        st = pandoc_main(6, latin1, NULL, 0, NULL, 0);
        check(st == 0 && strcmp(read_file("edge-latin1.html", out, sizeof out),
                                "<p><em>caf\xc3\xa9</em></p>\n") == 0,
              "pandoc_main: a file named in Latin-1 (caf\\xe9.md), as the command reads it");
        remove("caf\xe9.md");
        remove("edge-latin1.html");
#endif
    }

#ifndef _WIN32
    /* pandoc_main: standard output a broken pipe (in a child process, which
     * the runtime used to end with status 0, as the command's) */
    {
        char cmd[4096];
        snprintf(cmd, sizeof cmd, "\"%s\" broken-stdout", argv[0]);
        int st = system(cmd);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 42,
              "pandoc_main: a broken pipe on standard output returns pandoc's status (0)");
    }

    /* a JSON filter that exits without reading its input, SIGPIPE at its
     * default: the filter's error, not the process ended (in a child) */
    {
        char cmd[4096];
        snprintf(cmd, sizeof cmd, "\"%s\" filter-exits", argv[0]);
        int st = system(cmd);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 42,
              "a JSON filter that exits without reading its input: its error, not SIGPIPE");
    }

    /* pandoc_main --version: as the command's, with its features and
     * scripting engine */
    {
        char out[2048];
        const char *version[] = {"pandoc", "--version"};
        int st = main_output(2, version, out, sizeof out);
        check(st == 0 && strncmp(out, "pandoc 3.", 9) == 0
              && has(out, "\nFeatures: -server +lua\n") && has(out, "\nScripting engine: Lua 5.")
              && has(out, "\nUser data directory: "),
              "pandoc_main --version: features and scripting engine, as the command's");
        printf("      %.120s\n", out);
    }

    /* pandoc_main --help: the usage message names argv[0], as the command
     * names itself (from 1.11; before, the host process's name) */
    {
        char out[16384];
        const char *help[] = {"/usr/bin/mypandoc", "--help"};
        int st = main_output(2, help, out, sizeof out);
        check(st == 0 && strncmp(out, "mypandoc [OPTIONS] [FILES]", 26) == 0,
              "pandoc_main --help: the usage names argv[0]");
    }

    /* an error that isn't pandoc's: its message, without GHC's backtrace */
    {
        const char *tmp = getenv("TMPDIR");
        char *saved = tmp ? strcpy(malloc(strlen(tmp) + 1), tmp) : NULL;
        setenv("TMPDIR", "edge-no-such-directory", 1);
        const char *opts = "{\"to\": \"html\"}";
        r = pandoc_convert(opts, strlen(opts), md, strlen(md));
        if (saved) setenv("TMPDIR", saved, 1); else unsetenv("TMPDIR");
        free(saved);
        check(r && r->status != 0 && strcmp(r->error_kind, "Exception") == 0
              && has(r->error_message, "edge-no-such-directory")
              && !has(r->error_message, "backtrace") && !has(r->error_message, "\n"),
              "an error that isn't pandoc's: its message alone, no backtrace");
        show(r);
        pandoc_result_free(r);
    }
#endif

    /* input given, and input files that don't name it ("-") */
    {
        const char *files = "{\"to\": \"html\", \"input-files\": [\"edge-in.md\"]}";
        r = pandoc_convert(files, strlen(files), md, strlen(md));
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
              "input given with input-files not naming it (\"-\") is an error");
        show(r);
        pandoc_result_free(r);
        const char *dash = "{\"to\": \"html\", \"input-files\": [\"edge-in.md\", \"-\"]}";
        r = pandoc_convert(dash, strlen(dash), "*y*", 3);
        check(r && r->status == 0 && has(r->output, "<em>x</em>") && has(r->output, "<em>y</em>"),
              "input given as \"-\" among input-files");
        pandoc_result_free(r);
        const char *none = "{\"to\": \"html\", \"input-files\": []}";
        r = pandoc_convert(none, strlen(none), md, strlen(md));
        check(r && r->status == 0 && strcmp(r->output, "<p><em>x</em></p>\n") == 0,
              "input given with no input-files ([])");
        pandoc_result_free(r);
        const char *args[] = {"-t", "html", "edge-in.md"};
        r = pandoc_convert_args(3, args, md, strlen(md));
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
              "convert_args: input given with a file argument that isn't \"-\" is an error");
        pandoc_result_free(r);
    }

    /* a native filter whose fn is NULL: an error, not a crash */
    {
        pandoc_filter fs[] = {{unused, NULL}, {NULL, NULL}};
        const char *o = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 1}]}";
        r = pandoc_convert_filters(o, strlen(o), md, strlen(md), fs, 2);
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocFilterError") == 0
              && has(r->error_message, "NULL"),
              "a filter whose fn is NULL is an error");
        show(r);
        pandoc_result_free(r);
    }

    /* pandoc_read_many: the readers' messages are in the result's log, as a
     * conversion's, not on standard error */
    {
        const char *rq = "{\"options\": {\"from\": \"latex\"}, "
                         "\"inputs\": [\"a\", \"\\\\input{edge-no-such-file}\", \"\\\\emph{b}\"]}";
#ifndef _WIN32
        fflush(stderr);
        int saved = dup(2);
        FILE *f = fopen("edge-err.txt", "wb");
        if (f) dup2(fileno(f), 2);
#endif
        r = pandoc_read_many(rq, strlen(rq));
#ifndef _WIN32
        if (f) { dup2(saved, 2); fclose(f); }
        close(saved);
        char err[512];
        read_file("edge-err.txt", err, sizeof err);
        remove("edge-err.txt");
        check(err[0] == '\0', "read_many: nothing on standard error");
        if (err[0]) printf("      %.200s\n", err);
#endif
        check(r && r->status == 0 && has(r->log, "CouldNotLoadIncludeFile")
              && has(r->log, "edge-no-such-file") && has(r->output, "\"Emph\""),
              "read_many: a reader's warning in the log");
        if (r) printf("      log: %.120s\n", r->log);
        pandoc_result_free(r);
        const char *quiet = "{\"options\": {\"from\": \"markdown\"}, \"inputs\": [\"*a*\", \"b\"]}";
        r = pandoc_read_many(quiet, strlen(quiet));
        check(r && r->status == 0 && strcmp(r->log, "[]") == 0, "read_many: no messages, []");
        pandoc_result_free(r);
    }

    remove("edge-in.md");
    return failures != 0;
}
