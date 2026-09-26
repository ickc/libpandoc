/* Smoke test of the C ABI: cc test/smoke.c -Idist/include -Ldist/lib -lpandoc */
#include <stdio.h>
#include <string.h>
#include <libpandoc.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
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

    /* smoke SCHEMA.json: also save the AST schema */
    if (argc > 1) {
        const char *sq = "{\"query\": \"ast-schema\"}";
        FILE *f = fopen(argv[1], "wb");
        r = pandoc_query(sq, strlen(sq));
        check(r && r->status == 0 && f, "ast schema");
        if (r && f) fwrite(r->output, 1, r->output_len, f);
        if (f) fclose(f);
        pandoc_result_free(r);
    }

    return failures != 0;
}
