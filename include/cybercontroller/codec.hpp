#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cybercontroller {

inline constexpr int32_t imaSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
inline constexpr int32_t imaIndexes[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

class ImaAdpcm {
public:
    int16_t decodeNibble(uint8_t nibble) {
        const int32_t step = imaSteps[index_];
        int32_t difference = step >> 3;
        if (nibble & 1) difference += step >> 2;
        if (nibble & 2) difference += step >> 1;
        if (nibble & 4) difference += step;
        predictor_ = std::clamp<int32_t>(predictor_ + ((nibble & 8) ? -difference : difference), -32768, 32767);
        index_ = std::clamp<int32_t>(index_ + imaIndexes[nibble & 7], 0, 88);
        return static_cast<int16_t>(predictor_);
    }
    void encode(const int16_t* samples, size_t count, uint8_t* bytes) {
        for (size_t position = 0; position < count; ++position) {
            int32_t difference = static_cast<int32_t>(samples[position]) - predictor_;
            uint8_t nibble = difference < 0 ? 8 : 0;
            if (difference < 0) difference = -difference;
            int32_t step = imaSteps[index_];
            for (uint8_t bit : {4, 2, 1}) {
                if (difference >= step) { nibble |= bit; difference -= step; }
                step >>= 1;
            }
            decodeNibble(nibble);
            if (position % 2 == 0) bytes[position / 2] = nibble << 4;
            else bytes[position / 2] |= nibble;
        }
    }
private:
    int32_t predictor_ = 0;
    int32_t index_ = 0;
};

inline uint32_t crc32(const uint8_t* bytes, size_t count, uint32_t crc = 0xffffffff) {
    for (size_t position = 0; position < count; ++position) {
        crc ^= bytes[position];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

}
