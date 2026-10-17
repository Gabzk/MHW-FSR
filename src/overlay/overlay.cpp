#include "overlay.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "antilag.h"
#include "config.h"
#include "d3d_util.h"
#include "framegen.h"
#include "fsr_context.h"
#include "fsr_loader.h"
#include "game.h"
#include "log.h"
#include "memory.h"
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
WNDPROC g_origWndProc = nullptr;

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

    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    style.WindowPadding = ImVec2(14.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);

    // Deep Obsidian Dark Base
    colors[ImGuiCol_Text]                  = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.09f, 0.09f, 0.11f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.12f, 0.14f, 0.80f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.11f, 0.11f, 0.13f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.24f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Dark Card Elements
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.08f, 0.10f, 0.85f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.13f, 0.13f, 0.16f, 1.00f);

    // AMD FidelityFX Ruby Red Accents
    colors[ImGuiCol_CheckMark]             = ImVec4(0.92f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.85f, 0.15f, 0.22f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.78f, 0.14f, 0.20f, 0.88f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.92f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(1.00f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.78f, 0.14f, 0.20f, 0.65f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.88f, 0.16f, 0.22f, 0.85f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.98f, 0.22f, 0.28f, 1.00f);

    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.22f, 0.26f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.85f, 0.15f, 0.22f, 0.85f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.95f, 0.20f, 0.28f, 1.00f);

    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.85f, 0.15f, 0.22f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.95f, 0.20f, 0.28f, 0.75f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(1.00f, 0.28f, 0.35f, 1.00f);

    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.85f, 0.15f, 0.22f, 0.80f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.80f, 0.14f, 0.20f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
}

LRESULT CALLBACK HookWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
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
    // Double the frame resources so rendering ImGui on both Interpolated and Real frames never collides
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
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));

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
        if (initialVer.find("FSR") == std::string::npos && initialVer.find("fsr") == std::string::npos) {
            initialVer = "FSR " + initialVer;
        }
        SetActiveFsrVersion(initialVer);
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

void RenderMainWindow() {
    if (!g_showMenu) return;

    ImGui::SetNextWindowSize(ImVec2(590.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("MHW-FSR :: AMD FidelityFX Control Center", &g_showMenu, flags)) {
        bool dlssOn = game::DlssEnabledInGame();
        float currentScale = game::RenderScale();
        telemetry::Metrics m = telemetry::GetMetrics();

        // Header Status Banner
        ImGui::BeginChild("StatusHeader", ImVec2(0, 72), true);
        {
            if (dlssOn) {
                ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "[ACTIVE] %s", ActiveFsrVersion().c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("Scale: %.1f%% (%s)", currentScale * 100.0f, QualityModeName(g_config.mode));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "[PAUSED] UPSCALING PAUSED");
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("Enable 'NVIDIA DLSS' in game Display Settings to activate");
            }

            float baseFps = m.baseFps > 0.1f ? m.baseFps : ImGui::GetIO().Framerate;
            float dispFps = m.presentedFps > 0.1f ? m.presentedFps : baseFps;
            float dispDt = m.presentedFrameTimeMs > 0.1f ? m.presentedFrameTimeMs : (1000.0f / std::max(1.0f, dispFps));

            ImGui::Text("Framerate: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "%.1f FPS (%.2f ms)", dispFps, dispDt);
            ImGui::SameLine();
            ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            ImGui::Text("PC Latency: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f), "%.1f ms", m.inputToPresentMs);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        if (ImGui::BeginTabBar("FSRTabs")) {
            // Tab 1: Upscaling & Quality
            if (ImGui::BeginTabItem("Upscaling & Resolution")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "QUALITY PRESET");

                const char* presets[] = {
                    "NativeAA (1.0x)",
                    "Quality (1.5x)",
                    "Balanced (1.7x)",
                    "Performance (2.0x)",
                    "Ultra Performance (3.0x)",
                    "Custom"
                };

                int currentPreset = static_cast<int>(g_config.mode);
                if (ImGui::Combo("Preset", &currentPreset, presets, IM_ARRAYSIZE(presets))) {
                    g_config.mode = static_cast<QualityMode>(currentPreset);
                    game::ApplyConfig(g_config);
                    game::RecomputeRenderScale();
                }

                if (g_config.mode == QualityMode::Custom) {
                    ImGui::Spacing();
                    if (ImGui::SliderFloat("Custom Scale", &g_config.customScale, 0.50f, 1.00f, "%.2fx")) {
                        game::ApplyConfig(g_config);
                        game::RecomputeRenderScale();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "RCAS SHARPNESS");
                if (ImGui::SliderFloat("Sharpness", &g_config.sharpness, 0.0f, 1.0f, "%.2f")) {
                    game::ApplyConfig(g_config);
                }

                // Quick Sharpness Buttons
                if (ImGui::Button("Off (0.0)")) { g_config.sharpness = 0.0f; game::ApplyConfig(g_config); }
                ImGui::SameLine();
                if (ImGui::Button("Subtle (0.33)")) { g_config.sharpness = 0.33f; game::ApplyConfig(g_config); }
                ImGui::SameLine();
                if (ImGui::Button("Moderate (0.66)")) { g_config.sharpness = 0.66f; game::ApplyConfig(g_config); }
                ImGui::SameLine();
                if (ImGui::Button("Crisp (1.0)")) { g_config.sharpness = 1.0f; game::ApplyConfig(g_config); }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "ON-SCREEN DISPLAY (OSD)");
                ImGui::Checkbox("Show Corner Overlay (OSD)", &g_config.showOsd);
                ImGui::SameLine();
                ImGui::Checkbox("Show FPS Counter", &g_config.showFps);

                ImGui::EndTabItem();
            }

            // Tab 2: FidelityFX Core
            if (ImGui::BeginTabItem("FidelityFX Core")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "UPSCALER ARCHITECTURE");

                bool hasFsr4 = loader::HasFsr4Support(g_device.Get());

                std::vector<const char*> versions;
                versions.push_back("Auto (Recommended)");
                if (hasFsr4) {
                    versions.push_back("Force FSR 4.1.1 (DirectML)");
                }
                versions.push_back("Force FSR 3.1.5 (Temporal)");

                int currentVerIdx = 0;
                if (g_config.version == "FSR 4" || g_config.version == "4.1" || g_config.version == "FSR4") {
                    currentVerIdx = hasFsr4 ? 1 : 0;
                } else if (g_config.version == "FSR 3.1" || g_config.version == "3.1.5" || g_config.version == "FSR3") {
                    currentVerIdx = hasFsr4 ? 2 : 1;
                }

                if (ImGui::Combo("Engine Version", &currentVerIdx, versions.data(), static_cast<int>(versions.size()))) {
                    if (hasFsr4) {
                        if (currentVerIdx == 1) g_config.version = "FSR 4";
                        else if (currentVerIdx == 2) g_config.version = "FSR 3.1";
                        else g_config.version = "Auto";
                    } else {
                        if (currentVerIdx == 1) g_config.version = "FSR 3.1";
                        else g_config.version = "Auto";
                    }
                    game::ApplyConfig(g_config);
                }

                if (!hasFsr4) {
                    ImGui::TextDisabled("Note: FSR 4.1.1 is unavailable on this GPU (requires RDNA 3 / DirectML CS 6.6).");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "ADVANCED RENDERING");
                if (ImGui::Checkbox("Dynamic Auto Exposure", &g_config.autoExposure)) {
                    game::ApplyConfig(g_config);
                }
                ImGui::TextDisabled("Allows FSR to compute its own exposure luminance buffer dynamically.");

                ImGui::Spacing();
                if (ImGui::Checkbox("Reflection History Filter Fix", &g_config.reflectionFix)) {
                    game::ApplyConfig(g_config);
                }
                ImGui::TextDisabled("Fixes 2x2 grid jitter artifacts on screen-space water/wet reflections.");

                ImGui::EndTabItem();
            }

            // Tab 3: Anti-Lag 2 & Latency
            if (ImGui::BeginTabItem("Anti-Lag 2 & Latency")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "AMD FIDELITYFX ANTI-LAG 2");

                bool supported = antilag::IsSupported();
                bool active = antilag::IsActive();

                if (supported) {
                    if (active) {
                        ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "Driver Status: Active (amdxc64.dll)");
                    } else {
                        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Driver Status: Ready (Disabled)");
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "Driver Status: Unavailable (Requires AMD Radeon GPU)");
                }

                ImGui::Spacing();
                if (ImGui::Checkbox("Enable Anti-Lag 2", &g_config.antiLag2)) {
                    antilag::Update(g_config.antiLag2, static_cast<uint32_t>(g_config.antiLagMaxFps));
                }
                ImGui::TextDisabled("Synchronizes CPU input polling with GPU execution to minimize click-to-photon latency.");

                if (active) {
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.50f, 1.0f),
                                       "AL2 Driver Sync Wait: %.2f ms  |  Input-to-Present Latency: %.1f ms",
                                       m.antiLagWaitMs, m.inputToPresentMs);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "DRIVER FRAME PACING");
                if (ImGui::SliderInt("Driver Max FPS", &g_config.antiLagMaxFps, 0, 240, g_config.antiLagMaxFps == 0 ? "Unlimited" : "%d FPS")) {
                    antilag::Update(g_config.antiLag2, static_cast<uint32_t>(g_config.antiLagMaxFps));
                }
                ImGui::TextDisabled("Driver-level frame rate limiter. Ensures stable frame pacing without queue bloat.");

                ImGui::EndTabItem();
            }

            // Tab 4: Diagnostics & Telemetry Profiler
            if (ImGui::BeginTabItem("Diagnostics & Profiler")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "LATENCY & PERFORMANCE PROFILER (OPTISCALER STYLE)");

                ImGui::BeginChild("ProfilerBox", ImVec2(0, 150), true);
                {
                    ImGui::Text("Upscaler Pass (%s):", ActiveFsrVersion().c_str());
                    ImGui::SameLine(280);
                    ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "%.2f ms (GPU)  /  %.2f ms (CPU)", m.upscaleGpuMs, m.upscaleCpuMs);

                    ImGui::Text("Anti-Lag 2 CPU Pacing Delay:");
                    ImGui::SameLine(280);
                    if (antilag::IsActive()) {
                        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.50f, 1.0f), "%.2f ms (amdxc64 sync)", m.antiLagWaitMs);
                    } else {
                        ImGui::TextDisabled("Disabled");
                    }

                    ImGui::Text("GPU Render Queue Latency:");
                    ImGui::SameLine(280);
                    ImGui::Text("%.2f ms", m.gpuQueueLatencyMs);

                    ImGui::Text("PC Latency (Input-to-Present):");
                    ImGui::SameLine(280);
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f), "%.1f ms", m.inputToPresentMs);

                    ImGui::Separator();

                    size_t histOffset = 0;
                    const auto& hist = telemetry::GetFrameTimeHistory(histOffset);
                    char overlayLabel[64];
                    std::snprintf(overlayLabel, sizeof(overlayLabel), "Presented Frame Time: %.2f ms (%.1f FPS)",
                                  m.presentedFrameTimeMs, m.presentedFps);
                    ImGui::PlotLines("##FrameTimePlot", hist.data(), static_cast<int>(hist.size()),
                                     static_cast<int>(histOffset), overlayLabel, 0.0f, 33.3f, ImVec2(-1, 48));
                }
                ImGui::EndChild();

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.92f, 0.18f, 0.24f, 1.0f), "OVERLAY & VISUAL COMPARISON");
                ImGui::Checkbox("Show Corner Watermark (OSD)", &g_config.showOsd);
                ImGui::SameLine();
                ImGui::Checkbox("Show FPS Counter", &g_config.showFps);
                ImGui::SameLine();
                ImGui::Checkbox("Show Latency Breakdown", &g_config.showLatencyOsd);

                if (ImGui::Checkbox("Bypass FSR (Show Vanilla Capcom TAA)", &g_config.bypass)) {
                    game::ApplyConfig(g_config);
                }

                ImGui::Spacing();
                ImGui::TextDisabled("Jitter Telemetry: %s", game::JitterStats().c_str());

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Footer Actions
        if (ImGui::Button("Save to MHW-FSR.ini", ImVec2(180, 32))) {
            SaveConfig(g_config);
            g_saveFeedback = "Settings saved to MHW-FSR.ini!";
            g_saveFeedbackTime = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();
        if (ImGui::Button("Close Menu [F10]", ImVec2(140, 32))) {
            g_showMenu = false;
        }

        if (!g_saveFeedback.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - g_saveFeedbackTime).count() < 3) {
                ImGui::SameLine();
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

    static uint64_t s_lastFrame = 0;
    uint64_t frame = game::Frame();
    if (frame != 0 && frame == s_lastFrame) {
        return CallRealPresent(swapChain, syncInterval, flags, pPresentParameters);
    }
    s_lastFrame = frame;

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

bool IsInstalled() {
    return g_hooked.load();
}

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

void OnPresent(IDXGISwapChain3* /*swapChain*/) {
    // Handled directly inside PresentWithOverlay
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
}

bool IsVisible() {
    return g_showMenu;
}

void SetVisible(bool visible) {
    g_showMenu = visible;
}

}  // namespace mhwfsr::overlay
