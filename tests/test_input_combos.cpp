#include <cassert>
#include <iostream>
#include "input_combo.hpp"

using namespace hlx;

void TestDlssToggle_WithLeftDpadAndRightMajeur() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightGripPull = 0.75f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == true && "Left DPad + Right Majeur (Grip >= 70%) MUST trigger DLSS Toggle!");
    assert(out.triggerMenuToggle == false && "Must not trigger menu toggle!");
    std::cout << "[PASS] TestDlssToggle_WithLeftDpadAndRightMajeur" << std::endl;
}

void TestNoDlssToggle_WithThumbOnLeftStickAndRightMajeur() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Thumb is on the left joystick (even if skeletal curl triggered leftDpadTouched)
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = true;
    in.rightGripPull = 1.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == false && "Thumb on Joystick + Right Majeur MUST NOT trigger DLSS Toggle!");
    assert(out.triggerMenuToggle == false && "Must not trigger menu toggle!");
    std::cout << "[PASS] TestNoDlssToggle_WithThumbOnLeftStickAndRightMajeur" << std::endl;
}

void TestNoDlssToggle_WhileWalkingForwardAndGripping() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Player walking forward (stick deflected), thumb on joystick, holding weapon / grabbing with right hand
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = true;
    in.leftStickY = 0.85f;
    in.rightGripPull = 1.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == false && "Walking forward with joystick + gripping MUST NOT trigger DLSS Toggle!");
    std::cout << "[PASS] TestNoDlssToggle_WhileWalkingForwardAndGripping" << std::endl;
}

void TestMenuToggle_WithLeftDpadAndRightStickUp() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Left D-Pad + Right Stick Up
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightStickY = 0.65f;
    in.rightGripPull = 0.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerMenuToggle == true && "Left D-Pad + Right Stick Up MUST trigger Menu Toggle!");
    assert(out.triggerDlssToggle == false && "Must not trigger DLSS Toggle!");
    std::cout << "[PASS] TestMenuToggle_WithLeftDpadAndRightStickUp" << std::endl;
}

void TestNoMenuToggle_WithThumbOnLeftStickAndRightStickUp() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Left Stick (thumbOnLeftStick == true) + Right Stick Up
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = true;
    in.rightStickY = 0.65f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerMenuToggle == false && "Left Stick (not D-Pad) + Right Stick Up MUST NOT trigger Menu!");
    std::cout << "[PASS] TestNoMenuToggle_WithThumbOnLeftStickAndRightStickUp" << std::endl;
}

void TestNoMenuToggle_WithLeftDpadAndRightStickDown() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Left D-Pad + Right Stick Down (not Up)
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightStickY = -0.50f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerMenuToggle == false && "Right Stick Down must not trigger Menu!");
    std::cout << "[PASS] TestNoMenuToggle_WithLeftDpadAndRightStickDown" << std::endl;
}

void TestNoCombo_WithRightMajeurAlone() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftDpadTouched = false;
    in.thumbOnLeftStick = false;
    in.rightGripPull = 1.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == false && "Right Majeur alone must do nothing!");
    assert(out.triggerMenuToggle == false && "Right Majeur alone must do nothing!");
    std::cout << "[PASS] TestNoCombo_WithRightMajeurAlone" << std::endl;
}

void TestNoCombo_WithLeftDpadAlone() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightGripPull = 0.0f;
    in.rightStickY = 0.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == false && "Left DPad alone must do nothing!");
    assert(out.triggerMenuToggle == false && "Left DPad alone must do nothing!");
    std::cout << "[PASS] TestNoCombo_WithLeftDpadAlone" << std::endl;
}

void TestCooldownAndGateRearm() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightGripPull = 0.85f;

    // 1. Initial trigger
    auto out1 = eval.Evaluate(in, t0);
    assert(out1.triggerDlssToggle == true && "First toggle must succeed!");

    // 2. Still held after 400ms -> no re-trigger
    auto t1 = t0 + std::chrono::milliseconds(400);
    auto out2 = eval.Evaluate(in, t1);
    assert(out2.triggerDlssToggle == false && "Cannot re-trigger before cooldown!");

    // 3. Still held after 1500ms (cooldown passed, but controls NEVER released!) -> no re-trigger
    auto t2 = t0 + std::chrono::milliseconds(1500);
    auto out3 = eval.Evaluate(in, t2);
    assert(out3.triggerDlssToggle == false && "Cannot re-trigger without releasing controls!");

    // 4. Release controls at 1600ms
    ControllerInputs releasedIn;
    auto t3 = t0 + std::chrono::milliseconds(1600);
    eval.Evaluate(releasedIn, t3);
    assert(eval.IsGateOpen() == true && "Gate must open after releasing controls!");

    // 5. Press combo again at 1700ms -> successful second toggle!
    auto t4 = t0 + std::chrono::milliseconds(1700);
    auto out4 = eval.Evaluate(in, t4);
    assert(out4.triggerDlssToggle == true && "Second toggle must succeed after release and cooldown!");
    std::cout << "[PASS] TestCooldownAndGateRearm" << std::endl;
}

void TestKeyboardFallbacks() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs inDlss;
    inDlss.keyDlssToggle = true;
    auto outDlss = eval.Evaluate(inDlss, t0);
    assert(outDlss.triggerDlssToggle == true && "Keyboard DLSS toggle must work!");

    eval.Reset();
    ControllerInputs inMenu;
    inMenu.keyMenuToggle = true;
    auto outMenu = eval.Evaluate(inMenu, t0);
    assert(outMenu.triggerMenuToggle == true && "Keyboard Menu toggle must work!");
    std::cout << "[PASS] TestKeyboardFallbacks" << std::endl;
}

int main() {
    std::cout << "=== Running Input Combos Automated Test Suite ===" << std::endl;
    TestDlssToggle_WithLeftDpadAndRightMajeur();
    TestNoDlssToggle_WithThumbOnLeftStickAndRightMajeur();
    TestNoDlssToggle_WhileWalkingForwardAndGripping();
    TestMenuToggle_WithLeftDpadAndRightStickUp();
    TestNoMenuToggle_WithThumbOnLeftStickAndRightStickUp();
    TestNoMenuToggle_WithLeftDpadAndRightStickDown();
    TestNoCombo_WithRightMajeurAlone();
    TestNoCombo_WithLeftDpadAlone();
    TestCooldownAndGateRearm();
    TestKeyboardFallbacks();
    std::cout << "All Input Combos tests PASSED successfully!" << std::endl;
    return 0;
}
