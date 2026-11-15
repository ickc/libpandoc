/* The time limit ("timeout" in the options): a call past it fails with the
 * error kind "Timeout", the process goes on, and nothing stays behind (the
 * call's temporary files, file descriptors, a JSON filter's process, the
 * threads). The files are written (and removed) in the current directory.
 *
 *   cc test/limits.c -Idist/include -Ldist/lib -lpandoc -pthread && ./a.out
 *
 * The slow input is 100,000 '>' read as markdown: block quotes nested that
 * deep take pandoc minutes (quadratic; 20,000 take 11 s), so each limit
 * below fires on any machine.
 *
 * Not on Windows: the temporary directory (TMPDIR), the descriptors and the
 * JSON filter (a shell script). The descriptors only where /proc/self/fd or
 * /dev/fd lists them (Linux, macOS). */
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
#endif

static char *slow; /* the slow input */
static size_t slow_len;

static int timed_out(const pandoc_result *r)
{
    return r && r->status != 0 && r->error_kind && strcmp(r->error_kind, "Timeout") == 0;
}

/* a conversion of the slow input under options o; its time in *secs */
static pandoc_result *convert_slow(const char *o, double *secs)
{
    double t = now();
    pandoc_result *r = pandoc_convert(o, strlen(o), slow, slow_len);
    *secs = now() - t;
    return r;
}

static int ok_html(const pandoc_result *r)
{
    return r && r->status == 0 && strcmp(r->output, "<p><em>x</em></p>\n") == 0;
}

static pandoc_result *convert_x(void)
{
    const char *o = "{\"to\": \"html\"}";
    return pandoc_convert(o, strlen(o), "*x*", 3);
}

/* A callback filter that takes a second: not interrupted (a foreign call),
 * the limit fires when it returns. */
static int sleepy(void *userdata, const char *doc, size_t doc_len,
                  const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)context; (void)context_len;
    pause_ms(1000);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

#ifndef _WIN32
/* the number of open file descriptors, or -1 */
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

/* the entries of a directory, or -1 */
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

/* The parallel workload after the timeouts: THREADS threads converting at
 * once, each ROUNDS times. */
#define THREADS 8
#define ROUNDS 4
static atomic_int started = 0, finished = 0, wrong = 0;

static void work(void)
{
    atomic_fetch_add(&started, 1);
    while (atomic_load(&started) < THREADS) ;
    for (int k = 0; k < ROUNDS; k++) {
        pandoc_result *r = convert_x();
        if (!ok_html(r)) atomic_fetch_add(&wrong, 1);
        pandoc_result_free(r);
    }
    atomic_fetch_add(&finished, 1);
}

#ifdef _WIN32
static DWORD WINAPI run(LPVOID arg) { (void)arg; work(); return 0; }
#else
static void *run(void *arg) { (void)arg; work(); return NULL; }
#endif

/* 0 if all THREADS threads finished within 60 s with the right output */
static int parallel(void)
{
    for (int i = 0; i < THREADS; i++) {
#ifdef _WIN32
        if (!CreateThread(NULL, 0, run, NULL, 0, NULL)) return 2;
#else
        pthread_t t;
        if (pthread_create(&t, NULL, run, NULL) != 0 || pthread_detach(t) != 0) return 2;
#endif
    }
    for (int ms = 0; atomic_load(&finished) < THREADS; ms += 10) {
        if (ms >= 60000) {
            printf("hung: %d of %d threads still running after 60 s\n",
                   THREADS - atomic_load(&finished), THREADS);
            fflush(stdout);
            _exit(3); /* not exit: the runtime is stuck */
        }
        pause_ms(10);
    }
    return atomic_load(&wrong);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    slow_len = 100000;
    slow = malloc(slow_len);
    memset(slow, '>', slow_len);
    pandoc_result *r;
    double secs;

#ifndef _WIN32
    /* this test's own temporary directory, to see that it is left empty */
    mkdir("limits-tmp", 0700);
    setenv("TMPDIR", "limits-tmp", 1);
#endif

    /* a conversion past its limit: "Timeout", soon after it */
    r = convert_slow("{\"from\": \"markdown\", \"to\": \"html\", \"timeout\": 0.5}", &secs);
    check(timed_out(r) && has(r->error_message, "time limit") && has(r->error_message, "0.5")
          && strcmp(r->log, "[]") == 0 && secs >= 0.5 && secs < 10,
          "convert: past its time limit, a \"Timeout\" error");
    show(r, secs);
    pandoc_result_free(r);
    r = convert_x();
    check(ok_html(r), "convert: the next call works");
    pandoc_result_free(r);

    /* within the limit, as without one; 0 is none; and what isn't a limit */
    {
        const char *o = "{\"to\": \"html\", \"timeout\": 30}";
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        check(ok_html(r), "convert: within its limit, as without one");
        pandoc_result_free(r);
        const char *zero = "{\"to\": \"html\", \"timeout\": 0}";
        r = pandoc_convert(zero, strlen(zero), "*x*", 3);
        check(ok_html(r), "convert: \"timeout\": 0 is none");
        pandoc_result_free(r);
        const char *bad[] = {
            "{\"to\": \"html\", \"timeout\": -1}",
            "{\"to\": \"html\", \"timeout\": \"2\"}",
            "{\"to\": \"html\", \"timeout\": null}",
        };
        int all = 1;
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            r = pandoc_convert(bad[i], strlen(bad[i]), "*x*", 3);
            all = all && r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0
                  && has(r->error_message, "timeout");
            if (i == 0) show(r, 0);
            pandoc_result_free(r);
        }
        check(all, "convert: a \"timeout\" that isn't seconds (0 or more) is a PandocOptionError");
    }

    /* untrusted code may set it (a host sets it for such code: the header) */
    r = convert_slow("{\"untrusted\": true, \"from\": \"markdown\", \"to\": \"html\", \"timeout\": 0.3}", &secs);
    check(timed_out(r), "convert: \"timeout\" with \"untrusted\"");
    pandoc_result_free(r);

    /* read_many: the limit is the call's; an input not read by then is a
     * "Timeout" in its place, the others are read */
    {
        size_t n = slow_len + 200;
        char *rq = malloc(n);
        snprintf(rq, n, "{\"options\": {\"from\": \"markdown\", \"timeout\": 0.5}, "
                 "\"inputs\": [\"*a*\", \"%.*s\", \"b\"]}", (int)slow_len, slow);
        double t = now();
        r = pandoc_read_many(rq, strlen(rq));
        secs = now() - t;
        const char *err = r && r->output ? strstr(r->output, "{\"error\":{\"kind\":\"Timeout\"") : NULL;
        const char *b = r && r->output ? strstr(r->output, "\"c\":\"b\"") : NULL;
        check(r && r->status == 0 && err && b && err < b && has(r->output, "\"Emph\"")
              && secs >= 0.5 && secs < 10,
              "read_many: an input not read within the limit is a \"Timeout\" in its place");
        show(r, secs);
        if (r) printf("      %.160s\n", err ? err : r->output);
        pandoc_result_free(r);
        free(rq);
        const char *bad = "{\"options\": {\"timeout\": \"1\"}, \"inputs\": [\"a\"]}";
        r = pandoc_read_many(bad, strlen(bad));
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
              "read_many: a \"timeout\" that isn't seconds is a PandocOptionError");
        pandoc_result_free(r);
    }

    /* a callback filter isn't interrupted: the limit fires when it returns */
    {
        pandoc_filter fs[] = {{sleepy, NULL}};
        const char *o = "{\"to\": \"html\", \"timeout\": 0.2, "
                        "\"filters\": [{\"type\": \"callback\", \"index\": 0}]}";
        double t = now();
        r = pandoc_convert_filters(o, strlen(o), "*x*", 3, fs, 1);
        secs = now() - t;
        check(timed_out(r) && secs >= 1.0 && secs < 10,
              "a callback filter runs to its end, then the call is a \"Timeout\"");
        show(r, secs);
        pandoc_result_free(r);
    }

#ifndef _WIN32
    /* a JSON filter past the limit is ended (SIGTERM) and reaped: no
     * process left running, and no zombie */
    {
        FILE *f = fopen("limits-sleep.sh", "w");
        fputs("#!/bin/sh\necho $$ > limits-pid.txt\nexec sleep 30\n", f);
        fclose(f);
        chmod("limits-sleep.sh", 0755);
        remove("limits-pid.txt");
        const char *o = "{\"to\": \"html\", \"timeout\": 0.5, \"filters\": [\"./limits-sleep.sh\"]}";
        double t = now();
        r = pandoc_convert(o, strlen(o), "*x*", 3);
        secs = now() - t;
        int pid = 0;
        f = fopen("limits-pid.txt", "r");
        if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
        /* reaped by a thread of the runtime's, a moment later: kill(pid, 0)
         * succeeds as long as the process exists, also as a zombie */
        int gone = 0;
        for (int ms = 0; pid > 0 && ms < 5000 && !gone; ms += 20) {
            gone = kill(pid, 0) != 0 && errno == ESRCH;
            if (!gone) pause_ms(20);
        }
        check(timed_out(r) && secs < 10 && pid > 0 && gone,
              "a JSON filter past the limit is ended and reaped");
        show(r, secs);
        if (pid > 0 && !gone) kill(pid, SIGKILL);
        pandoc_result_free(r);
        remove("limits-sleep.sh");
        remove("limits-pid.txt");
    }

    /* a hundred timeouts leave no descriptor open and no temporary file;
     * (one first, so that what the runtime opens once, for its timers, is
     * open already) */
    {
        double s;
        const char *o = "{\"from\": \"markdown\", \"to\": \"html\", \"timeout\": 0.02}";
        pandoc_result_free(convert_slow(o, &s));
        int before = open_fds(), all = 1;
        double t = now();
        for (int i = 0; i < 100; i++) {
            r = convert_slow(o, &s);
            all = all && timed_out(r);
            pandoc_result_free(r);
        }
        secs = now() - t;
        int after = open_fds();
        printf("      %d descriptors before, %d after; %.2f s\n", before, after, secs);
        check(all && after == before, "a hundred timeouts: no descriptor left open");
        int left = entries("limits-tmp");
        check(left == 0, "a hundred timeouts: no temporary file left");
        if (left != 0) printf("      %d entries in limits-tmp\n", left);
    }
#else
    for (int i = 0; i < 100; i++) {
        double s;
        pandoc_result_free(convert_slow("{\"from\": \"markdown\", \"timeout\": 0.02}", &s));
    }
#endif

    /* then a parallel workload: the threads aren't stuck */
    check(parallel() == 0, "after the timeouts, 8 threads converting at once all finish");

#ifndef _WIN32
    unsetenv("TMPDIR");
    rmdir("limits-tmp");
#endif
    free(slow);
    return failures != 0;
}
