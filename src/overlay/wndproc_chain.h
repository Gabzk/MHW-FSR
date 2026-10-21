#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mhwfsr::overlay {

struct InFlightMsg {
    HWND hwnd = nullptr;
    UINT msg = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    const InFlightMsg* prev = nullptr;
};

inline thread_local const InFlightMsg* t_inFlight = nullptr;

class WndProcChain {
public:
    std::atomic<WNDPROC> nextProc{nullptr};
    std::atomic<WNDPROC> classProc{nullptr};
    std::atomic<int> cycleCount{0};
    std::atomic<bool> passthrough{false};

    // Checks if the identical message is already in flight on this thread (recursion/cycle)
    bool InCycle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) const {
        for (const InFlightMsg* p = t_inFlight; p; p = p->prev) {
            if (p->hwnd == hwnd && p->msg == msg && p->wParam == wParam && p->lParam == lParam) {
                return true;
            }
        }
        return false;
    }

    // Resolves and caches the class window procedure (the root WndProc registered for this window class)
    WNDPROC ResolveClassProc(HWND hwnd) {
        WNDPROC cached = classProc.load(std::memory_order_relaxed);
        if (cached) return cached;

        ULONG_PTR ptr = GetClassLongPtrW(hwnd, GCLP_WNDPROC);
        WNDPROC resolved = reinterpret_cast<WNDPROC>(ptr);
        if (!resolved) {
            resolved = &DefWindowProcW;
        }
        classProc.store(resolved, std::memory_order_release);
        return resolved;
    }

    // Repairs the downstream chain target to classProc if it creates a cycle
    bool Repair(HWND hwnd, WNDPROC selfProc) {
        WNDPROC root = ResolveClassProc(hwnd);
        if (!root || root == selfProc) {
            root = &DefWindowProcW;
        }

        WNDPROC oldNext = nextProc.exchange(root, std::memory_order_acq_rel);
        cycleCount.fetch_add(1, std::memory_order_relaxed);
        return oldNext != root;
    }

    // Proactively checks if the current top-of-chain is identical to our downstream target
    bool CheckAndRepairIfWrapped(HWND hwnd, WNDPROC selfProc) {
        WNDPROC curTop = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        WNDPROC curNext = nextProc.load(std::memory_order_acquire);
        if (curTop && curNext && curTop != selfProc && curTop == curNext) {
            return Repair(hwnd, selfProc);
        }
        return false;
    }
};

}  // namespace mhwfsr::overlay
