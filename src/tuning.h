#pragma once

// Input conventions and behaviours MT Framework (MHW 15.2x) requires for FSR to work. They were
// once ini keys; switching any of them off breaks or degrades the image, so they are fixed here.
namespace mhwfsr::tuning {

// The game uses reversed-Z (1 = near, 0 = far).
constexpr bool kDepthInverted = true;

// Motion vectors are in render pixels, current -> previous; FSR wants them as a fraction of the
// render size with the Y axis flipped.
constexpr float kMotionScaleX = 0.5f;
constexpr float kMotionScaleY = -0.5f;

// The camera jitter is in clip units (+-1 spans the render target); FSR wants render pixels.
constexpr float kJitterScaleX = 0.5f;
constexpr float kJitterScaleY = -0.5f;

constexpr float kPreExposure = 1.0f;

}  // namespace mhwfsr::tuning
