#pragma once
#include <Arduino.h>
#include <atomic>
#include <array>
#include <M5Unified.h>
#include <M5EchoPyramid.h>
#include "../device.hpp"
#include "cybercontroller/pcm_stream.hpp"

namespace cybercontroller {
class AtomS3Pyramid final : public Device {
public:
    bool begin() override;
    void update() override;
    bool talkPressed() const override { return M5.BtnA.isPressed(); }
    bool pairRequested() const override { return pairEvent_; }
    void display(State state, const char* detail) override;
    void prepareMicrophone() override;
    bool microphoneReady() const override;
    bool startCapture() override;
    bool captureFrame(int16_t* samples) override;
    bool captureHealthy() const override { return !captureError_; }
    void stopCapture() override;
    bool play(const uint8_t* encoded, uint32_t samples, uint8_t volume) override;
    bool playbackFinished() override;
    bool playbackHealthy() const override { return !playbackError_; }
    void stopPlayback() override;
private:
    bool receive();
    static bool IRAM_ATTR overflow(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
        auto* self = static_cast<AtomS3Pyramid*>(context);
        if (self->capturing_.load()) self->captureError_.store(true);
        return false;
    }
    static bool IRAM_ATTR sent(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
        static_cast<AtomS3Pyramid*>(context)->sentBuffers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::atomic<uint32_t> sentBuffers_{0};
    I2CBus bus_;
    SI5351 clock_{bus_};
    ES7210 adc_{bus_};
    ES8311 codec_{bus_};
    STM32Ctrl control_{bus_};
    AW87559 amplifier_{bus_};
    i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
    std::array<int16_t, 480> input_{}, output_{};
    PcmStream stream_;
    size_t inputBytes_ = 0, outputBytes_ = 0, outputOffset_ = 0;
    bool ready_ = false, playbackError_ = false;
    std::atomic<bool> capturing_{false}, captureError_{false};
    bool playing_ = false, draining_ = false, txEnabled_ = false;
    bool touching_ = false, touchLatched_ = false, pairEvent_ = false;
    uint32_t microphoneStarted_ = 0, touchStarted_ = 0, lastTouch_ = 0, drainStarted_ = 0;
};
}
