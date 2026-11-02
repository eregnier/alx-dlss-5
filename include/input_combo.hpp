#pragma once
#include <algorithm>
#include <cmath>
#include <chrono>

namespace hlx {

struct ControllerInputs {
    // Left Controller
    bool leftDpadTouched = false;       // Pouce posé sur le D-Pad gauche (surface D-Pad / boutons X, Y)
    bool leftFaceBtnHeld = false;       // Bouton X/A gauche maintenu enfoncé (HealthPen / LaserSight - universel Pimax/Index/Quest)
    bool thumbOnLeftStick = false;      // Pouce sur le joystick gauche (stick cap touch, ou joystick dévié pour marcher)
    float leftStickX = 0.0f;
    float leftStickY = 0.0f;
    float leftGripPull = 0.0f;          // Grip gauche [0.0 - 1.0]
    bool leftUseGrip = false;
    float leftTriggerPull = 0.0f;       // Gâchette gauche [0.0 - 1.0]

    // Right Controller
    float rightGripPull = 0.0f;         // Majeur droit grip [0.0 - 1.0]
    float rightTriggerPull = 0.0f;      // Gâchette droite [0.0 - 1.0]
    bool rightUseGrip = false;
    float rightStickX = 0.0f;
    float rightStickY = 0.0f;           // Joystick droit vertical [-1.0 - 1.0]

    // Keyboard fallbacks
    bool keyDlssToggle = false;
    bool keyMenuToggle = false;
};

struct ComboOutputs {
    bool triggerDlssToggle = false;     // Bascule DLSS 5 (silencieux / bips, sans menu)
    bool triggerMenuToggle = false;     // Afficher / masquer le menu de réglages VR
    bool longPressHalfwayBeep = false;  // Petit bip d'armement à 1.5s lors d'un appui long (3s)
};

// Raw left-thumb evidence, captured once per frame by the OpenVR proxy.
struct LeftThumbSignals {
    // Positive evidence: thumb is on the flat face area of the left controller.
    bool faceButtonClicked = false;     // X / Y / HealthPen / LaserSight actually pressed
    float thumbCurl = 0.0f;             // SteamVR skeletal thumb curl [0..1]
    float thumbCurlAnalog = 0.0f;       // /actions/default/in/FingerCurlThumb [0..1] (unmapped in Alyx, usually 0)

    // Negative evidence: thumb is on the joystick instead.
    bool stickClicked = false;          // joystick press (/actions/move/in/Walk)
    bool stickDeflected = false;        // joystick pushed in any direction (/actions/move/in/Teleport)
    float stickX = 0.0f;
    float stickY = 0.0f;
};

struct LeftThumbState {
    bool onFacePlate = false;           // arms the shortcuts
    bool onStick = false;               // must never arm anything
};

// Measured on Quest 3 / oculus_touch (hlx_dlss5.log, 361 samples): the SteamVR
// emulated skeleton only reports the joystick cap touch (thumb curl 0.41) and is
// identical for FromDevice and FromAnimation (358/361 samples): it is NOT a
// position sensor. The flat face plate ("/input/thumbrest/touch" in the driver
// profile) is not bound by any Alyx action, so the plate reads as an open hand
// (0.00-0.08) while the joystick reads 0.41.
// shortcut: single-controller calibration, re-measure with the F9 [CAPTURE] log
// if another controller reports different curl values.
inline constexpr float STICK_TOUCH_CURL_MIN = 0.25f;   // >= : thumb is on the joystick cap
inline constexpr float THUMB_PRESENT_CURL_MIN = 0.02f; // >  : thumb is on the controller at all
inline constexpr float STICK_DEFLECT_MIN = 0.20f;

inline LeftThumbState ClassifyLeftThumb(const LeftThumbSignals& s) {
    LeftThumbState st;
    st.onStick = s.stickClicked || s.stickDeflected ||
                 std::abs(s.stickX) > STICK_DEFLECT_MIN ||
                 std::abs(s.stickY) > STICK_DEFLECT_MIN ||
                 s.thumbCurl >= STICK_TOUCH_CURL_MIN;
    // Stick evidence always wins: a thumb on the joystick must never arm a combo.
    st.onFacePlate = !st.onStick &&
                     (s.faceButtonClicked ||
                      s.thumbCurl > THUMB_PRESENT_CURL_MIN ||
                      s.thumbCurlAnalog >= 0.40f);
    return st;
}

class InputComboEvaluator {
public:
    static constexpr int64_t TOGGLE_COOLDOWN_MS = 1200;
    static constexpr int64_t LONG_PRESS_MENU_MS = 3000;
    static constexpr int64_t LONG_PRESS_BEEP_MS = 1500;

    ComboOutputs Evaluate(const ControllerInputs& in, std::chrono::steady_clock::time_point now) {
        ComboOutputs out;

        // 1. Two distinct right-hand fingers >= 70%:
        // - Index = analog trigger (TriggerPull)        -> VR settings menu
        // - Middle = grip / hand curl (HandCurl, UseGrip) -> DLSS 5 toggle
        bool rightGripPressed = (in.rightGripPull >= 0.70f) || in.rightUseGrip;
        bool rightTriggerPressed = in.rightTriggerPull >= 0.70f;
        bool leftGripPressed = (in.leftGripPull >= 0.75f) || in.leftUseGrip;
        bool leftTriggerPressed = in.leftTriggerPull >= 0.50f;

        // 2. Right stick up (> 0.40): secondary menu shortcut
        bool rightStickUp = in.rightStickY > 0.40f;

        // 3. Strict left D-Pad / Face button validation:
        // Valid on Quest 3 (capacitive face plate) AND on Pimax / Index / Vive (holding Left X/A button),
        // ONLY while the left joystick is not being pushed/clicked.
        bool sticksNeutral = (std::abs(in.leftStickX) <= STICK_DEFLECT_MIN) &&
                             (std::abs(in.leftStickY) <= STICK_DEFLECT_MIN) &&
                             (std::abs(in.rightStickX) <= STICK_DEFLECT_MIN) &&
                             (std::abs(in.rightStickY) <= STICK_DEFLECT_MIN);
        bool validLeftDpad = (in.leftDpadTouched || in.leftFaceBtnHeld) && !in.thumbOnLeftStick;

        // 4. Universal Long-Press (3.0s) Menu Toggle for Pimax / Index / Vive / Quest:
        //    Mode A: Hold Left X/A button (HealthPen) alone for 3.0s while sticks & right hand are idle.
        //    Mode B: Hold BOTH Left Grip + Right Grip (>= 75%) together for 3.0s while triggers & sticks are idle.
        bool longPressCandidate =
            sticksNeutral && !rightTriggerPressed && !leftTriggerPressed &&
            ((in.leftFaceBtnHeld && !rightGripPressed && !in.thumbOnLeftStick) ||
             (leftGripPressed && rightGripPressed && !in.leftFaceBtnHeld && !in.leftDpadTouched));

        bool longPressTriggered = false;
        if (longPressCandidate) {
            if (!m_longPressActive) {
                m_longPressActive = true;
                m_longPressHalfwayBeepSent = false;
                m_longPressTriggered = false;
                m_longPressStart = now;
            } else if (!m_longPressTriggered) {
                auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_longPressStart).count();
                if (heldMs >= LONG_PRESS_BEEP_MS && !m_longPressHalfwayBeepSent) {
                    m_longPressHalfwayBeepSent = true;
                    out.longPressHalfwayBeep = true;
                }
                if (heldMs >= LONG_PRESS_MENU_MS) {
                    m_longPressTriggered = true;
                    longPressTriggered = true;
                }
            }
        } else {
            m_longPressActive = false;
            m_longPressHalfwayBeepSent = false;
            m_longPressTriggered = false;
        }

        // 5. Instant 2-hand combos (grip + trigger together = ambiguous, nothing fires):
        // - DLSS 5 toggle: (Left D-Pad or Left X/A button) + Right middle-finger grip
        bool dlssCombo = (validLeftDpad && rightGripPressed && !rightTriggerPressed && !leftGripPressed) || in.keyDlssToggle;

        // - Menu toggle: (Left D-Pad or Left X/A button) + Right index trigger (or Right stick up), OR 3s long-press, OR keyboard
        bool menuCombo = (validLeftDpad && (rightTriggerPressed || rightStickUp) && !rightGripPressed) ||
                         longPressTriggered ||
                         in.keyMenuToggle;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastToggleTime).count();

        // Rearm gate: every combo control must be released
        if (!in.leftDpadTouched && !in.leftFaceBtnHeld && !leftGripPressed &&
            !rightGripPressed && !rightTriggerPressed && !rightStickUp &&
            !in.keyDlssToggle && !in.keyMenuToggle &&
            (in.rightGripPull < 0.30f) && (in.rightTriggerPull < 0.30f)) {
            if (elapsed > TOGGLE_COOLDOWN_MS) {
                m_gateOpen = true;
            }
        }

        // Déclenchement si la porte est ouverte et le délai minimum de cooldown écoulé
        if (m_gateOpen && (elapsed > TOGGLE_COOLDOWN_MS)) {
            if (dlssCombo) {
                m_gateOpen = false;
                m_lastToggleTime = now;
                out.triggerDlssToggle = true;
            } else if (menuCombo) {
                m_gateOpen = false;
                m_lastToggleTime = now;
                out.triggerMenuToggle = true;
            }
        }

        return out;
    }

    void Reset() {
        m_gateOpen = true;
        m_lastToggleTime = std::chrono::steady_clock::time_point{};
        m_longPressActive = false;
        m_longPressHalfwayBeepSent = false;
        m_longPressTriggered = false;
    }

    bool IsGateOpen() const { return m_gateOpen; }

private:
    bool m_gateOpen = true;
    std::chrono::steady_clock::time_point m_lastToggleTime{};
    bool m_longPressActive = false;
    bool m_longPressHalfwayBeepSent = false;
    bool m_longPressTriggered = false;
    std::chrono::steady_clock::time_point m_longPressStart{};
};

} // namespace hlx
