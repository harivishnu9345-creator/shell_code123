/*
 * jobs.c  --  hsh background-job table  (owner: executor/jobs module)
 *
 * ---------------------------------------------------------------------------
 *  VIVA CHEAT-SHEET
 * ---------------------------------------------------------------------------
 *  Unix job control             Win32 used here
 *  ---------------------------  ----------------------------------------------
 *  SIGCHLD / waitpid(WNOHANG)   RegisterWaitForSingleObject(hProcess): the
 *                               thread pool calls our callback the moment the
 *                               process exits. The callback only MARKS the job
 *                               done; jobs_reap() prints "Done" before the next
 *                               prompt (printing from the callback would mix
 *                               with what the user is typing).
 *  process group + killpg()     Job Object: the process (and every child it
 *                               spawns later) belongs to one kernel object, so
 *                               TerminateJobObject() kills the whole tree.
 *  fg  (wait + SIGINT)          WaitForSingleObject loop; Ctrl+C is turned into
 *                               CTRL_BREAK_EVENT for the job's process group.
 *  bg  (SIGCONT)                Windows cannot suspend a console process the way
 *                               SIGTSTP/SIGSTOP does, so every job is ALWAYS
 *                               running; bg_job() just confirms that.
 * ---------------------------------------------------------------------------
 *
 *  Ownership: jobs_add() TAKES OWNERSHIP of the process HANDLE it is given
 *  (the executor must not CloseHandle it afterwards).
 *
 *  Locking: all table access is under one CRITICAL_SECTION because the wait
 *  callbacks run on thread-pool threads.  UnregisterWaitEx(..., INVALID_HANDLE_VALUE)
 *  is always called WITHOUT holding the lock (it waits for a running callback,
 *  and the callback needs the lock -> that would deadlock).
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

#define MAX_JOBS 64

/* Defined in executor.c: counts Ctrl+C presses (see ctrl_handler there). */
extern volatile LONG hsh_ctrl_c_hits;

typedef struct {
    int           in_use;
    int           id;          /* the number in "[1]"                          */
    unsigned long seq;         /* creation order: highest = current job ("+")  */
    DWORD         pid;
    HANDLE        proc;        /* process handle (owned)                       */
    HANDLE        jobobj;      /* Job Object (owned, may be NULL)              */
    HANDLE        wait;        /* handle from RegisterWaitForSingleObject      */
    char         *cmd;         /* command text without trailing '&'            */
    volatile LONG done;        /* set by the wait callback                     */
    DWORD         exit_code;
    int           notified;    /* "Done" already shown by `jobs`               */
    int           claimed;     /* fg_job() is removing it: reap must skip it   */
} Job;

static Job              g_jobs[MAX_JOBS + 1];   /* index = job id (slot 0 unused) */
static unsigned long    g_seq = 0;
static CRITICAL_SECTION g_lock;

/* Lazy, thread-safe initialisation of the critical section. */
static BOOL CALLBACK init_lock_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}
static void ensure_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_lock_once, NULL, NULL);
}

/* Write a UTF-8 string to a HANDLE (console, file or pipe). */
static void write_handle(HANDLE h, const char *s)
{
    DWORD wr;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s, (DWORD)strlen(s), &wr, NULL);
}
static void write_err(const char *s) { write_handle(GetStdHandle(STD_ERROR_HANDLE), s); }

/* ---------------------------------------------------------------------------
 *  Wait callback: runs on a thread-pool thread when a job's process exits
 * ------------------------------------------------------------------------- */
static VOID CALLBACK on_process_exit(PVOID ctx, BOOLEAN timed_out)
{
    Job *j = (Job *)ctx;
    (void)timed_out;                         /* INFINITE wait: never a timeout */
    EnterCriticalSection(&g_lock);
    DWORD code = 0;
    GetExitCodeProcess(j->proc, &code);
    j->exit_code = code;
    j->done = 1;
    LeaveCriticalSection(&g_lock);
}

/* '+' = current job, '-' = previous one (bash convention). Caller holds lock. */
static char marker_locked(const Job *j)
{
    unsigned long max1 = 0, max2 = 0;
    for (int i = 1; i <= MAX_JOBS; i++) {
        if (!g_jobs[i].in_use) continue;
        if (g_jobs[i].seq > max1)      { max2 = max1; max1 = g_jobs[i].seq; }
        else if (g_jobs[i].seq > max2) { max2 = g_jobs[i].seq; }
    }
    return j->seq == max1 ? '+' : (j->seq == max2 ? '-' : ' ');
}

/* Release everything a (copied) job entry owns. Never call with the lock held. */
static void destroy_job(Job *j)
{
    if (j->wait) UnregisterWaitEx(j->wait, INVALID_HANDLE_VALUE);  /* blocks until callback is finished */
    if (j->proc)   CloseHandle(j->proc);
    if (j->jobobj) CloseHandle(j->jobobj);
    free(j->cmd);
}

/* ---------------------------------------------------------------------------
 *  jobs_add
 * ------------------------------------------------------------------------- */
void jobs_add(HANDLE proc, DWORD pid, const char *cmdline)
{
    ensure_init();

    /* Copy the command text and drop the trailing "&" and spaces for display. */
    char *cmd = _strdup(cmdline ? cmdline : "");
    if (!cmd) { CloseHandle(proc); return; }
    size_t l = strlen(cmd);
    while (l > 0 && (cmd[l - 1] == '&' || cmd[l - 1] == ' ' || cmd[l - 1] == '\t' ||
                     cmd[l - 1] == '\r' || cmd[l - 1] == '\n'))
        cmd[--l] = 0;

    EnterCriticalSection(&g_lock);

    int id = 0;
    for (int i = 1; i <= MAX_JOBS; i++)           /* lowest free number, like bash */
        if (!g_jobs[i].in_use) { id = i; break; }
    if (!id) {
        LeaveCriticalSection(&g_lock);
        write_err("hsh: too many background jobs\n");
        free(cmd);
        CloseHandle(proc);
        return;
    }

    Job *j = &g_jobs[id];
    memset(j, 0, sizeof *j);
    j->in_use = 1;
    j->id     = id;
    j->seq    = ++g_seq;
    j->pid    = pid;
    j->proc   = proc;
    j->cmd    = cmd;

    /* Put the process in a Job Object so the whole process TREE can be managed
     * (terminated) as a unit. Windows 8+ allows nested jobs, so this still works
     * if the terminal already put hsh inside a job. If it fails we simply run
     * without one. NOTE: no KILL_ON_JOB_CLOSE - like bash, background jobs
     * survive when the shell exits (e.g. a GUI app started with `&`). */
    j->jobobj = CreateJobObjectW(NULL, NULL);
    if (j->jobobj && !AssignProcessToJobObject(j->jobobj, proc)) {
        CloseHandle(j->jobobj);
        j->jobobj = NULL;
    }

    /* Ask the thread pool to call on_process_exit when `proc` becomes signalled.
     * Registered while the lock is held, so the callback cannot run before the
     * entry is completely filled in. WT_EXECUTEONLYONCE: one-shot. */
    if (!RegisterWaitForSingleObject(&j->wait, proc, on_process_exit, j,
                                     INFINITE, WT_EXECUTEONLYONCE)) {
        j->wait = NULL;       /* fall back to polling in reap/list (see refresh_locked) */
    }

    char line[64];
    snprintf(line, sizeof line, "[%d] %lu\n", id, (unsigned long)pid);
    LeaveCriticalSection(&g_lock);
    write_err(line);                              /* "[1] 4321" */
}

/* If RegisterWait failed we poll instead. Caller holds the lock. */
static void refresh_locked(Job *j)
{
    if (!j->done && !j->wait && WaitForSingleObject(j->proc, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(j->proc, &j->exit_code);
        j->done = 1;
    }
}

static void format_status(const Job *j, char *buf, size_t n)
{
    if (!j->done)               snprintf(buf, n, "Running");
    else if (j->exit_code == 0) snprintf(buf, n, "Done");
    else                        snprintf(buf, n, "Exit %lu", (unsigned long)j->exit_code);
}

/* ---------------------------------------------------------------------------
 *  jobs_list   (the `jobs` builtin calls this)
 * ------------------------------------------------------------------------- */
void jobs_list(HANDLE out)
{
    ensure_init();
    size_t cap = (size_t)(MAX_JOBS + 1) * 700;
    char *buf = (char *)malloc(cap);
    if (!buf) return;
    buf[0] = 0;
    size_t used = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 1; i <= MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (!j->in_use) continue;
        refresh_locked(j);
        char st[32];
        format_status(j, st, sizeof st);
        int w = snprintf(buf + used, cap - used, "[%d]%c  %-10s %.500s%s\n",
                         j->id, marker_locked(j), st, j->cmd, j->done ? "" : " &");
        if (w > 0) used += (size_t)w;
        if (j->done) j->notified = 1;           /* shown once; reap removes it silently */
    }
    LeaveCriticalSection(&g_lock);

    write_handle(out, buf);                      /* write outside the lock: may block */
    free(buf);
}

/* ---------------------------------------------------------------------------
 *  jobs_reap   (main loop calls it before each prompt)
 * ------------------------------------------------------------------------- */
void jobs_reap(void)
{
    ensure_init();
    for (;;) {
        Job  copy;
        char mark = ' ';
        int  found = 0;

        EnterCriticalSection(&g_lock);
        for (int i = 1; i <= MAX_JOBS; i++) {
            Job *j = &g_jobs[i];
            if (!j->in_use || j->claimed) continue;
            refresh_locked(j);
            if (j->done) {
                mark  = marker_locked(j);
                copy  = *j;                      /* take the entry out of the table... */
                memset(j, 0, sizeof *j);         /* ...and free the slot               */
                found = 1;
                break;
            }
        }
        LeaveCriticalSection(&g_lock);
        if (!found) break;

        if (!copy.notified) {                    /* bash-style completion notice */
            char st[32], line[700];
            format_status(&copy, st, sizeof st);
            snprintf(line, sizeof line, "[%d]%c  %-10s %.500s\n", copy.id, mark, st, copy.cmd);
            write_err(line);
        }
        destroy_job(&copy);                      /* lock NOT held: see header comment */
    }
}

/* Find a job by id (id <= 0 means "current job"). Caller holds the lock. */
static Job *find_job_locked(int id)
{
    if (id > 0) return (id <= MAX_JOBS && g_jobs[id].in_use) ? &g_jobs[id] : NULL;
    Job *best = NULL;
    for (int i = 1; i <= MAX_JOBS; i++)
        if (g_jobs[i].in_use && (!best || g_jobs[i].seq > best->seq)) best = &g_jobs[i];
    return best;
}

/* ---------------------------------------------------------------------------
 *  fg_job : bring a background job to the foreground and wait for it
 * ------------------------------------------------------------------------- */
int fg_job(int id)
{
    ensure_init();

    EnterCriticalSection(&g_lock);
    Job *j = find_job_locked(id);
    if (!j) {
        LeaveCriticalSection(&g_lock);
        char msg[64];
        if (id > 0) snprintf(msg, sizeof msg, "hsh: fg: %d: no such job\n", id);
        else        snprintf(msg, sizeof msg, "hsh: fg: no current job\n");
        write_err(msg);
        return 1;
    }
    j->claimed = 1;                              /* reap must leave it alone now */
    const int jid = j->id;
    HANDLE proc   = j->proc;
    HANDLE jobobj = j->jobobj;
    DWORD  pid    = j->pid;
    char   cmd[512];
    snprintf(cmd, sizeof cmd, "%s\n", j->cmd);
    LeaveCriticalSection(&g_lock);

    write_err(cmd);                              /* bash echoes the command line */

    /* Keep the console mode safe in case the job changed it. */
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode = 0;
    BOOL   have_mode = (hin && hin != INVALID_HANDLE_VALUE) && GetConsoleMode(hin, &mode);

    /*
     * Background jobs were created with CREATE_NEW_PROCESS_GROUP, which makes
     * them IGNORE Ctrl+C.  So while we wait in the foreground we watch the Ctrl+C
     * counter ourselves and translate a press into CTRL_BREAK_EVENT sent to the
     * job's process group (its group id == its pid).  If it does not exit within
     * 2 seconds we kill the whole job tree.
     */
    LONG       seen = hsh_ctrl_c_hits;
    ULONGLONG  interrupted_at = 0;
    for (;;) {
        if (WaitForSingleObject(proc, 100) == WAIT_OBJECT_0) break;   /* 100 ms poll */
        if (hsh_ctrl_c_hits != seen) {
            seen = hsh_ctrl_c_hits;
            if (!interrupted_at) {
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
                interrupted_at = GetTickCount64();
            }
        }
        if (interrupted_at && GetTickCount64() - interrupted_at > 2000) {
            if (jobobj) TerminateJobObject(jobobj, 130);
            else        TerminateProcess(proc, 130);
            interrupted_at = 0;
        }
    }

    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    if (have_mode) SetConsoleMode(hin, mode);

    /* Remove the entry. Order matters: unregister the wait (lock NOT held) so
     * the callback cannot touch the slot after we free it, then clear the slot. */
    Job copy;
    EnterCriticalSection(&g_lock);
    copy = g_jobs[jid];
    LeaveCriticalSection(&g_lock);
    if (copy.wait) UnregisterWaitEx(copy.wait, INVALID_HANDLE_VALUE);

    EnterCriticalSection(&g_lock);
    memset(&g_jobs[jid], 0, sizeof(Job));
    LeaveCriticalSection(&g_lock);

    if (copy.proc)   CloseHandle(copy.proc);
    if (copy.jobobj) CloseHandle(copy.jobobj);
    free(copy.cmd);

    return code == 0xC000013AUL ? 130 : (int)code;
}

/* ---------------------------------------------------------------------------
 *  bg_job : "continue in background"
 *  Windows has no SIGSTOP/SIGCONT for console programs, so a job is never
 *  stopped; it is always running in the background. We just validate and echo.
 * ------------------------------------------------------------------------- */
int bg_job(int id)
{
    ensure_init();
    char msg[700];

    EnterCriticalSection(&g_lock);
    Job *j = find_job_locked(id);
    if (!j) {
        LeaveCriticalSection(&g_lock);
        if (id > 0) snprintf(msg, sizeof msg, "hsh: bg: %d: no such job\n", id);
        else        snprintf(msg, sizeof msg, "hsh: bg: no current job\n");
        write_err(msg);
        return 1;
    }
    refresh_locked(j);
    if (j->done) {
        snprintf(msg, sizeof msg, "hsh: bg: job %d has terminated\n", j->id);
        LeaveCriticalSection(&g_lock);
        write_err(msg);
        return 1;
    }
    snprintf(msg, sizeof msg, "[%d]%c %.500s &\n", j->id, marker_locked(j), j->cmd);
    LeaveCriticalSection(&g_lock);
    write_err(msg);
    return 0;
}
