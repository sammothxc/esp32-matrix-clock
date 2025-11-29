#include <Arduino.h>
#include <AsyncTCP.h>
#include <EEPROM.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>

#define TZ -7 // Mountain Time Zone (UTC-7)

// pins
#define MAX_DEVICES 4
#define DIN_PIN 13
#define CS_PIN   12
#define CLK_PIN 11
#define LED_PIN 48

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xA55A1234

struct EEPROMstorage {
    uint32_t magic;
    char wifi_ssid[50];
    char wifi_password[50];
    uint8_t use12HourFormat;
    uint8_t dpEnabled;
    uint8_t colonBlinkEnabled;
    uint8_t colonBlinkSlow;
};

const long gmtOffset_sec = TZ * 3600; // MST
const int daylightOffset_sec = 3600; // MDT
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* hostname = "matrix-ntp-clock";
const char* pass = "clock-pass";
const unsigned long NTP_INTERVAL = 3600000UL;
bool colonOn = true;
bool isPM = true;
bool reboot = false;
bool wifiConnected = false;
bool otaInProgress = false;
unsigned long lastColonChange = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastNTPSync = 0;
unsigned long rebootAt = 0;
unsigned long otaProgressMillis = 0;
IPAddress AP_IP(10,1,1,1);
IPAddress AP_subnet(255,255,255,0);
AsyncWebServer server(80);
EEPROMstorage config;
MD_Parola matrix = MD_Parola(HARDWARE_TYPE, DIN_PIN, CLK_PIN,CS_PIN, MAX_DEVICES);

// bits 0..6 = a..g
const uint8_t digits[10] = {

};

void readConf();
void writeConf();
bool connectToWiFi();
void setUpAccessPoint();
void handleWebServerRequest(AsyncWebServerRequest *request);
void startMDNS();
void setUpWebServer();
void requestReboot();
void rebootCheck();
void NTPsync();
void updateTime();
void updateColon();
void display();

void setup() {
    Serial.begin(115200);
    Serial.println("Starting up...");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    matrix.begin();
    matrix.setIntensity(8);
    matrix.displayClear();
    EEPROM.begin(EEPROM_SIZE);
    readConf();
    if (!connectToWiFi()){ setUpAccessPoint(); }
    startMDNS();
    setUpWebServer();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    NTPsync();
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    matrix.setTextAlignment(PA_LEFT);
    matrix.print("Left");
    delay(2000);

    matrix.setTextAlignment(PA_CENTER);
    matrix.print("Center");
    delay(2000);

    matrix.setTextAlignment(PA_RIGHT);
    matrix.print("Right");
    delay(2000);

    matrix.setTextAlignment(PA_CENTER);
    matrix.setInvert(true);
    matrix.print("Invert");
    delay(2000);

    matrix.setInvert(false);
    matrix.print(1234);
    delay(2000);

    rebootCheck();
    NTPsync();
    updateTime();
    updateColon();
    display();
}

void readConf() {
    for (size_t i = 0; i < sizeof(EEPROMstorage); i++) {
        ((uint8_t*)&config)[i] = EEPROM.read(i);
    }
    if (config.magic != EEPROM_MAGIC) {
        Serial.println("EEPROM not initialized or corrupt. Loading defaults...");
        //TODO errorCtrl();
        memset(&config, 0, sizeof(EEPROMstorage));
        config.magic = EEPROM_MAGIC;
        strcpy(config.wifi_ssid, "your-ssid");
        strcpy(config.wifi_password, "your-password");
        config.use12HourFormat = 1;
        config.dpEnabled = 1;
        config.colonBlinkEnabled = 1;
        config.colonBlinkSlow = 1;
        writeConf();
    }
}

void writeConf() {
    for (size_t i = 0; i < sizeof(EEPROMstorage); i++) {
        EEPROM.write(i, ((uint8_t*)&config)[i]);
    }
    EEPROM.commit();
}

bool connectToWiFi() {
    Serial.printf("Connecting to '%s'\n", config.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
        wifiConnected = true;
        return true;
    } 
    Serial.println("Connection failed");
    //TODO errorCtrl();
    return false;
}

void setUpAccessPoint() {
    Serial.println("Setting up AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_subnet);
    WiFi.softAP(hostname, pass);
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void handleWebServerRequest(AsyncWebServerRequest *request) {
    bool save = false;
    if (request->method() == HTTP_POST) {
        if (request->hasParam("ssid",true) && request->hasParam("password",true)) {
            String s = request->getParam("ssid",true)->value();
            String p = request->getParam("password",true)->value();
            s.toCharArray(config.wifi_ssid,sizeof(config.wifi_ssid));
            p.toCharArray(config.wifi_password,sizeof(config.wifi_password));
        }
        if (request->hasParam("tf", true)) {
            String tf = request->getParam("tf",true)->value();
            config.use12HourFormat = (tf == "12");
        }
        if (request->hasParam("cbi", true)) {
            String cbi = request->getParam("cbi",true)->value();
            config.colonBlinkSlow = (cbi == "1000");
        }
        if (request->hasParam("dp", true)) {
            config.dpEnabled = 1;
        } else {
            config.dpEnabled = 0;
        }
        if (request->hasParam("colon", true)) {
            config.colonBlinkEnabled = 1;
        } else {
            config.colonBlinkEnabled = 0;
        }
        writeConf();
        save = true;
    }
    String msg;
    msg = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    if (save) {
        msg += "<meta http-equiv='refresh' content='1;url=/' />";
        msg += "<title>Saved! Rebooting...</title></head><body>";
        msg += "<div style='font-family:sans-serif;text-align:center;margin-top:50px;'>";
        msg += "<h1 style='color:green;'>Configuration Saved!</h1>";
        msg += "<p>Rebooting ESP...</p></div></body></html>";
    } else {
        msg += "<title>NTP Clock Configuration</title><style>";
        msg += "body{font-family:Arial,sans-serif;background:#f4f4f4;color:#333;margin:0;padding:0;}";
        msg += ".container{max-width:500px;margin:50px auto;padding:30px;background:#fff;border-radius:10px;box-shadow:0 0 15px rgba(0,0,0,0.1);}";
        msg += "h1{text-align:center;color:#444;;margin-bottom:30px;}";
        msg += "form div{margin-top:20px;margin-bottom:5px;}";
        msg += "label{display:block;margin-bottom:5px;}";
        msg += "input[type=text], input[type=password], select{width:100%;padding:8px;border:1px solid #ccc;border-radius:5px;}";
        msg += "input[type=submit]{background:#007BFF;color:#fff;border:none;padding:12px 25px;border-radius:5px;cursor:pointer;font-size:16px;display:block;margin:20px auto;}";
        msg += "input[type=submit]:hover{background:#0056b3;}";
        msg += "hr{margin:30px 0;}";
        msg += "</style></head><body><div class='container'>";
        msg += "<h1>NTP Clock Configuration</h1>";
        msg += "<form action='/restart' method='POST' style='text-align:center;'><input type='submit' value='Reboot Clock'/></form>";
        msg += "<form action='/' method='POST'>";
        msg += "<div><label>WiFi SSID:</label></div><input type='text' name='ssid' value='"+String(config.wifi_ssid)+"'/>";
        msg += "<div><label>Password:</label></div><input type='password' name='password' value='"+String(config.wifi_password)+"'/>";
        msg += "<div><label>Time Format:</label></div><select name='tf'>";
        msg += config.use12HourFormat ? "<option value='24'>24-hour</option><option value='12' selected>12-hour</option></select>" :
                "<option value='24' selected>24-hour</option><option value='12'>12-hour</option></select>";
        msg += "<div><label>PM Indicator (Decimal Point on 4th digit):</label></div>";
        msg += "<input type='checkbox' name='dp' value='1' ";
        msg += (config.dpEnabled ? "checked" : "");
        msg += "> Enable</label>";
        msg += "<div><label>Colon (Decimal Point on 2nd digit):</label></div>";
        msg += "<label><input type='checkbox' name='colon' value='1' ";
        msg += (config.colonBlinkEnabled ? "checked" : "");
        msg += "> Enable</label>";
        msg += "<div><label>Colon Blink Interval:</label></div><select name='cbi'>";
        msg += config.colonBlinkSlow ? "<option value='500'>0.5s</option><option value='1000' selected>1s</option>" :
                "<option value='500' selected>0.5s</option><option value='1000'>1s</option></select>";
        msg += "<input type='submit' value='Save Configuration + Reboot'/></form>";
        msg += "</div></body></html>";
    }
    request->send(200,"text/html",msg);
    if (save) {
        requestReboot();
    }
}

void startMDNS() {
    if (!MDNS.begin(hostname)) {
        Serial.println("Error setting up MDNS responder!");
        //TODO errorCtrl();
        return;
    }
    Serial.println("mDNS responder started");
}

void setUpWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ handleWebServerRequest(request); });
    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request){ handleWebServerRequest(request); });
    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta http-equiv='refresh' content='1;url=/' />";
        html += "<title>Restarting...</title></head><body>";
        html += "<h1>Restarting...</h1>";
        html += "</body></html>";
        request->send(200, "text/html", html);
        requestReboot();
    });
    ElegantOTA.setAuth("admin", pass);
    ElegantOTA.begin(&server);
    ElegantOTA.onStart([]() {
        Serial.println("OTA update process started.");
        otaInProgress = true;
    });
    ElegantOTA.onProgress([](size_t current, size_t final) {
        if (millis() - otaProgressMillis > 1000) {
            otaProgressMillis = millis();
            Serial.printf("Progress: %u%%\n", (current * 100) / final);
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        }
        Serial.printf("Progress: %u%%\n", (current * 100) / final);
    });
    ElegantOTA.onEnd([](bool success) {
        otaInProgress = false;
        digitalWrite(LED_PIN, LOW);
        if (success) {
            Serial.println("OTA update completed successfully! Restarting...");
            delay(500);
            requestReboot();
        } else {
            Serial.println("OTA update failed :(");
            //TODO errorCtrl();
        }
    });
    Serial.println("ElegantOTA server started");
    server.begin();
    Serial.println("Web server started");
}

void requestReboot() {
    reboot = true;
    rebootAt = millis();
}

void rebootCheck() {
    if (reboot && millis() >= rebootAt + 2000) {
        server.end();
        delay(500);
        ESP.restart();
    }
}

void NTPsync() {
    if (millis() - lastNTPSync >= NTP_INTERVAL) {
        lastNTPSync = millis();
        unsigned long startAttempt = millis();
        struct tm timeinfo;
        Serial.println("NTP sync requested...");
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi not connected, reconnecting...");
            WiFi.reconnect();
            delay(500);
            return;
        }
        while (!getLocalTime(&timeinfo)) {
            if (millis() - startAttempt > 3000) {
                Serial.println("NTP sync failed.");
                //TODO errorCtrl();
                return;
            }
            delay(100);
        }
        Serial.println("NTP sync complete!");
    }
}

void updateTime() {
    if (millis() - lastTimeUpdate >= 1000) {
        lastTimeUpdate = millis();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            int hour = timeinfo.tm_hour;
            int minute = timeinfo.tm_min;

            if (config.use12HourFormat) {
                isPM = (hour >= 12);
                hour = hour % 12;
                if (hour == 0) hour = 12;  // midnight/noon correction
            }
            //display
        }
    }
}

void updateColon() {
    if (!config.colonBlinkEnabled) {
        colonOn = true;
        return;
    }
    if (millis() - lastColonChange >= (config.colonBlinkSlow ? 1000 : 500)) {
        colonOn = !colonOn;
        lastColonChange = millis();
    }
}

void display() {
    if (otaInProgress) return;
    // display update code here
}
