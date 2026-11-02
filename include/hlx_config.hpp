#pragma once

#include <windows.h>
#include <string>
#include "hlx_logger.hpp"

namespace hlx {

static inline int g_dummyConfigSymbol = 0;

struct Config {
    bool enabled = true;
    bool foveated = true;             // Cheeky foveated rendering
    float foveated_width = 0.55f;     // Sweetspot width ratio [0.00 - 1.00] (0.00 = 0% / bypass)
    float foveated_height = 0.55f;    // Sweetspot height ratio [0.00 - 1.00] (0.00 = 0% / bypass)
    float foveated_roundness = 0.50f; // Ellipse roundness [0.0 = box, 1.0 = ellipse]
    float foveated_feather = 0.12f;   // Smooth transition falloff width [0.01 - 0.50]
    bool eye_tracking = true;         // Dynamic Foveated Rendering (Eye Tracking via OpenVR/Pimax/OpenXR when supported)
    float gaze_smoothing_ms = 35.0f;  // Cheeky temporal gaze smoothing [0.0 - 120.0 ms]
    int gaze_quantization_pixels = 8; // Cheeky U-Net macroblock quantization step [2 - 32 px]
    float gaze_jump_reset_ratio = 0.60f; // Cheeky saccade jump threshold before temporal reset [0.10 - 1.00]
    bool show_border = false;         // Green alignment ring on sweetspot boundary
    float working_scale = 0.50f;      // Scale of neural evaluation relative to fovea region (0.50 = 4x faster with Matched Residual Transfer)
    float sharpness = 0.35f;          // Neural detail sharpness [0.0 - 1.0]
    float detail_boost = 1.0f;        // Neural detail intensity factor [0.0 - 2.0]
    int passes = 1;                   // Number of sequential DLSS-NR neural passes [1 - 3]
    int preset = 2;                   // DLSS-NR model preset [0 - 3] (2 = Performance VR topology)
    int style = 0;                    // DLSS 5 style [0: Standard, 1: Natural, 2: Cinematic]
    float local_structure = 1.0f;     // Local structural synthesis strength [0.0 - 2.0]
    float local_tone = 0.0f;          // Local tone / shadow balancing [0.0 - 2.0]
    float skin_structure = -1.0f;     // Skin detail / smoothing [-1.0 = auto, 0.0 - 2.0]
    bool auto_mask = true;            // Semantic object protection mask
    bool fallback_on_error = true;    // Pass-through vanilla texture if NGX fails
    bool enable_d3d11_interop = true; // Support Alyx in -dx11 mode
    bool enable_vulkan_interop = true;// Support Alyx in -vulkan mode
    bool use_shared_hardware_fence = true; // Cheeky transport: zero-wait D3D11/D3D12 hardware fence
    bool dump_frames = false;         // Diagnostics: dump input & output textures to disk
    std::wstring dlssnr_path = L"";   // Custom path to nvngx_dlssnr.dll if specified

    void Load(const wchar_t* iniFilename = L"hlx_dlss5.ini") {
        wchar_t modulePath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_dummyConfigSymbol),
            &hModule
        );
        GetModuleFileNameW(hModule, modulePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
        std::wstring fullIniPath = (lastSlash ? std::wstring(modulePath, lastSlash + 1) : L".\\") + iniFilename;
        const wchar_t* iniPath = fullIniPath.c_str();

        enabled = GetPrivateProfileIntW(L"HLX_DLSS5", L"Enabled", 1, iniPath) != 0;
        foveated = GetPrivateProfileIntW(L"HLX_DLSS5", L"Foveated", 1, iniPath) != 0;
        eye_tracking = GetPrivateProfileIntW(L"HLX_DLSS5", L"EyeTracking", 1, iniPath) != 0;
        show_border = GetPrivateProfileIntW(L"HLX_DLSS5", L"ShowBorder", 0, iniPath) != 0;
        
        wchar_t buf[64];
        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"FoveatedWidth", L"0.55", buf, 64, iniPath)) {
            foveated_width = static_cast<float>(_wtof(buf));
            if (foveated_width < 0.0f) foveated_width = 0.0f;
            if (foveated_width > 1.0f) foveated_width = 1.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"FoveatedHeight", L"0.55", buf, 64, iniPath)) {
            foveated_height = static_cast<float>(_wtof(buf));
            if (foveated_height < 0.0f) foveated_height = 0.0f;
            if (foveated_height > 1.0f) foveated_height = 1.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"GazeSmoothingMs", L"35.0", buf, 64, iniPath)) {
            gaze_smoothing_ms = static_cast<float>(_wtof(buf));
            if (gaze_smoothing_ms < 0.0f) gaze_smoothing_ms = 0.0f;
            if (gaze_smoothing_ms > 120.0f) gaze_smoothing_ms = 120.0f;
        }

        gaze_quantization_pixels = GetPrivateProfileIntW(L"HLX_DLSS5", L"GazeQuantizationPixels", 8, iniPath);
        if (gaze_quantization_pixels < 2) gaze_quantization_pixels = 2;
        if (gaze_quantization_pixels > 32) gaze_quantization_pixels = 32;

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"GazeJumpResetRatio", L"0.60", buf, 64, iniPath)) {
            gaze_jump_reset_ratio = static_cast<float>(_wtof(buf));
            if (gaze_jump_reset_ratio < 0.10f) gaze_jump_reset_ratio = 0.10f;
            if (gaze_jump_reset_ratio > 1.0f) gaze_jump_reset_ratio = 1.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"FoveatedRoundness", L"0.50", buf, 64, iniPath)) {
            foveated_roundness = static_cast<float>(_wtof(buf));
            if (foveated_roundness < 0.0f) foveated_roundness = 0.0f;
            if (foveated_roundness > 1.0f) foveated_roundness = 1.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"FoveatedFeather", L"0.12", buf, 64, iniPath)) {
            foveated_feather = static_cast<float>(_wtof(buf));
            if (foveated_feather < 0.01f) foveated_feather = 0.01f;
            if (foveated_feather > 0.50f) foveated_feather = 0.50f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"WorkingScale", L"0.50", buf, 64, iniPath)) {
            working_scale = static_cast<float>(_wtof(buf));
            if (working_scale < 0.20f) working_scale = 0.20f;
            if (working_scale > 1.50f) working_scale = 1.50f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"Sharpness", L"0.35", buf, 64, iniPath)) {
            sharpness = static_cast<float>(_wtof(buf));
            if (sharpness < 0.0f) sharpness = 0.0f;
            if (sharpness > 1.0f) sharpness = 1.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"DetailBoost", L"1.0", buf, 64, iniPath)) {
            detail_boost = static_cast<float>(_wtof(buf));
            if (detail_boost < 0.0f) detail_boost = 0.0f;
            if (detail_boost > 2.0f) detail_boost = 2.0f;
        }

        passes = GetPrivateProfileIntW(L"HLX_DLSS5", L"Passes", 1, iniPath);
        if (passes < 1) passes = 1;
        if (passes > 3) passes = 3;

        preset = GetPrivateProfileIntW(L"HLX_DLSS5", L"Preset", 2, iniPath);
        if (preset < 0 || preset > 3) preset = 2;

        style = GetPrivateProfileIntW(L"HLX_DLSS5", L"Style", 0, iniPath);
        if (style < 0 || style > 2) style = 0;

        auto_mask = GetPrivateProfileIntW(L"HLX_DLSS5", L"AutoMask", 1, iniPath) != 0;

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"LocalStructure", L"1.0", buf, 64, iniPath)) {
            local_structure = static_cast<float>(_wtof(buf));
            if (local_structure < 0.0f) local_structure = 0.0f;
            if (local_structure > 2.0f) local_structure = 2.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"LocalTone", L"0.0", buf, 64, iniPath)) {
            local_tone = static_cast<float>(_wtof(buf));
            if (local_tone < 0.0f) local_tone = 0.0f;
            if (local_tone > 2.0f) local_tone = 2.0f;
        }

        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"SkinStructure", L"-1.0", buf, 64, iniPath)) {
            skin_structure = static_cast<float>(_wtof(buf));
            if (skin_structure < -1.0f) skin_structure = -1.0f;
            if (skin_structure > 2.0f) skin_structure = 2.0f;
        }

        fallback_on_error = GetPrivateProfileIntW(L"HLX_DLSS5", L"FallbackOnError", 1, iniPath) != 0;
        enable_d3d11_interop = GetPrivateProfileIntW(L"HLX_DLSS5", L"EnableD3D11Interop", 1, iniPath) != 0;
        enable_vulkan_interop = GetPrivateProfileIntW(L"HLX_DLSS5", L"EnableVulkanInterop", 1, iniPath) != 0;
        use_shared_hardware_fence = GetPrivateProfileIntW(L"HLX_DLSS5", L"UseSharedHardwareFence", 1, iniPath) != 0;
        dump_frames = GetPrivateProfileIntW(L"HLX_DLSS5", L"DumpFrames", 0, iniPath) != 0;

        wchar_t pathBuf[MAX_PATH];
        if (GetPrivateProfileStringW(L"HLX_DLSS5", L"DLSSNR_Path", L"", pathBuf, MAX_PATH, iniPath) && wcslen(pathBuf) > 0) {
            dlssnr_path = pathBuf;
        }

        HLX_INFO("Config loaded from %ls: Enabled=%d, Foveated=%d (%.2fx%.2f, round=%.2f), WorkingScale=%.2f, Passes=%d, Sharpness=%.2f, DetailBoost=%.2f, Preset=%d, Style=%d",
            iniPath, enabled, foveated, foveated_width, foveated_height, foveated_roundness, working_scale, passes, sharpness, detail_boost, preset, style);
    }

    void Save(const wchar_t* iniFilename = L"hlx_dlss5.ini") const {
        wchar_t modulePath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_dummyConfigSymbol),
            &hModule
        );
        GetModuleFileNameW(hModule, modulePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
        std::wstring fullIniPath = (lastSlash ? std::wstring(modulePath, lastSlash + 1) : L".\\") + iniFilename;
        const wchar_t* iniPath = fullIniPath.c_str();

        wchar_t buf[64];
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Enabled", enabled ? L"1" : L"0", iniPath);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Foveated", foveated ? L"1" : L"0", iniPath);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"EyeTracking", eye_tracking ? L"1" : L"0", iniPath);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"ShowBorder", show_border ? L"1" : L"0", iniPath);

        swprintf_s(buf, L"%.2f", foveated_width);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"FoveatedWidth", buf, iniPath);
        swprintf_s(buf, L"%.2f", foveated_height);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"FoveatedHeight", buf, iniPath);
        swprintf_s(buf, L"%.1f", gaze_smoothing_ms);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"GazeSmoothingMs", buf, iniPath);
        swprintf_s(buf, L"%d", gaze_quantization_pixels);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"GazeQuantizationPixels", buf, iniPath);
        swprintf_s(buf, L"%.2f", gaze_jump_reset_ratio);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"GazeJumpResetRatio", buf, iniPath);
        swprintf_s(buf, L"%.2f", foveated_roundness);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"FoveatedRoundness", buf, iniPath);
        swprintf_s(buf, L"%.2f", foveated_feather);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"FoveatedFeather", buf, iniPath);
        swprintf_s(buf, L"%.2f", working_scale);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"WorkingScale", buf, iniPath);
        swprintf_s(buf, L"%.2f", sharpness);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Sharpness", buf, iniPath);
        swprintf_s(buf, L"%.2f", detail_boost);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"DetailBoost", buf, iniPath);

        swprintf_s(buf, L"%d", passes);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Passes", buf, iniPath);
        swprintf_s(buf, L"%d", preset);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Preset", buf, iniPath);
        swprintf_s(buf, L"%d", style);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"Style", buf, iniPath);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"AutoMask", auto_mask ? L"1" : L"0", iniPath);

        swprintf_s(buf, L"%.2f", local_structure);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"LocalStructure", buf, iniPath);
        swprintf_s(buf, L"%.2f", local_tone);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"LocalTone", buf, iniPath);
        swprintf_s(buf, L"%.2f", skin_structure);
        WritePrivateProfileStringW(L"HLX_DLSS5", L"SkinStructure", buf, iniPath);

        HLX_INFO("Config saved to %ls successfully.", iniPath);
    }
};

} // namespace hlx
