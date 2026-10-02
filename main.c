/*
 * main.c -- entry point and read-eval loop of hsh
 *
 * Startup:  enable ANSI/VT escape processing, switch the console to UTF-8,
 *           ignore Ctrl+C in the shell itself (children still receive it).
 * Loop:     ui_print_prompt -> ui_read_line -> parse_line ->
 *           execute_pipeline -> jobs_reap          (as required by the brief)
 *
 * Two extra non-interactive modes (used by sudo.c and handy for testing):
 *     hsh -c "ls -l | wc -l"            run one line and exit
 *     hsh --run [--pause] prog args...  run one command *without parsing*
 *                                       (sudo uses this so quoting survives)
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>   /* CommandLineToArgvW */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004   /* old MinGW headers */
#endif

#define HSH_VERSION "0.1"

/* ---- glue between MY OWN files (not part of the shared contract) -------- */
extern int g_shell_exit_requested;            /* set by builtin `exit`       */
extern int g_shell_exit_code;                 /* exit status requested       */
void builtins_history_add(const char *line);  /* feeds the `history` builtin */

/* Write a UTF-8 string straight to a handle (no CRT buffering surprises). */
static void put(HANDLE h, const char *s)
{
    DWORD w;
    WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
}

/*
 * Turn on VT processing so "\x1b[1;32m" colour codes work in the prompt and
 * in ls. Works on Windows 10 1511+ in conhost and Windows Terminal.
 */
static void enable_vt(void)
{
    DWORD ids[2] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    for (int i = 0; i < 2; i++) {
        HANDLE h = GetStdHandle(ids[i]);
        DWORD mode;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT |
                              ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

/*
 * Ctrl+C / Ctrl+Break: the *shell* must survive, but the foreground child
 * (same console) still gets the event and dies as usual. Returning TRUE
 * means "handled", so Windows does not terminate hsh.
 */
static BOOL WINAPI on_ctrl(DWORD type)
{
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ? TRUE : FALSE;
}

/* UTF-8 copy of a wide string (caller frees). */
static char *w2u(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)calloc(n > 0 ? (size_t)n : 1, 1);
    if (s && n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* In-place trim of leading/trailing whitespace; returns pointer into s. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

/*
 * Parse + run one input line. Returns the new "last exit code"
 * (or `prev` if nothing was executed, e.g. blank line).
 */
static int run_line(const char *line, int prev)
{
    char *errmsg = NULL;
    Pipeline *p = parse_line(line, &errmsg);
    if (!p) {
        /* Syntax error. Ownership of errmsg is not specified by the
         * contract, so we do not free it (tiny leak, no double-free). */
        if (errmsg && *errmsg) {
            put(GetStdHandle(STD_ERROR_HANDLE), "hsh: ");
            put(GetStdHandle(STD_ERROR_HANDLE), errmsg);
            put(GetStdHandle(STD_ERROR_HANDLE), "\n");
        }
        return 2;                       /* bash uses 2 for syntax errors */
    }
    if (p->ncmds == 0) {                /* blank line / comment */
        free_pipeline(p);
        return prev;
    }
    int rc = execute_pipeline(p, line);
    free_pipeline(p);
    return rc;
}

/* `hsh --run [--pause] prog args...`: build the Command by hand. */
static int run_direct(char **av, int n, int pause)
{
    Command c;
    memset(&c, 0, sizeof c);
    c.argv = av;                        /* av[n] == NULL (see main)        */
    c.argc = n;

    Pipeline p;
    p.cmds = &c;
    p.ncmds = 1;
    p.background = 0;

    char raw[512] = "";                 /* readable text for the job table */
    for (int i = 0; i < n && strlen(raw) + strlen(av[i]) + 2 < sizeof raw; i++) {
        if (i) strcat(raw, " ");
        strcat(raw, av[i]);
    }

    int rc = execute_pipeline(&p, raw);

    if (pause) {
        /* An elevated console window closes the instant we exit, so keep
         * it open long enough for the user to read the output. */
        char msg[96];
        snprintf(msg, sizeof msg,
                 "\n[hsh] finished (exit code %d). Press Enter to close...", rc);
        put(GetStdHandle(STD_ERROR_HANDLE), msg);
        char b[16];
        DWORD got;
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), b, sizeof b, &got, NULL);
    }
    return rc;
}

int main(void)
{
    /* ---- 1. console setup ------------------------------------------- */
    SetConsoleOutputCP(CP_UTF8);        /* our strings are UTF-8           */
    SetConsoleCP(CP_UTF8);
    enable_vt();
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    /* ---- 2. command line -> UTF-8 argv ------------------------------
     * The CRT's argv is in the ANSI code page, so fetch the wide command
     * line ourselves and convert it. */
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **av = (char **)calloc((size_t)wargc + 1, sizeof *av); /* NULL-terminated */
    for (int i = 0; wargv && i < wargc; i++)
        av[i] = w2u(wargv[i]);

    /* ---- 3. non-interactive modes ----------------------------------- */
    if (wargc >= 3 && strcmp(av[1], "-c") == 0)
        return run_line(av[2], 0);

    if (wargc >= 3 && strcmp(av[1], "--run") == 0) {
        int pause = (strcmp(av[2], "--pause") == 0);
        int first = pause ? 3 : 2;
        if (first >= wargc) return 1;
        return run_direct(av + first, wargc - first, pause);
    }

    /* ---- 4. interactive loop ---------------------------------------- */
    put(GetStdHandle(STD_OUTPUT_HANDLE),
        "hsh " HSH_VERSION " - bash-style shell for native Windows "
        "(type 'help')\n");

    int last_exit = 0;
    while (!g_shell_exit_requested) {
        ui_print_prompt(last_exit, is_elevated());

        char *line = ui_read_line();            /* history + editing in ui */
        if (!line) {                            /* Ctrl+Z / EOF => exit    */
            put(GetStdHandle(STD_OUTPUT_HANDLE), "exit\n");
            break;
        }

        char *t = trim(line);
        if (*t) {
            builtins_history_add(t);            /* for the `history` cmd   */
            last_exit = run_line(t, last_exit);
        }
        free(line);

        jobs_reap();                            /* collect finished jobs   */
    }

    return g_shell_exit_requested ? g_shell_exit_code : last_exit;
}
