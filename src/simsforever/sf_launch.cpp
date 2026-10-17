// Sims Forever launcher (32-bit, built by tools/build_simsforever.sh).
// sims4vr.dll runs: sf_launch.exe "<path to Sims.exe>" [arguments for Sims 1]
// It starts Sims 1 in its own folder, waits for its window and loads
// sf_hook.dll (next to this exe) into it. A 32-bit process is needed to load a
// 32-bit DLL into a 32-bit game. Exit code: 0 done, else the step that failed.
#include <windows.h>
#include <shellapi.h>

#include <wchar.h>

#include "sf_shared.h"

static SfShared* g_shared = nullptr;

static void status(const wchar_t* text) {
    if (!g_shared) return;
    char line[128];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, line, sizeof(line) - 1, nullptr, nullptr);
    line[sizeof(line) - 1] = 0;
    memcpy(g_shared->status, line, sizeof(line));
}

struct FindData {
    DWORD pid;
    HWND found;
};
static BOOL CALLBACK findWindow(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == f->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) return 1;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SF_MAPPING_NAME);
    if (mapping) g_shared = static_cast<SfShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SfShared)));

    // Sims 1's folder is its working folder (it loads its data relative to it).
    wchar_t exe[MAX_PATH], folder[MAX_PATH];
    lstrcpynW(exe, argv[1], MAX_PATH);
    lstrcpynW(folder, exe, MAX_PATH);
    wchar_t* cut = wcsrchr(folder, L'\\');
    if (!cut) return 2;
    *cut = 0;
    wchar_t command[2048];
    _snwprintf(command, 2047, L"\"%ls\"", exe);
    command[2047] = 0;
    for (int i = 2; i < argc; ++i) {
        wcsncat(command, L" ", 2047 - wcslen(command));
        wcsncat(command, argv[i], 2047 - wcslen(command));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command, nullptr, nullptr, FALSE, 0, nullptr, folder, &si, &pi)) {
        status(L"launcher: Sims 1 didn't start (path in [simsforever] sims1_path?)");
        return 3;
    }
    status(L"launcher: Sims 1 started, waiting for its window");

    // Its window (it takes a while: intro, loading).
    FindData f{pi.dwProcessId, nullptr};
    for (int i = 0; i < 1200 && !f.found; ++i) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            status(L"launcher: Sims 1 closed before it opened a window");
            return 4;
        }
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    }
    if (!f.found) {
        status(L"launcher: no Sims 1 window after 2 minutes");
        return 5;
    }

    // Load sf_hook.dll (next to this exe) into it.
    wchar_t hook[MAX_PATH];
    GetModuleFileNameW(nullptr, hook, MAX_PATH);
    cut = wcsrchr(hook, L'\\');
    if (!cut) return 6;
    lstrcpyW(cut + 1, L"sf_hook.dll");
    SIZE_T bytes = (lstrlenW(hook) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, hook, bytes, nullptr)) {
        status(L"launcher: couldn't write into Sims 1");
        return 7;
    }
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(pi.hProcess, nullptr, 0, load, remote, 0, nullptr);
    if (!thread) {
        status(L"launcher: couldn't load sf_hook.dll into Sims 1");
        return 8;
    }
    WaitForSingleObject(thread, 10000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!module) {
        status(L"launcher: sf_hook.dll didn't load in Sims 1");
        return 9;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
