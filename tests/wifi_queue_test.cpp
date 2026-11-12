#include <cassert>
#include <iostream>
#include "../src/wifi_queue.cpp"
using namespace cybercontroller;
namespace cybercontroller {
// Replace only socket startup: exercise the actual firmware queue and generation logic.
bool Wifi::begin() {
    if (!input_) input_ = xQueueCreate(8, sizeof(WriteEvent));
    if (!output_) output_ = xQueueCreate(8, sizeof(Packet));
    generation_.fetch_add(1); authenticated_.store(true);
    const uint8_t ready[]{10,1,0,0,0,2}; incoming(WriteKind::Atvv,ready,sizeof(ready));
    return true;
}
}
static Wifi* instance;
int main() {
    Wifi wifi; instance=&wifi; assert(wifi.begin());
    WriteEvent event{};
    assert(!wifi.pop(event)); // Do not consume a new session's handshake prematurely.
    assert(wifi.sessionChanged()); assert(wifi.pop(event));
    assert(event.kind==WriteKind::Atvv && event.size==6 && event.bytes[0]==10);
    assert(!wifi.sessionChanged());
    hardware::onQueuePeek=[] { instance->begin(); };
    assert(!wifi.pop(event)); // READY races the main loop after its generation check.
    assert(wifi.sessionChanged()); assert(wifi.pop(event) && event.bytes[0]==10);
    const uint8_t pressed[]{8}; assert(wifi.sendControl(pressed,1));
    assert(wifi.pop(event) && event.size==2 && event.bytes[0]==12);
    const uint8_t sample[]{0x12};
    for (unsigned i=0;i<7;++i) assert(wifi.sendAudio(sample,1));
    assert(!wifi.sendAudio(sample,1)); assert(wifi.overflowed()); assert(!wifi.overflowed());
    wifi.disconnect(); assert(!wifi.connected()); assert(!wifi.sendAudio(sample,1));
    assert(wifi.begin()); assert(!wifi.pop(event)); assert(wifi.sessionChanged()); assert(wifi.pop(event));
    assert(!wifi.pop(event));
    std::cout << "PASS Wi-Fi actual queues: handshake race, generation, bounded backpressure and disconnect\n";
}
