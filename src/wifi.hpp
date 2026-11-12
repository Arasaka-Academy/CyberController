#pragma once
#include <Arduino.h>
#include <atomic>
#include "transport.hpp"
namespace cybercontroller {
// The network worker owns sockets; the audio loop only accesses bounded queues.
class Wifi {
public:
    bool begin();
    void printStatus();
    void pairing();
    void tick() {}
    bool connected() const { return authenticated_.load(); }
    bool sessionChanged();
    bool pairingActive() const { return configuring_.load(); }
    uint32_t passkey() const { return setupKey_; }
    bool pop(WriteEvent& event);
    bool overflowed() { return overflow_.exchange(false); }
    bool atvvSubscribed() const { return connected(); }
    bool sendControl(const uint8_t*, size_t);
    bool sendAudio(const uint8_t*, size_t);
    bool sendStatus(const std::array<uint8_t,20>&);
    void disconnect();
private:
    struct Packet { uint16_t size; std::array<uint8_t,513> bytes; uint32_t session; };
    bool send(uint8_t, const uint8_t*, size_t);
    void incoming(WriteKind, const uint8_t*, size_t);
    void worker();
    static void task(void* self) { static_cast<Wifi*>(self)->worker(); }
    QueueHandle_t input_ = nullptr, output_ = nullptr;
    std::atomic<bool> authenticated_{false}, configuring_{false}, configureRequested_{false}, disconnectRequested_{false}, overflow_{false};
    std::atomic<uint32_t> generation_{0};
    uint32_t session_ = 0, setupKey_ = 0;
};
}
