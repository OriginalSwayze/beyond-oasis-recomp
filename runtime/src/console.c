/* The game is a windowed program (no console window of its own). Started from a terminal,
 * its output (the headless tests' results, errors) goes to that terminal: output that is
 * already redirected to a file or pipe is kept, otherwise it is attached to the parent's
 * console when there is one. Started by double-click there is none, and nothing changes. */
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int redirected(DWORD which) {
    HANDLE h = GetStdHandle(which);
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

void console_attach(void) {
    int out = redirected(STD_OUTPUT_HANDLE), err = redirected(STD_ERROR_HANDLE);
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    if (!out) freopen("CONOUT$", "w", stdout);
    if (!err) freopen("CONOUT$", "w", stderr);
}
#else
void console_attach(void) {}
#endif
