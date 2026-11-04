#include "ble.hpp"

#include <esp_random.h>
#include <cstring>
#include "cybercontroller/playback.hpp"

namespace cybercontroller {

static constexpr const char* atvvService = "ab5e0001-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvTx = "ab5e0002-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvAudio = "ab5e0003-5a21-4f05-bc7d-af01f617b664";
static constexpr const char* atvvControl = "ab5e0004-5a21-4f05-bc7d-af01f617b664";

bool Ble::begin() {
    writes_ = xQueueCreate(16, sizeof(WriteEvent));
    if (!writes_ || !NimBLEDevice::init("CyberController StickS3")) return false;
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
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
    passkey_.store(esp_random() % 1000000);
    NimBLEDevice::setSecurityPasskey(passkey_.load());
    pairingUntil_.store(millis() + 60000);
    Serial.printf("PAIRING %06lu (60s)\n", static_cast<unsigned long>(passkey_.load()));
}

void Ble::tick() {
    if (pairingUntil_.load() && !pairingActive()) { pairingUntil_.store(0); if (!connected()) disconnect(); }
}

void Ble::onConnect(NimBLEServer* server, NimBLEConnInfo& info) {
    if (server->getConnectedCount() != 1 || (!pairingActive() && !NimBLEDevice::isBonded(info.getIdAddress()))) {
        server->disconnect(info); return;
    }
    handle_.store(info.getConnHandle());
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    NimBLEDevice::startSecurity(info.getConnHandle());
}

void Ble::onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) {
    if (info.getConnHandle() != handle_.load()) return;
    authenticated_.store(false); handle_.store(BLE_HS_CONN_HANDLE_NONE);
    controlSubscribed_.store(false); audioSubscribed_.store(false); statusSubscribed_.store(false);
    Serial.printf("BLE disconnected %d\n", reason);
}

void Ble::onAuthenticationComplete(NimBLEConnInfo& info) {
    if (info.getConnHandle() != handle_.load() || !info.isEncrypted() || !info.isAuthenticated() || !info.isBonded()) { server_->disconnect(info); return; }
    authenticated_.store(true); pairingUntil_.store(0);
    Serial.println("BLE authenticated and bonded");
}

uint32_t Ble::onPassKeyDisplay() {
    if (!pairingActive()) { disconnect(); return esp_random() % 1000000; }
    return passkey_.load();
}
void Ble::onConfirmPassKey(NimBLEConnInfo& info, uint32_t) { NimBLEDevice::injectConfirmPasskey(info, false); }

void Ble::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
    if (!connected() || info.getConnHandle() != handle_.load() || !info.isEncrypted() || !info.isAuthenticated()) return;
    const auto& value = characteristic->getValue();
    if (!writes_ || value.size() > 512) { overflow_.store(true); return; }
    WriteEvent event{};
    event.kind = characteristic == atvvTx_ ? WriteKind::Atvv : characteristic == playbackControl_ ? WriteKind::Control : WriteKind::Data;
    event.size = value.size();
    std::memcpy(event.bytes.data(), value.data(), value.size());
    if (xQueueSend(writes_, &event, 0) != pdTRUE) overflow_.store(true);
}

void Ble::onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) {
    if (info.getConnHandle() != handle_.load()) return;
    if (characteristic == control_) controlSubscribed_.store(value & 1);
    if (characteristic == audio_) audioSubscribed_.store(value & 1);
    if (characteristic == status_) statusSubscribed_.store(value & 1);
}

bool Ble::pop(WriteEvent& event) { return writes_ && xQueueReceive(writes_, &event, 0) == pdTRUE; }
bool Ble::sendControl(const uint8_t* bytes, size_t count) { return connected() && controlSubscribed_.load() && control_->notify(bytes, count, handle_.load()); }
bool Ble::sendAudio(const uint8_t* bytes, size_t count) {
    if (!connected() || !audioSubscribed_.load()) return false;
    const size_t capacity = std::max<uint16_t>(23, server_->getPeerMTU(handle_.load())) - 3;
    for (size_t offset = 0; offset < count; offset += capacity) {
        if (!audio_->notify(bytes + offset, std::min(capacity, count - offset), handle_.load())) return false;
    }
    return true;
}
bool Ble::sendStatus(const std::array<uint8_t, 20>& status) {
    status_->setValue(status.data(), status.size());
    return connected() && statusSubscribed_.load() && status_->notify(handle_.load());
}
void Ble::disconnect() { const auto handle = handle_.load(); if (handle != BLE_HS_CONN_HANDLE_NONE) server_->disconnect(handle); }

}
