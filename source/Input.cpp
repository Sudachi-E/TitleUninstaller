#include "Input.hpp"
#include <cstring>

Input::Input() : buttonsPressed(0), buttonsHeld(0) {
    memset(&vpadStatus,  0, sizeof(vpadStatus));
    memset(&kpadStatus,  0, sizeof(kpadStatus));

    WPADEnableURCC(1);
}

void Input::Update() {
    uint32_t pressed = 0;
    uint32_t held    = 0;

    // GamePad (VPAD)
    VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &vpadError);
    if (vpadError == VPAD_READ_SUCCESS) {
        pressed |= MapVPADButtons(vpadStatus.trigger);
        held    |= MapVPADButtons(vpadStatus.hold);
    }

    // Pro Controller (KPAD)
    for (int ch = 0; ch < 4; ch++) {
        int32_t count = KPADRead((KPADChan)ch, &kpadStatus[ch], 1);
        if (count <= 0) continue;

        KPADStatus& k = kpadStatus[ch];
        if (k.extensionType != WPAD_EXT_PRO_CONTROLLER) continue;

        pressed |= MapProButtons(k.pro.trigger);
        held    |= MapProButtons(k.pro.hold);
    }

    buttonsPressed = pressed;
    buttonsHeld    = held;
}

bool Input::IsPressed(uint32_t button) const { return (buttonsPressed & button) != 0; }
bool Input::IsHeld(uint32_t button)    const { return (buttonsHeld    & button) != 0; }

uint32_t Input::MapVPADButtons(uint32_t v) {
    uint32_t m = 0;
    if (v & VPAD_BUTTON_A)     m |= BUTTON_A;
    if (v & VPAD_BUTTON_B)     m |= BUTTON_B;
    if (v & VPAD_BUTTON_X)     m |= BUTTON_X;
    if (v & VPAD_BUTTON_Y)     m |= BUTTON_Y;
    if (v & VPAD_BUTTON_LEFT)  m |= BUTTON_LEFT;
    if (v & VPAD_BUTTON_RIGHT) m |= BUTTON_RIGHT;
    if (v & VPAD_BUTTON_UP)    m |= BUTTON_UP;
    if (v & VPAD_BUTTON_DOWN)  m |= BUTTON_DOWN;
    if (v & VPAD_BUTTON_L)     m |= BUTTON_L;
    if (v & VPAD_BUTTON_R)     m |= BUTTON_R;
    if (v & VPAD_BUTTON_PLUS)  m |= BUTTON_PLUS;
    if (v & VPAD_BUTTON_MINUS) m |= BUTTON_MINUS;
    if (v & VPAD_BUTTON_ZL)    m |= BUTTON_ZL;
    if (v & VPAD_BUTTON_ZR)    m |= BUTTON_ZR;
    return m;
}

uint32_t Input::MapProButtons(uint32_t p) {
    uint32_t m = 0;
    if (p & WPAD_PRO_BUTTON_A)     m |= BUTTON_A;
    if (p & WPAD_PRO_BUTTON_B)     m |= BUTTON_B;
    if (p & WPAD_PRO_BUTTON_X)     m |= BUTTON_X;
    if (p & WPAD_PRO_BUTTON_Y)     m |= BUTTON_Y;
    if (p & WPAD_PRO_BUTTON_LEFT)  m |= BUTTON_LEFT;
    if (p & WPAD_PRO_BUTTON_RIGHT) m |= BUTTON_RIGHT;
    if (p & WPAD_PRO_BUTTON_UP)    m |= BUTTON_UP;
    if (p & WPAD_PRO_BUTTON_DOWN)  m |= BUTTON_DOWN;
    if (p & WPAD_PRO_BUTTON_L)     m |= BUTTON_L;
    if (p & WPAD_PRO_BUTTON_R)     m |= BUTTON_R;
    if (p & WPAD_PRO_BUTTON_PLUS)  m |= BUTTON_PLUS;
    if (p & WPAD_PRO_BUTTON_MINUS) m |= BUTTON_MINUS;
    if (p & WPAD_PRO_TRIGGER_ZL)   m |= BUTTON_ZL;
    if (p & WPAD_PRO_TRIGGER_ZR)   m |= BUTTON_ZR;
    // Also map left stick emulation to D-pad for navigation
    if (p & WPAD_PRO_STICK_L_EMULATION_UP)    m |= BUTTON_UP;
    if (p & WPAD_PRO_STICK_L_EMULATION_DOWN)  m |= BUTTON_DOWN;
    if (p & WPAD_PRO_STICK_L_EMULATION_LEFT)  m |= BUTTON_LEFT;
    if (p & WPAD_PRO_STICK_L_EMULATION_RIGHT) m |= BUTTON_RIGHT;
    return m;
}
