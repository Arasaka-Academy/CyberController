#pragma once
#include <cstdint>
#include <string>
namespace cybercontroller {
struct WifiAddress { std::string host; uint16_t port = 80; };
inline bool parseWifiAddress(const std::string& url, WifiAddress& parsed) {
    if (url.compare(0, 7, "http://") != 0) return false;
    auto authority = url.substr(7);
    if (!authority.empty() && authority.back() == '/') authority.pop_back();
    if (authority.empty() || authority.size() > 253) return false;
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty()) return false;
    for (const char c : host) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    uint32_t port = 80;
    if (colon != std::string::npos) {
        const auto number = authority.substr(colon + 1);
        if (number.empty() || number.size() > 5) return false;
        port = 0;
        for (const char c : number) { if (c < '0' || c > '9') return false; port = port * 10 + c - '0'; }
        if (!port || port > 65535) return false;
    }
    parsed = {host, static_cast<uint16_t>(port)};
    return true;
}
}
