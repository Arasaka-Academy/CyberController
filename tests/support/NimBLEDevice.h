#pragma once

#include "Arduino.h"
#include <string>

namespace NIMBLE_PROPERTY {
inline constexpr uint32_t WRITE = 1, WRITE_NR = 2, WRITE_ENC = 4, WRITE_AUTHEN = 8,
    NOTIFY = 16, READ = 32, READ_ENC = 64, READ_AUTHEN = 128;
}
inline constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;
inline constexpr uint8_t BLE_HS_IO_DISPLAY_ONLY = 0;
inline struct { bool sm_sc_only = false; } ble_hs_cfg;
struct ble_addr_t { uint8_t type = 0; uint8_t val[6]{}; };
struct NimBLEAddress {
    ble_addr_t address;
    std::string toString() const { return "test-peer"; }
    const ble_addr_t* getBase() const { return &address; }
};
struct NimBLEConnInfo {
    uint16_t handle = 1;
    bool encrypted = true, authenticated = true, bonded = true;
    uint8_t keySize = 16;
    uint16_t getConnHandle() const { return handle; }
    NimBLEAddress getIdAddress() const { return {}; }
    bool isEncrypted() const { return encrypted; }
    bool isAuthenticated() const { return authenticated; }
    bool isBonded() const { return bonded; }
    uint8_t getSecKeySize() const { return keySize; }
};
struct NimBLEServer;
struct NimBLECharacteristic;
struct NimBLEServerCallbacks {
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
    virtual uint32_t onPassKeyDisplay() { return 123456; }
    virtual void onConfirmPassKey(NimBLEConnInfo&, uint32_t) {}
};
struct NimBLECharacteristicCallbacks {
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
    virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) {}
};
struct NimBLECharacteristic {
    std::string uuid;
    NimBLECharacteristicCallbacks* callbacks = nullptr;
    std::vector<uint8_t> value;
    std::vector<std::vector<uint8_t>> notifications;
    unsigned failures = 0, attempts = 0;
    void setCallbacks(NimBLECharacteristicCallbacks* next) { callbacks = next; }
    const std::vector<uint8_t>& getValue() const { return value; }
    void setValue(const uint8_t* bytes, size_t size) { value.assign(bytes, bytes + size); }
    bool notify(const uint8_t* bytes, size_t size, uint16_t handle) {
        ++attempts;
        if (handle == BLE_HS_CONN_HANDLE_NONE || failures) { if (failures) --failures; return false; }
        notifications.emplace_back(bytes, bytes + size);
        return true;
    }
    bool notify(uint16_t handle) { return notify(value.data(), value.size(), handle); }
};
struct NimBLEService {
    std::vector<std::unique_ptr<NimBLECharacteristic>> characteristics;
    NimBLECharacteristic* createCharacteristic(const char* uuid, uint32_t, size_t) {
        characteristics.emplace_back(new NimBLECharacteristic{});
        characteristics.back()->uuid = uuid;
        return characteristics.back().get();
    }
};
struct NimBLEServer {
    NimBLEServerCallbacks* callbacks = nullptr;
    bool disconnected = false;
    unsigned peers = 1;
    std::vector<std::unique_ptr<NimBLEService>> services;
    void setCallbacks(NimBLEServerCallbacks* next, bool) { callbacks = next; }
    void advertiseOnDisconnect(bool) {}
    NimBLEService* createService(const char*) {
        services.emplace_back(new NimBLEService{});
        return services.back().get();
    }
    unsigned getConnectedCount() const { return peers; }
    void disconnect(NimBLEConnInfo&) { disconnected = true; }
    void disconnect(uint16_t) { disconnected = true; }
    void updateConnParams(uint16_t, int, int, int, int) {}
    uint16_t getPeerMTU(uint16_t) const { return 247; }
};
struct NimBLEAdvertising {
    void enableScanResponse(bool) {}
    bool setName(const char*) { return true; }
    bool addServiceUUID(const char*) { return true; }
    bool start() { return true; }
    bool isAdvertising() const { return true; }
};
struct NimBLEDevice {
    inline static NimBLEServer server;
    inline static NimBLEAdvertising advertising;
    inline static uint32_t securityPasskey = 123456;
    inline static bool knownBond = true;
    inline static bool secureBond = true;
    static bool init(const char*) { return true; }
    static void setMTU(uint16_t) {}
    static void setSecurityAuth(bool, bool, bool) {}
    static void setSecurityIOCap(uint8_t) {}
    static NimBLEServer* createServer() { return &server; }
    static NimBLEAdvertising* getAdvertising() { return &advertising; }
    static NimBLEAddress getAddress() { return {}; }
    static unsigned getNumBonds() { return knownBond ? 1 : 0; }
    static void setSecurityPasskey(uint32_t value) { securityPasskey = value; }
    static uint32_t getSecurityPasskey() { return securityPasskey; }
    static bool isBonded(NimBLEAddress) { return knownBond; }
    static bool startSecurity(uint16_t) { return true; }
    static void injectConfirmPasskey(NimBLEConnInfo&, bool) {}
};

struct ble_store_key_sec { ble_addr_t peer_addr; uint8_t idx = 0; };
struct ble_store_value_sec { uint8_t key_size = 16; bool authenticated = true, sc = true; };
inline int ble_store_read_our_sec(const ble_store_key_sec*, ble_store_value_sec* value) {
    value->sc = NimBLEDevice::secureBond;
    return NimBLEDevice::knownBond ? 0 : 1;
}
