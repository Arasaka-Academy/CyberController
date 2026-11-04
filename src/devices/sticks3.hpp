#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <atomic>
#include <array>

#include "../device.hpp"

namespace cybercontroller {

class StickS3 final : public Device {
public:
    bool begin() override;
    void update() override;
    bool talkPressed() const override;
    bool pairRequested() const override;
    void display(State state, const char* detail) override;
    void prepareMicrophone() override;
    bool microphoneReady() const override;
    bool startCapture() override;
    bool captureFrame(int16_t* samples) override;
    bool captureHealthy() const override { return !overrun_.load(); }
    void stopCapture() override;
    bool play(const uint8_t* encoded, uint32_t samples, uint8_t volume) override;
    bool playbackFinished() override;
    void stopPlayback() override;

private:
    static void released(void* context, void* data, size_t count);
    QueueHandle_t completed_ = nullptr;
    std::array<std::array<int16_t, 240>, 2> capture_{};
    std::atomic<bool> overrun_{false};
    int16_t* playback_ = nullptr;
    uint32_t playbackCount_ = 0;
    uint32_t microphoneStarted_ = 0;
};

}
