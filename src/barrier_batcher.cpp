#include "barrier_batcher.h"

namespace mhwfsr::d3d {

void BarrierBatcher::Transition(ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES stateBefore,
                                D3D12_RESOURCE_STATES stateAfter,
                                UINT subresource) {
    if (!resource || stateBefore == stateAfter) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = resource;
    b.Transition.StateBefore = stateBefore;
    b.Transition.StateAfter = stateAfter;
    b.Transition.Subresource = subresource;
    m_barriers.push_back(b);
}

void BarrierBatcher::Uav(ID3D12Resource* resource) {
    if (!resource) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = resource;
    m_barriers.push_back(b);
}

void BarrierBatcher::Flush() {
    if (!m_barriers.empty() && m_cl) {
        m_cl->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
        m_barriers.clear();
    }
}

}  // namespace mhwfsr::d3d
