#pragma once

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace mhwdlss::probe {

// Debug readback of DLSS inputs. Schedule() records copies of the textures (all in
// NON_PIXEL_SHADER_RESOURCE) into readback buffers; Poll() logs statistics a few frames later,
// once the GPU is certainly done with them. Both run on the thread evaluating DLSS.
void Schedule(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* depth,
              ID3D12Resource* output);
void Poll();
bool Pending();

// Dumps a centred crop of `count` consecutive frames, starting `delayMs` after the request (time to
// get back into the game and move the camera): colour luminance to <dir>\frame_<i>.bin (uint32
// width, uint32 height, float[w*h]), motion vectors to motion_<i>.bin (float2) and depth to
// depth_<i>.bin, with the jitter of each frame in frames.txt.
void RequestFrameDump(const wchar_t* dir, int count, unsigned delayMs);
void RecordFrame(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
                 float jitterX, float jitterY);

}  // namespace mhwdlss::probe
