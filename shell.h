/*
 * shell.h -- SHARED CONTRACT for hsh (native Windows shell, no WSL / no fork)
 *
 * Every module (parser, executor, builtins, UI, jobs) includes this file and
 * talks to the others ONLY through the declarations below.
 */
#ifndef HSH_SHELL_H
#define HSH_SHELL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Parsed command structures (produced by parser.c, consumed by executor)
 * All strings are UTF-8, heap allocated, owned by the Pipeline.
 * ------------------------------------------------------------------------- */
typedef struct {
    char **argv;      /* NULL-terminated argument vector; argv[0] = program   */
    int    argc;      /* number of entries in argv (always >= 1 after parse)  */
    char  *in_file;   /* "< file"   -> file name, or NULL                     */
    char  *out_file;  /* "> file" / ">> file" -> file name, or NULL           */
    char  *err_file;  /* "2> file"  -> file name, or NULL                     */
    int    append;    /* 1 if out_file came from ">>", else 0                 */
    int    err_to_out;/* 1 if "2>&1" was given (stderr -> same place as stdout)*/
} Command;

typedef struct {
    Command *cmds;    /* array of ncmds commands, left-to-right in the line   */
    int      ncmds;   /* number of commands joined by '|'                     */
    int      background; /* 1 if the line ended with '&'                      */
} Pipeline;

/* ---------------------------------------------------------------------------
 * PARSER MODULE (parser.c)
 *
 * parse_line(line, errmsg)
 *   Success      -> returns a Pipeline (ncmds >= 1); *errmsg == NULL.
 *   Empty line   -> returns NULL and *errmsg == NULL   (nothing to execute).
 *   Syntax error -> returns NULL and *errmsg = malloc'd message; the caller
 *                   prints it and then free()s it.
 *   'errmsg' itself may be NULL if the caller does not care.
 *
 * free_pipeline(p): frees everything parse_line allocated (NULL-safe).
 * ------------------------------------------------------------------------- */
Pipeline *parse_line(const char *line, char **errmsg);
void      free_pipeline(Pipeline *p);

/* ---------------------------------------------------------------------------
 * Other modules (implemented by other team members; only declared here)
 * ------------------------------------------------------------------------- */
int  execute_pipeline(Pipeline *p, const char *raw_line); /* returns exit code */

typedef int (*BuiltinFn)(int argc, char **argv, HANDLE in, HANDLE out, HANDLE err);
BuiltinFn find_builtin(const char *name);                 /* NULL if not builtin */

int  resolve_program(const char *name, char *out_path, size_t n); /* 0 = found */

void  ui_print_prompt(int last_exit, int is_elevated);
char *ui_read_line(void);                                 /* history + editing */

void jobs_add(HANDLE proc, DWORD pid, const char *cmdline);
void jobs_list(HANDLE out);
void jobs_reap(void);
int  fg_job(int id);
int  bg_job(int id);

int  is_elevated(void);

#endif /* HSH_SHELL_H */
