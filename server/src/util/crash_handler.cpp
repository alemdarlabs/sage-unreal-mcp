#include "util/crash_handler.h"
#include "util/env.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>
#include <signal.h>
#include <stdlib.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace sage {

namespace {

std::wstring crashDumpDir() {
    namespace fs = std::filesystem;
    fs::path base;
    const std::string dataDir = sage::util::envValue("SAGE_DATA_DIR");
    const std::string localAppData = sage::util::envValue("LOCALAPPDATA");
    if (!dataDir.empty()) {
        base = fs::path(dataDir) / "crashdumps";
    } else if (!localAppData.empty()) {
        base = fs::path(localAppData) / "CrashDumps" / "sage-server";
    } else {
        base = fs::path("crashdumps");
    }
    std::error_code ec;
    fs::create_directories(base, ec);  // best-effort; CreateFileW reports failures
    return base.wstring();
}

void writeMiniDump(EXCEPTION_POINTERS* ep) {
    const std::wstring dir = crashDumpDir();
    SYSTEMTIME t;
    GetLocalTime(&t);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
        L"%ls\\sage_%04u%02u%02u_%02u%02u%02u_pid%lu.dmp",
        dir.c_str(),
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond,
        GetCurrentProcessId());

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"[sage-crash] CreateFileW failed (%lu) for %ls\n",
                      GetLastError(), path);
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs           |
        MiniDumpWithProcessThreadData  |
        MiniDumpWithThreadInfo         |
        MiniDumpWithUnloadedModules    |
        MiniDumpWithIndirectlyReferencedMemory);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(),
                                      GetCurrentProcessId(),
                                      f, type,
                                      ep ? &mei : nullptr,
                                      nullptr, nullptr);
    CloseHandle(f);
    if (ok) {
        std::fwprintf(stderr, L"[sage-crash] minidump: %ls\n", path);
    } else {
        std::fwprintf(stderr, L"[sage-crash] MiniDumpWriteDump failed (%lu)\n",
                      GetLastError());
    }
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    writeMiniDump(ep);
    return EXCEPTION_EXECUTE_HANDLER;  // process terminates after dump
}

void invalidParamHandler(const wchar_t* expr, const wchar_t* fn,
                         const wchar_t* file, unsigned line, uintptr_t /*reserved*/) {
    std::fwprintf(stderr,
        L"[sage-crash] CRT invalid parameter\n"
        L"  function  : %ls\n"
        L"  location  : %ls:%u\n"
        L"  expression: %ls\n",
        fn   ? fn   : L"(unknown)",
        file ? file : L"(unknown)", line,
        expr ? expr : L"(unspecified)");
    // Promote to structured exception so unhandledFilter runs and we get
    // a minidump with the full call stack at the precondition site.
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void abortSignalHandler(int /*sig*/) {
    std::fwprintf(stderr, L"[sage-crash] abort() raised\n");
    RaiseException(0xE0000002u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

}  // namespace

void installCrashHandlers() {
    SetUnhandledExceptionFilter(unhandledFilter);
    _set_invalid_parameter_handler(invalidParamHandler);

    // Send debug-CRT _CrtDbgReport output to stderr instead of the modal
    // MessageBox + WER popup. This is the line that kills the xmemory:209
    // dialog that previously blocked the process indefinitely.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);

    // Don't show abort()'s "This application has requested the Runtime to
    // terminate it in an unusual way" dialog or the WER report dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    signal(SIGABRT, abortSignalHandler);

    // OS-level: silence WER popup if any subprocess we spawn (e.g. UE
    // editor restart) crashes — they inherit our error mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

}  // namespace sage

#else  // !_WIN32

namespace sage {
void installCrashHandlers() {}
}  // namespace sage

#endif
