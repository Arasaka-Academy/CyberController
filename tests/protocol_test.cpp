#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cybercontroller/codec.hpp"
#include "cybercontroller/playback.hpp"

using namespace cybercontroller;

static std::array<uint8_t, 20> startPacket(uint32_t id = 42) {
    std::array<uint8_t, 20> packet{1, 1, 0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0xff, 0x12, 0xd9, 0x41, 5, 1};
    write32(packet.data() + 2, id);
    return packet;
}

int main() {
    assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926);
    ImaAdpcm encoder;
    std::array<int16_t, 6> reference{1, 4, 15, 17, -8, -64};
    std::array<uint8_t, 3> encoded{};
    encoder.encode(reference.data(), reference.size(), encoded.data());
    assert((encoded == std::array<uint8_t, 3>{0x12, 0x70, 0xff}));
    ImaAdpcm decoder;
    for (size_t position = 0; position < reference.size(); ++position) {
        assert(decoder.decodeNibble(position % 2 == 0 ? encoded[position / 2] >> 4 : encoded[position / 2] & 15) == reference[position]);
    }
    Playback receiver;
    auto start = startPacket();
    assert(receiver.start(start.data(), start.size(), 100));
    assert(!receiver.ready());
    const std::array<uint8_t, 10> data{42, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(receiver.append(data.data(), data.size()));
    assert(!receiver.ready());
    assert(receiver.finish(42));
    assert(receiver.ready());
    assert(receiver.state() != PlaybackState::Completed);
    receiver.playing();
    assert(receiver.state() == PlaybackState::Playing);
    receiver.completed();
    assert(receiver.state() == PlaybackState::Completed);
    assert(receiver.audio() == nullptr);
    const std::array<uint8_t, 20> completed{1, 3, 42, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(receiver.status() == completed);

    start = startPacket(43);
    start[14] = 0;
    assert(receiver.start(start.data(), start.size(), 200));
    auto corrupted = data; corrupted[0] = 43;
    assert(receiver.append(corrupted.data(), corrupted.size()));
    assert(!receiver.finish(43));
    assert(receiver.error() == PlaybackError::Checksum);
    assert(receiver.audio() == nullptr);

    start = startPacket(44);
    assert(receiver.start(start.data(), start.size(), 300));
    auto skipped = data; skipped[0] = 44; skipped[4] = 1;
    assert(!receiver.append(skipped.data(), skipped.size()));
    assert(receiver.error() == PlaybackError::Sequence);
    assert(receiver.audio() == nullptr);

    start = startPacket(45);
    assert(receiver.start(start.data(), start.size(), 400));
    receiver.cancel(44);
    assert(receiver.state() == PlaybackState::Receiving);
    receiver.cancel(45);
    assert(receiver.state() == PlaybackState::Cancelled);
    assert(receiver.audio() == nullptr);

    start = startPacket(46);
    assert(receiver.start(start.data(), start.size(), 500));
    receiver.tick(120500);
    assert(receiver.error() == PlaybackError::Timeout);
    assert(receiver.audio() == nullptr);

    for (uint32_t samples : {0u, 2080001u, 0xffffffffu}) {
        start = startPacket(47); write32(start.data() + 6, samples);
        assert(!receiver.start(start.data(), start.size(), 0));
    }
    start = startPacket(48); write32(start.data() + 10, 2097153);
    assert(!receiver.start(start.data(), start.size(), 0));
    start = startPacket(49); start[19] = 2;
    assert(!receiver.start(start.data(), start.size(), 0));
    assert(!receiver.start(start.data(), 19, 0));
    start = startPacket(50); write32(start.data() + 6, 1200); write32(start.data() + 10, 600);
    assert(receiver.start(start.data(), start.size(), 0));
    std::array<uint8_t, 513> oversized{}; write32(oversized.data(), 50);
    assert(!receiver.append(oversized.data(), oversized.size()));
    assert(receiver.audio() == nullptr);

    Playback flow;
    start = startPacket(51);
    write32(start.data() + 6, 18);
    write32(start.data() + 10, 9);
    const std::array<uint8_t, 9> silence{};
    write32(start.data() + 14, crc32(silence.data(), silence.size()));
    assert(flow.start(start.data(), start.size(), 0));
    assert(flow.notification());
    assert(flow.status()[14] == 8);
    for (uint32_t sequence = 0; sequence < 9; ++sequence) {
        std::array<uint8_t, 9> fragment{};
        write32(fragment.data(), 51);
        write32(fragment.data() + 4, sequence);
        assert(flow.append(fragment.data(), fragment.size()));
        assert(flow.notification() == (sequence == 7 || sequence == 8));
        assert(!flow.ready());
    }
    assert(flow.finish(51));
    assert(flow.status()[14] == 0);
    flow.playing();
    flow.cancel(50);
    assert(flow.state() == PlaybackState::Playing);
    assert(flow.audio() != nullptr);
    flow.fail(PlaybackError::Disconnected);
    assert(flow.audio() == nullptr);
    assert(flow.state() == PlaybackState::Error);
    assert(flow.error() == PlaybackError::Disconnected);
    assert(flow.status()[14] == 0);

    start = startPacket(52);
    assert(receiver.start(start.data(), start.size(), 0));
    assert(!receiver.finish(52));
    assert(receiver.audio() == nullptr);
    assert(receiver.state() != PlaybackState::Playing);
    assert(receiver.start(start.data(), start.size(), 0));
    assert(!receiver.start(start.data(), start.size(), 0));
    assert(receiver.error() == PlaybackError::Busy);
    assert(receiver.audio() == nullptr);

    Playback unavailable([](size_t) -> void* { return nullptr; });
    assert(!unavailable.start(start.data(), start.size(), 0));
    assert(unavailable.error() == PlaybackError::Allocation);
    assert(unavailable.audio() == nullptr);

    start = startPacket(53);
    assert(receiver.start(start.data(), start.size(), 0xfffffff0));
    receiver.tick(119983);
    assert(receiver.state() == PlaybackState::Receiving);
    receiver.tick(119984);
    assert(receiver.error() == PlaybackError::Timeout);
    assert(receiver.audio() == nullptr);
    assert(!receiver.start(nullptr, 0, 0));
}
