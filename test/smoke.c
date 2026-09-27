/* Smoke test of the C ABI: cc test/smoke.c -Idist/include -Ldist/lib -lpandoc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <libpandoc.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

/* Callback filters. userdata counts calls; the context of the last call is
 * kept for checking. */
static char last_context[4096];

static void keep_context(const char *context, size_t len)
{
    if (len >= sizeof last_context) len = sizeof last_context - 1;
    memcpy(last_context, context, len);
    last_context[len] = '\0';
}

static int identity(void *userdata, const char *doc, size_t doc_len,
                    const char *context, size_t context_len, pandoc_buffer *out)
{
    ++*(int *)userdata;
    keep_context(context, context_len);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

/* Upper-cases every "world" in the document's JSON. */
static int shout(void *userdata, const char *doc, size_t doc_len,
                 const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)context; (void)context_len;
    char *copy = malloc(doc_len);
    memcpy(copy, doc, doc_len);
    for (size_t i = 0; i + 5 <= doc_len; i++)
        if (memcmp(copy + i, "world", 5) == 0) memcpy(copy + i, "WORLD", 5);
    pandoc_buffer_set(out, copy, doc_len);
    free(copy);
    return 0;
}

static int fail(void *userdata, const char *doc, size_t doc_len,
                const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)userdata; (void)doc; (void)doc_len; (void)context; (void)context_len;
    const char *msg = "boom from the callback";
    pandoc_buffer_set(out, msg, strlen(msg));
    return 1;
}

/* Calls libpandoc again from inside the conversion. */
static int nested(void *userdata, const char *doc, size_t doc_len,
                  const char *context, size_t context_len, pandoc_buffer *out)
{
    (void)context; (void)context_len;
    const char *o = "{\"from\": \"markdown\", \"to\": \"latex\"}";
    const char *in = "*inner*";
    pandoc_result *r = pandoc_convert(o, strlen(o), in, strlen(in));
    *(int *)userdata = r && r->status == 0 && strstr(r->output, "\\emph{inner}") != NULL;
    pandoc_result_free(r);
    pandoc_buffer_set(out, doc, doc_len);
    return 0;
}

int main(int argc, char **argv)
{
    pandoc_result *r;
    /* unbuffered, so a crash shows how far it got */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *opts = "{\"from\": \"markdown\", \"to\": \"html\"}";
    const char *md = "# Hello *world*\n";

    check(pandoc_abi_version() == LIBPANDOC_ABI_VERSION, "abi version");

    r = pandoc_convert(opts, strlen(opts), md, strlen(md));
    check(r && r->status == 0, "convert status");
    check(r && strcmp(r->output, "<h1 id=\"hello-world\">Hello <em>world</em></h1>\n") == 0,
          "convert output");
    if (r) printf("      %s", r->output);
    pandoc_result_free(r);

    const char *args[] = {"-f", "markdown", "-t", "latex"};
    r = pandoc_convert_args(4, args, md, strlen(md));
    check(r && r->status == 0 && strstr(r->output, "\\emph{world}"), "convert_args");
    pandoc_result_free(r);

    /* errors from the argv form: with stdin, without, informational */
    const char *badarg[] = {"-t", "nonesuch"};
    r = pandoc_convert_args(2, badarg, md, strlen(md));
    check(r && r->status != 0, "argv error, with stdin");
    pandoc_result_free(r);
    r = pandoc_convert_args(2, badarg, NULL, 0);
    check(r && r->status != 0, "argv error, without stdin");
    pandoc_result_free(r);

    /* an error thrown before converting: malformed options */
    const char *junk = "{\"from\": ";
    r = pandoc_convert(junk, strlen(junk), md, strlen(md));
    check(r && r->status != 0, "malformed options are an error");
    pandoc_result_free(r);

    const char *list[] = {"--list-input-formats"};
    r = pandoc_convert_args(1, list, md, strlen(md));
    check(r && r->status != 0, "informational option, with stdin");
    pandoc_result_free(r);

    const char *ver[] = {"--version"};
    r = pandoc_convert_args(1, ver, NULL, 0);
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
          "informational option is rejected");
    if (r && r->error_message) printf("      %s\n", r->error_message);
    pandoc_result_free(r);

    const char *bad = "{\"from\": \"nonesuch\"}";
    r = pandoc_convert(bad, strlen(bad), md, strlen(md));
    check(r && r->status != 0 && strcmp(r->error_kind, "PandocUnknownReaderError") == 0,
          "unknown reader is an error, not a crash");
    if (r && r->error_message) printf("      %s: %s\n", r->error_kind, r->error_message);
    pandoc_result_free(r);

    const char *q = "{\"query\": \"version\"}";
    r = pandoc_query(q, strlen(q));
    check(r && r->status == 0, "query version");
    if (r) printf("      %s\n", r->output);
    pandoc_result_free(r);

    const char *warn = "![](missing.png)";
    const char *docx = "{\"to\": \"docx\"}";
    r = pandoc_convert(docx, strlen(docx), warn, strlen(warn));
    check(r && r->status == 0 && r->output_len > 1000 && memcmp(r->output, "PK", 2) == 0,
          "binary output (docx)");
    check(r && strstr(r->log, "CouldNotFetchResource"), "warnings are returned in the log");
    if (r) printf("      log: %.160s\n", r->log);
    pandoc_result_free(r);

    /* callback filters */
    {
        int calls = 0, nested_ok = 0;
        pandoc_filter fs[] = {
            {identity, &calls}, {shout, NULL}, {fail, NULL}, {nested, &nested_ok},
        };
        const char *o1 = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 0}]}";
        r = pandoc_convert_filters(o1, strlen(o1), md, strlen(md), fs, 4);
        check(r && r->status == 0 && calls == 1
              && strcmp(r->output, "<h1 id=\"hello-world\">Hello <em>world</em></h1>\n") == 0,
              "callback filter: identity, called once");
        check(strstr(last_context, "\"format\":\"html\"") && strstr(last_context, "\"reader-options\"")
              && strstr(last_context, "\"input-format\":\"markdown\"")
              && strstr(last_context, "\"output-format\":\"html\""),
              "callback filter: context");
        printf("      context: %.120s\n", last_context);
        pandoc_result_free(r);

        const char *o2 = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 1},"
                         " {\"type\": \"callback\", \"index\": 0}]}";
        r = pandoc_convert_filters(o2, strlen(o2), md, strlen(md), fs, 4);
        check(r && r->status == 0 && strstr(r->output, "<em>WORLD</em>") && calls == 2,
              "callback filter: changes the document, in order");
        pandoc_result_free(r);

        const char *o3 = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 2}]}";
        r = pandoc_convert_filters(o3, strlen(o3), md, strlen(md), fs, 4);
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocFilterError") == 0
              && strstr(r->error_message, "boom from the callback"),
              "callback filter: its error fails the conversion");
        if (r && r->error_message) printf("      %s\n", r->error_message);
        pandoc_result_free(r);

        const char *o4 = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 3}]}";
        r = pandoc_convert_filters(o4, strlen(o4), md, strlen(md), fs, 4);
        check(r && r->status == 0 && nested_ok, "callback filter: calls libpandoc again");
        pandoc_result_free(r);

        const char *o6 = "{\"from\": \"commonmark_x-smart\", \"to\": \"html5+smart\", "
                         "\"filters\": [{\"type\": \"callback\", \"index\": 0}]}";
        r = pandoc_convert_filters(o6, strlen(o6), md, strlen(md), fs, 4);
        check(r && r->status == 0 && strstr(last_context, "\"input-format\":\"commonmark_x-smart\"")
              && strstr(last_context, "\"output-format\":\"html5+smart\"")
              && strstr(last_context, "\"format\":\"html5\""),
              "callback filter: context has the formats with extensions");
        pandoc_result_free(r);

        const char *o5 = "{\"to\": \"html\", \"filters\": [{\"type\": \"callback\", \"index\": 4}]}";
        r = pandoc_convert_filters(o5, strlen(o5), md, strlen(md), fs, 4);
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
              "callback filter: index out of range");
        pandoc_result_free(r);

        const char *a1[] = {"-t", "html", "-L", "libpandoc:callback/1", "--lua-filter=libpandoc:callback/0"};
        calls = 0;
        r = pandoc_convert_args_filters(5, a1, md, strlen(md), fs, 4);
        check(r && r->status == 0 && strstr(r->output, "<em>WORLD</em>") && calls == 1,
              "callback filter: in the argv form");
        pandoc_result_free(r);

        const char *a2[] = {"-t", "html", "-L", "libpandoc:callback/7"};
        r = pandoc_convert_args_filters(4, a2, md, strlen(md), fs, 4);
        check(r && r->status != 0, "callback filter: argv index out of range is an error");
        pandoc_result_free(r);

        r = pandoc_convert(o1, strlen(o1), md, strlen(md));
        check(r && r->status != 0, "callback filter: none given to pandoc_convert");
        pandoc_result_free(r);
    }

#ifndef _WIN32
    /* JSON filters are told the formats too (PANDOC_INPUT_FORMAT, ...) */
    {
        FILE *f = fopen("envfilter.sh", "w");
        fputs("#!/bin/sh\nprintf '%s|%s|%s' \"$PANDOC_INPUT_FORMAT\" \"$PANDOC_OUTPUT_FORMAT\""
              " \"$PANDOC_VERSION\" > envfilter.out\ncat\n", f);
        fclose(f);
        chmod("envfilter.sh", 0755);
        const char *oj = "{\"from\": \"commonmark_x-smart\", \"to\": \"html5+smart\", \"filters\": [\"./envfilter.sh\"]}";
        r = pandoc_convert(oj, strlen(oj), md, strlen(md));
        char seen[256] = {0};
        f = fopen("envfilter.out", "r");
        if (f) { fread(seen, 1, sizeof seen - 1, f); fclose(f); }
        check(r && r->status == 0 && strncmp(seen, "commonmark_x-smart|html5+smart|3.", 32) == 0,
              "JSON filter: told the input and output formats");
        printf("      %s\n", seen);
        pandoc_result_free(r);
        const char *ob = "{\"filters\": [\"./no-such-filter\"]}";
        r = pandoc_convert(ob, strlen(ob), md, strlen(md));
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocFilterError") == 0
              && strstr(r->error_message, "no-such-filter"), "JSON filter: missing one is an error");
        if (r && r->error_message) printf("      %s\n", r->error_message);
        pandoc_result_free(r);
        remove("envfilter.sh");
        remove("envfilter.out");
    }
#endif

    /* many fragments at once */
    {
        const char *rq = "{\"options\": {\"from\": \"commonmark_x\"}, \"inputs\": [\"*a*\", \"~~b~~\", \"\"]}";
        r = pandoc_read_many(rq, strlen(rq));
        check(r && r->status == 0 && r->output[0] == '['
              && strstr(r->output, "\"Emph\"") && strstr(r->output, "\"Strikeout\"")
              && strstr(r->output, "\"blocks\":[]"),
              "read_many: each input read");
        if (r) printf("      %.100s...\n", r->output);
        pandoc_result_free(r);
        const char *rb = "{\"options\": {\"from\": \"nonesuch\"}, \"inputs\": [\"x\"]}";
        r = pandoc_read_many(rb, strlen(rb));
        check(r && r->status != 0, "read_many: unknown format is an error");
        pandoc_result_free(r);
        const char *rj = "{\"inputs\": 3}";
        r = pandoc_read_many(rj, strlen(rj));
        check(r && r->status != 0 && strcmp(r->error_kind, "PandocOptionError") == 0,
              "read_many: malformed request is an error");
        pandoc_result_free(r);
    }

    /* smoke API.json: also save the pandoc API version, e.g. [1,23,1,2] */
    if (argc > 1) {
        const char *aq = "{\"query\": \"api-version\"}";
        FILE *f = fopen(argv[1], "wb");
        r = pandoc_query(aq, strlen(aq));
        check(r && r->status == 0 && f && r->output[0] == '[', "api version");
        if (r && f) fwrite(r->output, 1, r->output_len, f);
        if (f) fclose(f);
        pandoc_result_free(r);
    }

    return failures != 0;
}
