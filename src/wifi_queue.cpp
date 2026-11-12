#include "wifi.hpp"
#include <cstring>
namespace cybercontroller {
void Wifi::pairing() { configureRequested_.store(true); }
void Wifi::disconnect() { authenticated_.store(false); disconnectRequested_.store(true); }
bool Wifi::sessionChanged() { const auto value = generation_.load(); if (value == session_) return false; session_ = value; return true; }
void Wifi::printStatus() { Serial.printf("WIFI connected=%u setup=%u session=%lu\n", connected(), pairingActive(), static_cast<unsigned long>(generation_.load())); }
bool Wifi::pop(WriteEvent& event) {
    if (!connected() || generation_.load() != session_) return false;
    while (xQueuePeek(input_, &event, 0) == pdTRUE) {
        // A READY arriving halfway through an application tick belongs to the next session.
        // Leave its handshake queued until sessionChanged() has consumed that generation.
        if (event.session != session_ && event.session == generation_.load()) return false;
        xQueueReceive(input_, &event, 0);
        if (event.session == session_ && connected() && generation_.load() == session_) return true;
    }
    return false;
}
void Wifi::incoming(WriteKind kind, const uint8_t* bytes, size_t size) {
    if (size > 512) { overflow_.store(true); return; }
    WriteEvent event{}; event.kind=kind; event.size=size; event.session=generation_.load();
    std::memcpy(event.bytes.data(), bytes, size);
    if (xQueueSend(input_, &event, 0) != pdTRUE) overflow_.store(true);
}
bool Wifi::send(uint8_t type, const uint8_t* bytes, size_t size) {
    if (!connected() || size > 512) return false;
    Packet packet{}; packet.bytes[0]=type; packet.size=size+1; packet.session=generation_.load();
    if (size) std::memcpy(packet.bytes.data()+1, bytes, size);
    if (xQueueSend(output_, &packet, 0) == pdTRUE) return true;
    overflow_.store(true); return false;
}
bool Wifi::sendAudio(const uint8_t* bytes, size_t size) { return send(2, bytes, size); }
bool Wifi::sendStatus(const std::array<uint8_t,20>& value) { return send(6, value.data(), value.size()); }
bool Wifi::sendControl(const uint8_t* bytes, size_t size) {
    if (size == 1 && bytes[0] == 8) {
        if (!send(1, nullptr, 0)) return false;
        const uint8_t opened[]{12,0}; incoming(WriteKind::Atvv, opened, sizeof(opened));
    } else if (size == 1 && bytes[0] == 0) return send(3, nullptr, 0);
    return connected();
}
}
