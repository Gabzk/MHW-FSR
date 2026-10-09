#include "ngx_params.h"

#include <nvsdk_ngx_params.h>

namespace mhwdlss {

void ShimParameters::Store(const char* name, const Value& v) {
    if (!name) return;
    std::lock_guard lock(mutex_);
    values_[name] = v;
}

bool ShimParameters::Lookup(const char* name, Value* out) const {
    if (!name) return false;
    std::lock_guard lock(mutex_);
    auto it = values_.find(name);
    if (it == values_.end()) return false;
    *out = it->second;
    return true;
}

void ShimParameters::Set(const char* name, unsigned long long value) {
    Value v{Type::U64};
    v.u64 = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, float value) {
    Value v{Type::F32};
    v.f32 = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, double value) {
    Value v{Type::F64};
    v.f64 = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, unsigned int value) {
    Value v{Type::U32};
    v.u32 = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, int value) {
    Value v{Type::I32};
    v.i32 = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, ID3D11Resource* value) {
    Value v{Type::D3D11};
    v.ptr = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, ID3D12Resource* value) {
    Value v{Type::D3D12};
    v.ptr = value;
    Store(name, v);
}
void ShimParameters::Set(const char* name, void* value) {
    Value v{Type::Ptr};
    v.ptr = value;
    Store(name, v);
}

NVSDK_NGX_Result ShimParameters::Get(const char* name, unsigned long long* value) const {
    Value v;
    if (!value || !Lookup(name, &v) || !ToNumber(v, value)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, float* value) const {
    Value v;
    if (!value || !Lookup(name, &v) || !ToNumber(v, value)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, double* value) const {
    Value v;
    if (!value || !Lookup(name, &v) || !ToNumber(v, value)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, unsigned int* value) const {
    Value v;
    if (!value || !Lookup(name, &v) || !ToNumber(v, value)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, int* value) const {
    Value v;
    if (!value || !Lookup(name, &v) || !ToNumber(v, value)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, ID3D11Resource** value) const {
    Value v;
    if (!value || !Lookup(name, &v) || (v.type != Type::D3D11 && v.type != Type::Ptr))
        return NVSDK_NGX_Result_Fail;
    *value = static_cast<ID3D11Resource*>(v.ptr);
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, ID3D12Resource** value) const {
    Value v;
    if (!value || !Lookup(name, &v) || (v.type != Type::D3D12 && v.type != Type::Ptr))
        return NVSDK_NGX_Result_Fail;
    *value = static_cast<ID3D12Resource*>(v.ptr);
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result ShimParameters::Get(const char* name, void** value) const {
    Value v;
    if (!value || !Lookup(name, &v) ||
        (v.type != Type::Ptr && v.type != Type::D3D11 && v.type != Type::D3D12))
        return NVSDK_NGX_Result_Fail;
    *value = v.ptr;
    return NVSDK_NGX_Result_Success;
}

void ShimParameters::Reset() {
    std::lock_guard lock(mutex_);
    values_.clear();
}

unsigned int ShimParameters::GetUInt(const char* name, unsigned int def) const {
    unsigned int v = def;
    return Get(name, &v) == NVSDK_NGX_Result_Success ? v : def;
}
int ShimParameters::GetInt(const char* name, int def) const {
    int v = def;
    return Get(name, &v) == NVSDK_NGX_Result_Success ? v : def;
}
float ShimParameters::GetFloat(const char* name, float def) const {
    float v = def;
    return Get(name, &v) == NVSDK_NGX_Result_Success ? v : def;
}
void* ShimParameters::GetPointer(const char* name) const {
    void* v = nullptr;
    return Get(name, &v) == NVSDK_NGX_Result_Success ? v : nullptr;
}

void ShimParameters::CopyNumbersTo(NVSDK_NGX_Parameter* other) const {
    std::lock_guard lock(mutex_);
    for (const auto& [name, v] : values_) {
        switch (v.type) {
            case Type::U64: other->Set(name.c_str(), v.u64); break;
            case Type::F32: other->Set(name.c_str(), v.f32); break;
            case Type::F64: other->Set(name.c_str(), v.f64); break;
            case Type::U32: other->Set(name.c_str(), v.u32); break;
            case Type::I32: other->Set(name.c_str(), v.i32); break;
            case Type::D3D11: other->Set(name.c_str(), static_cast<ID3D11Resource*>(v.ptr)); break;
            case Type::D3D12: other->Set(name.c_str(), static_cast<ID3D12Resource*>(v.ptr)); break;
            case Type::Ptr: break;  // callbacks belong to whoever created the object
        }
    }
}

}  // namespace mhwdlss
