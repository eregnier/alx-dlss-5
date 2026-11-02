#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <chrono>
#include "../external/openvr/headers/openvr.h"
#include "hlx_logger.hpp"
#include "hlx_config.hpp"

namespace hlx {

struct EyeStats {
    uint64_t totalFrames = 0;
    uint64_t enhancedFrames = 0;
    uint64_t fallbackFrames = 0;
    double lastGpuTimeMs = 0.0;
};

struct EyeContext {
    static constexpr uint32_t MAX_PASSES = 3;

    uint32_t eyeIndex = 0; // 0 = Left, 1 = Right
    void* nrFeature = nullptr;
    void* nrFeatures[MAX_PASSES] = { nullptr, nullptr, nullptr };
    
    uint32_t width = 0;       // Neural evaluate width (workW = foveaWidth * working_scale)
    uint32_t height = 0;      // Neural evaluate height (workH = foveaHeight * working_scale)
    
    // Full display extent (eyeWidth x eyeHeight)
    uint32_t fullWidth = 0;
    uint32_t fullHeight = 0;

    // Foveated sweetspot subrect inside full eye
    uint32_t foveaX = 0;
    uint32_t foveaY = 0;
    uint32_t foveaWidth = 0;
    uint32_t foveaHeight = 0;
    float centerU = 0.5f;
    float centerV = 0.5f;
    uint32_t lastDrsWidth = 0;
    uint32_t lastDrsHeight = 0;

    // D3D11 resources
    ID3D11Texture2D* sharedInputTex11 = nullptr;       // Fovea/proxy input to D3D12 (size width x height)
    ID3D11Texture2D* sharedOutputTex11 = nullptr;      // Fovea/proxy neural output from D3D12 (size width x height)
    ID3D11Texture2D* fullNativeTex11 = nullptr;        // Fovea subrect native capture from game (size foveaWidth x foveaHeight)
    ID3D11Texture2D* fullCompositedTex11 = nullptr;    // Fovea subrect blended result (size foveaWidth x foveaHeight)

    // D3D11 views for compositing pass
    ID3D11ShaderResourceView* nativeSrv11 = nullptr;
    ID3D11ShaderResourceView* neuralSrv11 = nullptr;
    ID3D11ShaderResourceView* inputSrv11 = nullptr;    // SRV on sharedInputTex11 (low-res input proxy for residual transfer)
    ID3D11UnorderedAccessView* inputUav11 = nullptr;   // write target of the fovea downsample
    ID3D11UnorderedAccessView* compositedUav11 = nullptr;
    
    // D3D12 resources opened from shared handles + multi-pass ping-pong buffer
    ID3D12Resource* d3d12Input = nullptr;
    ID3D12Resource* d3d12Output = nullptr;
    ID3D12Resource* d3d12Interm = nullptr;
    
    // D3D12 guides for DLSS-NR
    ID3D12Resource* depthRes = nullptr;
    ID3D12Resource* motionRes = nullptr;

    // Per-eye isolated D3D12 command list & allocator (Cheeky transport)
    ID3D12CommandAllocator* cmdAllocator = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    uint64_t lastDoneValue = 0;

    bool isInitialized = false;
    bool needsReset = true;
    EyeStats stats;

    void Release() {
        if (compositedUav11) { compositedUav11->Release(); compositedUav11 = nullptr; }
        if (inputUav11) { inputUav11->Release(); inputUav11 = nullptr; }
        if (inputSrv11) { inputSrv11->Release(); inputSrv11 = nullptr; }
        if (neuralSrv11) { neuralSrv11->Release(); neuralSrv11 = nullptr; }
        if (nativeSrv11) { nativeSrv11->Release(); nativeSrv11 = nullptr; }
        if (fullCompositedTex11) { fullCompositedTex11->Release(); fullCompositedTex11 = nullptr; }
        if (fullNativeTex11) { fullNativeTex11->Release(); fullNativeTex11 = nullptr; }
        if (cmdList) { cmdList->Release(); cmdList = nullptr; }
        if (cmdAllocator) { cmdAllocator->Release(); cmdAllocator = nullptr; }
        lastDoneValue = 0;
        if (depthRes) { depthRes->Release(); depthRes = nullptr; }
        if (motionRes) { motionRes->Release(); motionRes = nullptr; }
        if (d3d12Interm) { d3d12Interm->Release(); d3d12Interm = nullptr; }
        if (d3d12Output) { d3d12Output->Release(); d3d12Output = nullptr; }
        if (d3d12Input) { d3d12Input->Release(); d3d12Input = nullptr; }
        if (sharedOutputTex11) { sharedOutputTex11->Release(); sharedOutputTex11 = nullptr; }
        if (sharedInputTex11) { sharedInputTex11->Release(); sharedInputTex11 = nullptr; }
        width = 0;
        height = 0;
        fullWidth = 0;
        fullHeight = 0;
        foveaX = 0;
        foveaY = 0;
        foveaWidth = 0;
        foveaHeight = 0;
        centerU = 0.5f;
        centerV = 0.5f;
        lastDrsWidth = 0;
        lastDrsHeight = 0;
        isInitialized = false;
        needsReset = true;
    }
};

class StereoPipeline {
public:
    static StereoPipeline& Instance() {
        static StereoPipeline instance;
        return instance;
    }

    void Init(const Config& config) {
        config_ = config;
        for (uint32_t i = 0; i < 2; ++i) {
            eyes_[i].eyeIndex = i;
            eyes_[i].stats = {};
            eyes_[i].isInitialized = false;
            eyes_[i].needsReset = true;
        }
        HLX_INFO("StereoPipeline initialized with dual-eye isolation.");
    }

    EyeContext& GetEye(uint32_t eyeIndex) {
        return eyes_[eyeIndex & 1];
    }

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return config_;
    }

    void UpdateConfig(const Config& config) {
        const bool geomChanged =
            (config.foveated != config_.foveated) ||
            (std::abs(config.foveated_width - config_.foveated_width) > 1e-4f) ||
            (std::abs(config.foveated_height - config_.foveated_height) > 1e-4f) ||
            (std::abs(config.working_scale - config_.working_scale) > 1e-4f) ||
            (config.passes != config_.passes);
        config_ = config;
        if (geomChanged) {
            for (uint32_t i = 0; i < 2; ++i) {
                eyes_[i].isInitialized = false;
                eyes_[i].needsReset = true;
            }
        }
    }

    void RecordFrame(uint32_t eyeIndex, bool enhanced, double gpuTimeMs) {
        auto& eye = eyes_[eyeIndex & 1];
        eye.stats.totalFrames++;
        if (enhanced) {
            eye.stats.enhancedFrames++;
        } else {
            eye.stats.fallbackFrames++;
        }
        eye.stats.lastGpuTimeMs = gpuTimeMs;
    }

    void ResetEyeHistory(uint32_t eyeIndex) {
        eyes_[eyeIndex & 1].needsReset = true;
        HLX_INFO("Eye %u temporal history reset.", eyeIndex);
    }

    void ResetAllEyesHistory() {
        eyes_[0].needsReset = true;
        eyes_[1].needsReset = true;
    }

    void Shutdown() {
        for (uint32_t i = 0; i < 2; ++i) {
            eyes_[i].Release();
        }
    }

private:
    StereoPipeline() = default;
    ~StereoPipeline() { Shutdown(); }

    Config config_;
    EyeContext eyes_[2];
};

} // namespace hlx
