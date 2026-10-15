#pragma once

#include <cstdint>

#define NVSDK_CONV __cdecl

typedef enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = NVSDK_NGX_Result_Fail | 1,
    NVSDK_NGX_Result_FAIL_PlatformError = NVSDK_NGX_Result_Fail | 2,
    NVSDK_NGX_Result_FAIL_FeatureAlreadyExists = NVSDK_NGX_Result_Fail | 3,
    NVSDK_NGX_Result_FAIL_FeatureNotFound = NVSDK_NGX_Result_Fail | 4,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall = NVSDK_NGX_Result_Fail | 6,
    NVSDK_NGX_Result_FAIL_NotInitialized = NVSDK_NGX_Result_Fail | 7,
    NVSDK_NGX_Result_FAIL_UnsupportedInputFormat = NVSDK_NGX_Result_Fail | 8,
    NVSDK_NGX_Result_FAIL_RWFlagMissing = NVSDK_NGX_Result_Fail | 9,
    NVSDK_NGX_Result_FAIL_MissingInput = NVSDK_NGX_Result_Fail | 10,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = NVSDK_NGX_Result_Fail | 11,
    NVSDK_NGX_Result_FAIL_OutOfDate = NVSDK_NGX_Result_Fail | 12,
    NVSDK_NGX_Result_FAIL_OutOfGPUMemory = NVSDK_NGX_Result_Fail | 13,
    NVSDK_NGX_Result_FAIL_UnsupportedFormat = NVSDK_NGX_Result_Fail | 14,
    NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath = NVSDK_NGX_Result_Fail | 15,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = NVSDK_NGX_Result_Fail | 16,
    NVSDK_NGX_Result_FAIL_Denied = NVSDK_NGX_Result_Fail | 17,
    NVSDK_NGX_Result_FAIL_NotImplemented = NVSDK_NGX_Result_Fail | 18,
} NVSDK_NGX_Result;

#define NVSDK_NGX_SUCCEED(value) (((value) & 0xFFF00000) != NVSDK_NGX_Result_Fail)
#define NVSDK_NGX_FAILED(value) (((value) & 0xFFF00000) == NVSDK_NGX_Result_Fail)

typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_Reserved0             = 0,
    NVSDK_NGX_Feature_SuperSampling         = 1,
    NVSDK_NGX_Feature_InPainting            = 2,
    NVSDK_NGX_Feature_ImageSuperResolution  = 3,
    NVSDK_NGX_Feature_SlowMotion            = 4,
    NVSDK_NGX_Feature_VideoSuperResolution  = 5,
    NVSDK_NGX_Feature_ImageSignalProcessing = 9,
    NVSDK_NGX_Feature_DeepResolve           = 10,
    NVSDK_NGX_Feature_FrameGeneration       = 11,
    NVSDK_NGX_Feature_DeepDVC               = 12,
    NVSDK_NGX_Feature_RayReconstruction     = 13,
    NVSDK_NGX_Feature_Reserved_Unknown      = 32766
} NVSDK_NGX_Feature;

struct NVSDK_NGX_Handle {
    unsigned int Id = 0;
};

typedef enum NVSDK_NGX_Version {
    NVSDK_NGX_Version_API = 0x13
} NVSDK_NGX_Version;

struct ID3D11Resource;
struct ID3D12Resource;

// Pure virtual interface matching NVIDIA NGX 1.x / 2.x parameter object vtable layout.
struct NVSDK_NGX_Parameter {
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;

    virtual void Reset() = 0;
};

inline void NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* name, unsigned int val) {
    if (p) p->Set(name, val);
}

// Common parameter names used by Monster Hunter: World
#define NVSDK_NGX_Parameter_Width "Width"
#define NVSDK_NGX_Parameter_Height "Height"
#define NVSDK_NGX_Parameter_OutWidth "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight "OutHeight"
#define NVSDK_NGX_Parameter_Sharpness "Sharpness"
#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_Reset "Reset"
#define NVSDK_NGX_Parameter_MotionVectors "MotionVectors"
#define NVSDK_NGX_Parameter_Depth "Depth"
#define NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback "DLSSOptimalSettingsCallback"
#define NVSDK_NGX_Parameter_DLSSMode "DLSSMode"
#define NVSDK_NGX_Parameter_SuperSampling_Available "SuperSampling.Available"
#define NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver "SuperSampling.NeedsUpdatedDriver"
#define NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor "SuperSampling.MinDriverVersionMajor"
#define NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor "SuperSampling.MinDriverVersionMinor"
#define NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult "SuperSampling.FeatureInitResult"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width "DLSS.Get.Dynamic.Max.Render.Width"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height "DLSS.Get.Dynamic.Max.Render.Height"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width "DLSS.Get.Dynamic.Min.Render.Width"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height "DLSS.Get.Dynamic.Min.Render.Height"
