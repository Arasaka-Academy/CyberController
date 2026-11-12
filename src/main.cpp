#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cmath>

#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
#include "wifi.hpp"
#else
#include "ble.hpp"
#endif
#include "profile.hpp"
#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
#include "devices/atoms3_pyramid.hpp"
#else
#include "devices/sticks3.hpp"
#endif
#include "cybercontroller/playback.hpp"

using namespace cybercontroller;

static void* audioAllocate(size_t size) { return heap_caps_malloc(size,
#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
#else
MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
#endif
); }
#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
static AtomS3Pyramid device;
#else
static StickS3 device;
#endif
#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
static Wifi transport;
#else
static Ble transport;
#endif
static Playback playback(audioAllocate, heap_caps_free, maxPlaybackSamples);
static ImaAdpcm encoder;
static State state = State::Disconnected;
static bool negotiated = false, wasConnected = false, held = false, requested = false, serialHeld = false;
static bool initialized = false;
static bool microphoneOpen = false;
static uint8_t stream = 0;
static uint32_t recordingStarted = 0, requestStarted = 0, playbackStarted = 0, lastDisplay = 0;
static uint32_t playbackTimeout = 0;
static std::array<int16_t, 240> frame{};
static char detail[96] = "Explicit pairing required";

static void transition(State next, const char* message) {
    state = next;
    snprintf(detail, sizeof(detail), "%s", message);
    if (transport.pairingActive()) {
        char pairingDetail[64];
        #ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
        snprintf(pairingDetail, sizeof(pairingDetail), "Setup: %08lu", static_cast<unsigned long>(transport.passkey()));
#else
        snprintf(pairingDetail, sizeof(pairingDetail), "Pairing: %06lu", static_cast<unsigned long>(transport.passkey()));
#endif
        device.display(state, pairingDetail);
    } else device.display(state, detail);
    Serial.printf("STATE %u %s\n", static_cast<unsigned>(state), detail);
}

static uint8_t* diagnosticAudio = nullptr;
static void stopDevicePlayback() {
    device.stopPlayback();
    heap_caps_free(diagnosticAudio); diagnosticAudio = nullptr;
}

static void interruptPlayback() {
    stopDevicePlayback();
    playback.cancel(playback.id());
    if (playback.notification()) transport.sendStatus(playback.status());
}

static void finishRecording(bool notify) {
    for (unsigned pending = 0; pending < 2 && device.captureFrame(frame.data()); ++pending) {
        std::array<uint8_t, 120> encoded{};
        encoder.encode(frame.data(), frame.size(), encoded.data());
        if (!transport.sendAudio(encoded.data(), encoded.size())) { device.stopCapture(); transport.disconnect(); return; }
    }
    device.stopCapture(); requested = false; microphoneOpen = false; frame.fill(0);
    if (notify) { const uint8_t ended[]{0}; if (!transport.sendControl(ended, sizeof(ended))) transport.disconnect(); }
    transition(transport.connected() ? State::Waiting : State::Disconnected, "Recording released; no offline queue");
    device.prepareMicrophone();
}

static void inputControl(const WriteEvent& event) {
    const auto* bytes = event.bytes.data();
    if (event.size == 6 && bytes[0] == 10 && bytes[1] == 1 && bytes[2] == 0 && (bytes[5] & 2)) {
        const uint8_t capabilities[]{11, 1, 0, 2, 0, 0, 120};
        negotiated = transport.sendControl(capabilities, sizeof(capabilities));
        if (negotiated && state == State::Disconnected) transition(State::Ready, "ATVV 1.0 / 16k mono IMA ADPCM");
    } else if (event.size == 2 && bytes[0] == 12 && bytes[1] == 0 && requested && held && negotiated && state != State::Recording) {
        microphoneOpen = true;
        device.prepareMicrophone();
        transition(State::Waiting, "Preparing mic; keep holding");
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
        if (diagnosticAudio) stopDevicePlayback();
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
    if (transport.connected()) return;
    interruptPlayback();
    device.prepareMicrophone();
    const uint32_t started = millis();
    transition(State::Waiting, "USB microphone test: preparing");
    while (!device.microphoneReady() && millis() - started < 2000 && !transport.connected()) { device.update(); delay(1); }
    // Internal RAM on AtomS3 must also leave room for BLE, DMA and the display.
    auto* samples = static_cast<int16_t*>(audioAllocate(32000 * sizeof(int16_t)));
    if (!samples || transport.connected() || !device.startCapture()) { heap_caps_free(samples); transition(State::Error, "Microphone test could not start"); return; }
    transition(State::Recording, "USB microphone test: 2 seconds");
    size_t count = 0;
    while (count < 32000 && millis() - started < 6000 && !transport.connected() && device.captureHealthy()) {
        if (device.captureFrame(frame.data())) {
            const size_t copied = std::min<size_t>(240, 32000 - count);
            std::memcpy(samples + count, frame.data(), copied * sizeof(int16_t)); count += copied;
        }
        delay(1);
    }
    device.stopCapture(); frame.fill(0);
    if (count == 32000 && !transport.connected()) {
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
    transition(transport.connected() ? State::Ready : State::Disconnected, "USB microphone test ended"); device.prepareMicrophone();
}

static void serialCommand() {
    static char command[64]{};
    static size_t length = 0;
    while (Serial.available()) {
        const char character = Serial.read();
        if (character == '\r') continue;
        if (character != '\n') { if (length < sizeof(command) - 1) command[length++] = character; continue; }
        command[length] = 0; length = 0;
        if (!strcmp(command, "PAIR")) transport.pairing();
        else if (!strcmp(command, "MIC_TEST") && !transport.connected()) microphoneTest();
        else if (!strcmp(command, "PTT 1")) serialHeld = true;
        else if (!strcmp(command, "PTT 0")) serialHeld = false;
        else if (!strcmp(command, "STATUS")) { Serial.printf("STATUS connected=%u negotiated=%u state=%u flash=%u psram=%u free_psram=%u heap=%u\n", transport.connected(), negotiated, static_cast<unsigned>(state), ESP.getFlashChipSize(), ESP.getPsramSize(), ESP.getFreePsram(), ESP.getFreeHeap()); transport.printStatus(); }
        else if (!strcmp(command, "SPEAKER_TEST") && !transport.connected()) {
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
                diagnosticAudio = encoded;
                if (device.play(encoded, 16000, 10)) transition(State::Playing, "Local 440 Hz test; low volume (10%)");
                else stopDevicePlayback();
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    if (!device.begin()) { transition(State::Error, "Device initialization failed"); return; }
    Serial.printf("DEVICE %s max_segment_samples=%lu\n", deviceName, static_cast<unsigned long>(maxPlaybackSamples));
    if (!transport.begin()) { transition(State::Error, "Transport initialization failed"); return; }
    initialized = true;
    transition(State::Disconnected, pairingHint);
}

void loop() {
    if (!initialized) { delay(100); return; }
    device.update();
    transport.tick(); serialCommand();
    if (device.pairRequested()) transport.pairing();
    const bool changed = transport.sessionChanged();
    const bool connected = transport.connected();
    if (changed || connected != wasConnected) {
        wasConnected = connected;
        device.stopCapture(); stopDevicePlayback(); playback.fail(PlaybackError::Disconnected);
        playback.notification(); // Never deliver the old session's status to a new peer.
        negotiated = false; requested = false; microphoneOpen = false; serialHeld = false; frame.fill(0);
        held = device.talkPressed();
        transition(connected ? State::Ready : State::Disconnected,
            connected ? "Connected; preparing voice protocol" : "Disconnected: recording and playback discarded");
        device.prepareMicrophone();
    }
    if (transport.overflowed()) { device.stopCapture(); interruptPlayback(); transport.disconnect(); transition(State::Error, "Transport queue overflow; recording discarded"); }
    const bool pressed = device.talkPressed() || serialHeld;
    if (pressed && !held) {
        interruptPlayback();
        device.prepareMicrophone();
        if (connected && negotiated && transport.atvvSubscribed()) {
            const uint8_t start[]{8};
            requested = transport.sendControl(start, sizeof(start)); requestStarted = millis();
            transition(State::Waiting, "Requesting microphone");
        } else transition(State::Disconnected, "Not ready; offline audio is not retained");
    }
    held = pressed;
    if (!held && (requested || state == State::Recording)) {
        if (state == State::Recording) finishRecording(true); else { requested = false; microphoneOpen = false; transition(State::Ready, "Released before microphone opened"); }
    }
    WriteEvent event;
    while (transport.connected() && transport.pop(event)) {
        if (event.kind == WriteKind::Atvv) inputControl(event);
        else if (event.kind == WriteKind::Control) outputControl(event);
        else playback.append(event.bytes.data(), event.size);
        // An adapter may borrow the verified compressed buffer for incremental decoding.
        // Stop before another queued event can replace a failed/cancelled segment.
        if (playback.state() == PlaybackState::Error || playback.state() == PlaybackState::Cancelled)
            stopDevicePlayback();
    }
    if (microphoneOpen && held && device.microphoneReady() && state != State::Recording) {
        microphoneOpen = false;
        if (!device.startCapture()) { transition(State::Error, "Microphone start failed"); transport.disconnect(); return; }
        ++stream; encoder = ImaAdpcm();
        const uint8_t opened[]{4, 0, 0, stream};
        const uint8_t sync[]{10, 0, 0, stream, 0, 0, 0};
        if (!transport.sendControl(opened, sizeof(opened)) || !transport.sendControl(sync, sizeof(sync))) { device.stopCapture(); transport.disconnect(); return; }
        recordingStarted = millis();
        transition(State::Recording, "Release button to send");
    }
    if (state == State::Recording) {
        if (!device.captureHealthy()) { device.stopCapture(); transport.disconnect(); transition(State::Error, "Microphone overrun; recording discarded"); }
        else if (device.captureFrame(frame.data())) {
            std::array<uint8_t, 120> encoded{};
            encoder.encode(frame.data(), frame.size(), encoded.data());
            if (!transport.sendAudio(encoded.data(), encoded.size())) { device.stopCapture(); transport.disconnect(); transition(State::Error, "ATVV delivery failed; recording discarded"); }
        }
        if (millis() - recordingStarted >= 30000 && state == State::Recording) finishRecording(true);
    }
    if (requested && state != State::Recording && millis() - requestStarted >= 5000) { requested = false; microphoneOpen = false; transition(State::Error, "ATVV microphone open timed out"); }
    playback.tick(millis());
    if (state == State::Playing) {
        if (!device.playbackHealthy()) {
            stopDevicePlayback(); playback.fail(PlaybackError::Audio);
        } else if (device.playbackFinished()) {
            stopDevicePlayback();
            playback.completed();
            Serial.printf("PLAYBACK completed id=%lu duration_ms=%lu\n", static_cast<unsigned long>(playback.id()), static_cast<unsigned long>(millis() - playbackStarted));
            transition(connected ? State::Ready : State::Disconnected, "Speaker playback complete"); device.prepareMicrophone();
        } else if (millis() - playbackStarted >= playbackTimeout) playback.fail(PlaybackError::Timeout);
    }
    if (playback.notification()) {
        transport.sendStatus(playback.status());
        if (playback.state() == PlaybackState::Error) {
            stopDevicePlayback();
            if (state != State::Recording) {
                transition(State::Error, playback.error() == PlaybackError::Timeout ? "Playback timed out; buffer cleared" : "Playback rejected; buffer cleared");
                device.prepareMicrophone();
            }
        }
    }
    static bool pairingShown = false;
    if (pairingShown && !transport.pairingActive()) { device.display(state, detail); pairingShown = false; }
    if (transport.pairingActive() && millis() - lastDisplay >= 500) {
        pairingShown = true;
        lastDisplay = millis();
        char pairingDetail[64];
        #ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
        snprintf(pairingDetail, sizeof(pairingDetail), "Setup: %08lu", static_cast<unsigned long>(transport.passkey()));
#else
        snprintf(pairingDetail, sizeof(pairingDetail), "Pairing: %06lu", static_cast<unsigned long>(transport.passkey()));
#endif
        device.display(state, pairingDetail);
    }
    delay(1);
}
