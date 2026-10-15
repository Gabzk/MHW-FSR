#pragma once

#include <d3d12.h>
#include <cstdint>

namespace mhwfsr::antilag {

// Initialize Anti-Lag 2 with the D3D12 device. Safe to call multiple times (no-op if already initialized).
bool Init(ID3D12Device* device);

// Shutdown Anti-Lag 2 context cleanly.
void Shutdown();

// Called once per frame right before user input is polled (main thread).
void Update(bool enabled, uint32_t maxFps);

// Called when the main frame rendering commands have been submitted to the GPU (render thread).
void MarkEndOfFrameRendering();

// Called on the presentation thread right before Present.
// bInterpolatedFrame = true for FSR FG generated frames, false for real frames.
void SetFrameGenFrameType(bool bInterpolatedFrame);

// Returns true if the AMD driver interface (amdxc64.dll) is available and initialized.
bool IsSupported();

// Returns true if Anti-Lag 2 is currently active.
bool IsActive();

}  // namespace mhwfsr::antilag
