/*
 * test_parser.c -- unit tests for parse_line()
 *
 * Build (MSVC):   cl /utf-8 /W4 test_parser.c parser.c
 * Build (MinGW):  gcc -std=c17 -Wall -Wextra test_parser.c parser.c -o test_parser.exe
 *
 * Each case parses a line and serialises the result to a compact string:
 *     [arg][arg] <(in) >(out) >>(out) 2>(err) 2>&1  | next command ... &
 * so one string comparison checks argv, redirections, pipes and background.
 * Special results:  "EMPTY" (blank line)   "ERR:<message>" (syntax error)
 */
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void app(char *buf, size_t cap, const char *fmt, const char *a)
{
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, fmt, a);
}

/* turn a parse result into a comparable string (static buffer) */
static const char *run(const char *line)
{
    static char out[2048];
    char *err = NULL;
    Pipeline *p = parse_line(line, &err);

    out[0] = '\0';
    if (!p) {
        if (err) { app(out, sizeof out, "ERR:%s", err); free(err); }
        else     strcpy(out, "EMPTY");
        return out;
    }
    for (int i = 0; i < p->ncmds; i++) {
        Command *c = &p->cmds[i];
        if (i) strcat(out, " | ");
        for (int j = 0; j < c->argc; j++) app(out, sizeof out, "[%s]", c->argv[j]);
        if (c->in_file)  app(out, sizeof out, " <(%s)", c->in_file);
        if (c->out_file) app(out, sizeof out, c->append ? " >>(%s)" : " >(%s)", c->out_file);
        if (c->err_file) app(out, sizeof out, " 2>(%s)", c->err_file);
        if (c->err_to_out) strcat(out, " 2>&1");
    }
    if (p->background) strcat(out, " &");
    free_pipeline(p);
    return out;
}

static int passed = 0, failed = 0;

static void check(const char *line, const char *expect)
{
    const char *got = run(line);
    if (strcmp(got, expect) == 0) {
        passed++;
        printf("  PASS  %s\n", line);
    } else {
        failed++;
        printf("  FAIL  %s\n        expected: %s\n        got:      %s\n", line, expect, got);
    }
}

int main(void)
{
    /* deterministic environment for the expansion tests */
    SetEnvironmentVariableA("HSH_T", "hello");
    SetEnvironmentVariableA("HSH_NOPE", NULL);
    SetEnvironmentVariableA("USERPROFILE", "C:\\Users\\hsh_test");

    puts("--- basics / empty ---");
    check("",                    "EMPTY");
    check("   \t  ",             "EMPTY");
    check("ls",                  "[ls]");
    check("ls -l /tmp",          "[ls][-l][/tmp]");

    puts("--- quoting and escapes ---");
    check("echo 'a  b'",         "[echo][a  b]");
    check("echo \"a  b\"",       "[echo][a  b]");
    check("echo 'a|b>c&'",       "[echo][a|b>c&]");
    check("echo foo\\ bar",      "[echo][foo bar]");
    check("echo \"a\\\"b\"",     "[echo][a\"b]");
    check("echo ''",             "[echo][]");
    check("echo a\"b c\"d",      "[echo][ab cd]");
    check("echo C:\\Users\\foo", "[echo][C:\\Users\\foo]");       /* path kept */
    check("echo 'C:\\x y\\'",    "[echo][C:\\x y\\]");
    check("echo h\xc3\xa9llo",   "[echo]" "[h\xc3\xa9llo]");      /* UTF-8 */

    puts("--- pipes ---");
    check("ls | grep foo",       "[ls] | [grep][foo]");
    check("ls|grep foo|wc -l",   "[ls] | [grep][foo] | [wc][-l]");

    puts("--- redirection ---");
    check("cat < in.txt",        "[cat] <(in.txt)");
    check("ls > out.txt",        "[ls] >(out.txt)");
    check("ls >> out.txt",       "[ls] >>(out.txt)");
    check("ls>out.txt",          "[ls] >(out.txt)");
    check("make 2> err.log",     "[make] 2>(err.log)");
    check("make > all.log 2>&1", "[make] >(all.log) 2>&1");
    check("echo hi 1>o",         "[echo][hi] >(o)");
    check("echo a2>f",           "[echo][a2] >(f)");               /* not a 2> */
    check("ls < a < b",          "[ls] <(b)");                     /* last wins */
    check("ls 2>&1 2> e",        "[ls] 2>(e)");
    check("ls 2> e 2>&1",        "[ls] 2>&1");
    check("sort < in | uniq > out", "[sort] <(in) | [uniq] >(out)");

    puts("--- background ---");
    check("sleep 5 &",           "[sleep][5] &");
    check("sleep 5&",            "[sleep][5] &");
    check("ls | grep x > f &",   "[ls] | [grep][x] >(f) &");

    puts("--- variable and tilde expansion ---");
    check("echo $HSH_T",         "[echo][hello]");
    check("echo ${HSH_T}world",  "[echo][helloworld]");
    check("echo '$HSH_T'",       "[echo][$HSH_T]");
    check("echo \"$HSH_T\"",     "[echo][hello]");
    check("echo %HSH_T%",        "[echo][hello]");
    check("echo %HSH_NOPE%",     "[echo][%HSH_NOPE%]");            /* cmd style */
    check("echo $HSH_NOPE x",    "[echo][x]");                     /* bash style */
    check("echo \"$HSH_NOPE\"",  "[echo][]");
    check("echo $",              "[echo][$]");
    check("echo ~",              "[echo][C:\\Users\\hsh_test]");
    check("echo ~/docs",         "[echo][C:\\Users\\hsh_test/docs]");
    check("echo a~",             "[echo][a~]");
    check("echo '~'",            "[echo][~]");
    check("echo ~bob",           "[echo][~bob]");

    puts("--- syntax errors ---");
    check("ls | | grep",         "ERR:syntax error near `|'");
    check("| ls",                "ERR:syntax error near `|'");
    check("ls |",                "ERR:syntax error: missing command after `|'");
    check("ls || ls",            "ERR:syntax error near `|'");
    check("echo 'abc",           "ERR:syntax error: unterminated single quote");
    check("echo \"abc",          "ERR:syntax error: unterminated double quote");
    check("cat <",               "ERR:syntax error near `newline'");
    check("ls > > f",            "ERR:syntax error near `>'");
    check("ls > | wc",           "ERR:syntax error near `|'");
    check("> out.txt",           "ERR:syntax error: missing command");
    check("&",                   "ERR:syntax error near `&'");
    check("ls & ls",             "ERR:syntax error near `&'");
    check("ls && ls",            "ERR:syntax error near `&'");
    check("make 2>> log",        "ERR:syntax error: `2>>' is not supported");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
