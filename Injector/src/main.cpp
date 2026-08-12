#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "utils.h"
#include "injector.h"
#include "XorStr.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <intrin.h>

int main(int argc, char** argv) {
    srand((unsigned)(__rdtsc() ^ GetCurrentProcessId() ^ GetTickCount()));
    g_ntdll = GetModuleHandleA(xorstr_("ntdll.dll"));

    const char* dll_path = NULL;

    for (int i = 1; i < argc; i++) {
        dll_path = argv[i];
    }

    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp = {}; tp.PrivilegeCount = 1;
        LookupPrivilegeValueA(NULL, xorstr_("SeDebugPrivilege"), &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL); CloseHandle(hToken);
    }

    static char default_path[MAX_PATH];
    if (!dll_path) {
        GetModuleFileNameA(NULL, default_path, MAX_PATH);
        char* slash = strrchr(default_path, '\\'); if (slash) slash[1] = 0;
        strcat_s(default_path, xorstr_("module.dll")); dll_path = default_path;
    }

    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) { printf(xorstr_("not found\n")); return 1; }

    DWORD pid = getpid();
    if (!pid) { printf(xorstr_("process not found\n")); return 1; }

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid);
    }
    if (!proc) { printf(xorstr_("access denied\n")); return 1; }

    bool ok = inject_dll(proc, dll_path);
    CloseHandle(proc);

    printf(xorstr_("%s\n"), ok ? xorstr_("injected") : xorstr_("failed"));

    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &csbi)) {
        printf(xorstr_("Press any key to exit..."));
        getchar();
    }
    return ok ? 0 : 1;
}
