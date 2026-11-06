#include <iostream>
#include <new>
#include <stdexcept>

// Run the actual callbacks and application loop with only the hardware APIs replaced.
#include "../src/ble.cpp"
#include "../src/main.cpp"

namespace cybercontroller {
bool StickS3::begin() { return true; }
void StickS3::update() {}
bool StickS3::talkPressed() const { return hardware::button; }
bool StickS3::pairRequested() const { return false; }
void StickS3::display(State, const char*) {}
void StickS3::prepareMicrophone() {
    if (hardware::onPrepare) { const auto callback = hardware::onPrepare; hardware::onPrepare = nullptr; callback(); }
}
bool StickS3::microphoneReady() const { return true; }
bool StickS3::startCapture() { hardware::capturing = true; return true; }
bool StickS3::captureFrame(int16_t* samples) {
    if (!hardware::frames || !hardware::capturing) return false;
    --hardware::frames; std::fill(samples, samples + 240, 1); return true;
}
void StickS3::stopCapture() { hardware::capturing = false; hardware::frames = 0; }
bool StickS3::play(const uint8_t*, uint32_t, uint8_t) { hardware::speaker = true; return true; }
bool StickS3::playbackFinished() {
    if (!hardware::speaker || !hardware::speakerFinished) return false;
    stopPlayback(); return true;
}
void StickS3::stopPlayback() { hardware::speaker = false; }
}

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

static NimBLEConnInfo peer;

static NimBLECharacteristic& characteristic(const char* uuid) {
    for (auto& service : NimBLEDevice::server.services)
        for (auto& value : service->characteristics)
            if (value->uuid == uuid) return *value;
    throw std::runtime_error("Missing characteristic");
}

static void reset() {
    device.stopCapture(); device.stopPlayback();
    playback.cancel(playback.id()); playback.notification();
    ble.~Ble(); new (&ble) Ble;
    NimBLEDevice::server = NimBLEServer{};
    NimBLEDevice::securityPasskey = 123456; NimBLEDevice::knownBond = true;
    NimBLEDevice::secureBond = true;
    ble_hs_cfg.sm_sc_only = false;
    hardware::queues.clear(); hardware::now = 100;
    hardware::button = hardware::capturing = hardware::speaker = hardware::speakerFinished = false;
    hardware::frames = 0;
    hardware::onPrepare = nullptr;
    state = State::Disconnected; initialized = negotiated = wasConnected = held = requested = serialHeld = microphoneOpen = false;
    encoder = ImaAdpcm{}; peer = NimBLEConnInfo{};
    setup(); CHECK(initialized);
}

static uint32_t displayPasskey() {
    // NimBLE 2.5.2 skips the callback when a non-default static key is installed.
    return NimBLEDevice::getSecurityPasskey() == 123456
        ? NimBLEDevice::server.callbacks->onPassKeyDisplay() : NimBLEDevice::getSecurityPasskey();
}

static void connect(bool bonded = true) {
    NimBLEDevice::knownBond = bonded;
    NimBLEDevice::server.disconnected = false;
    NimBLEDevice::server.callbacks->onConnect(&NimBLEDevice::server, peer);
    if (!bonded) displayPasskey();
    NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
}

static void subscribe(bool playbackSubscribed = true) {
    for (const auto* uuid : {atvvControl, atvvAudio, playbackStatus}) {
        auto& value = characteristic(uuid);
        value.callbacks->onSubscribe(&value, peer, uuid == playbackStatus && !playbackSubscribed ? 0 : 1);
    }
}

static void write(const char* uuid, std::initializer_list<uint8_t> bytes) {
    auto& value = characteristic(uuid); value.value = bytes;
    value.callbacks->onWrite(&value, peer);
}

static void ready(bool playbackSubscribed = true) {
    connect(); subscribe(playbackSubscribed); loop();
    write(atvvTx, {10, 1, 0, 0, 3, 3}); loop();
    CHECK(negotiated);
}

static void startPlayback() {
    write(playbackControl, {1, 1, 42, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1}); loop();
    write(playbackData, {42, 0, 0, 0, 0, 0, 0, 0, 0, 0}); loop();
    write(playbackControl, {1, 2, 42, 0, 0, 0}); loop();
    CHECK(hardware::speaker);
}

static void requiresSecureConnections() {
    CHECK(ble_hs_cfg.sm_sc_only);
}

static void rejectsRepairOutsideWindow() {
    ble.pairing(); connect(false); CHECK(ble.connected()); CHECK(!ble.pairingActive());
    NimBLEDevice::server.callbacks->onDisconnect(&NimBLEDevice::server, peer, 0);
    NimBLEDevice::knownBond = true;
    NimBLEDevice::server.disconnected = false;
    NimBLEDevice::server.callbacks->onConnect(&NimBLEDevice::server, peer);
    displayPasskey();
    NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
    CHECK(NimBLEDevice::server.disconnected);
    CHECK(!ble.connected());
}

static void discardsRecordingAcrossFastReconnect() {
    ready(); hardware::button = true; loop();
    write(atvvTx, {12, 0}); loop(); CHECK(hardware::capturing);
    auto& audio = characteristic(atvvAudio); const auto before = audio.notifications.size();
    write(playbackControl, {1, 1, 99, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1});
    NimBLEDevice::server.callbacks->onDisconnect(&NimBLEDevice::server, peer, 0);
    connect(); subscribe();
    write(atvvTx, {10, 1, 0, 0, 3, 3});
    hardware::frames = 1; loop();
    CHECK(!hardware::capturing);
    CHECK(audio.notifications.size() == before);
    CHECK(negotiated); // Preserve the new peer's handshake while discarding old writes.
    CHECK(playback.id() != 99);
}

static void retriesFailedStatus() {
    ready(); auto& status = characteristic(playbackStatus); status.failures = 1;
    write(playbackControl, {1, 1, 42, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1}); loop();
    CHECK(status.notifications.empty());
    hardware::now += 50; loop();
    CHECK(status.notifications.size() == 1);
    CHECK(status.notifications.back()[1] == 1 && status.notifications.back()[2] == 42);
}

static void stopsStalledSpeaker() {
    ready(); startPlayback(); hardware::now += 5100; loop();
    CHECK(!hardware::speaker);
    CHECK(playback.audio() == nullptr);
    CHECK(playback.state() == PlaybackState::Error && playback.error() == PlaybackError::Timeout);
    const auto& status = characteristic(playbackStatus).notifications.back();
    CHECK(status[1] == 5 && status[16] == 6);
}

static void rejectsStoredLegacyBond() {
    NimBLEDevice::secureBond = false;
    connect();
    CHECK(NimBLEDevice::server.disconnected);
    CHECK(!ble.connected());
}

static void permitsBondedReconnectWithoutPairing() {
    connect(); CHECK(ble.connected()); CHECK(!NimBLEDevice::server.disconnected);
    NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
    CHECK(ble.connected());
}

static void rejectsExpiredPairing() {
    ble.pairing(); NimBLEDevice::knownBond = false;
    NimBLEDevice::server.callbacks->onConnect(&NimBLEDevice::server, peer);
    displayPasskey(); hardware::now += 60000;
    NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
    CHECK(NimBLEDevice::server.disconnected && !ble.connected());
    ble.tick(); CHECK(ble.passkey() == 0);
}

static void rejectsRotatedPairingKey() {
    ble.pairing(); NimBLEDevice::knownBond = false;
    NimBLEDevice::server.callbacks->onConnect(&NimBLEDevice::server, peer);
    displayPasskey(); ble.pairing();
    NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
    CHECK(NimBLEDevice::server.disconnected && !ble.connected());
}

static void rejectsShortEncryptionKey() {
    peer.keySize = 8; connect();
    CHECK(NimBLEDevice::server.disconnected && !ble.connected());
}

static void blocksAudioBeforeNewSessionIsConsumed() {
    ready();
    NimBLEDevice::server.callbacks->onDisconnect(&NimBLEDevice::server, peer, 0);
    connect(); subscribe();
    const uint8_t audio[]{1, 2, 3};
    CHECK(!ble.sendAudio(audio, sizeof(audio)));
    CHECK(characteristic(atvvAudio).notifications.empty());
}

static void abandonsStatusAfterRetryDeadline() {
    ready(); characteristic(playbackStatus).failures = 1000;
    write(playbackControl, {1, 1, 42, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1}); loop();
    hardware::now += 5000; loop();
    CHECK(NimBLEDevice::server.disconnected && !ble.connected());
    CHECK(playback.audio() == nullptr);
}

static void retriesCompletionStatus() {
    ready(); startPlayback(); auto& status = characteristic(playbackStatus);
    status.failures = 1; hardware::speakerFinished = true;
    loop(); const auto before = status.notifications.size();
    hardware::now += 50; loop();
    CHECK(status.notifications.size() == before + 1);
    CHECK(status.notifications.back()[1] == 3 && status.notifications.back()[2] == 42);
    CHECK(!hardware::speaker && playback.audio() == nullptr);
}

static void neverRetriesOldStatusOnNewConnection() {
    ready(); auto& status = characteristic(playbackStatus); status.failures = 1;
    write(playbackControl, {1, 1, 42, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1}); loop();
    CHECK(status.notifications.empty());
    NimBLEDevice::server.callbacks->onDisconnect(&NimBLEDevice::server, peer, 0);
    connect(); subscribe(); hardware::now += 50; loop();
    CHECK(status.notifications.empty());
    CHECK(status.value[2] == 0);
}

static void retriesStatusAcrossClockWrap() {
    ready(); hardware::now = 0xfffffff0; auto& status = characteristic(playbackStatus); status.failures = 1;
    write(playbackControl, {1, 1, 42, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1}); loop();
    hardware::now += 50; loop();
    CHECK(status.notifications.size() == 1 && status.notifications.back()[1] == 1);
}

static void timesOutPlaybackAcrossClockWrap() {
    ready(); hardware::now = 0xfffffff0; startPlayback();
    hardware::now = playbackStarted + 5000; loop(); CHECK(hardware::speaker);
    loop(); CHECK(!hardware::speaker && playback.error() == PlaybackError::Timeout);
}

static void doesNotDisconnectAtvvOnlyRecording() {
    ready(false); hardware::button = true; loop(); write(atvvTx, {12, 0}); loop();
    hardware::now += 6000; loop();
    CHECK(ble.connected() && hardware::capturing);
    CHECK(!NimBLEDevice::server.disconnected);
}

static void preservesHandshakeDuringMicrophonePreparation() {
    NimBLEDevice::server.callbacks->onConnect(&NimBLEDevice::server, peer);
    hardware::onPrepare = [] {
        NimBLEDevice::server.callbacks->onAuthenticationComplete(peer);
        subscribe(); write(atvvTx, {10, 1, 0, 0, 3, 3});
    };
    loop(); loop();
    CHECK(negotiated);
    CHECK(!characteristic(atvvControl).notifications.empty());
}

static void discardsOldQueueOverflowOnReconnect() {
    ready();
    for (unsigned index = 0; index < 17; ++index) write(atvvTx, {10, 1, 0, 0, 3, 3});
    NimBLEDevice::server.callbacks->onDisconnect(&NimBLEDevice::server, peer, 0);
    connect(); subscribe(); loop();
    CHECK(ble.connected());
    CHECK(!NimBLEDevice::server.disconnected);
}

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"requires Secure Connections", requiresSecureConnections},
        {"rejects re-pairing outside the window", rejectsRepairOutsideWindow},
        {"discards recording across fast reconnect", discardsRecordingAcrossFastReconnect},
        {"retries failed status", retriesFailedStatus},
        {"stops a stalled speaker", stopsStalledSpeaker},
        {"rejects a stored Legacy bond", rejectsStoredLegacyBond},
        {"permits a bonded reconnect without pairing", permitsBondedReconnectWithoutPairing},
        {"rejects expired pairing and clears the code", rejectsExpiredPairing},
        {"rejects authentication with a rotated pairing key", rejectsRotatedPairingKey},
        {"rejects a short encryption key", rejectsShortEncryptionKey},
        {"blocks audio until the new session is consumed", blocksAudioBeforeNewSessionIsConsumed},
        {"disconnects when status retries reach their deadline", abandonsStatusAfterRetryDeadline},
        {"retries a failed completion notification", retriesCompletionStatus},
        {"never retries old status on a new connection", neverRetriesOldStatusOnNewConnection},
        {"retries status across clock wrap", retriesStatusAcrossClockWrap},
        {"times out playback across clock wrap", timesOutPlaybackAcrossClockWrap},
        {"keeps ATVV-only recording connected", doesNotDisconnectAtvvOnlyRecording},
        {"preserves a handshake during microphone preparation", preservesHandshakeDuringMicrophonePreparation},
        {"discards old queue overflow on reconnect", discardsOldQueueOverflowOnReconnect},
    };
    unsigned failures = 0;
    for (const auto& test : tests) {
        try { reset(); test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
