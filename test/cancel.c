/* Cancelling (pandoc_cancel, 1.13): a call given a token that another
 * thread cancels fails with the error kind "Cancelled", soon after, and
 * leaves nothing behind (temporary files, descriptors, a JSON filter's
 * process); the library goes on. Before, during and after a call; many
 * calls at once with one cancelled; a callback filter running when it
 * happens, and the calls it makes; read_many. And (1.14) the host's token
 * given inside "untrusted": {"cancel-token": N}.
 *
 *   cc test/cancel.c -Idist/include -Ldist/lib -lpandoc -pthread && ./a.out
 *
 * The slow input is limits.c's: 100,000 '>' read as markdown, minutes of
 * pandoc's time. Not on Windows: the temporary directory, the descriptors,
 * the JSON filter. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libpandoc.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int has(const char *s, const char *what) { return s && strstr(s, what); }

static int kind(const pandoc_result *r, const char *k)
{
    return r && r->status != 0 && r->error_kind && strcmp(r->error_kind, k) == 0;
}

static int cancelled(const pandoc_result *r)
{
    return kind(r, "Cancelled") && has(r->error_message, "cancelled") && strcmp(r->log, "[]") == 0;
}

static void show(const pandoc_result *r, double secs)
{
    if (r && r->error_kind) printf("      %.2f s, %s: %.200s\n", secs, r->error_kind, r->error_message);
    else if (r) printf("      %.2f s, status %d\n", secs, r->status);
}

#ifdef _WIN32
static double now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void pause_ms(int ms) { Sleep(ms); }
typedef HANDLE thread;
static DWORD WINAPI trampoline(LPVOID p)
{
    void **a = p;
    ((void (*)(void *))a[0])(a[1]);
    free(a);
    return 0;
}
static thread spawn(void (*f)(void *), void *arg)
{
    void **a = malloc(2 * sizeof *a);
    a[0] = (void *)f;
    a[1] = arg;
    return CreateThread(NULL, 0, trampoline, a, 0, NULL);
}
static void join(thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
typedef pthread_t thread;
static void *trampoline(void *p)
{
    void **a = p;
    ((void (*)(void *))a[0])(a[1]);
    free(a);
    return NULL;
}
static thread spawn(void (*f)(void *), void *arg)
{
    pthread_t t;
    void **a = malloc(2 * sizeof *a);
    a[0] = (void *)f;
    a[1] = arg;
    pthread_create(&t, NULL, trampoline, a);
    return t;
}
static void join(thread t) { pthread_join(t, NULL); }
#endif

static char *slow; /* the slow input */
static size_t slow_len;

/* options with a token: "{...extra, "cancel-token": N}" */
static char *with_token(const char *extra, int64_t token)
{
    char *o = malloc(512);
    snprintf(o, 512, "{%s%s\"cancel-token\": %lld}", extra, *extra ? ", " : "", (long long)token);
    return o;
}

/* untrusted options with the host's token (1.14):
 * "{...extra, "untrusted": {"cancel-token": N}}" */
static char *with_host_token(const char *extra, int64_t token)
{
    char *o = malloc(512);
    snprintf(o, 512, "{%s%s\"untrusted\": {\"cancel-token\": %lld}}", extra, *extra ? ", " : "",
             (long long)token);
    return o;
}

static int ok_html(const pandoc_result *r, const char *html)
{
    return r && r->status == 0 && strcmp(r->output, html) == 0;
}

/* A conversion on another thread: its options and input; its result and
 * when it returned. */
typedef struct {
    const char *opts, *input;
    size_t input_len;
    const pandoc_filter *filters;
    size_t n;
    pandoc_result *r;
    double ended;
} job;

static void run_job(void *p)
{
    job *j = p;
    j->r = j->filters ? pandoc_convert_filters(j->opts, strlen(j->opts), j->input, j->input_len,
                                               j->filters, j->n)
                      : pandoc_convert(j->opts, strlen(j->opts), j->input, j->input_len);
    j->ended = now();
}

static void run_read_many(void *p)
{
    job *j = p;
    j->r = pandoc_read_many(j->opts, strlen(j->opts));
    j->ended = now();
}

/* Callback filters. */
static atomic_int calls = 0;
static int64_t self_token; /* the token the "cancel itself" filter cancels */
static char nested_kind[64];
static double nested_secs;

/* sleeps a second, answers the document */
static int sleepy(void *userdata, const char *doc, size_t doc_len,
                  const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)context; (void)context_len;
    atomic_fetch_add(&calls, 1);
    pause_ms(1000);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

/* waits half a second, then converts something (with no token of its own),
 * slow if userdata is not NULL; tells what that answered */
static int nested(void *userdata, const char *doc, size_t doc_len,
                  const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)context; (void)context_len;
    atomic_fetch_add(&calls, 1);
    if (!userdata) pause_ms(500);
    const char *o = "{\"from\": \"markdown\", \"to\": \"html\"}";
    double t = now();
    pandoc_result *r = userdata ? pandoc_convert(o, strlen(o), slow, slow_len)
                                : pandoc_convert(o, strlen(o), "*y*", 3);
    nested_secs = now() - t;
    snprintf(nested_kind, sizeof nested_kind, "%s", r && r->error_kind ? r->error_kind : "(none)");
    pandoc_result_free(r);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

/* cancels its own conversion's token, and answers */
static int cancel_itself(void *userdata, const char *doc, size_t doc_len,
                         const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)context; (void)context_len;
    pandoc_cancel(self_token);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

#ifndef _WIN32
static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) d = opendir("/dev/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static int entries(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
    closedir(d);
    return n;
}
#endif

/* The input converted with a token on another thread, cancelled after
 * `after` ms: its result, and the seconds from the cancel to its return. */
static pandoc_result *cancel_input(const char *extra, const char *input, size_t len, int after,
                                   double *secs)
{
    int64_t t = pandoc_cancel_token_new();
    char *o = with_token(extra, t);
    job j = {o, input, len, NULL, 0, NULL, 0};
    thread th = spawn(run_job, &j);
    pause_ms(after);
    double c = now();
    int st = pandoc_cancel(t);
    join(th);
    *secs = j.ended - c;
    pandoc_cancel_token_free(t);
    free(o);
    if (st != 0) printf("      pandoc_cancel returned %d\n", st);
    return j.r;
}

static pandoc_result *cancel_slow(const char *extra, int after, double *secs)
{
    return cancel_input(extra, slow, slow_len, after, secs);
}

#define MD "\"from\": \"markdown\", \"to\": \"html\""

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    slow_len = 100000;
    slow = malloc(slow_len);
    memset(slow, '>', slow_len);
    pandoc_result *r;
    double secs;
    char *o;

#ifndef _WIN32
    mkdir("cancel-tmp", 0700);
    setenv("TMPDIR", "cancel-tmp", 1);
#endif

    /* tokens: numbers from 1, not reused; unknown ones are nothing */
    {
        int64_t a = pandoc_cancel_token_new(), b = pandoc_cancel_token_new();
        check(a > 0 && b > a, "pandoc_cancel_token_new: distinct numbers from 1");
        pandoc_cancel_token_free(a);
        int64_t c = pandoc_cancel_token_new();
        check(c > b, "... a freed one's not given again");
        check(pandoc_cancel(a) == -1 && pandoc_cancel(0) == -1 && pandoc_cancel(-5) == -1
              && pandoc_cancel(c + 1000) == -1,
              "pandoc_cancel of what is no token: -1, nothing happens");
        pandoc_cancel_token_free(a);
        pandoc_cancel_token_free(0);
        pandoc_cancel_token_free(c + 1000);
        check(pandoc_cancel(b) == 0 && pandoc_cancel(b) == 0, "pandoc_cancel of a token: 0, twice too");
        pandoc_cancel_token_free(b);
        pandoc_cancel_token_free(c);

        /* "cancel-token" that names no token: a PandocOptionError */
        const char *bad[] = {
            "{\"cancel-token\": \"1\"}", "{\"cancel-token\": -1}", "{\"cancel-token\": 0}",
            "{\"cancel-token\": 1.5}", "{\"cancel-token\": null}",
        };
        int all = 1;
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            r = pandoc_convert(bad[i], strlen(bad[i]), "x", 1);
            all = all && kind(r, "PandocOptionError") && has(r->error_message, "cancel-token");
            pandoc_result_free(r);
        }
        check(all, "\"cancel-token\" that isn't a token's number: a PandocOptionError");
        o = with_token("", c);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "no token"),
              "... a freed token: a PandocOptionError (the call can't be cancelled: it doesn't start)");
        show(r, 0);
        pandoc_result_free(r);
        free(o);
        int64_t u = pandoc_cancel_token_new();
        o = with_token("\"untrusted\": true", u);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "cancel-token"),
              "... with \"untrusted\": refused");
        pandoc_result_free(r);
        free(o);
        o = malloc(128);
        snprintf(o, 128, "{\"options\": {\"cancel-token\": %lld}, \"inputs\": [\"*a*\"]}", (long long)u);
        r = pandoc_read_many(o, strlen(o));
        check(r && r->status == 0 && has(r->output, "Emph"), "read_many takes a token too");
        pandoc_result_free(r);
        free(o);

        /* the host's token inside "untrusted" (1.14) */
        o = with_host_token("\"to\": \"html\"", u);
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        check(ok_html(r, "<p><em>x</em></p>\n"), "\"untrusted\": {\"cancel-token\": N} converts");
        pandoc_result_free(r);
        free(o);
        o = malloc(256);
        snprintf(o, 256, "{\"untrusted\": {\"cancel-token\": %lld}, \"cancel-token\": %lld}",
                 (long long)u, (long long)u);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "not allowed for untrusted code: cancel-token"),
              "... beside a top-level \"cancel-token\": refused, as with \"untrusted\": true");
        pandoc_result_free(r);
        snprintf(o, 256, "{\"untrusted\": {}, \"cancel-token\": %lld}", (long long)u);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "not allowed for untrusted code: cancel-token"),
              "... a top-level one beside \"untrusted\": {}: refused");
        pandoc_result_free(r);
        snprintf(o, 256, "{\"untrusted\": {\"cancel-token\": %lld, \"timeout\": 5}}", (long long)u);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "unknown field \"timeout\""),
              "... another field in \"untrusted\": an error naming it");
        pandoc_result_free(r);
        all = 1;
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            snprintf(o, 256, "{\"untrusted\": %s}", bad[i]);
            r = pandoc_convert(o, strlen(o), "x", 1);
            all = all && kind(r, "PandocOptionError") && has(r->error_message, "cancel-token");
            pandoc_result_free(r);
        }
        check(all, "... a \"cancel-token\" there that isn't a token's number: a PandocOptionError");
        snprintf(o, 256, "{\"query\": \"version\", \"untrusted\": {\"cancel-token\": %lld}}", (long long)u);
        r = pandoc_query(o, strlen(o));
        check(kind(r, "PandocOptionError") && has(r->error_message, "cancel-token"),
              "... in a query: a PandocOptionError (queries take no token)");
        pandoc_result_free(r);
        snprintf(o, 256, "{\"options\": {\"untrusted\": {\"cancel-token\": %lld}}, \"inputs\": [\"*a*\"]}",
                 (long long)u);
        r = pandoc_read_many(o, strlen(o));
        check(r && r->status == 0 && has(r->output, "Emph"), "... read_many takes it too");
        pandoc_result_free(r);
        free(o);
        o = with_host_token("", c);
        r = pandoc_convert(o, strlen(o), "x", 1);
        check(kind(r, "PandocOptionError") && has(r->error_message, "no token"),
              "... a freed token there: a PandocOptionError");
        pandoc_result_free(r);
        free(o);
        pandoc_cancel_token_free(u);
    }

    /* before: a cancelled token stops a call at once, every time */
    {
        int64_t t = pandoc_cancel_token_new();
        pandoc_cancel(t);
        o = with_token(MD, t);
        double s = now();
        r = pandoc_convert(o, strlen(o), slow, slow_len);
        secs = now() - s;
        check(cancelled(r) && secs < 1, "before: a cancelled token's call fails with \"Cancelled\" at once");
        show(r, secs);
        pandoc_result_free(r);
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        check(cancelled(r), "... and stays cancelled");
        pandoc_result_free(r);
        free(o);
        o = with_host_token(MD, t);
        r = pandoc_convert(o, strlen(o), slow, slow_len);
        check(cancelled(r), "... the host's token inside \"untrusted\" too");
        pandoc_result_free(r);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* during, from another thread */
    r = cancel_slow(MD, 300, &secs);
    check(cancelled(r) && secs < 2, "during: the call stops, \"Cancelled\", soon after");
    show(r, secs);
    pandoc_result_free(r);
    r = pandoc_convert("{\"to\": \"html\"}", 14, "*x*", 3);
    check(ok_html(r, "<p><em>x</em></p>\n"), "... and the next call works");
    pandoc_result_free(r);
    /* untrusted, the host's token inside "untrusted" (1.14) */
    {
        int64_t t = pandoc_cancel_token_new();
        o = with_host_token(MD, t);
        job j = {o, slow, slow_len, NULL, 0, NULL, 0};
        thread th = spawn(run_job, &j);
        pause_ms(300);
        double c = now();
        pandoc_cancel(t);
        join(th);
        secs = j.ended - c;
        check(cancelled(j.r) && secs < 2, "during, untrusted, the token inside \"untrusted\": \"Cancelled\"");
        show(j.r, secs);
        pandoc_result_free(j.r);
        free(o);
        pandoc_cancel_token_free(t);
    }
    /* with a time limit too: whichever comes first */
    r = cancel_slow(MD ", \"timeout\": 60", 300, &secs);
    check(cancelled(r) && secs < 2, "during, with a later \"timeout\": \"Cancelled\"");
    pandoc_result_free(r);

    /* after: the result stands; the token then stops the next call */
    {
        int64_t t = pandoc_cancel_token_new();
        o = with_token(MD, t);
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        check(ok_html(r, "<p><em>x</em></p>\n") && pandoc_cancel(t) == 0
              && strcmp(r->output, "<p><em>x</em></p>\n") == 0,
              "after: a finished call keeps its result");
        pandoc_result_free(r);
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        check(cancelled(r), "... and the next call with the token fails");
        pandoc_result_free(r);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* one token, four calls at once: all stop */
    {
        int64_t t = pandoc_cancel_token_new();
        o = with_token(MD, t);
        job j[4];
        thread th[4];
        for (int i = 0; i < 4; i++) {
            j[i] = (job){o, slow, slow_len, NULL, 0, NULL, 0};
            th[i] = spawn(run_job, &j[i]);
        }
        pause_ms(300);
        double c = now();
        pandoc_cancel(t);
        int all = 1;
        double worst = 0;
        for (int i = 0; i < 4; i++) {
            join(th[i]);
            all = all && cancelled(j[i].r);
            if (j[i].ended - c > worst) worst = j[i].ended - c;
            pandoc_result_free(j[i].r);
        }
        check(all && worst < 3, "one token, four calls at once: all stop");
        printf("      the last %.2f s after the cancel\n", worst);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* eight calls at once, each with its own token, one cancelled: the
     * others finish as without */
    {
        int64_t ts[8];
        char *os[8];
        job j[8];
        thread th[8];
        for (int i = 0; i < 8; i++) {
            ts[i] = pandoc_cancel_token_new();
            os[i] = with_token(MD, ts[i]);
            j[i] = i == 0 ? (job){os[i], slow, slow_len, NULL, 0, NULL, 0}
                          : (job){os[i], "*x*\n\n> q\n", 9, NULL, 0, NULL, 0};
            th[i] = spawn(run_job, &j[i]);
        }
        pause_ms(300);
        pandoc_cancel(ts[0]);
        int others = 1;
        for (int i = 0; i < 8; i++) {
            join(th[i]);
            if (i > 0) others = others && ok_html(j[i].r, "<p><em>x</em></p>\n<blockquote>\n<p>q</p>\n</blockquote>\n");
        }
        check(cancelled(j[0].r) && others, "eight calls at once, one cancelled: the others finish");
        for (int i = 0; i < 8; i++) {
            pandoc_result_free(j[i].r);
            free(os[i]);
            pandoc_cancel_token_free(ts[i]);
        }
    }

    /* a callback filter running when the cancel comes: it runs to its end
     * (a foreign call), its answer is dropped, the call fails then */
    {
        pandoc_filter fs[] = {{sleepy, NULL}};
        int64_t t = pandoc_cancel_token_new();
        o = with_token(MD ", \"filters\": [{\"type\": \"callback\", \"index\": 0}]", t);
        atomic_store(&calls, 0);
        job j = {o, "*x*", 3, fs, 1, NULL, 0};
        double s = now();
        thread th = spawn(run_job, &j);
        pause_ms(300);
        pandoc_cancel(t);
        join(th);
        secs = j.ended - s;
        check(cancelled(j.r) && secs >= 0.9 && secs < 3 && atomic_load(&calls) == 1,
              "a callback running: it runs to its end, once, then \"Cancelled\"");
        show(j.r, secs);
        pandoc_result_free(j.r);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* a call the callback makes after the cancel: cancelled with it (it
     * inherits the conversion's token), though it has no token itself */
    {
        pandoc_filter fs[] = {{nested, NULL}};
        int64_t t = pandoc_cancel_token_new();
        o = with_token(MD ", \"filters\": [{\"type\": \"callback\", \"index\": 0}]", t);
        nested_kind[0] = '\0';
        job j = {o, "*x*", 3, fs, 1, NULL, 0};
        thread th = spawn(run_job, &j);
        pause_ms(200);
        pandoc_cancel(t);
        join(th);
        check(cancelled(j.r) && strcmp(nested_kind, "Cancelled") == 0,
              "a call a callback makes after the cancel: \"Cancelled\" too");
        printf("      the callback's call: %s\n", nested_kind);
        pandoc_result_free(j.r);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* ... and one it is making when the cancel comes stops */
    {
        int one = 1;
        pandoc_filter fs[] = {{nested, &one}};
        int64_t t = pandoc_cancel_token_new();
        o = with_token(MD ", \"filters\": [{\"type\": \"callback\", \"index\": 0}]", t);
        nested_kind[0] = '\0';
        job j = {o, "*x*", 3, fs, 1, NULL, 0};
        double s = now();
        thread th = spawn(run_job, &j);
        pause_ms(500);
        pandoc_cancel(t);
        join(th);
        secs = j.ended - s;
        check(cancelled(j.r) && strcmp(nested_kind, "Cancelled") == 0 && secs < 3,
              "a slow call a callback is making stops, then the conversion");
        printf("      the callback's call: %s after %.2f s; all %.2f s\n", nested_kind, nested_secs, secs);
        pandoc_result_free(j.r);
        free(o);
        pandoc_cancel_token_free(t);
    }

    /* a callback cancelling its own conversion */
    {
        pandoc_filter fs[] = {{cancel_itself, NULL}};
        self_token = pandoc_cancel_token_new();
        o = with_token(MD ", \"filters\": [{\"type\": \"callback\", \"index\": 0}]", self_token);
        r = pandoc_convert_filters(o, strlen(o), "*x*", 3, fs, 1);
        check(cancelled(r), "a callback may cancel its own conversion");
        pandoc_result_free(r);
        free(o);
        pandoc_cancel_token_free(self_token);
    }

    /* read_many: every reader stops, and the call fails as a whole; also
     * untrusted, the host's token inside "untrusted" (1.14) */
    for (int host = 0; host < 2; host++) {
        size_t n = 8 * (slow_len + 4) + 200;
        char *rq = malloc(n), *p = rq;
        int64_t t = pandoc_cancel_token_new();
        p += sprintf(p, host ? "{\"options\": {\"from\": \"markdown\", \"untrusted\": {\"cancel-token\": %lld}}, \"inputs\": ["
                             : "{\"options\": {\"from\": \"markdown\", \"cancel-token\": %lld}, \"inputs\": [",
                     (long long)t);
        for (int i = 0; i < 8; i++) p += sprintf(p, "%s\"%.*s\"", i ? ", " : "", (int)slow_len, slow);
        strcpy(p, "]}");
        job j = {rq, NULL, 0, NULL, 0, NULL, 0};
        thread th = spawn(run_read_many, &j);
        pause_ms(300);
        double c = now();
        pandoc_cancel(t);
        join(th);
        secs = j.ended - c;
        check(cancelled(j.r) && secs < 3, host ? "read_many, the token inside \"untrusted\": cancelled"
                                               : "read_many: cancelled, the whole call fails, soon after");
        show(j.r, secs);
        pandoc_result_free(j.r);
        r = pandoc_read_many(rq, strlen(rq));
        check(cancelled(r), "... and the token stays cancelled");
        pandoc_result_free(r);
        pandoc_cancel_token_free(t);
        free(rq);
    }

#ifndef _WIN32
    /* a JSON filter running when the cancel comes: ended and reaped */
    {
        FILE *f = fopen("cancel-sleep.sh", "w");
        fputs("#!/bin/sh\necho $$ > cancel-pid.txt\nexec sleep 30\n", f);
        fclose(f);
        chmod("cancel-sleep.sh", 0755);
        remove("cancel-pid.txt");
        r = cancel_input("\"to\": \"html\", \"filters\": [\"./cancel-sleep.sh\"]", "*x*", 3, 500, &secs);
        int pid = 0;
        f = fopen("cancel-pid.txt", "r");
        if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
        int gone = 0;
        for (int ms = 0; pid > 0 && ms < 5000 && !gone; ms += 20) {
            gone = kill(pid, 0) != 0 && errno == ESRCH;
            if (!gone) pause_ms(20);
        }
        check(cancelled(r) && secs < 3 && pid > 0 && gone, "a JSON filter running: ended and reaped");
        show(r, secs);
        if (pid > 0 && !gone) kill(pid, SIGKILL);
        pandoc_result_free(r);
        remove("cancel-sleep.sh");
        remove("cancel-pid.txt");
    }

    /* fifty cancels: no descriptor left open, no temporary file */
    {
        pandoc_result_free(cancel_slow(MD, 20, &secs));
        int before = open_fds(), all = 1;
        for (int i = 0; i < 50; i++) {
            r = cancel_slow(MD, 5 + i % 7 * 5, &secs);
            all = all && cancelled(r);
            pandoc_result_free(r);
        }
        int after = open_fds();
        printf("      %d descriptors before, %d after\n", before, after);
        check(all && after == before, "fifty cancels: no descriptor left open");
        int left = entries("cancel-tmp");
        check(left == 0, "fifty cancels: no temporary file left");
        if (left != 0) printf("      %d entries in cancel-tmp\n", left);
    }
    unsetenv("TMPDIR");
    rmdir("cancel-tmp");
#endif

    r = pandoc_convert("{\"to\": \"html\"}", 14, "*x*", 3);
    check(ok_html(r, "<p><em>x</em></p>\n"), "the library goes on");
    pandoc_result_free(r);
    free(slow);
    return failures != 0;
}
