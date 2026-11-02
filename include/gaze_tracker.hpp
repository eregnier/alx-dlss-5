#pragma once

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <atomic>
#include "../external/openvr/headers/openvr.h"
#include "hlx_config.hpp"
#include "hlx_logger.hpp"

namespace hlx {

struct EyeGazeState {
    // Temporal filter state (Cheeky GazeTemporalPolicyState)
    double lastUpdateSeconds = 0.0;
    double lastValidSeconds = 0.0;
    float filteredU = 0.5f;
    float filteredV = 0.5f;
    float returnStartU = 0.5f;
    float returnStartV = 0.5f;
    bool hasFiltered = false;
    bool returningToFixed = false;
    bool wasUsingValidGaze = false;

    // Quantized crop placement & saccade reset state (Cheeky GazeCropPolicyState)
    uint32_t lastFoveaX = 0;
    uint32_t lastFoveaY = 0;
    uint32_t lastFoveaW = 0;
    uint32_t lastFoveaH = 0;
    bool hasLastCrop = false;
};

struct GazeFramePlacement {
    uint32_t foveaX = 0;
    uint32_t foveaY = 0;
    float resolvedU = 0.5f;
    float resolvedV = 0.5f;
    bool usingGaze = false;
    bool needsHistoryReset = false;
};

class GazeTracker {
public:
    static constexpr double GAZE_HOLD_SECONDS = 0.100;   // Hold last gaze during ~100ms blinks
    static constexpr double GAZE_RETURN_SECONDS = 0.150; // Smooth 150ms return to optical axis on tracking loss

    static GazeTracker& Instance() {
        static GazeTracker instance;
        return instance;
    }

    // Convert OpenVR NDC [-1, +1] (Y-up) to texture UV [0, 1] (V-down)
    // Exact formula from Cheeky openvr_gaze_math.hpp: openvr_ndc_center
    static bool NdcToUv(float ndcX, float ndcY, float& outU, float& outV) noexcept {
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;
        outU = (ndcX + 1.0f) * 0.5f;
        outV = (1.0f - ndcY) * 0.5f;
        return std::isfinite(outU) && std::isfinite(outV) &&
               outU >= -0.25f && outU <= 1.25f &&
               outV >= -0.25f && outV <= 1.25f;
    }

    // Project a head-space 3D gaze ray into per-eye UV coordinates using GetEyeToHeadTransform and GetProjectionRaw
    // Exact formula from Cheeky openvr_gaze_math.hpp: openvr_project_direction
    static bool ProjectHeadRayToEyeUv(
        const float eyeToHead[3][4],
        float left, float right, float top, float bottom,
        const float ray[3],
        float& outU, float& outV
    ) noexcept {
        float d[3] = { 0.0f, 0.0f, 0.0f };
        for (unsigned i = 0; i < 3; ++i) {
            for (unsigned j = 0; j < 3; ++j) {
                d[i] += eyeToHead[j][i] * ray[j];
            }
        }
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom) ||
            right <= left || bottom <= top || d[2] >= -0.0001f) {
            return false;
        }
        outU = (d[0] / -d[2] - left) / (right - left);
        outV = (-d[1] / -d[2] - top) / (bottom - top);
        return std::isfinite(outU) && std::isfinite(outV);
    }

    // Quantize fovea start coordinate to a pixel grid (e.g. 8px for U-Net macroblock stability)
    // with hysteresis so micro-tremor near a bucket boundary never oscillates.
    static uint32_t QuantizedCenterStart(
        float centerNorm,
        uint32_t capacity,
        uint32_t size,
        uint32_t quantum,
        uint32_t previousStart,
        bool hasPrevious
    ) noexcept {
        if (size >= capacity) return 0u;
        const uint32_t available = capacity - size;
        const double clampedCenter = static_cast<double>((std::clamp)(centerNorm, 0.0f, 1.0f));
        const int64_t desired = static_cast<int64_t>(std::llround(
            clampedCenter * static_cast<double>(capacity) - static_cast<double>(size) * 0.5
        ));
        uint32_t rawStart = static_cast<uint32_t>((std::clamp<int64_t>)(desired, 0, static_cast<int64_t>(available)));

        const uint32_t safeQuantum = (std::max)(quantum, 2u) & ~1u; // Always at least 2-pixel aligned
        // Hysteresis deadband: only move to a new quantized bucket if rawStart moved by > 60% of safeQuantum
        if (hasPrevious && previousStart <= available) {
            const uint32_t diff = (rawStart > previousStart) ? (rawStart - previousStart) : (previousStart - rawStart);
            const uint32_t hysteresisThreshold = (std::max)(2u, (safeQuantum * 3u) / 5u);
            if (diff < hysteresisThreshold) {
                return previousStart & ~1u;
            }
        }

        uint32_t quantized = ((rawStart + safeQuantum / 2u) / safeQuantum) * safeQuantum;
        quantized = (std::min)(available, quantized) & ~1u;
        return quantized;
    }

    // Poll OpenVR eye tracker once per stereo frame (on Left Eye submission)
    void PollFrameGaze(vr::IVRSystem* pVRSystem, const Config& config) noexcept {
        LARGE_INTEGER qpcNow{}, qpcFreq{};
        QueryPerformanceCounter(&qpcNow);
        QueryPerformanceFrequency(&qpcFreq);
        m_nowSeconds = (qpcFreq.QuadPart > 0)
            ? (static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpcFreq.QuadPart))
            : 0.0;

        m_rawValid[0] = false;
        m_rawValid[1] = false;

        if (!config.eye_tracking || !pVRSystem) {
            m_hardwareActive.store(false, std::memory_order_relaxed);
            return;
        }

        // Primary path: OpenVR 2.x IVRSystem::GetEyeTrackedFoveationCenter
        // Supported natively by SteamVR, Pimax Play (Crystal / Crystal Super), Varjo, Quest Pro, Vive Pro Eye, etc.
        vr::HmdVector2_t ndc[2] = {};
        if (pVRSystem->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1])) {
            float u0 = 0.5f, v0 = 0.5f;
            float u1 = 0.5f, v1 = 0.5f;
            if (NdcToUv(ndc[0].v[0], ndc[0].v[1], u0, v0) &&
                NdcToUv(ndc[1].v[0], ndc[1].v[1], u1, v1)) {
                m_rawU[0] = (std::clamp)(u0, 0.0f, 1.0f);
                m_rawV[0] = (std::clamp)(v0, 0.0f, 1.0f);
                m_rawU[1] = (std::clamp)(u1, 0.0f, 1.0f);
                m_rawV[1] = (std::clamp)(v1, 0.0f, 1.0f);
                m_rawValid[0] = true;
                m_rawValid[1] = true;
                m_hardwareActive.store(true, std::memory_order_relaxed);
                if (!m_loggedFirstAcquisition) {
                    m_loggedFirstAcquisition = true;
                    HLX_INFO("[EYE-TRACKING] Active hardware gaze detected via IVRSystem::GetEyeTrackedFoveationCenter! L=(%.3f, %.3f) R=(%.3f, %.3f)",
                        m_rawU[0], m_rawV[0], m_rawU[1], m_rawV[1]);
                }
                return;
            }
        }

        // Secondary path: CheekyOpenXRLayer.dll snapshot if loaded in the process
        if (PollCheekyOpenXRLayer()) {
            m_hardwareActive.store(true, std::memory_order_relaxed);
            return;
        }

        // Check if hardware property reports support even during transient blink
        vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
        bool propSupported = pVRSystem->GetBoolTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd,
            vr::Prop_SupportsXrEyeGazeInteraction_Bool,
            &propErr
        );
        m_hardwareSupported.store(propSupported && propErr == vr::TrackedProp_Success, std::memory_order_relaxed);
    }

    // Resolve the smoothed & quantized fovea subrect for a given eye
    GazeFramePlacement ResolveEyePlacement(
        uint32_t eyeIdx,
        uint32_t eyeWidth,
        uint32_t eyeHeight,
        uint32_t foveaWidth,
        uint32_t foveaHeight,
        float fallbackU,
        float fallbackV,
        const Config& config
    ) noexcept {
        GazeFramePlacement out = {};
        uint32_t idx = eyeIdx & 1u;
        auto& st = m_eyes[idx];

        float resolvedU = fallbackU;
        float resolvedV = fallbackV;
        bool usingGaze = false;
        bool reacquired = false;

        if (!config.eye_tracking) {
            st = {};
        } else if (m_rawValid[idx]) {
            const float rawU = m_rawU[idx];
            const float rawV = m_rawV[idx];
            if (!st.hasFiltered) {
                st.filteredU = rawU;
                st.filteredV = rawV;
                st.hasFiltered = true;
            } else {
                const double dt = (std::max)(m_nowSeconds - st.lastUpdateSeconds, 0.0);
                const double tau = (std::max)(static_cast<double>(config.gaze_smoothing_ms), 0.0) / 1000.0;
                const double alpha = (tau <= 0.0) ? 1.0 : (1.0 - std::exp(-dt / tau));
                st.filteredU += static_cast<float>(alpha * (rawU - st.filteredU));
                st.filteredV += static_cast<float>(alpha * (rawV - st.filteredV));
            }
            st.lastUpdateSeconds = m_nowSeconds;
            st.lastValidSeconds = m_nowSeconds;
            reacquired = st.returningToFixed || !st.wasUsingValidGaze;
            st.returningToFixed = false;
            st.wasUsingValidGaze = true;
            resolvedU = st.filteredU;
            resolvedV = st.filteredV;
            usingGaze = true;
        } else if (st.hasFiltered) {
            const double elapsed = (std::max)(m_nowSeconds - st.lastValidSeconds, 0.0);
            if (elapsed <= GAZE_HOLD_SECONDS) {
                // Blink hold: keep last valid gaze position steady
                resolvedU = st.filteredU;
                resolvedV = st.filteredV;
                usingGaze = true;
            } else {
                if (!st.returningToFixed) {
                    st.returningToFixed = true;
                    st.returnStartU = st.filteredU;
                    st.returnStartV = st.filteredV;
                }
                const double amount = (std::clamp)((elapsed - GAZE_HOLD_SECONDS) / GAZE_RETURN_SECONDS, 0.0, 1.0);
                resolvedU = static_cast<float>(st.returnStartU + amount * (fallbackU - st.returnStartU));
                resolvedV = static_cast<float>(st.returnStartV + amount * (fallbackV - st.returnStartV));
                usingGaze = amount < 1.0;
                if (amount >= 1.0) {
                    st.wasUsingValidGaze = false;
                    m_hardwareActive.store(false, std::memory_order_relaxed);
                }
            }
        }

        const uint32_t quantum = usingGaze
            ? static_cast<uint32_t>((std::clamp)(config.gaze_quantization_pixels, 2, 32))
            : 2u;

        out.foveaX = QuantizedCenterStart(
            resolvedU, eyeWidth, foveaWidth, quantum, st.lastFoveaX, st.hasLastCrop && usingGaze);
        out.foveaY = QuantizedCenterStart(
            resolvedV, eyeHeight, foveaHeight, quantum, st.lastFoveaY, st.hasLastCrop && usingGaze);
        out.resolvedU = resolvedU;
        out.resolvedV = resolvedV;
        out.usingGaze = usingGaze;

        // Evaluate Cheeky saccade / jump reset policy
        if (!st.hasLastCrop && usingGaze) {
            out.needsHistoryReset = true;
        } else if (reacquired) {
            out.needsHistoryReset = true;
        } else if (st.hasLastCrop && (st.lastFoveaW != foveaWidth || st.lastFoveaH != foveaHeight)) {
            out.needsHistoryReset = true;
        } else if (st.hasLastCrop) {
            const uint32_t dx = (out.foveaX > st.lastFoveaX) ? (out.foveaX - st.lastFoveaX) : (st.lastFoveaX - out.foveaX);
            const uint32_t dy = (out.foveaY > st.lastFoveaY) ? (out.foveaY - st.lastFoveaY) : (st.lastFoveaY - out.foveaY);
            const float ratio = (std::clamp)(config.gaze_jump_reset_ratio, 0.10f, 1.0f);
            const uint32_t threshX = (std::max)(64u, static_cast<uint32_t>(std::lround(foveaWidth * ratio)));
            const uint32_t threshY = (std::max)(64u, static_cast<uint32_t>(std::lround(foveaHeight * ratio)));
            if (dx >= threshX || dy >= threshY) {
                out.needsHistoryReset = true;
            }
        }

        st.lastFoveaX = out.foveaX;
        st.lastFoveaY = out.foveaY;
        st.lastFoveaW = foveaWidth;
        st.lastFoveaH = foveaHeight;
        st.hasLastCrop = true;

        return out;
    }

    // Used for testing or external injection of gaze coordinates
    void InjectTestGaze(float leftU, float leftV, float rightU, float rightV, bool valid, double nowSec) noexcept {
        m_nowSeconds = nowSec;
        m_rawU[0] = leftU;
        m_rawV[0] = leftV;
        m_rawU[1] = rightU;
        m_rawV[1] = rightV;
        m_rawValid[0] = valid;
        m_rawValid[1] = valid;
        m_hardwareActive.store(valid, std::memory_order_relaxed);
    }

    void Reset() noexcept {
        m_eyes[0] = {};
        m_eyes[1] = {};
        m_rawValid[0] = false;
        m_rawValid[1] = false;
        m_hardwareActive.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsHardwareGazeActive() const noexcept {
        return m_hardwareActive.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsHardwareGazeSupported() const noexcept {
        return m_hardwareActive.load(std::memory_order_relaxed) ||
               m_hardwareSupported.load(std::memory_order_relaxed);
    }

private:
    GazeTracker() = default;

    // Optional interop with CheekyOpenXRLayer.dll if loaded in the process
    struct CheekyGazeViewV1 {
        uint32_t structure_size;
        uint32_t view_index;
        uint32_t flags;
        float center_u;
        float center_v;
        uint8_t padding[128];
    };
    struct CheekyGazeSnapshotV1 {
        uint32_t abi_version;
        uint32_t structure_size;
        uint32_t status_flags;
        uint32_t view_count;
        uint64_t session_generation;
        uint64_t swapchain_generation;
        int64_t predicted_display_time;
        int64_t sample_time;
        uint64_t publication_qpc;
        char runtime_name[64];
        CheekyGazeViewV1 views[2];
    };
    using PFN_CheekyOpenXR_GetGazeSnapshot = uint32_t(__cdecl*)(uint32_t, void*, uint32_t);

    bool PollCheekyOpenXRLayer() noexcept {
        HMODULE hMod = GetModuleHandleW(L"CheekyOpenXRLayer.dll");
        if (!hMod) return false;
        auto pfn = reinterpret_cast<PFN_CheekyOpenXR_GetGazeSnapshot>(
            GetProcAddress(hMod, "CheekyOpenXR_GetGazeSnapshot"));
        if (!pfn) return false;
        CheekyGazeSnapshotV1 snap = {};
        if (pfn(1u, &snap, sizeof(snap)) == 0u) return false;
        // CHEEKY_GAZE_STATUS_GAZE_VALID = 0x04
        if ((snap.status_flags & 0x04u) == 0u || snap.view_count < 2u) return false;
        if (!std::isfinite(snap.views[0].center_u) || !std::isfinite(snap.views[0].center_v) ||
            !std::isfinite(snap.views[1].center_u) || !std::isfinite(snap.views[1].center_v)) {
            return false;
        }
        m_rawU[0] = (std::clamp)(snap.views[0].center_u, 0.0f, 1.0f);
        m_rawV[0] = (std::clamp)(snap.views[0].center_v, 0.0f, 1.0f);
        m_rawU[1] = (std::clamp)(snap.views[1].center_u, 0.0f, 1.0f);
        m_rawV[1] = (std::clamp)(snap.views[1].center_v, 0.0f, 1.0f);
        m_rawValid[0] = true;
        m_rawValid[1] = true;
        return true;
    }

    EyeGazeState m_eyes[2] = {};
    float m_rawU[2] = { 0.5f, 0.5f };
    float m_rawV[2] = { 0.5f, 0.5f };
    bool m_rawValid[2] = { false, false };
    double m_nowSeconds = 0.0;
    bool m_loggedFirstAcquisition = false;
    std::atomic<bool> m_hardwareActive{ false };
    std::atomic<bool> m_hardwareSupported{ false };
};

} // namespace hlx
