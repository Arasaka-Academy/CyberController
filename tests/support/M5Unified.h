#pragma once
#include "Arduino.h"
namespace m5 { enum class board_t { board_M5AtomS3, board_M5StickS3 }; }
struct FakeM5 {
    struct Configuration { m5::board_t fallback_board{}; bool internal_mic = false, internal_spk = false; };
    Configuration config() { return {}; }
    void begin(Configuration) {}
    void update() {}
    struct Button { bool isPressed() const { return hardware::button; } } BtnA;
    struct Screen {
        void setRotation(int) {} void setBrightness(int) {} void fillScreen(int) {}
        void setTextColor(int, int) {} void setTextSize(int) {} void setCursor(int, int) {}
        void println(const char*) {}
    } Display;
};
inline FakeM5 M5;
