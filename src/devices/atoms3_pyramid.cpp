#include "atoms3_pyramid.hpp"
#include "../profile.hpp"
#include <cstring>

namespace cybercontroller {
bool AtomS3Pyramid::begin() {
    auto configuration = M5.config();
    configuration.fallback_board = m5::board_t::board_M5AtomS3;
    configuration.internal_mic = false;
    configuration.internal_spk = false;
    M5.begin(configuration);
    M5.Display.setRotation(0);
    M5.Display.setBrightness(80);
    Wire1.end();
    Wire1.setTimeOut(10);
    if (!bus_.begin(&Wire1, 38, 39)) return false;
    for (const uint8_t address : {0x60, 0x40, 0x18, 0x1a}) {
        Wire1.beginTransmission(address);
        if (Wire1.endTransmission() != 0) {
            Serial.printf("PYRAMID missing_i2c=0x%02x; use bottom USB power\n", address);
            return false;
        }
    }
    // STM32 releases the amplifier reset; probing 0x5b before this can falsely report it missing.
    if (!control_.begin()) return false;
    Wire1.beginTransmission(0x5b);
    if (Wire1.endTransmission() != 0) {
        uint16_t millivolts = 0; control_.getUSB5V(millivolts);
        Serial.printf("PYRAMID amplifier unavailable after reset; bottom_usb_mv=%u\n", millivolts);
        return false;
    }
    if (!clock_.begin() || !clock_.setMCLK(4096000)
        || !adc_.begin(4096000, 16000, ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3)
        || !codec_.begin(4096000, 16000) || !amplifier_.begin() || !codec_.mute(true)) return false;
    control_.setBrightness(1, 24); control_.setBrightness(2, 24);
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = 4;
    channels.dma_frame_num = 240;
    channels.auto_clear = true;
    if (i2s_new_channel(&channels, &tx_, &rx_) != ESP_OK) return false;
    i2s_std_config_t audio{};
    audio.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    audio.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    audio.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    audio.gpio_cfg.bclk = GPIO_NUM_6;
    audio.gpio_cfg.ws = GPIO_NUM_8;
    // The base labels DOUT from the ADC's perspective, not the ESP32's.
    audio.gpio_cfg.dout = GPIO_NUM_7;
    audio.gpio_cfg.din = GPIO_NUM_5;
    i2s_event_callbacks_t callbacks{};
    callbacks.on_recv_q_ovf = overflow;
    i2s_event_callbacks_t txCallbacks{};
    txCallbacks.on_sent = sent;
    if (i2s_channel_register_event_callback(tx_, &txCallbacks, this) != ESP_OK
        || i2s_channel_register_event_callback(rx_, &callbacks, this) != ESP_OK
        || i2s_channel_init_std_mode(tx_, &audio) != ESP_OK
        || i2s_channel_init_std_mode(rx_, &audio) != ESP_OK
        || i2s_channel_enable(rx_) != ESP_OK) return false;
    ready_ = ESP.getFlashChipSize() == 8 * 1024 * 1024;
    microphoneStarted_ = millis();
    return ready_;
}

bool AtomS3Pyramid::receive() {
    size_t count = 0;
    const auto result = i2s_channel_read(rx_, reinterpret_cast<uint8_t*>(input_.data()) + inputBytes_,
        sizeof(input_) - inputBytes_, &count, 0);
    inputBytes_ += count;
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) captureError_ = true;
    return inputBytes_ == sizeof(input_);
}

void AtomS3Pyramid::update() {
    M5.update();
    pairEvent_ = false;
    if (!ready_) return;
    if (millis() - lastTouch_ >= 20) {
        lastTouch_ = millis();
        const bool touched = control_.isPressed(1);
        if (!touched) { touching_ = touchLatched_ = false; }
        else {
            if (!touching_) { touching_ = true; touchStarted_ = millis(); }
            if (!touchLatched_ && millis() - touchStarted_ >= 2000) { touchLatched_ = true; pairEvent_ = true; }
        }
    }
    // Keep RX fresh even while idle or playing; never accumulate offline audio.
    if (!capturing_) { for (unsigned i = 0; i < 5 && receive(); ++i) inputBytes_ = 0; }
    if (!playing_ || draining_ || playbackError_) return;
    if (outputOffset_ == outputBytes_) {
        if (stream_.finished()) { draining_ = true; drainStarted_ = sentBuffers_.load(std::memory_order_relaxed); return; }
        outputBytes_ = stream_.read(output_.data(), 240) * 2 * sizeof(int16_t);
        outputOffset_ = 0;
    }
    size_t written = 0;
    const auto result = i2s_channel_write(tx_, reinterpret_cast<uint8_t*>(output_.data()) + outputOffset_,
        outputBytes_ - outputOffset_, &written, 0);
    outputOffset_ += written;
    // A driver failure leaves completion false; the application deadline reports failure.
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) { codec_.mute(true); playbackError_ = true; }
}

void AtomS3Pyramid::display(State state, const char* detail) {
    const char* names[] = {"Disconnected", "Ready", "Recording", "Waiting reply", "Receiving", "Playing", "Error"};
    const bool pairing = std::strncmp(detail, "Setup:", 6) == 0;
    M5.Display.fillScreen(0);
    M5.Display.setTextColor(state == State::Error ? 0xf800 : 0xffff, 0);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(2, 3);
    M5.Display.println("CyberController");
    M5.Display.println("AtomS3 / Pyramid");
    M5.Display.setCursor(2, 32);
    M5.Display.println(pairing ? "Wi-Fi setup" : names[static_cast<unsigned>(state)]);
    M5.Display.setCursor(2, 50);
    if (pairing) { M5.Display.setTextSize(2); M5.Display.println(detail + 7); }
    else M5.Display.println(detail);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(2, 80);
    if (pairing) { M5.Display.println("CyberController-Setup"); M5.Display.println("192.168.4.1"); }
    M5.Display.setCursor(2, 102);
    M5.Display.println("Hold screen: talk");
    M5.Display.println("TP1 2s: Wi-Fi setup");
    if (!ready_) return;
    const uint8_t colors[][3] = {{12, 12, 12}, {0, 40, 0}, {45, 0, 0}, {35, 20, 0}, {0, 15, 40}, {0, 35, 25}, {45, 0, 20}};
    const auto* color = colors[static_cast<unsigned>(state)];
    for (uint8_t channel : {1, 2}) control_.setAllRGB(channel,
        pairing ? 20 : color[0], pairing ? 0 : color[1], pairing ? 40 : color[2]);
}

void AtomS3Pyramid::prepareMicrophone() { if (ready_ && !playing_) codec_.mute(true); }
bool AtomS3Pyramid::microphoneReady() const { return ready_ && !playing_ && millis() - microphoneStarted_ >= 1100; }
bool AtomS3Pyramid::startCapture() {
    if (!microphoneReady()) return false;
    // Discard queued idle samples before arming overrun detection.
    for (unsigned i = 0; i < 5 && receive(); ++i) inputBytes_ = 0;
    // In full-duplex mode RX shares the TX clock. Keep muted, zero-filled TX
    // running during capture; disabling TX otherwise yields no microphone frames.
    if (!txEnabled_) {
        output_.fill(0);
        for (unsigned i = 0; i < 4; ++i) {
            size_t loaded = 0;
            if (i2s_channel_preload_data(tx_, output_.data(), sizeof(output_), &loaded) != ESP_OK
                || loaded != sizeof(output_)) return false;
        }
        if (i2s_channel_enable(tx_) != ESP_OK) return false;
        txEnabled_ = true;
    }
    inputBytes_ = 0; input_.fill(0); captureError_ = false; capturing_ = true;
    return true;
}
bool AtomS3Pyramid::captureFrame(int16_t* samples) {
    if (!capturing_ || !receive()) return false;
    for (size_t i = 0; i < 240; ++i) samples[i] = input_[2 * i];
    input_.fill(0); inputBytes_ = 0;
    return true;
}
void AtomS3Pyramid::stopCapture() {
    capturing_ = false; inputBytes_ = 0; input_.fill(0);
    if (txEnabled_ && !playing_) { i2s_channel_disable(tx_); txEnabled_ = false; }
}

bool AtomS3Pyramid::play(const uint8_t* encoded, uint32_t samples, uint8_t volume) {
    stopCapture(); stopPlayback();
    if (!ready_ || !encoded || !samples || samples > maxPlaybackSamples) return false;
    // Clear all four DMA descriptors while TX is stopped, so re-enabling cannot replay old speech.
    output_.fill(0);
    for (unsigned i = 0; i < 4; ++i) {
        size_t loaded = 0;
        if (i2s_channel_preload_data(tx_, output_.data(), sizeof(output_), &loaded) != ESP_OK
            || loaded != sizeof(output_)) return false;
    }
    if (!codec_.setVolume(std::min<unsigned>(volume, 100) * 25 / 100)
        || i2s_channel_enable(tx_) != ESP_OK) return false;
    txEnabled_ = true;
    if (!codec_.mute(false)) { stopPlayback(); return false; }
    stream_.begin(encoded, samples);
    playing_ = true;
    return true;
}
bool AtomS3Pyramid::playbackFinished() {
    // Observe a full four-descriptor DMA cycle plus one boundary after the silent tail.
    // Wall-clock time alone cannot prove the peripheral actually drained.
    if (playbackError_ || !playing_ || !draining_ || sentBuffers_.load(std::memory_order_relaxed) - drainStarted_ < 5) return false;
    stopPlayback(); return true;
}
void AtomS3Pyramid::stopPlayback() {
    if (ready_) codec_.mute(true);
    if (txEnabled_) { i2s_channel_disable(tx_); txEnabled_ = false; }
    stream_.reset(); playing_ = draining_ = playbackError_ = false;
    outputBytes_ = outputOffset_ = 0; output_.fill(0);
}
}
