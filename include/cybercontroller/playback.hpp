#pragma once

#include <array>
#include <cstdlib>
#include <cstring>

#include "codec.hpp"

namespace cybercontroller {

inline constexpr const char* playbackService = "d9870001-bc7a-4c40-8c98-781a66a7b401";
inline constexpr const char* playbackControl = "d9870002-bc7a-4c40-8c98-781a66a7b401";
inline constexpr const char* playbackData = "d9870003-bc7a-4c40-8c98-781a66a7b401";
inline constexpr const char* playbackStatus = "d9870004-bc7a-4c40-8c98-781a66a7b401";

inline uint32_t read32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
inline void write32(uint8_t* bytes, uint32_t value) {
    for (unsigned position = 0; position < 4; ++position) bytes[position] = static_cast<uint8_t>(value >> (position * 8));
}

enum class PlaybackState : uint8_t { Receiving = 1, Playing = 2, Completed = 3, Cancelled = 4, Error = 5 };
enum class PlaybackError : uint16_t { None, Invalid, Busy, Sequence, Checksum, Allocation, Timeout, Audio, Disconnected };

class Playback {
public:
    using Allocate = void* (*)(size_t);
    using Release = void (*)(void*);
    explicit Playback(Allocate allocate = std::malloc, Release release = std::free, uint32_t maxSamples = 16000 * 130)
        : maxSamples_(maxSamples), allocate_(allocate), release_(release) {}
    ~Playback() { clear(); }
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    bool start(const uint8_t* packet, size_t size, uint32_t now) {
        if (state_ == PlaybackState::Receiving || state_ == PlaybackState::Playing) return fail(PlaybackError::Busy);
        if (size != 20 || packet[0] != 1 || packet[1] != 1) return fail(PlaybackError::Invalid);
        id_ = read32(packet + 2);
        samples_ = read32(packet + 6);
        length_ = read32(packet + 10);
        checksum_ = read32(packet + 14);
        volume_ = packet[18];
        received_ = sequence_ = 0;
        if (id_ == 0 || samples_ == 0 || samples_ > maxSamples_ || length_ > 2 * 1024 * 1024
            || length_ != (samples_ + 1) / 2 || volume_ > 100 || packet[19] != 1) return fail(PlaybackError::Invalid);
        clear();
        audio_ = static_cast<uint8_t*>(allocate_(length_));
        if (!audio_) return fail(PlaybackError::Allocation);
        started_ = now;
        state_ = PlaybackState::Receiving;
        error_ = PlaybackError::None;
        ready_ = false;
        notification_ = true;
        return true;
    }
    bool append(const uint8_t* packet, size_t size) {
        if (state_ != PlaybackState::Receiving || ready_ || !audio_ || size < 9 || size > 512
            || read32(packet) != id_ || read32(packet + 4) != sequence_
            || size - 8 > length_ - received_) return fail(PlaybackError::Sequence);
        std::memcpy(audio_ + received_, packet + 8, size - 8);
        received_ += size - 8;
        ++sequence_;
        if (sequence_ % 8 == 0 || received_ == length_) notification_ = true;
        return true;
    }
    bool finish(uint32_t id) {
        if (id != id_ || state_ != PlaybackState::Receiving || !audio_ || received_ != length_) return fail(PlaybackError::Invalid);
        if (crc32(audio_, length_) != checksum_) return fail(PlaybackError::Checksum);
        ready_ = true;
        return true;
    }
    void playing() { if (ready_) { state_ = PlaybackState::Playing; ready_ = false; notification_ = true; } }
    void completed() { if (state_ == PlaybackState::Playing) { clear(); state_ = PlaybackState::Completed; notification_ = true; } }
    void cancel(uint32_t id) {
        if (id != id_) return;
        clear(); state_ = PlaybackState::Cancelled; error_ = PlaybackError::None; notification_ = true;
    }
    bool fail(PlaybackError error) {
        clear(); state_ = PlaybackState::Error; error_ = error; notification_ = true;
        return false;
    }
    void tick(uint32_t now) { if (state_ == PlaybackState::Receiving && now - started_ >= 120000) fail(PlaybackError::Timeout); }
    bool notification() { bool pending = notification_; notification_ = false; return pending; }
    std::array<uint8_t, 20> status() const {
        std::array<uint8_t, 20> bytes{};
        bytes[0] = 1; bytes[1] = static_cast<uint8_t>(state_);
        write32(bytes.data() + 2, id_); write32(bytes.data() + 6, sequence_); write32(bytes.data() + 10, received_);
        bytes[14] = state_ == PlaybackState::Receiving && !ready_ ? 8 : 0;
        bytes[16] = static_cast<uint16_t>(error_) & 255;
        bytes[17] = static_cast<uint16_t>(error_) >> 8;
        return bytes;
    }
    PlaybackState state() const { return state_; }
    PlaybackError error() const { return error_; }
    bool ready() const { return ready_; }
    const uint8_t* audio() const { return audio_; }
    uint32_t samples() const { return samples_; }
    uint32_t id() const { return id_; }
    uint8_t volume() const { return volume_; }

private:
    void clear() {
        if (audio_) { std::memset(audio_, 0, length_); release_(audio_); audio_ = nullptr; }
        ready_ = false;
    }
    uint32_t maxSamples_;
    Allocate allocate_;
    Release release_;
    uint8_t* audio_ = nullptr;
    PlaybackState state_ = PlaybackState::Cancelled;
    PlaybackError error_ = PlaybackError::None;
    uint32_t id_ = 0, samples_ = 0, length_ = 0, checksum_ = 0, received_ = 0, sequence_ = 0, started_ = 0;
    uint8_t volume_ = 0;
    bool ready_ = false, notification_ = false;
};

}
