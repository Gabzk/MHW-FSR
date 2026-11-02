#include "overlay.h"
#include "wndproc_chain.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "antilag.h"
#include "config.h"
#include "d3d_util.h"
#include "fsr_context.h"
#include "fsr_loader.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "ngx_shim.h"
#include "telemetry.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mhwfsr::overlay {

namespace {

using Microsoft::WRL::ComPtr;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using GetCurrentBackBufferIndexFn = UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn g_realPresent = nullptr;
Present1Fn g_realPresent1 = nullptr;
ResizeBuffersFn g_realResizeBuffers = nullptr;
GetCurrentBackBufferIndexFn g_realGetCurrentBackBufferIndex = nullptr;
ExecuteCommandListsFn g_realExecuteCommandLists = nullptr;

std::atomic<ID3D12CommandQueue*> g_gameCommandQueue{nullptr};
std::atomic<uint64_t> g_gameBbIdxQueries{0};

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
std::vector<ComPtr<ID3D12Resource>> g_renderTargets;

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    UINT64 fenceValue = 0;
};
std::vector<FrameContext> g_frameContexts;
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValue = 0;

constexpr UINT kMinRtvDescriptors = 8;
UINT g_rtvCapacity = 0;
UINT g_rtvDescriptorSize = 0;
UINT g_bufferCount = 0;
UINT g_swapChainWidth = 0;
UINT g_swapChainHeight = 0;
DXGI_FORMAT g_swapChainFormat = DXGI_FORMAT_UNKNOWN;
HWND g_hwnd = nullptr;
WndProcChain g_wndProcChain;

std::atomic<bool> g_hooked{false};
bool g_initialized = false;
bool g_showMenu = false;
std::string g_saveFeedback;
std::chrono::steady_clock::time_point g_saveFeedbackTime;
std::recursive_mutex g_mutex;

UINT STDMETHODCALLTYPE HookGetCurrentBackBufferIndex(IDXGISwapChain3* swapChain) {
    uint64_t count = g_gameBbIdxQueries.fetch_add(1, std::memory_order_relaxed);
    UINT idx = g_realGetCurrentBackBufferIndex ? g_realGetCurrentBackBufferIndex(swapChain) : 0;
    if (count == 0) {
        LOGI("overlay: confirmed game queries IDXGISwapChain3::GetCurrentBackBufferIndex (initial idx={})", idx);
    }
    return idx;
}

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (queue && !g_gameCommandQueue.load(std::memory_order_relaxed)) {
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            ID3D12CommandQueue* expected = nullptr;
            if (g_gameCommandQueue.compare_exchange_strong(expected, queue)) {
                LOGI("overlay: captured game DIRECT ID3D12CommandQueue at {}", static_cast<void*>(queue));
            }
        }
    }
    if (g_realExecuteCommandLists) {
        g_realExecuteCommandLists(queue, numCommandLists, ppCommandLists);
    }
}

void ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 5.0f);

    // Deep Obsidian Dark Base
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.52f, 0.54f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.97f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.11f, 0.14f, 0.90f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.13f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.22f, 0.22f, 0.27f, 0.75f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Dark Card Elements
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.13f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.08f, 0.10f, 0.85f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);

    // AMD FidelityFX Ruby Red Accents
    colors[ImGuiCol_CheckMark]             = ImVec4(0.92f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.85f, 0.15f, 0.22f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.78f, 0.14f, 0.20f, 0.90f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.92f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(1.00f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.18f, 0.18f, 0.22f, 0.90f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.28f, 0.28f, 0.34f, 1.00f);

    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.22f, 0.27f, 0.65f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.85f, 0.15f, 0.22f, 0.85f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.95f, 0.20f, 0.28f, 1.00f);

    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.85f, 0.15f, 0.22f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.95f, 0.20f, 0.28f, 0.75f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(1.00f, 0.28f, 0.35f, 1.00f);
}

LRESULT CALLBACK HookWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& chain = g_wndProcChain;

    // 1. Proactive check: if another hook (e.g. SPL 2.0 on ResizeBuffers)
    // wrapped the window and points to us as its next, calling our nextProc would loop.
    if (chain.CheckAndRepairIfWrapped(hWnd, &HookWndProc)) {
        LOGW("overlay: WndProc cycle detected proactively (another hook wrapped ours); repaired chain to class WndProc");
    }

    // 2. Reactive check: identical message already in-flight on this thread
    if (chain.InCycle(hWnd, msg, wParam, lParam)) {
        int c = chain.cycleCount.fetch_add(1, std::memory_order_relaxed);
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("overlay: WndProc recursion cycle detected; repairing chain to class WndProc");
        }
        chain.Repair(hWnd, &HookWndProc);

        if (c > 3) {
            chain.passthrough.store(true, std::memory_order_relaxed);
            static bool ptLogged = false;
            if (!ptLogged) {
                ptLogged = true;
                LOGE("overlay: persistent WndProc loop detected; activating emergency passthrough mode");
            }
        }

        WNDPROC root = chain.ResolveClassProc(hWnd);
        return CallWindowProcW(root, hWnd, msg, wParam, lParam);
    }

    if (chain.passthrough.load(std::memory_order_relaxed)) {
        WNDPROC root = chain.ResolveClassProc(hWnd);
        return CallWindowProcW(root, hWnd, msg, wParam, lParam);
    }

    InFlightMsg inFlight{hWnd, msg, wParam, lParam, t_inFlight};
    t_inFlight = &inFlight;
    struct InFlightGuard {
        const InFlightMsg* prev;
        ~InFlightGuard() { t_inFlight = prev; }
    } guard{inFlight.prev};

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (wParam == VK_F10 || wParam == VK_INSERT)) {
        bool isRepeat = (lParam & (1 << 30)) != 0;
        if (!isRepeat) {
            static auto s_lastToggle = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastToggle).count() > 150) {
                s_lastToggle = now;
                g_showMenu = !g_showMenu;
            }
        }
        return 0; // Consume toggle hotkey completely so game doesn't receive it
    }

    if (g_showMenu) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        // Block ALL mouse and keyboard input messages from reaching MHW while menu is open
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
            msg == WM_SYSCHAR || msg == WM_INPUT) {
            return 0;
        }
    }

    WNDPROC next = chain.nextProc.load(std::memory_order_acquire);
    if (!next || next == &HookWndProc) {
        next = chain.ResolveClassProc(hWnd);
    }
    return CallWindowProcW(next, hWnd, msg, wParam, lParam);
}

bool CreateRenderTargetViews(IDXGISwapChain3* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    DXGI_FORMAT rtvFormat = desc.BufferDesc.Format;
    if (rtvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (rtvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (rtvFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS) rtvFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_renderTargets.resize(g_bufferCount);
    for (UINT i = 0; i < g_bufferCount; ++i) {
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i])))) {
            LOGE("overlay: failed to get swapchain backbuffer {}", i);
            return false;
        }
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), &rtvDesc, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
    return true;
}

bool InitD3D12(IDXGISwapChain3* swapChain) {
    ID3D12CommandQueue* cmdQueue = g_gameCommandQueue.load();
    if (!cmdQueue) {
        LOGW("overlay: waiting for game DIRECT command queue before initializing overlay");
        return false;
    }

    if (!g_device && FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        LOGE("overlay: cannot query ID3D12Device from swapchain");
        return false;
    }
    antilag::Init(g_device.Get());
    telemetry::Init(g_device.Get());

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    g_bufferCount = desc.BufferCount;
    g_swapChainWidth = desc.BufferDesc.Width;
    g_swapChainHeight = desc.BufferDesc.Height;
    g_swapChainFormat = desc.BufferDesc.Format;

    LOGI("overlay: swapchain BufferCount={}, {}x{}, format={}",
         g_bufferCount, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format));

    // RTV Heap (reserve capacity for at least 8 backbuffers)
    g_rtvCapacity = std::max(g_bufferCount, kMinRtvDescriptors);
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = g_rtvCapacity;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) {
        LOGE("overlay: cannot create RTV descriptor heap");
        return false;
    }
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateRenderTargetViews(swapChain)) return false;

    // SRV Heap for ImGui Font & Textures
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) {
        LOGE("overlay: cannot create SRV descriptor heap for ImGui");
        return false;
    }

    // Command Allocators per frame & Command List
    g_frameContexts.resize(g_bufferCount);
    for (UINT i = 0; i < g_bufferCount; ++i) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContexts[i].commandAllocator)))) {
            LOGE("overlay: cannot create command allocator {}", i);
            return false;
        }
        g_frameContexts[i].fenceValue = 0;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContexts[0].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList)))) {
        LOGE("overlay: cannot create graphics command list");
        return false;
    }
    g_commandList->Close();

    // Fence for frame synchronization
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        LOGE("overlay: cannot create synchronization fence");
        return false;
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        LOGE("overlay: cannot create fence event");
        return false;
    }

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Avoid cluttering game folder with imgui.ini

    ApplyTheme();

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        LOGE("overlay: ImGui_ImplWin32_Init failed");
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = g_device.Get();
    initInfo.CommandQueue = cmdQueue;
    initInfo.NumFramesInFlight = std::max<int>(static_cast<int>(g_bufferCount) * 2, 6);
    initInfo.RTVFormat = desc.BufferDesc.Format;
    initInfo.SrvDescriptorHeap = g_srvHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    initInfo.LegacySingleSrvGpuDescriptor = g_srvHeap->GetGPUDescriptorHandleForHeapStart();

    if (!ImGui_ImplDX12_Init(&initInfo)) {
        LOGE("overlay: ImGui_ImplDX12_Init failed");
        return false;
    }

    // Hook game window input
    WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
    if (orig != &HookWndProc) {
        g_wndProcChain.nextProc.store(orig, std::memory_order_release);
    }
    WNDPROC cls = g_wndProcChain.ResolveClassProc(g_hwnd);
    if (orig && orig != cls) {
        LOGI("overlay: hooked game window WndProc (previous hook at {:#x}, class WndProc at {:#x})",
             reinterpret_cast<uintptr_t>(orig), reinterpret_cast<uintptr_t>(cls));
    } else {
        LOGI("overlay: hooked game window WndProc (class WndProc at {:#x})",
             reinterpret_cast<uintptr_t>(cls));
    }

    auto versions = loader::QueryVersions(g_device.Get());
    if (!versions.empty()) {
        uint64_t targetId = loader::ResolveVersionId(g_device.Get(), g_config.version);
        std::string initialVer = versions[0].name;
        for (const auto& v : versions) {
            if (targetId != 0 && v.id == targetId) {
                initialVer = v.name;
                break;
            }
        }
        SetActiveFsrVersion(loader::FormatVersionName(initialVer));
    }

    LOGI("MHW-FSR DirectX 12 Overlay initialized successfully (Hotkey: [F10] or [Insert])");
    return true;
}

void RenderOsdBadge() {
    if (!g_config.showOsd) return;

    bool dlssOn = game::DlssEnabledInGame();
    float scale = game::RenderScale();
    const char* modeName = QualityModeName(g_config.mode);
    telemetry::Metrics m = telemetry::GetMetrics();
    bool al2Active = antilag::IsActive();

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##MhwFsrOsd", nullptr, flags)) {
        if (dlssOn) {
            std::string ver = ActiveFsrVersion();
            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "%s", ver.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "DLSS OFF");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s (%.0f%%)", modeName, scale * 100.0f);

        if (g_config.showFps) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            float baseFps = m.baseFps > 0.1f ? m.baseFps : ImGui::GetIO().Framerate;
            float dispFps = m.presentedFps > 0.1f ? m.presentedFps : baseFps;
            float dispDt = m.presentedFrameTimeMs > 0.1f ? m.presentedFrameTimeMs : (1000.0f / std::max(1.0f, dispFps));

            ImGui::Text("%.0f FPS (%.1fms)", dispFps, dispDt);
        }

        if (al2Active) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.22f, 0.28f, 1.0f), "AL2");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 0.8f), "[F10]");

        if (g_config.showLatencyOsd && dlssOn) {
            ImGui::Separator();
            float upMs = m.upscaleGpuMs > 0.01f ? m.upscaleGpuMs : m.upscaleCpuMs;
            ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 0.95f),
                               "Upscale: %.2fms (%s)",
                               upMs, m.upscaleGpuMs > 0.01f ? "GPU" : "CPU");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (al2Active) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.50f, 0.95f), "AL2 Wait: %.2fms", m.antiLagWaitMs);
            } else {
                ImGui::TextDisabled("AL2: OFF");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 0.95f), "PC Lat: %.1fms", m.inputToPresentMs);
        }
    }
    ImGui::End();
}

void DrawSectionHeader(const char* title, const char* rightText = nullptr, ImVec4 rightColor = ImVec4(0.6f, 0.6f, 0.65f, 1.0f)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.20f, 0.27f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (rightText && rightText[0] != '\0') {
        float textW = ImGui::CalcTextSize(rightText).x;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > textW + 10.0f) {
            ImGui::SameLine(ImGui::GetCursorPosX() + avail - textW);
            ImGui::TextColored(rightColor, "%s", rightText);
        }
    }
    ImGui::Separator();
}

bool SegmentedButton(const char* label, bool active, float width = 0.0f) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.16f, 0.22f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.20f, 0.27f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.26f, 0.33f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.24f, 0.29f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.30f, 0.36f, 1.00f));
    }
    bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleColor(3);
    return pressed;
}

void RenderMainWindow() {
    if (!g_showMenu) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("MHW-FSR :: AMD FidelityFX Control Center", &g_showMenu, flags)) {
        bool dlssOn = game::DlssEnabledInGame();
        float currentScale = game::RenderScale();
        telemetry::Metrics m = telemetry::GetMetrics();

        float baseFps = m.baseFps > 0.1f ? m.baseFps : ImGui::GetIO().Framerate;
        float dispFps = m.presentedFps > 0.1f ? m.presentedFps : baseFps;
        float dispDt = m.presentedFrameTimeMs > 0.1f ? m.presentedFrameTimeMs : (1000.0f / std::max(1.0f, dispFps));

        // 1. Top Status Bar (4-Column Live Telemetry Strip)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.14f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
        ImGui::BeginChild("StatusStrip", ImVec2(0, 56), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 2.0f));
        if (ImGui::BeginTable("StatusGrid", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthStretch, 1.45f);
            ImGui::TableSetupColumn("Framerate", ImGuiTableColumnFlags_WidthStretch, 1.20f);
            ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthStretch, 0.80f);

            ImGui::TableNextRow();

            // Col 0: Engine Status
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("STATUS");
            if (dlssOn) {
                ImGui::TextColored(ImVec4(0.25f, 0.92f, 0.48f, 1.0f), "%s", ActiveFsrVersion().c_str());
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "DLSS OFF in Game");
            }

            // Col 1: Active Resolution
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("RESOLUTION (%s)", QualityModeName(g_config.mode));
            if (g_swapChainWidth > 0 && g_swapChainHeight > 0) {
                unsigned renW = std::max(1u, static_cast<unsigned>(std::lround(g_swapChainWidth * currentScale)));
                unsigned renH = std::max(1u, static_cast<unsigned>(std::lround(g_swapChainHeight * currentScale)));
                ImGui::Text("%ux%u -> %ux%u", renW, renH, g_swapChainWidth, g_swapChainHeight);
            } else {
                ImGui::Text("%.0f%% Scale", currentScale * 100.0f);
            }

            // Col 2: Framerate
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("FRAMERATE");
            ImGui::TextColored(ImVec4(0.38f, 0.86f, 1.0f, 1.0f), "%.1f FPS (%.1f ms)", dispFps, dispDt);

            // Col 3: PC Latency
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("PC LATENCY");
            ImGui::TextColored(ImVec4(0.96f, 0.80f, 0.32f, 1.0f), "%.1f ms", m.inputToPresentMs);

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // 2. Section: Super Resolution & Upscaler
        auto availableVersions = loader::QueryVersions(g_device.Get());
        bool hasFsr4 = loader::HasFsr4Support(g_device.Get());
        bool hasDriver412 = loader::HasFsr412DriverSupport(g_device.Get());

        DrawSectionHeader("FIDELITYFX SUPER RESOLUTION",
                          hasFsr4 ? "RDNA ML Active" : "Temporal Upscaler",
                          hasFsr4 ? ImVec4(0.25f, 0.90f, 0.45f, 1.0f) : ImVec4(0.60f, 0.62f, 0.68f, 1.0f));

        if (ImGui::BeginTable("FsrControlsTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

            // Row 1: Engine Version
            struct VersionOption {
                std::string label;
                std::string configVal;
                uint64_t id = 0;
            };
            std::vector<VersionOption> options;
            options.push_back({"Auto (Recommended)", "Auto", 0});

            for (const auto& v : availableVersions) {
                std::string formatted = loader::FormatVersionName(v.name);
                if (formatted.find("FSR 2") != std::string::npos || v.name.rfind("2.", 0) == 0) {
                    continue;
                }
                if (hasDriver412 && formatted.find("4.1.1") != std::string::npos) {
                    continue;
                }
                std::string suffix;
                if (v.name.find('*') != std::string::npos || formatted.find("4.1.2") != std::string::npos) {
                    suffix = " (Driver ML - RDNA 2/3/4)";
                } else if (formatted.find("FSR 4") != std::string::npos) {
                    suffix = " (SDK DirectML - RDNA 3/4)";
                } else if (formatted.find("FSR 3") != std::string::npos) {
                    suffix = " (Temporal)";
                }
                options.push_back({formatted + suffix, formatted, v.id});
            }

            std::vector<const char*> cLabels;
            cLabels.reserve(options.size());
            for (const auto& opt : options) {
                cLabels.push_back(opt.label.c_str());
            }

            int currentVerIdx = 0;
            std::string cfgLower = g_config.version;
            std::transform(cfgLower.begin(), cfgLower.end(), cfgLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (cfgLower != "auto" && cfgLower != "default" && !cfgLower.empty()) {
                uint64_t resolvedId = loader::ResolveVersionId(g_device.Get(), g_config.version);
                for (size_t i = 1; i < options.size(); ++i) {
                    if (options[i].id == resolvedId) {
                        currentVerIdx = static_cast<int>(i);
                        break;
                    }
                }
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Upscaler Engine");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##EngineVersion", &currentVerIdx, cLabels.data(), static_cast<int>(cLabels.size()))) {
                if (currentVerIdx >= 0 && currentVerIdx < static_cast<int>(options.size())) {
                    g_config.version = options[currentVerIdx].configVal;
                } else {
                    g_config.version = "Auto";
                }
                ngx::ResetAllFeatures();
            }

            // Row 2: FSR 4.1.2 Model (when applicable)
            bool fsr412Selected = hasDriver412 && currentVerIdx >= 0 &&
                                  currentVerIdx < static_cast<int>(options.size()) &&
                                  (currentVerIdx == 0 ||
                                   options[currentVerIdx].configVal.find("4.1.2") != std::string::npos);
            if (fsr412Selected) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("FSR 4.1.2 Model");
                ImGui::TableSetColumnIndex(1);

                static const char* kModelPills[] = {"Auto", "Standard", "RDNA2 INT8", "Lite (Fast)"};
                int currentModel = std::clamp(static_cast<int>(g_config.fsr4Model), 0, 3);
                float availW = ImGui::GetContentRegionAvail().x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float btnW = (availW - spacing * 3.0f) / 4.0f;
                for (int i = 0; i < 4; ++i) {
                    if (i > 0) ImGui::SameLine();
                    if (SegmentedButton(kModelPills[i], currentModel == i, btnW)) {
                        g_config.fsr4Model = static_cast<Fsr4Model>(i);
                        ngx::ResetAllFeatures();
                    }
                }
            }

            // Row 3: Quality Preset (Segmented Pill Bar)
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Quality Preset");
            ImGui::TableSetColumnIndex(1);
            {
                static const char* kPresetPills[] = {"NativeAA", "Quality", "Balanced", "Perf", "Custom"};
                int currentPreset = std::clamp(static_cast<int>(g_config.mode), 0, 4);
                float availW = ImGui::GetContentRegionAvail().x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float btnW = (availW - spacing * 4.0f) / 5.0f;
                for (int i = 0; i < 5; ++i) {
                    if (i > 0) ImGui::SameLine();
                    if (SegmentedButton(kPresetPills[i], currentPreset == i, btnW)) {
                        g_config.mode = static_cast<QualityMode>(i);
                        game::RecomputeRenderScale();
                        ngx::ResetAllFeatures();
                    }
                }
            }

            // Row 4: Custom Scale Slider (only visible when Custom is selected)
            if (g_config.mode == QualityMode::Custom) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Custom Scale");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##CustomScale", &g_config.customScale, 0.50f, 1.00f, "%.2fx")) {
                    game::RecomputeRenderScale();
                    ngx::ResetAllFeatures();
                }
            }

            // Row 5: RCAS Sharpness (Slider + Quick Pills on the same line)
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("RCAS Sharpness");
            ImGui::TableSetColumnIndex(1);
            {
                float sharpVal = g_config.Sharpness();
                float availW = ImGui::GetContentRegionAvail().x;
                float quickGroupW = 192.0f;
                ImGui::SetNextItemWidth(std::max(100.0f, availW - quickGroupW - ImGui::GetStyle().ItemSpacing.x));
                if (ImGui::SliderFloat("##Sharpness", &sharpVal, 0.0f, 1.0f, "%.2f")) {
                    g_config.sharpness = sharpVal;
                }
                ImGui::SameLine();
                if (SegmentedButton("Off", std::abs(sharpVal - 0.0f) < 0.02f, 42.0f)) g_config.sharpness = 0.0f;
                ImGui::SameLine();
                if (SegmentedButton("0.33", std::abs(sharpVal - 0.33f) < 0.02f, 44.0f)) g_config.sharpness = 0.33f;
                ImGui::SameLine();
                if (SegmentedButton("0.66", std::abs(sharpVal - 0.66f) < 0.02f, 44.0f)) g_config.sharpness = 0.66f;
                ImGui::SameLine();
                if (SegmentedButton("1.0", std::abs(sharpVal - 1.0f) < 0.02f, 40.0f)) g_config.sharpness = 1.0f;
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();

        // 3. Section: Anti-Lag 2 & Frame Pacing
        bool al2Supported = antilag::IsSupported();
        bool al2Active = antilag::IsActive();
        const char* al2Badge = !al2Supported ? "Unavailable (Radeon Only)"
                                             : (al2Active ? "Driver Active (amdxc64.dll)" : "Ready (Disabled)");
        ImVec4 al2Color = !al2Supported ? ImVec4(0.95f, 0.65f, 0.20f, 1.0f)
                                        : (al2Active ? ImVec4(0.25f, 0.90f, 0.45f, 1.0f) : ImVec4(0.60f, 0.62f, 0.68f, 1.0f));

        DrawSectionHeader("RADEON ANTI-LAG 2 & FRAME PACING", al2Badge, al2Color);

        if (ImGui::BeginTable("Al2ControlsTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

            // Row 1: Anti-Lag 2 Toggle
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Anti-Lag 2");
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("Sync CPU Input Polling with GPU Execution", &g_config.antiLag2);
            if (al2Active) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.50f, 1.0f), "(Wait: %.2f ms)", m.antiLagWaitMs);
            }

            // Row 2: Driver Max FPS Slider + Quick Pills
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Max FPS Limit");
            ImGui::TableSetColumnIndex(1);
            {
                int maxSliderFps = std::max(360, g_config.antiLagMaxFps);
                float availW = ImGui::GetContentRegionAvail().x;
                float quickGroupW = 216.0f;
                ImGui::SetNextItemWidth(std::max(100.0f, availW - quickGroupW - ImGui::GetStyle().ItemSpacing.x));
                ImGui::SliderInt("##DriverMaxFps", &g_config.antiLagMaxFps, 0, maxSliderFps,
                                 g_config.antiLagMaxFps == 0 ? "Unlimited" : "%d FPS");
                ImGui::SameLine();
                if (SegmentedButton("Off", g_config.antiLagMaxFps == 0, 38.0f)) g_config.antiLagMaxFps = 0;
                ImGui::SameLine();
                if (SegmentedButton("60", g_config.antiLagMaxFps == 60, 34.0f)) g_config.antiLagMaxFps = 60;
                ImGui::SameLine();
                if (SegmentedButton("120", g_config.antiLagMaxFps == 120, 38.0f)) g_config.antiLagMaxFps = 120;
                ImGui::SameLine();
                if (SegmentedButton("144", g_config.antiLagMaxFps == 144, 38.0f)) g_config.antiLagMaxFps = 144;
                ImGui::SameLine();
                if (SegmentedButton("240", g_config.antiLagMaxFps == 240, 38.0f)) g_config.antiLagMaxFps = 240;
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();

        // 4. Section: On-Screen Display (OSD) & Telemetry
        DrawSectionHeader("OVERLAY & TELEMETRY PROFILER");

        if (ImGui::BeginTable("OsdTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Corner OSD");
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("Show Badge", &g_config.showOsd);
            ImGui::SameLine(0.0f, 18.0f);
            ImGui::Checkbox("FPS Counter", &g_config.showFps);
            ImGui::SameLine(0.0f, 18.0f);
            ImGui::Checkbox("Latency Breakdown", &g_config.showLatencyOsd);

            ImGui::EndTable();
        }

        if (ImGui::CollapsingHeader("Frame-Time & Latency Breakdown")) {
            ImGui::BeginChild("ProfilerBox", ImVec2(0, 128), true, ImGuiWindowFlags_NoScrollbar);
            {
                if (ImGui::BeginTable("ProfilerMetrics", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 210.0f);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Upscaler Pass (%s)", ActiveFsrVersion().c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "%.2f ms GPU  |  %.2f ms CPU", m.upscaleGpuMs, m.upscaleCpuMs);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Anti-Lag 2 Pacing Wait / GPU Queue");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.2f ms Wait  |  %.2f ms Queue", m.antiLagWaitMs, m.gpuQueueLatencyMs);

                    ImGui::EndTable();
                }

                size_t histOffset = 0;
                const auto& hist = telemetry::GetFrameTimeHistory(histOffset);
                char overlayLabel[64];
                std::snprintf(overlayLabel, sizeof(overlayLabel), "Frame Time: %.2f ms (%.1f FPS)",
                              m.presentedFrameTimeMs, m.presentedFps);
                ImGui::PlotLines("##FrameTimePlot", hist.data(), static_cast<int>(hist.size()),
                                 static_cast<int>(histOffset), overlayLabel, 0.0f, 33.3f, ImVec2(-1, 52));
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 5. Footer Actions
        if (ImGui::Button("Save to MHW-FSR.ini", ImVec2(175, 30))) {
            SaveConfig(g_config);
            g_saveFeedback = "Saved to MHW-FSR.ini!";
            g_saveFeedbackTime = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.32f, 0.32f, 0.38f, 1.0f));
        if (ImGui::Button("Close [F10]", ImVec2(115, 30))) {
            g_showMenu = false;
        }
        ImGui::PopStyleColor(3);

        if (!g_saveFeedback.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - g_saveFeedbackTime).count() < 3) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "%s", g_saveFeedback.c_str());
            } else {
                g_saveFeedback.clear();
            }
        }
    }
    ImGui::End();
}

void WaitForGpu() {
    ID3D12CommandQueue* queue = g_gameCommandQueue.load();
    if (queue && g_fence && g_fenceEvent) {
        UINT64 f = ++g_fenceValue;
        if (SUCCEEDED(queue->Signal(g_fence.Get(), f))) {
            if (g_fence->GetCompletedValue() < f) {
                g_fence->SetEventOnCompletion(f, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, 2000);
            }
        }
    }
}

HRESULT CallRealPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if (pPresentParameters && g_realPresent1) {
        return g_realPresent1(swapChain, syncInterval, flags, pPresentParameters);
    }
    return g_realPresent(swapChain, syncInterval, flags);
}

void RecordImGuiOnBackBuffer(UINT backBufferIdx) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[backBufferIdx].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += backBufferIdx * g_rtvDescriptorSize;
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
}

void ExecuteOverlayCommandList(ID3D12CommandQueue* cmdQueue, FrameContext& frameCtx) {
    g_commandList->Close();
    ID3D12CommandList* cmdLists[] = { g_commandList.Get() };
    if (g_realExecuteCommandLists) {
        g_realExecuteCommandLists(cmdQueue, 1, cmdLists);
    } else {
        cmdQueue->ExecuteCommandLists(1, cmdLists);
    }
    UINT64 currentFence = ++g_fenceValue;
    cmdQueue->Signal(g_fence.Get(), currentFence);
    frameCtx.fenceValue = currentFence;
}

UINT QueryBackBufferIndex(IDXGISwapChain3* swapChain) {
    return g_realGetCurrentBackBufferIndex
               ? g_realGetCurrentBackBufferIndex(swapChain)
               : swapChain->GetCurrentBackBufferIndex();
}

HRESULT PresentWithOverlay(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if ((flags & DXGI_PRESENT_TEST) != 0) {
        return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
    }

    ID3D12CommandQueue* cmdQueue = g_gameCommandQueue.load();
    if (!cmdQueue) {
        return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
    }

    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        if (!InitD3D12(swapChain)) {
            return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
        }
        g_initialized = true;
    }

    bool profilerNeeded = g_showMenu || (g_config.showOsd && g_config.showLatencyOsd);
    telemetry::SetProfilerActive(profilerNeeded);
    telemetry::OnRealFramePresent(cmdQueue);

    bool drawUi = (g_showMenu || g_config.showOsd);
    if (!drawUi) {
        telemetry::OnSwapChainPresent();
        return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_showMenu;
    if (g_showMenu) {
        io.ConfigFlags |= (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    } else {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderOsdBadge();
    RenderMainWindow();

    ImGui::Render();

    UINT bbIdx = QueryBackBufferIndex(swapChain);
    if (bbIdx < g_renderTargets.size() && g_renderTargets[bbIdx] && bbIdx < g_frameContexts.size()) {
        FrameContext& frameCtx = g_frameContexts[bbIdx];
        if (frameCtx.fenceValue != 0 && g_fence->GetCompletedValue() < frameCtx.fenceValue) {
            g_fence->SetEventOnCompletion(frameCtx.fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }

        frameCtx.commandAllocator->Reset();
        g_commandList->Reset(frameCtx.commandAllocator.Get(), nullptr);

        RecordImGuiOnBackBuffer(bbIdx);

        ExecuteOverlayCommandList(cmdQueue, frameCtx);
    }

    telemetry::OnSwapChainPresent();
    return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags) {
    return PresentWithOverlay(swapChain, syncInterval, flags, nullptr);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain3* swapChain, UINT syncInterval, UINT presentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    return PresentWithOverlay(swapChain, syncInterval, presentFlags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
    OnPreResize();
    HRESULT hr = g_realResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
    OnPostResize(swapChain);
    return hr;
}

}  // namespace

void TryInstall() {
    if (g_hooked.load()) return;
    auto dx12 = game::IsDx12();
    if (!dx12 || !*dx12) return;
    void* sc = game::SwapChain();
    if (!sc) return;

    std::lock_guard lock(g_mutex);
    if (g_hooked.load()) return;

    auto* swapChain = static_cast<IDXGISwapChain3*>(sc);

    // Query D3D12 device from swapchain
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        antilag::Init(g_device.Get());
        // Create a temporary DIRECT queue to obtain the virtual table of ID3D12CommandQueue
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> dummyQueue;
        if (SUCCEEDED(g_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&dummyQueue)))) {
            void** qVtbl = *reinterpret_cast<void***>(dummyQueue.Get());
            if (!g_realExecuteCommandLists) {
                g_realExecuteCommandLists = reinterpret_cast<ExecuteCommandListsFn>(
                    mem::WritePointer(&qVtbl[10], reinterpret_cast<void*>(&HookExecuteCommandLists))
                );
                LOGI("overlay: hooked ID3D12CommandQueue::ExecuteCommandLists (vtable[10])");
            }
        }
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    g_realPresent = reinterpret_cast<PresentFn>(mem::WritePointer(&vtable[8], reinterpret_cast<void*>(&HookPresent)));
    g_realResizeBuffers = reinterpret_cast<ResizeBuffersFn>(mem::WritePointer(&vtable[13], reinterpret_cast<void*>(&HookResizeBuffers)));
    g_realPresent1 = reinterpret_cast<Present1Fn>(mem::WritePointer(&vtable[22], reinterpret_cast<void*>(&HookPresent1)));
    g_realGetCurrentBackBufferIndex = reinterpret_cast<GetCurrentBackBufferIndexFn>(
        mem::WritePointer(&vtable[36], reinterpret_cast<void*>(&HookGetCurrentBackBufferIndex))
    );

    g_hooked.store(true);
    LOGI("MHW-FSR: DirectX 12 SwapChain hooked successfully for in-game overlay");
}

void OnPreResize() {
    std::lock_guard lock(g_mutex);
    WaitForGpu();
    for (auto& rt : g_renderTargets) {
        rt.Reset();
    }
}

void OnPostResize(IDXGISwapChain3* swapChain) {
    std::lock_guard lock(g_mutex);
    if (!g_device) return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_bufferCount = desc.BufferCount;
    g_swapChainWidth = desc.BufferDesc.Width;
    g_swapChainHeight = desc.BufferDesc.Height;
    g_swapChainFormat = desc.BufferDesc.Format;

    if (!g_rtvHeap || g_bufferCount > g_rtvCapacity) {
        g_rtvCapacity = std::max(g_bufferCount, kMinRtvDescriptors);
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = g_rtvCapacity;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        g_rtvHeap.Reset();
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) {
            LOGE("overlay: cannot recreate RTV descriptor heap on resize");
            return;
        }
    }

    if (g_bufferCount > g_frameContexts.size()) {
        size_t oldSize = g_frameContexts.size();
        g_frameContexts.resize(g_bufferCount);
        for (size_t i = oldSize; i < g_bufferCount; ++i) {
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContexts[i].commandAllocator)))) {
                LOGE("overlay: cannot create command allocator on resize for index {}", i);
            }
            g_frameContexts[i].fenceValue = 0;
        }
    }

    CreateRenderTargetViews(swapChain);

    if (g_hwnd) {
        if (g_wndProcChain.CheckAndRepairIfWrapped(g_hwnd, &HookWndProc)) {
            LOGW("overlay: detected re-wrapped WndProc on resize (e.g. SPL); repaired chain to class WndProc");
        }
    }
}

bool IsVisible() {
    return g_showMenu;
}

void SetVisible(bool visible) {
    g_showMenu = visible;
}

}  // namespace mhwfsr::overlay
