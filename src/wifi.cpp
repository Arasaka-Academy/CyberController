#include "wifi.hpp"
#include "profile.hpp"
#include "cybercontroller/wifi_address.hpp"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <esp_system.h>
#include <cstring>

namespace cybercontroller {
namespace {
struct WifiCredentials { uint32_t version; char ssid[33], password[64], url[254], token[129]; };
const char page[] PROGMEM = R"HTML(<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>CyberController 配网</title><style>body{font:16px system-ui;max-width:420px;margin:32px auto;padding:16px;background:#121212;color:#eee}input,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0 18px;border-radius:6px}button{background:#80cbc4;border:0}small{color:#aaa}</style><h1>CyberController</h1><p>连接家中的 Wi-Fi 和 CyberOS。</p><form method="post" action="/configure"><label>Wi-Fi 名称<input name="ssid" maxlength="32" required></label><label>Wi-Fi 密码<input name="password" type="password" maxlength="63"></label><label>CyberOS 地址<input name="url" type="url" placeholder="http://192.168.1.100" maxlength="253" required></label><label>设备访问令牌<input name="token" type="password" minlength="32" maxlength="128" required></label><button>保存并连接</button></form><small>保存后设备会关闭热点。长按 TP1 两秒可重新配网。令牌由 CyberOS 管理员配置，不使用模型服务的 Token。</small></html>)HTML";
bool parseAddress(const String& url, String& host, uint16_t& port) {
    WifiAddress parsed;
    if (!parseWifiAddress(url.c_str(), parsed)) return false;
    host = parsed.host.c_str(); port = parsed.port;
    return true;
}
}
bool Wifi::begin() {
    input_ = xQueueCreate(8, sizeof(WriteEvent)); output_ = xQueueCreate(8, sizeof(Packet));
    setupKey_ = 10000000 + esp_random() % 90000000;
    return input_ && output_ && xTaskCreatePinnedToCore(task, "voice-network", 8192, this, 1, nullptr, 0) == pdPASS;
}
void Wifi::worker() {
    Preferences preferences; preferences.begin("cyber-wifi", false);
    WifiCredentials stored{};
    if (preferences.getBytesLength("config") == sizeof(stored)) preferences.getBytes("config", &stored, sizeof(stored));
    if (stored.version != 1) stored = {};
    stored.ssid[32] = stored.password[63] = stored.url[253] = stored.token[128] = 0;
    String ssid=stored.ssid, password=stored.password, url=stored.url, token=stored.token;
    String host, headers; uint16_t port=80;
    WebSocketsClient socket; WebServer portal(80); DNSServer dns;
    bool socketStarted=false, saving=false; uint32_t savedAt=0, setupStarted=0;
    const auto invalidate = [&] { authenticated_.store(false); generation_.fetch_add(1); };
    socket.onEvent([&](WStype_t type, uint8_t* bytes, size_t size) {
        if (type == WStype_DISCONNECTED) invalidate();
        else if (type == WStype_CONNECTED) { /* Wait for authenticated server READY. */ }
        else if (type == WStype_BIN && size) {
            if (bytes[0] == 0 && size == 2 && bytes[1] == 1 && !connected()) {
                generation_.fetch_add(1); authenticated_.store(true);
                const uint8_t negotiate[]{10,1,0,0,0,2}; incoming(WriteKind::Atvv, negotiate, sizeof(negotiate));
            } else if (connected() && bytes[0] == 4) incoming(WriteKind::Control, bytes+1, size-1);
            else if (connected() && bytes[0] == 5) incoming(WriteKind::Data, bytes+1, size-1);
            else { invalidate(); socket.disconnect(); }
        } else if (type == WStype_TEXT || type == WStype_FRAGMENT_BIN_START || type == WStype_FRAGMENT_TEXT_START) { invalidate(); socket.disconnect(); }
    });
    portal.on("/", HTTP_GET, [&] { portal.send_P(200,"text/html; charset=utf-8",page); });
    portal.on("/configure", HTTP_POST, [&] {
        if (!configuring_.load() || portal.client().localIP() != WiFi.softAPIP()) { portal.send(403,"text/plain","Setup requires device hotspot"); return; }
        const String nextSsid=portal.arg("ssid"), nextPassword=portal.arg("password"), nextUrl=portal.arg("url"), nextToken=portal.arg("token");
        String nextHost; uint16_t nextPort;
        bool validToken=nextToken.length()>=32 && nextToken.length()<=128;
        for (size_t i=0;i<nextToken.length();++i) if (!isAlphaNumeric(nextToken[i]) && nextToken[i]!='_' && nextToken[i]!='-') validToken=false;
        if (nextSsid.isEmpty() || nextSsid.length()>32 || nextPassword.length()>63 || (!nextPassword.isEmpty() && nextPassword.length()<8) || !parseAddress(nextUrl,nextHost,nextPort) || !validToken) { portal.send(400,"text/plain; charset=utf-8","请检查 Wi-Fi、http:// 主机地址和至少 32 位设备令牌"); return; }
        WifiCredentials next{}; next.version=1;
        nextSsid.toCharArray(next.ssid,sizeof(next.ssid)); nextPassword.toCharArray(next.password,sizeof(next.password));
        nextUrl.toCharArray(next.url,sizeof(next.url)); nextToken.toCharArray(next.token,sizeof(next.token));
        if (preferences.putBytes("config", &next, sizeof(next)) != sizeof(next)) { portal.send(503,"text/plain","Could not save configuration; please retry"); return; }
        ssid=nextSsid; password=nextPassword; url=nextUrl; token=nextToken;
        portal.send(200,"text/html; charset=utf-8","<meta name=viewport content='width=device-width'><h2>已保存，正在连接 CyberOS</h2><p>如连接失败，长按 TP1 两秒重新配网。</p>");
        saving=true; savedAt=millis();
    });
    portal.onNotFound([&] { portal.sendHeader("Location","http://192.168.4.1/"); portal.send(302,"text/plain",""); });
    WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.setSleep(false);
    if (ssid.isEmpty() || !parseAddress(url,host,port) || token.length()<32) configureRequested_.store(true);
    else WiFi.begin(ssid.c_str(),password.c_str());
    for (;;) {
        if (configureRequested_.exchange(false)) {
            invalidate(); if (socketStarted) socket.disconnect(); socketStarted=false;
            WiFi.disconnect(); WiFi.mode(WIFI_AP_STA);
            char key[9]; snprintf(key,sizeof(key),"%08lu",static_cast<unsigned long>(setupKey_));
            WiFi.softAP("CyberController-Setup",key,1,false,1);
            dns.start(53,"*",WiFi.softAPIP()); portal.begin(); configuring_.store(true); setupStarted=millis();
        }
        if (configuring_.load()) {
            dns.processNextRequest(); portal.handleClient();
            if ((saving && millis()-savedAt>=1000) || (!saving && !ssid.isEmpty() && millis()-setupStarted>=300000)) {
                portal.stop(); dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); configuring_.store(false); saving=false;
                WiFi.begin(ssid.c_str(),password.c_str());
            }
        } else if (WiFi.status()==WL_CONNECTED) {
            if (!socketStarted && parseAddress(url,host,port)) {
                headers="Authorization: Bearer "+token+"\r\nX-Cyber-Device: "+WiFi.macAddress()+"\r\nX-Cyber-Max-Samples: "+String(maxPlaybackSamples)+"\r\n";
                socket.begin(host,port,"/api/audio/device"); socket.setExtraHeaders(headers.c_str()); socket.setReconnectInterval(3000); socket.enableHeartbeat(15000,3000,2); socketStarted=true;
            }
            if (socketStarted) socket.loop();
        } else if (connected()) invalidate();
        if (disconnectRequested_.exchange(false)) { invalidate(); socket.disconnect(); }
        Packet packet{};
        for (unsigned i=0;i<4 && xQueueReceive(output_,&packet,0)==pdTRUE;++i) {
            if (packet.session != generation_.load() || !connected()) continue;
            if (!socket.sendBIN(packet.bytes.data(),packet.size)) { overflow_.store(true); invalidate(); socket.disconnect(); break; }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
}
