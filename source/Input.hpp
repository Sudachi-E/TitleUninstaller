#pragma once

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>

class Input {
public:
    Input();
    void Update();

    bool IsPressed(uint32_t button) const;
    bool IsHeld(uint32_t button)    const;
    uint8_t GetBattery() const { return vpadStatus.battery; }

    static constexpr uint32_t BUTTON_A     = 1 << 0;
    static constexpr uint32_t BUTTON_B     = 1 << 1;
    static constexpr uint32_t BUTTON_X     = 1 << 2;
    static constexpr uint32_t BUTTON_Y     = 1 << 3;
    static constexpr uint32_t BUTTON_LEFT  = 1 << 4;
    static constexpr uint32_t BUTTON_RIGHT = 1 << 5;
    static constexpr uint32_t BUTTON_UP    = 1 << 6;
    static constexpr uint32_t BUTTON_DOWN  = 1 << 7;
    static constexpr uint32_t BUTTON_L     = 1 << 8;
    static constexpr uint32_t BUTTON_R     = 1 << 9;
    static constexpr uint32_t BUTTON_PLUS  = 1 << 10;
    static constexpr uint32_t BUTTON_MINUS = 1 << 11;
    static constexpr uint32_t BUTTON_ZL    = 1 << 12;
    static constexpr uint32_t BUTTON_ZR    = 1 << 13;

private:
    VPADStatus    vpadStatus;
    VPADReadError vpadError;

    KPADStatus kpadStatus[4];

    uint32_t buttonsPressed;
    uint32_t buttonsHeld;

    uint32_t MapVPADButtons(uint32_t vpadButtons);
    uint32_t MapProButtons(uint32_t proButtons);
};
