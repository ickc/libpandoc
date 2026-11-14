/* Stress test: pandoc_set_num_threads from several threads at once.
 *
 *   cc test/threads.c -Idist/include -Ldist/lib -lpandoc -pthread && ./a.out
 *
 * GHC's setNumCapabilities reads how many capabilities exist before it
 * takes its lock, so a call made while another is adding capabilities
 * leaves some of them held for ever (the next garbage collection waits for
 * them: a hang) or crashes; libpandoc serialises the calls. The window is
 * open only while capabilities are added for the first time, so each try
 * is a new process (this program, run with "grow"): THREADS threads start
 * together and each asks for 2 to one-per-processor threads, a conversion
 * after each request. The parent runs TRIES of them, one after the other.
 * Without the lock, 26 of 30 tries hung on 32 cores, 3 of 30 on 4.
 *
 * And the number is clamped to the processors: more than that, changed
 * while conversions run, crashed GHC's parallel garbage collector (-qi1)
 * in about a third of runs of THREADS threads alternating 1 and 256 with
 * a conversion each time (one caller alone too), lock or not. The parent
 * runs ALTERNATIONS of those ("alternate 1" and "alternate THREADS"), then
 * checks the clamp itself.
 *
 * Exits 0 if every child finishes within TIMEOUT seconds with every call
 * answered as asked; a crash, a hang or a wrong answer fails. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <libpandoc.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef THREADS
#define THREADS 8
#endif
#ifndef ROUNDS
#define ROUNDS 4
#endif
#ifndef TRIES
#define TRIES 100
#endif
#ifndef ALTERNATIONS
#define ALTERNATIONS 4
#endif
#define ALTERNATE_ROUNDS 400
#define TIMEOUT 20

static int most = 2;    /* the number of processors, as pandoc clamps to */
static int setters = 0; /* alternate: how many threads change the number */
static atomic_int started = 0;
static atomic_int finished = 0;
static atomic_int wrong = 0;

static void ask(int want)
{
    int expect = want < most ? want : most;
    if (pandoc_set_num_threads(want) != expect) atomic_fetch_add(&wrong, 1);
}

static void convert(void)
{
    const char *opts = "{\"from\": \"markdown\", \"to\": \"html\"}";
    pandoc_result *r = pandoc_convert(opts, strlen(opts), "*x*", 3);
    if (!r || r->status != 0 || strcmp(r->output, "<p><em>x</em></p>\n") != 0)
        atomic_fetch_add(&wrong, 1);
    pandoc_result_free(r);
}

static void work(long id)
{
    atomic_fetch_add(&started, 1);
    while (atomic_load(&started) < THREADS) ; /* all at once */
    if (setters == 0) {
        /* grow: 2 to one-per-processor threads, each asked for at once */
        for (int k = 0; k < ROUNDS; k++) {
            ask(most > 2 ? 2 + (int)(id + THREADS * k) % (most - 1) : most);
            convert();
        }
    } else {
        /* alternate: 1 and 256 (clamped), with conversions */
        for (int k = 0; k < ALTERNATE_ROUNDS; k++) {
            if (id < setters) ask((k + id) % 2 ? 1 : 256);
            convert();
        }
    }
    atomic_fetch_add(&finished, 1);
}

#ifdef _WIN32
static DWORD WINAPI run(LPVOID arg) { work((long)(intptr_t)arg); return 0; }
static void pause_ms(int ms) { Sleep(ms); }
#else
static void *run(void *arg) { work((long)(intptr_t)arg); return NULL; }
static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

/* One child: 0 if it went well. */
static int child(void)
{
    most = pandoc_set_num_threads(INT_MAX); /* (this needs the clamp) */
    pandoc_set_num_threads(1);
    for (long i = 0; i < THREADS; i++) {
#ifdef _WIN32
        if (!CreateThread(NULL, 0, run, (LPVOID)(intptr_t)i, 0, NULL)) return 2;
#else
        pthread_t t;
        if (pthread_create(&t, NULL, run, (void *)(intptr_t)i) != 0 || pthread_detach(t) != 0)
            return 2;
#endif
    }
    for (int ms = 0; atomic_load(&finished) < THREADS; ms += 10) {
        if (ms >= TIMEOUT * 1000) {
            printf("hung: %d of %d threads still running after %d s\n",
                   THREADS - atomic_load(&finished), THREADS, TIMEOUT);
            fflush(stdout);
            _exit(3); /* not exit: the runtime is stuck */
        }
        pause_ms(10);
    }
    /* then the number of threads is the last one asked for */
    const char *q = "{\"query\": \"num-threads\"}";
    int last = pandoc_set_num_threads(1);
    pandoc_result *r = pandoc_query(q, strlen(q));
    if (last != 1 || !r || r->status != 0 || strcmp(r->output, "1") != 0)
        atomic_fetch_add(&wrong, 1);
    pandoc_result_free(r);
    if (atomic_load(&wrong) != 0) printf("%d calls answered wrongly\n", atomic_load(&wrong));
    return atomic_load(&wrong) != 0;
}

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

/* Run the child "mode" n times; 0 if all went well. */
static int children(const char *self, const char *mode, int n)
{
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, mode);
    for (int i = 0; i < n; i++) {
        int status = system(cmd);
        if (status != 0) {
            printf("      %s: run %d of %d crashed or hung (status %d)\n", mode, i + 1, n, status);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "grow") == 0) return child();
    if (argc > 2 && strcmp(argv[1], "alternate") == 0) {
        setters = atoi(argv[2]);
        return child();
    }
    char self[2048], what[256];
#ifdef _WIN32
    /* the full path, for cmd.exe */
    if (!GetModuleFileNameA(NULL, self, sizeof self)) return 1;
#else
    snprintf(self, sizeof self, "%s", argv[0]);
#endif
    snprintf(what, sizeof what, "pandoc_set_num_threads from %d threads at once, %d runs",
             THREADS, TRIES);
    check(children(self, "grow", TRIES) == 0, what);
    snprintf(what, sizeof what, "1 caller alternating 1 and 256, %d others converting, %d runs",
             THREADS - 1, ALTERNATIONS);
    check(children(self, "alternate 1", ALTERNATIONS) == 0, what);
    snprintf(what, sizeof what, "%d callers alternating 1 and 256, converting, %d runs",
             THREADS, ALTERNATIONS);
    char mode[32];
    snprintf(mode, sizeof mode, "alternate %d", THREADS);
    check(children(self, mode, ALTERNATIONS) == 0, what);

    /* the clamp: at most one thread per processor, and so by default
     * (unless LIBPANDOC_NUM_THREADS says fewer) */
    const char *q = "{\"query\": \"num-threads\"}";
    pandoc_result *r = pandoc_query(q, strlen(q));
    int dflt = r && r->status == 0 ? atoi(r->output) : 0;
    pandoc_result_free(r);
    int processors = pandoc_set_num_threads(INT_MAX);
    r = pandoc_query(q, strlen(q));
    int now = r && r->status == 0 ? atoi(r->output) : 0;
    pandoc_result_free(r);
    printf("      %d processors; %d threads by default\n", processors, dflt);
    check(processors >= 1 && processors < INT_MAX && now == processors
          && pandoc_set_num_threads(processors + 1) == processors
          && pandoc_set_num_threads(processors) == processors
          && pandoc_set_num_threads(0) == 1
          && (getenv("LIBPANDOC_NUM_THREADS") ? dflt <= processors : dflt == processors),
          "pandoc_set_num_threads: clamped to the processors");
    return failures != 0;
}
