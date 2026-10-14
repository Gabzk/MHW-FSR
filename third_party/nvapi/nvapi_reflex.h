// The part of NVIDIA's NVAPI SDK that NVIDIA Reflex needs: basic types, the sleep mode, sleep
// status, latency marker and latency report structures, and the function ids that
// nvapi_QueryInterface takes. Copied from nvapi_lite_common.h, nvapi.h and nvapi_interface.h of
// https://github.com/NVIDIA/nvapi (main branch, October 2026); the structure sizes are checked
// against the full header below.
//
// SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#pragma once

#include <cstdint>

#pragma pack(push, 8)  // as nvapi.h

typedef uint8_t NvU8;
typedef uint32_t NvU32;
typedef int32_t NvS32;
typedef uint64_t NvU64;
typedef NvU8 NvBool;

#define MAKE_NVAPI_VERSION(typeName, ver) (NvU32)(sizeof(typeName) | ((ver) << 16))

typedef int NvAPI_Status;
constexpr NvAPI_Status NVAPI_OK = 0;

// nvapi_interface.h
constexpr NvU32 kNvApiId_Initialize = 0x0150e828;
constexpr NvU32 kNvApiId_D3D_GetSleepStatus = 0xaef96ca1;
constexpr NvU32 kNvApiId_D3D_SetSleepMode = 0xac1ca9e0;
constexpr NvU32 kNvApiId_D3D_Sleep = 0x852cd1d2;
constexpr NvU32 kNvApiId_D3D_GetLatency = 0x1a587f9c;
constexpr NvU32 kNvApiId_D3D_SetLatencyMarker = 0xd9984c05;

typedef struct _NV_GET_SLEEP_STATUS_PARAMS
{
    NvU32  version;                                       //!< (IN) Structure version
    NvBool bLowLatencyMode;                               //!< (OUT) Is low latency mode enabled?
    NvBool bFsVrr;                                        //!< (OUT) Is fullscreen VRR enabled?
    NvBool bCplVsyncOn;                                   //!< (OUT) Is Control Panel overriding VSYNC ON?
    NvU32  sleepIntervalUs;                               //!< (OUT) Latest sleep interval in microseconds.
    NvBool bUseGameSleep;                                 //!< (OUT) Is NvAPI_D3D_Sleep() being called?
    NvBool bFullscreenIFlip;                              //!< (OUT) Is in fullscreen/iFlip mode?
    NvU8   fgMultiplier;                                  //!< (OUT) Latest Frame Generation multiplier used.
    NvBool bDfgControl;                                   //!< (OUT) Dynamic Frame Generation control active.
    NvU32  dfgFrameTimeTargetUs;                          //!< (OUT) Dynamic Frame Generation frame time target in microseconds.
    NvU8   rsvd[114];                                     //!< (IN) Reserved. Must be set to 0s.
} NV_GET_SLEEP_STATUS_PARAMS_V1;
#define NV_GET_SLEEP_STATUS_PARAMS_VER1          MAKE_NVAPI_VERSION(NV_GET_SLEEP_STATUS_PARAMS_V1, 1)

typedef struct _NV_SET_SLEEP_MODE_PARAMS
{
    NvU32  version;                                       //!< (IN) Structure version
    NvBool bLowLatencyMode;                               //!< (IN) Low latency mode enable/disable.
    NvBool bLowLatencyBoost;                              //!< (IN) Request maximum GPU clock frequency regardless of workload.
    NvU32  minimumIntervalUs;                             //!< (IN) Minimum frame interval in microseconds. 0 = no frame rate limit.
    NvBool bUseMarkersToOptimize;                         //!< (IN) Allow latency markers to be used for runtime optimizations.
    NvBool bUseMinQueueTime;                              //!< (IN) Consider all submission overlaps (both in between frames and within each frame).
    NvU8   rsvd[30];                                      //!< (IN) Reserved. Must be set to 0s.
} NV_SET_SLEEP_MODE_PARAMS_V1;
#define NV_SET_SLEEP_MODE_PARAMS_VER1          MAKE_NVAPI_VERSION(NV_SET_SLEEP_MODE_PARAMS_V1, 1)

typedef struct _NV_LATENCY_RESULT_PARAMS
{
    NvU32  version;                                       //!< (IN) Structure version
    struct FrameReport {
        NvU64 frameID;                                    //!< (OUT)
        NvU64 inputSampleTime;                            //!< (OUT)
        NvU64 simStartTime;                               //!< (OUT)
        NvU64 simEndTime;                                 //!< (OUT)
        NvU64 renderSubmitStartTime;                      //!< (OUT)
        NvU64 renderSubmitEndTime;                        //!< (OUT)
        NvU64 presentStartTime;                           //!< (OUT)
        NvU64 presentEndTime;                             //!< (OUT)
        NvU64 driverStartTime;                            //!< (OUT)
        NvU64 driverEndTime;                              //!< (OUT)
        NvU64 osRenderQueueStartTime;                     //!< (OUT)
        NvU64 osRenderQueueEndTime;                       //!< (OUT)
        NvU64 gpuRenderStartTime;                         //!< (OUT)
        NvU64 gpuRenderEndTime;                           //!< (OUT)
        NvU32 gpuActiveRenderTimeUs;                      //!< (OUT) Difference between gpuRenderStartTime and gpuRenderEndTime, excluding the idles in between, in microseconds.
        NvU32 gpuFrameTimeUs;                             //!< (OUT) Difference between previous and current frame's gpuRenderEndTime, in microseconds.
        NvU64 cameraConstructedTime;                      //!< (OUT)
        NvU32 crossAdapterCopyTimeUs;                     //!< (OUT) Cross adapter copy time in microseconds.
        NvU32 aiFrameTimeUs;                              //!< (OUT) Frame generation time in microseconds.
        NvU8  rsvd[104];
    } frameReport[64];
    NvU8  rsvd[32];
} NV_LATENCY_RESULT_PARAMS_V1;
#define NV_LATENCY_RESULT_PARAMS_VER1          MAKE_NVAPI_VERSION(NV_LATENCY_RESULT_PARAMS_V1, 1)

typedef enum
{
    SIMULATION_START               =  0,
    SIMULATION_END                 =  1,
    RENDERSUBMIT_START             =  2,
    RENDERSUBMIT_END               =  3,
    PRESENT_START                  =  4,
    PRESENT_END                    =  5,
    INPUT_SAMPLE                   =  6,
    TRIGGER_FLASH                  =  7,
    PC_LATENCY_PING                =  8,
    OUT_OF_BAND_RENDERSUBMIT_START =  9,
    OUT_OF_BAND_RENDERSUBMIT_END   = 10,
    OUT_OF_BAND_PRESENT_START      = 11,
    OUT_OF_BAND_PRESENT_END        = 12,
} NV_LATENCY_MARKER_TYPE;

typedef struct _NV_LATENCY_MARKER_PARAMS
{
    NvU32  version;                                       //!< (IN) Structure version
    NvU64  frameID;
    NV_LATENCY_MARKER_TYPE markerType;
    NvU64  rsvd0;
    NvU8   rsvd[56];
} NV_LATENCY_MARKER_PARAMS_V1;
#define NV_LATENCY_MARKER_PARAMS_VER1          MAKE_NVAPI_VERSION(NV_LATENCY_MARKER_PARAMS_V1, 1)

#pragma pack(pop)

// Sizes of the full header's structures (MSVC x64), which the version fields encode.
static_assert(sizeof(NV_GET_SLEEP_STATUS_PARAMS_V1) == 136);
static_assert(sizeof(NV_SET_SLEEP_MODE_PARAMS_V1) == 44);
static_assert(sizeof(NV_LATENCY_RESULT_PARAMS_V1) == 15400);
static_assert(sizeof(NV_LATENCY_MARKER_PARAMS_V1) == 88);
