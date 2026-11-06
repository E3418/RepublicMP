// SPDX-License-Identifier: GPL-3.0-only
// See host.h. The log, the patching primitives and the crash report are derived
// from TesmioLoader by MaxLegend (https://github.com/MaxLegend/TesmioLoader,
// GPL-3.0).
#include "host.h"
#include <wchar.h>

static ModHost g_host;
static CRITICAL_SECTION g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_gameDir[MAX_PATH];

const wchar_t* Host_GameDir() { return g_gameDir; }

// ---------------------------------------------------------------- log

void Logf(const char* fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);

    char line[4200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s\r\n",
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
    if (n <= 0 || g_log == INVALID_HANDLE_VALUE) return;

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_log, line, (DWORD)n, &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

static void OpenLog()
{
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&OpenLog, &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH))
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = 0;
    wcscpy_s(g_gameDir, path);

    wchar_t dir[MAX_PATH + 32];
    swprintf_s(dir, L"%ls\\RepublicMP", path);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(dir, L"%ls\\RepublicMP\\logs", path);
    CreateDirectoryW(dir, nullptr);

    wchar_t file[MAX_PATH + 64];
    swprintf_s(file, L"%ls\\republicmp.log", dir);
    // The last game's log stays: a new start must not take away the log of a desync or a crash.
    wchar_t previous[MAX_PATH + 64];
    swprintf_s(previous, L"%ls\\republicmp.previous.log", dir);
    MoveFileExW(file, previous, MOVEFILE_REPLACE_EXISTING);
    g_log = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// ---------------------------------------------------------------- the game's own log

typedef void (__cdecl* EngineLogFn)(char* fmt, ...);
static EngineLogFn o_LogInfo, o_LogWarn, o_LogError;

#define LOG_FORWARD(HOOK, ORIG, TAG)                                  \
    static void __cdecl HOOK(char* fmt, ...)                          \
    {                                                                 \
        char body[4096];                                              \
        va_list ap;                                                   \
        va_start(ap, fmt);                                            \
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);         \
        va_end(ap);                                                   \
        Logf("%s %s", TAG, body);                                     \
        if (ORIG) ORIG((char*)"%s", body);                            \
    }

LOG_FORWARD(h_LogInfo,  o_LogInfo,  "game.info ")
LOG_FORWARD(h_LogWarn,  o_LogWarn,  "game.WARN ")
LOG_FORWARD(h_LogError, o_LogError, "game.ERROR")

// ---------------------------------------------------------------- crash report

static const char* ExceptionName(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        default:                              return nullptr;
    }
}

static volatile LONG g_inCrashHandler;
static LONG g_crashesReported;

// Turns "it crashed" into an address. It only watches and never swallows an
// exception. It runs before any __try, so a fault the mod catches is reported too
// and counts toward the limit of 8.
static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep)
{
    const char* what = ExceptionName(ep->ExceptionRecord->ExceptionCode);
    if (!what) return EXCEPTION_CONTINUE_SEARCH;

    // Never report from inside a report: a fault raised here would re-enter this
    // and bury the original crash under its own noise.
    if (InterlockedCompareExchange(&g_inCrashHandler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashesReported) > 8)
    {
        InterlockedExchange(&g_inCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    BYTE* exeBase = g_host.exeBase;
    SIZE_T exeSize = g_host.exeSize;

    BYTE* addr = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
    Logf("=== CRASH: %s at %p ===", what, addr);

    if (addr >= exeBase && addr < exeBase + exeSize)
        Logf("    SOVIET64.exe + 0x%zX", (size_t)(addr - exeBase));
    else
    {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &m) && m)
        {
            GetModuleFileNameA(m, name, MAX_PATH);
            Logf("    %s + 0x%zX", name, (size_t)(addr - (BYTE*)m));
        }
    }

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        Logf("    %s address %p",
             ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
             (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    CONTEXT* c = ep->ContextRecord;
    Logf("    rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    Logf("    rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX", c->Rsi, c->Rdi, c->R8, c->R9);
    Logf("    rsp=%016llX rbp=%016llX rip=%016llX", c->Rsp, c->Rbp, c->Rip);

    // Cheap stack walk: a qword on the stack pointing into the executable is very
    // likely a return address. Bound by the stack's own region: reading past it
    // would fault inside this report.
    Logf("    --- return addresses on the stack ---");
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* limit = nullptr;
    if (VirtualQuery((void*)c->Rsp, &mbi, sizeof(mbi)))
        limit = (BYTE*)mbi.BaseAddress + mbi.RegionSize;

    BYTE** sp = (BYTE**)c->Rsp;
    int shown = 0;
    for (int i = 0; i < 768 && shown < 16; i++)
    {
        if (limit && (BYTE*)(sp + i + 1) > limit) break;
        BYTE* v = sp[i];
        if (v >= exeBase && v < exeBase + exeSize)
        {
            Logf("      SOVIET64.exe + 0x%zX", (size_t)(v - exeBase));
            shown++;
        }
    }

    Logf("=== end crash report ===");
    FlushFileBuffers(g_log);
    InterlockedExchange(&g_inCrashHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- start

const ModHost* Host_Init()
{
    InitializeCriticalSection(&g_logLock);
    OpenLog();

    g_host.exe = GetModuleHandleW(nullptr);
    g_host.exeBase = (BYTE*)g_host.exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_host.exeBase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_host.exeBase + dos->e_lfanew);
    g_host.exeSize = nt->OptionalHeader.SizeOfImage;
    g_host.engine = GetModuleHandleA(DLL_ENGINE);

    AddVectoredExceptionHandler(1, CrashHandler);

    Logf("exe base  %p    engine base %p", (void*)g_host.exe, (void*)g_host.engine);

    PatchIat(g_host.exe, DLL_ENGINE, "?C3DLog_PrintInfo@@YAXPEADZZ", (void*)h_LogInfo, (void**)&o_LogInfo, "C3DLog_PrintInfo");
    PatchIat(g_host.exe, DLL_ENGINE, "?C3DLog_PrintWarning@@YAXPEADZZ", (void*)h_LogWarn, (void**)&o_LogWarn, "C3DLog_PrintWarning");
    PatchIat(g_host.exe, DLL_ENGINE, "?C3DLog_PrintError@@YAXPEADZZ", (void*)h_LogError, (void**)&o_LogError, "C3DLog_PrintError");

    return &g_host;
}

// ---------------------------------------------------------------- patching

void** FindIatSlot(HMODULE mod, const char* dll, const char* fn)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return nullptr;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++)
    {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        DWORD nameRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + nameRva);
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) == 0) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}

bool PatchIat(HMODULE mod, const char* dll, const char* fn,
              void* detour, void** original, const char* label)
{
    void** slot = FindIatSlot(mod, dll, fn);
    if (!slot)
    {
        Logf("hook FAILED  %-22s (no import slot)", label);
        return false;
    }

    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot))
    {
        Logf("hook FAILED  %-22s (VirtualProtect %lu)", label, GetLastError());
        return false;
    }

    *original = *slot;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    Logf("hook ok      %-22s orig=%p", label, *original);
    return true;
}

bool InstallInlineHook(void* target, void* detour, void** trampoline,
                       const BYTE* expect, size_t stolen, const char* label)
{
    // The jump is 14 bytes. With fewer stolen bytes it would run past the prologue
    // while the trampoline returns into the middle of the jump's own address.
    if (stolen < 14)
    {
        Logf("hook FAILED  %-22s %zu bytes stolen, the jump needs 14 - refusing to patch", label, stolen);
        return false;
    }

    if (memcmp(target, expect, stolen) != 0)
    {
        Logf("hook FAILED  %-22s prologue mismatch - wrong game build, refusing to patch", label);
        BYTE* t = (BYTE*)target;
        char hex[128];
        int o = 0;
        for (size_t i = 0; i < stolen && o < 120; i++)
            o += _snprintf_s(hex + o, sizeof(hex) - o, _TRUNCATE, "%02X ", t[i]);
        Logf("             found: %s", hex);
        return false;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
    {
        Logf("hook FAILED  %-22s (VirtualAlloc %lu)", label, GetLastError());
        return false;
    }

    memcpy(tramp, target, stolen);
    BYTE* back = tramp + stolen;
    back[0] = 0xFF; back[1] = 0x25;                       // jmp qword ptr [rip+0]
    *(DWORD*)(back + 2) = 0;
    *(void**)(back + 6) = (BYTE*)target + stolen;

    DWORD prot = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("hook FAILED  %-22s (VirtualProtect %lu)", label, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    BYTE* p = (BYTE*)target;
    p[0] = 0xFF; p[1] = 0x25;
    *(DWORD*)(p + 2) = 0;
    *(void**)(p + 6) = detour;
    for (size_t i = 14; i < stolen; i++) p[i] = 0x90;     // keeps the disassembly readable

    VirtualProtect(target, stolen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);

    *trampoline = tramp;
    Logf("hook ok      %-22s target=%p tramp=%p (%zu bytes stolen)", label, target, tramp, stolen);
    return true;
}

BYTE* AllocNear(BYTE* anchor, SIZE_T size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T gran = si.dwAllocationGranularity;

    // Walks outward from the anchor until VirtualAlloc takes a hinted address.
    for (SIZE_T delta = gran; delta < 0x30000000; delta += gran)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            BYTE* want = dir ? anchor + delta : anchor - delta;
            void* got = VirtualAlloc(want, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (got) return (BYTE*)got;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- safe reads

// It walks the regions rather than trusting one VirtualQuery: a region is a run of
// pages sharing state and protection, not an allocation. The first write to a page
// of .data splits its region, so a single query around a long-lived object starts
// failing partway through a session though every byte is readable.
bool ReadablePtr(const void* p, size_t n)
{
    if (!p) return false;

    const BYTE* at = (const BYTE*)p;
    const BYTE* want = at + n;
    while (at < want)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(at, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

        const BYTE* end = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
        if (end <= at) return false;    // no progress: refuse rather than spin
        at = end;
    }
    return true;
}

bool SafeReadStr(const void* p, char* out, size_t n)
{
    out[0] = 0;
    if (!p) return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    const char* s = (const char*)p;
    size_t room = (size_t)(((BYTE*)mbi.BaseAddress + mbi.RegionSize) - (BYTE*)p);
    size_t lim = (room < n - 1) ? room : n - 1;

    size_t i = 0;
    for (; i < lim; i++)
    {
        char c = s[i];
        if (c == 0) break;
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

void Trim(char* s)
{
    char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (char* e = s + strlen(s); e > s; e--)
    {
        char c = e[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        e[-1] = 0;
    }
}
