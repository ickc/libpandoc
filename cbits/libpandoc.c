/*
 * libpandoc: C entry points. Starts the Haskell runtime on first use and
 * forwards to the functions exported by LibPandoc.hs.
 *
 * Copyright (C) 2026 Kolen Cheung
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define LIBPANDOC_BUILDING 1
#include "libpandoc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <HsFFI.h>
#include <Rts.h>

/* Exported from LibPandoc.hs */
extern pandoc_result *libpandoc_hs_convert(char *options, size_t options_len,
                                           char *input, size_t input_len, int has_input);
extern pandoc_result *libpandoc_hs_convert_args(int argc, char **argv,
                                                char *input, size_t input_len, int has_input);
extern pandoc_result *libpandoc_hs_query(char *query, size_t query_len);
extern pandoc_result *libpandoc_hs_read_many(char *request, size_t request_len);
extern int libpandoc_hs_set_num_threads(int n);
extern int libpandoc_hs_main(int argc, char **argv, char *filters_json, size_t filters_json_len,
                             void *filters, size_t filters_len);
extern pandoc_result *libpandoc_hs_convert_args_filters(int argc, char **argv,
                                                        char *input, size_t input_len, int has_input,
                                                        void *filters, size_t filters_len);
extern pandoc_result *libpandoc_hs_convert_filters(char *options, size_t options_len,
                                                   char *input, size_t input_len, int has_input,
                                                   void *filters, size_t filters_len);

static int init_status = -1;

extern void libpandoc_hs_expand_threads(void);

/* Calls in progress: when a second one starts while another runs, pandoc
 * gets its threads (the capabilities LIBPANDOC_NUM_THREADS or
 * pandoc_set_num_threads ask for, else one per core), once. A call made
 * from inside another, by a filter callback, runs on the same OS thread
 * while the outer one waits: it isn't a second caller, and doesn't count
 * (starting 32 capabilities for it would cost ~34 ms). */
static atomic_int active_calls = 0;
static atomic_int expanded = 0;

#if defined(__wasm__)
#define THREAD_LOCAL /* one thread */
#elif defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif
static THREAD_LOCAL int depth = 0;

/* SIGPIPE: a program pandoc writes to that exits without reading it all
 * (a JSON filter, a PDF engine, Lua's pandoc.pipe) makes the write raise
 * SIGPIPE, which ends a process that hasn't ignored it. GHC's programs
 * ignore it (pandoc's does), but libpandoc's runtime installs no signal
 * handlers, so the host's disposition holds. Instead SIGPIPE is blocked in
 * the threads that run pandoc: a host's thread while it is inside a call,
 * and the runtime's own threads, which inherit it from the thread that
 * starts them (start_runtime, pandoc_set_num_threads, a call). The write
 * then fails with EPIPE, which pandoc handles; a SIGPIPE such a write left
 * pending on the host's thread is taken before its mask is restored.
 * Programs pandoc starts inherit the mask (the process library keeps it),
 * as from Python's process they inherit SIGPIPE ignored. */
#if defined(__wasm__) || defined(_WIN32)
typedef int sigpipe_state;
static void block_sigpipe(sigpipe_state *s) { (void)s; }
static void restore_sigpipe(const sigpipe_state *s) { (void)s; }
#else
#include <pthread.h>
#include <signal.h>
typedef struct { int was_blocked; } sigpipe_state;
static void block_sigpipe(sigpipe_state *s)
{
    sigset_t pipe, old;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, &old);
    s->was_blocked = sigismember(&old, SIGPIPE);
}
static void restore_sigpipe(const sigpipe_state *s)
{
    sigset_t pipe, pending;
    int sig;
    if (s->was_blocked) return;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) sigwait(&pipe, &sig);
    pthread_sigmask(SIG_UNBLOCK, &pipe, NULL);
}
#endif
static THREAD_LOCAL sigpipe_state call_sigpipe;

static int enter(void)
{
    if (depth == 0) block_sigpipe(&call_sigpipe);
    if (pandoc_init() != 0) {
        if (depth == 0) restore_sigpipe(&call_sigpipe);
        return -1;
    }
    if (depth++ == 0 && atomic_fetch_add(&active_calls, 1) >= 1 && !atomic_exchange(&expanded, 1))
        libpandoc_hs_expand_threads();
    return 0;
}

static void leave(void)
{
    if (--depth == 0) {
        atomic_fetch_sub(&active_calls, 1);
        restore_sigpipe(&call_sigpipe);
    }
}

/* A workaround for a bug of GHC's runtime, to remove when libpandoc's GHC
 * has the fix: GHC 9.12.5, 9.14.2 or later (pins.env's GHC_VERSION). Then
 * delete lost_wakeup, RETRY_LOST_WAKEUP and their uses below (each call
 * back to "r = libpandoc_hs_...(...)"); in the header, the paragraphs "A
 * call that fails with the error "Exception", "<<loop>>" is made again"
 * and "fn may be called twice", and README's line; the comment at the top
 * of test/threads.c.
 *
 * GHC 9.10's runtime can lose the wakeup of threads waiting for a top-level
 * value (a CAF: a String literal's, evaluated by stg_unpack_cstring) that
 * another thread is evaluating for the first time, when that thread's stack
 * check fails right after it claimed the value. The waiting threads are
 * then unreachable, and the garbage collector throws them NonTermination:
 * the call fails with "Exception: <<loop>>" although nothing loops (GHC
 * #26324 and #27272). It needs several threads to first use the same
 * values at once, so several first calls in a process: about 1 in 3,000
 * processes making 8 at once (dev/libpandoc-review/LOOP.md in the
 * project). Such a call is made again, up to three times: a value that
 * really loops fails every time. Nothing of a failed attempt reaches the
 * caller (its result is freed; the runtime prints nothing). Safe to make
 * again: the caller's buffers are only read, and a conversion's files are
 * written again. But not a conversion given no input, which may have read
 * the process's standard input: made again, it would read what's left.
 * (A time limit doesn't hide it as a timeout: LibPandoc.hs's 'within'
 * holds the thread by a weak reference.) */
static int lost_wakeup(const pandoc_result *r)
{
    return r && r->status != 0 && r->error_kind && r->error_message &&
           strcmp(r->error_kind, "Exception") == 0 &&
           strcmp(r->error_message, "<<loop>>") == 0;
}

/* r = call, made again while it fails with a lost wakeup and again is
 * true, up to three times more */
#define RETRY_LOST_WAKEUP(r, again, call)                            \
    do {                                                             \
        for (int tries_ = 0;; tries_++) {                            \
            (r) = (call);                                            \
            if (tries_ == 3 || !(again) || !lost_wakeup(r)) break;   \
            pandoc_result_free(r);                                   \
        }                                                            \
    } while (0)

static void start_runtime(void)
{
    /* Options are given as trusted link-time options, and anything from the
     * command line or GHCRTS is ignored: a bad RTS option makes the RTS exit
     * the process, which a library must never do to its host. The RTS must
     * not take over the host's signal handling (Python's KeyboardInterrupt,
     * for one). -A8m is what upstream's pandoc binary uses. -N1: one
     * capability to start with (starting 32 costs ~40 ms, for a program
     * that may never use them); more come when first used in parallel (see
     * enter()). Parallel GC, but -qi1: not waking capabilities that were
     * idle for the last GC, so that one conversion at a time pays nothing
     * for the idle ones. Converting 64 documents of 800 paragraphs on 16
     * threads then takes 1.3 s (sequential GC, -qg: 3.1 s; one thread:
     * 5.4 s either way). Never with -qn, which with -qi crashes GHC's GC
     * (see UPSTREAM.md). */
    static char *args[] = {"libpandoc", NULL};
    int argc = 1;
    char **argv = args;
    RtsConfig conf = defaultRtsConfig;
    conf.rts_opts_enabled = RtsOptsIgnoreAll;
#ifdef __wasm__
    /* the non-threaded runtime: no -N or -q; -H64m as upstream's pandoc.js.
     * -M3584m, a heap limit under wasm32's 4 GiB: with it GHC compacts the
     * oldest generation when it grows large instead of copying it, which
     * needs much less memory. Without it, 79 MB of markdown stalled at the
     * 4 GiB ceiling (88 minutes at 100% CPU, no progress); with it, that
     * converts in 5 minutes, and 59 MB in 193 s instead of 266 (3.0 GiB
     * instead of 3.6). Bigger still (118 MB), the runtime runs out of
     * memory and exits the instance (the host reports it: core.mjs). */
    conf.rts_opts = "-A8m -H64m -M3584m";
#else
    conf.rts_opts = "-A8m -N1 -qi1 --install-signal-handlers=no"
#ifdef _WIN32
                    " --install-seh-handlers=no"
#endif
                    ;
#endif
    sigpipe_state s;
    block_sigpipe(&s); /* the runtime's threads keep SIGPIPE blocked */
    hs_init_ghc(&argc, &argv, conf);
    restore_sigpipe(&s);
    init_status = 0;
}

#if defined(__wasm__)
/* one thread */
int pandoc_init(void)
{
    if (init_status == -1) start_runtime();
    return init_status;
}
#elif defined(_WIN32)
#include <windows.h>
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK start_runtime_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    start_runtime();
    return TRUE;
}
int pandoc_init(void)
{
    InitOnceExecuteOnce(&once, start_runtime_once, NULL, NULL);
    return init_status;
}
#else
#include <pthread.h>
static pthread_once_t once = PTHREAD_ONCE_INIT;
int pandoc_init(void)
{
    pthread_once(&once, start_runtime);
    return init_status;
}
#endif

void pandoc_shutdown(void)
{
    if (init_status == 0) {
        hs_exit();
        init_status = -2;
    }
}

int pandoc_abi_version(void)
{
    return LIBPANDOC_ABI_VERSION;
}

pandoc_result *pandoc_convert(const char *options, size_t options_len,
                              const char *input, size_t input_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, input != NULL,
                      libpandoc_hs_convert((char *)options, options_len,
                                           (char *)input, input_len, input != NULL));
    leave();
    return r;
}

pandoc_result *pandoc_convert_args(int argc, const char *const *argv,
                                   const char *input, size_t input_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, input != NULL,
                      libpandoc_hs_convert_args(argc, (char **)argv,
                                                (char *)input, input_len, input != NULL));
    leave();
    return r;
}

pandoc_result *pandoc_convert_filters(const char *options, size_t options_len,
                                      const char *input, size_t input_len,
                                      const pandoc_filter *filters, size_t filters_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, input != NULL,
                      libpandoc_hs_convert_filters((char *)options, options_len,
                                                   (char *)input, input_len, input != NULL,
                                                   (void *)filters, filters_len));
    leave();
    return r;
}

pandoc_result *pandoc_convert_args_filters(int argc, const char *const *argv,
                                           const char *input, size_t input_len,
                                           const pandoc_filter *filters, size_t filters_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, input != NULL,
                      libpandoc_hs_convert_args_filters(argc, (char **)argv,
                                                        (char *)input, input_len, input != NULL,
                                                        (void *)filters, filters_len));
    leave();
    return r;
}

/* Callback filters: the buffer a filter answers in, and the call itself,
 * for LibPandoc.hs (a C call, so the Haskell side needs no function-pointer
 * wrappers). */
struct pandoc_buffer {
    char *data;
    size_t len;
};

void pandoc_buffer_set(pandoc_buffer *out, const char *data, size_t len)
{
    char *p;
    if (out == NULL) return;
    p = malloc(len + 1);
    if (p == NULL) return;
    if (len) memcpy(p, data, len);
    p[len] = '\0';
    free(out->data);
    out->data = p;
    out->len = len;
}

pandoc_buffer *libpandoc_buffer_new(void)
{
    return calloc(1, sizeof(pandoc_buffer));
}

void libpandoc_buffer_free(pandoc_buffer *b)
{
    if (b == NULL) return;
    free(b->data);
    free(b);
}

const char *libpandoc_buffer_data(const pandoc_buffer *b)
{
    return b->data;
}

size_t libpandoc_buffer_len(const pandoc_buffer *b)
{
    return b->len;
}

#ifdef __wasm__
/* A wasm host can't easily make function pointers, so a filter whose fn is
 * NULL is the host's import libpandoc.filter, told the filter's userdata
 * (the host's own number for it). It answers as a pandoc_filter_fn does,
 * with pandoc_buffer_set. */
__attribute__((import_module("libpandoc"), import_name("filter")))
extern int libpandoc_host_filter(void *userdata,
                                 const char *doc, size_t doc_len,
                                 const char *context, size_t context_len,
                                 pandoc_buffer *out);
#endif

int libpandoc_call_filter(const pandoc_filter *filters, size_t i,
                          const char *doc, size_t doc_len,
                          const char *context, size_t context_len,
                          pandoc_buffer *out)
{
#ifdef __wasm__
    if (filters[i].fn == NULL)
        return libpandoc_host_filter(filters[i].userdata, doc, doc_len,
                                     context, context_len, out);
#else
    if (filters[i].fn == NULL) {
        static const char msg[] = "the filter's function (fn) is NULL";
        pandoc_buffer_set(out, msg, sizeof msg - 1);
        return 1;
    }
#endif
    return filters[i].fn(filters[i].userdata, doc, doc_len, context, context_len, out);
}

int pandoc_set_num_threads(int n)
{
    sigpipe_state s;
    int r;
    if (pandoc_init() != 0) return 0;
    block_sigpipe(&s); /* the threads it may start keep SIGPIPE blocked */
    r = libpandoc_hs_set_num_threads(n);
    restore_sigpipe(&s);
    return r;
}

int pandoc_main(int argc, const char *const *argv,
                const char *filters_json, size_t filters_json_len,
                const pandoc_filter *filters, size_t filters_len)
{
    int r;
    if (enter() != 0) return 1;
    r = libpandoc_hs_main(argc, (char **)argv, (char *)filters_json, filters_json_len,
                             (void *)filters, filters_len);
    /* Lua's print writes through C's stdio, which pandoc's handles don't
     * flush: before the host writes anything else */
    fflush(stdout);
    fflush(stderr);
    leave();
    return r;
}

pandoc_result *pandoc_read_many(const char *request, size_t request_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, 1, libpandoc_hs_read_many((char *)request, request_len));
    leave();
    return r;
}

pandoc_result *pandoc_query(const char *query, size_t query_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    RETRY_LOST_WAKEUP(r, 1, libpandoc_hs_query((char *)query, query_len));
    leave();
    return r;
}

void pandoc_result_free(pandoc_result *result)
{
    if (result == NULL) return;
    free(result->output);
    free(result->error_kind);
    free(result->error_message);
    free(result->log);
    free(result);
}
