#pragma once

namespace mhwdlss {
struct Config;
}

namespace mhwdlss::reflex {

// NVIDIA Reflex through NVAPI: Low Latency mode, its frame rate cap and the latency markers. The
// game's main thread runs one frame per sMhMain update: frame rate limiter, then the render thread
// is started on the previous frame, then input and simulation, then the wait for the render thread
// and the Present of the previous frame. So Reflex sleeps at the start of that update, and the
// markers of frame N come from the main thread (simulation) and, one update later, from the render
// thread start and end and the Present. Works with or without DLSS; needs game::Init's hooks.
void Install();
// Takes the Reflex settings from `config` (after every ini load; applied at the next frame).
void Configure(const Config& config);

}  // namespace mhwdlss::reflex
