#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace hardware {
inline uint32_t now = 100;
inline bool button = false, capturing = false, speaker = false, speakerFinished = false;
inline unsigned frames = 0;
inline void (*onPrepare)() = nullptr;
struct Queue { size_t capacity, size; std::deque<std::vector<uint8_t>> values; };
inline std::vector<std::unique_ptr<Queue>> queues;
}

inline uint32_t millis() { return hardware::now; }
inline void delay(uint32_t duration) { hardware::now += duration; }
using QueueHandle_t = hardware::Queue*;
inline constexpr int pdTRUE = 1;
inline QueueHandle_t xQueueCreate(size_t capacity, size_t size) {
    hardware::queues.emplace_back(new hardware::Queue{capacity, size, {}});
    return hardware::queues.back().get();
}
inline int xQueueSend(QueueHandle_t queue, const void* value, unsigned) {
    if (queue->values.size() == queue->capacity) return 0;
    const auto* bytes = static_cast<const uint8_t*>(value);
    queue->values.emplace_back(bytes, bytes + queue->size);
    return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* value, unsigned) {
    if (queue->values.empty()) return 0;
    std::memcpy(value, queue->values.front().data(), queue->size);
    queue->values.pop_front();
    return pdTRUE;
}
inline void xQueueReset(QueueHandle_t queue) { queue->values.clear(); }

inline struct {
    void begin(unsigned) {}
    int available() const { return 0; }
    char read() const { return 0; }
    int printf(const char*, ...) const { return 0; }
    void println(const char*) const {}
    size_t write(const uint8_t*, size_t count) const { return count; }
} Serial;
inline struct {
    uint32_t getFlashChipSize() const { return 8388608; }
    uint32_t getPsramSize() const { return 8388608; }
    uint32_t getFreePsram() const { return 7000000; }
    uint32_t getFreeHeap() const { return 200000; }
} ESP;
