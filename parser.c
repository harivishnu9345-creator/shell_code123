/*
 * parser.c -- command-line parser for hsh (Windows-native bash-style shell)
 *
 * Two phases:
 *   PHASE 1  tokenize(): a 3-state machine turns the raw line into tokens
 *            (WORD or OPERATOR). Quoting, escapes and variable / ~ expansion
 *            all happen here, because they change what a "word" is.
 *   PHASE 2  build pipeline: walks the token list, filling Command structs
 *            and checking the grammar:
 *
 *              line     := command ( '|' command )* [ '&' ]
 *              command  := ( WORD | redirect )+      (at least one WORD)
 *              redirect := ('<' | '>' | '>>' | '2>') WORD  |  '2>&1'
 *
 * Tokenizer states (see tokenize()):
 *
 *     ST_NORMAL  : between/inside unquoted text. Whitespace ends a word,
 *                 operators | < > >> & 2> 2>&1 are recognised, ' and " switch
 *                 state, \ escapes the next special char, $ % ~ expand.
 *     ST_SQ      : inside '...'  -> EVERYTHING is literal until the next '.
 *     ST_DQ      : inside "..."  -> operators/spaces are literal, but $VAR and
 *                 %VAR% still expand, and \ escapes only  "  $  \  %
 *
 * Windows-specific design decisions (important for the viva):
 *   - '\' is the Windows path separator, so outside quotes it only acts as an
 *     escape before   space tab ' " $ | & < > ~ %   . Anything else keeps
 *     the backslash:  C:\Users\foo  stays  C:\Users\foo .
 *   - Environment lookups use GetEnvironmentVariableW and are converted
 *     to/from UTF-8 at that boundary (internal strings are UTF-8).
 *   - Unquoted expansion results are NOT word-split (simplification vs bash).
 *   - 2>> is not in the shared contract (only one 'append' flag), so it is
 *     reported as a clear error instead of silently misbehaving.
 *   - Out of memory aborts the shell (simple and honest for a student shell).
 */
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ============================ small utilities ============================ */

/* realloc that never returns NULL (aborts instead). xrealloc(NULL,n)==malloc */
static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("hsh: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static void *xcalloc(size_t n, size_t sz)
{
    void *p = xrealloc(NULL, n * sz);
    memset(p, 0, n * sz);
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = xrealloc(NULL, n);
    memcpy(d, s, n);
    return d;
}

/* printf-style: store a malloc'd error message in *errmsg (if caller wants) */
static void set_err(char **errmsg, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    if (!errmsg) return;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    *errmsg = xstrdup(tmp);
}

/* ---- growable string buffer used while building one word ---- */
typedef struct { char *d; size_t len, cap; } Buf;

static void buf_putn(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 32;
        while (nc < b->len + n + 1) nc *= 2;
        b->d = xrealloc(b->d, nc);
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
    b->d[b->len] = '\0';
}
static void buf_putc(Buf *b, char c) { buf_putn(b, &c, 1); }

/* hand the string to the caller and reset the buffer */
static char *buf_take(Buf *b)
{
    char *r = b->d ? b->d : xstrdup("");
    b->d = NULL;
    b->len = b->cap = 0;
    return r;
}

/* ======================= environment variable lookup ===================== */

/*
 * Look up an environment variable by UTF-8 name (len bytes, not NUL-terminated).
 * Returns a malloc'd UTF-8 value ("" if defined but empty) or NULL if unset.
 */
static char *env_lookup(const char *name, size_t len)
{
    char nm[256];
    wchar_t wname[256];
    DWORD n, got;
    wchar_t *wv;
    int m;
    char *out;

    if (len == 0 || len > 200) return NULL;
    memcpy(nm, name, len);
    nm[len] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, 0, nm, -1, wname, 256)) return NULL;

    SetLastError(0);
    n = GetEnvironmentVariableW(wname, NULL, 0);   /* size incl. terminator */
    if (n == 0)
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? NULL : xstrdup("");

    wv = xrealloc(NULL, n * sizeof(wchar_t));
    got = GetEnvironmentVariableW(wname, wv, n);
    if (got >= n) { free(wv); return NULL; }       /* changed under us */

    m = WideCharToMultiByte(CP_UTF8, 0, wv, -1, NULL, 0, NULL, NULL);
    out = xrealloc(NULL, (size_t)m);
    WideCharToMultiByte(CP_UTF8, 0, wv, -1, out, m, NULL, NULL);
    free(wv);
    return out;
}

static int is_name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_name_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }
/* characters allowed inside %NAME% (cmd.exe allows e.g. ProgramFiles(x86)) */
static int is_pct_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '(' || c == ')' ||
           c == '-' || c == '.';
}

/*
 * $VAR or ${VAR} at p (p[0]=='$'). Appends the value (nothing if unset, like
 * bash) and returns how many input chars were consumed. A '$' that does not
 * start a valid name is kept literally.
 */
static size_t expand_dollar(const char *p, Buf *w)
{
    const char *name;
    size_t len, consumed;
    char *v;

    if (p[1] == '{') {
        const char *close = strchr(p + 2, '}');
        if (!close || close == p + 2) { buf_putc(w, '$'); return 1; }
        name = p + 2;
        len = (size_t)(close - name);
        consumed = len + 3;                         /* ${ + name + } */
    } else if (is_name_start(p[1])) {
        name = p + 1;
        len = 1;
        while (is_name_char(name[len])) len++;
        consumed = len + 1;                         /* $ + name */
    } else {
        buf_putc(w, '$');
        return 1;
    }
    v = env_lookup(name, len);
    if (v) { buf_putn(w, v, strlen(v)); free(v); }
    return consumed;
}

/*
 * %VAR% at p (p[0]=='%'). Like cmd.exe: if VAR is undefined the text is left
 * as-is; a lone % is a literal percent sign.
 */
static size_t expand_percent(const char *p, Buf *w)
{
    const char *q = p + 1;
    while (is_pct_char(*q)) q++;
    if (*q == '%' && q > p + 1) {
        size_t len = (size_t)(q - (p + 1));
        char *v = env_lookup(p + 1, len);
        if (v) { buf_putn(w, v, strlen(v)); free(v); }
        else   buf_putn(w, p, len + 2);             /* keep "%NAME%" literal */
        return len + 2;
    }
    buf_putc(w, '%');
    return 1;
}

/* '~' only expands when followed by end-of-word, a slash, or an operator */
static int tilde_end(char c)
{
    return c == '\0' || c == '/' || c == '\\' || c == ' ' || c == '\t' ||
           c == '|' || c == '<' || c == '>' || c == '&';
}

/* ============================== PHASE 1 ================================== */

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR2OUT, T_AMP } TokType;
typedef struct { TokType type; char *text; } Token;      /* text only for T_WORD */
typedef struct { Token *v; int n, cap; } TokVec;

static void tv_push(TokVec *tv, TokType t, char *text)
{
    if (tv->n == tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 16;
        tv->v = xrealloc(tv->v, (size_t)tv->cap * sizeof(Token));
    }
    tv->v[tv->n].type = t;
    tv->v[tv->n].text = text;
    tv->n++;
}

static void tv_free(TokVec *tv)
{
    for (int i = 0; i < tv->n; i++) free(tv->v[i].text);   /* NULL-safe */
    free(tv->v);
    tv->v = NULL;
    tv->n = tv->cap = 0;
}

static const char *op_name(TokType t)
{
    switch (t) {
    case T_PIPE:   return "|";
    case T_IN:     return "<";
    case T_OUT:    return ">";
    case T_APPEND: return ">>";
    case T_ERR:    return "2>";
    case T_ERR2OUT:return "2>&1";
    case T_AMP:    return "&";
    default:       return "?";
    }
}

typedef enum { ST_NORMAL, ST_SQ, ST_DQ } State;

/*
 * Finish the current word. A word is emitted if it has text OR was quoted:
 *   ""      -> one empty argument          (quoted, no text)
 *   $UNSET  -> no argument at all          (not quoted, no text)
 */
static void flush_word(TokVec *tv, Buf *w, int *started, int *quoted)
{
    if (w->len > 0 || *quoted) tv_push(tv, T_WORD, buf_take(w));
    *started = *quoted = 0;
}

/* returns 1 on success, 0 on error (message stored in *errmsg) */
static int tokenize(const char *s, TokVec *tv, char **errmsg)
{
    Buf w = {0};
    State st = ST_NORMAL;
    int started = 0;   /* has the current word received any character yet?   */
    int quoted  = 0;   /* did the current word contain a quote (even empty)? */
    const char *p = s;

    while (*p) {
        char c = *p;

        /* ---------- state ST_SQ: everything literal until closing ' ------- */
        if (st == ST_SQ) {
            if (c == '\'') st = ST_NORMAL;
            else buf_putc(&w, c);
            p++;
            continue;
        }

        /* ---------- state ST_DQ: literal except $ % and a few escapes ----- */
        if (st == ST_DQ) {
            if (c == '"') { st = ST_NORMAL; p++; }
            else if (c == '\\' && p[1] && strchr("\"$\\%", p[1])) {
                buf_putc(&w, p[1]);               /* \"  \$  \\  \%  */
                p += 2;
            }
            else if (c == '$') p += expand_dollar(p, &w);
            else if (c == '%') p += expand_percent(p, &w);
            else { buf_putc(&w, c); p++; }
            continue;
        }

        /* ---------- state ST_NORMAL -------------------------------------- */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            flush_word(tv, &w, &started, &quoted);          /* word boundary */
            p++;
        }
        else if (c == '\'') { st = ST_SQ; started = quoted = 1; p++; }
        else if (c == '"')  { st = ST_DQ; started = quoted = 1; p++; }
        else if (c == '\\' && p[1] && strchr(" \t'\"$|&<>~%", p[1])) {
            buf_putc(&w, p[1]);                              /* escaped char */
            started = 1;
            p += 2;
        }
        else if (c == '$') { started = 1; p += expand_dollar(p, &w); }
        else if (c == '%') { started = 1; p += expand_percent(p, &w); }
        else if (c == '~' && !started && tilde_end(p[1])) {
            char *home = env_lookup("USERPROFILE", 11);
            if (home) { buf_putn(&w, home, strlen(home)); free(home); }
            else buf_putc(&w, '~');                          /* no home known */
            started = 1;
            p++;
        }
        else if (c == '|') { flush_word(tv, &w, &started, &quoted); tv_push(tv, T_PIPE, NULL); p++; }
        else if (c == '<') { flush_word(tv, &w, &started, &quoted); tv_push(tv, T_IN,   NULL); p++; }
        else if (c == '>') {
            flush_word(tv, &w, &started, &quoted);
            if (p[1] == '>') { tv_push(tv, T_APPEND, NULL); p += 2; }
            else             { tv_push(tv, T_OUT,    NULL); p++;    }
        }
        else if (c == '&') { flush_word(tv, &w, &started, &quoted); tv_push(tv, T_AMP, NULL); p++; }
        /* "1>" / "2>" are redirects only at the START of a word ("a2>f" is
         * the word a2 followed by '>') */
        else if (!started && c == '1' && p[1] == '>') {
            p++;                          /* drop the '1'; '>' handled next */
        }
        else if (!started && c == '2' && p[1] == '>') {
            if (p[2] == '>') {
                set_err(errmsg, "syntax error: `2>>' is not supported");
                free(w.d);
                return 0;
            }
            if (p[2] == '&' && p[3] == '1') { tv_push(tv, T_ERR2OUT, NULL); p += 4; }
            else                            { tv_push(tv, T_ERR,     NULL); p += 2; }
        }
        else { buf_putc(&w, c); started = 1; p++; }          /* ordinary char */
    }

    if (st == ST_SQ || st == ST_DQ) {
        set_err(errmsg, st == ST_SQ ? "syntax error: unterminated single quote"
                                   : "syntax error: unterminated double quote");
        free(w.d);
        return 0;
    }
    flush_word(tv, &w, &started, &quoted);
    return 1;
}

/* ============================== PHASE 2 ================================== */

typedef enum { P_NONE, P_IN, P_OUT, P_APPEND, P_ERR } Pending;

/* append a new zeroed Command to the pipeline */
static void cmd_push(Pipeline *p)
{
    p->cmds = xrealloc(p->cmds, (size_t)(p->ncmds + 1) * sizeof(Command));
    memset(&p->cmds[p->ncmds], 0, sizeof(Command));
    p->ncmds++;
}

/* add one argument; argv is kept NULL-terminated (handy for CreateProcess) */
static void argv_add(Command *c, char *word)
{
    c->argv = xrealloc(c->argv, (size_t)(c->argc + 2) * sizeof(char *));
    c->argv[c->argc++] = word;
    c->argv[c->argc] = NULL;
}

Pipeline *parse_line(const char *line, char **errmsg)
{
    TokVec tv = {0};
    Pipeline *p;
    Pending pend = P_NONE;

    if (errmsg) *errmsg = NULL;
    if (!line || !tokenize(line, &tv, errmsg)) { tv_free(&tv); return NULL; }
    if (tv.n == 0) { tv_free(&tv); return NULL; }        /* blank line: no-op */

    p = xcalloc(1, sizeof *p);
    cmd_push(p);

    for (int i = 0; i < tv.n; i++) {
        Token *t = &tv.v[i];
        Command *c = &p->cmds[p->ncmds - 1];

        if (t->type == T_WORD) {
            if (pend == P_NONE) {
                argv_add(c, t->text);                    /* ordinary argument */
            } else {                                     /* redirect target   */
                switch (pend) {
                case P_IN:     free(c->in_file);  c->in_file  = t->text; break;
                case P_OUT:    free(c->out_file); c->out_file = t->text; c->append = 0; break;
                case P_APPEND: free(c->out_file); c->out_file = t->text; c->append = 1; break;
                case P_ERR:    free(c->err_file); c->err_file = t->text; c->err_to_out = 0; break;
                default: break;
                }
                pend = P_NONE;
            }
            t->text = NULL;                              /* ownership moved   */
            continue;
        }

        /* any operator while a redirect still needs its file name = error */
        if (pend != P_NONE) {
            set_err(errmsg, "syntax error near `%s'", op_name(t->type));
            goto fail;
        }

        switch (t->type) {
        case T_PIPE:
            if (c->argc == 0) {                          /* "| x", "a | | b" */
                set_err(errmsg, "syntax error near `|'");
                goto fail;
            }
            cmd_push(p);
            break;
        case T_IN:     pend = P_IN;     break;
        case T_OUT:    pend = P_OUT;    break;
        case T_APPEND: pend = P_APPEND; break;
        case T_ERR:    pend = P_ERR;    break;
        case T_ERR2OUT:
            c->err_to_out = 1;
            free(c->err_file);                           /* last one wins */
            c->err_file = NULL;
            break;
        case T_AMP:
            /* contract has ONE background flag => '&' must be the last token */
            if (i != tv.n - 1 || c->argc == 0) {
                set_err(errmsg, "syntax error near `&'");
                goto fail;
            }
            p->background = 1;
            break;
        default: break;
        }
    }

    if (pend != P_NONE) {                                /* "cat <" */
        set_err(errmsg, "syntax error near `newline'");
        goto fail;
    }
    if (p->cmds[p->ncmds - 1].argc == 0) {               /* "ls |"  or  "> f" */
        set_err(errmsg, p->ncmds > 1 ? "syntax error: missing command after `|'"
                                     : "syntax error: missing command");
        goto fail;
    }

    tv_free(&tv);
    return p;

fail:
    free_pipeline(p);
    tv_free(&tv);
    return NULL;
}

void free_pipeline(Pipeline *p)
{
    if (!p) return;
    for (int i = 0; i < p->ncmds; i++) {
        Command *c = &p->cmds[i];
        for (int j = 0; j < c->argc; j++) free(c->argv[j]);
        free(c->argv);
        free(c->in_file);
        free(c->out_file);
        free(c->err_file);
    }
    free(p->cmds);
    free(p);
}
