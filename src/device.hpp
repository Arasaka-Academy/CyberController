#pragma once

#include <cstddef>
#include <cstdint>

namespace cybercontroller {

enum class State { Disconnected, Ready, Recording, Waiting, Receiving, Playing, Error };

class Device {
public:
    virtual ~Device() = default;
    virtual bool begin() = 0;
    virtual void update() = 0;
    virtual bool talkPressed() const = 0;
    virtual bool pairRequested() const = 0;
    virtual void display(State state, const char* detail) = 0;
    virtual void prepareMicrophone() = 0;
    virtual bool microphoneReady() const = 0;
    virtual bool startCapture() = 0;
    virtual bool captureFrame(int16_t* samples) = 0;
    virtual bool captureHealthy() const = 0;
    virtual void stopCapture() = 0;
    virtual bool play(const uint8_t* encoded, uint32_t samples, uint8_t volume) = 0;
    virtual bool playbackFinished() = 0;
    virtual bool playbackHealthy() const { return true; }
    virtual void stopPlayback() = 0;
};

}
