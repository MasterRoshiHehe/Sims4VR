#include "crashlog.h"

#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "log.h"

namespace s4vr::crashlog {

namespace {

using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

wchar_t g_textPath[MAX_PATH];
std::wstring g_directory;
WriteDumpFn g_writeDump = nullptr;
std::atomic<int> g_events{0};
std::atomic<int> g_dumps{0};
std::atomic<ULONGLONG> g_last[kActivityCount];
std::atomic<DWORD> g_lastThread[kActivityCount];
const char* const kNames[kActivityCount] = {"camera copy hook", "head joint read", "body SetTransform", "audio listener",
                                            "render scale", "script call", "present"};
thread_local bool t_inHandler = false;
thread_local bool t_quiet = false;

// Formats into a stack buffer and writes straight to the file: no heap, no
// locks, since the crash may have happened inside either.
void put(HANDLE file, const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n <= 0) return;
    if (n > static_cast<int>(sizeof(buffer)) - 1) n = sizeof(buffer) - 1;
    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(n), &written, nullptr);
}

bool readable(DWORD64 address, SIZE_T size) {
    if (address < 0x10000 || address > 0x7ffffffeffffull) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return address + size <= reinterpret_cast<DWORD64>(info.BaseAddress) + info.RegionSize;
}

// "TS4_x64.exe+0x141d081" style name for a code address.
void where(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        snprintf(out, size, "%016llx", static_cast<unsigned long long>(address));
        return;
    }
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    path[length < MAX_PATH ? length : MAX_PATH - 1] = 0;
    const wchar_t* slash = wcsrchr(path, L'\\');
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, name, sizeof(name), nullptr, nullptr);
    snprintf(out, size, "%s+0x%llx", name,
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
}

bool isCode(DWORD64 address) {
    if (!readable(address, 1)) return false;
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info));
    return info.Type == MEM_IMAGE &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

void dumpBytes(HANDLE file, const char* label, DWORD64 address) {
    DWORD64 from = (address & ~0xfull) - 0x20;
    if (!readable(from, 0x80)) return;
    put(file, "  memory at %s (%016llx):\n", label, static_cast<unsigned long long>(address));
    for (int row = 0; row < 8; ++row) {
        const auto* q = reinterpret_cast<const uint64_t*>(from + row * 16);
        const auto* f = reinterpret_cast<const float*>(from + row * 16);
        put(file, "    %016llx: %016llx %016llx  %10.4g %10.4g %10.4g %10.4g\n",
            static_cast<unsigned long long>(from + row * 16), static_cast<unsigned long long>(q[0]),
            static_cast<unsigned long long>(q[1]), f[0], f[1], f[2], f[3]);
    }
}

void walkStack(HANDLE file, const CONTEXT& start) {
    CONTEXT c = start;
    char name[192];
    put(file, "stack:\n");
    for (int i = 0; i < 40 && c.Rip; ++i) {
        where(c.Rip, name, sizeof(name));
        put(file, "  %2d %s\n", i, name);
        if (!readable(c.Rsp, 0x200)) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function, &c, &handlerData, &establisher, nullptr);
        } else {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
    }
    // In case unwinding went wrong: code addresses found on the stack.
    put(file, "return addresses on the stack:\n");
    int found = 0;
    for (DWORD64 p = start.Rsp; found < 24 && p < start.Rsp + 0x1000 && readable(p, 8); p += 8) {
        DWORD64 value = *reinterpret_cast<DWORD64*>(p);
        if (!isCode(value)) continue;
        where(value, name, sizeof(name));
        put(file, "  +0x%03llx %s\n", static_cast<unsigned long long>(p - start.Rsp), name);
        ++found;
    }
}

// Memory the dump should carry besides thread stacks: around the fault and
// around every register that points somewhere readable (a corrupted heap
// block is usually in one of them).
struct Ranges {
    ULONG64 base[24];
    ULONG size[24];
    int count = 0;
    int next = 0;
    void add(DWORD64 address) {
        DWORD64 from = (address & ~0xfffull) - 0x1000;
        if (count < 24 && readable(from, 0x3000)) {
            base[count] = from;
            size[count++] = 0x3000;
        } else if (count < 24 && readable(address & ~0xfffull, 0x1000)) {
            base[count] = address & ~0xfffull;
            size[count++] = 0x1000;
        }
    }
};
Ranges g_ranges;

BOOL CALLBACK dumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
    auto* ranges = static_cast<Ranges*>(param);
    if (input->CallbackType == MemoryCallback) {
        if (ranges->next >= ranges->count) return FALSE;
        output->MemoryBase = ranges->base[ranges->next];
        output->MemorySize = ranges->size[ranges->next];
        ++ranges->next;
        return TRUE;
    }
    return TRUE;
}

// Two at most: the game may raise a harmless access violation first.
void writeDump(HANDLE text, EXCEPTION_POINTERS* pointers, int number) {
    const CONTEXT& c = *pointers->ContextRecord;
    const DWORD64 regs[] = {c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.R8,  c.R9,
                            c.R10, c.R11, c.R12, c.R13, c.R14, c.R15, c.Rip};
    g_ranges = Ranges{};
    for (DWORD64 r : regs) g_ranges.add(r);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\sims4vr_crash_%d.dmp", g_directory.c_str(), number);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        put(text, "minidump: could not create the file (%lu)\n", GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
    MINIDUMP_CALLBACK_INFORMATION callback{dumpCallback, &g_ranges};
    auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                           MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, &callback);
    put(text, "minidump: %s sims4vr_crash_%d.dmp\n", ok ? "wrote" : "failed to write", number);
    CloseHandle(file);
}

LONG CALLBACK onException(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    DWORD code = record.ExceptionCode;
    bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000374 /* heap corruption */ ||
                 code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
                 code == EXCEPTION_IN_PAGE_ERROR;
    if (!fatal || t_inHandler || t_quiet) return EXCEPTION_CONTINUE_SEARCH;
    int event = ++g_events;
    if (event > 8) return EXCEPTION_CONTINUE_SEARCH;
    t_inHandler = true;
    HANDLE text = CreateFileW(g_textPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char at[192];
        where(reinterpret_cast<DWORD64>(record.ExceptionAddress), at, sizeof(at));
        put(text, "\n[%02d:%02d:%02d.%03d] exception #%d: code %08lx at %s on thread %lu\n", t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds, event, code, at, GetCurrentThreadId());
        if (code == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            put(text, "  %s address %016llx\n",
                record.ExceptionInformation[0] == 0 ? "read" : record.ExceptionInformation[0] == 1 ? "write" : "execute",
                static_cast<unsigned long long>(record.ExceptionInformation[1]));
        if (code != EXCEPTION_STACK_OVERFLOW) {
            ULONGLONG now = GetTickCount64();
            put(text, "last mod activity:\n");
            for (int i = 0; i < kActivityCount; ++i) {
                ULONGLONG last = g_last[i].load();
                if (last)
                    put(text, "  %-18s %6llu ms ago (thread %lu)\n", kNames[i], now - last, g_lastThread[i].load());
            }
            const CONTEXT& c = *pointers->ContextRecord;
            put(text, "registers:\n  rax %016llx rbx %016llx rcx %016llx rdx %016llx\n", c.Rax, c.Rbx, c.Rcx, c.Rdx);
            put(text, "  rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
            put(text, "  r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n", c.R8, c.R9, c.R10, c.R11);
            put(text, "  r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n", c.R12, c.R13, c.R14, c.R15);
            const struct { const char* name; DWORD64 value; } regs[] = {
                {"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi},
                {"rdi", c.Rdi}, {"r8", c.R8},   {"r9", c.R9},   {"r12", c.R12}, {"r13", c.R13}};
            for (const auto& r : regs)
                if (!isCode(r.value)) dumpBytes(text, r.name, r.value);
            walkStack(text, c);
            if (g_writeDump) {
                int number = ++g_dumps;
                if (number <= 2) writeDump(text, pointers, number);
            }
        }
        CloseHandle(text);
    }
    t_inHandler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

// A relaunch must not wipe the last crash: when the previous session recorded
// an exception, its text and dumps move to sims4vr_crash_prev*.
void keepPreviousCrash(const std::wstring& directory) {
    HANDLE old = CreateFileW(g_textPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (old == INVALID_HANDLE_VALUE) return;
    char head[512] = {};
    DWORD read = 0;
    ReadFile(old, head, sizeof(head) - 1, &read, nullptr);
    CloseHandle(old);
    if (!std::strstr(head, "] exception #")) return;
    const wchar_t* const names[][2] = {{L"sims4vr_crash.txt", L"sims4vr_crash_prev.txt"},
                                       {L"sims4vr_crash_1.dmp", L"sims4vr_crash_prev_1.dmp"},
                                       {L"sims4vr_crash_2.dmp", L"sims4vr_crash_prev_2.dmp"}};
    for (const auto& name : names) {
        std::wstring from = directory + L"\\" + name[0], to = directory + L"\\" + name[1];
        if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        else
            DeleteFileW(to.c_str());  // an older crash's second dump would read as this one's
    }
    LOG("Crash recorder: kept the last session's crash as sims4vr_crash_prev.txt");
}

}  // namespace

void install(const std::wstring& directory) {
    swprintf(g_textPath, MAX_PATH, L"%ls\\sims4vr_crash.txt", directory.c_str());
    g_directory = directory;
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
        g_writeDump = reinterpret_cast<WriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    keepPreviousCrash(directory);
    // A fresh file each session: if the game dies and this still says nothing
    // happened, the process was killed without an exception (fail-fast/abort).
    HANDLE text = CreateFileW(g_textPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        put(text, "[%02d:%02d:%02d] session started; no exception recorded yet\n", t.wHour, t.wMinute, t.wSecond);
        CloseHandle(text);
    }
    AddVectoredExceptionHandler(1, onException);
    LOG("Crash recorder on (%s)", g_writeDump ? "text and minidump" : "text only");
}

void note(Activity what) {
    g_last[what].store(GetTickCount64(), std::memory_order_relaxed);
    g_lastThread[what].store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void quietThread(bool quiet) { t_quiet = quiet; }

}  // namespace s4vr::crashlog
