/*
 * libpandoc: C entry points. Starts the Haskell runtime on first use and
 * forwards to the functions exported by LibPandoc.hs.
 *
 * Copyright (C) 2026 Kolen Cheung
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define LIBPANDOC_BUILDING 1
#include "libpandoc.h"

#include <stdlib.h>
#include <HsFFI.h>
#include <Rts.h>

/* Exported from LibPandoc.hs */
extern pandoc_result *libpandoc_hs_convert(char *options, size_t options_len,
                                           char *input, size_t input_len, int has_input);
extern pandoc_result *libpandoc_hs_convert_args(int argc, char **argv,
                                                char *input, size_t input_len, int has_input);
extern pandoc_result *libpandoc_hs_query(char *query, size_t query_len);

static int init_status = -1;

static void start_runtime(void)
{
    /* Options are given as trusted link-time options, and anything from the
     * command line or GHCRTS is ignored: a bad RTS option makes the RTS exit
     * the process, which a library must never do to its host. The RTS must
     * not take over the host's signal handling (Python's KeyboardInterrupt,
     * for one). -A8m is what upstream's pandoc binary uses. */
    static char *args[] = {"libpandoc", NULL};
    int argc = 1;
    char **argv = args;
    RtsConfig conf = defaultRtsConfig;
    conf.rts_opts_enabled = RtsOptsIgnoreAll;
    conf.rts_opts = "-A8m --install-signal-handlers=no"
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
    if (pandoc_init() != 0) return NULL;
    return libpandoc_hs_convert((char *)options, options_len,
                                (char *)input, input_len, input != NULL);
}

pandoc_result *pandoc_convert_args(int argc, const char *const *argv,
                                   const char *input, size_t input_len)
{
    if (pandoc_init() != 0) return NULL;
    return libpandoc_hs_convert_args(argc, (char **)argv,
                                     (char *)input, input_len, input != NULL);
}

pandoc_result *pandoc_query(const char *query, size_t query_len)
{
    if (pandoc_init() != 0) return NULL;
    return libpandoc_hs_query((char *)query, query_len);
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
