#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
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
    uint32_t eyeIndex = 0; // 0 = Left, 1 = Right
    void* nrFeature = nullptr;
    
    uint32_t width = 0;       // Neural evaluate width (foveaWidth)
    uint32_t height = 0;      // Neural evaluate height (foveaHeight)
    
    // Full display extent (eyeWidth x eyeHeight)
    uint32_t fullWidth = 0;
    uint32_t fullHeight = 0;

    // Foveated sweetspot subrect inside full eye
    uint32_t foveaX = 0;
    uint32_t foveaY = 0;
    uint32_t foveaWidth = 0;
    uint32_t foveaHeight = 0;

    // D3D11 resources
    ID3D11Texture2D* sharedInputTex11 = nullptr;       // Fovea input to D3D12 (size foveaWidth x foveaHeight)
    ID3D11Texture2D* sharedOutputTex11 = nullptr;      // Fovea neural output from D3D12 (size foveaWidth x foveaHeight)
    ID3D11Texture2D* fullNativeTex11 = nullptr;        // Full eye capture from game (size fullWidth x fullHeight)
    ID3D11Texture2D* fullCompositedTex11 = nullptr;    // Full eye blended result (size fullWidth x fullHeight)

    // D3D11 views for compositing pass
    ID3D11ShaderResourceView* nativeSrv11 = nullptr;
    ID3D11ShaderResourceView* neuralSrv11 = nullptr;
    ID3D11UnorderedAccessView* compositedUav11 = nullptr;
    
    // D3D12 resources opened from shared handles
    ID3D12Resource* d3d12Input = nullptr;
    ID3D12Resource* d3d12Output = nullptr;
    
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
        if (neuralSrv11) { neuralSrv11->Release(); neuralSrv11 = nullptr; }
        if (nativeSrv11) { nativeSrv11->Release(); nativeSrv11 = nullptr; }
        if (fullCompositedTex11) { fullCompositedTex11->Release(); fullCompositedTex11 = nullptr; }
        if (fullNativeTex11) { fullNativeTex11->Release(); fullNativeTex11 = nullptr; }
        if (cmdList) { cmdList->Release(); cmdList = nullptr; }
        if (cmdAllocator) { cmdAllocator->Release(); cmdAllocator = nullptr; }
        lastDoneValue = 0;
        if (depthRes) { depthRes->Release(); depthRes = nullptr; }
        if (motionRes) { motionRes->Release(); motionRes = nullptr; }
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
        config_ = config;
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
