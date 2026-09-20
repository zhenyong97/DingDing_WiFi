#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>

#define PIN_LED 8
#define PIN_BUTTON 9

#define LONG_PRESS_MS 1000                            // 长按阈值：清除已保存配置
#define CLICK_MAX_MS 800                              // 单次短按的最长时长
#define CLICK_DEBOUNCE_MS 60                          // 防抖：两次短按的最小间隔
#define CLICK_WINDOW_MS 1500                          // 连击窗口（从第一次按下算起）
#define CONFIG_CLICK_COUNT 3                          // 连按几次进入配置模式
#define CONFIG_MODE_TIMEOUT_MS (5UL * 60UL * 1000UL)  // 配置模式最长保持时间，超时自动回到已保存配置

Preferences preferences;
DNSServer dns;
AsyncWebServer server(80);

String ssid;
String password;
String macAddress;
uint8_t macArray[6];

bool enableDNS = true;

bool lastButtonState = HIGH;
bool currentButtonState = HIGH;
uint32_t pressedTime = 0;

bool configMode = false;
uint32_t configModeStartMs = 0;
uint8_t clickCount = 0;
uint32_t firstClickTime = 0;
uint32_t lastClickTime = 0;

const char *TAG = "MJOLNIR";

bool convertMacStringToArray() {
    if (macAddress.length() == 17) {
        const char *macStr = macAddress.c_str();
        for (unsigned char &i: macArray) {
            sscanf(macStr, "%02x", &i);
            macStr += 3;
        }
        ESP_LOGD(TAG, "Conversions successfully, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 macArray[0], macArray[1],
                 macArray[2], macArray[3],
                 macArray[4], macArray[5]);
        return true;
    } else {
        ESP_LOGD(TAG, "Invalid MAC address");
        return false;
    }
}

// 把 SSID 里可能出现的引号 / 反斜杠 / 控制字符转义成合法 JSON 字符串。
// SSID 里塞特殊字符是允许的，不转义会让前端 JSON.parse 直接炸掉。
String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        uint8_t c = (uint8_t) in[i];
        switch (c) {
            case '"': out += F("\\\""); break;
            case '\\': out += F("\\\\"); break;
            case '\b': out += F("\\b"); break;
            case '\f': out += F("\\f"); break;
            case '\n': out += F("\\n"); break;
            case '\r': out += F("\\r"); break;
            case '\t': out += F("\\t"); break;
            default:
                if (c < 0x20) {
                    // 其余控制字符走 \u00XX；>= 0x80 的字节是 UTF-8 多字节，原样输出
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
    return out;
}

bool createAP() {
    if (!convertMacStringToArray()) {
        digitalWrite(PIN_LED, HIGH);
        return false;
    }

    WiFi.enableAP(true);
    esp_err_t set_mac_err = esp_wifi_set_mac(WIFI_IF_AP, macArray);
    ESP_LOGD(TAG, "esp_wifi_set_mac: %s", esp_err_to_name(set_mac_err));
    if (set_mac_err == ESP_OK) {
        server.end();
        WiFi.softAPdisconnect();
        if (WiFi.softAP(ssid, password)) {
            esp_err_t set_power_err = esp_wifi_set_max_tx_power(34);
            ESP_LOGD(TAG, "esp_wifi_set_max_tx_power: %s", esp_err_to_name(set_power_err));
            digitalWrite(PIN_LED, LOW);
            configMode = false;  // 自定义热点建好了，说明已回到正常工作状态
            ESP_LOGD(TAG, "AP created successfully");
            ESP_LOGD(TAG, "#     SSID: %s", ssid.c_str());
            ESP_LOGD(TAG, "# Password: %s", password.c_str());
            ESP_LOGD(TAG, "#      MAC: %s", macAddress.c_str());
            server.begin();
            enableDNS = false;
            return true;
        } else {
            digitalWrite(PIN_LED, HIGH);
            return false;
        }
    } else {
        digitalWrite(PIN_LED, HIGH);
        ESP_LOGD(TAG, "Failed to set MAC address");
        return false;
    }
}

// 连按 BOOT 进入配置模式：临时广播默认热点，**不清除**已保存的配置。
// 断电重启、或配置模式超时，都会自动回到原来保存的那个热点。
void enterConfigMode() {
    ESP_LOGD(TAG, "Entering config mode (saved credentials are kept)");
    server.end();
    WiFi.softAPdisconnect();
    digitalWrite(PIN_LED, HIGH);
    if (WiFi.softAP("WIFI MANAGER")) {
        enableDNS = true;  // 恢复强制门户，手机连上会自动弹出配置页
        server.begin();
        configMode = true;
        configModeStartMs = millis();
        ESP_LOGD(TAG, "# Config AP: WIFI MANAGER -> http://192.168.4.1");
    } else {
        ESP_LOGD(TAG, "Failed to create config AP");
    }
}

void initWebServer() {
    if (!LittleFS.begin()) {
        ESP_LOGD(TAG, "Failed to initialize LittleFS");
        return;
    }

    server.serveStatic("/", LittleFS, "/");

    server.on("/", HTTP_GET,
              [](AsyncWebServerRequest *request) { request->send(LittleFS, "/index.html", "text/html"); });

    server.onNotFound([](AsyncWebServerRequest *request) { request->redirect("/"); });

    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
        String _ssid_str;
        String _password_str;
        String _mac_str;

        for (int i = 0; i < request->params(); i++) {
            AsyncWebParameter *p = request->getParam(i);

            if (p->name() == "wifi_ssid") {
                _ssid_str = p->value();
            }

            if (p->name() == "wiif_password") {
                _password_str = p->value();
            }

            if (p->name() == "wifi_mac") {
                _mac_str = p->value();
            }
        }

        if (_ssid_str.length() < 1 || _ssid_str.length() > 63) {
            ESP_LOGD(TAG, "Request send error");
            request->send(200, "text/plain", "SSID format error");
        } else if (_password_str.length() < 8 && _password_str.length() != 0) {
            ESP_LOGD(TAG, "Request send error");
            request->send(200, "text/plain", "Password format error");
        } else if (_mac_str.length() != 17) {
            ESP_LOGD(TAG, "Request send error");
            request->send(200, "text/plain", "MAC length error");
        } else {
            ssid = _ssid_str;
            password = _password_str;
            macAddress = _mac_str;
            if (!createAP()) {
                ESP_LOGD(TAG, "Request send result");
                request->send(200, "text/plain", "创建热点失败，请检查MAC地址是否正确");
                // 兜底：createAP() 内部可能已经把热点断开了。此时若没有热点就重新广播
                // 默认热点，保证设备仍然连得上、能继续改配置，而不是失联到只能靠按键。
                if ((uint32_t) WiFi.softAPIP() == 0 && WiFi.softAP("WIFI MANAGER")) {
                    enableDNS = true;
                    server.begin();
                }
            } else {
                preferences.begin("wifi_config", false);
                preferences.putString("ssid", ssid);
                preferences.putString("password", password);
                preferences.putString("mac", macAddress);
                preferences.end();
                // server.end();
                // server.begin();
            }
        }
    });

    // ---- 扫描附近 WiFi，让现场的人直接挑出要伪装的热点 ----
    // 用法：GET /scan?start=1 触发异步扫描 → 反复 GET /scan 轮询，直到 status 变成 done。
    // 为什么要异步：一次全信道扫描要 1.5~4 秒，同步跑会把 async_tcp 任务堵死，手机端 fetch 直接超时。
    // 限制：ESP32-C3 只有 2.4GHz 射频，扫不到 5GHz 网络。
    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("start")) {
            // 阻塞式 scanNetworks(async=false) 会卡住 1.5~4 秒，这里必须用异步
            int16_t started = WiFi.scanNetworks(true, true);  // async, show_hidden
            ESP_LOGD(TAG, "scanNetworks(async): %d", started);
            request->send(200, "application/json", "{\"status\":\"scanning\"}");
            return;
        }

        int16_t count = WiFi.scanComplete();
        if (count == WIFI_SCAN_RUNNING) {
            request->send(200, "application/json", "{\"status\":\"scanning\"}");
            return;
        }
        if (count < 0) {  // WIFI_SCAN_FAILED：还没开始扫，或扫描失败
            request->send(200, "application/json", "{\"status\":\"idle\"}");
            return;
        }

        String json = "{\"status\":\"done\",\"count\":" + String(count) + ",\"networks\":[";
        for (int16_t i = 0; i < count; i++) {
            if (i > 0) json += ',';
            json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
            json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
            json += ",\"rssi\":" + String(WiFi.RSSI(i));
            json += ",\"channel\":" + String(WiFi.channel(i));
            json += ",\"secure\":";
            json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
            json += "}";
        }
        json += "]}";

        WiFi.scanDelete();  // 释放 _scanResult，否则每次扫描都会漏一块堆内存
        ESP_LOGD(TAG, "scan done, %d networks", count);
        request->send(200, "application/json", json);
    });

    server.begin();
}

void setup() {
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    digitalWrite(PIN_LED, HIGH);

    if (!preferences.begin("wifi_config", false)) {
        ESP_LOGD(TAG, "Failed to initialize preferences");
    }

    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    macAddress = preferences.getString("mac", "");
    preferences.end();

    WiFi.mode(WIFI_AP);

    if (ssid.length() > 0 && macAddress.length() == 17) {
        if (!createAP()) {
            ESP_LOGD(TAG, "WiFi SSID or MAC error. Creating default AP");
            WiFi.softAP("WIFI MANAGER");
        }
    } else {
        ESP_LOGD(TAG, "No saved WiFi credentials. Creating default AP");
        WiFi.softAP("WIFI MANAGER");
    }

    initWebServer();
    dns.start(53, "*", WiFi.softAPIP());
}

void loop() {
    currentButtonState = digitalRead(PIN_BUTTON);

    if (lastButtonState == HIGH && currentButtonState == LOW) {
        // 按下
        pressedTime = millis();
        if (clickCount == 0) {
            firstClickTime = pressedTime;  // 连击窗口从第一次按下开始算
        }
    } else if (lastButtonState == LOW && currentButtonState == HIGH) {
        // 松开
        uint32_t heldMs = millis() - pressedTime;

        if (heldMs > LONG_PRESS_MS) {
            // 长按：清除已保存的配置并重启（行为与原版一致）
            ESP_LOGD(TAG, "Button long pressed, Clean WiFi credentials");
            preferences.begin("wifi_config", false);
            preferences.clear();
            preferences.end();
            delay(1000);
            ESP.restart();
        } else if (heldMs <= CLICK_MAX_MS && (millis() - lastClickTime) > CLICK_DEBOUNCE_MS) {
            // 短按：计数，连按 CONFIG_CLICK_COUNT 次进入配置模式
            lastClickTime = millis();
            clickCount++;
            ESP_LOGD(TAG, "Button click %u/%u", (unsigned) clickCount, (unsigned) CONFIG_CLICK_COUNT);
            if (clickCount >= CONFIG_CLICK_COUNT) {
                clickCount = 0;
                enterConfigMode();
            }
        }
    }
    lastButtonState = currentButtonState;

    // 连击窗口超时，把计数清零
    if (clickCount > 0 && (millis() - firstClickTime) > CLICK_WINDOW_MS) {
        clickCount = 0;
    }

    // 配置模式超时：自动重启回到已保存的配置，避免一直停在配置热点上
    if (configMode && (millis() - configModeStartMs) > CONFIG_MODE_TIMEOUT_MS) {
        ESP_LOGD(TAG, "Config mode timeout, restarting with saved credentials");
        delay(100);
        ESP.restart();
    }

    if (enableDNS) {
        dns.processNextRequest();
    }
    delay(10);  // 10ms 轮询，保证短按/连按不会被漏采
}