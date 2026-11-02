#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include "../external/openvr/headers/openvr.h"
#include "hlx_logger.hpp"
#include "hlx_config.hpp"
#include "stereo_pipeline.hpp"
#include "dlss5_bridge.hpp"
#include "gaze_tracker.hpp"

namespace hlx {

class VRMenu {
public:
    static constexpr int MENU_WIDTH = 560;
    static constexpr int MENU_HEIGHT = 548;
    static constexpr int ROW_COUNT = 17;

    static VRMenu& Instance() {
        static VRMenu instance;
        return instance;
    }

    VRMenu() {
        m_pixelsVR.resize(MENU_WIDTH * MENU_HEIGHT * 4, 0);
        InitGDIRasterizer();
    }

    ~VRMenu() {
        if (m_hGdiBmp) DeleteObject(m_hGdiBmp);
        if (m_hGdiMemDC) DeleteDC(m_hGdiMemDC);
    }

    [[nodiscard]] bool IsVisible() const noexcept {
        return m_visible;
    }

    void Show(vr::IVROverlay* pOverlay) {
        m_visible = true;
        m_dirty = true;
        if (pOverlay) {
            EnsureOverlay(pOverlay);
            if (m_hOverlay != vr::k_ulOverlayHandleInvalid) {
                pOverlay->ShowOverlay(m_hOverlay);
            }
        }
        HLX_INFO("VRMenu: Opened and displayed in VR headset.");
    }

    void Hide(vr::IVROverlay* pOverlay) {
        m_visible = false;
        if (pOverlay && m_hOverlay != vr::k_ulOverlayHandleInvalid) {
            pOverlay->HideOverlay(m_hOverlay);
        }
        HLX_INFO("VRMenu: Closed and hidden from VR headset.");
    }

    void Toggle(vr::IVROverlay* pOverlay) {
        if (m_visible) {
            Hide(pOverlay);
        } else {
            Show(pOverlay);
        }
    }

    void NavigateUp() {
        if (m_selectedRow > 0) {
            m_selectedRow--;
            m_dirty = true;
            PlaySoundAsync(1000, 25);
        }
    }

    void NavigateDown() {
        if (m_selectedRow < ROW_COUNT - 1) {
            m_selectedRow++;
            m_dirty = true;
            PlaySoundAsync(1000, 25);
        }
    }

    void Adjust(int direction, Config& config, vr::IVROverlay* pOverlay = nullptr) {
        bool changed = false;
        switch (m_selectedRow) {
        case 0: // Sharpness [0.0 - 1.0]
            config.sharpness += direction * 0.05f;
            if (config.sharpness < 0.0f) config.sharpness = 0.0f;
            if (config.sharpness > 1.0f) config.sharpness = 1.0f;
            changed = true;
            break;

        case 1: // Detail Boost [0.0 - 2.0]
            config.detail_boost += direction * 0.05f;
            if (config.detail_boost < 0.0f) config.detail_boost = 0.0f;
            if (config.detail_boost > 2.0f) config.detail_boost = 2.0f;
            changed = true;
            break;

        case 2: // Foveated Width [0.00 - 1.00] (0% - 100%)
            config.foveated_width = std::round((config.foveated_width + direction * 0.05f) * 20.0f) / 20.0f;
            if (config.foveated_width < 0.0f) config.foveated_width = 0.0f;
            if (config.foveated_width > 1.0f) config.foveated_width = 1.0f;
            changed = true;
            break;

        case 3: // Foveated Height [0.00 - 1.00] (0% - 100%)
            config.foveated_height = std::round((config.foveated_height + direction * 0.05f) * 20.0f) / 20.0f;
            if (config.foveated_height < 0.0f) config.foveated_height = 0.0f;
            if (config.foveated_height > 1.0f) config.foveated_height = 1.0f;
            changed = true;
            break;

        case 4: // Foveated Roundness [0.0 - 1.0]
            config.foveated_roundness += direction * 0.05f;
            if (config.foveated_roundness < 0.0f) config.foveated_roundness = 0.0f;
            if (config.foveated_roundness > 1.0f) config.foveated_roundness = 1.0f;
            changed = true;
            break;

        case 5: // Foveated Feather [0.01 - 0.30]
            config.foveated_feather += direction * 0.01f;
            if (config.foveated_feather < 0.01f) config.foveated_feather = 0.01f;
            if (config.foveated_feather > 0.30f) config.foveated_feather = 0.30f;
            changed = true;
            break;

        case 6: // Preset [0 - 3]
            config.preset = (config.preset + direction + 4) % 4;
            changed = true;
            break;

        case 7: // Style [0 - 2]
            config.style = (config.style + direction + 3) % 3;
            changed = true;
            break;

        case 8: // Local Structure [0.0 - 2.0]
            config.local_structure += direction * 0.05f;
            if (config.local_structure < 0.0f) config.local_structure = 0.0f;
            if (config.local_structure > 2.0f) config.local_structure = 2.0f;
            changed = true;
            break;

        case 9: // Local Tone [0.0 - 2.0]
            config.local_tone += direction * 0.05f;
            if (config.local_tone < 0.0f) config.local_tone = 0.0f;
            if (config.local_tone > 2.0f) config.local_tone = 2.0f;
            changed = true;
            break;

        case 10: // Skin Structure [-1.0 - 2.0]
            config.skin_structure += direction * 0.10f;
            if (config.skin_structure < -1.0f) config.skin_structure = -1.0f;
            if (config.skin_structure > 2.0f) config.skin_structure = 2.0f;
            changed = true;
            break;

        case 11: // Auto Mask [Toggle]
            config.auto_mask = !config.auto_mask;
            changed = true;
            break;

        case 12: // Show Border [Toggle]
            config.show_border = !config.show_border;
            changed = true;
            break;

        case 13: // Working Scale [0.35 - 1.00]
            config.working_scale += direction * 0.05f;
            if (config.working_scale < 0.35f) config.working_scale = 0.35f;
            if (config.working_scale > 1.00f) config.working_scale = 1.00f;
            changed = true;
            break;

        case 14: // Passes [1 - 3]
            config.passes = ((config.passes - 1 + direction + 3) % 3) + 1;
            changed = true;
            break;

        case 15: // Dynamic Foveated Rendering / Eye Tracking [Toggle]
            config.eye_tracking = !config.eye_tracking;
            changed = true;
            break;

        case 16: // [ FERMER LE MENU ]
            Hide(pOverlay);
            PlaySoundAsync(600, 60);
            return;
        }

        if (changed) {
            config.Save();
            StereoPipeline::Instance().UpdateConfig(config);
            DLSS5Bridge::Instance().UpdateConfig(config);
            m_dirty = true;
            PlaySoundAsync(1400, 35);
        }
    }

    void UpdateOverlay(vr::IVROverlay* pOverlay, const Config& config) {
        if (!m_visible || !pOverlay) return;

        EnsureOverlay(pOverlay);
        if (m_hOverlay == vr::k_ulOverlayHandleInvalid) return;

        bool hwGazeNow = GazeTracker::Instance().IsHardwareGazeActive();
        if (hwGazeNow != m_lastRenderedGazeActive) {
            m_lastRenderedGazeActive = hwGazeNow;
            m_dirty = true;
        }

        if (m_dirty) {
            Render(config);
            vr::EVROverlayError err = pOverlay->SetOverlayRaw(
                m_hOverlay,
                m_pixelsVR.data(),
                MENU_WIDTH,
                MENU_HEIGHT,
                4
            );
            if (err == vr::VROverlayError_None) {
                m_dirty = false;
            } else {
                HLX_WARN("SetOverlayRaw returned %d", err);
            }
        }
    }

private:
    bool m_visible = false;
    bool m_dirty = true;
    bool m_lastRenderedGazeActive = false;
    int m_selectedRow = 0;
    vr::VROverlayHandle_t m_hOverlay = vr::k_ulOverlayHandleInvalid;

    HDC m_hGdiMemDC = nullptr;
    HBITMAP m_hGdiBmp = nullptr;
    uint32_t* m_pGdiBits = nullptr;
    std::vector<uint8_t> m_pixelsVR;

    void PlaySoundAsync(DWORD freq, DWORD duration) {
        std::thread([freq, duration]() {
            Beep(freq, duration);
        }).detach();
    }

    void InitGDIRasterizer() {
        if (m_hGdiMemDC) return;
        HDC hdcScreen = GetDC(nullptr);
        m_hGdiMemDC = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = MENU_WIDTH;
        bmi.bmiHeader.biHeight = -MENU_HEIGHT; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        m_hGdiBmp = CreateDIBSection(m_hGdiMemDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_pGdiBits), nullptr, 0);
        SelectObject(m_hGdiMemDC, m_hGdiBmp);
        ReleaseDC(nullptr, hdcScreen);
    }

    bool EnsureOverlay(vr::IVROverlay* pOverlay) {
        if (!pOverlay) return false;
        if (m_hOverlay != vr::k_ulOverlayHandleInvalid) return true;

        char overlayKey[64];
        sprintf_s(overlayKey, sizeof(overlayKey), "HLX_DLSS5_MENU_%lu", GetCurrentProcessId());

        vr::EVROverlayError err = pOverlay->CreateOverlay(overlayKey, "DLSS 5 Settings Menu", &m_hOverlay);
        if (err != vr::VROverlayError_None) {
            HLX_ERROR("CreateOverlay failed with error: %d", err);
            m_hOverlay = vr::k_ulOverlayHandleInvalid;
            return false;
        }

        pOverlay->SetOverlayWidthInMeters(m_hOverlay, 0.58f);
        pOverlay->SetOverlayAlpha(m_hOverlay, 0.95f);

        // Position overlay in front of HMD: 70cm distance, slightly lowered for natural gaze
        vr::HmdMatrix34_t mat = {};
        mat.m[0][0] = 1.0f;
        mat.m[1][1] = 1.0f;
        mat.m[2][2] = 1.0f;
        mat.m[0][3] = 0.0f;   // Horizontal center
        mat.m[1][3] = -0.06f; // Slightly below eye level
        mat.m[2][3] = -0.70f; // 70cm forward

        pOverlay->SetOverlayTransformTrackedDeviceRelative(m_hOverlay, vr::k_unTrackedDeviceIndex_Hmd, &mat);
        HLX_INFO("Created and positioned SteamVR overlay for HLX DLSS5 Menu.");
        return true;
    }

    static void DrawMiniSlider(HDC hdc, int x, int y, int w, int h, float norm01, COLORREF fillColor, COLORREF thumbColor) {
        if (norm01 < 0.0f) norm01 = 0.0f;
        if (norm01 > 1.0f) norm01 = 1.0f;

        HBRUSH hTrackBg = CreateSolidBrush(RGB(22, 32, 48));
        HPEN hTrackPen = CreatePen(PS_SOLID, 1, RGB(45, 68, 98));
        HBRUSH hOldB = (HBRUSH)SelectObject(hdc, hTrackBg);
        HPEN hOldP = (HPEN)SelectObject(hdc, hTrackPen);
        RoundRect(hdc, x, y, x + w, y + h, 4, 4);

        int fillW = static_cast<int>(w * norm01);
        if (fillW > 0) {
            HBRUSH hFill = CreateSolidBrush(fillColor);
            HPEN hFillPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, hFill);
            SelectObject(hdc, hFillPen);
            RoundRect(hdc, x, y, x + fillW, y + h, 4, 4);
            DeleteObject(hFill);
            DeleteObject(hFillPen);
        }

        int thumbX = x + fillW;
        if (thumbX > x + w) thumbX = x + w;
        HBRUSH hThumb = CreateSolidBrush(thumbColor);
        HPEN hThumbPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        SelectObject(hdc, hThumb);
        SelectObject(hdc, hThumbPen);
        RoundRect(hdc, thumbX - 3, y - 2, thumbX + 3, y + h + 2, 4, 4);
        DeleteObject(hThumb);
        DeleteObject(hThumbPen);

        SelectObject(hdc, hOldB);
        SelectObject(hdc, hOldP);
        DeleteObject(hTrackBg);
        DeleteObject(hTrackPen);
    }

    void Render(const Config& config) {
        if (!m_hGdiMemDC || !m_pGdiBits) return;

        // Clear buffer
        memset(m_pGdiBits, 0, MENU_WIDTH * MENU_HEIGHT * 4);
        SetBkMode(m_hGdiMemDC, TRANSPARENT);

        HFONT hFontTitle = CreateFontA(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontMain = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontValue = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontBadge = CreateFontA(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontHelp = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        // 1. Background Card
        HBRUSH hBrushCard = CreateSolidBrush(RGB(14, 18, 26));
        HPEN hPenBorder = CreatePen(PS_SOLID, 1, RGB(0, 180, 235)); // Cyan neon outline
        HBRUSH hOldBrush = (HBRUSH)SelectObject(m_hGdiMemDC, hBrushCard);
        HPEN hOldPen = (HPEN)SelectObject(m_hGdiMemDC, hPenBorder);
        RoundRect(m_hGdiMemDC, 1, 1, MENU_WIDTH - 1, MENU_HEIGHT - 1, 14, 14);

        // 2. Header: Title
        SelectObject(m_hGdiMemDC, hFontTitle);
        SetTextColor(m_hGdiMemDC, RGB(0, 220, 255));
        TextOutA(m_hGdiMemDC, 16, 8, "DLSS 5 <> Half-Life: Alyx VR", 28);

        // Header Status Badge
        SelectObject(m_hGdiMemDC, hFontBadge);
        char badgeBuf[32];
        sprintf_s(badgeBuf, sizeof(badgeBuf), "%s  |  Preset %d", config.enabled ? "ACTIF" : "BYPASS", config.preset);
        COLORREF badgeBg = config.enabled ? RGB(16, 75, 42) : RGB(100, 24, 24);
        COLORREF badgeBorder = config.enabled ? RGB(45, 200, 100) : RGB(220, 60, 60);
        COLORREF badgeText = config.enabled ? RGB(220, 255, 230) : RGB(255, 220, 220);

        HBRUSH hBrushBadge = CreateSolidBrush(badgeBg);
        HPEN hPenBadge = CreatePen(PS_SOLID, 1, badgeBorder);
        SelectObject(m_hGdiMemDC, hBrushBadge);
        SelectObject(m_hGdiMemDC, hPenBadge);
        RoundRect(m_hGdiMemDC, MENU_WIDTH - 146, 6, MENU_WIDTH - 12, 28, 8, 8);
        DeleteObject(hBrushBadge);
        DeleteObject(hPenBadge);

        SetTextColor(m_hGdiMemDC, badgeText);
        RECT rcBadge = { MENU_WIDTH - 146, 6, MENU_WIDTH - 12, 28 };
        DrawTextA(m_hGdiMemDC, badgeBuf, -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Header Separator Line
        HPEN hPenSep = CreatePen(PS_SOLID, 1, RGB(32, 54, 82));
        SelectObject(m_hGdiMemDC, hPenSep);
        MoveToEx(m_hGdiMemDC, 14, 34, nullptr);
        LineTo(m_hGdiMemDC, MENU_WIDTH - 14, 34);
        DeleteObject(hPenSep);

        // 3. Render Rows
        int startY = 40;
        int rowHeight = 26;

        for (int i = 0; i < ROW_COUNT; ++i) {
            int y = startY + i * rowHeight;
            bool isActive = (i == m_selectedRow);

            if (isActive) {
                HBRUSH hBrushRow = CreateSolidBrush(RGB(24, 46, 76));
                HPEN hPenRow = CreatePen(PS_SOLID, 1, RGB(0, 170, 255));
                SelectObject(m_hGdiMemDC, hBrushRow);
                SelectObject(m_hGdiMemDC, hPenRow);
                RoundRect(m_hGdiMemDC, 10, y, MENU_WIDTH - 10, y + 24, 6, 6);
                DeleteObject(hBrushRow);
                DeleteObject(hPenRow);

                SelectObject(m_hGdiMemDC, hFontMain);
                SetTextColor(m_hGdiMemDC, RGB(255, 230, 0));
                TextOutA(m_hGdiMemDC, 16, y + 3, ">", 1);
            }

            SelectObject(m_hGdiMemDC, hFontMain);
            COLORREF labelColor = isActive ? RGB(255, 255, 255) : RGB(170, 185, 205);
            SetTextColor(m_hGdiMemDC, labelColor);

            char valBuf[64];
            switch (i) {
            case 0: // Sharpness
                TextOutA(m_hGdiMemDC, 30, y + 3, "Nettete Fovea (Sharpness)", 25);
                sprintf_s(valBuf, sizeof(valBuf), "%.2f", config.sharpness);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(0, 240, 255) : RGB(140, 200, 230));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.sharpness, RGB(0, 160, 225), RGB(0, 255, 255));
                break;

            case 1: // Detail Boost
                TextOutA(m_hGdiMemDC, 30, y + 3, "Intensite IA (Detail Boost)", 27);
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", config.detail_boost);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.detail_boost / 2.0f, RGB(230, 170, 0), RGB(255, 220, 0));
                break;

            case 2: // Foveated Width (0% - 100%)
                TextOutA(m_hGdiMemDC, 30, y + 3, "Largeur Zone Fovea (0-100%)", 27);
                if (config.foveated_width <= 0.005f) sprintf_s(valBuf, sizeof(valBuf), "0%% [OFF]");
                else sprintf_s(valBuf, sizeof(valBuf), "%.0f%%", config.foveated_width * 100.0f);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(80, 255, 180) : RGB(160, 220, 180));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.foveated_width, RGB(0, 200, 140), RGB(80, 255, 180));
                break;

            case 3: // Foveated Height (0% - 100%)
                TextOutA(m_hGdiMemDC, 30, y + 3, "Hauteur Zone Fovea (0-100%)", 27);
                if (config.foveated_height <= 0.005f) sprintf_s(valBuf, sizeof(valBuf), "0%% [OFF]");
                else sprintf_s(valBuf, sizeof(valBuf), "%.0f%%", config.foveated_height * 100.0f);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(80, 255, 180) : RGB(160, 220, 180));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.foveated_height, RGB(0, 200, 140), RGB(80, 255, 180));
                break;

            case 4: // Foveated Roundness
                TextOutA(m_hGdiMemDC, 30, y + 3, "Forme Fovea (Roundness)", 23);
                sprintf_s(valBuf, sizeof(valBuf), "%.2f", config.foveated_roundness);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(220, 150, 255) : RGB(180, 160, 220));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.foveated_roundness, RGB(180, 100, 240), RGB(220, 150, 255));
                break;

            case 5: // Foveated Feather
                TextOutA(m_hGdiMemDC, 30, y + 3, "Fondu Fovea (Feather)", 21);
                sprintf_s(valBuf, sizeof(valBuf), "%.2f", config.foveated_feather);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(220, 150, 255) : RGB(180, 160, 220));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, (config.foveated_feather - 0.01f) / 0.29f, RGB(180, 100, 240), RGB(220, 150, 255));
                break;

            case 6: // DLSS-NR Preset
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Modele DLSS-NR (Preset)", 23);
                    static const char* presetNames[4] = {
                        "0: Defaut", "1: Ultra Quality", "2: Performance VR", "3: DLSS-D Neural RR"
                    };
                    SelectObject(m_hGdiMemDC, hFontValue);
                    SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                    TextOutA(m_hGdiMemDC, 245, y + 4, presetNames[config.preset % 4], static_cast<int>(strlen(presetNames[config.preset % 4])));
                }
                break;

            case 7: // Style
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Style Visuel DLSS 5", 19);
                    static const char* styleNames[3] = { "Standard", "Naturel", "Cinematique" };
                    SelectObject(m_hGdiMemDC, hFontValue);
                    SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                    TextOutA(m_hGdiMemDC, 245, y + 4, styleNames[config.style % 3], static_cast<int>(strlen(styleNames[config.style % 3])));
                }
                break;

            case 8: // Local Structure
                TextOutA(m_hGdiMemDC, 30, y + 3, "Structure Decor (Local)", 23);
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", config.local_structure);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(80, 255, 180) : RGB(160, 220, 180));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.local_structure / 2.0f, RGB(0, 200, 140), RGB(80, 255, 180));
                break;

            case 9: // Local Tone
                TextOutA(m_hGdiMemDC, 30, y + 3, "Tonalite Ombres (Tone)", 22);
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", config.local_tone);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(220, 150, 255) : RGB(180, 160, 220));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, config.local_tone / 2.0f, RGB(180, 100, 240), RGB(220, 150, 255));
                break;

            case 10: // Skin Structure
                TextOutA(m_hGdiMemDC, 30, y + 3, "Structure Peau (Faces)", 22);
                if (config.skin_structure < -0.99f) sprintf_s(valBuf, sizeof(valBuf), "[Auto]");
                else sprintf_s(valBuf, sizeof(valBuf), "%.2fx", config.skin_structure);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 180, 150) : RGB(220, 170, 160));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, (config.skin_structure + 1.0f) / 3.0f, RGB(240, 120, 90), RGB(255, 180, 150));
                break;

            case 11: // Auto Mask
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Masque Semantique (AutoMask)", 28);
                    SelectObject(m_hGdiMemDC, hFontBadge);
                    const char* txt = config.auto_mask ? "[ ACTIVE ]" : "[ DESACTIVE ]";
                    COLORREF cBg = config.auto_mask ? RGB(16, 60, 95) : RGB(95, 60, 16);
                    COLORREF cBd = config.auto_mask ? RGB(0, 180, 255) : RGB(255, 170, 0);
                    HBRUSH hb = CreateSolidBrush(cBg);
                    HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                    SelectObject(m_hGdiMemDC, hb);
                    SelectObject(m_hGdiMemDC, hp);
                    RoundRect(m_hGdiMemDC, 380, y + 2, 535, y + 22, 6, 6);
                    DeleteObject(hb);
                    DeleteObject(hp);
                    SetTextColor(m_hGdiMemDC, config.auto_mask ? RGB(180, 230, 255) : RGB(255, 220, 160));
                    RECT rc = { 380, y + 2, 535, y + 22 };
                    DrawTextA(m_hGdiMemDC, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                break;

            case 12: // Show Border
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Bordure Foveale Permanente", 26);
                    SelectObject(m_hGdiMemDC, hFontBadge);
                    const char* txt = config.show_border ? "[ OUI - VERTE ]" : "[ NON - INVISIBLE ]";
                    COLORREF cBg = config.show_border ? RGB(15, 95, 50) : RGB(50, 50, 50);
                    COLORREF cBd = config.show_border ? RGB(45, 230, 100) : RGB(140, 140, 140);
                    HBRUSH hb = CreateSolidBrush(cBg);
                    HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                    SelectObject(m_hGdiMemDC, hb);
                    SelectObject(m_hGdiMemDC, hp);
                    RoundRect(m_hGdiMemDC, 380, y + 2, 535, y + 22, 6, 6);
                    DeleteObject(hb);
                    DeleteObject(hp);
                    SetTextColor(m_hGdiMemDC, config.show_border ? RGB(200, 255, 210) : RGB(200, 200, 200));
                    RECT rc = { 380, y + 2, 535, y + 22 };
                    DrawTextA(m_hGdiMemDC, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                break;

            case 13: // Working Scale
                TextOutA(m_hGdiMemDC, 30, y + 3, "Echelle Interne IA (Scale)", 26);
                sprintf_s(valBuf, sizeof(valBuf), "%.0f%%", config.working_scale * 100.0f);
                SelectObject(m_hGdiMemDC, hFontValue);
                SetTextColor(m_hGdiMemDC, isActive ? RGB(0, 240, 255) : RGB(140, 200, 230));
                TextOutA(m_hGdiMemDC, 245, y + 4, valBuf, static_cast<int>(strlen(valBuf)));
                DrawMiniSlider(m_hGdiMemDC, 335, y + 8, 200, 8, (config.working_scale - 0.35f) / 0.65f, RGB(0, 160, 225), RGB(0, 255, 255));
                break;

            case 14: // Passes (1 / 2 / 3)
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Passes IA DLSS 5 (1/2/3)", 24);
                    static const char* passNames[3] = {
                        "1 Passe (Optimal VR)", "2 Passes (Renforce)", "3 Passes (Ultra)"
                    };
                    int pIdx = (config.passes >= 1 && config.passes <= 3) ? (config.passes - 1) : 0;
                    SelectObject(m_hGdiMemDC, hFontValue);
                    SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                    TextOutA(m_hGdiMemDC, 245, y + 4, passNames[pIdx], static_cast<int>(strlen(passNames[pIdx])));
                }
                break;

            case 15: // Dynamic Foveated Rendering / Eye Tracking
                {
                    TextOutA(m_hGdiMemDC, 30, y + 3, "Suivi Oculaire DFR (EyeTrack)", 29);
                    SelectObject(m_hGdiMemDC, hFontBadge);
                    bool hwActive = GazeTracker::Instance().IsHardwareGazeActive();
                    const char* txt = !config.eye_tracking
                        ? "[ DESACTIVE (FIXE) ]"
                        : (hwActive ? "[ ACTIF - REGARD DETECTE ]" : "[ AUTO - FIXE (PAS D'ET) ]");
                    COLORREF cBg = !config.eye_tracking
                        ? RGB(50, 50, 50)
                        : (hwActive ? RGB(15, 95, 50) : RGB(16, 60, 95));
                    COLORREF cBd = !config.eye_tracking
                        ? RGB(140, 140, 140)
                        : (hwActive ? RGB(45, 230, 100) : RGB(0, 180, 255));
                    HBRUSH hb = CreateSolidBrush(cBg);
                    HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                    SelectObject(m_hGdiMemDC, hb);
                    SelectObject(m_hGdiMemDC, hp);
                    RoundRect(m_hGdiMemDC, 335, y + 2, 535, y + 22, 6, 6);
                    DeleteObject(hb);
                    DeleteObject(hp);
                    SetTextColor(m_hGdiMemDC, !config.eye_tracking
                        ? RGB(200, 200, 200)
                        : (hwActive ? RGB(200, 255, 210) : RGB(180, 230, 255)));
                    RECT rc = { 335, y + 2, 535, y + 22 };
                    DrawTextA(m_hGdiMemDC, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                break;

            case 16: // [ FERMER LE MENU ]
                {
                    COLORREF bBg = isActive ? RGB(18, 70, 115) : RGB(15, 30, 48);
                    COLORREF bBd = isActive ? RGB(0, 230, 255) : RGB(40, 85, 130);
                    HBRUSH hb = CreateSolidBrush(bBg);
                    HPEN hp = CreatePen(PS_SOLID, 1, bBd);
                    SelectObject(m_hGdiMemDC, hb);
                    SelectObject(m_hGdiMemDC, hp);
                    RoundRect(m_hGdiMemDC, 28, y + 1, MENU_WIDTH - 28, y + 22, 6, 6);
                    DeleteObject(hb);
                    DeleteObject(hp);

                    SelectObject(m_hGdiMemDC, hFontBadge);
                    SetTextColor(m_hGdiMemDC, isActive ? RGB(255, 235, 120) : RGB(140, 210, 255));
                    RECT rcClose = { 28, y + 1, MENU_WIDTH - 28, y + 22 };
                    DrawTextA(m_hGdiMemDC, "[ FERMER LE MENU ]", -1, &rcClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                break;
            }
        }

        // 4. Footer
        int footerY = startY + ROW_COUNT * rowHeight + 4;
        HPEN hPenFoot = CreatePen(PS_SOLID, 1, RGB(30, 50, 75));
        SelectObject(m_hGdiMemDC, hPenFoot);
        MoveToEx(m_hGdiMemDC, 14, footerY, nullptr);
        LineTo(m_hGdiMemDC, MENU_WIDTH - 14, footerY);
        DeleteObject(hPenFoot);

        SelectObject(m_hGdiMemDC, hFontHelp);
        SetTextColor(m_hGdiMemDC, RGB(120, 160, 200));
        RECT rcHelp = { 10, footerY + 4, MENU_WIDTH - 10, MENU_HEIGHT - 3 };
        DrawTextA(m_hGdiMemDC, "Sticks/Fleches: Naviguer | Maintien X(G) 3s ou F8: Menu | Y(G)/Echap: Fermer", -1, &rcHelp, DT_CENTER | DT_SINGLELINE);

        // Cleanup GDI objects
        SelectObject(m_hGdiMemDC, hOldBrush);
        SelectObject(m_hGdiMemDC, hOldPen);
        DeleteObject(hBrushCard);
        DeleteObject(hPenBorder);
        DeleteObject(hFontTitle);
        DeleteObject(hFontMain);
        DeleteObject(hFontValue);
        DeleteObject(hFontBadge);
        DeleteObject(hFontHelp);

        // 5. Post-process to straight RGBA for OpenVR SetOverlayRaw
        for (int y = 0; y < MENU_HEIGHT; ++y) {
            int rowIdx = y * MENU_WIDTH;
            for (int x = 0; x < MENU_WIDTH; ++x) {
                int idx = rowIdx + x;
                uint32_t px = m_pGdiBits[idx];
                uint8_t b = static_cast<uint8_t>(px & 0xFF);
                uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFF);
                uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFF);

                uint32_t a = 0;
                if (r | g | b) {
                    if (r <= 20 && g <= 24 && b <= 32) {
                        a = 230; // 90% translucent dark card background
                    } else {
                        a = 255; // Solid text, sliders, borders
                    }
                }
                *reinterpret_cast<uint32_t*>(&m_pixelsVR[idx * 4]) =
                    (a << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
            }
        }
    }
};

} // namespace hlx
