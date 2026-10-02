/*
 * builtins.c -- commands implemented inside the shell process
 *
 * Every builtin has the contract signature
 *     int fn(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
 * and reads/writes ONLY through the handles it is given. That single rule is
 * what makes `ls | grep txt > out.txt` work: the executor passes a pipe or
 * file handle instead of the console, and the builtin never notices.
 *
 * Strings are UTF-8 inside the shell; every Win32 call uses the wide (W)
 * variant after converting with u2w()/w2u().
 *
 * Simplifications (fine for a coursework shell, mention them in the viva):
 *   - no wildcard expansion here (*.c must be expanded by the parser);
 *   - grep uses a small regex subset:  .  *  ^  $  \  (not full POSIX);
 *   - paths are limited to ~1000 characters (no \\?\ long-path support);
 *   - owner/group columns of ls -l are fake (see sudo.c for permissions).
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "shell.h"

#define PATHBUF 1024

/* ---- provided by sudo.c (my own file) ---------------------------------- */
int  builtin_sudo(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err);
int  builtin_su(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err);
int  builtin_chmod(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err);
void hsh_mode_string(const wchar_t *full, DWORD attrs, char out[11]);
void hsh_perm_notice(HANDLE err);
int  hsh_is_exec_name(const wchar_t *name);

/* ---- shared with main.c (my own file) ---------------------------------- */
int g_shell_exit_requested = 0;     /* `exit` sets these; main loop reads   */
int g_shell_exit_code = 0;

/* ======================================================================== *
 *  Generic helpers                                                         *
 * ======================================================================== */
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("hsh: out of memory\n", stderr); ExitProcess(1); }
    return p;
}
static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fputs("hsh: out of memory\n", stderr); ExitProcess(1); }
    return p;
}
static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n ? n : 1);
    if (!p) { fputs("hsh: out of memory\n", stderr); ExitProcess(1); }
    return p;
}
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* UTF-8 -> UTF-16 and back. Both never return NULL; caller frees. */
static wchar_t *u2w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)xmalloc((n > 0 ? (size_t)n : 1) * sizeof *w);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}
static char *w2u(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)xmalloc(n > 0 ? (size_t)n : 1);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

/* ---- output through a HANDLE (console, file or pipe) ------------------- */
static void out_write(HANDLE h, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        DWORD chunk = n > 65536 ? 65536 : (DWORD)n, w = 0;
        if (!WriteFile(h, p, chunk, &w, NULL) || w == 0)
            return;                 /* reader closed the pipe: stop quietly */
        p += w;
        n -= w;
    }
}
static void out_puts(HANDLE h, const char *s) { out_write(h, s, strlen(s)); }

static void out_printf(HANDLE h, const char *fmt, ...)
{
    char small[512];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof small) {
        out_write(h, small, (size_t)n);
    } else if (n >= 0) {                    /* too long: allocate exactly */
        char *big = (char *)xmalloc((size_t)n + 1);
        vsnprintf(big, (size_t)n + 1, fmt, ap2);
        out_write(h, big, (size_t)n);
        free(big);
    }
    va_end(ap2);
}

static int is_console(HANDLE h)
{
    DWORD m;
    return GetConsoleMode(h, &m) != 0;
}

/* ---- error messages in Unix wording ------------------------------------ */
static const char *err_text(DWORD e, char *buf, size_t n)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:    return "No such file or directory";
    case ERROR_ACCESS_DENIED:     return "Permission denied";   /* as required */
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:       return "File exists";
    case ERROR_DIR_NOT_EMPTY:     return "Directory not empty";
    case ERROR_DIRECTORY:         return "Not a directory";
    default: {
        wchar_t wb[256];
        DWORD l = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, e, 0, wb, 256, NULL);
        while (l && (wb[l - 1] == L'\r' || wb[l - 1] == L'\n' || wb[l - 1] == L' '))
            wb[--l] = 0;
        if (!l || !WideCharToMultiByte(CP_UTF8, 0, wb, -1, buf, (int)n, NULL, NULL))
            snprintf(buf, n, "error %lu", (unsigned long)e);
        return buf;
    }
    }
}
/* "cmd: path: reason" */
static void report(HANDLE err, const char *cmd, const char *what, DWORD e)
{
    char tmp[512];
    out_printf(err, "%s: %s: %s\n", cmd, what, err_text(e, tmp, sizeof tmp));
}
static void report_w(HANDLE err, const char *cmd, const wchar_t *what, DWORD e)
{
    char *u = w2u(what);
    report(err, cmd, u, e);
    free(u);
}

/* ---- environment ------------------------------------------------------- */
static char *env_get(const char *name)           /* malloc'd UTF-8 or NULL */
{
    wchar_t *wn = u2w(name);
    DWORD n = GetEnvironmentVariableW(wn, NULL, 0);
    if (n == 0) { free(wn); return NULL; }
    wchar_t *v = (wchar_t *)xmalloc(n * sizeof *v);
    GetEnvironmentVariableW(wn, v, n);
    char *u = w2u(v);
    free(v); free(wn);
    return u;
}
static void env_set(const char *name, const char *val)   /* val NULL = unset */
{
    wchar_t *wn = u2w(name), *wv = val ? u2w(val) : NULL;
    SetEnvironmentVariableW(wn, wv);
    free(wn); free(wv);
}
static char *cwd_utf8(void)
{
    wchar_t b[PATHBUF];
    DWORD n = GetCurrentDirectoryW(PATHBUF, b);
    return (n == 0 || n >= PATHBUF) ? NULL : w2u(b);
}

static int is_dir_w(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* "<dir>\*" pattern for FindFirstFileW */
static void dir_pattern(wchar_t *out, size_t n, const wchar_t *dir)
{
    size_t l = wcslen(dir);
    int sep = l && (dir[l - 1] == L'\\' || dir[l - 1] == L'/');
    _snwprintf(out, n, L"%ls%ls*", dir, sep ? L"" : L"\\");
    out[n - 1] = 0;
}
/* "<dir>\<name>" */
static void path_join(wchar_t *out, size_t n, const wchar_t *dir, const wchar_t *name)
{
    size_t l = wcslen(dir);
    int sep = l && (dir[l - 1] == L'\\' || dir[l - 1] == L'/');
    _snwprintf(out, n, L"%ls%ls%ls", dir, sep ? L"" : L"\\", name);
    out[n - 1] = 0;
}
/* "<dir>\<last component of src>"  (used by cp/mv when dst is a directory) */
static void path_join_base(wchar_t *out, size_t n, const wchar_t *dir, const wchar_t *src)
{
    wchar_t s[PATHBUF];
    wcsncpy(s, src, PATHBUF - 1);
    s[PATHBUF - 1] = 0;
    size_t l = wcslen(s);
    while (l > 1 && (s[l - 1] == L'\\' || s[l - 1] == L'/')) s[--l] = 0;
    wchar_t *b = s;
    for (wchar_t *p = s; *p; p++)
        if ((*p == L'\\' || *p == L'/') && p[1]) b = p + 1;
    path_join(out, n, dir, b);
}

/* Open a file for reading, printing a Unix-style error on failure. */
static HANDLE open_input(const char *cmd, const char *path, HANDLE err)
{
    wchar_t *w = u2w(path);
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        out_printf(err, "%s: %s: Is a directory\n", cmd, path);
        free(w);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(w, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) report(err, cmd, path, GetLastError());
    free(w);
    return h;
}

/* ======================================================================== *
 *  LineReader: streaming "read one line at a time" from any HANDLE.        *
 *  Streaming matters: `ping host | grep x` must print as data arrives.     *
 * ======================================================================== */
typedef struct {
    HANDLE h;
    char  *buf;
    size_t cap, start, end;     /* valid data is buf[start..end) */
    int    eof;
} LineReader;

static void lr_init(LineReader *r, HANDLE h)
{
    r->h = h; r->cap = 65536; r->buf = (char *)xmalloc(r->cap);
    r->start = r->end = 0; r->eof = 0;
}
static void lr_free(LineReader *r) { free(r->buf); }

/* Returns 1 with a NUL-terminated line (no \n, no trailing \r) or 0 at EOF. */
static int lr_getline(LineReader *r, char **line, size_t *len)
{
    size_t scan = r->start;
    for (;;) {
        char *nl = r->end > scan ? (char *)memchr(r->buf + scan, '\n', r->end - scan) : NULL;
        if (nl) {
            size_t l = (size_t)(nl - (r->buf + r->start));
            *nl = '\0';
            *line = r->buf + r->start;
            r->start = (size_t)(nl - r->buf) + 1;
            if (l && (*line)[l - 1] == '\r') (*line)[--l] = '\0';
            *len = l;
            return 1;
        }
        scan = r->end;
        if (r->eof) {                                   /* last line, no \n */
            if (r->end == r->start) return 0;
            r->buf[r->end] = '\0';
            *line = r->buf + r->start;
            size_t l = r->end - r->start;
            r->start = r->end;
            if (l && (*line)[l - 1] == '\r') (*line)[--l] = '\0';
            *len = l;
            return 1;
        }
        if (r->start > 0) {                             /* compact */
            memmove(r->buf, r->buf + r->start, r->end - r->start);
            scan -= r->start; r->end -= r->start; r->start = 0;
        }
        if (r->end + 1 >= r->cap) {                     /* grow */
            r->cap *= 2;
            r->buf = (char *)xrealloc(r->buf, r->cap);
        }
        DWORD got = 0;
        if (!ReadFile(r->h, r->buf + r->end, (DWORD)(r->cap - 1 - r->end), &got, NULL) || got == 0)
            r->eof = 1;                                 /* EOF or broken pipe */
        else
            r->end += got;
    }
}

/* ======================================================================== *
 *  cd  pwd  exit  echo  export  unset  env  clear                          *
 * ======================================================================== */
static int bi_cd(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    if (argc > 2) { out_puts(err, "cd: too many arguments\n"); return 1; }

    char *target = NULL;
    int show = 0;                                   /* `cd -` prints the dir */
    if (argc == 1 || !strcmp(argv[1], "~")) {
        target = env_get("USERPROFILE");            /* Windows' $HOME */
        if (!target) { out_puts(err, "cd: HOME not set\n"); return 1; }
    } else if (!strcmp(argv[1], "-")) {
        target = env_get("OLDPWD");
        if (!target) { out_puts(err, "cd: OLDPWD not set\n"); return 1; }
        show = 1;
    } else if (argv[1][0] == '~' && (argv[1][1] == '/' || argv[1][1] == '\\')) {
        char *h = env_get("USERPROFILE");
        if (!h) { out_puts(err, "cd: HOME not set\n"); return 1; }
        size_t n = strlen(h) + strlen(argv[1]) + 1;
        target = (char *)xmalloc(n);
        snprintf(target, n, "%s%s", h, argv[1] + 1);
        free(h);
    } else {
        target = xstrdup(argv[1]);
    }

    char *old = cwd_utf8();
    wchar_t *w = u2w(target);
    int rc = 0;
    if (!SetCurrentDirectoryW(w)) {
        report(err, "cd", target, GetLastError());
        rc = 1;
    } else {
        if (old) env_set("OLDPWD", old);            /* remembered for `cd -` */
        char *now = cwd_utf8();
        if (now) {
            env_set("PWD", now);
            if (show) out_printf(out, "%s\n", now);
            free(now);
        }
    }
    free(w); free(target); free(old);
    return rc;
}

static int bi_pwd(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)argc; (void)argv; (void)in;
    char *c = cwd_utf8();
    if (!c) { out_puts(err, "pwd: cannot determine current directory\n"); return 1; }
    out_printf(out, "%s\n", c);
    free(c);
    return 0;
}

static int bi_exit(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int code = 0;
    if (argc > 1) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end || end == argv[1]) {
            out_printf(err, "exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        } else code = (int)(v & 0xFF);              /* like a Unix status */
    }
    g_shell_exit_code = code;
    g_shell_exit_requested = 1;                     /* main loop sees this */
    return code;
}

static void put_escaped(HANDLE out, const char *s)      /* echo -e */
{
    char *b = (char *)xmalloc(strlen(s) + 1), *d = b;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': *d++ = '\n'; break;
            case 't': *d++ = '\t'; break;
            case 'r': *d++ = '\r'; break;
            case 'a': *d++ = '\a'; break;
            case 'e': *d++ = '\x1b'; break;      /* ESC: lets you print colours */
            case '\\': *d++ = '\\'; break;
            default:  *d++ = '\\'; *d++ = *s; break;
            }
        } else *d++ = *s;
    }
    out_write(out, b, (size_t)(d - b));
    free(b);
}

static int bi_echo(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)err;
    int newline = 1, esc = 0, i = 1;
    for (; i < argc; i++) {                         /* leading -n -e -E flags */
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        const char *p = a + 1;
        while (*p && strchr("neE", *p)) p++;
        if (*p) break;                              /* not a pure flag word */
        for (p = a + 1; *p; p++) {
            if (*p == 'n') newline = 0;
            else if (*p == 'e') esc = 1;
            else esc = 0;
        }
    }
    for (; i < argc; i++) {
        if (esc) put_escaped(out, argv[i]); else out_puts(out, argv[i]);
        if (i + 1 < argc) out_puts(out, " ");
    }
    if (newline) out_puts(out, "\n");
    return 0;
}

static int valid_name(const char *s)
{
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

/* Print the environment block; declare=1 -> `declare -x NAME="value"`. */
static void env_dump(HANDLE out, int declare)
{
    wchar_t *blk = GetEnvironmentStringsW();
    if (!blk) return;
    for (wchar_t *p = blk; *p; p += wcslen(p) + 1) {
        if (*p == L'=') continue;                   /* hidden "=C:" entries */
        char *u = w2u(p);
        if (declare) {
            char *eq = strchr(u, '=');
            if (eq) { *eq = '\0'; out_printf(out, "declare -x %s=\"%s\"\n", u, eq + 1); }
        } else out_printf(out, "%s\n", u);
        free(u);
    }
    FreeEnvironmentStringsW(blk);
}

static int bi_export(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    if (argc == 1) { env_dump(out, 1); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name = eq ? (char *)memcpy(xcalloc((size_t)(eq - argv[i]) + 1, 1), argv[i],
                                         (size_t)(eq - argv[i]))
                        : xstrdup(argv[i]);
        if (!valid_name(name)) {
            out_printf(err, "export: `%s': not a valid identifier\n", argv[i]);
            rc = 1;
        } else if (eq) {
            env_set(name, eq + 1);                  /* inherited by children */
        }
        free(name);
    }
    return rc;
}

static int bi_unset(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out; (void)err;
    for (int i = 1; i < argc; i++) env_set(argv[i], NULL);
    return 0;
}

static int bi_env(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)argc; (void)argv; (void)in; (void)err;
    env_dump(out, 0);
    return 0;
}

static int bi_clear(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)argc; (void)argv; (void)in; (void)err;
    out_puts(out, "\x1b[2J\x1b[3J\x1b[H");          /* VT: erase + home */
    return 0;
}

/* ======================================================================== *
 *  history  jobs  fg  bg  which  type  help                                *
 * ======================================================================== */
static char **g_hist;
static size_t g_hist_n, g_hist_cap;

/* Called by main.c for every non-empty line (skips consecutive duplicates). */
void builtins_history_add(const char *line)
{
    if (!line || !*line) return;
    if (g_hist_n && !strcmp(g_hist[g_hist_n - 1], line)) return;
    if (g_hist_n == g_hist_cap) {
        g_hist_cap = g_hist_cap ? g_hist_cap * 2 : 64;
        g_hist = (char **)xrealloc(g_hist, g_hist_cap * sizeof *g_hist);
    }
    g_hist[g_hist_n++] = xstrdup(line);
}

static int bi_history(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    size_t start = 0;
    if (argc > 1 && !strcmp(argv[1], "-c")) {       /* clear */
        for (size_t i = 0; i < g_hist_n; i++) free(g_hist[i]);
        g_hist_n = 0;
        return 0;
    }
    if (argc > 1) {                                 /* history N = last N */
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (*end || n < 0) {
            out_printf(err, "history: %s: numeric argument required\n", argv[1]);
            return 1;
        }
        if ((size_t)n < g_hist_n) start = g_hist_n - (size_t)n;
    }
    for (size_t i = start; i < g_hist_n; i++)
        out_printf(out, "%5zu  %s\n", i + 1, g_hist[i]);
    return 0;
}

static int bi_jobs(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)argc; (void)argv; (void)in; (void)err;
    jobs_list(out);                                 /* jobs module prints */
    return 0;
}

static int parse_job_id(const char *s, int *id)     /* accepts "2" or "%2" */
{
    if (*s == '%') s++;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v <= 0 || v > INT_MAX) return -1;
    *id = (int)v;
    return 0;
}

static int bi_fg(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int id;
    if (argc != 2 || parse_job_id(argv[1], &id)) {
        out_puts(err, "usage: fg %job_id    (see `jobs`)\n");
        return 1;
    }
    return fg_job(id);          /* jobs module reports "no such job" itself */
}

static int bi_bg(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int id;
    if (argc != 2 || parse_job_id(argv[1], &id)) {
        out_puts(err, "usage: bg %job_id    (see `jobs`)\n");
        return 1;
    }
    return bg_job(id);
}

static int bi_which(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)err;
    int rc = argc > 1 ? 0 : 1;
    for (int i = 1; i < argc; i++) {
        char path[PATHBUF];
        if (find_builtin(argv[i]))
            out_printf(out, "%s: shell built-in command\n", argv[i]);
        else if (resolve_program(argv[i], path, sizeof path) == 0)
            out_printf(out, "%s\n", path);
        else
            rc = 1;                                 /* silent, like `which` */
    }
    return rc;
}

static int bi_type(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATHBUF];
        if (find_builtin(argv[i]))
            out_printf(out, "%s is a shell builtin\n", argv[i]);
        else if (resolve_program(argv[i], path, sizeof path) == 0)
            out_printf(out, "%s is %s\n", argv[i], path);
        else {
            out_printf(err, "hsh: type: %s: not found\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static int bi_help(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)argc; (void)argv; (void)in; (void)err;
    out_puts(out,
        "hsh built-in commands:\n"
        "  cd [dir|-|~]  pwd  exit [n]  echo [-n -e]  export [N=V]  unset N  env  clear\n"
        "  history [-c|N]  jobs  fg %id  bg %id  which  type  help\n"
        "  ls [-l -a -h]  cat  cp [-r]  mv  rm [-r -f]  mkdir [-p]  touch\n"
        "  head [-n N]  tail [-n N]  wc [-l -w -c]  grep [-i -n -r] PATTERN [files]\n"
        "  sudo CMD  su  chmod MODE FILE   (permissions are simulated, see chmod --help)\n"
        "Syntax: cmd1 | cmd2   < in   > out   >> out   2> err   2>&1   cmd &\n");
    return 0;
}

/* ======================================================================== *
 *  ls                                                                      *
 * ======================================================================== */
typedef struct { int l, a, h; } LsOpts;
typedef struct {
    wchar_t   name[MAX_PATH];
    wchar_t   full[MAX_PATH];
    DWORD     attrs;
    ULONGLONG size;
    FILETIME  mtime;
} LsEntry;

static int cmp_entry(const void *a, const void *b)
{
    return _wcsicmp(((const LsEntry *)a)->name, ((const LsEntry *)b)->name);
}

static void fmt_size(ULONGLONG s, int human, char *buf, size_t n)
{
    if (!human) { snprintf(buf, n, "%llu", (unsigned long long)s); return; }
    double v = (double)s;
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (i == 0)       snprintf(buf, n, "%llu", (unsigned long long)s);
    else if (v < 10)  snprintf(buf, n, "%.1f%c", v, "BKMGTP"[i]);
    else              snprintf(buf, n, "%.0f%c", v, "BKMGTP"[i]);
}

static void fmt_time(const FILETIME *ft, int curyear, char *buf, size_t n)
{
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    FILETIME lf;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(ft, &lf) || !FileTimeToSystemTime(&lf, &st)) {
        snprintf(buf, n, "??? ?? ?????");
        return;
    }
    if (st.wYear == curyear)
        snprintf(buf, n, "%s %2d %02d:%02d", mon[st.wMonth - 1], st.wDay, st.wHour, st.wMinute);
    else
        snprintf(buf, n, "%s %2d  %d", mon[st.wMonth - 1], st.wDay, st.wYear);
}

/* Name with colour: blue dirs, cyan links, green executables. */
static void ls_put_name(HANDLE out, const LsEntry *e, int color)
{
    char *u = w2u(e->name);
    const char *pre = "", *post = "";
    if (color) {
        if (e->attrs & FILE_ATTRIBUTE_DIRECTORY)          pre = "\x1b[1;34m";
        else if (e->attrs & FILE_ATTRIBUTE_REPARSE_POINT) pre = "\x1b[1;36m";
        else if (hsh_is_exec_name(e->name))               pre = "\x1b[1;32m";
        if (*pre) post = "\x1b[0m";
    }
    out_printf(out, "%s%s%s", pre, u, post);
    free(u);
}

static void ls_long(HANDLE out, const LsEntry *e, const LsOpts *o, int color,
                    const char *owner, int year)
{
    char mode[11], sz[32], tm[32];
    hsh_mode_string(e->full, e->attrs, mode);       /* simulated rwx string */
    fmt_size(e->size, o->h, sz, sizeof sz);
    fmt_time(&e->mtime, year, tm, sizeof tm);
    /* link count is always 1; owner/group are placeholders on Windows */
    out_printf(out, "%s %2d %-8s %-8s %8s %s ", mode, 1, owner, "Users", sz, tm);
    ls_put_name(out, e, color);
    out_puts(out, "\n");
}

static int console_width(HANDLE h)
{
    CONSOLE_SCREEN_BUFFER_INFO i;
    return GetConsoleScreenBufferInfo(h, &i) ? i.srWindow.Right - i.srWindow.Left + 1 : 80;
}

/* Sort and print a batch of entries (long, columns, or one per line). */
static void ls_emit(HANDLE out, LsEntry *v, size_t n, const LsOpts *o, int color,
                    const char *owner, int year)
{
    if (!n) return;
    qsort(v, n, sizeof *v, cmp_entry);

    if (o->l) {
        for (size_t i = 0; i < n; i++) ls_long(out, &v[i], o, color, owner, year);
        return;
    }
    if (!is_console(out)) {                         /* piped: one per line */
        for (size_t i = 0; i < n; i++) { ls_put_name(out, &v[i], 0); out_puts(out, "\n"); }
        return;
    }
    /* console: fill columns top-to-bottom like real ls */
    size_t maxlen = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = wcslen(v[i].name);
        if (l > maxlen) maxlen = l;
    }
    size_t colw = maxlen + 2;
    size_t cols = (size_t)console_width(out) / colw;
    if (cols < 1) cols = 1;
    size_t rows = (n + cols - 1) / cols;
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t idx = c * rows + r;
            if (idx >= n) break;
            ls_put_name(out, &v[idx], color);
            if ((c + 1) * rows + r < n)             /* pad only if a next column exists */
                out_printf(out, "%*s", (int)(colw - wcslen(v[idx].name)), "");
        }
        out_puts(out, "\n");
    }
}

static int ls_dir(HANDLE out, HANDLE err, const char *shown, const wchar_t *dir,
                  const LsOpts *o, int color, const char *owner, int year,
                  int header, int *printed)
{
    wchar_t pat[PATHBUF];
    dir_pattern(pat, PATHBUF, dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { report(err, "ls", shown, GetLastError()); return 2; }

    LsEntry *v = NULL;
    size_t n = 0, cap = 0;
    do {
        if (!o->a && fd.cFileName[0] == L'.') continue;                       /* dotfiles   */
        if (!o->a && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue; /* hidden attr */
        if (n == cap) { cap = cap ? cap * 2 : 64; v = (LsEntry *)xrealloc(v, cap * sizeof *v); }
        LsEntry *e = &v[n++];
        wcsncpy(e->name, fd.cFileName, MAX_PATH - 1);
        e->name[MAX_PATH - 1] = 0;
        path_join(e->full, MAX_PATH, dir, fd.cFileName);
        e->attrs = fd.dwFileAttributes;
        e->size  = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e->mtime = fd.ftLastWriteTime;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (header) {
        if (*printed) out_puts(out, "\n");
        out_printf(out, "%s:\n", shown);
    }
    ls_emit(out, v, n, o, color, owner, year);
    *printed = 1;
    free(v);
    return 0;
}

static int bi_ls(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in;
    LsOpts o = { 0, 0, 0 };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {   /* combined: -lah */
            if (*p == 'l') o.l = 1;
            else if (*p == 'a' || *p == 'A') o.a = 1;
            else if (*p == 'h') o.h = 1;
            else if (*p == '1') { /* one per line: already the piped default */ }
            else { out_printf(err, "ls: invalid option -- '%c'\n", *p); return 2; }
        }
    }
    if (o.l) hsh_perm_notice(err);                   /* "approximation" notice */

    int color = is_console(out);
    char *owner = env_get("USERNAME");
    if (!owner) owner = xstrdup("user");
    SYSTEMTIME now; GetLocalTime(&now);

    const char *dot = ".";
    char **paths = argv + i;
    int np = argc - i;
    if (np == 0) { paths = (char **)&dot; np = 1; }

    /* Like real ls: files named on the command line first, then each directory. */
    LsEntry *files = NULL;
    size_t nf = 0, capf = 0;
    wchar_t **dirs = (wchar_t **)xcalloc((size_t)np, sizeof *dirs);
    const char **dshown = (const char **)xcalloc((size_t)np, sizeof *dshown);
    int nd = 0, rc = 0;

    for (int k = 0; k < np; k++) {
        wchar_t *w = u2w(paths[k]);
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (!GetFileAttributesExW(w, GetFileExInfoStandard, &ad)) {
            report(err, "ls", paths[k], GetLastError());
            rc = 2; free(w); continue;
        }
        if (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs[nd] = w; dshown[nd] = paths[k]; nd++;
        } else {
            if (nf == capf) { capf = capf ? capf * 2 : 16; files = (LsEntry *)xrealloc(files, capf * sizeof *files); }
            LsEntry *e = &files[nf++];
            wcsncpy(e->name, w, MAX_PATH - 1); e->name[MAX_PATH - 1] = 0;
            wcsncpy(e->full, w, MAX_PATH - 1); e->full[MAX_PATH - 1] = 0;
            e->attrs = ad.dwFileAttributes;
            e->size  = ((ULONGLONG)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            e->mtime = ad.ftLastWriteTime;
            free(w);
        }
    }

    int printed = 0;
    if (nf) { ls_emit(out, files, nf, &o, color, owner, now.wYear); printed = 1; }
    for (int k = 0; k < nd; k++) {
        if (ls_dir(out, err, dshown[k], dirs[k], &o, color, owner, now.wYear,
                   np > 1, &printed)) rc = 2;
        free(dirs[k]);
    }
    free(files); free(dirs); free(dshown); free(owner);
    return rc;
}

/* ======================================================================== *
 *  cat  head  tail  wc                                                     *
 * ======================================================================== */
static void copy_stream(HANDLE in, HANDLE out)
{
    char *buf = (char *)xmalloc(65536);
    DWORD got;
    while (ReadFile(in, buf, 65536, &got, NULL) && got) {
        DWORD done = 0;
        while (done < got) {
            DWORD w = 0;
            if (!WriteFile(out, buf + done, got - done, &w, NULL) || !w) { free(buf); return; }
            done += w;
        }
    }
    free(buf);
}

static int bi_cat(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    if (argc == 1) { copy_stream(in, out); return 0; }       /* cat | ... */
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) { copy_stream(in, out); continue; }
        HANDLE h = open_input("cat", argv[i], err);
        if (h == INVALID_HANDLE_VALUE) { rc = 1; continue; }
        copy_stream(h, out);
        CloseHandle(h);
    }
    return rc;
}

/* shared option parser for head/tail: -n N | -nN | -N */
static int parse_count_args(const char *cmd, int argc, char **argv, HANDLE err,
                            long *n, int *first_file)
{
    *n = 10;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *num;
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-n")) {
            if (i + 1 >= argc) { out_printf(err, "%s: option requires an argument -- 'n'\n", cmd); return -1; }
            num = argv[i + 1]; i += 2;
        } else if (argv[i][1] == 'n') { num = argv[i] + 2; i++; }
        else if (isdigit((unsigned char)argv[i][1])) { num = argv[i] + 1; i++; }
        else { out_printf(err, "%s: invalid option -- '%c'\n", cmd, argv[i][1]); return -1; }
        char *end;
        long v = strtol(num, &end, 10);
        if (end == num || *end || v < 0) {
            out_printf(err, "%s: invalid number of lines: '%s'\n", cmd, num);
            return -1;
        }
        *n = v;
    }
    *first_file = i;
    return 0;
}

static void head_stream(HANDLE in, HANDLE out, long n)
{
    LineReader r; lr_init(&r, in);
    char *l; size_t len; long c = 0;
    while (c < n && lr_getline(&r, &l, &len)) {     /* stops early: cheap on huge files */
        out_write(out, l, len); out_write(out, "\n", 1); c++;
    }
    lr_free(&r);
}

static void tail_stream(HANDLE in, HANDLE out, long n)
{
    if (n == 0) return;
    if (n > 1000000) n = 1000000;                    /* sanity cap */
    char **ring = (char **)xcalloc((size_t)n, sizeof *ring);   /* circular buffer */
    LineReader r; lr_init(&r, in);
    char *l; size_t len, count = 0;
    while (lr_getline(&r, &l, &len)) {
        size_t slot = count % (size_t)n;
        free(ring[slot]);
        ring[slot] = xstrdup(l);                     /* keep only last n lines */
        count++;
    }
    lr_free(&r);
    size_t start = count > (size_t)n ? count - (size_t)n : 0;
    for (size_t k = start; k < count; k++) {
        char *s = ring[k % (size_t)n];
        out_write(out, s, strlen(s)); out_write(out, "\n", 1);
    }
    for (long k = 0; k < n; k++) free(ring[k]);
    free(ring);
}

static int lines_cmd(const char *cmd, int tail, int argc, char **argv,
                     HANDLE in, HANDLE out, HANDLE err)
{
    long n; int first;
    if (parse_count_args(cmd, argc, argv, err, &n, &first)) return 1;
    int nfiles = argc - first, rc = 0;
    if (nfiles == 0) {
        if (tail) tail_stream(in, out, n); else head_stream(in, out, n);
        return 0;
    }
    for (int i = first; i < argc; i++) {
        int dash = !strcmp(argv[i], "-");
        HANDLE h = dash ? in : open_input(cmd, argv[i], err);
        if (h == INVALID_HANDLE_VALUE) { rc = 1; continue; }
        if (nfiles > 1)
            out_printf(out, "%s==> %s <==\n", i > first ? "\n" : "", dash ? "standard input" : argv[i]);
        if (tail) tail_stream(h, out, n); else head_stream(h, out, n);
        if (!dash) CloseHandle(h);
    }
    return rc;
}
static int bi_head(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{ return lines_cmd("head", 0, argc, argv, in, out, err); }
static int bi_tail(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{ return lines_cmd("tail", 1, argc, argv, in, out, err); }

typedef struct { unsigned long long l, w, c; } WcCount;

static void wc_count(HANDLE h, WcCount *t)
{
    char *buf = (char *)xmalloc(65536);
    DWORD got;
    int inword = 0;                                  /* survives chunk borders */
    while (ReadFile(h, buf, 65536, &got, NULL) && got) {
        t->c += got;
        for (DWORD k = 0; k < got; k++) {
            unsigned char ch = (unsigned char)buf[k];
            if (ch == '\n') t->l++;
            if (isspace(ch)) inword = 0;
            else if (!inword) { inword = 1; t->w++; }
        }
    }
    free(buf);
}

static void wc_print(HANDLE out, const WcCount *c, int fl, int fw, int fc, const char *name)
{
    int width = (fl + fw + fc) > 1 ? 7 : 0, first = 1;
    if (fl) { out_printf(out, "%*llu", width, c->l); first = 0; }
    if (fw) { out_printf(out, "%s%*llu", first ? "" : " ", width, c->w); first = 0; }
    if (fc) { out_printf(out, "%s%*llu", first ? "" : " ", width, c->c); }
    if (name) out_printf(out, " %s", name);
    out_puts(out, "\n");
}

static int bi_wc(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    int fl = 0, fw = 0, fc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l') fl = 1; else if (*p == 'w') fw = 1;
            else if (*p == 'c' || *p == 'm') fc = 1;
            else { out_printf(err, "wc: invalid option -- '%c'\n", *p); return 1; }
        }
    }
    if (!fl && !fw && !fc) fl = fw = fc = 1;         /* default: all three */

    if (i >= argc) {                                  /* stdin */
        WcCount t = { 0, 0, 0 };
        wc_count(in, &t);
        wc_print(out, &t, fl, fw, fc, NULL);
        return 0;
    }
    WcCount total = { 0, 0, 0 };
    int rc = 0, nfiles = argc - i;
    for (; i < argc; i++) {
        WcCount t = { 0, 0, 0 };
        HANDLE h = open_input("wc", argv[i], err);
        if (h == INVALID_HANDLE_VALUE) { rc = 1; continue; }
        wc_count(h, &t);
        CloseHandle(h);
        wc_print(out, &t, fl, fw, fc, argv[i]);
        total.l += t.l; total.w += t.w; total.c += t.c;
    }
    if (nfiles > 1) wc_print(out, &total, fl, fw, fc, "total");
    return rc;
}

/* ======================================================================== *
 *  grep  (tiny regex engine: Kernighan & Pike's matcher + backslash)       *
 *  Supports  c  .  *  ^  $  \c       -- explain it in the viva!           *
 * ======================================================================== */
static int chr_eq(int pc, int tc, int ic)
{
    return ic ? tolower(pc) == tolower(tc) : pc == tc;
}
static int matchhere(const char *re, const char *t, int ic);

static int matchstar(int c, int any, const char *re, const char *t, int ic)
{
    for (;;) {                                       /* c* : try 0,1,2.. copies */
        if (matchhere(re, t, ic)) return 1;
        if (*t == '\0') return 0;
        if (!(any || chr_eq(c, (unsigned char)*t, ic))) return 0;
        t++;
    }
}
static int matchhere(const char *re, const char *t, int ic)
{
    if (re[0] == '\0') return 1;
    int c = (unsigned char)re[0], literal = 0;
    const char *next = re + 1;
    if (c == '\\' && re[1]) { c = (unsigned char)re[1]; literal = 1; next = re + 2; }
    int any = (!literal && c == '.');
    if (*next == '*') return matchstar(c, any, next + 1, t, ic);
    if (!literal && c == '$' && *next == '\0') return *t == '\0';
    if (*t && (any || chr_eq(c, (unsigned char)*t, ic))) return matchhere(next, t + 1, ic);
    return 0;
}
static int re_match(const char *re, const char *t, int ic)
{
    if (re[0] == '^') return matchhere(re + 1, t, ic);
    do { if (matchhere(re, t, ic)) return 1; } while (*t++ != '\0');
    return 0;
}

typedef struct {
    int icase, num, rec, show_name, implicit_dot, matched;
    const char *pat;
    HANDLE out, err;
} GrepCtx;

static void grep_stream(GrepCtx *g, HANDLE h, const char *name)
{
    LineReader r; lr_init(&r, h);
    char *l; size_t len; unsigned long long ln = 0;
    while (lr_getline(&r, &l, &len)) {
        ln++;
        if (!re_match(g->pat, l, g->icase)) continue;
        g->matched = 1;
        if (g->show_name) out_printf(g->out, "%s:", name);
        if (g->num)       out_printf(g->out, "%llu:", ln);
        out_write(g->out, l, len);
        out_write(g->out, "\n", 1);
    }
    lr_free(&r);
}

static int grep_path(GrepCtx *g, const char *path)
{
    wchar_t *w = u2w(path);
    DWORD a = GetFileAttributesW(w);
    int rc = 0;
    if (a == INVALID_FILE_ATTRIBUTES) {
        report(g->err, "grep", path, GetLastError());
        free(w);
        return 2;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!g->rec) {
            out_printf(g->err, "grep: %s: Is a directory\n", path);
            free(w);
            return 2;
        }
        wchar_t pat[PATHBUF];
        dir_pattern(pat, PATHBUF, w);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;                        /* don't follow junctions: avoids loops */
                char *name = w2u(fd.cFileName);
                size_t pl = strlen(path);
                size_t n = pl + strlen(name) + 2;
                char *child = (char *)xmalloc(n);
                if (g->implicit_dot && !strcmp(path, "."))
                    snprintf(child, n, "%s", name);              /* `grep -r x` -> "file" */
                else
                    snprintf(child, n, "%s%s%s", path,
                             (pl && (path[pl - 1] == '/' || path[pl - 1] == '\\')) ? "" : "/", name);
                if (grep_path(g, child)) rc = 2;
                free(child); free(name);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        HANDLE h = open_input("grep", path, g->err);
        if (h == INVALID_HANDLE_VALUE) rc = 2;
        else { grep_stream(g, h, path); CloseHandle(h); }
    }
    free(w);
    return rc;
}

static int bi_grep(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    GrepCtx g;
    memset(&g, 0, sizeof g);
    g.out = out; g.err = err;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'i') g.icase = 1;
            else if (*p == 'n') g.num = 1;
            else if (*p == 'r' || *p == 'R') g.rec = 1;
            else { out_printf(err, "grep: invalid option -- '%c'\n", *p); return 2; }
        }
    }
    if (i >= argc) { out_puts(err, "usage: grep [-i] [-n] [-r] PATTERN [FILE...]\n"); return 2; }
    g.pat = argv[i++];

    int nfiles = argc - i, rc = 0;
    g.show_name = g.rec || nfiles > 1;
    if (nfiles == 0) {
        if (g.rec) { g.implicit_dot = 1; rc = grep_path(&g, "."); }
        else grep_stream(&g, in, "(standard input)");
    } else {
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "-")) grep_stream(&g, in, "(standard input)");
            else if (grep_path(&g, argv[i])) rc = 2;
        }
    }
    if (rc == 2) return 2;                           /* error */
    return g.matched ? 0 : 1;                        /* 0 match, 1 no match */
}

/* ======================================================================== *
 *  cp  mv  rm  mkdir  touch                                                *
 * ======================================================================== */

/* Is `dst` the same as, or inside, directory `src`? (prevents cp -r a a\b) */
static int path_inside(const wchar_t *src, const wchar_t *dst)
{
    wchar_t s[PATHBUF], d[PATHBUF];
    if (!GetFullPathNameW(src, PATHBUF, s, NULL) || !GetFullPathNameW(dst, PATHBUF, d, NULL))
        return 0;
    size_t sl = wcslen(s);
    while (sl > 3 && (s[sl - 1] == L'\\' || s[sl - 1] == L'/')) s[--sl] = 0;
    return _wcsnicmp(d, s, sl) == 0 && (d[sl] == 0 || d[sl] == L'\\' || d[sl] == L'/');
}

static int cp_item(HANDLE err, const wchar_t *src, const wchar_t *dst, int rec)
{
    DWORD a = GetFileAttributesW(src);
    if (a == INVALID_FILE_ATTRIBUTES) { report_w(err, "cp", src, GetLastError()); return 1; }

    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!CopyFileW(src, dst, FALSE)) { report_w(err, "cp", dst, GetLastError()); return 1; }
        return 0;
    }
    char *us = w2u(src);
    if (!rec) {
        out_printf(err, "cp: -r not specified; omitting directory '%s'\n", us);
        free(us);
        return 1;
    }
    if (path_inside(src, dst)) {
        out_printf(err, "cp: cannot copy a directory, '%s', into itself\n", us);
        free(us);
        return 1;
    }
    free(us);
    if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        report_w(err, "cp", dst, GetLastError());
        return 1;
    }
    wchar_t pat[PATHBUF];
    dir_pattern(pat, PATHBUF, src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int rc = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t cs[PATHBUF], cd[PATHBUF];
            path_join(cs, PATHBUF, src, fd.cFileName);
            path_join(cd, PATHBUF, dst, fd.cFileName);
            if (cp_item(err, cs, cd, rec)) rc = 1;      /* recursion */
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return rc;
}

static int bi_cp(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int rec = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') rec = 1;
            else if (*p == 'f' || *p == 'i' || *p == 'v') { /* accepted, ignored */ }
            else { out_printf(err, "cp: invalid option -- '%c'\n", *p); return 1; }
        }
    }
    if (argc - i < 2) { out_puts(err, "usage: cp [-r] SOURCE... DEST\n"); return 1; }

    wchar_t *dst = u2w(argv[argc - 1]);
    int dst_dir = is_dir_w(dst), rc = 0;
    if (argc - i > 2 && !dst_dir) {
        out_printf(err, "cp: target '%s' is not a directory\n", argv[argc - 1]);
        free(dst);
        return 1;
    }
    for (int k = i; k < argc - 1; k++) {
        wchar_t *src = u2w(argv[k]);
        wchar_t target[PATHBUF];
        if (dst_dir) path_join_base(target, PATHBUF, dst, src);
        else { wcsncpy(target, dst, PATHBUF - 1); target[PATHBUF - 1] = 0; }
        if (cp_item(err, src, target, rec)) rc = 1;
        free(src);
    }
    free(dst);
    return rc;
}

static int bi_mv(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (argc - i < 2) { out_puts(err, "usage: mv SOURCE... DEST\n"); return 1; }

    wchar_t *dst = u2w(argv[argc - 1]);
    int dst_dir = is_dir_w(dst), rc = 0;
    if (argc - i > 2 && !dst_dir) {
        out_printf(err, "mv: target '%s' is not a directory\n", argv[argc - 1]);
        free(dst);
        return 1;
    }
    for (int k = i; k < argc - 1; k++) {
        wchar_t *src = u2w(argv[k]);
        wchar_t target[PATHBUF];
        if (dst_dir) path_join_base(target, PATHBUF, dst, src);
        else { wcsncpy(target, dst, PATHBUF - 1); target[PATHBUF - 1] = 0; }
        if (!MoveFileExW(src, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            report(err, "mv", argv[k], GetLastError());
            rc = 1;
        }
        free(src);
    }
    free(dst);
    return rc;
}

static int rm_item(HANDLE err, const wchar_t *path, int rec, int force)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (force && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) return 0;
        report_w(err, "rm", path, e);
        return 1;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!rec) {
            char *u = w2u(path);
            out_printf(err, "rm: cannot remove '%s': Is a directory\n", u);
            free(u);
            return 1;
        }
        int rc = 0;
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {   /* never descend into junctions */
            wchar_t pat[PATHBUF];
            dir_pattern(pat, PATHBUF, path);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    wchar_t child[PATHBUF];
                    path_join(child, PATHBUF, path, fd.cFileName);
                    if (rm_item(err, child, rec, force)) rc = 1;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (!RemoveDirectoryW(path)) { report_w(err, "rm", path, GetLastError()); rc = 1; }
        return rc;
    }
    if (!DeleteFileW(path)) {
        DWORD e = GetLastError();
        if (force && e == ERROR_ACCESS_DENIED && (a & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);      /* -f clears read-only */
            if (DeleteFileW(path)) return 0;
            e = GetLastError();
        }
        report_w(err, "rm", path, e);
        return 1;
    }
    return 0;
}

static int bi_rm(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int rec = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') rec = 1;
            else if (*p == 'f') force = 1;
            else { out_printf(err, "rm: invalid option -- '%c'\n", *p); return 1; }
        }
    }
    if (i >= argc) {
        if (force) return 0;
        out_puts(err, "rm: missing operand\n");
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        wchar_t *w = u2w(argv[i]);
        wchar_t full[PATHBUF];
        if (rec && GetFullPathNameW(w, PATHBUF, full, NULL) && wcslen(full) <= 3) {
            out_printf(err, "rm: it is dangerous to operate recursively on '%s'\n", argv[i]);
            rc = 1;                                  /* refuse "C:\" */
        } else if (rm_item(err, w, rec, force)) rc = 1;
        free(w);
    }
    return rc;
}

static int bi_mkdir(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    int parents = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'p') parents = 1;
            else { out_printf(err, "mkdir: invalid option -- '%c'\n", *p); return 1; }
        }
    }
    if (i >= argc) { out_puts(err, "mkdir: missing operand\n"); return 1; }

    int rc = 0;
    for (; i < argc; i++) {
        wchar_t *w = u2w(argv[i]);
        if (parents) {                               /* create a, a\b, a\b\c in turn */
            for (wchar_t *p = w; *p; p++) {
                if ((*p == L'\\' || *p == L'/') && p > w &&
                    p[-1] != L':' && p[-1] != L'\\' && p[-1] != L'/') {
                    wchar_t c = *p; *p = 0;
                    CreateDirectoryW(w, NULL);       /* failures surface on the final call */
                    *p = c;
                }
            }
        }
        if (!CreateDirectoryW(w, NULL)) {
            DWORD e = GetLastError();
            if (!(parents && e == ERROR_ALREADY_EXISTS && is_dir_w(w))) {
                char tmp[256];
                out_printf(err, "mkdir: cannot create directory '%s': %s\n",
                           argv[i], err_text(e, tmp, sizeof tmp));
                rc = 1;
            }
        }
        free(w);
    }
    return rc;
}

static int bi_touch(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err)
{
    (void)in; (void)out;
    if (argc < 2) { out_puts(err, "touch: missing file operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        wchar_t *w = u2w(argv[i]);
        /* OPEN_ALWAYS = create if missing, open if present; BACKUP_SEMANTICS
         * lets the same call work on directories. */
        HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            report(err, "touch", argv[i], GetLastError());
            rc = 1;
        } else {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            SetFileTime(h, NULL, &ft, &ft);          /* access + modified = now */
            CloseHandle(h);
        }
        free(w);
    }
    return rc;
}

/* ======================================================================== *
 *  Lookup table  (contract: find_builtin)                                  *
 * ======================================================================== */
typedef struct { const char *name; BuiltinFn fn; } BuiltinEntry;

static const BuiltinEntry g_builtins[] = {
    { "cd", bi_cd },       { "pwd", bi_pwd },     { "exit", bi_exit },
    { "echo", bi_echo },   { "export", bi_export }, { "unset", bi_unset },
    { "env", bi_env },     { "clear", bi_clear }, { "history", bi_history },
    { "jobs", bi_jobs },   { "fg", bi_fg },       { "bg", bi_bg },
    { "which", bi_which }, { "type", bi_type },   { "help", bi_help },
    { "ls", bi_ls },       { "cat", bi_cat },     { "cp", bi_cp },
    { "mv", bi_mv },       { "rm", bi_rm },       { "mkdir", bi_mkdir },
    { "touch", bi_touch }, { "head", bi_head },   { "tail", bi_tail },
    { "wc", bi_wc },       { "grep", bi_grep },
    /* implemented in sudo.c */
    { "sudo", builtin_sudo }, { "su", builtin_su }, { "chmod", builtin_chmod },
};

BuiltinFn find_builtin(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_builtins / sizeof *g_builtins; i++)
        if (strcmp(g_builtins[i].name, name) == 0)   /* case-sensitive, like bash */
            return g_builtins[i].fn;
    return NULL;
}
