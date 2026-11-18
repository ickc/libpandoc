/* pandoc's command on libpandoc, for test/panclude.mjs (panclude's
 * conformance cases against the include step built in):
 *
 *   command ARGS...                  pandoc_main(ARGS), its exit status; the
 *                                    environment's LIBPANDOC_TEST_FILTERS, if
 *                                    set, as its filters_json
 *   command --convert OPTIONS LOG    pandoc_convert(OPTIONS, standard input):
 *                                    the output on standard output, the log
 *                                    written to the file LOG; an error as
 *                                    "KIND: MESSAGE" on standard error, status 1
 *
 *   cc test/command.c -Idist/include -Ldist/lib -lpandoc -Wl,-rpath,'$ORIGIN/lib' -o dist/command
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpandoc.h>

/* standard input, all of it */
static char *slurp(size_t *len)
{
    size_t cap = 1 << 16, n = 0, got;
    char *buf = malloc(cap);
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--convert") == 0) {
        size_t len;
        char *input = slurp(&len);
        if (!input) return 2;
        pandoc_result *r = pandoc_convert(argv[2], strlen(argv[2]), input, len);
        free(input);
        if (!r) return 2;
        FILE *log = fopen(argv[3], "wb");
        if (log) {
            fputs(r->log, log);
            fclose(log);
        }
        int status = r->status == 0 ? 0 : 1;
        if (status == 0)
            fwrite(r->output, 1, r->output_len, stdout);
        else
            fprintf(stderr, "%s: %s\n", r->error_kind, r->error_message);
        pandoc_result_free(r);
        return status;
    }
    const char *filters = getenv("LIBPANDOC_TEST_FILTERS");
    argv[0] = "pandoc";
    return pandoc_main(argc, (const char *const *)argv, filters, filters ? strlen(filters) : 0, NULL, 0);
}
