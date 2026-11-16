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
 * All strings are UTF-8 (but pandoc_main's arguments). Buffers passed in
 * are borrowed; results are owned by the caller and released with
 * pandoc_result_free.
 *
 * No error ends or crashes the host process: every Haskell exception
 * becomes an error result, or pandoc_main's status. What can still end it
 * (running out of memory, a Lua filter's os.exit), and what stays the
 * host's business:
 *   - a Lua filter or script runs with Lua's standard library, so its
 *     os.exit ends the process (as it ends pandoc's), and its print and
 *     io.write write to the process's standard output;
 *   - pandoc_main writes to file descriptors 1 and 2, and so may filters
 *     (a JSON filter's standard error is the process's), so they must be
 *     open when the runtime starts: a descriptor the host closed is taken
 *     by the runtime's own files, and pandoc's messages then go there
 *     (pandoc_main with an unknown option and descriptor 2 closed hangs);
 *   - a callback filter must return: it must not unwind across pandoc
 *     (longjmp, a C++ exception, a Rust panic);
 *   - memory: running out of it ends the process, as the runtime can't
 *     recover from that inside a library; and there is no memory limit for
 *     a call (see "timeout" at pandoc_convert);
 *   - time: a call runs until it ends, unless its options give it a
 *     "timeout" (pandoc_convert, pandoc_read_many) or a "cancel-token"
 *     another thread cancels (pandoc_cancel), which stop pandoc's own
 *     code, not code in a foreign call (callbacks, Lua).
 *
 * Versions: a program or binding built against this header's MAJOR.MINOR
 * works with a library whose pandoc_abi_version() has the same MAJOR and a
 * MINOR at least as large, and should check it when it loads the library
 * (or libpandoc.wasm, which exports pandoc_abi_version too), against the
 * MINOR whose functions and meanings it relies on. MINOR also marks changes
 * of meaning (the list at LIBPANDOC_ABI_VERSION).
 *
 * Untrusted input, and three meanings of "sandbox":
 *   - "untrusted": true (libpandoc's option) is about options: from code
 *     the host doesn't trust (a wasm filter calling pandoc), only those
 *     that read, write, fetch and run nothing are accepted (details at
 *     pandoc_convert). It doesn't bound a document's time or memory: for
 *     documents from someone you don't trust, give the conversion a
 *     "timeout" and memory of its own (libpandoc.wasm, a separate process,
 *     a container), with "sandbox" (below) or "untrusted" so that it reads
 *     nothing the document names;
 *   - "sandbox": true in pandoc_convert's options is pandoc's --sandbox,
 *     with citeproc sandboxed too (pandoc_convert);
 *   - "sandbox": true in pandoc_read_many's options runs each reader in
 *     pandoc's sandbox: it reads no files;
 *   - "sandbox": {"read": [...], "write": [...]} is the bindings' (the
 *     directory sandbox: the whole conversion in libpandoc.wasm, seeing
 *     only those directories); libpandoc itself refuses it (it expects
 *     true or false), so a host that lacks it fails rather than ignore it.
 *
 * The options are pandoc's defaults-file keys, but not "defaults" (an
 * unknown option: a defaults file can't name others here), and with no
 * ${HOME}, ${.} or ${USERDATA} expanded in paths, which a defaults file
 * read by pandoc has (pandoc_convert_args with --defaults has both).
 * Relative paths resolve against the process's working directory (in
 * libpandoc.wasm, wasi-libc's: chdir), as pandoc's do, but for one thing: a
 * document given as input (not as a file) is read from a file in the
 * call's temporary directory (pandoc_convert), so RST's include and Org's
 * #+INCLUDE resolve against that directory and find nothing, and
 * $sourcefile$ and messages name that file. Unknown keys are refused (a
 * PandocOptionError); pandoc_read_many uses only the keys that affect
 * reading and ignores the rest of pandoc's.
 *
 * Paths starting with "libpandoc:" are libpandoc's (callback filters,
 * libpandoc:callback/i, also as a Lua filter's path in the options; JSON
 * filters and citeproc routed through it): don't name files so.
 */
#ifndef LIBPANDOC_H
#define LIBPANDOC_H

#include <stddef.h>
#include <stdint.h>

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
 * pandoc_read_many's "sandbox"; 1.7: "untrusted"; 1.8: "sandbox" covers
 * citeproc, "untrusted" allows it, and pandoc_set_num_threads may be
 * called from several threads at once and is clamped to the processors;
 * 1.9: changes of meaning, not only additions: pandoc_set_num_threads
 * changes nothing once pandoc's threads have grown, and returns the number
 * in effect; input given with input-files that don't name it is an error;
 * pandoc_main decodes its arguments as the command does and returns on a
 * broken standard output; an "Exception"'s message has no backtrace; a
 * filter whose fn is NULL is an error; pandoc_read_many returns its
 * messages in the log instead of printing them; 1.10: "timeout" and the
 * error kind "Timeout"; "untrusted" reads nothing of the user data
 * directory; filters are told the writer's options: "writer-options",
 * PANDOC_WRITER_OPTIONS; a call failing with "<<loop>>" is made again;
 * 1.11: a conversion's temporary files are removed in libpandoc.wasm too,
 * and a conversion whose temporary directory can't be removed fails;
 * libpandoc.wasm exports pandoc_main; pandoc_main --help names argv[0];
 * 1.12: SIGPIPE is blocked in the threads that run pandoc (pandoc_init);
 * a NUL in an error's message is U+FFFD;
 * 1.13: filters are told whether pandoc's sandbox is on: a callback's
 * "sandbox", PANDOC_SANDBOX, and parse-args's "sandbox";
 * pandoc_cancel_token_new, pandoc_cancel, pandoc_cancel_token_free,
 * "cancel-token" and the error kind "Cancelled").
 * The pandoc inside has
 * its own version, and so does its document AST: pandoc_query "version"
 * and "api-version". */
#define LIBPANDOC_ABI_VERSION_MAJOR 1
#define LIBPANDOC_ABI_VERSION_MINOR 13
#define LIBPANDOC_ABI_VERSION (LIBPANDOC_ABI_VERSION_MAJOR * 1000 + LIBPANDOC_ABI_VERSION_MINOR)

/* Its layout is part of the ABI: hosts of libpandoc.wasm read it from the
 * module's memory (six 4-byte fields on wasm32: wasm/core.mjs,
 * libpandoc-rs's sandbox). */
typedef struct pandoc_result {
    /* 0 on success, nonzero on failure. */
    int status;
    /* Output bytes (what pandoc would write to stdout). Always followed by a
     * NUL that is not counted in output_len. Empty when the options name an
     * output file. */
    char *output;
    size_t output_len;
    /* On failure, NUL-terminated: the pandoc error constructor (e.g.
     * "PandocParseError", "PandocUnknownReaderError"), "Timeout" for a call
     * stopped by its time limit (libpandoc's, from 1.10: "timeout" at
     * pandoc_convert), "Cancelled" for one stopped by pandoc_cancel (from
     * 1.13), or "Exception" for anything else, and the message
     * pandoc would print (for an "Exception", its message only, without
     * the backtrace GHC 9.10 adds to it: from 1.9). A NUL byte in either
     * (a file name's) is U+FFFD (from 1.12; before, the text ended there).
     * NULL on success. */
    char *error_kind;
    char *error_message;
    /* NUL-terminated JSON array of pandoc's log messages (warnings and
     * info), as written by --log. Never NULL. On failure, those pandoc
     * logged before failing, if it did (--fail-if-warnings's warnings);
     * else []. */
    char *log;
} pandoc_result;

/* Threads. pandoc runs on a number of threads (Haskell capabilities), so
 * that calls from different host threads, and pandoc_read_many, run in
 * parallel. It starts on one, and grows to that number once, the first
 * time it runs something in parallel: when a call starts while a call from
 * another thread is running, or pandoc_read_many is given more than one
 * input. (A call a filter makes from inside a conversion runs on that
 * conversion's thread, and doesn't count.) Calls made one at a time, from
 * one thread or several, never grow it.
 *
 * The number is the last pandoc_set_num_threads made before the growth,
 * else LIBPANDOC_NUM_THREADS (as OMP_NUM_THREADS), else one per logical
 * core the process may use (its CPU affinity); and always at least 1 and
 * at most one per logical core the process may use (from 1.8: more help
 * nobody, and changed while conversions ran they crashed GHC's garbage
 * collector).
 *
 * pandoc_set_num_threads(n), before the growth, sets the number and
 * returns it (n, clamped as above). After the growth it changes nothing and
 * returns the number in effect (from 1.9; before, it changed the number at
 * any time, which, while conversions ran, now and then hung GHC's runtime).
 * So a result other than n means that n was out of range or that pandoc
 * had already grown: a program choosing the number sets it before it first
 * calls pandoc from two threads at once, or with several inputs to
 * pandoc_read_many. {"query": "num-threads"} tells the number in effect,
 * or, before the growth, the number pandoc will grow to. It may be called
 * from any thread, also from several at once: the calls take turns, and
 * the last one before the growth decides. After pandoc_shutdown it
 * returns 0.
 *
 * A call that fails with the error "Exception", "<<loop>>" is made again,
 * up to three times (from 1.10). GHC 9.10's runtime, which libpandoc's
 * pandoc is built with, now and then loses the wakeup of threads waiting
 * for a value that another thread evaluates for the first time, and they
 * fail so although nothing loops (GHC #26324: about 1 in 3,000 processes
 * starting 8 conversions at once). Only several first calls at once in a
 * process meet it. Nothing of the failed attempt reaches the caller. Not
 * pandoc_main, nor a conversion given no input (it may have read the
 * process's standard input). A callback filter may thus run twice in one
 * call (below). To go when libpandoc's GHC has the fix (9.12.5, 9.14.2). */
LIBPANDOC_API int pandoc_set_num_threads(int n);

/* Start the Haskell runtime. Safe to call more than once and from any
 * thread; every other function calls it implicitly. The runtime is started
 * without installing signal handlers, so the host keeps SIGINT etc. It
 * cannot be restarted after pandoc_shutdown. Returns 0 on success.
 *
 * SIGPIPE (from 1.12; not on Windows nor in libpandoc.wasm): the threads
 * that run pandoc have it blocked, so that a program pandoc writes to and
 * that exits without reading it all (a JSON filter, a PDF engine, Lua's
 * pandoc.pipe) is an error and doesn't end the host (as it ended a host
 * leaving SIGPIPE at its default, before). These are a host's thread while
 * it is inside a call (a SIGPIPE raised on it meanwhile is discarded, and
 * its mask restored when the call returns; a callback filter runs with it
 * blocked too) and the runtime's own threads. The host's disposition of
 * SIGPIPE is left as it is. Programs pandoc starts inherit it blocked, as
 * from a host that ignores it (Python, Rust, Julia) they inherit it
 * ignored. */
LIBPANDOC_API int pandoc_init(void);

/* Stop the Haskell runtime. Optional, and seldom worth it (the process's
 * end frees everything); call at most once, at process end, when no call
 * is running and none will start, from no callback (inside a call). It
 * cannot be undone: pandoc_init then fails, every function returning a
 * pandoc_result returns NULL, pandoc_main returns 1 and
 * pandoc_set_num_threads 0. A call running on another thread meanwhile
 * may crash or hang the process: libpandoc doesn't wait for calls. Not in
 * libpandoc.wasm (drop the instance instead). */
LIBPANDOC_API void pandoc_shutdown(void);

/* LIBPANDOC_ABI_VERSION of the loaded library (major * 1000 + minor): see
 * Versions at the top. */
LIBPANDOC_API int pandoc_abi_version(void);

/* Convert, like running `pandoc` with a defaults file.
 *
 * options: JSON object in defaults-file format, e.g.
 *          {"from": "markdown", "to": "html", "standalone": true}
 * input:   bytes to use as standard input, or NULL to use the options'
 *          input-files (or, with neither, the process's real stdin, as the
 *          pandoc CLI would). Given with input-files, they must name it as
 *          "-" (as on pandoc's command line), else it's a PandocOptionError
 *          (from 1.9; before, the input was ignored). The same for
 *          pandoc_convert_args's file arguments.
 *          Input is written to a file (pandoc reads files) in a directory
 *          of the call's own in TMPDIR, with the output and the log,
 *          removed when the call ends, however it ends; one that can't be
 *          removed fails the call, naming it (from 1.11; libpandoc.wasm
 *          left every call's behind before).
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
 * is on. "citeproc" is allowed (from 1.8), sandboxed as below, but not
 * the options naming its files ("bibliography", "csl",
 * "citation-abbreviations"): references in the document's metadata work.
 * Anything else fails with a PandocOptionError naming it. pandoc reads
 * nothing of the user data directory then (from 1.10; before, its
 * templates and the like): templates and their partials, reference
 * documents, abbreviations, translations and CSL styles are pandoc's own.
 * PANDOC_DATA_DIRS (pandoc's system data directories) is honoured, by
 * design: it is the host process's own configuration, set by whoever runs
 * the host, which untrusted options can't choose. From its directories
 * pandoc reads the template, its partials, dzslides' template and the
 * abbreviations (before its sandbox), but not reference documents,
 * translations or citeproc's styles (in its sandbox, which leaves them
 * out). A host that wants none of it doesn't set PANDOC_DATA_DIRS. A host sets
 * "untrusted": true on whatever such code passes (replacing its own
 * "untrusted"), and the list lives here, the same for every host.
 * "untrusted" is about options: it sets no time limit. A host sets one
 * itself on such code's calls ("timeout", below: replacing the code's own,
 * or the smaller of the two), e.g. a wasm filter's remaining time, and
 * gives documents from someone it doesn't trust their own memory (below).
 *
 * "timeout": seconds (a number, fractions allowed; 0 or absent: none;
 * libpandoc's, not pandoc's, from 1.10). A conversion still running after
 * that long is stopped and fails with the error kind "Timeout" and an
 * empty log; its temporary files are removed, and the process goes on:
 * the next call works. It stops pandoc's own code: readers, writers,
 * templates, citeproc (all of which allocate as they run, which is where
 * the runtime can stop them), and a JSON filter, which is sent SIGTERM
 * (Windows: terminated) and reaped (a program it started itself, not
 * exec'd, is its own business). It cannot stop code in a foreign call:
 *   - a callback filter (and what it calls, pandoc included) runs to its
 *     end, and the call fails with "Timeout" when it returns;
 *   - Lua (filters, custom readers and writers) runs in C, so it is not
 *     stopped at all: a Lua filter's `while true do end` runs on, holding
 *     its thread, and so does pandoc's code a Lua filter calls
 *     (pandoc.read). The time limit doesn't cover Lua.
 * pandoc_convert_args, pandoc_convert_args_filters and pandoc_main have no
 * time limit (pandoc has no such option). libpandoc.wasm stops a call as
 * natively; its JSON filters are the host's, so callbacks.
 *
 * Memory: there is no memory limit for a call. Running out of memory ends
 * the process (see the top), and a heap limit (+RTS -M) in a library ends
 * it too: the runtime has no main thread to raise HeapOverflow in. (A
 * per-thread allocation limit would not be one either: pandoc allocates
 * gigabytes for a large document while holding little.) For documents
 * from someone you don't trust, give the conversion memory of its own:
 * libpandoc.wasm (its heap is capped, and running out ends only that
 * instance), a separate process, or a container with a memory limit,
 * together with a "timeout".
 *
 * "sandbox": true is pandoc's --sandbox (readers and writers read only the
 * files the options name, and fetch nothing), and more (from 1.8):
 * citeproc runs in the sandbox too, which pandoc doesn't do. It reads only
 * the "bibliography", "csl" and "citation-abbreviations" files the options
 * name, never one the document names (its metadata, LaTeX's
 * \bibliography), nor one named as metadata ("metadata", metadata files,
 * filters), nor the data directory's styles; any other file or URL is
 * missing, with the message a missing file gets, and nothing is fetched. */
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
 *              in PANDOC_READER_OPTIONS;
 *            "writer-options": the writer's options (from 1.10), what a JSON
 *              filter run by libpandoc gets in PANDOC_WRITER_OPTIONS (pandoc
 *              tells only Lua filters: PANDOC_WRITER_OPTIONS there). Lua's
 *              fields and values, keyed as the reader's options are
 *              (kebab-case of pandoc's field names): "number-sections",
 *              "top-level-division": "top-level-chapter", "number-offset",
 *              "toc-depth", "wrap-text": "wrap-auto", "math-method" (Lua's
 *              html_math_method), "variables" (a map of text, lists and
 *              maps), "template" (true if there is one, else null: not its
 *              text), ...; not the syntax definitions. In
 *              PANDOC_WRITER_OPTIONS without "variables" when the whole would
 *              be longer than 32,000 bytes (an environment variable's limit,
 *              on Windows);
 *            "sandbox": true when pandoc's sandbox is on for the conversion
 *              (from 1.13), however it was set: "sandbox": true or
 *              "untrusted" in the options, --sandbox, a defaults file; else
 *              false. What a JSON filter run by libpandoc gets in
 *              PANDOC_SANDBOX ("true" or "false"; pandoc tells filters
 *              nothing of it). So that a filter parsing the document's text
 *              (pandoc_read_many) reads it in the sandbox too, as a Lua
 *              filter's pandoc.read runs inside the conversion. In
 *              libpandoc.wasm it says the same, pandoc's own sandbox: the
 *              module can't know which directories its host opened to it,
 *              so a host running the conversion in a directory sandbox says
 *              true itself to the filters it runs outside the module.
 * Neither is NUL-terminated, and both are valid only during the call.
 *
 * fn returns 0 and puts the new document, as pandoc's JSON, in out with
 * pandoc_buffer_set; or returns nonzero and puts a UTF-8 error message in
 * out, which fails the conversion with a PandocFilterError. A filter whose
 * fn is NULL fails it so too (from 1.9; before, it crashed; in
 * libpandoc.wasm, NULL is the host's import libpandoc.filter instead).
 *
 * fn may call libpandoc again (pandoc_convert, pandoc_query, ...), for
 * example to parse a fragment of text; those are separate conversions.
 *
 * fn may be called twice for one place in one call, rarely: when the call
 * failed with "<<loop>>" after fn ran, and libpandoc makes it again (see
 * Threads; from 1.10). What fn does besides answering (counting, writing
 * a file) then happens twice. */
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
 * messages; --help's from 1.11, before the process's). pandoc reads
 * standard input and writes standard output and
 * standard error itself, answers informational options (--version,
 * --list-*, -D, --help, ...), and reports errors, all as the command does;
 * the return value is the exit status pandoc would exit with. Arguments
 * are decoded as the pandoc command decodes its own: in the file system's
 * encoding (the locale's, keeping bytes that aren't text, so that any
 * file name works), on Windows as UTF-8 (from 1.9; before, as UTF-8
 * everywhere, and an argument that wasn't ended the process). Standard
 * output closed by its reader (a broken pipe, SIGPIPE ignored) ends the
 * command with status 0, as it ends pandoc, but returns (from 1.9; before,
 * the process exited). --version prints pandoc-cli's lines, saying what
 * pandoc_main does: "Features: -server +lua" and the scripting engine.
 *
 * filters_json, if not NULL, is a JSON array (as the "filters" option)
 * replacing the filters pandoc found in the arguments and defaults files;
 * it may name filters[i] as {"type": "callback", "index": i}. A
 * pandoc-compatible command line uses {"query": "parse-args"} to see those
 * filters, then this to run some of them in process.
 *
 * `lua` as the first argument (or a program named pandoc-lua) runs pandoc
 * as a Lua interpreter, as `pandoc lua` does. A script's os.exit exits the
 * process, as it would pandoc's. `server` is not supported (status 4).
 *
 * Standard input is read once: pandoc closes it when it has read it, as
 * the command does, so a second call reading it in the same process (or
 * libpandoc.wasm instance) fails ("handle is closed"); calls naming their
 * input files can be made one after another.
 *
 * In libpandoc.wasm (from 1.11) standard input, output and error are WASI's
 * descriptors 0, 1 and 2, which the host connects, and the files are those
 * of the directories it gives. Filters: Lua runs inside; JSON filters can't
 * (WASI starts no programs: a PandocFilterError, status 83, "libpandoc.wasm
 * runs no programs"), so a host runs those it can (a wasm filter) as
 * callback filters through filters_json, as natively, finding them with
 * {"query": "parse-args"}; a filter whose fn is NULL is the host's import
 * libpandoc.filter, as for pandoc_convert_filters. No `lua` (status 4),
 * no PDF (no programs). --version says "Features: -server -lua". */
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
 * abbreviations), for untrusted code; the abbreviations are then pandoc's
 * own, or PANDOC_DATA_DIRS's (as for pandoc_convert). Each text is prepared as pandoc prepares its
 * input (tabs expanded unless preserve-tabs, carriage returns dropped).
 *
 * "timeout" in "options" (from 1.10), as for pandoc_convert, is the whole
 * call's: the readers run in parallel, and the call returns within about
 * that long. An input not read by then is {"error": {"kind": "Timeout",
 * ...}} in its place, and the others are kept; the call itself fails with
 * "Timeout" only if setting up the reader took that long.
 *
 * result->output: a JSON array with, for each input, its document as
 * pandoc's JSON, or {"error": {"kind": ..., "message": ...}}. The call itself
 * fails only for a malformed request, an unknown format, or its time
 * limit.
 *
 * result->log: pandoc's messages, as a conversion's (the reader's warnings,
 * such as an include file not found): the setup's, then each input's, in
 * the order of the inputs (from 1.9; before, they were printed on standard
 * error, and the log was []). */
LIBPANDOC_API pandoc_result *pandoc_read_many(const char *request, size_t request_len);

/* Ask pandoc for information. query is a JSON object with a "query" key:
 *   {"query": "version"}                  pandoc version, e.g. "3.14"
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
 *                                         (before it has grown, will use:
 *                                         pandoc_set_num_threads)
 *   {"query": "parse-args", "args": [...]}
 *                                         what pandoc makes of these
 *                                         command-line arguments: {"filters":
 *                                         [...], "sandbox": true|false}
 *                                         (defaults files included;
 *                                         "sandbox" from 1.13: --sandbox),
 *                                         or {"informational": "VersionInfo"},
 *                                         or {"subcommand": "lua"} (or
 *                                         "server"), whose arguments aren't
 *                                         pandoc's options
 * The answer is JSON, in result->output. With "untrusted": true, as for
 * pandoc_convert, only those that read nothing of the user's: not
 * default-template nor parse-args. */
LIBPANDOC_API pandoc_result *pandoc_query(const char *query, size_t query_len);

/* Cancelling a call (from 1.13): for a binding to stop a long conversion
 * (Ctrl-C, a cancel button) from another thread while it runs.
 *
 * pandoc_cancel_token_new() makes a token: a number (from 1, never reused;
 * 0 only when the runtime isn't running or memory ran out). A call takes it
 * as libpandoc's option "cancel-token": N (not pandoc's) in pandoc_convert's
 * (and pandoc_convert_filters's) options and in pandoc_read_many's
 * "options"; a number that is no token (freed, never made) is a
 * PandocOptionError, so a call that couldn't be cancelled doesn't start.
 * "untrusted" options can't name one (a PandocOptionError): a host's own
 * calls do, and the calls such code makes from inside a callback inherit
 * them (below).
 *
 * pandoc_cancel(token) cancels it: every call using it, running or to come,
 * stops and fails with the error kind "Cancelled" (and a message saying
 * so, an empty log), as one past its "timeout" fails with "Timeout": its
 * temporary files are removed, a JSON filter it runs is terminated and
 * reaped, and the process and the library go on. pandoc_read_many fails as
 * a whole (no array of partial results, as its time limit gives): a reader
 * still running stops, and the call returns once all have. A token stays
 * cancelled: a call given it later fails at once, so a cancel made just
 * before a call starts isn't lost. A token not cancelled serves any number
 * of calls, one after another or at once (one for a batch: cancelled, it
 * stops them all). A call that had finished before the cancel keeps its
 * result. Returns 0, or -1 for a number that is no token (nothing happens;
 * never a crash).
 *
 * pandoc_cancel_token_free(token) forgets it (a number that is no token:
 * nothing). Calls using it go on, and can no longer be cancelled through
 * it. Free each token made: a few dozen bytes each.
 *
 * Calls made from inside a callback filter, on the thread pandoc called it
 * on (to read a fragment, say), are cancelled with the conversion: they
 * stop for its tokens (and those it inherited) as well as their own. On
 * another thread they don't (libpandoc can't tell whose they are).
 *
 * Threads: these three may be called from any thread, at any time, also
 * while calls run and from inside a callback; they don't count as calls
 * (pandoc doesn't grow its threads for them). pandoc_cancel takes the
 * runtime briefly: it returns at once when a processor of pandoc's is
 * free, else when the running code next yields (within milliseconds: the
 * runtime switches threads every 20 ms) -- or, in a loop that doesn't
 * allocate (below), not until the loop ends. They are NOT async-signal-
 * safe: don't call them from a signal handler (the runtime takes locks and
 * allocates). From a signal, cancel on a normal thread: Python's handlers
 * (which run on its main thread), Julia's InterruptException, Rust's ctrlc
 * crate (whose handler runs on a thread of its own), or a thread of the
 * host's waiting on a pipe the handler writes to.
 *
 * Limits, those of "timeout" (pandoc_convert): a cancel stops pandoc's own
 * code where it allocates, which its readers, writers, templates and
 * citeproc do as they run, and a JSON filter (SIGTERM, reaped). Not code in
 * a foreign call: a callback filter runs to its end (its answer is then
 * freed and the call fails with "Cancelled"; a call it makes after the
 * cancel fails at once, above), and Lua (filters, custom readers and
 * writers, a Lua filter's pandoc.read) isn't stopped at all. Nor a loop of
 * pandoc's that doesn't allocate (libpandoc is built without
 * -fno-omit-yields; the fuzzer found one, the muse writer on a run of `{`):
 * it ends first. pandoc_convert_args, pandoc_convert_args_filters and
 * pandoc_main take no token (no options), but are cancelled as a callback's
 * calls above. libpandoc.wasm exports none of the three: nothing can call
 * them while a call runs there (one thread); a host stops a call from
 * outside instead (wasmtime's epoch interruption, a Worker's terminate()),
 * which stops the instance (load another), and "cancel-token" there names
 * no token. */
LIBPANDOC_API int64_t pandoc_cancel_token_new(void);
LIBPANDOC_API int pandoc_cancel(int64_t token);
LIBPANDOC_API void pandoc_cancel_token_free(int64_t token);

/* Every function returning a pandoc_result returns NULL only when the
 * runtime isn't running (after pandoc_shutdown) or memory ran out. */
LIBPANDOC_API void pandoc_result_free(pandoc_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LIBPANDOC_H */
