/* Stress test: pandoc_set_num_threads from several threads at once.
 *
 *   cc test/threads.c -Idist/include -Ldist/lib -lpandoc -pthread && ./a.out
 *
 * GHC's setNumCapabilities reads how many capabilities exist before it
 * takes its lock, so a call made while another is adding capabilities
 * leaves some of them held for ever (the next garbage collection waits for
 * them: a hang) or crashes; libpandoc serialises the calls. The window is
 * open only while capabilities are added for the first time, so each try
 * is a new process (this program, run with "child"): THREADS threads start
 * together and each asks for 2 to one-per-processor threads, a conversion
 * after each request. The parent runs TRIES of them, one after the other.
 * Without the lock, 26 of 30 tries hung on 32 cores, 3 of 30 on 4.
 *
 * Exits 0 if every try finishes within TIMEOUT seconds with every call
 * answered as asked; a crash, a hang or a wrong answer fails. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define TIMEOUT 20

static int most = 2; /* threads to ask for at most */
static atomic_int started = 0;
static atomic_int finished = 0;
static atomic_int wrong = 0;

static void work(long id)
{
    const char *opts = "{\"from\": \"markdown\", \"to\": \"html\"}";
    atomic_fetch_add(&started, 1);
    while (atomic_load(&started) < THREADS) ; /* all at once */
    for (int k = 0; k < ROUNDS; k++) {
        int want = 2 + (int)(id + THREADS * k) % (most - 1);
        if (pandoc_set_num_threads(want) != want) atomic_fetch_add(&wrong, 1);
        pandoc_result *r = pandoc_convert(opts, strlen(opts), "*x*", 3);
        if (!r || r->status != 0 || strcmp(r->output, "<p><em>x</em></p>\n") != 0)
            atomic_fetch_add(&wrong, 1);
        pandoc_result_free(r);
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

/* One try: 0 if it went well. */
static int child(void)
{
    /* one thread per processor (or LIBPANDOC_NUM_THREADS), not more: more
     * capabilities than processors, changed while conversions run, crash
     * GHC's parallel garbage collector with -qi1, lock or not (another
     * bug, not this test's) */
    const char *q = "{\"query\": \"num-threads\"}";
    pandoc_result *r = pandoc_query(q, strlen(q));
    if (r && r->status == 0 && atoi(r->output) > 2) most = atoi(r->output);
    pandoc_result_free(r);
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
    int last = pandoc_set_num_threads(2);
    r = pandoc_query(q, strlen(q));
    if (last != 2 || !r || r->status != 0 || strcmp(r->output, "2") != 0)
        atomic_fetch_add(&wrong, 1);
    pandoc_result_free(r);
    if (atomic_load(&wrong) != 0) printf("%d calls answered wrongly\n", atomic_load(&wrong));
    return atomic_load(&wrong) != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "child") == 0) return child();
    char self[2048], cmd[4096];
#ifdef _WIN32
    /* the full path, for cmd.exe */
    if (!GetModuleFileNameA(NULL, self, sizeof self)) return 1;
#else
    snprintf(self, sizeof self, "%s", argv[0]);
#endif
    snprintf(cmd, sizeof cmd, "\"%s\" child", self);
    for (int i = 0; i < TRIES; i++) {
        int status = system(cmd);
        if (status != 0) {
            printf("FAIL: pandoc_set_num_threads from %d threads at once: try %d of %d"
                   " crashed or hung (status %d)\n", THREADS, i + 1, TRIES, status);
            return 1;
        }
    }
    printf("ok  : pandoc_set_num_threads from %d threads at once, %d tries\n", THREADS, TRIES);
    return 0;
}
