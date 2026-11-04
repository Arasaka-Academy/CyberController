#include "sticks3.hpp"

#include <esp_heap_caps.h>
#include <cstring>

#include "cybercontroller/codec.hpp"

namespace cybercontroller {

bool StickS3::begin() {
    auto configuration = M5.config();
    configuration.fallback_board = m5::board_t::board_M5StickS3;
    configuration.internal_mic = true;
    configuration.internal_spk = true;
    M5.begin(configuration);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(90);
    M5.BtnB.setHoldThresh(2000);
    M5.Speaker.end();
    M5.Mic.end();
    auto microphone = M5.Mic.config();
    microphone.sample_rate = 16000;
    M5.Mic.config(microphone);
    completed_ = xQueueCreate(2, sizeof(uint8_t));
    M5.Mic.setBufferReleaseCallback(this, released);
    prepareMicrophone();
    return completed_ != nullptr && psramFound() && ESP.getPsramSize() >= 7 * 1024 * 1024 && ESP.getFlashChipSize() == 8 * 1024 * 1024;
}

void StickS3::update() { M5.update(); }
bool StickS3::talkPressed() const { return M5.BtnA.isPressed(); }
bool StickS3::pairRequested() const { return M5.BtnB.wasHold(); }

void StickS3::display(State state, const char* detail) {
    const char* names[] = {"Disconnected", "Ready", "Recording", "Waiting reply", "Receiving", "Playing", "Error"};
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(state == State::Error ? 0xf800 : 0xffff, 0x0000);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 8);
    M5.Display.println("CyberController / StickS3");
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 32);
    M5.Display.println(names[static_cast<unsigned>(state)]);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 68);
    M5.Display.println(detail);
    M5.Display.setCursor(8, 110);
    M5.Display.println("A: talk   B 2s: pair (60s)");
}

void StickS3::prepareMicrophone() {
    if (playback_) return;
    M5.Speaker.end();
    if (!M5.Mic.isRunning() && M5.Mic.begin()) microphoneStarted_ = millis();
}

bool StickS3::microphoneReady() const { return M5.Mic.isRunning() && millis() - microphoneStarted_ >= 1100; }

void StickS3::released(void* context, void* data, size_t count) {
    auto* device = static_cast<StickS3*>(context);
    if (count != 240) { device->overrun_.store(true); return; }
    for (uint8_t index = 0; index < device->capture_.size(); ++index) {
        if (data == device->capture_[index].data()) {
            if (xQueueSend(device->completed_, &index, 0) != pdTRUE) device->overrun_.store(true);
            return;
        }
    }
}

bool StickS3::startCapture() {
    if (!microphoneReady()) return false;
    stopPlayback();
    xQueueReset(completed_);
    overrun_.store(false);
    if (!M5.Mic.begin()) return false;
    return M5.Mic.record(capture_[0].data(), 240) && M5.Mic.record(capture_[1].data(), 240);
}

bool StickS3::captureFrame(int16_t* samples) {
    uint8_t index = 0;
    if (xQueueReceive(completed_, &index, 0) != pdTRUE) return false;
    std::memcpy(samples, capture_[index].data(), 240 * sizeof(int16_t));
    if (!M5.Mic.record(capture_[index].data(), 240)) overrun_.store(true);
    return true;
}

void StickS3::stopCapture() {
    M5.Mic.end();
    if (completed_) xQueueReset(completed_);
    for (auto& frame : capture_) frame.fill(0);
}

bool StickS3::play(const uint8_t* encoded, uint32_t samples, uint8_t volume) {
    stopCapture();
    stopPlayback();
    if (!encoded || samples == 0 || samples > 16000 * 130) return false;
    playbackCount_ = samples + 1600;
    playback_ = static_cast<int16_t*>(heap_caps_calloc(playbackCount_, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!playback_) return false;
    ImaAdpcm decoder;
    for (uint32_t position = 0; position < samples; ++position) {
        playback_[position] = decoder.decodeNibble(position % 2 == 0 ? encoded[position / 2] >> 4 : encoded[position / 2] & 15);
    }
    M5.Speaker.setVolume(static_cast<uint8_t>(std::min<unsigned>(volume, 100) * 64 / 100));
    if (!M5.Speaker.begin() || !M5.Speaker.playRaw(playback_, playbackCount_, 16000, false, 1, 0)) {
        stopPlayback(); return false;
    }
    return true;
}

bool StickS3::playbackFinished() {
    if (!playback_ || M5.Speaker.isPlaying(0)) return false;
    stopPlayback();
    return true;
}

void StickS3::stopPlayback() {
    M5.Speaker.stop();
    M5.Speaker.end();
    if (playback_) {
        std::memset(playback_, 0, playbackCount_ * sizeof(int16_t));
        heap_caps_free(playback_); playback_ = nullptr; playbackCount_ = 0;
    }
}

}
