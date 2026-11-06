#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cmath>

#include "ble.hpp"
#include "devices/sticks3.hpp"
#include "cybercontroller/playback.hpp"

using namespace cybercontroller;

static void* audioAllocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static StickS3 device;
static Ble ble;
static Playback playback(audioAllocate, heap_caps_free);
static ImaAdpcm encoder;
static State state = State::Disconnected;
static bool negotiated = false, wasConnected = false, held = false, requested = false, serialHeld = false;
static bool initialized = false;
static bool microphoneOpen = false;
static uint8_t stream = 0;
static uint32_t recordingStarted = 0, requestStarted = 0, playbackStarted = 0, lastDisplay = 0;
static uint32_t playbackTimeout = 0;
static std::array<int16_t, 240> frame{};
static char detail[96] = "Hold B for explicit pairing";

static void transition(State next, const char* message) {
    state = next;
    snprintf(detail, sizeof(detail), "%s", message);
    device.display(state, detail);
    Serial.printf("STATE %u %s\n", static_cast<unsigned>(state), detail);
}

static void interruptPlayback() {
    device.stopPlayback();
    playback.cancel(playback.id());
    if (playback.notification()) ble.sendStatus(playback.status());
}

static void finishRecording(bool notify) {
    for (unsigned pending = 0; pending < 2 && device.captureFrame(frame.data()); ++pending) {
        std::array<uint8_t, 120> encoded{};
        encoder.encode(frame.data(), frame.size(), encoded.data());
        if (!ble.sendAudio(encoded.data(), encoded.size())) { device.stopCapture(); ble.disconnect(); return; }
    }
    device.stopCapture(); requested = false; microphoneOpen = false; frame.fill(0);
    if (notify) { const uint8_t ended[]{0}; if (!ble.sendControl(ended, sizeof(ended))) ble.disconnect(); }
    transition(ble.connected() ? State::Waiting : State::Disconnected, "Recording released; no offline queue");
    device.prepareMicrophone();
}

static void inputControl(const WriteEvent& event) {
    const auto* bytes = event.bytes.data();
    if (event.size == 6 && bytes[0] == 10 && bytes[1] == 1 && bytes[2] == 0 && (bytes[5] & 2)) {
        const uint8_t capabilities[]{11, 1, 0, 2, 0, 0, 120};
        negotiated = ble.sendControl(capabilities, sizeof(capabilities));
        if (negotiated && state == State::Disconnected) transition(State::Ready, "ATVV 1.0 / 16k mono IMA ADPCM");
    } else if (event.size == 2 && bytes[0] == 12 && bytes[1] == 0 && requested && held && negotiated && state != State::Recording) {
        microphoneOpen = true;
        device.prepareMicrophone();
        transition(State::Waiting, "Preparing microphone; keep holding A");
    } else if (event.size == 2 && bytes[0] == 13 && bytes[1] == stream && state == State::Recording) {
        finishRecording(false);
    }
}

static void outputControl(const WriteEvent& event) {
    const auto* bytes = event.bytes.data();
    if (event.size < 6 || bytes[0] != 1) { playback.fail(PlaybackError::Invalid); return; }
    const uint32_t id = read32(bytes + 2);
    if (bytes[1] == 1) {
        if (held || requested || state == State::Recording) { playback.fail(PlaybackError::Busy); return; }
        if (playback.start(bytes, event.size, millis())) transition(State::Receiving, "Buffering complete segment before playback");
    } else if (event.size == 6 && bytes[1] == 2) {
        if (!playback.finish(id)) return;
        if (!device.play(playback.audio(), playback.samples(), playback.volume())) { playback.fail(PlaybackError::Audio); return; }
        playback.playing(); playbackStarted = millis();
        playbackTimeout = (playback.samples() + 15) / 16 + 5000;
        transition(State::Playing, "CRC verified; speaker active");
    } else if (event.size == 6 && bytes[1] == 3) {
        if (id != playback.id()) return;
        interruptPlayback();
        if (!held && !requested && state != State::Recording) { transition(State::Ready, "Playback cancelled"); device.prepareMicrophone(); }
    } else { playback.fail(PlaybackError::Invalid); }
}

static void microphoneTest() {
    if (ble.connected()) return;
    interruptPlayback();
    device.prepareMicrophone();
    const uint32_t started = millis();
    transition(State::Waiting, "USB microphone test: preparing");
    while (!device.microphoneReady() && millis() - started < 2000 && !ble.connected()) { device.update(); delay(1); }
    auto* samples = static_cast<int16_t*>(audioAllocate(32000 * sizeof(int16_t)));
    if (!samples || ble.connected() || !device.startCapture()) { heap_caps_free(samples); transition(State::Error, "Microphone test could not start"); return; }
    transition(State::Recording, "USB microphone test: 2 seconds");
    size_t count = 0;
    while (count < 32000 && millis() - started < 6000 && !ble.connected() && device.captureHealthy()) {
        if (device.captureFrame(frame.data())) {
            const size_t copied = std::min<size_t>(240, 32000 - count);
            std::memcpy(samples + count, frame.data(), copied * sizeof(int16_t)); count += copied;
        }
        delay(1);
    }
    device.stopCapture(); frame.fill(0);
    if (count == 32000 && !ble.connected()) {
        std::array<uint8_t, 44> header{};
        std::memcpy(header.data(), "RIFF", 4); write32(header.data() + 4, 64036);
        std::memcpy(header.data() + 8, "WAVEfmt ", 8); write32(header.data() + 16, 16);
        header[20] = 1; header[22] = 1; write32(header.data() + 24, 16000); write32(header.data() + 28, 32000);
        header[32] = 2; header[34] = 16; std::memcpy(header.data() + 36, "data", 4); write32(header.data() + 40, 64000);
        Serial.printf("MIC_WAV 64044\n");
        Serial.write(header.data(), header.size()); Serial.write(reinterpret_cast<uint8_t*>(samples), 64000);
        Serial.printf("\nMIC_TEST samples=32000 duration_ms=%lu\n", static_cast<unsigned long>(millis() - started));
    } else Serial.println("MIC_TEST aborted; no recording retained");
    std::memset(samples, 0, 32000 * sizeof(int16_t)); heap_caps_free(samples);
    transition(ble.connected() ? State::Ready : State::Disconnected, "USB microphone test ended"); device.prepareMicrophone();
}

static void serialCommand() {
    static char command[64]{};
    static size_t length = 0;
    while (Serial.available()) {
        const char character = Serial.read();
        if (character == '\r') continue;
        if (character != '\n') { if (length < sizeof(command) - 1) command[length++] = character; continue; }
        command[length] = 0; length = 0;
        if (!strcmp(command, "PAIR")) ble.pairing();
        else if (!strcmp(command, "MIC_TEST") && !ble.connected()) microphoneTest();
        else if (!strcmp(command, "PTT 1")) serialHeld = true;
        else if (!strcmp(command, "PTT 0")) serialHeld = false;
        else if (!strcmp(command, "STATUS")) { Serial.printf("STATUS connected=%u negotiated=%u state=%u flash=%u psram=%u free_psram=%u heap=%u\n", ble.connected(), negotiated, static_cast<unsigned>(state), ESP.getFlashChipSize(), ESP.getPsramSize(), ESP.getFreePsram(), ESP.getFreeHeap()); ble.printStatus(); }
        else if (!strcmp(command, "SPEAKER_TEST") && !ble.connected()) {
            interruptPlayback();
            auto* encoded = static_cast<uint8_t*>(audioAllocate(8000));
            if (encoded) {
                ImaAdpcm tone;
                for (uint32_t position = 0; position < 16000; position += 2) {
                    const int16_t samples[]{static_cast<int16_t>(3000 * std::sin(position * 2.0 * 3.141592653589793 * 440 / 16000)), static_cast<int16_t>(3000 * std::sin((position + 1) * 2.0 * 3.141592653589793 * 440 / 16000))};
                    tone.encode(samples, 2, encoded + position / 2);
                }
                playbackStarted = millis();
                playbackTimeout = 6000;
                if (device.play(encoded, 16000, 10)) transition(State::Playing, "Local 440 Hz test; low volume (10%)");
                heap_caps_free(encoded);
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    if (!device.begin()) { transition(State::Error, "N8R8 PSRAM or device initialization failed"); return; }
    if (!ble.begin()) { transition(State::Error, "BLE initialization/advertising failed"); return; }
    initialized = true;
    transition(State::Disconnected, "Hold B 2s to pair; USB PAIR also explicit");
}

void loop() {
    if (!initialized) { delay(100); return; }
    device.update();
    ble.tick(); serialCommand();
    if (device.pairRequested()) ble.pairing();
    const bool changed = ble.sessionChanged();
    const bool connected = ble.connected();
    if (changed || connected != wasConnected) {
        wasConnected = connected;
        device.stopCapture(); device.stopPlayback(); playback.fail(PlaybackError::Disconnected);
        playback.notification(); // Never deliver the old session's status to a new peer.
        negotiated = false; requested = false; microphoneOpen = false; serialHeld = false; frame.fill(0);
        held = device.talkPressed();
        transition(connected ? State::Ready : State::Disconnected,
            connected ? "Encrypted bonded connection; waiting ATVV" : "Disconnected: recording and playback discarded");
        device.prepareMicrophone();
    }
    if (ble.overflowed()) { device.stopCapture(); interruptPlayback(); ble.disconnect(); transition(State::Error, "BLE queue overflow; recording discarded"); }
    const bool pressed = device.talkPressed() || serialHeld;
    if (pressed && !held) {
        interruptPlayback();
        device.prepareMicrophone();
        if (connected && negotiated && ble.atvvSubscribed()) {
            const uint8_t start[]{8};
            requested = ble.sendControl(start, sizeof(start)); requestStarted = millis();
            transition(State::Waiting, "Requesting microphone");
        } else transition(State::Disconnected, "Not ready; offline audio is not retained");
    }
    held = pressed;
    if (!held && (requested || state == State::Recording)) {
        if (state == State::Recording) finishRecording(true); else { requested = false; microphoneOpen = false; transition(State::Ready, "Released before microphone opened"); }
    }
    WriteEvent event;
    while (ble.connected() && ble.pop(event)) {
        if (event.kind == WriteKind::Atvv) inputControl(event);
        else if (event.kind == WriteKind::Control) outputControl(event);
        else playback.append(event.bytes.data(), event.size);
    }
    if (microphoneOpen && held && device.microphoneReady() && state != State::Recording) {
        microphoneOpen = false;
        if (!device.startCapture()) { transition(State::Error, "Microphone start failed"); ble.disconnect(); return; }
        ++stream; encoder = ImaAdpcm();
        const uint8_t opened[]{4, 0, 0, stream};
        const uint8_t sync[]{10, 0, 0, stream, 0, 0, 0};
        if (!ble.sendControl(opened, sizeof(opened)) || !ble.sendControl(sync, sizeof(sync))) { device.stopCapture(); ble.disconnect(); return; }
        recordingStarted = millis();
        transition(State::Recording, "Release A to send");
    }
    if (state == State::Recording) {
        if (!device.captureHealthy()) { device.stopCapture(); ble.disconnect(); transition(State::Error, "Microphone overrun; recording discarded"); }
        else if (device.captureFrame(frame.data())) {
            std::array<uint8_t, 120> encoded{};
            encoder.encode(frame.data(), frame.size(), encoded.data());
            if (!ble.sendAudio(encoded.data(), encoded.size())) { device.stopCapture(); ble.disconnect(); transition(State::Error, "ATVV delivery failed; recording discarded"); }
        }
        if (millis() - recordingStarted >= 30000 && state == State::Recording) finishRecording(true);
    }
    if (requested && state != State::Recording && millis() - requestStarted >= 5000) { requested = false; microphoneOpen = false; transition(State::Error, "ATVV microphone open timed out"); }
    playback.tick(millis());
    if (state == State::Playing) {
        if (device.playbackFinished()) {
            playback.completed();
            Serial.printf("PLAYBACK completed id=%lu duration_ms=%lu\n", static_cast<unsigned long>(playback.id()), static_cast<unsigned long>(millis() - playbackStarted));
            transition(connected ? State::Ready : State::Disconnected, "Speaker playback complete"); device.prepareMicrophone();
        } else if (millis() - playbackStarted >= playbackTimeout) playback.fail(PlaybackError::Timeout);
    }
    if (playback.notification()) {
        ble.sendStatus(playback.status());
        if (playback.state() == PlaybackState::Error) {
            device.stopPlayback();
            if (state != State::Recording) {
                transition(State::Error, playback.error() == PlaybackError::Timeout ? "Playback timed out; buffer cleared" : "Playback rejected; buffer cleared");
                device.prepareMicrophone();
            }
        }
    }
    if (ble.pairingActive() && millis() - lastDisplay >= 500) {
        lastDisplay = millis();
        snprintf(detail, sizeof(detail), "Pairing 60s / passkey: %06lu", static_cast<unsigned long>(ble.passkey()));
        device.display(state, detail);
    }
    delay(1);
}
