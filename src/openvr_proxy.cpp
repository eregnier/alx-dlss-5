#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <xinput.h>
#include "../external/openvr/headers/openvr.h"
#include "../include/hlx_logger.hpp"
#include "../include/hlx_config.hpp"
#include "../include/openvr_hook.hpp"
#include "../include/stereo_pipeline.hpp"
#include "../include/d3d12_interop.hpp"
#include "../include/dlss5_bridge.hpp"
#include "../include/foveated_composite_shader.hpp"
#include "../include/hlx_vr_menu.hpp"
#include "../include/input_combo.hpp"
#include "../include/hlx_gpu_timers.hpp"
#include "../include/gaze_tracker.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace hlx {

static HMODULE g_realOpenVR = nullptr;
static std::mutex g_initMutex;
static VTableHook g_submitHook;
static PFN_VRCompositor_Submit g_originalSubmit = nullptr;

static VTableHook g_renderTargetHook;
static PFN_VRSystem_GetRecommendedRenderTargetSize g_originalGetRecommendedRenderTargetSize = nullptr;

static vr::IVRSystem* g_pVRSystem = nullptr;
static vr::IVRInput* g_pVRInput = nullptr;
static vr::IVROverlay* g_pVROverlay = nullptr;
static std::atomic<uint32_t> g_targetEyeWidth{ 0 };
static std::atomic<uint32_t> g_targetEyeHeight{ 0 };

static VTableHook g_updateActionStateHook;
using PFN_UpdateActionState = vr::EVRInputError(*)(void* thisPtr, vr::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount);
static PFN_UpdateActionState g_originalUpdateActionState = nullptr;
static vr::VRActionSetHandle_t g_hDevActionSet = vr::k_ulInvalidActionSetHandle;

static VTableHook g_getDigitalActionDataHook;
using PFN_GetDigitalActionData = vr::EVRInputError(*)(void* thisPtr, vr::VRActionHandle_t action, vr::InputDigitalActionData_t* pActionData, uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice);
static PFN_GetDigitalActionData g_originalGetDigitalActionData = nullptr;

static VTableHook g_getAnalogActionDataHook;
using PFN_GetAnalogActionData = vr::EVRInputError(*)(void* thisPtr, vr::VRActionHandle_t action, vr::InputAnalogActionData_t* pActionData, uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice);
static PFN_GetAnalogActionData g_originalGetAnalogActionData = nullptr;

// Set to true while CheckInputToggle() reads controller state so our own polling is never muted.
static thread_local bool g_inModInputPoll = false;
// Grace window (steady_clock ms) after closing/toggling the VR menu so releasing buttons never triggers game actions.
static std::atomic<int64_t> g_blockGameInputUntilMs{ 0 };

static int64_t SteadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void ExtendGameInputBlock(int64_t durationMs = 400) noexcept {
    g_blockGameInputUntilMs.store(SteadyNowMs() + durationMs);
}

static bool ShouldBlockGameInput() noexcept {
    return VRMenu::Instance().IsVisible() || (SteadyNowMs() < g_blockGameInputUntilMs.load());
}

static vr::EVRInputError Hooked_GetDigitalActionData(
    void* thisPtr, vr::VRActionHandle_t action,
    vr::InputDigitalActionData_t* pActionData, uint32_t unActionDataSize,
    vr::VRInputValueHandle_t ulRestrictToDevice
) {
    vr::EVRInputError err = g_originalGetDigitalActionData
        ? g_originalGetDigitalActionData(thisPtr, action, pActionData, unActionDataSize, ulRestrictToDevice)
        : vr::VRInputError_None;
    if (!g_inModInputPoll && pActionData && ShouldBlockGameInput()) {
        pActionData->bState = false;
        pActionData->bChanged = false;
    }
    return err;
}

static vr::EVRInputError Hooked_GetAnalogActionData(
    void* thisPtr, vr::VRActionHandle_t action,
    vr::InputAnalogActionData_t* pActionData, uint32_t unActionDataSize,
    vr::VRInputValueHandle_t ulRestrictToDevice
) {
    vr::EVRInputError err = g_originalGetAnalogActionData
        ? g_originalGetAnalogActionData(thisPtr, action, pActionData, unActionDataSize, ulRestrictToDevice)
        : vr::VRInputError_None;
    if (!g_inModInputPoll && pActionData && ShouldBlockGameInput()) {
        pActionData->x = 0.0f;
        pActionData->y = 0.0f;
        pActionData->z = 0.0f;
        pActionData->deltaX = 0.0f;
        pActionData->deltaY = 0.0f;
        pActionData->deltaZ = 0.0f;
    }
    return err;
}

static vr::EVRInputError Hooked_UpdateActionState(void* thisPtr, vr::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount) {
    if (g_pVRInput && g_hDevActionSet == vr::k_ulInvalidActionSetHandle) {
        g_pVRInput->GetActionSetHandle("/actions/dev", &g_hDevActionSet);
        if (g_hDevActionSet != vr::k_ulInvalidActionSetHandle) {
            HLX_INFO("Captured /actions/dev ActionSet handle: %llu", (unsigned long long)g_hDevActionSet);
        }
    }

    constexpr uint32_t MAX_SETS = 32;
    vr::VRActiveActionSet_t combinedSets[MAX_SETS];
    uint32_t count = (std::min)(unSetCount, MAX_SETS - 1);

    for (uint32_t i = 0; i < count; ++i) {
        combinedSets[i] = pSets[i];
    }

    // Append /actions/dev so SteamVR actively evaluates Button Y (/actions/dev/in/ToggleMenu)
    if (g_hDevActionSet != vr::k_ulInvalidActionSetHandle && count < MAX_SETS) {
        combinedSets[count].ulActionSet = g_hDevActionSet;
        combinedSets[count].ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        combinedSets[count].ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
        combinedSets[count].unPadding = 0;
        combinedSets[count].nPriority = 0;
        count++;
    }

    static bool s_loggedActionSets = false;
    if (!s_loggedActionSets) {
        s_loggedActionSets = true;
        HLX_INFO("Hooked_UpdateActionState called by engine with %u action sets (Appended /actions/dev: %d)", unSetCount, (int)(g_hDevActionSet != vr::k_ulInvalidActionSetHandle));
    }

    if (g_originalUpdateActionState) {
        return g_originalUpdateActionState(thisPtr, combinedSets, unSizeOfVRSelectedActionSet_t, count);
    }
    return vr::VRInputError_None;
}

static void Hooked_GetRecommendedRenderTargetSize(void* thisPtr, uint32_t* pnWidth, uint32_t* pnHeight) {
    if (g_originalGetRecommendedRenderTargetSize) {
        g_originalGetRecommendedRenderTargetSize(thisPtr, pnWidth, pnHeight);
    }
    if (pnWidth && pnHeight) {
        uint32_t origW = *pnWidth;
        uint32_t origH = *pnHeight;

        hlx::Config config;
        config.Load();

        float scale = config.working_scale;
        if (!config.foveated && config.enabled && scale > 0.1f && scale < 0.999f) {
            *pnWidth = static_cast<uint32_t>(origW * scale);
            *pnHeight = static_cast<uint32_t>(origH * scale);

            // Ensure dimensions are even numbers (multiples of 2)
            *pnWidth = (*pnWidth + 1) & ~1u;
            *pnHeight = (*pnHeight + 1) & ~1u;

            HLX_INFO("GetRecommendedRenderTargetSize: Original %ux%u -> Scaled (%.2fx) %ux%u",
                origW, origH, scale, *pnWidth, *pnHeight);
        } else {
            HLX_INFO("GetRecommendedRenderTargetSize: Original %ux%u (Foveated=%d, native recommended preserved)",
                origW, origH, config.foveated ? 1 : 0);
        }

        g_targetEyeWidth.store(*pnWidth);
        g_targetEyeHeight.store(*pnHeight);
    }
}

// Function pointer typedefs for OpenVR exports
using PFN_VR_InitInternal = uint32_t(*)(vr::EVRInitError *peError, vr::EVRApplicationType eType);
using PFN_VR_InitInternal2 = uint32_t(*)(vr::EVRInitError *peError, vr::EVRApplicationType eType, const char *pStartupInfo);
using PFN_VR_ShutdownInternal = void(*)();
using PFN_VR_IsHmdPresent = bool(*)();
using PFN_VR_GetGenericInterface = void*(*)(const char *pchInterfaceVersion, vr::EVRInitError *peError);
using PFN_VR_IsRuntimeInstalled = bool(*)();
using PFN_VR_RuntimePath = const char*(*)();
using PFN_VR_GetVRInitErrorAsSymbol = const char*(*)(vr::EVRInitError error);
using PFN_VR_GetVRInitErrorAsEnglishDescription = const char*(*)(vr::EVRInitError error);
using PFN_VR_GetInitToken = uint32_t(*)();
using PFN_VR_IsInterfaceVersionValid = bool(*)(const char *pchInterfaceVersion);
using PFN_VROculusDirect = void*(*)();
using PFN_VR_GetRuntimePath = bool(*)(char*, uint32_t, uint32_t*);
using PFN_LiquidVR = void*(*)();
using PFN_VRControlPanel = void*(*)();
using PFN_VRCompositorSystemInternal = void*(*)();
using PFN_VRHeadsetView = vr::IVRHeadsetView*(*)();
using PFN_VRPaths = void*(*)();
using PFN_VRRenderModelsInternal = void*(*)();
using PFN_VRSceneGraph = void*(*)();
using PFN_VRTrackedCameraInternal = void*(*)();
using PFN_VRVirtualDisplay = void*(*)();
using PFN_VR_GetStringForHmdError = const char*(*)(vr::EVRInitError);

static PFN_VR_InitInternal g_pfnVR_InitInternal = nullptr;
static PFN_VR_InitInternal2 g_pfnVR_InitInternal2 = nullptr;
static PFN_VR_ShutdownInternal g_pfnVR_ShutdownInternal = nullptr;
static PFN_VR_IsHmdPresent g_pfnVR_IsHmdPresent = nullptr;
static PFN_VR_GetGenericInterface g_pfnVR_GetGenericInterface = nullptr;
static PFN_VR_IsRuntimeInstalled g_pfnVR_IsRuntimeInstalled = nullptr;
static PFN_VR_RuntimePath g_pfnVR_RuntimePath = nullptr;
static PFN_VR_GetVRInitErrorAsSymbol g_pfnVR_GetVRInitErrorAsSymbol = nullptr;
static PFN_VR_GetVRInitErrorAsEnglishDescription g_pfnVR_GetVRInitErrorAsEnglishDescription = nullptr;
static PFN_VR_GetInitToken g_pfnVR_GetInitToken = nullptr;
static PFN_VR_IsInterfaceVersionValid g_pfnVR_IsInterfaceVersionValid = nullptr;
static PFN_VROculusDirect g_pfnVROculusDirect = nullptr;
static PFN_VR_GetRuntimePath g_pfnVR_GetRuntimePath = nullptr;
static PFN_LiquidVR g_pfnLiquidVR = nullptr;
static PFN_VRControlPanel g_pfnVRControlPanel = nullptr;
static PFN_VRCompositorSystemInternal g_pfnVRCompositorSystemInternal = nullptr;
static PFN_VRHeadsetView g_pfnVRHeadsetView = nullptr;
static PFN_VRPaths g_pfnVRPaths = nullptr;
static PFN_VRRenderModelsInternal g_pfnVRRenderModelsInternal = nullptr;
static PFN_VRSceneGraph g_pfnVRSceneGraph = nullptr;
static PFN_VRTrackedCameraInternal g_pfnVRTrackedCameraInternal = nullptr;
static PFN_VRVirtualDisplay g_pfnVRVirtualDisplay = nullptr;
static PFN_VR_GetStringForHmdError g_pfnVR_GetStringForHmdError = nullptr;

static bool LoadRealOpenVR() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_realOpenVR) return true;

    // 1. Try openvr_api_real.dll (standard proxy pattern)
    HMODULE hMod = LoadLibraryW(L"openvr_api_real.dll");

    // 2. Query Steam installation directory from Windows Registry
    if (!hMod) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t steamPath[MAX_PATH] = {};
            DWORD valType = 0, valSize = sizeof(steamPath);
            if (RegQueryValueExW(hKey, L"SteamPath", nullptr, &valType, reinterpret_cast<LPBYTE>(steamPath), &valSize) == ERROR_SUCCESS) {
                std::wstring openvrInSteam = std::wstring(steamPath) + L"\\openvr_api.dll";
                std::wstring openvrInSteamVR = std::wstring(steamPath) + L"\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll";
                hMod = LoadLibraryW(openvrInSteam.c_str());
                if (!hMod) hMod = LoadLibraryW(openvrInSteamVR.c_str());
            }
            RegCloseKey(hKey);
        }
    }

    // 3. Fallback to standard Steam paths
    if (!hMod) {
        hMod = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\openvr_api.dll");
    }
    if (!hMod) {
        hMod = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll");
    }

    if (!hMod) {
        HLX_ERROR("Failed to load real openvr_api.dll from any candidate location!");
        return false;
    }

    g_realOpenVR = hMod;
    HLX_INFO("Loaded real openvr_api.dll at %p", g_realOpenVR);

    // Resolve exports
    g_pfnVR_InitInternal = reinterpret_cast<PFN_VR_InitInternal>(GetProcAddress(hMod, "VR_InitInternal"));
    g_pfnVR_InitInternal2 = reinterpret_cast<PFN_VR_InitInternal2>(GetProcAddress(hMod, "VR_InitInternal2"));
    g_pfnVR_ShutdownInternal = reinterpret_cast<PFN_VR_ShutdownInternal>(GetProcAddress(hMod, "VR_ShutdownInternal"));
    g_pfnVR_IsHmdPresent = reinterpret_cast<PFN_VR_IsHmdPresent>(GetProcAddress(hMod, "VR_IsHmdPresent"));
    g_pfnVR_GetGenericInterface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(hMod, "VR_GetGenericInterface"));
    g_pfnVR_IsRuntimeInstalled = reinterpret_cast<PFN_VR_IsRuntimeInstalled>(GetProcAddress(hMod, "VR_IsRuntimeInstalled"));
    g_pfnVR_RuntimePath = reinterpret_cast<PFN_VR_RuntimePath>(GetProcAddress(hMod, "VR_RuntimePath"));
    g_pfnVR_GetVRInitErrorAsSymbol = reinterpret_cast<PFN_VR_GetVRInitErrorAsSymbol>(GetProcAddress(hMod, "VR_GetVRInitErrorAsSymbol"));
    g_pfnVR_GetVRInitErrorAsEnglishDescription = reinterpret_cast<PFN_VR_GetVRInitErrorAsEnglishDescription>(GetProcAddress(hMod, "VR_GetVRInitErrorAsEnglishDescription"));
    g_pfnVR_GetInitToken = reinterpret_cast<PFN_VR_GetInitToken>(GetProcAddress(hMod, "VR_GetInitToken"));
    g_pfnVR_IsInterfaceVersionValid = reinterpret_cast<PFN_VR_IsInterfaceVersionValid>(GetProcAddress(hMod, "VR_IsInterfaceVersionValid"));
    g_pfnVROculusDirect = reinterpret_cast<PFN_VROculusDirect>(GetProcAddress(hMod, "VROculusDirect"));
    g_pfnVR_GetRuntimePath = reinterpret_cast<PFN_VR_GetRuntimePath>(GetProcAddress(hMod, "VR_GetRuntimePath"));
    g_pfnLiquidVR = reinterpret_cast<PFN_LiquidVR>(GetProcAddress(hMod, "LiquidVR"));
    g_pfnVRControlPanel = reinterpret_cast<PFN_VRControlPanel>(GetProcAddress(hMod, "VRControlPanel"));
    g_pfnVRCompositorSystemInternal = reinterpret_cast<PFN_VRCompositorSystemInternal>(GetProcAddress(hMod, "VRCompositorSystemInternal"));
    g_pfnVRHeadsetView = reinterpret_cast<PFN_VRHeadsetView>(GetProcAddress(hMod, "VRHeadsetView"));
    g_pfnVRPaths = reinterpret_cast<PFN_VRPaths>(GetProcAddress(hMod, "VRPaths"));
    g_pfnVRRenderModelsInternal = reinterpret_cast<PFN_VRRenderModelsInternal>(GetProcAddress(hMod, "VRRenderModelsInternal"));
    g_pfnVRSceneGraph = reinterpret_cast<PFN_VRSceneGraph>(GetProcAddress(hMod, "VRSceneGraph"));
    g_pfnVRTrackedCameraInternal = reinterpret_cast<PFN_VRTrackedCameraInternal>(GetProcAddress(hMod, "VRTrackedCameraInternal"));
    g_pfnVRVirtualDisplay = reinterpret_cast<PFN_VRVirtualDisplay>(GetProcAddress(hMod, "VRVirtualDisplay"));
    g_pfnVR_GetStringForHmdError = reinterpret_cast<PFN_VR_GetStringForHmdError>(GetProcAddress(hMod, "VR_GetStringForHmdError"));

    return true;
}

static bool InitEyePipeline(uint32_t eyeIdx, ID3D11Texture2D* pGameTex, uint32_t eyeWidth, uint32_t eyeHeight, DXGI_FORMAT format) {
    auto& eye = StereoPipeline::Instance().GetEye(eyeIdx);
    if (D3D12Interop::Instance().IsInitialized()) {
        D3D12Interop::Instance().WaitForGpu();
    }
    DLSS5Bridge::Instance().ReleaseEyeFeature(eyeIdx);
    eye.Release();

    ID3D11Device* d3d11Device = nullptr;
    pGameTex->GetDevice(&d3d11Device);
    if (!d3d11Device) {
        HLX_ERROR("Failed to get D3D11 device from game texture for eye %u", eyeIdx);
        return false;
    }
    HLX_INFO("[INIT] Eye %u: got D3D11 device", eyeIdx);

    hlx::Config config;
    config.Load();

    uint32_t fullW = (eyeWidth + 1) & ~1u;
    uint32_t fullH = (eyeHeight + 1) & ~1u;

    uint32_t foveaW = fullW;
    uint32_t foveaH = fullH;
    uint32_t foveaX = 0;
    uint32_t foveaY = 0;

    float centerU = 0.5f;
    float centerV = 0.5f;

    if (g_pVRSystem) {
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
        g_pVRSystem->GetProjectionRaw(static_cast<vr::EVREye>(eyeIdx), &left, &right, &top, &bottom);
        HLX_INFO("[INIT] Eye %u: GetProjectionRaw -> L=%.3f R=%.3f T=%.3f B=%.3f", eyeIdx, left, right, top, bottom);
        if (std::isfinite(left) && std::isfinite(right) && std::isfinite(top) && std::isfinite(bottom) &&
            (right > left) && (bottom > top)) {
            // Asymmetric frustum optical axis calculation (OpenVR convention):
            // OpenVR raw tangents have down-positive signs. Forward optical axis (ray {0,0,-1})
            // projects to: u = -left / (right - left), v = -top / (bottom - top).
            float u = -left / (right - left);
            float v = -top / (bottom - top);
            if (u >= 0.15f && u <= 0.85f && v >= 0.15f && v <= 0.85f) {
                centerU = u;
                centerV = v;
                HLX_INFO("Eye %u optical center detected from OpenVR raw projection: (%.3f, %.3f) [L:%.2f, R:%.2f, T:%.2f, B:%.2f]",
                    eyeIdx, centerU, centerV, left, right, top, bottom);
            }
        }
    }

    if (config.foveated) {
        foveaW = (static_cast<uint32_t>(fullW * config.foveated_width) + 7) & ~7u;
        foveaH = (static_cast<uint32_t>(fullH * config.foveated_height) + 7) & ~7u;
        foveaW = (std::clamp)(foveaW, 32u, fullW);
        foveaH = (std::clamp)(foveaH, 32u, fullH);

        int32_t optCenterX = static_cast<int32_t>(fullW * centerU);
        int32_t optCenterY = static_cast<int32_t>(fullH * centerV);
        int32_t startX_fovea = optCenterX - static_cast<int32_t>(foveaW / 2);
        int32_t startY_fovea = optCenterY - static_cast<int32_t>(foveaH / 2);

        foveaX = static_cast<uint32_t>(std::clamp<int32_t>(startX_fovea, 0, static_cast<int32_t>(fullW - foveaW))) & ~1u;
        foveaY = static_cast<uint32_t>(std::clamp<int32_t>(startY_fovea, 0, static_cast<int32_t>(fullH - foveaH))) & ~1u;
    }

    // Neural working size: the model cost scales with the evaluated area
    // (measured GpuNr ~2.9 ms/eye at fovea size), so WorkingScale evaluates a
    // smaller image and Matched Residual Composition transfers the residual onto
    // full-res native pixels. When Passes > 1 (2 or 3 sequential layers), apply a
    // gentle area compensation (0.78x for 2 passes, 0.66x for 3 passes) so multi-pass
    // stays well inside the VR frame budget while preserving 100% native pixel sharpness.
    const int numPasses = (std::clamp)(config.passes, 1, 3);
    const float passScaleFactor = (numPasses <= 1) ? 1.0f : ((numPasses == 2) ? 0.78f : 0.66f);
    const float effectiveScale = (std::clamp)(config.working_scale * passScaleFactor, 0.30f, 1.0f);
    uint32_t workW = (static_cast<uint32_t>(static_cast<float>(foveaW) * effectiveScale) + 7) & ~7u;
    uint32_t workH = (static_cast<uint32_t>(static_cast<float>(foveaH) * effectiveScale) + 7) & ~7u;
    workW = (std::clamp)(workW, 32u, foveaW);
    workH = (std::clamp)(workH, 32u, foveaH);

    eye.fullWidth = fullW;
    eye.fullHeight = fullH;
    eye.foveaX = foveaX;
    eye.foveaY = foveaY;
    eye.foveaWidth = foveaW;
    eye.foveaHeight = foveaH;
    eye.centerU = centerU;
    eye.centerV = centerV;
    eye.lastDrsWidth = fullW;
    eye.lastDrsHeight = fullH;
    eye.width = workW;
    eye.height = workH;

    DXGI_FORMAT unormFormat = (format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM : format;

    // 1. Create Shared D3D11 Fovea Input Texture (size foveaW x foveaH)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = workW;
    td.Height = workH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = unormFormat;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    const bool needsDownsample = (workW != foveaW) || (workH != foveaH);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (needsDownsample) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = d3d11Device->CreateTexture2D(&td, nullptr, &eye.sharedInputTex11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create sharedInputTex11 for eye %u: 0x%08X", eyeIdx, hr);
        d3d11Device->Release();
        return false;
    }
    if (needsDownsample) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC iud = {};
        iud.Format = unormFormat;
        iud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        hr = d3d11Device->CreateUnorderedAccessView(eye.sharedInputTex11, &iud, &eye.inputUav11);
        if (FAILED(hr)) {
            HLX_ERROR("Failed to create inputUav11 for eye %u: 0x%08X", eyeIdx, hr);
            d3d11Device->Release();
            return false;
        }
    }

    // 2. Create Shared D3D11 Fovea Output Texture (size foveaW x foveaH)
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = d3d11Device->CreateTexture2D(&td, nullptr, &eye.sharedOutputTex11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create sharedOutputTex11 for eye %u: 0x%08X", eyeIdx, hr);
        d3d11Device->Release();
        return false;
    }

    // Create neural SRV on sharedOutputTex11 and input proxy SRV on sharedInputTex11 for Matched Residual Composite
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = unormFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = d3d11Device->CreateShaderResourceView(eye.sharedOutputTex11, &srvDesc, &eye.neuralSrv11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create neuralSrv11 for eye %u: 0x%08X", eyeIdx, hr);
    }
    hr = d3d11Device->CreateShaderResourceView(eye.sharedInputTex11, &srvDesc, &eye.inputSrv11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create inputSrv11 for eye %u: 0x%08X", eyeIdx, hr);
    }

    // 3. Create Fovea Native Capture Texture (size foveaW x foveaH — avoids copying/compositing untouched periphery)
    D3D11_TEXTURE2D_DESC fullTd = {};
    fullTd.Width = foveaW;
    fullTd.Height = foveaH;
    fullTd.MipLevels = 1;
    fullTd.ArraySize = 1;
    fullTd.Format = unormFormat;
    fullTd.SampleDesc.Count = 1;
    fullTd.SampleDesc.Quality = 0;
    fullTd.Usage = D3D11_USAGE_DEFAULT;
    fullTd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = d3d11Device->CreateTexture2D(&fullTd, nullptr, &eye.fullNativeTex11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create fullNativeTex11 for eye %u: 0x%08X", eyeIdx, hr);
    } else {
        d3d11Device->CreateShaderResourceView(eye.fullNativeTex11, &srvDesc, &eye.nativeSrv11);
    }

    // 4. Create Fovea Composited Destination Texture (size foveaW x foveaH)
    fullTd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    hr = d3d11Device->CreateTexture2D(&fullTd, nullptr, &eye.fullCompositedTex11);
    if (FAILED(hr)) {
        HLX_ERROR("Failed to create fullCompositedTex11 for eye %u: 0x%08X", eyeIdx, hr);
    } else {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = unormFormat;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        d3d11Device->CreateUnorderedAccessView(eye.fullCompositedTex11, &uavDesc, &eye.compositedUav11);
    }

    if (!eye.sharedInputTex11 || !eye.sharedOutputTex11 || !eye.nativeSrv11 || !eye.fullCompositedTex11 ||
        !eye.compositedUav11 || (needsDownsample && !eye.inputUav11)) {
        HLX_ERROR("Eye %u: incomplete D3D11 capture resources, aborting pipeline init", eyeIdx);
        d3d11Device->Release();
        return false;
    }
    HLX_INFO("[INIT] Eye %u: D3D11 capture resources ready", eyeIdx);

    // Initialize Foveated Composite & fovea downsample compute shaders
    FoveatedCompositePass::Instance().Init(d3d11Device);
    FoveaDownsamplePass::Instance().Init(d3d11Device);

    // 5. Open Shared Handles in D3D12
    IDXGIResource* resIn = nullptr;
    HANDLE hIn = nullptr;
    if (FAILED(eye.sharedInputTex11->QueryInterface(IID_PPV_ARGS(&resIn))) || !resIn) {
        HLX_ERROR("Eye %u: sharedInputTex11 is not an IDXGIResource", eyeIdx);
        d3d11Device->Release();
        return false;
    }
    resIn->GetSharedHandle(&hIn);
    resIn->Release();

    IDXGIResource* resOut = nullptr;
    HANDLE hOut = nullptr;
    if (FAILED(eye.sharedOutputTex11->QueryInterface(IID_PPV_ARGS(&resOut))) || !resOut) {
        HLX_ERROR("Eye %u: sharedOutputTex11 is not an IDXGIResource", eyeIdx);
        d3d11Device->Release();
        return false;
    }
    resOut->GetSharedHandle(&hOut);
    resOut->Release();

    auto* d3d12Device = D3D12Interop::Instance().GetDevice();
    if (!d3d12Device || !hIn || !hOut) {
        HLX_ERROR("Eye %u: missing D3D12 device or shared handles (dev=%p, in=%p, out=%p)", eyeIdx, (void*)d3d12Device, hIn, hOut);
        d3d11Device->Release();
        return false;
    }
    hr = d3d12Device->OpenSharedHandle(hIn, IID_PPV_ARGS(&eye.d3d12Input));
    if (FAILED(hr)) {
        HLX_ERROR("Failed to open shared input handle in D3D12 for eye %u: 0x%08X", eyeIdx, hr);
        d3d11Device->Release();
        return false;
    }

    hr = d3d12Device->OpenSharedHandle(hOut, IID_PPV_ARGS(&eye.d3d12Output));
    if (FAILED(hr)) {
        HLX_ERROR("Failed to open shared output handle in D3D12 for eye %u: 0x%08X", eyeIdx, hr);
        d3d11Device->Release();
        return false;
    }

    // 6. Create Depth & Motion Resources in D3D12 (same size as the evaluated color)
    D3D12_HEAP_PROPERTIES hpDefault = {};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rDesc = {};
    rDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rDesc.Width = workW;
    rDesc.Height = workH;
    rDesc.DepthOrArraySize = 1;
    rDesc.MipLevels = 1;
    rDesc.SampleDesc.Count = 1;

    rDesc.Format = DXGI_FORMAT_R32_FLOAT;
    d3d12Device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &rDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&eye.depthRes));

    rDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    d3d12Device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &rDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&eye.motionRes));

    // Create D3D12 intermediate ping-pong texture for Multi-Pass (2 or 3 passes)
    if (numPasses > 1) {
        D3D12_RESOURCE_DESC intermDesc = rDesc;
        intermDesc.Format = unormFormat;
        intermDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d3d12Device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &intermDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&eye.d3d12Interm));
    }

    // 7. Initialize Cheeky-inspired D3D11 <-> D3D12 Shared Hardware Fence
    D3D12Interop::Instance().InitSharedFence(d3d11Device);

    // 8. Create per-eye isolated Command Allocator & List
    d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&eye.cmdAllocator));
    d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, eye.cmdAllocator, nullptr, IID_PPV_ARGS(&eye.cmdList));
    if (eye.cmdList) eye.cmdList->Close();
    eye.lastDoneValue = 0;

    HLX_INFO("[INIT] Eye %u: D3D12 interop + command list ready", eyeIdx);

    // 9. Create DLSS-NR Feature 18 for this eye at workW x workH (scaled working resolution)
    bool ok = DLSS5Bridge::Instance().CreateEyeFeature(eyeIdx, workW, workH);
    d3d11Device->Release();

    HLX_INFO("Eye %u foveated pipeline initialized successfully (Full=%ux%u, Fovea=%ux%u @ (%u,%u), Model=%ux%u, scale=%.2f [eff=%.2f], passes=%d, result=%d)",
        eyeIdx, fullW, fullH, foveaW, foveaH, foveaX, foveaY, workW, workH, config.working_scale, effectiveScale, numPasses, ok);
    return ok;
}

static void DumpD3D11TextureToDisk(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* filename) {
    if (!dev || !ctx || !tex) return;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sDesc = desc;
    sDesc.Usage = D3D11_USAGE_STAGING;
    sDesc.BindFlags = 0;
    sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sDesc.MiscFlags = 0;
    if (sDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) sDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sDesc, nullptr, &staging);
    if (FAILED(hr)) return;

    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE map = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        DumpTask task;
        task.filename = filename;
        task.width = desc.Width;
        task.height = desc.Height;
        task.format = static_cast<uint32_t>(desc.Format);
        task.rowPitch = map.RowPitch;
        task.data.resize(desc.Height * desc.Width * 4);

        for (UINT y = 0; y < desc.Height; ++y) {
            const char* rowPtr = reinterpret_cast<const char*>(map.pData) + y * map.RowPitch;
            memcpy(&task.data[y * desc.Width * 4], rowPtr, desc.Width * 4);
        }

        ctx->Unmap(staging, 0);
        Logger::Instance().QueueDump(std::move(task));
        HLX_INFO("Enqueued async texture dump (%ux%u) to %ls", desc.Width, desc.Height, filename);
    }
    staging->Release();
}

// User runtime toggle: allows toggling DLSS 5 on/off on the fly
static std::atomic<bool> g_dlss5UserActive{ true };
static std::atomic<uint32_t> g_borderFlashFrames{ 0 };
using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

static void CheckInputToggle() {
    // The input read below makes ~50 vrclient IPC calls. Polling them at 30 Hz
    // instead of every eye keeps the shortcuts responsive while cutting our
    // exposure to SteamVR's transient state during loads by ~3x.
    static auto s_lastPoll = std::chrono::steady_clock::time_point{};
    const auto pollNow = std::chrono::steady_clock::now();
    if (s_lastPoll.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(pollNow - s_lastPoll).count() < 33) {
        return;
    }
    s_lastPoll = pollNow;

    struct ModInputPollScope {
        ModInputPollScope() { g_inModInputPoll = true; }
        ~ModInputPollScope() { g_inModInputPoll = false; }
    } pollScope;

    static bool s_initializedState = false;
    if (!s_initializedState) {
        s_initializedState = true;
        g_dlss5UserActive.store(StereoPipeline::Instance().GetConfig().enabled);
    }

    // Proactively acquire IVROverlay if not already captured
    if (!g_pVROverlay && g_pfnVR_GetGenericInterface) {
        vr::EVRInitError err = vr::VRInitError_None;
        g_pVROverlay = reinterpret_cast<vr::IVROverlay*>(g_pfnVR_GetGenericInterface(vr::IVROverlay_Version, &err));
        if (g_pVROverlay) {
            HLX_INFO("Captured IVROverlay interface via g_pfnVR_GetGenericInterface: %p", g_pVROverlay);
        }
    }

    // Left analog stick (Pouce gauche - navigation principale dans le menu)
    float leftStickX = 0.0f;
    float leftStickY = 0.0f;
    bool leftStickClick = false;

    // Right analog stick (Pouce droit - navigation alternative)
    float rightStickX = 0.0f;
    float rightStickY = 0.0f;
    bool rightStickClick = false;

    bool selectAction = false;
    bool leftThumbOnDpad = false;

    // Right Majeur / Grip / Trigger tracking (Gâchette sous le majeur droit >= 70%)
    float rightGripPull = 0.0f;
    float rightTriggerPull = 0.0f;
    float rightMiddleCurl = 0.0f;
    bool rightUseGrip = false;

    // Left Grip / Trigger tracking
    float leftGripPull = 0.0f;
    float leftTriggerPull = 0.0f;
    bool leftUseGrip = false;
    bool bSnapTurnEvent = false;

    // Left face buttons / capacitive touch tracking
    bool rawLeftDpadTouch = false;
    bool bToggleMenuBtn = false;
    bool bLeftHealthPenBtn = false;
    bool bLeftLaserBtn = false;
    float leftThumbCurl = 0.0f;
    float skelThumbCurl = 0.0f;
    float skelThumbCurlDevice = 0.0f;
    float skelThumbCurlAnim = 0.0f;
    bool bLeftTeleportTouch = false;
    bool bLeftWalkTouch = false;

    // 1. SteamVR Input (IVRInput) - Active when Half-Life: Alyx action manifest is in use
    static vr::VRActionHandle_t s_hContinuousTurn = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hTeleportTurn = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hTurnLeft = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hTurnRight = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hWalkMantle = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hShiftDashJump = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hAdjustHeight = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hUse = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hUseGrip = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hTriggerPull = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hHandCurl = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hFingerCurlMiddle = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hFingerCurlThumb = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hToggleMenu = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hHealthPen = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hToggleLaserSight = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hHandSkeletonLeft = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hTeleport = vr::k_ulInvalidActionHandle;
    static vr::VRActionHandle_t s_hWalk = vr::k_ulInvalidActionHandle;

    static vr::VRInputValueHandle_t s_hLeftHand = vr::k_ulInvalidInputValueHandle;
    static vr::VRInputValueHandle_t s_hRightHand = vr::k_ulInvalidInputValueHandle;

    static vr::VRActionSetHandle_t s_hSetDev = vr::k_ulInvalidActionSetHandle;
    static vr::VRActionSetHandle_t s_hSetDefault = vr::k_ulInvalidActionSetHandle;
    static vr::VRActionSetHandle_t s_hSetInteract = vr::k_ulInvalidActionSetHandle;
    static vr::VRActionSetHandle_t s_hSetMove = vr::k_ulInvalidActionSetHandle;
    static vr::VRActionSetHandle_t s_hSetWeapon = vr::k_ulInvalidActionSetHandle;
    static bool s_actionHandlesInit = false;

    if (g_pVRInput && !s_actionHandlesInit) {
        g_pVRInput->GetActionHandle("/actions/move/in/ContinuousTurn", &s_hContinuousTurn);
        g_pVRInput->GetActionHandle("/actions/move/in/TeleportTurn", &s_hTeleportTurn);
        g_pVRInput->GetActionHandle("/actions/move/in/TurnLeft", &s_hTurnLeft);
        g_pVRInput->GetActionHandle("/actions/move/in/TurnRight", &s_hTurnRight);
        g_pVRInput->GetActionHandle("/actions/move/in/WalkMantle", &s_hWalkMantle);
        g_pVRInput->GetActionHandle("/actions/move/in/ShiftDashJump", &s_hShiftDashJump);
        g_pVRInput->GetActionHandle("/actions/move/in/AdjustHeight", &s_hAdjustHeight);
        g_pVRInput->GetActionHandle("/actions/interact/in/Use", &s_hUse);
        g_pVRInput->GetActionHandle("/actions/interact/in/UseGrip", &s_hUseGrip);
        g_pVRInput->GetActionHandle("/actions/default/in/TriggerPull", &s_hTriggerPull);
        g_pVRInput->GetActionHandle("/actions/default/in/HandCurl", &s_hHandCurl);
        g_pVRInput->GetActionHandle("/actions/default/in/FingerCurlMiddle", &s_hFingerCurlMiddle);
        g_pVRInput->GetActionHandle("/actions/default/in/FingerCurlThumb", &s_hFingerCurlThumb);
        g_pVRInput->GetActionHandle("/actions/dev/in/ToggleMenu", &s_hToggleMenu);
        g_pVRInput->GetActionHandle("/actions/interact/in/HealthPen", &s_hHealthPen);
        g_pVRInput->GetActionHandle("/actions/interact/in/ToggleLaserSight", &s_hToggleLaserSight);
        g_pVRInput->GetActionHandle("/actions/default/in/HandSkeletonLeft", &s_hHandSkeletonLeft);
        g_pVRInput->GetActionHandle("/actions/move/in/Teleport", &s_hTeleport);
        g_pVRInput->GetActionHandle("/actions/move/in/Walk", &s_hWalk);

        g_pVRInput->GetInputSourceHandle("/user/hand/left", &s_hLeftHand);
        g_pVRInput->GetInputSourceHandle("/user/hand/right", &s_hRightHand);

        g_pVRInput->GetActionSetHandle("/actions/dev", &s_hSetDev);
        g_pVRInput->GetActionSetHandle("/actions/default", &s_hSetDefault);
        g_pVRInput->GetActionSetHandle("/actions/interact", &s_hSetInteract);
        g_pVRInput->GetActionSetHandle("/actions/move", &s_hSetMove);
        g_pVRInput->GetActionSetHandle("/actions/weapon", &s_hSetWeapon);

        s_actionHandlesInit = true;
        HLX_INFO("IVRInput Action Handles initialized (Hands: L=%llu, R=%llu | Sets: Dev=%llu, Def=%llu, Int=%llu)",
            (unsigned long long)s_hLeftHand, (unsigned long long)s_hRightHand,
            (unsigned long long)s_hSetDev, (unsigned long long)s_hSetDefault, (unsigned long long)s_hSetInteract);
    }

    if (g_pVRInput && s_actionHandlesInit) {
        // Right stick: Turn / Menu navigation
        if (s_hContinuousTurn != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hContinuousTurn, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None) {
                if (std::abs(data.x) > std::abs(rightStickX)) rightStickX = data.x;
                if (std::abs(data.y) > std::abs(rightStickY)) rightStickY = data.y;
            }
        }
        // Left stick: Teleport turn analog position
        if (s_hTeleportTurn != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hTeleportTurn, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None) {
                if (std::abs(data.x) > std::abs(leftStickX)) leftStickX = data.x;
                if (std::abs(data.y) > std::abs(leftStickY)) leftStickY = data.y;
            }
        }
        // Left stick: digital direction states in Alyx
        if (s_hWalkMantle != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hWalkMantle, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                if (leftStickY < 0.5f) leftStickY = 0.8f;
            }
        }
        if (s_hShiftDashJump != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hShiftDashJump, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                if (leftStickY > -0.5f) leftStickY = -0.8f;
            }
        }
        if (s_hTurnLeft != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hTurnLeft, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                if (rightStickX > -0.5f) rightStickX = -0.8f;
                if (data.bChanged) bSnapTurnEvent = true;
            }
        }
        if (s_hTurnRight != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hTurnRight, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                if (rightStickX < 0.5f) rightStickX = 0.8f;
                if (data.bChanged) bSnapTurnEvent = true;
            }
        }
        if (s_hAdjustHeight != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hAdjustHeight, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                leftStickClick = true;
            }
        }
        if (s_hUse != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hUse, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                selectAction = true;
            }
        }

        // Right hand: Majeur on grip / trigger (gâchette sous le majeur droit >= 70%)
        if (s_hHandCurl != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hHandCurl, &data, sizeof(data), s_hRightHand) == vr::VRInputError_None) {
                if (data.x > rightGripPull) rightGripPull = data.x;
            }
        }
        if (s_hTriggerPull != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hTriggerPull, &data, sizeof(data), s_hRightHand) == vr::VRInputError_None) {
                if (data.x > rightTriggerPull) rightTriggerPull = data.x;
                if (data.x > 0.40f) selectAction = true;
            }
        }
        if (s_hFingerCurlMiddle != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hFingerCurlMiddle, &data, sizeof(data), s_hRightHand) == vr::VRInputError_None) {
                if (data.x > rightMiddleCurl) rightMiddleCurl = data.x;
            }
        }
        if (s_hUseGrip != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hUseGrip, &data, sizeof(data), s_hRightHand) == vr::VRInputError_None && data.bState) {
                rightUseGrip = true;
            }
        }

        // Left hand: Grip / Trigger tracking
        if (s_hHandCurl != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hHandCurl, &data, sizeof(data), s_hLeftHand) == vr::VRInputError_None) {
                if (data.x > leftGripPull) leftGripPull = data.x;
            }
        }
        if (s_hUseGrip != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hUseGrip, &data, sizeof(data), s_hLeftHand) == vr::VRInputError_None && data.bState) {
                leftUseGrip = true;
            }
        }
        if (s_hTriggerPull != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hTriggerPull, &data, sizeof(data), s_hLeftHand) == vr::VRInputError_None) {
                if (data.x > leftTriggerPull) leftTriggerPull = data.x;
            }
        }

        // Left D-Pad / Face Buttons in Alyx action manifest:
        if (s_hToggleMenu != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hToggleMenu, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None) {
                bToggleMenuBtn = data.bState;
                if (bToggleMenuBtn) leftThumbOnDpad = true;
            }
        }
        if (s_hHealthPen != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hHealthPen, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                bLeftHealthPenBtn = true;
                leftThumbOnDpad = true;
            }
        }
        if (s_hToggleLaserSight != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hToggleLaserSight, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bState) {
                bLeftLaserBtn = true;
                leftThumbOnDpad = true;
            }
        }
        if (s_hFingerCurlThumb != vr::k_ulInvalidActionHandle) {
            vr::InputAnalogActionData_t data = {};
            if (g_pVRInput->GetAnalogActionData(s_hFingerCurlThumb, &data, sizeof(data), s_hLeftHand) == vr::VRInputError_None) {
                leftThumbCurl = data.x;
                if (leftThumbCurl >= 0.50f) {
                    leftThumbOnDpad = true;
                }
            }
        }

        // Skeletal query for left thumb presence
        if (s_hHandSkeletonLeft != vr::k_ulInvalidActionHandle) {
            vr::VRSkeletalSummaryData_t skelSummary = {};
            if (g_pVRInput->GetSkeletalSummaryData(s_hHandSkeletonLeft, vr::VRSummaryType_FromDevice, &skelSummary) == vr::VRInputError_None) {
                if (skelSummary.flFingerCurl[vr::VRFinger_Thumb] > skelThumbCurlDevice) {
                    skelThumbCurlDevice = skelSummary.flFingerCurl[vr::VRFinger_Thumb];
                }
            }
            if (g_pVRInput->GetSkeletalSummaryData(s_hHandSkeletonLeft, vr::VRSummaryType_FromAnimation, &skelSummary) == vr::VRInputError_None) {
                if (skelSummary.flFingerCurl[vr::VRFinger_Thumb] > skelThumbCurlAnim) {
                    skelThumbCurlAnim = skelSummary.flFingerCurl[vr::VRFinger_Thumb];
                }
            }
            // Device sensor first: the animation summary follows the game's animated hand, not the real thumb.
            skelThumbCurl = (skelThumbCurlDevice > 0.001f) ? skelThumbCurlDevice : skelThumbCurlAnim;
        }

        if (s_hTeleport != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hTeleport, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None) {
                bLeftTeleportTouch = data.bState;
            }
        }

        if (s_hWalk != vr::k_ulInvalidActionHandle) {
            vr::InputDigitalActionData_t data = {};
            if (g_pVRInput->GetDigitalActionData(s_hWalk, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None) {
                bLeftWalkTouch = data.bState;
            }
        }
    }

    // 2. Raw OpenVR Controller Capacitive Touch & Button Press
    // Evaluates both standard role enum and GetTrackedDeviceIndexForControllerRole.
    // Checks Left controller DPAD / Button Y (Upper) / Button X (Lower) / Touchpad / Thumbrest for combo.
    // Also reads Left and Right analog stick axes for smooth in-menu navigation!
    vr::TrackedDeviceIndex_t leftRoleIdx = g_pVRSystem ? g_pVRSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) : vr::k_unTrackedDeviceIndexInvalid;
    vr::TrackedDeviceIndex_t rightRoleIdx = g_pVRSystem ? g_pVRSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) : vr::k_unTrackedDeviceIndexInvalid;

    static bool s_loggedRoles = false;
    if (!s_loggedRoles && (leftRoleIdx != vr::k_unTrackedDeviceIndexInvalid || rightRoleIdx != vr::k_unTrackedDeviceIndexInvalid)) {
        s_loggedRoles = true;
        HLX_INFO("OpenVR Controller Roles detected: LeftHand Device Index=%u | RightHand Device Index=%u", leftRoleIdx, rightRoleIdx);
    }

    uint64_t rawLeftPressMask = 0;
    uint64_t rawLeftTouchMask = 0;
    bool rawLeftStickTouch = false;

    // The legacy controller scan costs up to 45 IPC calls and is measured dead
    // under SteamVR Input (zero [BTN-RAW] lines in every session: the masks are
    // muted for apps with an action manifest). Keep it as a 3 Hz fallback only.
    static uint32_t s_pollCount = 0;
    const bool doLegacyScan = ((++s_pollCount % 10) == 0);

    if (g_pVRSystem && doLegacyScan) {
        for (vr::TrackedDeviceIndex_t i = 1; i < 16; ++i) {
            if (!g_pVRSystem->IsTrackedDeviceConnected(i)) continue;
            vr::ETrackedDeviceClass devClass = g_pVRSystem->GetTrackedDeviceClass(i);
            if (devClass == vr::TrackedDeviceClass_Controller) {
                vr::ETrackedControllerRole role = g_pVRSystem->GetControllerRoleForTrackedDeviceIndex(i);
                bool isLeft = (role == vr::TrackedControllerRole_LeftHand) || (i == leftRoleIdx);
                bool isRight = (role == vr::TrackedControllerRole_RightHand) || (i == rightRoleIdx);

                vr::VRControllerState_t state = {};
                bool stateOk = g_pVRSystem->GetControllerState(i, &state, sizeof(state));

                // Diagnostic log whenever any button is pressed or touched on any controller
                if (stateOk && (state.ulButtonPressed != 0 || state.ulButtonTouched != 0)) {
                    static auto s_lastBtnDump = std::chrono::steady_clock::now();
                    auto nowDump = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDump - s_lastBtnDump).count() > 400) {
                        s_lastBtnDump = nowDump;
                        HLX_INFO("[BTN-RAW] Dev %u (Hand: %s, Role: %d): Press=0x%016llX | Touch=0x%016llX | Axis0=(%.2f, %.2f)",
                            i, isLeft ? "LEFT" : (isRight ? "RIGHT" : "UNKNOWN"), (int)role,
                            (unsigned long long)state.ulButtonPressed,
                            (unsigned long long)state.ulButtonTouched,
                            state.rAxis[0].x, state.rAxis[0].y);
                    }
                }

                if (stateOk && isLeft) {
                    rawLeftPressMask = state.ulButtonPressed;
                    rawLeftTouchMask = state.ulButtonTouched;
                    if (state.ulButtonTouched & (1ULL << vr::k_EButton_Axis0)) {
                        rawLeftStickTouch = true;
                    }

                    // Left controller face buttons / D-Pad:
                    // Any button pressed or touched on left controller EXCEPT Axis0 (joystick itself) and Axis1 (trigger)
                    uint64_t excludedNonDpadMask = (1ULL << vr::k_EButton_Axis0) | (1ULL << vr::k_EButton_Axis1);
                    uint64_t leftFaceButtons = (state.ulButtonPressed | state.ulButtonTouched) & ~excludedNonDpadMask;

                    if (leftFaceButtons != 0) {
                        rawLeftDpadTouch = true;
                        leftThumbOnDpad = true;
                    }

                    // Left analog joystick (rAxis[2] is the grip, never a stick axis)
                    float ax = state.rAxis[0].x;
                    float ay = state.rAxis[0].y;
                    if (std::abs(ax) > std::abs(leftStickX)) leftStickX = ax;
                    if (std::abs(ay) > std::abs(leftStickY)) leftStickY = ay;

                    if (state.ulButtonPressed & (1ULL << vr::k_EButton_Axis0)) {
                        leftStickClick = true;
                    }

                    if (state.rAxis[1].x > leftTriggerPull) leftTriggerPull = state.rAxis[1].x;
                    if (state.rAxis[2].x > leftGripPull) leftGripPull = state.rAxis[2].x;
                    if (state.ulButtonPressed & (1ULL << vr::k_EButton_ApplicationMenu)) {
                        leftThumbOnDpad = true;
                    }
                } else if (stateOk && isRight) {
                    // Right analog joystick (rAxis[2] is the grip, never a stick axis)
                    float ax = state.rAxis[0].x;
                    float ay = state.rAxis[0].y;
                    if (std::abs(ax) > std::abs(rightStickX)) rightStickX = ax;
                    if (std::abs(ay) > std::abs(rightStickY)) rightStickY = ay;

                    if (state.ulButtonPressed & (1ULL << vr::k_EButton_Axis0)) {
                        rightStickClick = true;
                    }

                    // Raw triggers / grip if populated
                    if (state.rAxis[1].x > rightTriggerPull) rightTriggerPull = state.rAxis[1].x;
                    if (state.rAxis[2].x > rightGripPull) rightGripPull = state.rAxis[2].x;
                    if (state.ulButtonPressed & (1ULL << vr::k_EButton_Grip)) rightUseGrip = true;
                }
            }
        }
    }

    // 3. Gamepad via XInput
    static HMODULE s_hXInput = nullptr;
    static PFN_XInputGetState s_pfnXInputGetState = nullptr;
    static bool s_xinputChecked = false;
    if (!s_xinputChecked) {
        s_xinputChecked = true;
        s_hXInput = LoadLibraryW(L"xinput1_4.dll");
        if (!s_hXInput) s_hXInput = LoadLibraryW(L"xinput1_3.dll");
        if (!s_hXInput) s_hXInput = LoadLibraryW(L"xinput9_1_0.dll");
        if (s_hXInput) {
            s_pfnXInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(s_hXInput, "XInputGetState"));
        }
    }

    if (s_pfnXInputGetState) {
        XINPUT_STATE xstate = {};
        for (DWORD user = 0; user < 4; ++user) {
            if (s_pfnXInputGetState(user, &xstate) == ERROR_SUCCESS) {
                // Gamepad D-Pad (Up/Down/Left/Right/Back)
                bool xDpad = ((xstate.Gamepad.wButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_BACK)) != 0);
                if (xDpad) leftThumbOnDpad = true;

                float lx = static_cast<float>(xstate.Gamepad.sThumbLX) / 32767.0f;
                float ly = static_cast<float>(xstate.Gamepad.sThumbLY) / 32767.0f;
                float rx = static_cast<float>(xstate.Gamepad.sThumbRX) / 32767.0f;
                float ry = static_cast<float>(xstate.Gamepad.sThumbRY) / 32767.0f;

                if (std::abs(lx) > std::abs(leftStickX)) leftStickX = lx;
                if (std::abs(ly) > std::abs(leftStickY)) leftStickY = ly;
                if (std::abs(rx) > std::abs(rightStickX)) rightStickX = rx;
                if (std::abs(ry) > std::abs(rightStickY)) rightStickY = ry;

                if (xstate.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) leftStickClick = true;
                if (xstate.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) rightStickClick = true;
                if ((xstate.Gamepad.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_RIGHT_SHOULDER)) || xstate.Gamepad.bRightTrigger > 100) selectAction = true;

                // Gamepad Triggers / Grip
                float rtNorm = static_cast<float>(xstate.Gamepad.bRightTrigger) / 255.0f;
                if (rtNorm > rightTriggerPull) rightTriggerPull = rtNorm;
                if (xstate.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) rightUseGrip = true;

                float ltNorm = static_cast<float>(xstate.Gamepad.bLeftTrigger) / 255.0f;
                if (ltNorm > leftTriggerPull) leftTriggerPull = ltNorm;
                if (xstate.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) leftGripPull = 1.0f;
            }
        }
    }

    // 1. Classify the left thumb: flat face area (arms shortcuts) vs joystick (never arms).
    // In Alyx's manifest, no action reports the joystick cap touch, so a resting thumb is
    // only visible through the skeletal thumb curl (measured: 0.41 on the stick cap, open
    // hand 0.00-0.08 elsewhere). Stick click / deflection always override it.
    hlx::LeftThumbSignals thumbSignals;
    thumbSignals.faceButtonClicked = bToggleMenuBtn || bLeftHealthPenBtn || bLeftLaserBtn || rawLeftDpadTouch || leftThumbOnDpad;
    thumbSignals.thumbCurl = skelThumbCurl;
    thumbSignals.thumbCurlAnalog = leftThumbCurl;
    thumbSignals.stickClicked = leftStickClick || bLeftWalkTouch;
    thumbSignals.stickDeflected = bLeftTeleportTouch || rawLeftStickTouch;
    thumbSignals.stickX = leftStickX;
    thumbSignals.stickY = leftStickY;

    hlx::LeftThumbState thumbState = hlx::ClassifyLeftThumb(thumbSignals);
    bool thumbOnLeftStick = thumbState.onStick;
    bool leftDpadTouched = thumbState.onFacePlate;

    // Reset temporal neural history on Snap Turn or Teleport completion so 1-frame camera jumps never ghost
    static bool s_wasTeleporting = false;
    bool teleportCompleted = (s_wasTeleporting && !bLeftTeleportTouch);
    s_wasTeleporting = bLeftTeleportTouch;
    if ((bSnapTurnEvent || teleportCompleted) && !VRMenu::Instance().IsVisible()) {
        StereoPipeline::Instance().ResetAllEyesHistory();
    }

    // 2. Evaluator instantiation & input feeding
    static hlx::InputComboEvaluator s_comboEvaluator;
    hlx::ControllerInputs inputs;
    inputs.leftDpadTouched = leftDpadTouched;
    inputs.leftFaceBtnHeld = bLeftHealthPenBtn || bLeftLaserBtn;
    inputs.thumbOnLeftStick = thumbOnLeftStick;
    inputs.leftStickX = leftStickX;
    inputs.leftStickY = leftStickY;
    inputs.leftGripPull = leftGripPull;
    inputs.leftUseGrip = leftUseGrip;
    inputs.leftTriggerPull = leftTriggerPull;
    inputs.rightGripPull = rightGripPull;
    inputs.rightTriggerPull = rightTriggerPull;
    inputs.rightUseGrip = rightUseGrip;
    inputs.rightStickX = rightStickX;
    inputs.rightStickY = rightStickY;

    bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    inputs.keyDlssToggle = (GetAsyncKeyState(VK_F7) & 0x8000) ||
                           (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) ||
                           (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) ||
                           (shiftPressed && (GetAsyncKeyState('2') & 0x8000));
    inputs.keyMenuToggle = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) ||
                           (GetAsyncKeyState(VK_OEM_5) & 0x8000) ||
                           (GetAsyncKeyState(VK_OEM_8) & 0x8000) ||
                           (shiftPressed && (GetAsyncKeyState('8') & 0x8000)) ||
                           (GetAsyncKeyState(VK_F8) & 0x8000) ||
                           (GetAsyncKeyState(VK_F11) & 0x8000) ||
                           (((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('D') & 0x8000)));

    auto now = std::chrono::steady_clock::now();
    auto comboOut = s_comboEvaluator.Evaluate(inputs, now);

    if (comboOut.longPressHalfwayBeep) {
        ExtendGameInputBlock(2000);
        std::thread([]() {
            Beep(950, 25);
        }).detach();
    }

    // Periodic diagnostic probe logger
    static auto s_lastProbeLog = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastProbeLog).count() > 400) {
        bool validLeftDpad = leftDpadTouched && !thumbOnLeftStick;
        bool gripPressed = (rightGripPull >= 0.70f) || rightUseGrip;
        bool triggerPressed = rightTriggerPull >= 0.70f;
        if (leftDpadTouched || thumbOnLeftStick || gripPressed || triggerPressed || skelThumbCurl > 0.05f || std::abs(rightStickY) > 0.20f) {
            s_lastProbeLog = now;
            HLX_INFO("[VRINPUT-PROBE] ValidDPadG=%d (DevCurl=%.2f AnimCurl=%.2f Used=%.2f, Stick=%d, LStickY=%.2f) | GripD=%.2f (%d) TrigD=%.2f (%d) StickDHaut=%d (Y=%.2f) | Menu=%d Toggle=%d | Visible=%d",
                (int)validLeftDpad, skelThumbCurlDevice, skelThumbCurlAnim, skelThumbCurl, (int)thumbOnLeftStick, leftStickY,
                rightGripPull, (int)gripPressed, rightTriggerPull, (int)triggerPressed,
                (int)(rightStickY > 0.40f), rightStickY,
                (int)comboOut.triggerMenuToggle, (int)comboOut.triggerDlssToggle,
                (int)VRMenu::Instance().IsVisible());
        }
    }

    // F9: capture 120 consecutive frames of raw left-thumb signals for calibration.
    // Hold the pose (thumb on the plate, then on the joystick) and compare the logs.
    static bool s_f9WasDown = false;
    static int s_captureFrames = 0;
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !s_f9WasDown && s_captureFrames == 0) {
        s_captureFrames = 120;
        Beep(1200, 60);
        HLX_INFO("--- [CAPTURE] START (hold one pose for ~1.5s) ---");
    }
    s_f9WasDown = f9Down;
    if (s_captureFrames > 0) {
        s_captureFrames--;
        HLX_INFO("[CAPTURE] %s Dev=%.3f Anim=%.3f Stick=%d Click=%d Defl=%d LStick=(%.2f,%.2f)",
            thumbState.onFacePlate ? "PLATE" : (thumbState.onStick ? "STICK" : "NONE"),
            skelThumbCurlDevice, skelThumbCurlAnim, (int)thumbState.onStick,
            (int)leftStickClick, (int)bLeftTeleportTouch, leftStickX, leftStickY);
        if (s_captureFrames == 0) HLX_INFO("--- [CAPTURE] END ---");
    }

    // Process Quick Silent Toggle (D-Pad/X Gauche + Majeur Droit >= 70% ou F7)
    if (comboOut.triggerDlssToggle) {
        bool newState = !g_dlss5UserActive.load();
        g_dlss5UserActive.store(newState);
        StereoPipeline::Instance().ResetAllEyesHistory();

        // Explicitly hide menu and suppress green border / circle
        VRMenu::Instance().Hide(g_pVROverlay);
        ExtendGameInputBlock(450);
        g_borderFlashFrames.store(0);

        HLX_INFO(">>> [USER-QUICK-TOGGLE] DLSS 5 Neural Reconstruction is now: %s (Silent: No Menu, No Border) <<<",
            newState ? "ENABLED" : "DISABLED (BYPASS)");

        std::thread([newState]() {
            if (newState) {
                Beep(1800, 80);
            } else {
                Beep(650, 100);
            }
        }).detach();

        return; // Suppress any navigation on the exact toggle frame
    }

    // Process Menu Visibility Toggle (3s Hold X/Grips, Left X/D-Pad + Right Trigger, or F8/F11)
    if (comboOut.triggerMenuToggle) {
        VRMenu::Instance().Toggle(g_pVROverlay);
        ExtendGameInputBlock(500);
        HLX_INFO(">>> [USER-MENU] DLSS 5 Settings Menu toggled (Visible: %d, Game Inputs Blocked: %d) <<<",
            (int)VRMenu::Instance().IsVisible(), (int)ShouldBlockGameInput());

        std::thread([]() {
            Beep(1400, 50);
        }).detach();

        return; // Suppress any navigation on the exact toggle frame
    }

    // Grace period: While the left D-pad or left X button is held for a combo, ignore menu navigation
    bool inComboGracePeriod = leftDpadTouched || bLeftHealthPenBtn || bLeftLaserBtn;
    if (inComboGracePeriod) {
        return;
    }

    // When VRMenu is visible and not in toggle grace period, process navigation & slider adjustments
    // Game controls are 100% blocked via Hooked_GetDigitalActionData / Hooked_GetAnalogActionData!
    if (VRMenu::Instance().IsVisible()) {
        ExtendGameInputBlock(450);
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || bToggleMenuBtn) {
            VRMenu::Instance().Hide(g_pVROverlay);
            ExtendGameInputBlock(500);
            return;
        }

        // Discrete navigation signals
        bool navUp = false;
        bool navDown = false;
        bool navLeft = false;
        bool navRight = false;

        // 1. Primary: Left joystick navigation (Axis-dominant deadzone check)
        if (std::abs(leftStickY) >= std::abs(leftStickX)) {
            if (leftStickY > 0.35f) navUp = true;
            else if (leftStickY < -0.35f) navDown = true;
        } else {
            if (leftStickX < -0.35f) navLeft = true;
            else if (leftStickX > 0.35f) navRight = true;
        }

        // 2. Alternative: Right joystick navigation
        if (std::abs(rightStickY) >= std::abs(rightStickX)) {
            if (rightStickY > 0.35f) navUp = true;
            else if (rightStickY < -0.35f) navDown = true;
        } else {
            if (rightStickX < -0.35f) navLeft = true;
            else if (rightStickX > 0.35f) navRight = true;
        }

        // 3. Keyboard navigation
        if (GetAsyncKeyState(VK_UP) & 0x8000) navUp = true;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) navDown = true;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) navLeft = true;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) navRight = true;

        bool navSelect = selectAction || leftStickClick || rightStickClick ||
                         ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);

        static auto s_lastNavTime = std::chrono::steady_clock::now();
        static bool s_stickCentered = true;
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastNavTime).count();

        bool anyDir = navUp || navDown || navLeft || navRight || navSelect;
        if (!anyDir) {
            s_stickCentered = true;
        } else {
            if (s_stickCentered || elapsedMs > 220) {
                s_stickCentered = false;
                s_lastNavTime = now;

                Config currentConfig = StereoPipeline::Instance().GetConfig();
                if (navUp) {
                    VRMenu::Instance().NavigateUp();
                } else if (navDown) {
                    VRMenu::Instance().NavigateDown();
                } else if (navLeft) {
                    VRMenu::Instance().Adjust(-1, currentConfig, g_pVROverlay);
                } else if (navRight || navSelect) {
                    VRMenu::Instance().Adjust(+1, currentConfig, g_pVROverlay);
                }
            }
        }
    }
}

// Set while we are inside the real compositor Submit, so the SEH handler knows
// whether the fault came from our own preprocessing or from SteamVR itself.
static thread_local bool g_inOriginalSubmit = false;
// Set after any SEH fault: DLSS processing is permanently disabled (vanilla
// passthrough) so a broken pipeline can never fault again every frame.
static std::atomic<bool> g_pipelineFaulted{ false };

static vr::EVRCompositorError SubmitOriginal(
    void* thisPtr, vr::EVREye eEye, const vr::Texture_t* pTexture,
    const vr::VRTextureBounds_t* pBounds, vr::EVRSubmitFlags nSubmitFlags) {
    if (!g_originalSubmit) return vr::VRCompositorError_None;
    g_inOriginalSubmit = true;
    vr::EVRCompositorError err = g_originalSubmit(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
    g_inOriginalSubmit = false;
    return err;
}

static int LogSehAndContinue(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    const uintptr_t pc = rec ? reinterpret_cast<uintptr_t>(rec->ExceptionAddress) : 0;
    const uintptr_t fault = (rec && rec->NumberParameters > 1) ? static_cast<uintptr_t>(rec->ExceptionInformation[1]) : 0;

    auto describe = [](uintptr_t addr, wchar_t* buf, size_t bufCount, unsigned long long* offset) {
        buf[0] = 0;
        *offset = 0;
        if (!addr) return;
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
            GetModuleFileNameW(mod, buf, static_cast<DWORD>(bufCount));
            *offset = addr - reinterpret_cast<uintptr_t>(mod);
        }
    };

    wchar_t pcMod[MAX_PATH] = {}, faultMod[MAX_PATH] = {};
    unsigned long long pcOff = 0, faultOff = 0;
    describe(pc, pcMod, MAX_PATH, &pcOff);
    describe(fault, faultMod, MAX_PATH, &faultOff);

    HLX_ERROR("SEH 0x%08X at %p (%ls+0x%llX) | fault addr 0x%llX (%ls+0x%llX) | inside original Submit=%d",
        rec ? rec->ExceptionCode : 0, reinterpret_cast<void*>(pc), pcMod, pcOff,
        static_cast<unsigned long long>(fault), faultMod, faultOff, (int)g_inOriginalSubmit);
    g_pipelineFaulted.store(true);
    Logger::Instance().Flush();  // the process may die here: make sure the line is on disk
    return EXCEPTION_EXECUTE_HANDLER;
}

// Internal implementation of Submit
static vr::EVRCompositorError Hooked_Submit_Impl(
    void* thisPtr,
    vr::EVREye eEye,
    const vr::Texture_t* pTexture,
    const vr::VRTextureBounds_t* pBounds,
    vr::EVRSubmitFlags nSubmitFlags
) {
    static uint64_t frameCount = 0;
    static bool loggedFirstEye[2] = { false, false };
    uint64_t currentFrame = ++frameCount;

    // Persistent static storage for submitted textures & bounds so SteamVR never dereferences dangling stack pointers!
    static vr::Texture_t s_submitTextures[2];
    static vr::VRTextureBounds_t s_submitBounds[2];

    uint32_t eyeIdx = (eEye == vr::Eye_Right) ? 1 : 0;

    // Check user toggle shortcut once per stereo frame (on Left Eye submission)
    if (eyeIdx == 0) {
        CheckInputToggle();
    }

    if (!loggedFirstEye[eyeIdx]) {
        loggedFirstEye[eyeIdx] = true;
        HLX_INFO("Hooked_Submit first call for Eye %u (Type=%d, ColorSpace=%d, Handle=%p, SubmitFlags=0x%X)",
            eyeIdx, pTexture ? pTexture->eType : -1, pTexture ? pTexture->eColorSpace : -1,
            pTexture ? pTexture->handle : nullptr, (unsigned)nSubmitFlags);
        if (pBounds) {
            HLX_INFO("Hooked_Submit Bounds for Eye %u: (uMin=%.3f, vMin=%.3f, uMax=%.3f, vMax=%.3f)",
                eyeIdx, pBounds->uMin, pBounds->vMin, pBounds->uMax, pBounds->vMax);
        }
    }

    const auto& config = StereoPipeline::Instance().GetConfig();
    bool zeroZone = config.foveated && (config.foveated_width <= 0.005f || config.foveated_height <= 0.005f);
    bool dlssActive = config.enabled && g_dlss5UserActive.load() && !zeroZone;

    if (eyeIdx == 0) {
        GazeTracker::Instance().PollFrameGaze(g_pVRSystem, config);
        VRMenu::Instance().UpdateOverlay(g_pVROverlay, config);
    }

    if (g_pipelineFaulted.load()) {
        return SubmitOriginal(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
    }

    if (dlssActive && pTexture && pTexture->handle && (pTexture->eType == vr::TextureType_DirectX)) {
            ID3D11Texture2D* pGameTex = reinterpret_cast<ID3D11Texture2D*>(pTexture->handle);
            D3D11_TEXTURE2D_DESC desc;
            pGameTex->GetDesc(&desc);

            // Determine active eye sub-rectangle within texture atlas using exact UV bounds
            UINT startX = 0;
            UINT startY = 0;
            UINT eyeWidth = desc.Width;
            UINT eyeHeight = desc.Height;

            if (pBounds) {
                float uSpan = pBounds->uMax - pBounds->uMin;
                float vSpan = pBounds->vMax - pBounds->vMin;

                if (uSpan > 0.01f && uSpan <= 1.0f) {
                    startX = static_cast<UINT>(pBounds->uMin * desc.Width + 0.5f);
                    eyeWidth = static_cast<UINT>(uSpan * desc.Width + 0.5f);
                }
                if (vSpan > 0.01f && vSpan <= 1.0f) {
                    startY = static_cast<UINT>(pBounds->vMin * desc.Height + 0.5f);
                    eyeHeight = static_cast<UINT>(vSpan * desc.Height + 0.5f);
                }
            }

            // Ensure dimensions are even numbers (required for neural tensor alignment)
            eyeWidth = (eyeWidth + 1) & ~1u;
            eyeHeight = (eyeHeight + 1) & ~1u;

            // Track active resolution changes in real time
            static uint32_t s_lastSeenWidth[2] = { 0, 0 };
            static uint32_t s_lastSeenHeight[2] = { 0, 0 };
            if (s_lastSeenWidth[eyeIdx] != eyeWidth || s_lastSeenHeight[eyeIdx] != eyeHeight) {
                HLX_INFO("[RES-EVENT] Eye %u: %ux%u -> %ux%u (Bounds: uMin=%.3f, vMin=%.3f, uMax=%.3f, vMax=%.3f, desc=%ux%u)",
                    eyeIdx, s_lastSeenWidth[eyeIdx], s_lastSeenHeight[eyeIdx], eyeWidth, eyeHeight,
                    pBounds ? pBounds->uMin : 0.0f, pBounds ? pBounds->vMin : 0.0f,
                    pBounds ? pBounds->uMax : 1.0f, pBounds ? pBounds->vMax : 1.0f,
                    desc.Width, desc.Height);
                s_lastSeenWidth[eyeIdx] = eyeWidth;
                s_lastSeenHeight[eyeIdx] = eyeHeight;
            }

            auto& eye = StereoPipeline::Instance().GetEye(eyeIdx);

            uint32_t targetW = g_targetEyeWidth.load();
            uint32_t targetH = g_targetEyeHeight.load();
            if (targetW == 0 || targetH == 0) {
                targetW = eyeWidth;
                targetH = eyeHeight;
            }

            // Permanent single-instance pipeline initialization:
            // Wait until full gameplay resolution is active before initializing Feature 18.
            // This skips low-res loading screens and permanently eliminates mid-game destruction/recreation stalls.
            if (!eye.isInitialized) {
                if (eyeWidth >= static_cast<UINT>(targetW * 0.90f)) {
                    HLX_INFO("Eye %u initializing permanent DLSS-NR pipeline at %ux%u (desc=%ux%u)...",
                        eyeIdx, targetW, targetH, desc.Width, desc.Height);
                    InitEyePipeline(eyeIdx, pGameTex, targetW, targetH, desc.Format);
                } else {
                    // Still in startup loading diorama: forward cleanly to SteamVR
                    if (g_originalSubmit) {
                        return SubmitOriginal(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
                    }
                    return vr::VRCompositorError_None;
                }
            }

            // Once initialized, support Source 2 Dynamic Resolution Scaling (DRS) seamlessly without
            // disabling DLSS 5 or reallocating Feature 18: as long as the current DRS eye viewport fits
            // our fixed fovea sweetspot (foveaWidth x foveaHeight), recenter the fovea subrect on the
            // current viewport's optical axis!
            if (eye.isInitialized) {
                if (eyeWidth < eye.foveaWidth || eyeHeight < eye.foveaHeight ||
                    (!config.foveated && (eyeWidth != eye.fullWidth || eyeHeight != eye.fullHeight))) {
                    if (g_originalSubmit) {
                        return SubmitOriginal(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
                    }
                    return vr::VRCompositorError_None;
                }
                if (config.foveated) {
                    const auto placement = GazeTracker::Instance().ResolveEyePlacement(
                        eyeIdx, eyeWidth, eyeHeight,
                        eye.foveaWidth, eye.foveaHeight,
                        eye.centerU, eye.centerV, config
                    );
                    eye.foveaX = placement.foveaX;
                    eye.foveaY = placement.foveaY;
                    if (placement.needsHistoryReset) {
                        eye.needsReset = true;
                    }
                }
                if (eyeWidth != eye.lastDrsWidth || eyeHeight != eye.lastDrsHeight) {
                    eye.lastDrsWidth = eyeWidth;
                    eye.lastDrsHeight = eyeHeight;
                    eye.needsReset = true;
                }
            }

            if (eye.isInitialized && eye.sharedInputTex11 && eye.sharedOutputTex11 && eye.fullNativeTex11 && eye.fullCompositedTex11) {
                auto startTotal = std::chrono::high_resolution_clock::now();

                // Engine frame time: eye 0 to eye 0, so the DLSS path stays comparable with the
                // vanilla path in [CADENCE-1SEC] (a per-eye delta would report half a frame).
                static auto s_lastSubmitEnd = std::chrono::high_resolution_clock::now();
                static auto s_lastFrameStart = std::chrono::high_resolution_clock::now();
                double engineMs = 0.0;
                if (eyeIdx == 0) {
                    engineMs = std::chrono::duration<double, std::milli>(startTotal - s_lastFrameStart).count();
                    s_lastFrameStart = startTotal;
                }

                ID3D11Device* d3d11Device = nullptr;
                pGameTex->GetDevice(&d3d11Device);
                ID3D11DeviceContext* d3d11Context = nullptr;
                if (d3d11Device) {
                    d3d11Device->GetImmediateContext(&d3d11Context);
                }

                if (d3d11Device) {
                    GpuTimers::Instance().Init(d3d11Device);
                }
                if (d3d11Context) {
                    GpuTimers::Instance().TryCollect(d3d11Context, static_cast<int>(eyeIdx));
                }

                auto t0 = std::chrono::high_resolution_clock::now();
                if (d3d11Context) {
                    GpuTimers::Instance().Begin(d3d11Context, static_cast<int>(eyeIdx));
                    // Unbind active render targets to prevent D3D11 copy hazards and allow compositor read access
                    ID3D11RenderTargetView* nullRTV[8] = { nullptr };
                    d3d11Context->OMSetRenderTargets(8, nullRTV, nullptr);

                    // 1. Copy only the foveated sweetspot subrect into fullNativeTex11 (foveaWidth x foveaHeight)
                    //    Periphery outside foveaBox remains untouched in pGameTex, cutting copy & composite cost by ~70%
                    D3D11_BOX foveaBox = {};
                    foveaBox.left = startX + eye.foveaX;
                    foveaBox.top = startY + eye.foveaY;
                    foveaBox.front = 0;
                    foveaBox.right = foveaBox.left + eye.foveaWidth;
                    foveaBox.bottom = foveaBox.top + eye.foveaHeight;
                    foveaBox.back = 1;

                    d3d11Context->CopySubresourceRegion(
                        eye.fullNativeTex11, 0,
                        0, 0, 0,
                        pGameTex, 0,
                        &foveaBox
                    );

                    // 2. Populate sharedInputTex11 (workW x workH) via copy (scale=1.0) or area-weighted box downsample
                    if (eye.width == eye.foveaWidth && eye.height == eye.foveaHeight) {
                        d3d11Context->CopySubresourceRegion(
                            eye.sharedInputTex11, 0,
                            0, 0, 0,
                            pGameTex, 0,
                            &foveaBox
                        );
                    } else {
                        FoveaDownsampleConstants dsConsts = {};
                        dsConsts.srcBaseX = 0;
                        dsConsts.srcBaseY = 0;
                        dsConsts.srcWidth = eye.foveaWidth;
                        dsConsts.srcHeight = eye.foveaHeight;
                        dsConsts.dstWidth = eye.width;
                        dsConsts.dstHeight = eye.height;
                        FoveaDownsamplePass::Instance().Execute(
                            d3d11Context, eye.nativeSrv11, eye.inputUav11, dsConsts);
                    }
                }
                if (d3d11Context) {
                    GpuTimers::Instance().Mark(d3d11Context, static_cast<int>(eyeIdx), 1);
                }
                auto t1 = std::chrono::high_resolution_clock::now();

                // Step 2 & 3: Evaluate DLSS 5 Neural Reconstruction & Writeback
                auto t_eval0 = std::chrono::high_resolution_clock::now();
                auto& d3d12 = D3D12Interop::Instance();
                ComPtr<ID3D11DeviceContext4> ctx11_4;
                bool hasCtx4 = d3d11Context && SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&ctx11_4)));
                bool useSharedFence = config.use_shared_hardware_fence &&
                                      d3d12.HasSharedFence() &&
                                      eye.cmdList && eye.cmdAllocator &&
                                      hasCtx4;

                bool evaluated = false;
                if (useSharedFence) {
                    uint64_t readyVal = d3d12.NextSharedFenceValue();
                    uint64_t doneVal = d3d12.NextSharedFenceValue();

                    // 1. D3D11 signals that it finished copying to shared input on GPU
                    ctx11_4->Signal(d3d12.GetD3D11Fence(), readyVal);

                    // 2. D3D12 command queue waits for D3D11 copy to finish on GPU
                    d3d12.GetCommandQueue()->Wait(d3d12.GetD3D12SharedFence(), readyVal);

                    // 3. Reset per-eye allocator & command list (zero-stall double buffering)
                    if (eye.lastDoneValue > 0 && d3d12.GetD3D12SharedFence()->GetCompletedValue() < eye.lastDoneValue) {
                        HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                        d3d12.GetD3D12SharedFence()->SetEventOnCompletion(eye.lastDoneValue, hEvent);
                        WaitForSingleObject(hEvent, 100);
                        CloseHandle(hEvent);
                    }
                    eye.cmdAllocator->Reset();
                    eye.cmdList->Reset(eye.cmdAllocator, nullptr);

                    evaluated = DLSS5Bridge::Instance().EvaluateEye(eyeIdx, eye.cmdList);
                    if (evaluated) {
                        eye.cmdList->Close();
                        ID3D12CommandList* lists[] = { eye.cmdList };
                        d3d12.GetCommandQueue()->ExecuteCommandLists(1, lists);

                        // 4. D3D12 signals completion on GPU
                        d3d12.GetCommandQueue()->Signal(d3d12.GetD3D12SharedFence(), doneVal);
                        eye.lastDoneValue = doneVal;

                        // 5. D3D11 waits for D3D12 on GPU
                        ctx11_4->Wait(d3d12.GetD3D11Fence(), doneVal);

                        GpuTimers::Instance().Mark(d3d11Context, static_cast<int>(eyeIdx), 2);

                        // 6. D3D11 Foveated Compositing: Matched Residual Transfer over fovea subrect only
                        FoveatedConstants fovConsts = {};
                        fovConsts.outputWidth = eye.foveaWidth;
                        fovConsts.outputHeight = eye.foveaHeight;
                        fovConsts.foveaBaseX = 0;
                        fovConsts.foveaBaseY = 0;
                        fovConsts.foveaWidth = eye.foveaWidth;
                        fovConsts.foveaHeight = eye.foveaHeight;
                        fovConsts.neuralWidth = eye.width;
                        fovConsts.neuralHeight = eye.height;
                        fovConsts.foveaRoundness = config.foveated_roundness;
                        fovConsts.featherWidth = config.foveated_feather;
                        fovConsts.foveatedEnabled = config.foveated ? 1 : 0;
                        uint32_t flashLeft = g_borderFlashFrames.load();
                        bool flashActive = (flashLeft > 0);
                        if (flashActive && eyeIdx == 1) {
                            g_borderFlashFrames.store(flashLeft - 1);
                        }
                        fovConsts.showBorder = (config.show_border || flashActive || VRMenu::Instance().IsVisible()) ? 1 : 0;
                        fovConsts.sharpness = static_cast<float>(config.sharpness);

                        FoveatedCompositePass::Instance().Execute(
                            d3d11Context,
                            eye.nativeSrv11,
                            eye.neuralSrv11,
                            eye.compositedUav11,
                            fovConsts,
                            eye.inputSrv11
                        );

                        // 7. Copy composited fovea subrect back into pGameTex at (startX + foveaX, startY + foveaY)
                        D3D11_BOX outBox = {};
                        outBox.left = 0;
                        outBox.top = 0;
                        outBox.front = 0;
                        outBox.right = eye.foveaWidth;
                        outBox.bottom = eye.foveaHeight;
                        outBox.back = 1;

                        d3d11Context->CopySubresourceRegion(
                            pGameTex, 0,
                            startX + eye.foveaX, startY + eye.foveaY, 0,
                            eye.fullCompositedTex11, 0,
                            &outBox
                        );
                        // Flush GPU command buffer to ensure SteamVR compositor sees the written eye immediately
                        d3d11Context->Flush();
                    }
                } else {
                    // Fallback to CPU synchronized execution
                    if (d3d11Context) d3d11Context->Flush();
                    evaluated = DLSS5Bridge::Instance().EvaluateEye(eyeIdx);
                    if (evaluated) {
                        d3d12.ExecuteAndSync();
                        if (d3d11Context) {
                            GpuTimers::Instance().Mark(d3d11Context, static_cast<int>(eyeIdx), 2);
                        }

                        if (d3d11Context) {
                            FoveatedConstants fovConsts = {};
                            fovConsts.outputWidth = eye.foveaWidth;
                            fovConsts.outputHeight = eye.foveaHeight;
                            fovConsts.foveaBaseX = 0;
                            fovConsts.foveaBaseY = 0;
                            fovConsts.foveaWidth = eye.foveaWidth;
                            fovConsts.foveaHeight = eye.foveaHeight;
                            fovConsts.neuralWidth = eye.width;
                            fovConsts.neuralHeight = eye.height;
                            fovConsts.foveaRoundness = config.foveated_roundness;
                            fovConsts.featherWidth = config.foveated_feather;
                            fovConsts.foveatedEnabled = config.foveated ? 1 : 0;
                            uint32_t flashLeft = g_borderFlashFrames.load();
                            bool flashActive = (flashLeft > 0);
                            if (flashActive && eyeIdx == 1) {
                                g_borderFlashFrames.store(flashLeft - 1);
                            }
                            fovConsts.showBorder = (config.show_border || flashActive || VRMenu::Instance().IsVisible()) ? 1 : 0;
                            fovConsts.sharpness = static_cast<float>(config.sharpness);

                            FoveatedCompositePass::Instance().Execute(
                                d3d11Context,
                                eye.nativeSrv11,
                                eye.neuralSrv11,
                                eye.compositedUav11,
                                fovConsts,
                                eye.inputSrv11
                            );

                            D3D11_BOX outBox = {};
                            outBox.left = 0;
                            outBox.top = 0;
                            outBox.front = 0;
                            outBox.right = eye.foveaWidth;
                            outBox.bottom = eye.foveaHeight;
                            outBox.back = 1;

                            d3d11Context->CopySubresourceRegion(
                                pGameTex, 0,
                                startX + eye.foveaX, startY + eye.foveaY, 0,
                                eye.fullCompositedTex11, 0,
                                &outBox
                            );
                            d3d11Context->Flush();
                        }
                    }
                }
                if (d3d11Context) {
                    GpuTimers::Instance().End(d3d11Context, static_cast<int>(eyeIdx));
                }
                auto t_sync1 = std::chrono::high_resolution_clock::now();

                // Diagnostics frame dump (manual trigger only, checked at most once every 600 frames to avoid disk stalls)
                if (currentFrame % 600 == 0) {
                    bool triggerFileExists = (GetFileAttributesW(L"dump_trigger.txt") != INVALID_FILE_ATTRIBUTES);
                    if (triggerFileExists && d3d11Device && d3d11Context) {
                        DeleteFileW(L"dump_trigger.txt");
                        HLX_INFO("Manual dump trigger detected at Frame %llu for Eye %u!", currentFrame, eyeIdx);
                        DumpD3D11TextureToDisk(d3d11Device, d3d11Context, eye.fullNativeTex11, L"dump_input.raw");
                        DumpD3D11TextureToDisk(d3d11Device, d3d11Context, eye.fullCompositedTex11, L"dump_output.raw");
                    }
                }

                if (d3d11Context) d3d11Context->Release();
                if (d3d11Device) d3d11Device->Release();
                auto t3 = std::chrono::high_resolution_clock::now();

                // Forward to original Submit and capture exact SteamVR compositor presentation latency
                auto t_sub0 = std::chrono::high_resolution_clock::now();
                vr::EVRCompositorError err = vr::VRCompositorError_None;
                if (g_originalSubmit) {
                    err = SubmitOriginal(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
                    if (err != vr::VRCompositorError_None) {
                        static uint32_t s_submitErrCount = 0;
                        if (++s_submitErrCount <= 10) {
                            HLX_ERROR("Original Submit for Eye %u returned error: %d", eyeIdx, err);
                        }
                    }
                }
                auto t_sub1 = std::chrono::high_resolution_clock::now();
                s_lastSubmitEnd = t_sub1;

                double copyInMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                double dlssNrMs = std::chrono::duration<double, std::milli>(t_sync1 - t_eval0).count();
                double copyOutMs = std::chrono::duration<double, std::milli>(t3 - t_sync1).count();
                double vrSubmitMs = std::chrono::duration<double, std::milli>(t_sub1 - t_sub0).count();
                double totalMs = std::chrono::duration<double, std::milli>(t_sub1 - startTotal).count();

                StereoPipeline::Instance().RecordFrame(eyeIdx, evaluated, totalMs);

                // Real-time spike logger (> 10.0 ms per eye)
                if (totalMs > 10.0) {
                    HLX_WARN("[PERF-SPIKE] Eye %u Frame %llu (Total=%.2f ms, Engine=%.2f ms): CopyIn=%.2f, DLSS-NR=%.2f, CopyOut=%.2f, VRSubmit=%.2f | Size=%ux%u",
                        eyeIdx, currentFrame, totalMs, engineMs, copyInMs, dlssNrMs, copyOutMs, vrSubmitMs, eyeWidth, eyeHeight);
                }

                // Periodic 1-second cadence summary aggregator
                static auto s_lastCadenceSec = std::chrono::high_resolution_clock::now();
                static uint32_t s_cadenceEyeCount = 0;
                static uint32_t s_cadenceSpikes = 0;
                static double s_cadenceTotalSum = 0.0;
                static double s_cadenceMaxTotal = 0.0;
                static double s_cadenceDlssSum = 0.0;
                static double s_cadenceSubmitSum = 0.0;
                static double s_cadenceEngineSum = 0.0;

                s_cadenceEyeCount++;
                s_cadenceTotalSum += totalMs;
                s_cadenceDlssSum += dlssNrMs;
                s_cadenceSubmitSum += vrSubmitMs;
                s_cadenceEngineSum += engineMs;
                if (totalMs > 10.0) s_cadenceSpikes++;
                if (totalMs > s_cadenceMaxTotal) s_cadenceMaxTotal = totalMs;

                auto nowSec = std::chrono::high_resolution_clock::now();
                double elapsedSec = std::chrono::duration<double>(nowSec - s_lastCadenceSec).count();
                if (elapsedSec >= 1.0) {
                    double stereoFps = (s_cadenceEyeCount / 2.0) / elapsedSec;
                    double avgTotal = s_cadenceEyeCount > 0 ? (s_cadenceTotalSum / s_cadenceEyeCount) : 0.0;
                    double avgDlss = s_cadenceEyeCount > 0 ? (s_cadenceDlssSum / s_cadenceEyeCount) : 0.0;
                    double avgSubmit = s_cadenceEyeCount > 0 ? (s_cadenceSubmitSum / s_cadenceEyeCount) : 0.0;
                    const double frames = (std::max)(1.0, s_cadenceEyeCount / 2.0);
                    double avgEngine = s_cadenceEngineSum / frames;
                    double gpu[3] = {};
                    GpuTimers::Instance().Consume(gpu);
                    double estimatedStereoMs = (avgTotal * 2.0) + avgEngine;
                    bool locked90 = (estimatedStereoMs <= 11.11);

                    HLX_INFO("[CADENCE-1SEC] Dlss=ON StereoFPS=%.1f | EyeAvg=%.2f ms (Max=%.2f ms) | EngineAvg=%.2f ms | DlssNrAvg=%.2f ms | VRSubmitAvg=%.2f ms | StereoEst=%.2f ms [%s] | GpuIn=%.2f GpuNr=%.2f GpuOut=%.2f ms/eye | Spikes (>10ms)=%u | Size=%ux%u",
                        stereoFps, avgTotal, s_cadenceMaxTotal, avgEngine, avgDlss, avgSubmit, estimatedStereoMs,
                        locked90 ? "90Hz LOCKED" : "THROTTLED/REPROJECT", gpu[0], gpu[1], gpu[2],
                        s_cadenceSpikes, eyeWidth, eyeHeight);

                    s_lastCadenceSec = nowSec;
                    s_cadenceEyeCount = 0;
                    s_cadenceSpikes = 0;
                    s_cadenceTotalSum = 0.0;
                    s_cadenceMaxTotal = 0.0;
                    s_cadenceDlssSum = 0.0;
                    s_cadenceSubmitSum = 0.0;
                    s_cadenceEngineSum = 0.0;
                }

                return err;
            }
        }

    // Vanilla (DLSS off) cadence: same per-second line as the DLSS path, so ON vs OFF
    // can be compared from the log alone instead of an external GPU overlay.
    static auto s_vanillaPrevEye0 = std::chrono::high_resolution_clock::now();
    static auto s_vanillaLastLog = std::chrono::high_resolution_clock::now();
    static double s_vanillaEngineSum = 0.0;
    static uint32_t s_vanillaFrames = 0;
    if (eyeIdx == 0 && !dlssActive) {
        auto nowT = std::chrono::high_resolution_clock::now();
        s_vanillaEngineSum += std::chrono::duration<double, std::milli>(nowT - s_vanillaPrevEye0).count();
        s_vanillaPrevEye0 = nowT;
        s_vanillaFrames++;
        double elapsed = std::chrono::duration<double>(nowT - s_vanillaLastLog).count();
        if (elapsed >= 1.0) {
            double avgEngine = s_vanillaFrames > 0 ? s_vanillaEngineSum / s_vanillaFrames : 0.0;
            HLX_INFO("[CADENCE-1SEC] Dlss=OFF StereoFPS=%.1f | EngineAvg=%.2f ms", s_vanillaFrames / elapsed, avgEngine);
            s_vanillaLastLog = nowT;
            s_vanillaFrames = 0;
            s_vanillaEngineSum = 0.0;
        }
    }

    // Forward to original Submit and capture error code
    if (g_originalSubmit) {
        vr::EVRCompositorError err = SubmitOriginal(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
        if (err != vr::VRCompositorError_None) {
            static uint32_t errCount = 0;
            if (++errCount <= 10) {
                HLX_ERROR("Original Submit for Eye %u returned error: %d", eyeIdx, err);
            }
        }
        return err;
    }
    return vr::VRCompositorError_None;
}

// Intercepted Submit method with SEH exception barrier
static vr::EVRCompositorError Hooked_Submit(
    void* thisPtr,
    vr::EVREye eEye,
    const vr::Texture_t* pTexture,
    const vr::VRTextureBounds_t* pBounds,
    vr::EVRSubmitFlags nSubmitFlags
) {
    __try {
        return Hooked_Submit_Impl(thisPtr, eEye, pTexture, pBounds, nSubmitFlags);
    } __except (LogSehAndContinue(GetExceptionInformation())) {
        // Never re-enter the compositor after a fault: the eye is skipped for this
        // frame and every later frame takes the plain vanilla passthrough path.
        (void)g_inOriginalSubmit;
        return vr::VRCompositorError_None;
    }
}

} // namespace hlx

// ============================================================================
// OpenVR C Export Implementations
// ============================================================================

extern "C" {

__declspec(dllexport) uint32_t VR_InitInternal(vr::EVRInitError *peError, vr::EVRApplicationType eType) {
    hlx::Logger::Instance().Init();
    HLX_INFO("VR_InitInternal called with ApplicationType=%d", eType);
    hlx::LoadRealOpenVR();

    // Initialize subsystems
    hlx::Config config;
    config.Load();
    hlx::StereoPipeline::Instance().Init(config);
    hlx::D3D12Interop::Instance().Init();
    hlx::DLSS5Bridge::Instance().Init(hlx::D3D12Interop::Instance().GetDevice(), config);

    if (hlx::g_pfnVR_InitInternal) {
        return hlx::g_pfnVR_InitInternal(peError, eType);
    }
    return 1;
}

__declspec(dllexport) uint32_t VR_InitInternal2(vr::EVRInitError *peError, vr::EVRApplicationType eType, const char *pStartupInfo) {
    hlx::Logger::Instance().Init();
    HLX_INFO("VR_InitInternal2 called with ApplicationType=%d, StartupInfo='%s'", eType, pStartupInfo ? pStartupInfo : "");
    hlx::LoadRealOpenVR();

    // Initialize subsystems
    hlx::Config config;
    config.Load();
    hlx::StereoPipeline::Instance().Init(config);
    hlx::D3D12Interop::Instance().Init();
    hlx::DLSS5Bridge::Instance().Init(hlx::D3D12Interop::Instance().GetDevice(), config);

    if (hlx::g_pfnVR_InitInternal2) {
        return hlx::g_pfnVR_InitInternal2(peError, eType, pStartupInfo);
    }
    return 1;
}

__declspec(dllexport) void VR_ShutdownInternal() {
    HLX_INFO("VR_ShutdownInternal called");
    hlx::g_submitHook.Restore();
    hlx::g_getAnalogActionDataHook.Restore();
    hlx::g_getDigitalActionDataHook.Restore();
    hlx::g_updateActionStateHook.Restore();
    hlx::g_renderTargetHook.Restore();
    hlx::StereoPipeline::Instance().Shutdown();
    hlx::DLSS5Bridge::Instance().Shutdown();
    hlx::D3D12Interop::Instance().Shutdown();

    if (hlx::g_pfnVR_ShutdownInternal) {
        hlx::g_pfnVR_ShutdownInternal();
    }
    hlx::Logger::Instance().Shutdown();
}

__declspec(dllexport) bool VR_IsHmdPresent() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_IsHmdPresent) return hlx::g_pfnVR_IsHmdPresent();
    return false;
}

__declspec(dllexport) void* VR_GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError) {
    hlx::LoadRealOpenVR();
    void* iface = nullptr;
    if (hlx::g_pfnVR_GetGenericInterface) {
        iface = hlx::g_pfnVR_GetGenericInterface(pchInterfaceVersion, peError);
    }

    if (iface && pchInterfaceVersion) {
        HLX_INFO("VR_GetGenericInterface requested: %s -> %p", pchInterfaceVersion, iface);

        // Hook IVRSystem
        if (strncmp(pchInterfaceVersion, "IVRSystem", 9) == 0) {
            hlx::g_pVRSystem = reinterpret_cast<vr::IVRSystem*>(iface);
            void** vtable = *reinterpret_cast<void***>(iface);
            if (vtable && !hlx::g_renderTargetHook.IsInstalled()) {
                // GetRecommendedRenderTargetSize is slot 0 in IVRSystem vtable
                void** rtsSlot = &vtable[0];
                hlx::g_renderTargetHook.Install(
                    rtsSlot,
                    reinterpret_cast<void*>(&hlx::Hooked_GetRecommendedRenderTargetSize),
                    reinterpret_cast<void**>(&hlx::g_originalGetRecommendedRenderTargetSize)
                );
                HLX_INFO("Successfully hooked IVRSystem::GetRecommendedRenderTargetSize at slot 0!");
            }
        }

        // Capture IVRInput and hook UpdateActionState
        if (strncmp(pchInterfaceVersion, "IVRInput_", 9) == 0) {
            hlx::g_pVRInput = reinterpret_cast<vr::IVRInput*>(iface);
            HLX_INFO("Successfully captured IVRInput client interface: %s -> %p", pchInterfaceVersion, hlx::g_pVRInput);

            void** vtable = *reinterpret_cast<void***>(iface);
            if (vtable && !hlx::g_updateActionStateHook.IsInstalled()) {
                // UpdateActionState is slot 4 in IVRInput vtable
                void** updateSlot = &vtable[4];
                hlx::g_updateActionStateHook.Install(
                    updateSlot,
                    reinterpret_cast<void*>(&hlx::Hooked_UpdateActionState),
                    reinterpret_cast<void**>(&hlx::g_originalUpdateActionState)
                );
                HLX_INFO("Successfully hooked IVRInput::UpdateActionState at slot 4!");
            }
            if (vtable && !hlx::g_getDigitalActionDataHook.IsInstalled()) {
                // GetDigitalActionData is slot 5 in IVRInput vtable
                void** digitalSlot = &vtable[5];
                hlx::g_getDigitalActionDataHook.Install(
                    digitalSlot,
                    reinterpret_cast<void*>(&hlx::Hooked_GetDigitalActionData),
                    reinterpret_cast<void**>(&hlx::g_originalGetDigitalActionData)
                );
                HLX_INFO("Successfully hooked IVRInput::GetDigitalActionData at slot 5!");
            }
            if (vtable && !hlx::g_getAnalogActionDataHook.IsInstalled()) {
                // GetAnalogActionData is slot 6 in IVRInput vtable
                void** analogSlot = &vtable[6];
                hlx::g_getAnalogActionDataHook.Install(
                    analogSlot,
                    reinterpret_cast<void*>(&hlx::Hooked_GetAnalogActionData),
                    reinterpret_cast<void**>(&hlx::g_originalGetAnalogActionData)
                );
                HLX_INFO("Successfully hooked IVRInput::GetAnalogActionData at slot 6!");
            }
        }

        // Capture IVROverlay
        if (strncmp(pchInterfaceVersion, "IVROverlay_", 11) == 0) {
            hlx::g_pVROverlay = reinterpret_cast<vr::IVROverlay*>(iface);
            HLX_INFO("Successfully captured IVROverlay client interface: %s -> %p", pchInterfaceVersion, hlx::g_pVROverlay);
        }

        // Hook IVRCompositor
        if (strncmp(pchInterfaceVersion, "IVRCompositor", 13) == 0) {
            void** vtable = *reinterpret_cast<void***>(iface);
            if (vtable && !hlx::g_submitHook.IsInstalled()) {
                // Submit is slot 6 in IVRCompositor vtable
                void** submitSlot = &vtable[6];
                hlx::g_submitHook.Install(
                    submitSlot,
                    reinterpret_cast<void*>(&hlx::Hooked_Submit),
                    reinterpret_cast<void**>(&hlx::g_originalSubmit)
                );
                HLX_INFO("Successfully hooked IVRCompositor::Submit at slot 6!");
            }
        }
    }
    return iface;
}

__declspec(dllexport) bool VR_IsRuntimeInstalled() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_IsRuntimeInstalled) return hlx::g_pfnVR_IsRuntimeInstalled();
    return true;
}

__declspec(dllexport) const char* VR_RuntimePath() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_RuntimePath) return hlx::g_pfnVR_RuntimePath();
    return "";
}

__declspec(dllexport) const char* VR_GetVRInitErrorAsSymbol(vr::EVRInitError error) {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_GetVRInitErrorAsSymbol) return hlx::g_pfnVR_GetVRInitErrorAsSymbol(error);
    return "VRInitError_Unknown";
}

__declspec(dllexport) const char* VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError error) {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_GetVRInitErrorAsEnglishDescription) return hlx::g_pfnVR_GetVRInitErrorAsEnglishDescription(error);
    return "Unknown VR init error";
}

__declspec(dllexport) uint32_t VR_GetInitToken() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_GetInitToken) return hlx::g_pfnVR_GetInitToken();
    return 1;
}

__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char *pchInterfaceVersion) {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_IsInterfaceVersionValid) return hlx::g_pfnVR_IsInterfaceVersionValid(pchInterfaceVersion);
    return true;
}

__declspec(dllexport) void* VRControlPanel() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRControlPanel) return hlx::g_pfnVRControlPanel();
    return nullptr;
}

__declspec(dllexport) void* VROculusDirect() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVROculusDirect) return hlx::g_pfnVROculusDirect();
    return nullptr;
}

__declspec(dllexport) void* LiquidVR() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnLiquidVR) return hlx::g_pfnLiquidVR();
    return nullptr;
}

__declspec(dllexport) void* VRCompositorSystemInternal() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRCompositorSystemInternal) return hlx::g_pfnVRCompositorSystemInternal();
    return nullptr;
}

__declspec(dllexport) vr::IVRHeadsetView* VRHeadsetView() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRHeadsetView) return hlx::g_pfnVRHeadsetView();
    return nullptr;
}

__declspec(dllexport) void* VRPaths() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRPaths) return hlx::g_pfnVRPaths();
    return nullptr;
}

__declspec(dllexport) void* VRRenderModelsInternal() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRRenderModelsInternal) return hlx::g_pfnVRRenderModelsInternal();
    return nullptr;
}

__declspec(dllexport) void* VRSceneGraph() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRSceneGraph) return hlx::g_pfnVRSceneGraph();
    return nullptr;
}

__declspec(dllexport) void* VRTrackedCameraInternal() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRTrackedCameraInternal) return hlx::g_pfnVRTrackedCameraInternal();
    return nullptr;
}

__declspec(dllexport) void* VRVirtualDisplay() {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVRVirtualDisplay) return hlx::g_pfnVRVirtualDisplay();
    return nullptr;
}

__declspec(dllexport) const char* VR_GetStringForHmdError(vr::EVRInitError error) {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_GetStringForHmdError) return hlx::g_pfnVR_GetStringForHmdError(error);
    return "HmdError";
}

__declspec(dllexport) bool VR_GetRuntimePath(char *pchPathBuffer, uint32_t unBufferSize, uint32_t *punRequiredBufferSize) {
    hlx::LoadRealOpenVR();
    if (hlx::g_pfnVR_GetRuntimePath) return hlx::g_pfnVR_GetRuntimePath(pchPathBuffer, unBufferSize, punRequiredBufferSize);
    if (pchPathBuffer && unBufferSize > 0) pchPathBuffer[0] = '\0';
    if (punRequiredBufferSize) *punRequiredBufferSize = 0;
    return false;
}

} // extern "C"

// Standard DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) {
            hlx::g_submitHook.Restore();
            hlx::g_renderTargetHook.Restore();
        }
        break;
    }
    return TRUE;
}
