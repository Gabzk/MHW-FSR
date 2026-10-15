#pragma once

#include <d3d12.h>
#include <vector>

namespace mhwfsr::d3d {

class BarrierBatcher {
public:
    explicit BarrierBatcher(ID3D12GraphicsCommandList* cl) : m_cl(cl) {}
    ~BarrierBatcher() { Flush(); }

    BarrierBatcher(const BarrierBatcher&) = delete;
    BarrierBatcher& operator=(const BarrierBatcher&) = delete;

    void Transition(ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES stateBefore,
                    D3D12_RESOURCE_STATES stateAfter,
                    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    void Uav(ID3D12Resource* resource);

    void Flush();

    bool HasPending() const { return !m_barriers.empty(); }

private:
    ID3D12GraphicsCommandList* m_cl = nullptr;
    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
};

}  // namespace mhwfsr::d3d
