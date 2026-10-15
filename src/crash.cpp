#include "crash.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "log.h"

namespace mhwfsr {

namespace {

std::atomic<int> g_reported{0};
// First-chance exceptions are logged too (they may still be handled), so keep a few.
constexpr int kMaxReports = 8;

std::string Describe(DWORD64 address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* name = strrchr(path, '\\');
            return std::format("{}+{:#x}", name ? name + 1 : path, address - reinterpret_cast<DWORD64>(module));
        }
    }
    return std::format("{:#x}", address);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW && rec->ExceptionCode != 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */ &&
        rec->ExceptionCode != 0xC0000374 /* STATUS_HEAP_CORRUPTION */)
        return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = info->ContextRecord;
    // Faults inside the plugin come from SafeRead and are handled there.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Handler), &self);
    HMODULE where = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(c->Rip), &where);
    if (where && where == self) return EXCEPTION_CONTINUE_SEARCH;
    if (g_reported.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

    std::string access;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        access = std::format(", {} {:#x}", rec->ExceptionInformation[0] == 1 ? "writing" : "reading",
                             rec->ExceptionInformation[1]);
    LOGE("exception {:#x} at {}{} (thread {})", rec->ExceptionCode, Describe(c->Rip), access, GetCurrentThreadId());
    LOGE("  rax {:016x} rbx {:016x} rcx {:016x} rdx {:016x}", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOGE("  rsi {:016x} rdi {:016x} rbp {:016x} rsp {:016x}", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    LOGE("  r8  {:016x} r9  {:016x} r10 {:016x} r11 {:016x}", c->R8, c->R9, c->R10, c->R11);
    LOGE("  r12 {:016x} r13 {:016x} r14 {:016x} r15 {:016x}", c->R12, c->R13, c->R14, c->R15);

    CONTEXT walk = *c;
    for (int frame = 0; frame < 24 && walk.Rip; ++frame) {
        LOGE("  #{:02} {}", frame, Describe(walk.Rip));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
        if (!fn) {
            // Leaf function: the return address is on top of the stack.
            walk.Rip = *reinterpret_cast<DWORD64*>(walk.Rsp);
            walk.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLogger() { AddVectoredExceptionHandler(1, &Handler); }

}  // namespace mhwfsr
