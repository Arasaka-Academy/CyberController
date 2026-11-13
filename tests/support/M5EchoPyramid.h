#pragma once
#include "Arduino.h"
#include <array>
namespace pyramidHardware {
inline bool touch = false, chips = true, muted = true, amplifierAwake = false, txRunning = false;
inline unsigned volume = 0, pending = 0;
inline size_t writeLimit = 960, readLimit = 960;
inline bool writeFailure = false, readFailure = false;
inline std::vector<int16_t> written;
inline std::vector<uint8_t> incoming;
inline void (*onOverflow)() = nullptr;
}
struct TwoWire {
    void end() {} void setTimeOut(int) {} uint8_t address=0; void beginTransmission(uint8_t value) { address=value; }
    int endTransmission() { return pyramidHardware::chips && (address != 0x5b || pyramidHardware::amplifierAwake) ? 0 : 1; }
};
inline TwoWire Wire1;
struct I2CBus { bool begin(TwoWire*, int, int) { return true; } };
struct SI5351 { explicit SI5351(I2CBus&) {} bool begin() { return true; } bool setMCLK(unsigned) { return true; } };
inline constexpr int ES7210_INPUT_MIC1 = 1, ES7210_INPUT_MIC3 = 4;
struct ES7210 { explicit ES7210(I2CBus&) {} bool begin(unsigned, unsigned, int) { return true; } };
struct ES8311 {
    explicit ES8311(I2CBus&) {} bool begin(unsigned, unsigned) { return true; }
    bool mute(bool value) { pyramidHardware::muted = value; return true; }
    bool setVolume(unsigned value) { pyramidHardware::volume = value; return true; }
};
struct AW87559 { explicit AW87559(I2CBus&) {} bool begin() { return true; } };
struct STM32Ctrl {
    explicit STM32Ctrl(I2CBus&) {} bool begin() { pyramidHardware::amplifierAwake=true; return true; }
    bool getUSB5V(uint16_t& value) { value=5000; return true; }
    bool isPressed(int) { return pyramidHardware::touch; }
    void setBrightness(int, int) {} void setAllRGB(int, int, int, int) {}
};
using i2s_chan_handle_t = int*;
struct i2s_event_data_t {};
struct i2s_event_callbacks_t { bool (*on_sent)(i2s_chan_handle_t, i2s_event_data_t*, void*) = nullptr; bool (*on_recv_q_ovf)(i2s_chan_handle_t, i2s_event_data_t*, void*) = nullptr; };
inline i2s_event_callbacks_t pyramidCallbacks;
inline void* pyramidContext = nullptr;
inline i2s_event_callbacks_t pyramidTxCallbacks;
inline void* pyramidTxContext = nullptr;
inline constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = 1;
inline constexpr int I2S_NUM_0=0, I2S_ROLE_MASTER=0, I2S_GPIO_UNUSED=-1;
inline constexpr int I2S_DATA_BIT_WIDTH_16BIT=16, I2S_SLOT_MODE_STEREO=2;
inline constexpr int GPIO_NUM_6=6, GPIO_NUM_8=8, GPIO_NUM_7=7, GPIO_NUM_5=5;
struct Channels { int dma_desc_num=0, dma_frame_num=0; bool auto_clear=false; };
using i2s_chan_config_t = Channels;
inline Channels I2S_CHANNEL_DEFAULT_CONFIG(int, int) { return {}; }
struct i2s_std_config_t { int clk_cfg=0, slot_cfg=0; struct { int mclk=0,bclk=0,ws=0,dout=0,din=0; } gpio_cfg; };
inline int I2S_STD_CLK_DEFAULT_CONFIG(int value) { return value; }
inline int I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(int, int) { return 0; }
inline i2s_chan_handle_t testTx = nullptr;
inline int i2s_new_channel(Channels*, i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
    static int a, b; *tx=&a; *rx=&b; testTx=*tx; return ESP_OK;
}
inline int i2s_channel_register_event_callback(i2s_chan_handle_t, i2s_event_callbacks_t* cb, void* ctx) {
    if (cb->on_sent) { pyramidTxCallbacks=*cb; pyramidTxContext=ctx; }
    else { pyramidCallbacks=*cb; pyramidContext=ctx; }
    return ESP_OK;
}
inline int i2s_channel_init_std_mode(i2s_chan_handle_t, i2s_std_config_t*) { return ESP_OK; }
inline int i2s_channel_preload_data(i2s_chan_handle_t, const void*, size_t size, size_t* loaded) { *loaded=size; return ESP_OK; }
inline int i2s_channel_enable(i2s_chan_handle_t channel) { if (channel == testTx) pyramidHardware::txRunning=true; return ESP_OK; }
inline int i2s_channel_disable(i2s_chan_handle_t channel) { if (channel == testTx) pyramidHardware::txRunning=false; return ESP_OK; }
inline int i2s_channel_read(i2s_chan_handle_t, void* output, size_t size, size_t* count, int) {
    using namespace pyramidHardware;
    *count = std::min({size, incoming.size(), readLimit});
    std::memcpy(output, incoming.data(), *count);
    incoming.erase(incoming.begin(), incoming.begin()+*count);
    return readFailure ? 2 : (*count == size ? ESP_OK : ESP_ERR_TIMEOUT);
}
inline int i2s_channel_write(i2s_chan_handle_t, const void* input, size_t size, size_t* count, int) {
    using namespace pyramidHardware;
    *count = writeFailure ? 0 : std::min(size, writeLimit);
    const auto* samples = static_cast<const int16_t*>(input);
    written.insert(written.end(), samples, samples + *count / 2);
    return writeFailure ? 2 : (*count == size ? ESP_OK : ESP_ERR_TIMEOUT);
}
