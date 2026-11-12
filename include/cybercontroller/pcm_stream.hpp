#pragma once
#include <algorithm>
#include <cstdint>
#include "codec.hpp"

namespace cybercontroller {
// Caller owns encoded bytes until reset; decoding runs only in the application loop.
class PcmStream {
public:
    void begin(const uint8_t* encoded, uint32_t samples) {
        reset(); encoded_ = encoded; samples_ = samples;
    }
    void reset() { encoded_ = nullptr; samples_ = position_ = 0; decoder_ = ImaAdpcm{}; }
    size_t read(int16_t* stereo, size_t frames) {
        if (!encoded_) return 0;
        const auto count = std::min<uint32_t>(frames, samples_ + 1600 - position_);
        for (uint32_t i = 0; i < count; ++i, ++position_) {
            int16_t value = 0;
            if (position_ < samples_) {
                const auto byte = encoded_[position_ / 2];
                value = decoder_.decodeNibble(position_ % 2 ? byte & 15 : byte >> 4);
            }
            stereo[2 * i] = stereo[2 * i + 1] = value;
        }
        return count;
    }
    bool finished() const { return encoded_ && position_ == samples_ + 1600; }
private:
    const uint8_t* encoded_ = nullptr;
    uint32_t samples_ = 0, position_ = 0;
    ImaAdpcm decoder_;
};
}
