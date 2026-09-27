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
 *   - queries (formats, extensions, templates, versions) are JSON.
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

/* The version of this C interface, libpandoc's own. MAJOR changes only when
 * something below changes incompatibly; MINOR when something is added
 * (1.1: callback filters). The pandoc inside has its own version, and so
 * does its document AST: pandoc_query "version" and "api-version". */
#define LIBPANDOC_ABI_VERSION_MAJOR 1
#define LIBPANDOC_ABI_VERSION_MINOR 1
#define LIBPANDOC_ABI_VERSION (LIBPANDOC_ABI_VERSION_MAJOR * 1000 + LIBPANDOC_ABI_VERSION_MINOR)

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

/* LIBPANDOC_ABI_VERSION of the loaded library (major * 1000 + minor). A
 * program built against major.minor works with it if the major is the same
 * and the minor at least as large. */
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

/* Filters implemented by the caller, run inside a conversion.
 *
 * A conversion's "filters" option may list, among pandoc's own (Lua and JSON
 * filter paths, "citeproc"), entries {"type": "callback", "index": i}. At
 * that point of the conversion pandoc calls filters[i].fn, as it would run
 * a JSON filter, but in this process and on the thread that called
 * pandoc_convert_filters. Everything else about the conversion is as with
 * pandoc_convert, so it is one pandoc run: what a reader keeps in memory
 * (such as images embedded in a docx) reaches the writer.
 *
 * fn receives:
 *   doc:     the document, as pandoc's JSON;
 *   context: a JSON object:
 *            "format": the output format's name, what a JSON filter gets as
 *              its first argument, e.g. "html5";
 *            "input-format", "output-format": the formats pandoc reads and
 *              writes, with extensions, as pandoc decided them (from the
 *              options, else the file names), e.g. "commonmark_x-smart",
 *              "html5+smart". JSON filters aren't told these;
 *            "reader-options": the reader's options, what a JSON filter gets
 *              in PANDOC_READER_OPTIONS.
 * Neither is NUL-terminated, and both are valid only during the call.
 *
 * fn returns 0 and puts the new document, as pandoc's JSON, in out with
 * pandoc_buffer_set; or returns nonzero and puts a UTF-8 error message in
 * out, which fails the conversion with a PandocFilterError.
 *
 * fn may call libpandoc again (pandoc_convert, pandoc_query, ...), for
 * example to parse a fragment of text; those are separate conversions. */
typedef struct pandoc_buffer pandoc_buffer;

typedef int (*pandoc_filter_fn)(void *userdata,
                                const char *doc, size_t doc_len,
                                const char *context, size_t context_len,
                                pandoc_buffer *out);

typedef struct pandoc_filter {
    pandoc_filter_fn fn;
    void *userdata;
} pandoc_filter;

/* Set the contents of out (copied). May be called more than once; the last
 * call wins. */
LIBPANDOC_API void pandoc_buffer_set(pandoc_buffer *out, const char *data, size_t len);

/* pandoc_convert, with filters the options refer to as
 * {"type": "callback", "index": i}, i < filters_len. */
LIBPANDOC_API pandoc_result *pandoc_convert_filters(const char *options, size_t options_len,
                                                    const char *input, size_t input_len,
                                                    const pandoc_filter *filters,
                                                    size_t filters_len);

/* pandoc_convert_args, with filters the arguments refer to as the Lua
 * filters --lua-filter=libpandoc:callback/i (i < filters_len). A
 * pandoc-compatible command line uses this to run filters written in its
 * own language in process, wherever the user put them among the others. */
LIBPANDOC_API pandoc_result *pandoc_convert_args_filters(int argc, const char *const *argv,
                                                         const char *input, size_t input_len,
                                                         const pandoc_filter *filters,
                                                         size_t filters_len);

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
 * The answer is JSON, in result->output. */
LIBPANDOC_API pandoc_result *pandoc_query(const char *query, size_t query_len);

LIBPANDOC_API void pandoc_result_free(pandoc_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LIBPANDOC_H */
