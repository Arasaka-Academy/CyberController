#include "ble.hpp"

#include <esp_random.h>
#include <cstring>
#ifdef USING_NIMBLE_ARDUINO_HEADERS
#include <nimble/nimble/host/include/host/ble_hs.h>
#include <nimble/nimble/host/include/host/ble_store.h>
#else
#include <host/ble_hs.h>
#include <host/ble_store.h>
#endif
#include "cybercontroller/playback.hpp"

namespace cybercontroller {

static constexpr const char* atvvService = "ab5e0001-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvTx = "ab5e0002-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvAudio = "ab5e0003-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvControl = "ab5e0004-5a21-4f05-bc7d-af01f617b664";

static bool secureBond(const NimBLEConnInfo& info) {
    ble_store_key_sec key{};
    key.peer_addr = *info.getIdAddress().getBase();
    ble_store_value_sec value{};
    const bool secure = ble_store_read_our_sec(&key, &value) == 0
        && value.sc && value.authenticated && value.key_size == 16;
    std::memset(&value, 0, sizeof(value));
    return secure;
}

bool Ble::begin() {
    writes_ = xQueueCreate(16, sizeof(WriteEvent));
    if (!writes_ || !NimBLEDevice::init("CyberController StickS3")) return false;
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setSecurityAuth(true, true, true);
    ble_hs_cfg.sm_sc_only = 1;
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    // NimBLE 2.5.2 calls onPassKeyDisplay only with its default static key.
    NimBLEDevice::setSecurityPasskey(123456);
    server_ = NimBLEDevice::createServer();
    if (!server_) return false;
    server_->setCallbacks(this, false);
    server_->advertiseOnDisconnect(true);
    auto* input = server_->createService(atvvService);
    const uint32_t writable = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;
    atvvTx_ = input->createCharacteristic(atvvTx, writable, 20);
    atvvTx_->setCallbacks(this);
    audio_ = input->createCharacteristic(atvvAudio, NIMBLE_PROPERTY::NOTIFY, 512);
    control_ = input->createCharacteristic(atvvControl, NIMBLE_PROPERTY::NOTIFY, 20);
    audio_->setCallbacks(this); control_->setCallbacks(this);
    auto* output = server_->createService(playbackService);
    playbackControl_ = output->createCharacteristic(playbackControl, writable, 20);
    playbackData_ = output->createCharacteristic(playbackData, writable, 512);
    status_ = output->createCharacteristic(playbackStatus, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY, 20);
    playbackControl_->setCallbacks(this); playbackData_->setCallbacks(this); status_->setCallbacks(this);
    const std::array<uint8_t, 20> initial{1, 4};
    status_->setValue(initial.data(), initial.size());
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->enableScanResponse(true);
    return advertising->setName("CyberController StickS3") && advertising->addServiceUUID(atvvService) && advertising->start();
}

void Ble::printStatus() {
    Serial.printf("BLE address=%s advertising=%u bonds=%u\n", NimBLEDevice::getAddress().toString().c_str(), NimBLEDevice::getAdvertising()->isAdvertising(), NimBLEDevice::getNumBonds());
}

bool Ble::pairingActive() const {
    const uint32_t deadline = pairingUntil_.load();
    return deadline != 0 && static_cast<int32_t>(deadline - millis()) > 0;
}

void Ble::pairing() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    passkey_.store(esp_random() % 1000000);
    pairingUntil_.store(millis() + 60000);
    Serial.printf("PAIRING %06lu (60s)\n", static_cast<unsigned long>(passkey_.load()));
}

void Ble::tick() {
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (pairingUntil_.load() && !pairingActive()) {
            pairingUntil_.store(0); passkey_.store(0); expired = true;
        }
    }
    if (expired && !connected()) disconnect();
    if (!statusPending_) return;
    if (!sessionActive() || statusSession_ != session_ || !statusSubscribed_.load()) { statusPending_ = false; return; }
    if (millis() - statusStarted_ >= 5000) { statusPending_ = false; disconnect(); return; }
    if (millis() - statusAttempted_ >= 20) notifyStatus();
}

void Ble::onConnect(NimBLEServer* server, NimBLEConnInfo& info) {
    const bool bonded = NimBLEDevice::isBonded(info.getIdAddress()) && secureBond(info);
    if (server->getConnectedCount() != 1 || (!pairingActive() && !bonded)) {
        server->disconnect(info); return;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        bondedOnConnect_ = bonded;
        passkeyRequested_ = false; pairingKey_ = 0;
        authenticated_.store(false);
        controlSubscribed_.store(false); audioSubscribed_.store(false); statusSubscribed_.store(false);
        generation_.fetch_add(1);
        xQueueReset(writes_);
        overflow_.store(false);
        const std::array<uint8_t, 20> initial{1, 4};
        status_->setValue(initial.data(), initial.size());
        handle_.store(info.getConnHandle());
    }
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    NimBLEDevice::startSecurity(info.getConnHandle());
}

void Ble::onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (info.getConnHandle() != handle_.load()) return;
        authenticated_.store(false); handle_.store(BLE_HS_CONN_HANDLE_NONE);
        generation_.fetch_add(1);
        controlSubscribed_.store(false); audioSubscribed_.store(false); statusSubscribed_.store(false);
        xQueueReset(writes_);
        overflow_.store(false);
        passkeyRequested_ = false; pairingKey_ = 0;
    }
    Serial.printf("BLE disconnected %d\n", reason);
}

void Ble::onAuthenticationComplete(NimBLEConnInfo& info) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (info.getConnHandle() != handle_.load()) { server_->disconnect(info); return; }
    const bool pairing = !bondedOnConnect_ || passkeyRequested_;
    if (!info.isEncrypted() || !info.isAuthenticated()
        || !info.isBonded() || info.getSecKeySize() != 16
        || (pairing && (!passkeyRequested_ || !pairingActive() || pairingKey_ != passkey_.load()))) {
        authenticated_.store(false); server_->disconnect(info); return;
    }
    if (!authenticated_.load()) generation_.fetch_add(1);
    authenticated_.store(true); pairingUntil_.store(0); passkey_.store(0); pairingKey_ = 0;
    bondedOnConnect_ = true; passkeyRequested_ = false;
    Serial.println("BLE authenticated and bonded");
}

uint32_t Ble::onPassKeyDisplay() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    passkeyRequested_ = true;
    if (!pairingActive()) {
        authenticated_.store(false);
        const auto handle = handle_.load();
        if (handle != BLE_HS_CONN_HANDLE_NONE) server_->disconnect(handle);
        return esp_random() % 1000000;
    }
    pairingKey_ = passkey_.load();
    return pairingKey_;
}
void Ble::onConfirmPassKey(NimBLEConnInfo& info, uint32_t) { NimBLEDevice::injectConfirmPasskey(info, false); }

void Ble::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
    if (!connected() || info.getConnHandle() != handle_.load() || !info.isEncrypted() || !info.isAuthenticated()) return;
    const auto& value = characteristic->getValue();
    if (!writes_ || value.size() > 512) { overflow_.store(true); return; }
    WriteEvent event{};
    event.kind = characteristic == atvvTx_ ? WriteKind::Atvv : characteristic == playbackControl_ ? WriteKind::Control : WriteKind::Data;
    event.size = value.size();
    event.session = generation_.load();
    std::memcpy(event.bytes.data(), value.data(), value.size());
    if (xQueueSend(writes_, &event, 0) != pdTRUE) overflow_.store(true);
}

void Ble::onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) {
    if (info.getConnHandle() != handle_.load()) return;
    if (characteristic == control_) controlSubscribed_.store(value & 1);
    if (characteristic == audio_) audioSubscribed_.store(value & 1);
    if (characteristic == status_) statusSubscribed_.store(value & 1);
}

bool Ble::sessionChanged() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto generation = generation_.load();
    if (generation == session_) return false;
    session_ = generation;
    return true;
}
bool Ble::pop(WriteEvent& event) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!sessionActive() || !writes_) return false;
    while (xQueueReceive(writes_, &event, 0) == pdTRUE) {
        if (event.session == session_) return true;
    }
    return false;
}
bool Ble::sendControl(const uint8_t* bytes, size_t count) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return sessionActive() && controlSubscribed_.load() && control_->notify(bytes, count, handle_.load());
}
bool Ble::sendAudio(const uint8_t* bytes, size_t count) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!sessionActive() || !audioSubscribed_.load()) return false;
    const auto handle = handle_.load();
    const size_t capacity = std::max<uint16_t>(23, server_->getPeerMTU(handle)) - 3;
    for (size_t offset = 0; offset < count; offset += capacity) {
        if (!audio_->notify(bytes + offset, std::min(capacity, count - offset), handle)) return false;
    }
    return true;
}
bool Ble::sendStatus(const std::array<uint8_t, 20>& status) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!sessionActive()) return false;
        status_->setValue(status.data(), status.size());
        if (!statusSubscribed_.load()) { statusPending_ = false; return false; }
        if (!statusPending_ || statusSession_ != session_) statusStarted_ = millis();
        statusSession_ = session_; statusPending_ = true;
    }
    return notifyStatus();
}
bool Ble::notifyStatus() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!statusPending_ || !sessionActive() || statusSession_ != session_ || !statusSubscribed_.load()) {
        statusPending_ = false; return false;
    }
    statusAttempted_ = millis();
    if (!status_->notify(handle_.load())) return false;
    statusPending_ = false;
    return true;
}
void Ble::disconnect() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    authenticated_.store(false);
    const auto handle = handle_.load();
    if (handle != BLE_HS_CONN_HANDLE_NONE) server_->disconnect(handle);
}

}
