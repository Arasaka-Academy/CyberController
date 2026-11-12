#pragma once
#include <cstdint>
namespace cybercontroller {
#ifdef CYBERCONTROLLER_ATOMS3_PYRAMID
inline constexpr const char* deviceName = "CyberController AtomS3";
inline constexpr const char* pairingHint = "TP1 2s: Wi-Fi setup";
inline constexpr uint32_t maxPlaybackSamples = 131072;
#else
inline constexpr const char* deviceName = "CyberController StickS3";
inline constexpr const char* pairingHint = "Hold B 2s to pair";
inline constexpr uint32_t maxPlaybackSamples = 16000 * 130;
#endif
}
