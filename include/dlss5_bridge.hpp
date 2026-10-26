#pragma once

#include <windows.h>
#include <d3d12.h>
#include <cstdint>
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
using PFN_dlssnr_call_error = const char*(*)();

using PFN_NVSDK_NGX_D3D12_Init_Ext = uint32_t(*)(uint64_t, const wchar_t*, ID3D12Device*, const void*, void*);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = uint32_t(*)(void**);

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

        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        pfnInitCore(0x24480451ull, tempPath, device_, nullptr, nullptr);
        pfnGetCapCore(&capabilityParams_);
        HLX_INFO("Initialized NGX Core capability parameters: %p", capabilityParams_);

        // 2. Load nvngx.dll_dlssnr.dll forwarder
        const wchar_t* candidatePaths[] = {
            L"nvngx.dll_dlssnr.dll",
            L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx.dll_dlssnr.dll",
            L"C:\\code\\dlss5-vr\\dist\\DLSS5-VR-Release\\deps\\nvngx.dll_dlssnr.dll"
        };

        for (const auto* path : candidatePaths) {
            hForwarder_ = LoadLibraryW(path);
            if (hForwarder_) {
                HLX_INFO("Successfully loaded forwarder from: %ls", path);
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
            pfnSetFloatSlot_(1);
        }

        // 3. Locate DLSS 5 Neural Model nvngx_dlssnr.dll
        const wchar_t* nrCandidates[] = {
            L"nvngx_dlssnr.dll",
            L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll",
            L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\nvngx_dlssnr.dll"
        };
        for (const auto* path : nrCandidates) {
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                nrSnippetPath_ = path;
                HLX_INFO("Located DLSS 5 Neural Model: %ls", path);
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

        auto* cmdList = D3D12Interop::Instance().GetCommandList();
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
            config_.local_structure, config_.local_tone, config_.skin_structure,
            config_.auto_mask ? 1 : 0,                // useAutoMask
            0                                         // uiCorrection = 0
        );

        D3D12Interop::Instance().ExecuteAndSync();

        if (!feature) {
            HLX_ERROR("Failed to create DLSS-NR Feature 18 for Eye %u (%ux%u)", eyeIndex, width, height);
            if (pfnCallError_) {
                HLX_ERROR("DLSS-NR Model Error: %s", pfnCallError_());
            }
            return false;
        }

        eye.nrFeature = feature;
        eye.width = width;
        eye.height = height;
        eye.isInitialized = true;
        eye.needsReset = true;
        HLX_INFO("Successfully instantiated DLSS 5 Feature 18 for Eye %u: %p (%ux%u)", eyeIndex, feature, width, height);
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

        int res = pfnEvaluate_(
            cmdList,
            eye.nrFeature,
            capabilityParams_,
            eye.d3d12Input,
            eye.depthRes,
            eye.motionRes,
            eye.d3d12Output,
            eye.width, eye.height,
            eye.width, eye.height,
            eye.width, eye.height,
            0, 0, 0, 0,
            0,             // depthInverted
            reset,
            static_cast<float>(config_.detail_boost),
            config_.style, // style
            config_.local_structure, config_.local_tone, config_.skin_structure,
            config_.auto_mask ? 1 : 0, // useAutoMask
            0.0f, 0.0f     // mvScaleX, mvScaleY = 0.0f: disable motion vector warping on dummy/zero vectors
        );

        if (res == 1) {
            eye.needsReset = false;
            return true;
        } else {
            if (pfnCallError_) {
                HLX_ERROR("EvaluateEye %u failed with error: %s", eyeIndex, pfnCallError_());
            }
            return false;
        }
    }

    void ReleaseEyeFeature(uint32_t eyeIndex) {
        auto& eye = StereoPipeline::Instance().GetEye(eyeIndex);
        if (eye.nrFeature && pfnRelease_) {
            pfnRelease_(eye.nrFeature);
            eye.nrFeature = nullptr;
        }
    }

    void Shutdown() {
        if (!isInitialized_) return;
        HLX_INFO("DLSS5Bridge shutting down...");
        for (uint32_t i = 0; i < 2; ++i) {
            auto& eye = StereoPipeline::Instance().GetEye(i);
            if (eye.nrFeature && pfnRelease_) {
                pfnRelease_(eye.nrFeature);
                eye.nrFeature = nullptr;
            }
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
