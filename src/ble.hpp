#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <array>
#include <mutex>

namespace cybercontroller {

enum class WriteKind : uint8_t { Atvv, Control, Data };
struct WriteEvent {
    WriteKind kind;
    uint16_t size;
    std::array<uint8_t, 512> bytes;
    uint32_t session;
};

class Ble final : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
public:
    bool begin();
    void printStatus();
    void pairing();
    void tick();
    bool connected() const { return authenticated_.load(); }
    bool sessionChanged();
    bool pairingActive() const;
    uint32_t passkey() const { return passkey_.load(); }
    bool pop(WriteEvent& event);
    bool overflowed() { return overflow_.exchange(false); }
    bool atvvSubscribed() const { return controlSubscribed_.load() && audioSubscribed_.load(); }
    bool sendControl(const uint8_t* bytes, size_t count);
    bool sendAudio(const uint8_t* bytes, size_t count);
    bool sendStatus(const std::array<uint8_t, 20>& status);
    void disconnect();

private:
    bool sessionActive() const { return connected() && generation_.load() == session_ && handle_.load() != BLE_HS_CONN_HANDLE_NONE; }
    bool notifyStatus();
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override;
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override;
    void onAuthenticationComplete(NimBLEConnInfo& info) override;
    uint32_t onPassKeyDisplay() override;
    void onConfirmPassKey(NimBLEConnInfo& info, uint32_t passkey) override;
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override;
    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) override;
    NimBLEServer* server_ = nullptr;
    NimBLECharacteristic* atvvTx_ = nullptr;
    NimBLECharacteristic* audio_ = nullptr;
    NimBLECharacteristic* control_ = nullptr;
    NimBLECharacteristic* playbackControl_ = nullptr;
    NimBLECharacteristic* playbackData_ = nullptr;
    NimBLECharacteristic* status_ = nullptr;
    QueueHandle_t writes_ = nullptr;
    std::atomic<bool> authenticated_{false}, controlSubscribed_{false}, audioSubscribed_{false}, statusSubscribed_{false}, overflow_{false};
    std::atomic<uint16_t> handle_{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<uint32_t> pairingUntil_{0}, passkey_{0};
    std::atomic<uint32_t> generation_{0};
    uint32_t session_ = 0;
    bool statusPending_ = false;
    uint32_t statusSession_ = 0, statusStarted_ = 0, statusAttempted_ = 0;
    std::mutex stateMutex_;
    bool bondedOnConnect_ = false, passkeyRequested_ = false;
    uint32_t pairingKey_ = 0;
};

}
