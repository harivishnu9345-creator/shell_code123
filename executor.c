/*
 * executor.c  --  hsh executor  (owner: executor/jobs module)
 *
 * Runs a parsed Pipeline on native Windows (no fork, no WSL).
 *
 * ---------------------------------------------------------------------------
 *  VIVA CHEAT-SHEET: how the Unix process model maps onto Win32
 * ---------------------------------------------------------------------------
 *  Unix                         Win32 used here                  Why
 *  ---------------------------  -------------------------------  --------------
 *  fork() + execve()            CreateProcessW                   Windows has no
 *                                                                fork; one call
 *                                                                creates a NEW
 *                                                                process that
 *                                                                runs a program.
 *  pipe()                       CreatePipe                       Anonymous pipe:
 *                                                                read + write
 *                                                                HANDLE.
 *  open(file, O_RDONLY/...)     CreateFileW                      Redirect files;
 *                               (FILE_APPEND_DATA for >>)        O_APPEND == the
 *                                                                FILE_APPEND_DATA
 *                                                                access right.
 *  dup2(fd, 0/1/2)              STARTUPINFO.hStdInput/Output/    We never edit our
 *                               Error + STARTF_USESTDHANDLES     own fds; we tell
 *                                                                the child which
 *                                                                handles are its
 *                                                                stdin/out/err.
 *  fd inheritance / close-      PROC_THREAD_ATTRIBUTE_HANDLE_    Child inherits
 *  on-exec                      LIST (STARTUPINFOEX)             ONLY listed
 *                                                                handles, so no
 *                                                                stray pipe ends
 *                                                                leak -> no hangs.
 *  waitpid()                    WaitForSingleObject(hProcess)    A process handle
 *                               + GetExitCodeProcess             is "signalled"
 *                                                                when it exits.
 *  SIGINT to foreground group   SetConsoleCtrlHandler            Shell ignores
 *                                                                Ctrl+C, children
 *                                                                get default.
 *  setpgid() for bg jobs        CREATE_NEW_PROCESS_GROUP         Background jobs
 *                                                                are immune to
 *                                                                the console's
 *                                                                Ctrl+C.
 *  execvp() PATH search         manual PATH+PATHEXT walk, then   Windows needs
 *                               HKCU/HKLM "App Paths", then      extensions and
 *                               ShellExecuteExW                  has App Paths.
 * ---------------------------------------------------------------------------
 *
 *  Build notes: link advapi32, shell32, ole32 (MSVC picks them up through the
 *  #pragma comment lines below; with MinGW add them to target_link_libraries).
 *
 *  Security: this file never uses the "runas" verb, never touches credentials
 *  and never tries to bypass UAC.  If a program needs elevation, Windows itself
 *  shows the normal consent prompt (via ShellExecuteExW "open"), or we print a
 *  hint.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00          /* Windows 10: STARTUPINFOEX, handle lists */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>                 /* CoInitializeEx (needed by ShellExecuteEx) */
#include <shellapi.h>
#include <process.h>                 /* _beginthreadex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "shell.h"

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

#define PATH_BUF        4096         /* wide chars for path buffers            */
#define PIPE_BUFSZ      (64 * 1024)  /* pipe buffer, bigger than the 4 KB default */
#define EXIT_CANNOT_EXEC 126         /* bash convention: found, not executable */
#define EXIT_NOT_FOUND   127         /* bash convention: command not found     */
#define EXIT_CTRL_C      130         /* bash convention: 128 + SIGINT          */
#define STATUS_CTRL_C    0xC000013AUL/* NTSTATUS of a process killed by Ctrl+C */

/* ---------------------------------------------------------------------------
 *  Ctrl+C handling (shared with jobs.c)
 * ------------------------------------------------------------------------- */

/* Incremented every time the user presses Ctrl+C / Ctrl+Break. jobs.c reads it
 * while a job is brought to the foreground (fg). */
volatile LONG hsh_ctrl_c_hits = 0;

/*
 * Console control handler.  Windows runs it on a separate thread.
 * Pressing Ctrl+C sends CTRL_C_EVENT to EVERY process attached to the console
 * (shell + children).  Returning TRUE means "handled": the default action
 * (ExitProcess) is skipped, so the SHELL survives.  Children do not install a
 * handler, so their default action still terminates them -> only the
 * foreground child dies.  Background jobs are started with
 * CREATE_NEW_PROCESS_GROUP, which disables Ctrl+C for them.
 */
static BOOL WINAPI ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        InterlockedIncrement(&hsh_ctrl_c_hits);
        return TRUE;                 /* swallow: shell keeps running */
    default:
        return FALSE;                /* close/logoff/shutdown: let Windows act */
    }
}

/* Safe to call many times; main() may call it too, otherwise we do it lazily. */
void executor_init(void)
{
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) == 0)
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

/* ---------------------------------------------------------------------------
 *  UTF-8 <-> UTF-16 helpers (shell is UTF-8 inside, Win32 "W" APIs outside)
 * ------------------------------------------------------------------------- */

static wchar_t *u8_to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int w_to_u8(const wchar_t *w, char *out, size_t n)
{
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    return r > 0 ? 0 : -1;
}

/* Print "hsh: <what>: <Windows error text>" to stderr. */
static void report_win_error(const char *what, DWORD err)
{
    wchar_t msg[512];
    char    u8[1024] = "";
    DWORD   n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, 0, msg, 512, NULL);
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' ||
                     msg[n - 1] == L' '  || msg[n - 1] == L'.'))
        msg[--n] = 0;
    if (n == 0 || w_to_u8(msg, u8, sizeof u8) != 0)
        snprintf(u8, sizeof u8, "error %lu", (unsigned long)err);
    fprintf(stderr, "hsh: %s: %s\n", what, u8);
}

/* ---------------------------------------------------------------------------
 *  Tiny growable wide-string buffer + Windows command-line quoting
 * ------------------------------------------------------------------------- */

typedef struct { wchar_t *p; size_t len, cap; int oom; } WBuf;

static void wb_put(WBuf *b, const wchar_t *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        wchar_t *np = (wchar_t *)realloc(b->p, nc * sizeof(wchar_t));
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->p[b->len] = 0;
}
static void wb_putc(WBuf *b, wchar_t c) { wb_put(b, &c, 1); }

/*
 * Windows has ONE command-line string (not an argv array), and the child's C
 * runtime splits it again.  So we must quote each argument using the rules of
 * CommandLineToArgvW: wrap in "..." if it has space/tab/quote, double the
 * backslashes that precede a quote, and escape the quote with a backslash.
 */
static void wb_arg(WBuf *b, const wchar_t *a)
{
    if (*a && !wcspbrk(a, L" \t\n\v\"")) { wb_put(b, a, wcslen(a)); return; }
    wb_putc(b, L'"');
    for (;;) {
        size_t bs = 0, i;
        while (*a == L'\\') { a++; bs++; }
        if (*a == 0) {                       /* backslashes before closing quote */
            for (i = 0; i < bs * 2; i++) wb_putc(b, L'\\');
            break;
        } else if (*a == L'"') {             /* backslashes before a literal quote */
            for (i = 0; i < bs * 2 + 1; i++) wb_putc(b, L'\\');
            wb_putc(b, L'"');
        } else {                             /* ordinary: backslashes are literal */
            for (i = 0; i < bs; i++) wb_putc(b, L'\\');
            wb_putc(b, *a);
        }
        a++;
    }
    wb_putc(b, L'"');
}

/* Join argv[first..argc-1] into one quoted wide command line (caller frees). */
static wchar_t *build_cmdline(int argc, char **argv, int first)
{
    WBuf b = {0};
    for (int i = first; i < argc; i++) {
        wchar_t *w = u8_to_w(argv[i]);
        if (!w) { b.oom = 1; break; }
        if (i > first) wb_putc(&b, L' ');
        wb_arg(&b, w);
        free(w);
    }
    if (b.oom) { free(b.p); return NULL; }
    if (!b.p) { b.p = (wchar_t *)calloc(1, sizeof(wchar_t)); }   /* empty string */
    return b.p;
}

/* ---------------------------------------------------------------------------
 *  resolve_program():  builtin > PATH+PATHEXT > registry App Paths
 *  (the ShellExecuteEx fallback is done at launch time, see below)
 * ------------------------------------------------------------------------- */

/* Regular file (not a directory)?  App-Execution-Aliases such as the
 * Microsoft Store "python.exe" are reparse points that sometimes answer
 * ERROR_CANT_ACCESS_FILE; they are still launchable, so accept them. */
static BOOL is_file(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_CANT_ACCESS_FILE;
    return !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *get_env_w(const wchar_t *name)
{
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    if (!n) return NULL;
    wchar_t *b = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (b) GetEnvironmentVariableW(name, b, n);
    return b;
}

static BOOL has_extension(const wchar_t *p)
{
    const wchar_t *dot = wcsrchr(p, L'.');
    const wchar_t *sep = wcsrchr(p, L'\\');
    return dot && (!sep || dot > sep) && dot[1] != 0;
}

/* Try "<base>" (if it already has an extension) and "<base><ext>" for every
 * extension in PATHEXT (".COM;.EXE;.BAT;.CMD;..."). On success fills out. */
static BOOL probe(const wchar_t *base, const wchar_t *pathext, wchar_t *out, size_t outn)
{
    size_t blen = wcslen(base);
    if (has_extension(base) && blen < outn && is_file(base)) {
        memcpy(out, base, (blen + 1) * sizeof(wchar_t));
        return TRUE;
    }
    for (const wchar_t *p = pathext; *p; ) {
        const wchar_t *semi = wcschr(p, L';');
        size_t elen = semi ? (size_t)(semi - p) : wcslen(p);
        if (elen > 0 && blen + elen < outn) {
            memcpy(out, base, blen * sizeof(wchar_t));
            memcpy(out + blen, p, elen * sizeof(wchar_t));
            out[blen + elen] = 0;
            if (is_file(out)) return TRUE;
        }
        if (!semi) break;
        p = semi + 1;
    }
    return FALSE;
}

/* Walk %PATH% directory by directory (like execvp).  Like bash - and unlike
 * cmd.exe - the current directory is NOT searched; use ./prog for that. */
static BOOL search_path(const wchar_t *wname, const wchar_t *pathext,
                        wchar_t *out, size_t outn)
{
    wchar_t *path = get_env_w(L"PATH");
    if (!path) return FALSE;
    size_t nlen = wcslen(wname);
    BOOL found = FALSE;
    wchar_t base[PATH_BUF];

    for (wchar_t *p = path; p && *p && !found; ) {
        wchar_t *semi = wcschr(p, L';');
        const wchar_t *d = p;
        size_t l = semi ? (size_t)(semi - p) : wcslen(p);
        p = semi ? semi + 1 : NULL;

        if (l >= 2 && d[0] == L'"' && d[l - 1] == L'"') { d++; l -= 2; }   /* "C:\a b" */
        while (l > 0 && d[l - 1] == L'\\') l--;                          /* trailing \ */
        if (l == 0 || l + 1 + nlen + 1 >= PATH_BUF) continue;

        memcpy(base, d, l * sizeof(wchar_t));
        base[l] = L'\\';
        memcpy(base + l + 1, wname, (nlen + 1) * sizeof(wchar_t));
        found = probe(base, pathext, out, outn);
    }
    free(path);
    return found;
}

/*
 * "App Paths": installers register e.g. wireshark.exe under
 *   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\wireshark.exe
 * with the full path as the key's default value.  This is how the Win+R box
 * finds programs that are not on PATH.  Windows consults HKCU first, then HKLM;
 * we also try both the 64-bit and 32-bit registry views.
 */
static BOOL search_app_paths(const wchar_t *wname, wchar_t *out, size_t outn)
{
    static const wchar_t prefix[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
    wchar_t subkey[512];
    size_t plen = wcslen(prefix), nlen = wcslen(wname);
    BOOL   has_exe = nlen > 4 && _wcsicmp(wname + nlen - 4, L".exe") == 0;
    if (plen + nlen + 5 >= 512) return FALSE;

    wcscpy(subkey, prefix);
    wcscat(subkey, wname);
    if (!has_exe) wcscat(subkey, L".exe");

    const HKEY  hives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    for (int h = 0; h < 2; h++)
        for (int v = 0; v < 2; v++) {
            HKEY hk;
            if (RegOpenKeyExW(hives[h], subkey, 0, KEY_READ | views[v], &hk) != ERROR_SUCCESS)
                continue;
            wchar_t val[PATH_BUF];
            DWORD   sz = sizeof val;
            /* NULL value name = the key's (Default) value; REG_EXPAND_SZ is
             * expanded for us (e.g. %ProgramFiles%). */
            LSTATUS rc = RegGetValueW(hk, NULL, NULL,
                                      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                                      NULL, val, &sz);
            RegCloseKey(hk);
            if (rc != ERROR_SUCCESS) continue;

            wchar_t *s = val;
            size_t l = wcslen(s);
            if (l >= 2 && s[0] == L'"' && s[l - 1] == L'"') { s[l - 1] = 0; s++; }  /* strip quotes */
            if (is_file(s) && wcslen(s) < outn) { wcscpy(out, s); return TRUE; }
        }
    return FALSE;
}

/* Wide-character core of resolve_program(). Returns 0 and a full path in out. */
static int resolve_program_w(const wchar_t *wname, wchar_t *out, size_t outn)
{
    wchar_t nm[PATH_BUF];
    size_t  nlen = wcslen(wname);
    if (nlen == 0 || nlen >= PATH_BUF) return -1;
    for (size_t i = 0; i <= nlen; i++)                       /* accept ./prog style */
        nm[i] = (wname[i] == L'/') ? L'\\' : wname[i];

    wchar_t *pathext = get_env_w(L"PATHEXT");
    const wchar_t *pe = pathext ? pathext : L".COM;.EXE;.BAT;.CMD";
    BOOL found;

    if (wcschr(nm, L'\\') || wcschr(nm, L':')) {
        /* The user typed a path (relative or absolute): do not search PATH. */
        found = probe(nm, pe, out, outn);
    } else {
        found = search_path(nm, pe, out, outn) || search_app_paths(nm, out, outn);
    }
    free(pathext);
    return found ? 0 : -1;
}

/* Public contract: builtin > PATH+PATHEXT > App Paths.  0 = found.
 * For a builtin, out_path is simply the builtin's name. */
int resolve_program(const char *name, char *out_path, size_t n)
{
    if (!name || !*name || n == 0) return -1;

    if (find_builtin(name)) {
        if (strlen(name) + 1 > n) return -1;
        strcpy(out_path, name);
        return 0;
    }
    wchar_t *wname = u8_to_w(name);
    if (!wname) return -1;
    wchar_t full[PATH_BUF];
    int rc = resolve_program_w(wname, full, PATH_BUF);
    free(wname);
    if (rc != 0) return -1;
    return w_to_u8(full, out_path, n);
}

/* ---------------------------------------------------------------------------
 *  Handles: the three standard streams of one pipeline stage
 * ------------------------------------------------------------------------- */

typedef struct {
    HANDLE in, out, err;
    BOOL   own_in, own_out, own_err;   /* TRUE = we must CloseHandle() it       */
} StdSet;

static void close_owned(HANDLE *h, BOOL *own)
{
    if (*own && *h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
    *h = NULL; *own = FALSE;
}
static void release(StdSet *s)
{
    close_owned(&s->in,  &s->own_in);
    close_owned(&s->out, &s->own_out);
    close_owned(&s->err, &s->own_err);
}

/* bInheritHandle = TRUE makes the new handle inheritable (needed for children). */
static SECURITY_ATTRIBUTES inherit_sa(void)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    return sa;
}

/* "NUL" device: the Windows /dev/null. Used when the shell has no usable std handle. */
static HANDLE open_nul(BOOL for_read)
{
    SECURITY_ATTRIBUTES sa = inherit_sa();
    return CreateFileW(L"NUL", for_read ? GENERIC_READ : GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Shell's own std handle, or NUL if the shell was started without one. */
static HANDLE shell_std(DWORD which, BOOL *own)
{
    HANDLE h = GetStdHandle(which);
    *own = FALSE;
    if (!h || h == INVALID_HANDLE_VALUE) {
        h = open_nul(which == STD_INPUT_HANDLE);
        *own = TRUE;
    }
    return h;
}

/* `< file`  ==  open(file, O_RDONLY) */
static HANDLE open_input(const char *path)
{
    wchar_t *w = u8_to_w(path);
    if (!w) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = inherit_sa();
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) report_win_error(path, GetLastError());
    free(w);
    return h;
}

/*
 * `> file`  ==  open(file, O_WRONLY|O_CREAT|O_TRUNC)   -> CREATE_ALWAYS
 * `>> file` ==  open(file, O_WRONLY|O_CREAT|O_APPEND)  -> OPEN_ALWAYS + only
 *               FILE_APPEND_DATA access, so every write lands at end-of-file
 *               (this is exactly what O_APPEND means).
 */
static HANDLE open_output(const char *path, int append)
{
    wchar_t *w = u8_to_w(path);
    if (!w) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = inherit_sa();
    HANDLE h = CreateFileW(w,
                           append ? FILE_APPEND_DATA : GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) report_win_error(path, GetLastError());
    free(w);
    return h;
}

/* ---------------------------------------------------------------------------
 *  Launching one stage
 * ------------------------------------------------------------------------- */

typedef struct {
    HANDLE proc;       /* process handle (NULL for thread/inline/failed stage)   */
    DWORD  pid;
    HANDLE thread;     /* thread handle for a builtin running inside a pipeline  */
    int    rc;         /* exit code when already known (inline/failed)           */
} Launch;

/*
 * CreateProcessW wrapper that wires the three std handles into the child.
 *
 * Classic problem: with bInheritHandles=TRUE the child inherits EVERY
 * inheritable handle of the shell, including the other pipe ends.  A child
 * that holds a stray write end of a pipe keeps it open, so the reader never
 * sees EOF -> the pipeline hangs.  The fix is a PROC_THREAD_ATTRIBUTE_HANDLE_LIST:
 * the child inherits exactly the (at most 3) handles listed there.
 */
static BOOL do_create_process(const wchar_t *exe, wchar_t *cmdline, const StdSet *s,
                              BOOL background, PROCESS_INFORMATION *pi)
{
    /* unique handles only: duplicates make UpdateProcThreadAttribute fail */
    HANDLE hl[3];
    DWORD  cnt = 0;
    HANDLE all[3] = { s->in, s->out, s->err };
    for (int i = 0; i < 3; i++) {
        BOOL dup = FALSE;
        for (DWORD j = 0; j < cnt; j++) if (hl[j] == all[i]) dup = TRUE;
        if (!dup) {
            hl[cnt++] = all[i];
            /* handles in the list must be inheritable (std handles may not be) */
            SetHandleInformation(all[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb         = sizeof si;
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;   /* honour the 3 handles below */
    si.StartupInfo.hStdInput  = s->in;                  /* this is our dup2(.., 0)    */
    si.StartupInfo.hStdOutput = s->out;                 /*              dup2(.., 1)   */
    si.StartupInfo.hStdError  = s->err;                 /*              dup2(.., 2)   */

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);   /* query needed size */
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, size);
    if (!si.lpAttributeList) { SetLastError(ERROR_OUTOFMEMORY); return FALSE; }

    BOOL ok = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
              UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        hl, cnt * sizeof(HANDLE), NULL, NULL);
    DWORD err = 0;
    if (ok) {
        /* Background jobs get their own process group: that disables Ctrl+C for
         * them, so the user's Ctrl+C at the prompt cannot kill them. */
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (background ? CREATE_NEW_PROCESS_GROUP : 0);
        ZeroMemory(pi, sizeof *pi);
        ok  = CreateProcessW(exe,        /* lpApplicationName: the exact file to run */
                             cmdline,    /* lpCommandLine:     argv[0] args...       */
                             NULL, NULL, /* process/thread security attributes       */
                             TRUE,       /* bInheritHandles (restricted by the list) */
                             flags,
                             NULL,       /* environment: inherit ours                */
                             NULL,       /* current dir: inherit ours (so `cd` works) */
                             &si.StartupInfo, pi);
    }
    err = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    SetLastError(err);
    return ok;
}

/*
 * Fallback for things CreateProcess cannot start: documents/URLs, GUI apps that
 * only exist as a file association, App-Paths-less shortcuts, scripts such as
 * .py/.ps1, or programs whose manifest demands elevation (error 740).
 * ShellExecuteExW does what the Win+R box does.  It cannot redirect handles, so
 * the caller only uses it for a lone command without redirection.
 * With the "open" verb Windows itself shows the normal UAC consent dialog when
 * needed - we never use "runas" and never handle credentials.
 */
static int shell_execute_fallback(const Command *c, Launch *L)
{
    static BOOL com_ready = FALSE;
    if (!com_ready) {   /* ShellExecute wants COM initialised on the calling thread */
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        com_ready = TRUE;
    }
    wchar_t *wfile = u8_to_w(c->argv[0]);
    wchar_t *wpar  = build_cmdline(c->argc, c->argv, 1);   /* args only */
    if (!wfile || !wpar) { free(wfile); free(wpar); return EXIT_CANNOT_EXEC; }

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize       = sizeof sei;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; /* want hProcess, no error boxes */
    sei.lpVerb       = L"open";
    sei.lpFile       = wfile;
    sei.lpParameters = wpar[0] ? wpar : NULL;
    sei.nShow        = SW_SHOWNORMAL;

    int rc = 0;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_NO_ASSOCIATION) {
            fprintf(stderr, "hsh: %s: command not found\n", c->argv[0]);
            rc = EXIT_NOT_FOUND;
        } else {
            report_win_error(c->argv[0], e);
            rc = EXIT_CANNOT_EXEC;
        }
    } else if (sei.hProcess) {          /* may be NULL (e.g. document opened in a running app) */
        L->proc = sei.hProcess;
        L->pid  = GetProcessId(sei.hProcess);
    }
    free(wfile);
    free(wpar);
    return rc;
}

/* Start an external program for one stage. Returns 0 on success, else a shell exit code. */
static int launch_process(const Command *c, const StdSet *s, BOOL background,
                          BOOL allow_shexec, Launch *L)
{
    wchar_t *wname = u8_to_w(c->argv[0]);
    if (!wname) return EXIT_CANNOT_EXEC;

    wchar_t exe[PATH_BUF];
    if (resolve_program_w(wname, exe, PATH_BUF) != 0) {
        free(wname);
        if (allow_shexec) return shell_execute_fallback(c, L);
        fprintf(stderr, "hsh: %s: command not found\n", c->argv[0]);
        return EXIT_NOT_FOUND;
    }
    free(wname);

    /* argv[0] stays as the user typed it; lpApplicationName holds the real file.
     * (.bat/.cmd files are run through cmd.exe by CreateProcess itself.) */
    wchar_t *cmdline = build_cmdline(c->argc, c->argv, 0);
    if (!cmdline) return EXIT_CANNOT_EXEC;

    PROCESS_INFORMATION pi;
    int rc = 0;
    if (do_create_process(exe, cmdline, s, background, &pi)) {
        CloseHandle(pi.hThread);          /* we only need the process handle */
        L->proc = pi.hProcess;
        L->pid  = pi.dwProcessId;
    } else {
        DWORD e = GetLastError();
        if ((e == ERROR_ELEVATION_REQUIRED || e == ERROR_BAD_EXE_FORMAT) && allow_shexec) {
            rc = shell_execute_fallback(c, L);   /* Windows shows the UAC prompt itself */
        } else if (e == ERROR_ELEVATION_REQUIRED) {
            fprintf(stderr, "hsh: %s: requires administrator rights "
                            "(start hsh from an elevated terminal)\n", c->argv[0]);
            rc = EXIT_CANNOT_EXEC;
        } else {
            report_win_error(c->argv[0], e);
            rc = EXIT_CANNOT_EXEC;
        }
    }
    free(cmdline);
    return rc;
}

/* ---------------------------------------------------------------------------
 *  Builtins inside a pipeline run on a thread
 * ------------------------------------------------------------------------- */

typedef struct {
    BuiltinFn fn;
    int       argc;
    char    **argv;               /* private deep copy: the Pipeline may be freed */
    StdSet    s;                  /* the thread owns (and closes) these handles   */
} BuiltinTask;

static char **dup_argv(int argc, char **argv)
{
    char **v = (char **)calloc((size_t)argc + 1, sizeof *v);
    if (!v) return NULL;
    for (int i = 0; i < argc; i++) {
        v[i] = _strdup(argv[i]);
        if (!v[i]) { while (i--) free(v[i]); free(v); return NULL; }
    }
    return v;
}

static unsigned __stdcall builtin_thread(void *arg)
{
    BuiltinTask *t = (BuiltinTask *)arg;
    int rc = t->fn(t->argc, t->argv, t->s.in, t->s.out, t->s.err);
    /* Closing our pipe write end is what lets the next stage see EOF. */
    release(&t->s);
    for (int i = 0; i < t->argc; i++) free(t->argv[i]);
    free(t->argv);
    free(t);
    return (unsigned)rc;
}

/* On success the thread takes over the handles (s's "own" flags are cleared). */
static int launch_builtin_thread(BuiltinFn fn, const Command *c, StdSet *s, Launch *L)
{
    BuiltinTask *t = (BuiltinTask *)calloc(1, sizeof *t);
    if (!t) return EXIT_CANNOT_EXEC;
    t->fn   = fn;
    t->argc = c->argc;
    t->argv = dup_argv(c->argc, c->argv);
    if (!t->argv) { free(t); return EXIT_CANNOT_EXEC; }
    t->s = *s;

    uintptr_t th = _beginthreadex(NULL, 0, builtin_thread, t, 0, NULL);
    if (!th) {
        for (int i = 0; i < t->argc; i++) free(t->argv[i]);
        free(t->argv);
        free(t);
        return EXIT_CANNOT_EXEC;
    }
    L->thread = (HANDLE)th;
    s->own_in = s->own_out = s->own_err = FALSE;   /* ownership moved to the thread */
    return 0;
}

/* ---------------------------------------------------------------------------
 *  execute_pipeline()
 * ------------------------------------------------------------------------- */

static int map_exit(DWORD code)
{
    return code == STATUS_CTRL_C ? EXIT_CTRL_C : (int)code;
}

int execute_pipeline(Pipeline *p, const char *raw_line)
{
    executor_init();
    if (!p || p->ncmds <= 0) return 0;

    const int n = p->ncmds;
    Launch *L = (Launch *)calloc((size_t)n, sizeof *L);
    if (!L) { fprintf(stderr, "hsh: out of memory\n"); return 1; }

    /* A child may leave the console in raw mode (editors, REPLs). Save the
     * input mode now and restore it after a foreground run. */
    HANDLE hcon = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  saved_mode = 0;
    BOOL   have_mode = (hcon && hcon != INVALID_HANDLE_VALUE) && GetConsoleMode(hcon, &saved_mode);

    /* ShellExecute fallback only makes sense for a lone, unredirected command. */
    const BOOL lone = (n == 1) && !p->cmds[0].in_file && !p->cmds[0].out_file &&
                      !p->cmds[0].err_file && !p->cmds[0].err_to_out;

    HANDLE prev_read = NULL;   /* read end of the pipe coming from the previous stage */

    for (int i = 0; i < n; i++) {
        Command *c = &p->cmds[i];
        StdSet   s;  ZeroMemory(&s, sizeof s);
        BOOL     ok = TRUE;
        HANDLE   pipe_r = NULL, pipe_w = NULL;

        /* ---- stdin: file > previous pipe > NUL (bg, first stage) > shell stdin ---- */
        if (c->in_file) {
            s.in = open_input(c->in_file);
            s.own_in = TRUE;
            if (s.in == INVALID_HANDLE_VALUE) { ok = FALSE; s.in = NULL; s.own_in = FALSE; }
            if (prev_read) { CloseHandle(prev_read); prev_read = NULL; } /* upstream gets a broken pipe */
        } else if (prev_read) {
            s.in = prev_read; s.own_in = TRUE; prev_read = NULL;
        } else if (i == 0 && p->background) {
            /* A background job must not steal keystrokes from the prompt. */
            s.in = open_nul(TRUE); s.own_in = TRUE;
        } else {
            s.in = shell_std(STD_INPUT_HANDLE, &s.own_in);
        }

        /* ---- stdout: file > next pipe > shell stdout ---- */
        if (i < n - 1) {
            SECURITY_ATTRIBUTES sa = inherit_sa();
            if (!CreatePipe(&pipe_r, &pipe_w, &sa, PIPE_BUFSZ)) {
                report_win_error("pipe", GetLastError());
                pipe_r = pipe_w = NULL;
                ok = FALSE;
            }
        }
        if (c->out_file) {
            s.out = open_output(c->out_file, c->append);
            s.own_out = TRUE;
            if (s.out == INVALID_HANDLE_VALUE) { ok = FALSE; s.out = NULL; s.own_out = FALSE; }
            if (pipe_w) { CloseHandle(pipe_w); pipe_w = NULL; }   /* next stage just sees EOF */
        } else if (pipe_w) {
            s.out = pipe_w; s.own_out = TRUE;
        } else {
            s.out = shell_std(STD_OUTPUT_HANDLE, &s.own_out);
        }
        prev_read = pipe_r;                       /* feeds stage i+1 */

        /* ---- stderr: 2>&1 (share stdout) > 2> file > shell stderr ---- */
        if (c->err_to_out) {
            s.err = s.out; s.own_err = FALSE;     /* same handle, closed once via own_out */
        } else if (c->err_file) {
            s.err = open_output(c->err_file, 0);
            s.own_err = TRUE;
            if (s.err == INVALID_HANDLE_VALUE) { ok = FALSE; s.err = NULL; s.own_err = FALSE; }
        } else {
            s.err = shell_std(STD_ERROR_HANDLE, &s.own_err);
        }

        /* ---- launch ---- */
        if (!ok || !s.in || !s.out || !s.err) {
            L[i].rc = 1;                          /* redirect/pipe failed; message already printed */
            release(&s);
            continue;
        }
        if (c->argc <= 0 || !c->argv || !c->argv[0]) {   /* e.g. just "> file": file was created */
            L[i].rc = 0;
            release(&s);
            continue;
        }

        BuiltinFn bf = find_builtin(c->argv[0]);
        if (bf && n == 1) {
            /* Lone builtin: run IN-PROCESS so `cd`, `export`, `exit` change the
             * shell itself (a child process could not). Handles are passed in. */
            L[i].rc = bf(c->argc, c->argv, s.in, s.out, s.err);
            release(&s);
        } else if (bf) {
            int rc = launch_builtin_thread(bf, c, &s, &L[i]);
            if (rc) L[i].rc = rc;
            release(&s);                          /* no-op if the thread took them */
        } else {
            int rc = launch_process(c, &s, (BOOL)p->background, lone, &L[i]);
            if (rc) L[i].rc = rc;
            /* CRUCIAL: close OUR copies of the pipe ends right away. The child
             * has its own inherited copies; if we kept ours the reader would
             * never get EOF and the pipeline would hang. */
            release(&s);
        }
    }
    if (prev_read) CloseHandle(prev_read);        /* defensive; normally NULL here */

    int exit_code = 0;

    if (!p->background) {
        /* Foreground: wait for every stage (like bash) and report the LAST one's status.
         * WaitForSingleObject on a process handle == waitpid(). */
        for (int i = 0; i < n; i++) {
            DWORD code = 0;
            if (L[i].thread) {
                WaitForSingleObject(L[i].thread, INFINITE);
                GetExitCodeThread(L[i].thread, &code);
                CloseHandle(L[i].thread);
                L[i].rc = (int)code;
            } else if (L[i].proc) {
                WaitForSingleObject(L[i].proc, INFINITE);
                GetExitCodeProcess(L[i].proc, &code);
                CloseHandle(L[i].proc);
                L[i].rc = map_exit(code);
            }
        }
        exit_code = L[n - 1].rc;
        if (have_mode) SetConsoleMode(hcon, saved_mode);
    } else {
        /* Background: don't wait. Earlier stages just keep running; the LAST
         * stage's process goes into the job table and prints "[1] 4321". */
        for (int i = 0; i < n; i++) {
            if (i == n - 1 && L[i].proc) {
                jobs_add(L[i].proc, L[i].pid, raw_line ? raw_line : "");  /* jobs.c owns the handle now */
                L[i].proc = NULL;
            }
            if (L[i].proc)   CloseHandle(L[i].proc);
            if (L[i].thread) CloseHandle(L[i].thread);   /* thread keeps running, frees itself */
        }
        exit_code = 0;
    }

    free(L);
    return exit_code;
}
