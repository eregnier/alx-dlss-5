#pragma once

#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include "hlx_logger.hpp"
#include "hlx_config.hpp"
#include "stereo_pipeline.hpp"
#include "d3d12_interop.hpp"

namespace hlx {

using PFN_dlssnr_call_create = void*(*)(
    const wchar_t *snippetPath, const wchar_t *dataPath,
    ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
    void *capabilityParams, unsigned int width,
    unsigned int height, int preset, float intensity,
    int style, float localStructure, float localTone,
    float skinStructure, int useAutoMask,
    int uiCorrection
);

using PFN_dlssnr_call_evaluate_v2 = int(*)(
    ID3D12GraphicsCommandList *cmd, void *feature,
    void *capabilityParams, ID3D12Resource *color,
    ID3D12Resource *depth, ID3D12Resource *motion,
    ID3D12Resource *output, unsigned int width,
    unsigned int height, unsigned int guideWidth,
    unsigned int guideHeight, unsigned int motionWidth,
    unsigned int motionHeight, unsigned int depthBaseX,
    unsigned int depthBaseY, unsigned int motionBaseX,
    unsigned int motionBaseY, int depthInverted, int reset,
    float intensity, int style, float localStructure,
    float localTone, float skinStructure, int useAutoMask,
    float mvScaleX, float mvScaleY
);

using PFN_dlssnr_call_release = void(*)(void* feature);
using PFN_dlssnr_call_set_float_slot = void(*)(int slot);
using PFN_dlssnr_call_probe_float = int(*)(void* params);
using PFN_dlssnr_call_error = const char*(*)();

struct NgxParameters {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual uint32_t Get(const char*, unsigned long long*) const = 0;
    virtual uint32_t Get(const char*, float*) const = 0;
    virtual uint32_t Get(const char*, double*) const = 0;
    virtual uint32_t Get(const char*, unsigned int*) const = 0;
    virtual uint32_t Get(const char*, int*) const = 0;
    virtual uint32_t Get(const char*, ID3D11Resource**) const = 0;
    virtual uint32_t Get(const char*, ID3D12Resource**) const = 0;
    virtual uint32_t Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

inline uint32_t NeuralScalingRatioCallback(NgxParameters* parameters) noexcept {
    if (!parameters) return 0xBAD00005u;
    float scale = 1.0f;
    if ((parameters->Get("DLSSNR.Scale", &scale) & 0xFFF00000u) == 0xBAD00000u) scale = 1.0f;
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 1.0f) scale = 1.0f;
    parameters->Set("DLSSNR.ScalingRatio", scale);
    return 1u;
}

using PFN_NVSDK_NGX_D3D12_Init_Ext = uint32_t(*)(uint64_t, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = uint32_t(*)(void**);
using PFN_NVSDK_NGX_D3D12_AllocateParameters = uint32_t(*)(void**);

class DLSS5Bridge {
public:
    static DLSS5Bridge& Instance() {
        static DLSS5Bridge instance;
        return instance;
    }

    bool Init(ID3D12Device* device, const Config& config) {
        if (isInitialized_) return true;
        device_ = device;
        config_ = config;

        // 1. Load official NVIDIA Core DLL for capability parameters
        HKEY hKey = nullptr;
        wchar_t regPath[MAX_PATH] = {};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD pathSize = sizeof(regPath);
            RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(regPath), &pathSize);
            RegCloseKey(hKey);
        }
        std::wstring coreDll = std::wstring(regPath) + L"\\_nvngx.dll";
        hCore_ = LoadLibraryW(coreDll.c_str());
        if (!hCore_) {
            HLX_ERROR("Failed to load driver _nvngx.dll from %ls", coreDll.c_str());
            return false;
        }

        auto pfnInitCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hCore_, "NVSDK_NGX_D3D12_Init_Ext"));
        auto pfnGetCapCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetCapabilityParameters>(GetProcAddress(hCore_, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        auto pfnAllocParams = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(GetProcAddress(hCore_, "NVSDK_NGX_D3D12_AllocateParameters"));

        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        uint32_t initRes = 0;
        if (pfnInitCore) {
            initRes = pfnInitCore(0x24480451ull, tempPath, device_, 0x0000015u, nullptr);
        }
        uint32_t capRes = 0;
        if (pfnGetCapCore) {
            capRes = pfnGetCapCore(&capabilityParams_);
            if ((capRes & 0xFFF00000u) == 0xBAD00000u) {
                capabilityParams_ = nullptr;
            }
        }
        if (!capabilityParams_ && pfnAllocParams) {
            uint32_t allocRes = pfnAllocParams(&capabilityParams_);
            if ((allocRes & 0xFFF00000u) == 0xBAD00000u) {
                capabilityParams_ = nullptr;
            }
        }
        if (capabilityParams_) {
            auto* ngxParams = reinterpret_cast<NgxParameters*>(capabilityParams_);
            ngxParams->Set("DLSSNR.ScalingRatio", 1.0f);
            ngxParams->Set("DLSSNR.Scale", 1.0f);
            ngxParams->Set("DLSSNR.Upscaling", 0u);
            ngxParams->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&NeuralScalingRatioCallback));
            ngxParams->Set("PerfQualityValue", 0u);
        }
        HLX_INFO("Initialized NGX Core capability parameters: %p (init=0x%08X, cap=0x%08X)", capabilityParams_, initRes, capRes);

        // 2. Load nvngx.dll_dlssnr.dll forwarder
        wchar_t modPath[MAX_PATH] = {};
        HMODULE hModThis = nullptr;
        std::wstring moduleForwarder = L"";
        std::wstring moduleNr = L"";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&g_dummyConfigSymbol), &hModThis) && hModThis) {
            GetModuleFileNameW(hModThis, modPath, MAX_PATH);
            wchar_t* slash = wcsrchr(modPath, L'\\');
            if (slash) {
                *(slash + 1) = L'\0';
                moduleForwarder = std::wstring(modPath) + L"nvngx.dll_dlssnr.dll";
                moduleNr = std::wstring(modPath) + L"nvngx_dlssnr.dll";
            }
        }

        std::vector<std::wstring> candidatePaths;
        if (!moduleForwarder.empty()) candidatePaths.push_back(moduleForwarder);
        candidatePaths.push_back(L"nvngx.dll_dlssnr.dll");

        for (const auto& path : candidatePaths) {
            hForwarder_ = LoadLibraryW(path.c_str());
            if (hForwarder_) {
                HLX_INFO("Successfully loaded forwarder from: %ls", path.c_str());
                break;
            }
        }

        if (!hForwarder_) {
            HLX_ERROR("Failed to load nvngx.dll_dlssnr.dll forwarder!");
            return false;
        }

        pfnCreate_ = reinterpret_cast<PFN_dlssnr_call_create>(GetProcAddress(hForwarder_, "dlssnr_call_create"));
        pfnEvaluate_ = reinterpret_cast<PFN_dlssnr_call_evaluate_v2>(GetProcAddress(hForwarder_, "dlssnr_call_evaluate_v2"));
        pfnRelease_ = reinterpret_cast<PFN_dlssnr_call_release>(GetProcAddress(hForwarder_, "dlssnr_call_release"));
        pfnSetFloatSlot_ = reinterpret_cast<PFN_dlssnr_call_set_float_slot>(GetProcAddress(hForwarder_, "dlssnr_call_set_float_slot"));
        pfnCallError_ = reinterpret_cast<PFN_dlssnr_call_error>(GetProcAddress(hForwarder_, "dlssnr_call_error"));

        if (!pfnCreate_ || !pfnEvaluate_) {
            HLX_ERROR("Forwarder missing dlssnr_call_create or dlssnr_call_evaluate_v2!");
            return false;
        }

        if (pfnSetFloatSlot_) {
            pfnSetFloatSlot_(6);
        }

        // 3. Locate DLSS 5 Neural Model nvngx_dlssnr.dll
        std::vector<std::wstring> nrCandidates;
        if (!moduleNr.empty()) nrCandidates.push_back(moduleNr);
        nrCandidates.push_back(L"nvngx_dlssnr.dll");

        for (const auto& path : nrCandidates) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                nrSnippetPath_ = path;
                HLX_INFO("Located DLSS 5 Neural Model: %ls", path.c_str());
                break;
            }
        }

        isInitialized_ = true;
        HLX_INFO("DLSS5Bridge initialized successfully with production forwarder and driver core.");
        return true;
    }

    bool CreateEyeFeature(uint32_t eyeIndex, uint32_t width, uint32_t height) {
        if (!isInitialized_ || !pfnCreate_) return false;

        auto& eye = StereoPipeline::Instance().GetEye(eyeIndex);
        if (eye.nrFeature) {
            if (pfnRelease_) pfnRelease_(eye.nrFeature);
            eye.nrFeature = nullptr;
        }

        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);

        if (capabilityParams_) {
            auto* ngxParams = reinterpret_cast<NgxParameters*>(capabilityParams_);
            ngxParams->Set("Width", width);
            ngxParams->Set("Height", height);
            ngxParams->Set("OutWidth", width);
            ngxParams->Set("OutHeight", height);
            ngxParams->Set("DLSSNR.InputWidth", width);
            ngxParams->Set("DLSSNR.InputHeight", height);
            ngxParams->Set("DLSSNR.OutputWidth", width);
            ngxParams->Set("DLSSNR.OutputHeight", height);
            ngxParams->Set("DLSSNR.Output.Width", width);
            ngxParams->Set("DLSSNR.Output.Height", height);
            ngxParams->Set("DLSSNR.ScalingRatio", 1.0f);
            ngxParams->Set("DLSSNR.Scale", 1.0f);
            ngxParams->Set("DLSSNR.Upscaling", 0u);
            ngxParams->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&NeuralScalingRatioCallback));
        }

        float skinStrength = (config_.skin_structure < 0.0f) ? 1.0f : config_.skin_structure;
        uint32_t numPasses = static_cast<uint32_t>((std::clamp)(config_.passes, 1, 3));

        for (uint32_t p = 0; p < EyeContext::MAX_PASSES; ++p) {
            if (eye.nrFeatures[p] && pfnRelease_) {
                pfnRelease_(eye.nrFeatures[p]);
                eye.nrFeatures[p] = nullptr;
            }
        }
        eye.nrFeature = nullptr;

        for (uint32_t p = 0; p < numPasses; ++p) {
            auto* cmdList = D3D12Interop::Instance().GetCommandList();
            float passLocalTone = (p == 0) ? config_.local_tone : 0.0f;
            void* feature = pfnCreate_(
                nrSnippetPath_.c_str(),
                tempPath,
                device_,
                cmdList,
                capabilityParams_,
                width, height,
                config_.preset,                           // preset [0 - 3]
                static_cast<float>(config_.detail_boost), // intensity [0.0 - 2.0]
                config_.style,                            // style [0 - 2]
                config_.local_structure, passLocalTone, skinStrength,
                config_.auto_mask ? 1 : 0,                // useAutoMask
                0                                         // uiCorrection = 0
            );

            D3D12Interop::Instance().ExecuteAndSync();

            if (!feature) {
                HLX_ERROR("Failed to create DLSS-NR Feature 18 (Pass %u/%u) for Eye %u (%ux%u)", p + 1, numPasses, eyeIndex, width, height);
                if (pfnCallError_) {
                    HLX_ERROR("DLSS-NR Model Error: %s", pfnCallError_());
                }
                for (uint32_t k = 0; k < p; ++k) {
                    if (eye.nrFeatures[k] && pfnRelease_) {
                        pfnRelease_(eye.nrFeatures[k]);
                        eye.nrFeatures[k] = nullptr;
                    }
                }
                return false;
            }
            eye.nrFeatures[p] = feature;
        }

        eye.nrFeature = eye.nrFeatures[0];
        eye.width = width;
        eye.height = height;
        eye.isInitialized = true;
        eye.needsReset = true;
        HLX_INFO("Successfully instantiated %u DLSS 5 Feature 18 pass(es) for Eye %u: %p (%ux%u)", numPasses, eyeIndex, eye.nrFeature, width, height);
        return true;
    }

    bool EvaluateEye(uint32_t eyeIndex, ID3D12GraphicsCommandList* customCmdList = nullptr) {
        if (!isInitialized_ || !pfnEvaluate_) return false;

        auto& eye = StereoPipeline::Instance().GetEye(eyeIndex);
        if (!eye.isInitialized || !eye.nrFeature || !eye.d3d12Input || !eye.d3d12Output || !eye.depthRes || !eye.motionRes) {
            return false;
        }

        auto* cmdList = customCmdList ? customCmdList : D3D12Interop::Instance().GetCommandList();
        int reset = eye.needsReset ? 1 : 0;
        float skinStrength = (config_.skin_structure < 0.0f) ? 1.0f : config_.skin_structure;

        uint32_t numPasses = static_cast<uint32_t>((std::clamp)(config_.passes, 1, 3));
        if (numPasses > 1 && (!eye.d3d12Interm || !eye.nrFeatures[1])) {
            numPasses = 1;
        }
        if (numPasses == 3 && !eye.nrFeatures[2]) {
            numPasses = 2;
        }

        for (uint32_t p = 0; p < numPasses; ++p) {
            ID3D12Resource* inColor = eye.d3d12Input;
            ID3D12Resource* outColor = eye.d3d12Output;
            if (numPasses == 2) {
                inColor  = (p == 0) ? eye.d3d12Input  : eye.d3d12Interm;
                outColor = (p == 0) ? eye.d3d12Interm : eye.d3d12Output;
            } else if (numPasses == 3) {
                if (p == 0)      { inColor = eye.d3d12Input;  outColor = eye.d3d12Output; }
                else if (p == 1) { inColor = eye.d3d12Output; outColor = eye.d3d12Interm; }
                else             { inColor = eye.d3d12Interm; outColor = eye.d3d12Output; }
            }

            float passLocalTone = (p == 0) ? config_.local_tone : 0.0f;
            void* feature = eye.nrFeatures[p] ? eye.nrFeatures[p] : eye.nrFeature;

            int res = pfnEvaluate_(
                cmdList,
                feature,
                capabilityParams_,
                inColor,
                eye.depthRes,
                eye.motionRes,
                outColor,
                eye.width, eye.height,
                eye.width, eye.height,
                eye.width, eye.height,
                0, 0, 0, 0,
                0,             // depthInverted
                reset,
                static_cast<float>(config_.detail_boost),
                config_.style, // style
                config_.local_structure, passLocalTone, skinStrength,
                config_.auto_mask ? 1 : 0, // useAutoMask
                0.0f, 0.0f     // mvScaleX, mvScaleY = 0.0f: disable motion vector warping on dummy/zero vectors
            );

            if (res != 1) {
                if (pfnCallError_) {
                    HLX_ERROR("EvaluateEye %u (Pass %u/%u) failed with error: %s", eyeIndex, p + 1, numPasses, pfnCallError_());
                }
                return false;
            }

            if (p + 1 < numPasses) {
                D3D12_RESOURCE_BARRIER uavBarrier = {};
                uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavBarrier.UAV.pResource = outColor;
                cmdList->ResourceBarrier(1, &uavBarrier);
            }
        }

        eye.needsReset = false;
        return true;
    }

    void ReleaseEyeFeature(uint32_t eyeIndex) {
        auto& eye = StereoPipeline::Instance().GetEye(eyeIndex);
        for (uint32_t p = 0; p < EyeContext::MAX_PASSES; ++p) {
            if (eye.nrFeatures[p] && pfnRelease_) {
                pfnRelease_(eye.nrFeatures[p]);
                eye.nrFeatures[p] = nullptr;
            }
        }
        if (eye.nrFeature && pfnRelease_) {
            eye.nrFeature = nullptr;
        }
    }

    void Shutdown() {
        if (!isInitialized_) return;
        HLX_INFO("DLSS5Bridge shutting down...");
        for (uint32_t i = 0; i < 2; ++i) {
            ReleaseEyeFeature(i);
        }
        isInitialized_ = false;
        HLX_INFO("DLSS5Bridge shutdown complete.");
    }

    void UpdateConfig(const Config& config) {
        int oldPreset = config_.preset;
        config_ = config;
        if (isInitialized_ && oldPreset != config_.preset) {
            HLX_INFO("Preset changed from %d to %d; recreating DLSS 5 features...", oldPreset, config_.preset);
            for (uint32_t i = 0; i < 2; ++i) {
                auto& eye = StereoPipeline::Instance().GetEye(i);
                if (eye.isInitialized && eye.width > 0 && eye.height > 0) {
                    CreateEyeFeature(i, eye.width, eye.height);
                }
            }
        }
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return isInitialized_; }

private:
    DLSS5Bridge() = default;
    ~DLSS5Bridge() { Shutdown(); }

    bool isInitialized_ = false;
    ID3D12Device* device_ = nullptr;
    Config config_;
    HMODULE hCore_ = nullptr;
    HMODULE hForwarder_ = nullptr;
    void* capabilityParams_ = nullptr;
    std::wstring nrSnippetPath_;

    PFN_dlssnr_call_create pfnCreate_ = nullptr;
    PFN_dlssnr_call_evaluate_v2 pfnEvaluate_ = nullptr;
    PFN_dlssnr_call_release pfnRelease_ = nullptr;
    PFN_dlssnr_call_set_float_slot pfnSetFloatSlot_ = nullptr;
    PFN_dlssnr_call_error pfnCallError_ = nullptr;
};

} // namespace hlx
