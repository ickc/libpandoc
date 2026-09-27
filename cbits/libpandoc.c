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
 * pandoc_set_num_threads ask for, else one per core), once. */
static atomic_int active_calls = 0;
static atomic_int expanded = 0;

static int enter(void)
{
    if (pandoc_init() != 0) return -1;
    if (atomic_fetch_add(&active_calls, 1) >= 1 && !atomic_exchange(&expanded, 1))
        libpandoc_hs_expand_threads();
    return 0;
}

static void leave(void)
{
    atomic_fetch_sub(&active_calls, 1);
}

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
    conf.rts_opts = "-A8m -N1 -qi1 --install-signal-handlers=no"
#ifdef _WIN32
                    " --install-seh-handlers=no"
#endif
                    ;
    hs_init_ghc(&argc, &argv, conf);
    init_status = 0;
}

#ifdef _WIN32
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
    r = libpandoc_hs_convert((char *)options, options_len,
                                (char *)input, input_len, input != NULL);
    leave();
    return r;
}

pandoc_result *pandoc_convert_args(int argc, const char *const *argv,
                                   const char *input, size_t input_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    r = libpandoc_hs_convert_args(argc, (char **)argv,
                                     (char *)input, input_len, input != NULL);
    leave();
    return r;
}

pandoc_result *pandoc_convert_filters(const char *options, size_t options_len,
                                      const char *input, size_t input_len,
                                      const pandoc_filter *filters, size_t filters_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    r = libpandoc_hs_convert_filters((char *)options, options_len,
                                        (char *)input, input_len, input != NULL,
                                        (void *)filters, filters_len);
    leave();
    return r;
}

pandoc_result *pandoc_convert_args_filters(int argc, const char *const *argv,
                                           const char *input, size_t input_len,
                                           const pandoc_filter *filters, size_t filters_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    r = libpandoc_hs_convert_args_filters(argc, (char **)argv,
                                             (char *)input, input_len, input != NULL,
                                             (void *)filters, filters_len);
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

int libpandoc_call_filter(const pandoc_filter *filters, size_t i,
                          const char *doc, size_t doc_len,
                          const char *context, size_t context_len,
                          pandoc_buffer *out)
{
    return filters[i].fn(filters[i].userdata, doc, doc_len, context, context_len, out);
}

int pandoc_set_num_threads(int n)
{
    if (pandoc_init() != 0) return 0;
    return libpandoc_hs_set_num_threads(n);
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
    r = libpandoc_hs_read_many((char *)request, request_len);
    leave();
    return r;
}

pandoc_result *pandoc_query(const char *query, size_t query_len)
{
    pandoc_result *r;
    if (enter() != 0) return NULL;
    r = libpandoc_hs_query((char *)query, query_len);
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
