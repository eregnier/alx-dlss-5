#undef NDEBUG // Build is Release: keep asserts live in this test
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

void TestMenuToggle_WithLeftDpadAndRightTrigger() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Left D-Pad + Right index trigger (gachette)
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightTriggerPull = 0.80f;
    in.rightGripPull = 0.0f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerMenuToggle == true && "Left D-Pad + Right Trigger MUST trigger Menu Toggle!");
    assert(out.triggerDlssToggle == false && "Right Trigger must NOT trigger DLSS Toggle!");
    std::cout << "[PASS] TestMenuToggle_WithLeftDpadAndRightTrigger" << std::endl;
}

void TestNoCombo_WithLeftDpadAndBothRightGripAndTrigger() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Ambiguous: grip (majeur) and trigger (index) squeezed together -> no action
    ControllerInputs in;
    in.leftDpadTouched = true;
    in.thumbOnLeftStick = false;
    in.rightGripPull = 0.90f;
    in.rightTriggerPull = 0.90f;

    auto out = eval.Evaluate(in, t0);
    assert(out.triggerDlssToggle == false && "Grip + Trigger together MUST NOT toggle DLSS!");
    assert(out.triggerMenuToggle == false && "Grip + Trigger together MUST NOT open Menu!");
    std::cout << "[PASS] TestNoCombo_WithLeftDpadAndBothRightGripAndTrigger" << std::endl;
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

// --- Left thumb classification: flat face plate vs joystick ------------------
// Measured on Quest 3: joystick cap touch -> thumb curl 0.41; face plate or thumb
// away -> open hand (0.00-0.08). The plate itself is not exposed by Alyx actions.

void TestThumbAway_NoFacePlateNoStick() {
    LeftThumbSignals s;
    s.thumbCurl = 0.00f; // measured: thumb not on the controller at all
    auto st = ClassifyLeftThumb(s);
    assert(st.onFacePlate == false && "Thumb away must not arm anything!");
    assert(st.onStick == false && "Thumb away is not on the stick!");
    std::cout << "[PASS] TestThumbAway_NoFacePlateNoStick" << std::endl;
}

void TestThumbOnStickCap_DoesNotArm() {
    // Regression: a thumb resting on a centered joystick cap reports curl 0.41 and
    // used to arm both shortcuts instead of the face plate.
    LeftThumbSignals s;
    s.thumbCurl = 0.41f; // measured: thumb on the joystick cap (capacitive touch)
    auto st = ClassifyLeftThumb(s);
    assert(st.onStick == true && "Joystick cap touch must be detected from the curl!");
    assert(st.onFacePlate == false && "Thumb on the joystick MUST NOT arm shortcuts!");

    InputComboEvaluator eval;
    ControllerInputs in;
    in.leftDpadTouched = st.onFacePlate;
    in.thumbOnLeftStick = st.onStick;
    in.rightTriggerPull = 0.85f;
    auto out = eval.Evaluate(in, std::chrono::steady_clock::now());
    assert(out.triggerMenuToggle == false && "Thumb on joystick + trigger MUST NOT open the menu!");
    assert(out.triggerDlssToggle == false && "Thumb on joystick + trigger MUST NOT toggle DLSS!");
    std::cout << "[PASS] TestThumbOnStickCap_DoesNotArm" << std::endl;
}

void TestThumbOnFacePlate_Arms() {
    LeftThumbSignals s;
    s.thumbCurl = 0.07f; // measured: thumb resting on the flat face plate
    auto st = ClassifyLeftThumb(s);
    assert(st.onFacePlate == true && "Thumb on the face plate must arm shortcuts!");
    assert(st.onStick == false && "Thumb on the face plate is not on the stick!");

    InputComboEvaluator eval;
    ControllerInputs in;
    in.leftDpadTouched = st.onFacePlate;
    in.thumbOnLeftStick = st.onStick;
    in.rightTriggerPull = 0.85f;
    auto out = eval.Evaluate(in, std::chrono::steady_clock::now());
    assert(out.triggerMenuToggle == true && "Face plate + right trigger MUST open the menu!");
    std::cout << "[PASS] TestThumbOnFacePlate_Arms" << std::endl;
}

void TestFacePlateWithDeflectedStick_IsStick() {
    // Face plate cannot coexist with a deflected stick: stick evidence wins.
    LeftThumbSignals s;
    s.thumbCurl = 0.07f;
    s.stickY = 0.35f;
    auto st = ClassifyLeftThumb(s);
    assert(st.onStick == true && "Deflected stick must be detected!");
    assert(st.onFacePlate == false && "Stick deflection must cancel face-plate detection!");
    std::cout << "[PASS] TestFacePlateWithDeflectedStick_IsStick" << std::endl;
}

void TestFacePlateWithStickClick_IsStick() {
    LeftThumbSignals s;
    s.thumbCurl = 0.07f;
    s.stickClicked = true;
    auto st = ClassifyLeftThumb(s);
    assert(st.onStick == true && "Stick click must be detected!");
    assert(st.onFacePlate == false && "Stick click must cancel face-plate detection!");
    std::cout << "[PASS] TestFacePlateWithStickClick_IsStick" << std::endl;
}

void TestFaceButtonClick_ArmsWithoutCurl() {
    LeftThumbSignals s;
    s.faceButtonClicked = true; // e.g. Y clicked: the thumb is physically on Y
    auto st = ClassifyLeftThumb(s);
    assert(st.onFacePlate == true && "Face button click must arm shortcuts!");
    std::cout << "[PASS] TestFaceButtonClick_ArmsWithoutCurl" << std::endl;
}

void TestFaceButtonClickWhileStickDeflected_IsStick() {
    LeftThumbSignals s;
    s.faceButtonClicked = true;
    s.stickX = -0.40f; // walking / turning
    auto st = ClassifyLeftThumb(s);
    assert(st.onFacePlate == false && "Face button while the stick is pushed MUST NOT arm!");
    std::cout << "[PASS] TestFaceButtonClickWhileStickDeflected_IsStick" << std::endl;
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

void TestLongPressLeftFaceButton_OpensMenuAt3s() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftFaceBtnHeld = true;

    // Frame 0: starts hold, does not fire yet
    auto out0 = eval.Evaluate(in, t0);
    assert(!out0.triggerMenuToggle && !out0.longPressHalfwayBeep);

    // At 1.0s: still holding, neither halfway beep nor menu toggle
    auto out1 = eval.Evaluate(in, t0 + std::chrono::milliseconds(1000));
    assert(!out1.triggerMenuToggle && !out1.longPressHalfwayBeep);

    // At 1.5s: halfway arming beep fires once
    auto outHalf = eval.Evaluate(in, t0 + std::chrono::milliseconds(1500));
    assert(outHalf.longPressHalfwayBeep == true && "Halfway beep must fire at 1.5s!");
    assert(!outHalf.triggerMenuToggle);

    // At 2.0s: halfway beep does not repeat
    auto out2 = eval.Evaluate(in, t0 + std::chrono::milliseconds(2000));
    assert(!out2.longPressHalfwayBeep && !out2.triggerMenuToggle);

    // At 3.0s: menu toggle fires!
    auto out3 = eval.Evaluate(in, t0 + std::chrono::milliseconds(3000));
    assert(out3.triggerMenuToggle == true && "3s long press on Left X/A must open VR Menu!");

    // At 4.5s (still held): must not re-fire without releasing
    auto out4 = eval.Evaluate(in, t0 + std::chrono::milliseconds(4500));
    assert(!out4.triggerMenuToggle);
    std::cout << "[PASS] TestLongPressLeftFaceButton_OpensMenuAt3s" << std::endl;
}

void TestLongPressBothGrips_OpensMenuAt3s() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    ControllerInputs in;
    in.leftGripPull = 0.85f;
    in.rightGripPull = 0.85f;

    assert(!eval.Evaluate(in, t0).triggerMenuToggle);
    assert(eval.Evaluate(in, t0 + std::chrono::milliseconds(1550)).longPressHalfwayBeep);
    assert(eval.Evaluate(in, t0 + std::chrono::milliseconds(3050)).triggerMenuToggle);
    std::cout << "[PASS] TestLongPressBothGrips_OpensMenuAt3s" << std::endl;
}

void TestPimaxController_LeftFaceButtonCombos() {
    InputComboEvaluator eval;
    auto t0 = std::chrono::steady_clock::now();

    // Pimax Crystal controller: no capacitive flat plate (leftDpadTouched=false), but Left X held (leftFaceBtnHeld=true)
    ControllerInputs inMenu;
    inMenu.leftFaceBtnHeld = true;
    inMenu.rightTriggerPull = 0.80f;
    auto outMenu = eval.Evaluate(inMenu, t0);
    assert(outMenu.triggerMenuToggle == true && "Pimax Left X + Right Trigger must open VR menu!");

    eval.Reset();
    ControllerInputs inDlss;
    inDlss.leftFaceBtnHeld = true;
    inDlss.rightGripPull = 0.80f;
    auto outDlss = eval.Evaluate(inDlss, t0);
    assert(outDlss.triggerDlssToggle == true && "Pimax Left X + Right Grip must toggle DLSS 5!");
    std::cout << "[PASS] TestPimaxController_LeftFaceButtonCombos" << std::endl;
}

int main() {
    std::cout << "=== Running Input Combos Automated Test Suite ===" << std::endl;
    TestDlssToggle_WithLeftDpadAndRightMajeur();
    TestNoDlssToggle_WithThumbOnLeftStickAndRightMajeur();
    TestNoDlssToggle_WhileWalkingForwardAndGripping();
    TestMenuToggle_WithLeftDpadAndRightStickUp();
    TestMenuToggle_WithLeftDpadAndRightTrigger();
    TestNoCombo_WithLeftDpadAndBothRightGripAndTrigger();
    TestNoMenuToggle_WithThumbOnLeftStickAndRightStickUp();
    TestNoMenuToggle_WithLeftDpadAndRightStickDown();
    TestNoCombo_WithRightMajeurAlone();
    TestNoCombo_WithLeftDpadAlone();
    TestCooldownAndGateRearm();
    TestThumbAway_NoFacePlateNoStick();
    TestThumbOnStickCap_DoesNotArm();
    TestThumbOnFacePlate_Arms();
    TestFacePlateWithDeflectedStick_IsStick();
    TestFacePlateWithStickClick_IsStick();
    TestFaceButtonClick_ArmsWithoutCurl();
    TestFaceButtonClickWhileStickDeflected_IsStick();
    TestKeyboardFallbacks();
    TestLongPressLeftFaceButton_OpensMenuAt3s();
    TestLongPressBothGrips_OpensMenuAt3s();
    TestPimaxController_LeftFaceButtonCombos();
    std::cout << "All Input Combos tests PASSED successfully!" << std::endl;
    return 0;
}
