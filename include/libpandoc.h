/*
 * libpandoc: pandoc as a C library.
 *
 * Copyright (C) 2026 Kolen Cheung
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The ABI is deliberately small. Everything that follows pandoc's own
 * evolution crosses it as bytes in formats pandoc already defines and
 * versions:
 *
 *   - conversion options are a JSON object in pandoc's defaults-file format
 *     (https://pandoc.org/MANUAL.html#defaults-files), the same contract as
 *     upstream's pandoc.wasm `convert`;
 *   - the document AST is pandoc's JSON (`-t json` / `-f json`), versioned by
 *     its "pandoc-api-version";
 *   - queries (formats, extensions, templates, the AST schema) are JSON.
 *
 * All strings are UTF-8. Buffers passed in are borrowed; results are owned
 * by the caller and released with pandoc_result_free.
 */
#ifndef LIBPANDOC_H
#define LIBPANDOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(LIBPANDOC_BUILDING)
#    define LIBPANDOC_API __declspec(dllexport)
#  else
#    define LIBPANDOC_API __declspec(dllimport)
#  endif
#else
#  define LIBPANDOC_API __attribute__((visibility("default")))
#endif

/* Bumped only when the functions or struct below change incompatibly. */
#define LIBPANDOC_ABI_VERSION 1

typedef struct pandoc_result {
    /* 0 on success, nonzero on failure. */
    int status;
    /* Output bytes (what pandoc would write to stdout). Always followed by a
     * NUL that is not counted in output_len. Empty when the options name an
     * output file. */
    char *output;
    size_t output_len;
    /* On failure, NUL-terminated: the pandoc error constructor (e.g.
     * "PandocParseError", "PandocUnknownReaderError") or "Exception" for
     * anything else, and the message pandoc would print. NULL on success. */
    char *error_kind;
    char *error_message;
    /* NUL-terminated JSON array of pandoc's log messages (warnings and
     * info), as written by --log. Never NULL. */
    char *log;
} pandoc_result;

/* Start the Haskell runtime. Safe to call more than once and from any
 * thread; every other function calls it implicitly. The runtime is started
 * without installing signal handlers, so the host keeps SIGINT etc. It
 * cannot be restarted after pandoc_shutdown. Returns 0 on success. */
LIBPANDOC_API int pandoc_init(void);

/* Stop the Haskell runtime. Optional; call at most once, at process end. */
LIBPANDOC_API void pandoc_shutdown(void);

/* LIBPANDOC_ABI_VERSION of the loaded library. */
LIBPANDOC_API int pandoc_abi_version(void);

/* Convert, like running `pandoc` with a defaults file.
 *
 * options: JSON object in defaults-file format, e.g.
 *          {"from": "markdown", "to": "html", "standalone": true}
 * input:   bytes to use as standard input, or NULL to use the options'
 *          input-files (or, with neither, the process's real stdin, as the
 *          pandoc CLI would).
 *
 * Output goes to result->output unless the options set output-file. Captured
 * output has LF line endings unless the options set "eol"; files get
 * pandoc's default (native), as with the CLI. */
LIBPANDOC_API pandoc_result *pandoc_convert(const char *options, size_t options_len,
                                            const char *input, size_t input_len);

/* Convert, like running `pandoc` with command-line arguments (argv[0] is
 * NOT the program name; pass only the arguments). Informational options
 * (--version, --list-*, --print-default-*) are rejected: use pandoc_query. */
LIBPANDOC_API pandoc_result *pandoc_convert_args(int argc, const char *const *argv,
                                                 const char *input, size_t input_len);

/* Ask pandoc for information. query is a JSON object with a "query" key:
 *   {"query": "version"}                  pandoc version, e.g. "3.11"
 *   {"query": "api-version"}              pandoc-types API version, [1,23,1]
 *   {"query": "input-formats"}            list of reader names
 *   {"query": "output-formats"}           list of writer names
 *   {"query": "highlight-languages"}      list of languages
 *   {"query": "highlight-styles"}         list of style names
 *   {"query": "extensions-for-format", "format": F}
 *                                         {extension: enabled-by-default}
 *   {"query": "default-template", "format": F}
 *                                         template text
 *   {"query": "ast-schema"}               the AST type schema (see README)
 * The answer is JSON, in result->output. */
LIBPANDOC_API pandoc_result *pandoc_query(const char *query, size_t query_len);

LIBPANDOC_API void pandoc_result_free(pandoc_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LIBPANDOC_H */
