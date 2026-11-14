/* "sandbox": true and citeproc: citeproc reads only the bibliography, CSL
 * style and abbreviations the caller names in the options, never what a
 * document names, and fetches nothing. The secrets are canary files this
 * test writes (and removes) in the current directory.
 *
 *   cc test/sandbox.c -Idist/include -Ldist/lib -lpandoc -pthread && ./a.out
 *
 * Not on Windows: the symbolic link and the URL (a local listening socket
 * that counts connections) cases. */
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
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

/* The result leaks nothing of the canaries. */
static int clean(const pandoc_result *r)
{
    return r && !has(r->output, "CANARY") && !has(r->error_message, "CANARY")
           && !has(r->log, "CANARY");
}

static pandoc_result *convert(const char *opts, const char *input)
{
    return pandoc_convert(opts, strlen(opts), input, strlen(input));
}

static char *copy(const char *s) { return s ? strcpy(malloc(strlen(s) + 1), s) : NULL; }

#ifndef _WIN32
/* A local server that counts connections and hangs up on each. */
static int connections = 0;
static pthread_mutex_t counting = PTHREAD_MUTEX_INITIALIZER;

static void *serve(void *arg)
{
    int s = *(int *)arg;
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) return NULL;
        pthread_mutex_lock(&counting);
        connections++;
        pthread_mutex_unlock(&counting);
        close(c);
    }
}
#endif

#define SANDBOX "{\"to\": \"plain\", \"citeproc\": true, \"sandbox\": true"
#define LATEX "{\"from\": \"latex\", \"to\": \"plain\", \"citeproc\": true, \"sandbox\": true"
#define NOCITE "nocite: \"@*\"\n---\n\nText.\n"
#define REFS "references: [{id: x, title: T, type: book}]\n---\n\nText [@x].\n"

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pandoc_result *r;
    make_dir("sbx");
    write_file("sbx/refs.bib", "@book{mine, title={Caller Title}, author={Caller, Carl}, year={2002}}\n");
    write_file("sbx-out.bib", "@book{canary, title={CANARY-BIB}, author={Outside, Olga}, year={2001}}\n");
    write_file("sbx-out.csl", "CANARY-CSL\n");
    write_file("sbx-out.json", "CANARY-ABBR\n");
    char cwd[2048], abs_doc[2400];
    if (!get_cwd(cwd, sizeof cwd)) return 1;
    for (char *p = cwd; *p; p++) if (*p == '\\') *p = '/';
    snprintf(abs_doc, sizeof abs_doc, "---\nbibliography: \"%s/sbx-out.bib\"\n" NOCITE, cwd);
#ifndef _WIN32
    int linked = symlink("../sbx-out.bib", "sbx/link.bib") == 0;
#endif

    /* What a document names: none of it is read, and the message is the
     * same as when the file doesn't exist (so nothing can be probed). */
    struct { const char *what, *opts, *doc; } named[] = {
        {"bibliography, relative", SANDBOX "}", "---\nbibliography: sbx-out.bib\n" NOCITE},
        {"bibliography, absolute", SANDBOX "}", abs_doc},
        {"bibliography, through ..", SANDBOX "}", "---\nbibliography: sbx/../sbx-out.bib\n" NOCITE},
        {"csl", SANDBOX "}", "---\ncsl: sbx-out.csl\n" REFS},
        {"citation-style", SANDBOX "}", "---\ncitation-style: sbx/../sbx-out.csl\n" REFS},
        {"citation-abbreviations", SANDBOX "}", "---\ncitation-abbreviations: sbx-out.json\n" REFS},
        {"LaTeX \\bibliography", LATEX "}", "\\bibliography{sbx/../sbx-out}\\nocite{*}"},
        {"LaTeX \\addbibresource", LATEX "}", "\\addbibresource{sbx/../sbx-out.bib}\\nocite{*}"},
        /* with a bibliography of the caller's: the document's CSL still not */
        {"csl, beside the caller's bibliography", SANDBOX ", \"bibliography\": [\"sbx/refs.bib\"]}",
         "---\ncsl: sbx-out.csl\n---\n\nText [@mine].\n"},
#ifndef _WIN32
        {"bibliography, a symbolic link", SANDBOX "}", "---\nbibliography: sbx/link.bib\n" NOCITE},
#endif
    };
    enum { N = sizeof named / sizeof *named };
    char *kind[N], *message[N];
    char what[256];
    for (int i = 0; i < N; i++) {
        r = convert(named[i].opts, named[i].doc);
        snprintf(what, sizeof what, "sandbox, citeproc: a document's %s is not read", named[i].what);
        check(r && r->status != 0 && clean(r)
              && strcmp(r->error_kind, "PandocResourceNotFound") == 0, what);
        if (r && r->error_message) printf("      %s\n", r->error_message);
        kind[i] = copy(r ? r->error_kind : NULL);
        message[i] = copy(r ? r->error_message : NULL);
        pandoc_result_free(r);
    }
#ifndef _WIN32
    check(linked, "(the symbolic link was made)");
#endif

    /* without the sandbox, as before: the canaries show, so they would
     * above if they were read */
    const char *trusted = "{\"to\": \"plain\", \"citeproc\": true}";
    r = convert(trusted, named[0].doc);
    check(r && r->status == 0 && has(r->output, "CANARY-BIB"),
          "no sandbox: a document's bibliography is read, as pandoc does");
    pandoc_result_free(r);
    r = convert(trusted, named[3].doc);
    check(r && r->status != 0 && has(r->error_message, "CANARY-CSL"),
          "no sandbox: a document's csl is read, as pandoc does");
    pandoc_result_free(r);
    const char *latex = "{\"from\": \"latex\", \"to\": \"plain\", \"citeproc\": true}";
    r = convert(latex, named[6].doc);
    check(r && r->status == 0 && has(r->output, "CANARY-BIB"),
          "no sandbox: LaTeX's \\bibliography is read, as pandoc does");
    pandoc_result_free(r);

    /* the caller's bibliography works, also when the document names it */
    const char *own = SANDBOX ", \"bibliography\": [\"sbx/refs.bib\"]}";
    r = convert(own, "Text [@mine].\n");
    check(r && r->status == 0 && has(r->output, "Caller Title"),
          "sandbox, citeproc: the caller's bibliography is read");
    pandoc_result_free(r);
    r = convert(own, "---\nbibliography: ./sbx/refs.bib\n---\n\nText [@mine].\n");
    check(r && r->status == 0 && has(r->output, "Caller Title"),
          "sandbox, citeproc: ... also when the document names the same file");
    pandoc_result_free(r);
    /* markdown writes the citations citeproc rendered, as without the
     * sandbox, not [@mine] (pandoc turns its citations extension off) */
    const char *md_sb = "{\"to\": \"markdown\", \"citeproc\": true, \"sandbox\": true,"
                        " \"bibliography\": [\"sbx/refs.bib\"]}";
    const char *md = "{\"to\": \"markdown\", \"citeproc\": true, \"bibliography\": [\"sbx/refs.bib\"]}";
    r = convert(md_sb, "Text @mine.\n");
    pandoc_result *r2 = convert(md, "Text @mine.\n");
    check(r && r2 && r->status == 0 && has(r->output, "Caller (2002)") && !has(r->output, "@mine")
          && strcmp(r->output, r2->output) == 0,
          "sandbox, citeproc: markdown output as without the sandbox");
    pandoc_result_free(r);
    pandoc_result_free(r2);

    /* untrusted: citeproc allowed, sandboxed; no option naming a file */
    const char *ut = "{\"untrusted\": true, \"to\": \"plain\", \"citeproc\": true}";
    r = convert(ut, "---\nreferences: [{id: x, title: Inline Title, type: book}]\n---\n\nText [@x].\n");
    check(r && r->status == 0 && has(r->output, "Inline Title"), "untrusted: citeproc runs");
    pandoc_result_free(r);
    r = convert(ut, named[0].doc);
    check(r && r->status != 0 && clean(r), "untrusted: citeproc reads no bibliography a document names");
    pandoc_result_free(r);
    const char *ut_meta = "{\"untrusted\": true, \"to\": \"plain\", \"citeproc\": true,"
                          " \"metadata\": {\"bibliography\": \"sbx-out.bib\", \"nocite\": \"@*\"}}";
    r = convert(ut_meta, "Text.\n");
    check(r && r->status != 0 && clean(r), "untrusted: ... nor one its metadata option names");
    pandoc_result_free(r);
    const char *refused[] = {
        "{\"untrusted\": true, \"citeproc\": true, \"bibliography\": [\"sbx/refs.bib\"]}",
        "{\"untrusted\": true, \"citeproc\": true, \"csl\": \"sbx-out.csl\"}",
        "{\"untrusted\": true, \"citeproc\": true, \"citation-abbreviations\": \"sbx-out.json\"}",
        "{\"untrusted\": true, \"filters\": [\"citeproc\"]}",
    };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        r = convert(refused[i], "a");
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0, refused[i]);
        pandoc_result_free(r);
    }

    /* the same names, the files gone: the same answers */
    remove("sbx-out.bib");
    remove("sbx-out.csl");
    remove("sbx-out.json");
    for (int i = 0; i < N; i++) {
        r = convert(named[i].opts, named[i].doc);
        snprintf(what, sizeof what, "sandbox, citeproc: %s: as if it didn't exist", named[i].what);
        check(r && kind[i] && message[i] && r->error_kind && r->error_message
              && strcmp(r->error_kind, kind[i]) == 0 && strcmp(r->error_message, message[i]) == 0,
              what);
        pandoc_result_free(r);
        free(kind[i]);
        free(message[i]);
    }

#ifndef _WIN32
    /* a URL: nothing is fetched (a local server counts connections) */
    {
        static int s;
        s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        int up = s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0
                 && getsockname(s, (struct sockaddr *)&a, &len) == 0;
        pthread_t t;
        up = up && pthread_create(&t, NULL, serve, &s) == 0 && pthread_detach(t) == 0;
        check(up, "(a local server is listening)");
        if (up) {
            char doc[256];
            snprintf(doc, sizeof doc, "---\nbibliography: http://127.0.0.1:%d/x.bib\n" NOCITE,
                     ntohs(a.sin_port));
            r = convert(SANDBOX "}", doc);
            pthread_mutex_lock(&counting);
            int seen = connections;
            pthread_mutex_unlock(&counting);
            check(r && r->status != 0 && strcmp(r->error_kind, "PandocResourceNotFound") == 0
                  && seen == 0, "sandbox, citeproc: a URL bibliography is not fetched");
            if (r && r->error_message) printf("      %s (%d connections)\n", r->error_message, seen);
            pandoc_result_free(r);
            /* the server works: without the sandbox pandoc connects */
            r = convert(trusted, doc);
            pthread_mutex_lock(&counting);
            seen = connections;
            pthread_mutex_unlock(&counting);
            check(r && r->status != 0 && seen > 0, "no sandbox: a URL bibliography is fetched, as pandoc does");
            pandoc_result_free(r);
        }
    }
    remove("sbx/link.bib");
#endif
    remove("sbx/refs.bib");
    remove_dir("sbx");
    return failures != 0;
}
