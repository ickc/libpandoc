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
 * (1.1: callback filters; 1.2: pandoc_read_many; 1.3: pandoc_main;
 * 1.4: pandoc_set_num_threads; 1.5: pandoc lua in pandoc_main; 1.6:
 * pandoc_read_many's "sandbox"; 1.7: "untrusted"). The pandoc inside has
 * its own version, and so does its document AST: pandoc_query "version"
 * and "api-version". */
#define LIBPANDOC_ABI_VERSION_MAJOR 1
#define LIBPANDOC_ABI_VERSION_MINOR 7
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

/* Threads. pandoc runs on a number of threads (Haskell capabilities), so
 * that calls from different host threads, and pandoc_read_many, run in
 * parallel: by default one per logical core the process may use, or
 * LIBPANDOC_NUM_THREADS (read when the runtime starts, as OMP_NUM_THREADS).
 * pandoc_set_num_threads changes it from now on (n >= 1) and returns the
 * new number; {"query": "num-threads"} tells the current one. */
LIBPANDOC_API int pandoc_set_num_threads(int n);

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
 * pandoc's default (native), as with the CLI.
 *
 * "untrusted": true (not a pandoc option, libpandoc's) is for options from
 * code the host doesn't trust, such as a wasm filter calling pandoc: only
 * options that read no files, write none, fetch nothing and run no
 * programs are accepted (the reading ones, as pandoc_read_many's below,
 * and those that only shape the output, such as "to", "standalone",
 * "toc", "html-math-method", "variables"), formats are names (not Lua
 * readers or writers, and not "pdf"), input must be given, and "sandbox"
 * is on. Anything else fails with a PandocOptionError naming it. A host
 * sets "untrusted": true on whatever such code passes (replacing its own
 * "untrusted"), and the list lives here, the same for every host. */
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

/* The pandoc command, in this process: what running `pandoc` with these
 * arguments would do. argv[0] is the program's name (for pandoc's usage
 * messages). pandoc reads standard input and writes standard output and
 * standard error itself, answers informational options (--version,
 * --list-*, -D, --help, ...), and reports errors, all as the command does;
 * the return value is the exit status pandoc would exit with.
 *
 * filters_json, if not NULL, is a JSON array (as the "filters" option)
 * replacing the filters pandoc found in the arguments and defaults files;
 * it may name filters[i] as {"type": "callback", "index": i}. A
 * pandoc-compatible command line uses {"query": "parse-args"} to see those
 * filters, then this to run some of them in process.
 *
 * `lua` as the first argument (or a program named pandoc-lua) runs pandoc
 * as a Lua interpreter, as `pandoc lua` does. A script's os.exit exits the
 * process, as it would pandoc's. `server` is not supported (status 4). */
LIBPANDOC_API int pandoc_main(int argc, const char *const *argv,
                              const char *filters_json, size_t filters_json_len,
                              const pandoc_filter *filters, size_t filters_len);

/* Read many texts at once: each on its own (nothing carries over between
 * them), all in parallel, with one reader set up once. For filters that
 * parse many fragments, such as table cells; much cheaper than a
 * pandoc_convert per fragment.
 *
 * request: a JSON object
 *   {"options": {"from": "commonmark_x", "tab-stop": 8, ...},
 *    "inputs": ["*a*", "b", ...]}
 * where "options" are defaults-file keys, of which those that affect
 * reading apply (from, tab-stop, preserve-tabs, indented-code-classes,
 * default-image-extension, track-changes, strip-comments, abbreviations,
 * data-dir, resource-path, sandbox). With "sandbox": true, as pandoc's
 * --sandbox, readers read no files (LaTeX's \input, RST's include, ...):
 * for untrusted texts. "untrusted": true, as for pandoc_convert, accepts only
 * the reading options that read no files (not data-dir, resource-path,
 * abbreviations), for untrusted code. Each text is prepared as pandoc prepares its
 * input (tabs expanded unless preserve-tabs, carriage returns dropped).
 *
 * result->output: a JSON array with, for each input, its document as
 * pandoc's JSON, or {"error": {"kind": ..., "message": ...}}. The call itself
 * fails only for a malformed request or an unknown format. */
LIBPANDOC_API pandoc_result *pandoc_read_many(const char *request, size_t request_len);

/* Ask pandoc for information. query is a JSON object with a "query" key:
 *   {"query": "version"}                  pandoc version, e.g. "3.12"
 *   {"query": "api-version"}              pandoc-types API version, [1,23,1]
 *   {"query": "input-formats"}            list of reader names
 *   {"query": "output-formats"}           list of writer names
 *   {"query": "highlight-languages"}      list of languages
 *   {"query": "highlight-styles"}         list of style names
 *   {"query": "extensions-for-format", "format": F}
 *                                         {extension: enabled-by-default}
 *   {"query": "default-template", "format": F}
 *                                         template text
 *   {"query": "num-threads"}             the number of threads pandoc uses
 *   {"query": "parse-args", "args": [...]}
 *                                         what pandoc makes of these
 *                                         command-line arguments: {"filters":
 *                                         [...]} (defaults files included),
 *                                         or {"informational": "VersionInfo"},
 *                                         or {"subcommand": "lua"} (or
 *                                         "server"), whose arguments aren't
 *                                         pandoc's options
 * The answer is JSON, in result->output. With "untrusted": true, as for
 * pandoc_convert, only those that read nothing of the user's: not
 * default-template nor parse-args. */
LIBPANDOC_API pandoc_result *pandoc_query(const char *query, size_t query_len);

LIBPANDOC_API void pandoc_result_free(pandoc_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LIBPANDOC_H */
