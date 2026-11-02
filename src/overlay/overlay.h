#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace mhwfsr::overlay {

// Tries to hook the DX12 swapchain if available from game::SwapChain()
void TryInstall();

// Frame callbacks called from HookResizeBuffers
void OnPreResize();
void OnPostResize(IDXGISwapChain3* swapChain);

// Overlay visibility control
bool IsVisible();
void SetVisible(bool visible);

}  // namespace mhwfsr::overlay
