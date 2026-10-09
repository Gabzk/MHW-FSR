#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include <nvsdk_ngx.h>

namespace mhwdlss {

// NVSDK_NGX_Parameter implementation handed to the game in place of the one from the
// NGX 1.x driver core. The interface has not changed since NGX 1.x, so the game's calls
// through the vtable (and through its exported NVSDK_NGX_Parameter_* helpers) land here.
class ShimParameters final : public NVSDK_NGX_Parameter {
public:
    ShimParameters() = default;

    void Set(const char* name, unsigned long long value) override;
    void Set(const char* name, float value) override;
    void Set(const char* name, double value) override;
    void Set(const char* name, unsigned int value) override;
    void Set(const char* name, int value) override;
    void Set(const char* name, ID3D11Resource* value) override;
    void Set(const char* name, ID3D12Resource* value) override;
    void Set(const char* name, void* value) override;

    NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const override;
    NVSDK_NGX_Result Get(const char* name, float* value) const override;
    NVSDK_NGX_Result Get(const char* name, double* value) const override;
    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override;
    NVSDK_NGX_Result Get(const char* name, int* value) const override;
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const override;
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override;
    NVSDK_NGX_Result Get(const char* name, void** value) const override;

    void Reset() override;

    // Convenience accessors with defaults, used by the shim.
    unsigned int GetUInt(const char* name, unsigned int def = 0) const;
    int GetInt(const char* name, int def = 0) const;
    float GetFloat(const char* name, float def = 0.0f) const;
    void* GetPointer(const char* name) const;

    // Copies every numeric value into another parameter object (used when forwarding to the
    // legacy NGX core, which wants its own parameter objects).
    void CopyNumbersTo(NVSDK_NGX_Parameter* other) const;

private:
    enum class Type { U64, F32, F64, U32, I32, D3D11, D3D12, Ptr };
    struct Value {
        Type type;
        union {
            unsigned long long u64;
            float f32;
            double f64;
            unsigned int u32;
            int i32;
            void* ptr;
        };
    };

    void Store(const char* name, const Value& v);
    bool Lookup(const char* name, Value* out) const;

    template <typename T>
    static bool ToNumber(const Value& v, T* out) {
        switch (v.type) {
            case Type::U64: *out = static_cast<T>(v.u64); return true;
            case Type::F32: *out = static_cast<T>(v.f32); return true;
            case Type::F64: *out = static_cast<T>(v.f64); return true;
            case Type::U32: *out = static_cast<T>(v.u32); return true;
            case Type::I32: *out = static_cast<T>(v.i32); return true;
            default: return false;
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Value> values_;
};

}  // namespace mhwdlss
