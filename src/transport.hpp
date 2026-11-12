#pragma once
#include <array>
#include <cstdint>
namespace cybercontroller {
enum class WriteKind : uint8_t { Atvv, Control, Data };
struct WriteEvent {
    WriteKind kind;
    uint16_t size;
    std::array<uint8_t, 512> bytes;
    uint32_t session;
};

}
