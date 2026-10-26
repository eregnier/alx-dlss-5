#pragma once

#include <windows.h>
#include <cstdint>
#include "../external/openvr/headers/openvr.h"
#include "hlx_logger.hpp"

namespace hlx {

// Type definition for IVRCompositor::Submit
// EVRCompositorError Submit( EVREye eEye, const Texture_t *pTexture, const VRTextureBounds_t *pBounds = 0, EVRSubmitFlags nSubmitFlags = Submit_Default )
using PFN_VRCompositor_Submit = vr::EVRCompositorError(*)(
    void* thisPtr,
    vr::EVREye eEye,
    const vr::Texture_t* pTexture,
    const vr::VRTextureBounds_t* pBounds,
    vr::EVRSubmitFlags nSubmitFlags
);

// Type definition for IVRSystem::GetRecommendedRenderTargetSize
// virtual void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) = 0;
using PFN_VRSystem_GetRecommendedRenderTargetSize = void(*)(
    void* thisPtr,
    uint32_t* pnWidth,
    uint32_t* pnHeight
);

class VTableHook {
public:
    VTableHook() = default;
    ~VTableHook() { Restore(); }

    bool Install(void** vtableEntry, void* detourFunction, void** originalTrampoline) {
        if (!vtableEntry || !detourFunction) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(vtableEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            HLX_ERROR("Failed to VirtualProtect vtable entry (%p)", vtableEntry);
            return false;
        }

        original_ = *vtableEntry;
        detour_ = detourFunction;
        slot_ = vtableEntry;

        if (originalTrampoline) {
            *originalTrampoline = original_;
        }

        // Atomically replace slot
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot_), detour_);

        DWORD temp = 0;
        VirtualProtect(vtableEntry, sizeof(void*), oldProtect, &temp);

        HLX_INFO("VTableHook successfully installed at slot %p (Original: %p -> Detour: %p)",
            vtableEntry, original_, detour_);
        return true;
    }

    void Restore() {
        if (slot_ && original_) {
            DWORD oldProtect = 0;
            if (VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot_), original_, detour_);
                DWORD temp = 0;
                VirtualProtect(slot_, sizeof(void*), oldProtect, &temp);
                HLX_INFO("VTableHook restored at slot %p", slot_);
            }
            slot_ = nullptr;
            original_ = nullptr;
            detour_ = nullptr;
        }
    }

    [[nodiscard]] bool IsInstalled() const noexcept { return slot_ != nullptr; }

private:
    void** slot_ = nullptr;
    void* original_ = nullptr;
    void* detour_ = nullptr;
};

} // namespace hlx
