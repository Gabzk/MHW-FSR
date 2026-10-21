#include "wndproc_chain.h"
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <cassert>

using namespace mhwfsr::overlay;

static std::vector<std::string> s_log;
static WNDPROC s_splNext = nullptr;
static WndProcChain s_fsrChain;
static int s_gameMsgCount = 0;
static int s_splMsgCount = 0;
static int s_fsrMsgCount = 0;

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg >= WM_USER) {
        s_gameMsgCount++;
        s_log.push_back("GameWndProc received msg " + std::to_string(msg));
        if (msg == WM_USER + 100) {
            // Nested legitimate message with different ID
            SendMessageW(hwnd, WM_USER + 101, wParam, lParam);
        }
        return 42;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MockSplWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg >= WM_USER) {
        s_splMsgCount++;
        s_log.push_back("MockSplWndProc received msg " + std::to_string(msg));
    }
    return CallWindowProcW(s_splNext, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MockFsrWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& chain = s_fsrChain;

    if (chain.InCycle(hwnd, msg, wParam, lParam)) {
        s_log.push_back("MockFsrWndProc detected cycle for msg " + std::to_string(msg));
        chain.Repair(hwnd, &MockFsrWndProc);
        WNDPROC root = chain.ResolveClassProc(hwnd);
        return CallWindowProcW(root, hwnd, msg, wParam, lParam);
    }

    InFlightMsg inFlight{hwnd, msg, wParam, lParam, t_inFlight};
    t_inFlight = &inFlight;
    struct InFlightGuard {
        const InFlightMsg* prev;
        ~InFlightGuard() { t_inFlight = prev; }
    } guard{inFlight.prev};

    if (msg >= WM_USER) {
        s_fsrMsgCount++;
        s_log.push_back("MockFsrWndProc processed msg " + std::to_string(msg));
    }

    WNDPROC next = chain.nextProc.load(std::memory_order_acquire);
    if (!next || next == &MockFsrWndProc) {
        next = chain.ResolveClassProc(hwnd);
    }
    return CallWindowProcW(next, hwnd, msg, wParam, lParam);
}

int main() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GameWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WndProcSelftestClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"TestWindow", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        std::cerr << "Failed to create window\n";
        return 1;
    }

    WNDPROC clsProc = reinterpret_cast<WNDPROC>(GetClassLongPtrW(hwnd, GCLP_WNDPROC));
    assert(clsProc == GameWndProc);
    std::cout << "[PASS] Class WndProc matches GameWndProc\n";

    // 1. SPL installs
    s_splNext = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MockSplWndProc)));
    assert(s_splNext == GameWndProc);

    // 2. FSR installs
    WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MockFsrWndProc)));
    assert(orig == MockSplWndProc);
    s_fsrChain.nextProc.store(orig);

    // 3. SPL re-wraps on resize (reproduces SPL 2.0 bug condition)
    s_splNext = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MockSplWndProc)));
    assert(s_splNext == MockFsrWndProc);
    assert(s_fsrChain.nextProc.load() == MockSplWndProc);
    std::cout << "[PASS] Reproduced SPL 2.0 cyclic subclassing condition (SPL.next=FSR, FSR.next=SPL)\n";

    // Test Scenario A: Proactive check on resize
    bool repaired = s_fsrChain.CheckAndRepairIfWrapped(hwnd, &MockFsrWndProc);
    assert(repaired);
    assert(s_fsrChain.nextProc.load() == GameWndProc);
    std::cout << "[PASS] Scenario A: Proactive CheckAndRepairIfWrapped repaired chain directly to GameWndProc\n";

    // Test message dispatch after proactive repair
    s_gameMsgCount = s_splMsgCount = s_fsrMsgCount = 0;
    LRESULT r = SendMessageW(hwnd, WM_USER + 1, 0, 0);
    assert(r == 42);
    assert(s_splMsgCount == 1);
    assert(s_fsrMsgCount == 1);
    assert(s_gameMsgCount == 1);
    std::cout << "[PASS] Scenario A: Message dispatched smoothly with zero recursion: SPL -> FSR -> Game\n";

    // Test Scenario B: Reactive InCycle check (if proactive check did not run)
    // Deliberately re-create the cycle
    s_fsrChain.nextProc.store(MockSplWndProc);
    s_gameMsgCount = s_splMsgCount = s_fsrMsgCount = 0;
    s_log.clear();

    r = SendMessageW(hwnd, WM_USER + 2, 0, 0);
    assert(r == 42);
    assert(s_gameMsgCount == 1);
    assert(s_fsrChain.nextProc.load() == GameWndProc);
    std::cout << "[PASS] Scenario B: Reactive InCycle caught loop at depth 2, repaired chain and completed successfully\n";

    // Subsequent message should have zero extra recursion
    s_gameMsgCount = s_splMsgCount = s_fsrMsgCount = 0;
    r = SendMessageW(hwnd, WM_USER + 3, 0, 0);
    assert(r == 42);
    assert(s_splMsgCount == 1);
    assert(s_fsrMsgCount == 1);
    assert(s_gameMsgCount == 1);
    std::cout << "[PASS] Scenario B: Subsequent messages dispatched with zero extra recursion\n";

    // Test Scenario C: Legitimate nested message (WM_USER+100 triggers nested WM_USER+101)
    s_gameMsgCount = s_splMsgCount = s_fsrMsgCount = 0;
    int cycleBefore = s_fsrChain.cycleCount.load();
    r = SendMessageW(hwnd, WM_USER + 100, 0, 0);
    assert(r == 42);
    assert(s_gameMsgCount == 2); // 100 and 101
    assert(s_fsrChain.cycleCount.load() == cycleBefore); // Must NOT falsely trigger cycle!
    std::cout << "[PASS] Scenario C: Nested legitimate messages handled without false positive cycle detection\n";

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);

    std::cout << "\nALL WNDPROC CHAIN SELFTESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
