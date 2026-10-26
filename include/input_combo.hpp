#pragma once
#include <algorithm>
#include <cmath>
#include <chrono>

namespace hlx {

struct ControllerInputs {
    // Left Controller
    bool leftDpadTouched = false;       // Pouce posé sur le D-Pad gauche (surface D-Pad / boutons X, Y)
    bool thumbOnLeftStick = false;      // Pouce sur le joystick gauche (stick cap touch, ou joystick dévié pour marcher)
    float leftStickX = 0.0f;
    float leftStickY = 0.0f;

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
};

class InputComboEvaluator {
public:
    static constexpr int64_t TOGGLE_COOLDOWN_MS = 1200;

    ComboOutputs Evaluate(const ControllerInputs& in, std::chrono::steady_clock::time_point now) {
        ComboOutputs out;

        // 1. Right Majeur (Grip / Trigger) >= 70%
        float maxRightMajeur = (std::max)(in.rightGripPull, in.rightTriggerPull);
        bool rightMajeurPressed = (maxRightMajeur >= 0.70f) || in.rightUseGrip;

        // 2. Right Stick vers le haut (> 0.40)
        bool rightStickUp = in.rightStickY > 0.40f;

        // 3. Validation stricte du D-Pad gauche :
        // Le D-Pad gauche est valide UNIQUEMENT si le pouce n'est PAS sur le joystick gauche !
        bool validLeftDpad = in.leftDpadTouched && !in.thumbOnLeftStick;

        // 4. Combinaisons exactes :
        // - Bascule DLSS 5 : D-Pad gauche + Majeur droit (sans stick droit vers le haut)
        bool dlssCombo = (validLeftDpad && rightMajeurPressed && !rightStickUp) || in.keyDlssToggle;

        // - Bascule Menu : D-Pad gauche + Joystick droit vers le haut (sans majeur droit)
        bool menuCombo = (validLeftDpad && rightStickUp && !rightMajeurPressed) || in.keyMenuToggle;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastToggleTime).count();

        // Réarmement de la porte : toutes les commandes du combo doivent être relâchées
        if (!in.leftDpadTouched && !rightMajeurPressed && !rightStickUp &&
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
    }

    bool IsGateOpen() const { return m_gateOpen; }

private:
    bool m_gateOpen = true;
    std::chrono::steady_clock::time_point m_lastToggleTime{};
};

} // namespace hlx
