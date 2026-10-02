// ui.c (Gemini 1) -- reproduced verbatim from chat for integration test
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "shell.h"
#define MAX_LINE 2048
#define MAX_HISTORY 100
static WCHAR history[MAX_HISTORY][MAX_LINE];
static int hist_count = 0;
static void enable_ansi_processing() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
    SetConsoleOutputCP(CP_UTF8);
}
static char* wstr_to_utf8(const WCHAR* wstr) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* utf8 = malloc(size_needed);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, size_needed, NULL, NULL);
    return utf8;
}
void ui_print_prompt(int last_exit, int is_elevated) {
    static bool first_run = true;
    if (first_run) {
        enable_ansi_processing();
        printf("\033[34;1m");
        printf("====================================================\n");
        printf(" hsh - Native Windows Shell (Win32 API) \n");
        printf(" Welcome! Use carefully. Type 'exit' to quit.\n");
        printf("====================================================\n\033[0m");
        first_run = false;
    }
    WCHAR username[256]; DWORD unlen = 256; GetUserNameW(username, &unlen);
    WCHAR hostname[256]; DWORD hnlen = 256; GetComputerNameW(hostname, &hnlen);
    WCHAR cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    WCHAR profile[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    WCHAR display_cwd[MAX_PATH];
    size_t prof_len = wcslen(profile);
    if (prof_len > 0 && _wcsnicmp(cwd, profile, prof_len) == 0) {
        swprintf(display_cwd, MAX_PATH, L"~%s", cwd + prof_len);
    } else { wcscpy(display_cwd, cwd); }
    char* u_utf8 = wstr_to_utf8(username);
    char* h_utf8 = wstr_to_utf8(hostname);
    char* c_utf8 = wstr_to_utf8(display_cwd);
    const char* c_reset = "\033[0m";
    const char* c_blue  = "\033[34;1m";
    const char* c_user  = is_elevated ? "\033[31;1m" : "\033[32;1m";
    const char* c_cwd   = "\033[0m";
    const char* c_frame = (last_exit == 0) ? "\033[34;1m" : "\033[31;1m";
    printf("%s┌──(%s%s㉿%s%s)-[%s%s%s]\n", c_frame, c_user, u_utf8, h_utf8, c_frame, c_cwd, c_utf8, c_frame);
    char symbol = is_elevated ? '#' : '$';
    printf("%s└─%s%c%s ", c_frame, is_elevated ? c_user : c_blue, symbol, c_reset);
    fflush(stdout);
    free(u_utf8); free(h_utf8); free(c_utf8);
}
static void redraw_line(const WCHAR* buf, int len, int pos, int is_elevated) {
    char symbol = is_elevated ? '#' : '$';
    const char* c_color = is_elevated ? "\033[31;1m" : "\033[34;1m";
    char* utf8_buf = wstr_to_utf8(buf);
    printf("\r\033[2K\033[34;1m└─%s%c\033[0m %s", c_color, symbol, utf8_buf);
    if (pos < len) { printf("\033[%dD", len - pos); }
    fflush(stdout);
    free(utf8_buf);
}
char* ui_read_line(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(hIn, &mode)) {
        char* fallback_buf = malloc(MAX_LINE);
        if (fgets(fallback_buf, MAX_LINE, stdin) == NULL) { free(fallback_buf); return NULL; }
        fallback_buf[strcspn(fallback_buf, "\r\n")] = '\0';
        return fallback_buf;
    }
    DWORD origMode = mode;
    SetConsoleMode(hIn, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    WCHAR buf[MAX_LINE] = {0};
    int len = 0, pos = 0;
    int hist_idx = hist_count;
    bool search_mode = false;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW ffd;
    INPUT_RECORD ir; DWORD read;
    while (true) {
        ReadConsoleInputW(hIn, &ir, 1, &read);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vkey = ir.Event.KeyEvent.wVirtualKeyCode;
        WCHAR c = ir.Event.KeyEvent.uChar.UnicodeChar;
        if (vkey != VK_TAB && hFind != INVALID_HANDLE_VALUE) { FindClose(hFind); hFind = INVALID_HANDLE_VALUE; search_mode = false; }
        if (vkey == VK_RETURN) { printf("\n"); break; }
        else if (vkey == VK_BACK) {
            if (pos > 0) { memmove(&buf[pos-1], &buf[pos], (len - pos) * sizeof(WCHAR)); len--; pos--; buf[len] = L'\0'; redraw_line(buf, len, pos, is_elevated()); }
        } else if (vkey == VK_DELETE) {
            if (pos < len) { memmove(&buf[pos], &buf[pos+1], (len - pos - 1) * sizeof(WCHAR)); len--; buf[len] = L'\0'; redraw_line(buf, len, pos, is_elevated()); }
        } else if (vkey == VK_LEFT) { if (pos > 0) { pos--; redraw_line(buf, len, pos, is_elevated()); } }
        else if (vkey == VK_RIGHT) { if (pos < len) { pos++; redraw_line(buf, len, pos, is_elevated()); } }
        else if (vkey == VK_UP) {
            if (hist_idx > 0) { hist_idx--; wcscpy(buf, history[hist_idx]); len = pos = wcslen(buf); redraw_line(buf, len, pos, is_elevated()); }
        } else if (vkey == VK_DOWN) {
            if (hist_idx < hist_count - 1) { hist_idx++; wcscpy(buf, history[hist_idx]); }
            else { hist_idx = hist_count; buf[0] = L'\0'; }
            len = pos = wcslen(buf); redraw_line(buf, len, pos, is_elevated());
        } else if (vkey == VK_HOME) { pos = 0; redraw_line(buf, len, pos, is_elevated()); }
        else if (vkey == VK_END) { pos = len; redraw_line(buf, len, pos, is_elevated()); }
        else if (c == 12) { printf("\033[2J\033[H"); redraw_line(buf, len, pos, is_elevated()); }
        else if (vkey == VK_TAB) {
            if (!search_mode) {
                int start = pos; while (start > 0 && buf[start-1] != L' ') start--;
                WCHAR search_pattern[MAX_PATH];
                wcsncpy(search_pattern, &buf[start], pos - start);
                search_pattern[pos - start] = L'\0';
                wcscat(search_pattern, L"*");
                hFind = FindFirstFileW(search_pattern, &ffd);
                search_mode = true;
            } else {
                if (!FindNextFileW(hFind, &ffd)) { FindClose(hFind); hFind = INVALID_HANDLE_VALUE; search_mode = false; continue; }
            }
            if (hFind != INVALID_HANDLE_VALUE) {
                int start = pos; while (start > 0 && buf[start-1] != L' ') start--;
                wcscpy(&buf[start], ffd.cFileName);
                len = pos = start + wcslen(ffd.cFileName);
                redraw_line(buf, len, pos, is_elevated());
            }
        } else if (c >= 32 && c <= 126) {
            if (len < MAX_LINE - 1) { memmove(&buf[pos+1], &buf[pos], (len - pos) * sizeof(WCHAR)); buf[pos] = c; len++; pos++; buf[len] = L'\0'; redraw_line(buf, len, pos, is_elevated()); }
        }
    }
    SetConsoleMode(hIn, origMode);
    if (len > 0) {
        if (hist_count == 0 || wcscmp(history[hist_count - 1], buf) != 0) {
            if (hist_count < MAX_HISTORY) { wcscpy(history[hist_count++], buf); }
            else { memmove(&history[0], &history[1], (MAX_HISTORY - 1) * sizeof(history[0])); wcscpy(history[MAX_HISTORY - 1], buf); }
        }
    }
    return wstr_to_utf8(buf);
}
