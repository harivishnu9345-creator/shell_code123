/*
 * sudo.c -- privilege elevation + simulated Unix permissions for hsh
 *
 * ELEVATION
 *   Windows has no setuid. The only legitimate way to get admin rights is
 *   UAC, which we trigger with ShellExecuteExW(verb = "runas"). The OS shows
 *   its own consent dialog; hsh NEVER sees or handles a password and never
 *   tries to bypass UAC.
 *
 *   An elevated process cannot share our console or pipes, so
 *       sudo <cmd> args...   ->  runs  hsh.exe --run --pause <cmd> args...
 *                                elevated, in a new console window, and we
 *                                wait for it and return its exit code.
 *       su / sudo su         ->  opens an elevated interactive hsh.
 *   (--run is implemented in main.c; it skips the parser so quoting of the
 *   arguments is preserved exactly.)
 *
 * PERMISSION SIMULATION  (!! approximation, NOT real Unix permissions !!)
 *   Windows uses ACLs. `ls -l` therefore *derives* rwx strings from file
 *   attributes (read-only flag, executable extension, directory), and
 *   `chmod` (a) records the mode in a table that lives only inside this hsh
 *   session and (b) really toggles the read-only attribute when the owner
 *   write bit is added/removed. Group/other bits are display-only.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "shell.h"

#define PATHBUF 1024

/* ---------------------------------------------------------------------- *
 *  small helpers (static copies; each .c file is self-contained)          *
 * ---------------------------------------------------------------------- */
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("hsh: out of memory\n", stderr); ExitProcess(1); }
    return p;
}
static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n ? n : 1);
    if (!p) { fputs("hsh: out of memory\n", stderr); ExitProcess(1); }
    return p;
}

/* UTF-8 -> wide (caller frees). Never returns NULL. */
static wchar_t *u2w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)xmalloc((n > 0 ? (size_t)n : 1) * sizeof *w);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

static void out_puts(HANDLE h, const char *s)
{
    DWORD w;
    WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
}
static void out_printf(HANDLE h, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    DWORD w;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
}

/* ---------------------------------------------------------------------- *
 *  is_elevated()  -- contract function                                    *
 * ---------------------------------------------------------------------- */
int is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION te;
    DWORD sz = 0;
    int result = 0;

    /* Ask the process token whether it is an elevated (admin) token. */
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof te, &sz))
            result = te.TokenIsElevated ? 1 : 0;
        CloseHandle(tok);
    }
    return result;
}

/* ---------------------------------------------------------------------- *
 *  Quoting: build a Windows command line that CommandLineToArgvW (used by *
 *  main.c in the elevated copy) turns back into the exact same argv.      *
 * ---------------------------------------------------------------------- */
static wchar_t *quote_arg(const wchar_t *a, wchar_t *d)   /* d >= 2*len+3 */
{
    *d++ = L'"';
    for (;;) {
        unsigned bs = 0;
        while (*a == L'\\') { a++; bs++; }
        if (*a == 0) {                       /* backslashes before closing " */
            for (unsigned i = 0; i < bs * 2; i++) *d++ = L'\\';
            break;
        } else if (*a == L'"') {             /* backslashes before a quote   */
            for (unsigned i = 0; i < bs * 2 + 1; i++) *d++ = L'\\';
            *d++ = L'"';
        } else {                             /* plain backslashes            */
            for (unsigned i = 0; i < bs; i++) *d++ = L'\\';
            *d++ = *a;
        }
        a++;
    }
    *d++ = L'"';
    *d = 0;
    return d;
}

/*
 * Run hsh.exe elevated with `params`, wait for it, store its exit code.
 * Returns 0 on success, nonzero if the elevation itself failed.
 */
static int launch_elevated(HANDLE err, const wchar_t *params, DWORD *code,
                           const char *who)
{
    wchar_t exe[PATHBUF], cwd[PATHBUF];
    if (!GetModuleFileNameW(NULL, exe, PATHBUF)) {
        out_printf(err, "%s: cannot locate hsh.exe\n", who);
        return 1;
    }
    if (!GetCurrentDirectoryW(PATHBUF, cwd)) cwd[0] = 0;

    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof sei);
    sei.cbSize       = sizeof sei;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";              /* <- this is what triggers UAC */
    sei.lpFile       = exe;
    sei.lpParameters = params;
    sei.lpDirectory  = cwd[0] ? cwd : NULL;   /* start in the same folder     */
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED)
            out_printf(err, "%s: elevation cancelled (UAC prompt declined)\n", who);
        else if (e == ERROR_ACCESS_DENIED)
            out_printf(err, "%s: Permission denied\n", who);
        else
            out_printf(err, "%s: could not elevate (Win32 error %lu)\n",
                       who, (unsigned long)e);
        return 1;
    }

    *code = 0;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);   /* behave like sudo */
        GetExitCodeProcess(sei.hProcess, code);
        CloseHandle(sei.hProcess);
    }
    return 0;
}

/* ---------------------------------------------------------------------- *
 *  su  /  sudo su                                                         *
 * ---------------------------------------------------------------------- */
int builtin_su(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    /* Only "become Administrator" exists on Windows, not arbitrary users. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") && strcmp(argv[i], "-l") &&
            strcmp(argv[i], "-i") && _stricmp(argv[i], "root") &&
            _stricmp(argv[i], "administrator")) {
            out_puts(err, "su: only elevation to Administrator is supported\n");
            return 1;
        }
    }
    if (is_elevated()) {
        out_puts(err, "su: this shell is already elevated (Administrator)\n");
        return 0;
    }
    out_puts(out, "su: a UAC prompt will appear; an elevated hsh opens in a "
                  "new window.\nsu: waiting for it to exit...\n");
    DWORD code = 0;
    if (launch_elevated(err, L"", &code, "su")) return 1;
    return (int)code;
}

/* ---------------------------------------------------------------------- *
 *  sudo <cmd> [args...]                                                   *
 * ---------------------------------------------------------------------- */
int builtin_sudo(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    if (argc < 2) {
        out_puts(err, "usage: sudo <command> [args...]\n       sudo su\n");
        return 1;
    }
    /* `sudo su`, `sudo -i`, `sudo -s` -> elevated interactive shell */
    if (!strcmp(argv[1], "su") || !strcmp(argv[1], "-i") || !strcmp(argv[1], "-s"))
        return builtin_su(argc - 1, argv + 1, in, out, err);

    /* Already admin: just run the command; temporarily point the process
     * standard handles at the pipe/file handles we were given so that
     * redirections like `sudo ls | wc` still work. */
    if (is_elevated()) {
        HANDLE si = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE se = GetStdHandle(STD_ERROR_HANDLE);
        SetStdHandle(STD_INPUT_HANDLE, in);
        SetStdHandle(STD_OUTPUT_HANDLE, out);
        SetStdHandle(STD_ERROR_HANDLE, err);

        Command c;
        memset(&c, 0, sizeof c);
        c.argv = argv + 1;
        c.argc = argc - 1;
        Pipeline p;
        p.cmds = &c;
        p.ncmds = 1;
        p.background = 0;
        int rc = execute_pipeline(&p, "sudo");

        SetStdHandle(STD_INPUT_HANDLE, si);
        SetStdHandle(STD_OUTPUT_HANDLE, so);
        SetStdHandle(STD_ERROR_HANDLE, se);
        return rc;
    }

    /* Not admin: build  `--run --pause "cmd" "arg1" ...`  and elevate hsh. */
    size_t cap = 64;
    wchar_t **wa = (wchar_t **)xmalloc((size_t)argc * sizeof *wa);
    for (int i = 1; i < argc; i++) {
        wa[i] = u2w(argv[i]);
        cap += 2 * wcslen(wa[i]) + 4;
    }
    wchar_t *params = (wchar_t *)xmalloc(cap * sizeof *params);
    wchar_t *d = params;
    wcscpy(d, L"--run --pause ");
    d += wcslen(d);
    for (int i = 1; i < argc; i++) {
        d = quote_arg(wa[i], d);
        if (i + 1 < argc) *d++ = L' ';
        free(wa[i]);
    }
    *d = 0;
    free(wa);

    out_printf(out, "sudo: requesting elevation (UAC) for: %s\n", argv[1]);
    DWORD code = 0;
    int bad = launch_elevated(err, params, &code, "sudo");
    free(params);
    /* NB: exported variables are not inherited by an elevated process. */
    return bad ? 1 : (int)code;
}

/* ====================================================================== *
 *  Permission simulation                                                 *
 * ====================================================================== */

/* Executable extensions: shown as x-bit and coloured green by ls. */
int hsh_is_exec_name(const wchar_t *name)
{
    static const wchar_t *ext[] = { L".exe", L".bat", L".cmd", L".com", L".ps1" };
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
        if (_wcsicmp(dot, ext[i]) == 0) return 1;
    return 0;
}

/* Say it clearly, but only once per session so scripts stay readable. */
void hsh_perm_notice(HANDLE err)
{
    static int shown = 0;
    if (shown) return;
    shown = 1;
    out_puts(err,
        "hsh: note: Unix permission bits are only an APPROXIMATION of Windows ACLs.\n"
        "     ls -l derives them from file attributes (read-only flag, .exe/.bat...);\n"
        "     chmod stores the mode in this session and toggles the read-only flag.\n");
}

/* --- override table: full path -> mode set by chmod (session only) ------ */
typedef struct { wchar_t *path; unsigned mode; } ModeOvr;
static ModeOvr *g_ovr;
static size_t g_ovr_n, g_ovr_cap;

static void normalize(const wchar_t *p, wchar_t *out)       /* out[PATHBUF] */
{
    DWORD n = GetFullPathNameW(p, PATHBUF, out, NULL);
    if (n == 0 || n >= PATHBUF) { wcsncpy(out, p, PATHBUF - 1); out[PATHBUF - 1] = 0; }
}

static ModeOvr *ovr_find(const wchar_t *full)
{
    for (size_t i = 0; i < g_ovr_n; i++)
        if (_wcsicmp(g_ovr[i].path, full) == 0) return &g_ovr[i];   /* NTFS is case-insensitive */
    return NULL;
}

static void ovr_set(const wchar_t *full, unsigned mode)
{
    ModeOvr *o = ovr_find(full);
    if (o) { o->mode = mode; return; }
    if (g_ovr_n == g_ovr_cap) {
        g_ovr_cap = g_ovr_cap ? g_ovr_cap * 2 : 16;
        g_ovr = (ModeOvr *)xrealloc(g_ovr, g_ovr_cap * sizeof *g_ovr);
    }
    size_t len = wcslen(full) + 1;
    g_ovr[g_ovr_n].path = (wchar_t *)xmalloc(len * sizeof(wchar_t));
    memcpy(g_ovr[g_ovr_n].path, full, len * sizeof(wchar_t));
    g_ovr[g_ovr_n].mode = mode;
    g_ovr_n++;
}

/* Effective (simulated) mode of a path, e.g. 0644. */
static unsigned mode_get(const wchar_t *path, DWORD attrs)
{
    if (g_ovr_n) {                       /* only normalise if chmod was used */
        wchar_t full[PATHBUF];
        normalize(path, full);
        ModeOvr *o = ovr_find(full);
        if (o) return o->mode;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return 0755;
    unsigned m = (attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644;
    if (hsh_is_exec_name(path)) m |= 0111;
    return m;
}

/* "drwxr-xr-x" style string for ls -l. out must hold 11 bytes. */
void hsh_mode_string(const wchar_t *full, DWORD attrs, char out[11])
{
    unsigned m = mode_get(full, attrs);
    out[0] = (attrs & FILE_ATTRIBUTE_DIRECTORY)     ? 'd' :
             (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? 'l' : '-';
    for (int i = 0; i < 9; i++)
        out[1 + i] = (m & (0400u >> i)) ? "rwxrwxrwx"[i] : '-';
    out[10] = '\0';
}

/*
 * Parse a chmod mode: octal ("755") or symbolic ("u+x", "a-w", "go=r",
 * "u+rw,g-w"). `cur` is the current mode (symbolic modes edit it).
 */
static int parse_mode(const char *s, unsigned cur, unsigned *result)
{
    if (!*s) return -1;
    if (isdigit((unsigned char)s[0])) {                    /* octal */
        unsigned v = 0, n = 0;
        for (const char *p = s; *p; p++) {
            if (*p < '0' || *p > '7' || ++n > 4) return -1;
            v = v * 8 + (unsigned)(*p - '0');
        }
        *result = v & 0777;
        return 0;
    }
    unsigned m = cur;
    const char *p = s;
    for (;;) {                                             /* symbolic */
        unsigned who = 0;                       /* bit0=u bit1=g bit2=o */
        for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
            who |= (*p == 'u') ? 1u : (*p == 'g') ? 2u : (*p == 'o') ? 4u : 7u;
        if (!who) who = 7;                      /* no letters = all      */
        if (*p != '+' && *p != '-' && *p != '=') return -1;
        char op = *p++;
        unsigned perm = 0;
        for (; *p == 'r' || *p == 'w' || *p == 'x'; p++)
            perm |= (*p == 'r') ? 4u : (*p == 'w') ? 2u : 1u;
        for (int k = 0; k < 3; k++) {           /* k: 0=user 1=group 2=other */
            if (!(who & (1u << k))) continue;
            int sh = 6 - 3 * k;
            if (op == '+')      m |= perm << sh;
            else if (op == '-') m &= ~(perm << sh);
            else                m = (m & ~(7u << sh)) | (perm << sh);
        }
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;
    }
    *result = m & 0777;
    return 0;
}

int builtin_chmod(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        out_puts(out, "usage: chmod MODE FILE...   (MODE: 644, 755, u+x, a-w, go=r ...)\n");
        hsh_perm_notice(out);
        return 0;
    }
    if (argc < 3) {
        out_puts(err, "usage: chmod MODE FILE...\n");
        return 1;
    }
    hsh_perm_notice(err);

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        wchar_t *w = u2w(argv[i]);
        wchar_t full[PATHBUF];
        normalize(w, full);
        DWORD attrs = GetFileAttributesW(full);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            out_printf(err, "chmod: cannot access '%s': %s\n", argv[i],
                       e == ERROR_ACCESS_DENIED ? "Permission denied"
                                                : "No such file or directory");
            rc = 1; free(w); continue;
        }
        unsigned nm;
        if (parse_mode(argv[1], mode_get(full, attrs), &nm) != 0) {
            out_printf(err, "chmod: invalid mode: '%s'\n", argv[1]);
            free(w);
            return 1;
        }
        ovr_set(full, nm);                       /* display table */

        /* Real effect: owner-write bit <-> READONLY attribute (files). */
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            DWORD keep = attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                  FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY |
                                  FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
            DWORD na = keep | ((nm & 0200) ? 0 : FILE_ATTRIBUTE_READONLY);
            if (!na) na = FILE_ATTRIBUTE_NORMAL;
            if (na != attrs && !SetFileAttributesW(full, na)) {
                DWORD e = GetLastError();
                out_printf(err, "chmod: changing permissions of '%s': %s\n", argv[i],
                           e == ERROR_ACCESS_DENIED ? "Permission denied (try sudo)"
                                                    : "failed");
                rc = 1;
            }
        }
        free(w);
    }
    return rc;
}
