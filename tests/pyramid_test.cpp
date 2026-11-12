#include <iostream>
#include <stdexcept>
#include "../src/devices/atoms3_pyramid.cpp"
#include "cybercontroller/playback.hpp"
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
using namespace cybercontroller;
int main() {
    AtomS3Pyramid device;
    CHECK(device.begin());
    hardware::now += 1100;
    CHECK(device.microphoneReady());
    pyramidHardware::touch = true; device.update();
    hardware::now += 1999; device.update(); CHECK(!device.pairRequested());
    hardware::now += 20; device.update(); CHECK(device.pairRequested());
    hardware::now += 2500; device.update(); CHECK(!device.pairRequested());
    pyramidHardware::touch = false; hardware::now += 20; device.update();
    pyramidHardware::touch = true; hardware::now += 20; device.update();
    hardware::now += 2000; device.update(); CHECK(device.pairRequested());
    pyramidHardware::touch = false;
    CHECK(device.startCapture());
    std::array<int16_t, 480> input{};
    for (unsigned i=0; i<240; ++i) { input[2*i]=i; input[2*i+1]=-100; }
    const auto* raw = reinterpret_cast<const uint8_t*>(input.data());
    pyramidHardware::incoming.assign(raw, raw + sizeof(input));
    pyramidHardware::readLimit = 480;
    std::array<int16_t, 240> frame{};
    CHECK(!device.captureFrame(frame.data())); CHECK(device.captureFrame(frame.data()));
    for (unsigned i=0; i<240; ++i) CHECK(frame[i] == static_cast<int16_t>(i));
    pyramidCallbacks.on_recv_q_ovf(nullptr, nullptr, pyramidContext);
    CHECK(!device.captureHealthy()); device.stopCapture();
    CHECK(device.startCapture()); CHECK(device.captureHealthy());
    pyramidHardware::readFailure = true; device.captureFrame(frame.data()); CHECK(!device.captureHealthy());
    pyramidHardware::readFailure = false; device.stopCapture();
    const uint8_t golden[]{0x12,0x70,0xff};
    CHECK(!device.play(golden, 131073, 100));
    CHECK(device.play(golden, 5, 100)); CHECK(pyramidHardware::volume == 25);
    pyramidHardware::writeLimit = 100; // Partial writes must not re-decode or skip samples.
    for (unsigned i=0; i<100; ++i) device.update();
    CHECK(!device.playbackFinished());
    hardware::now += 1000; CHECK(!device.playbackFinished()); // No DMA progress: never complete by time alone.
    for (unsigned i=0; i<4; ++i) pyramidTxCallbacks.on_sent(nullptr, nullptr, pyramidTxContext);
    CHECK(!device.playbackFinished());
    pyramidTxCallbacks.on_sent(nullptr, nullptr, pyramidTxContext);
    CHECK(device.playbackFinished()); CHECK(pyramidHardware::muted);
    CHECK(pyramidHardware::written.size() == (5+1600)*2);
    const int16_t reference[]{1,4,15,17,-8};
    for (unsigned i=0; i<5; ++i) CHECK(pyramidHardware::written[2*i] == reference[i] && pyramidHardware::written[2*i+1] == reference[i]);
    for (unsigned i=10; i<pyramidHardware::written.size(); ++i) CHECK(pyramidHardware::written[i] == 0);
    CHECK(device.play(golden, 5, 10)); device.update(); device.stopPlayback();
    const auto count = pyramidHardware::written.size(); device.update(); CHECK(count == pyramidHardware::written.size());
    CHECK(!device.playbackFinished());
    CHECK(device.play(golden, 5, 10)); pyramidHardware::writeFailure = true; device.update();
    CHECK(!device.playbackHealthy()); hardware::now += 1000; CHECK(!device.playbackFinished()); device.stopPlayback();
    pyramidHardware::chips = false; AtomS3Pyramid missing; CHECK(!missing.begin());
    std::cout << "PASS Pyramid actual adapter: touch, capture, overflow, partial writes, drain, cancel, driver errors\n";
}
