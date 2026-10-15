#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace mhwfsr::overlay {

// Checks whether the in-game overlay has attached to the game's swapchain
bool IsInstalled();

// Tries to hook the DX12 swapchain if available from game::SwapChain()
void TryInstall();

// Frame callbacks called from HookPresent / HookResizeBuffers
void OnPresent(IDXGISwapChain3* swapChain);
void OnPreResize();
void OnPostResize(IDXGISwapChain3* swapChain);

// Overlay visibility control
bool IsVisible();
void SetVisible(bool visible);

}  // namespace mhwfsr::overlay
