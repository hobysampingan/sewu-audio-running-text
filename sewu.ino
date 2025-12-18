/*
 * SEWU AUDIO - Running Text Display System
 * NodeMCU ESP8266 + LED P10 + RTC DS3231
 * 
 * FITUR: JAM BESAR, TANGGAL, SUHU, RUNNING TEXT INFO 1/2/3
 * 
Pin on  DMD P10     GPIO      NODEMCU               Pin on  DS3231      NODEMCU
        2  A        GPIO16    D0                            SCL         D1 (GPIO 5)
        4  B        GPIO12    D6                            SDA         D2 (GPIO 4)
        8  CLK      GPIO14    D5                            VCC         3V
        10 SCK      GPIO0     D3                            GND         GND
        12 R        GPIO13    D7
        1  NOE      GPIO15    D8
        3  GND      GND       GND

Catatan:
o Perlu Power Eksternal 5V ke LED P10.

Eksternal Library:
- HJS589 (DMD porting for ESP8266 by dmk007)
- RTC DS3231: https://github.com/Makuna/Rtc
- ArduinoJson V6: https://github.com/bblanchon/ArduinoJson

SEWU AUDIO - Sound System & Lighting
*/

#include <SPI.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <RtcDS3231.h>
RtcDS3231<TwoWire> Rtc(Wire);

#include <HJS589.h>

#include <fonts/ElektronMart6x8.h>
#include <fonts/ElektronMart6x16.h>
#include <fonts/ElektronMart5x6.h>

#include "sewuwebpage.h"

// Data Structures
struct ConfigInfo {
  char nama[64];
  char info1[256];
  char info2[256];
  char info3[256];
};

struct ConfigWifi {
  char wifissid[64];
  char wifipass[64];
};

struct ConfigDisp {
  int cerah;
  int panelCount;
  int displayMode;  // 0 = Normal, 1 = Running Text Only
};

// LED Internal
uint8_t pin_led = 2;

// SETUP RTC
RtcDateTime now;
char weekDay[][7] = {"AHAD", "SENIN", "SELASA", "RABU", "KAMIS", "JUM'AT", "SABTU", "AHAD"};
char monthYear[][4] = {"DES", "JAN", "FEB", "MAR", "APR", "MEI", "JUN", "JUL", "AGU", "SEP", "OKT", "NOV", "DES"};

// SETUP DMD - Will be initialized dynamically based on panel count
HJS589 *Disp = nullptr;

// WEB Server & DNS for Captive Portal
ESP8266WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

IPAddress local_ip(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress netmask(255, 255, 255, 0);

const char *fileconfigdisp = "/configdisp.json";
ConfigDisp configdisp;

const char *fileconfiginfo = "/configinfo.json";
ConfigInfo configinfo;

const char *fileconfigwifi = "/configwifi.json";
ConfigWifi configwifi;

//----------------------------------------------------------------------
// FORWARD DECLARATIONS

void ICACHE_RAM_ATTR refresh();
int I2C_ClearBus();
void textCenter(int y, String Msg);
void branding();
void checkWiFiAP();
void restartWiFiAP();

// WiFi Watchdog Variables
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000;  // Check setiap 5 detik
unsigned long wifiDownSince = 0;
bool wifiWasDown = false;

//----------------------------------------------------------------------
// XML DATA BRIDGE BETWEEN DEVICE AND WEB

void buildXML(){
  yield();

  RtcDateTime now = Rtc.GetDateTime();
  RtcTemperature temp = Rtc.GetTemperature();
  
  int celsius = temp.AsFloatDegC();
  int Year = now.Year();
  int Month = now.Month();
  int Day = now.Day();
  int Hour = now.Hour();
  int Minute = now.Minute();
  int Second = now.Second();

  XML = "<?xml version='1.0'?>";
  XML += "<rData>";
  
  // Date & Time
  XML += "<rYear>"; XML += Year; XML += "</rYear>";
  XML += "<rMonth>"; XML += Month; XML += "</rMonth>";
  XML += "<rDay>"; XML += Day; XML += "</rDay>";
  XML += "<rHour>"; XML += Hour; XML += "</rHour>";
  XML += "<rMinute>"; XML += Minute; XML += "</rMinute>";
  XML += "<rSecond>"; XML += Second; XML += "</rSecond>";
  XML += "<rTemp>"; XML += celsius; XML += "</rTemp>";

  // Config data
  XML += "<rNama>"; XML += configinfo.nama; XML += "</rNama>";
  XML += "<rInfo1>"; XML += configinfo.info1; XML += "</rInfo1>";
  XML += "<rInfo2>"; XML += configinfo.info2; XML += "</rInfo2>";
  XML += "<rInfo3>"; XML += configinfo.info3; XML += "</rInfo3>";
  XML += "<rWifissid>"; XML += configwifi.wifissid; XML += "</rWifissid>";
  XML += "<rWifipass>"; XML += configwifi.wifipass; XML += "</rWifipass>";
  XML += "<rCerah>"; XML += configdisp.cerah; XML += "</rCerah>";
  XML += "<rPanelCount>"; XML += configdisp.panelCount; XML += "</rPanelCount>";
  XML += "<rDisplayMode>"; XML += configdisp.displayMode; XML += "</rDisplayMode>";

  XML += "</rData>";
  yield();
}

void handleXML() {
  yield();
  buildXML();
  server.send(200, "text/xml", XML);
  yield();
}

//----------------------------------------------------------------------
// SAFE PAGE SENDING

void sendPageSafe(const char* page) {
  yield();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  
  const size_t chunkSize = 1024;
  size_t len = strlen_P(page);
  size_t sent = 0;
  
  while (sent < len) {
    yield();
    size_t toSend = min(chunkSize, len - sent);
    char buffer[chunkSize + 1];
    memcpy_P(buffer, page + sent, toSend);
    buffer[toSend] = 0;
    server.sendContent(buffer);
    sent += toSend;
  }
  
  server.sendContent("");
  yield();
}

//----------------------------------------------------------------------
// REINIT DISPLAY WITH DYNAMIC PANEL COUNT

void Disp_reinit() {
  if (Disp != nullptr) {
    timer0_detachInterrupt();
    delete Disp;
  }
  
  Disp = new HJS589(configdisp.panelCount, 1);
  Disp->start();
  
  noInterrupts();
  timer0_isr_init();
  timer0_attachInterrupt(refresh);
  timer0_write(ESP.getCycleCount() + 80000);
  interrupts();
  
  Disp->clear();
  Disp->setBrightness(configdisp.cerah);
}

//----------------------------------------------------------------------
// HJS589 P10 REFRESH FUNCTION

void ICACHE_RAM_ATTR refresh() {
  if (Disp != nullptr) {
    Disp->refresh();
    timer0_write(ESP.getCycleCount() + 80000);
  }
}

//----------------------------------------------------------------------
// LOAD DEFAULT DATA

void LoadDataAwal() {

  if (strlen(configinfo.nama) == 0) {
    strlcpy(configinfo.nama, "SEWU AUDIO", sizeof(configinfo.nama));
  }

  if (strlen(configinfo.info1) == 0) {
    strlcpy(configinfo.info1, "selamat datang di hajatan sewu audio", sizeof(configinfo.info1));
  }

  if (strlen(configinfo.info2) == 0) {
    strlcpy(configinfo.info2, "selamat menikmati", sizeof(configinfo.info2));
  }

  if (strlen(configinfo.info3) == 0) {
    strlcpy(configinfo.info3, "sewu audio sound system and lighting", sizeof(configinfo.info3));
  }

  if (strlen(configwifi.wifissid) == 0) {
    strlcpy(configwifi.wifissid, "SEWU AUDIO", sizeof(configwifi.wifissid));
  }

  if (strlen(configwifi.wifipass) == 0) {
    strlcpy(configwifi.wifipass, "sewuaudio123", sizeof(configwifi.wifipass));
  }

  if (configdisp.cerah == 0) {
    configdisp.cerah = 100;
  }

  if (configdisp.panelCount == 0) {
    configdisp.panelCount = 2;
  }
  
  // displayMode default is 0 (Normal), no need to set if 0
}

//----------------------------------------------------------------------
// SETUP

void setup() {

  Serial.begin(115200);
  Serial.println("\n\nSEWU AUDIO - Running Text System");
  Serial.println("================================");

  // RTC DS3231
  int rtn = I2C_ClearBus();
  if (rtn != 0) {
    Serial.println(F("I2C bus error. Could not clear"));
    if (rtn == 1) {
      Serial.println(F("SCL clock line held low"));
    } else if (rtn == 2) {
      Serial.println(F("SCL clock line held low by slave clock stretch"));
    } else if (rtn == 3) {
      Serial.println(F("SDA data line held low"));
    }
  } else {
    Wire.begin();
  }
  
  Rtc.Begin();

  if (!Rtc.GetIsRunning()) {
    Rtc.SetIsRunning(true);
  }
  
  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);
  
  Serial.println("RTC initialized");

  // WIFI
  pinMode(pin_led, OUTPUT);

  SPIFFS.begin();
  
  loadWifiConfig(fileconfigwifi, configwifi);
  loadInfoConfig(fileconfiginfo, configinfo);
  loadDispConfig(fileconfigdisp, configdisp);

  LoadDataAwal();

  // WiFi AP Mode Only - dengan konfigurasi stabil
  WiFi.persistent(true);  // Simpan konfigurasi ke flash
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  
  // Bersihkan state WiFi sebelumnya
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  delay(100);
  
  // Set mode dan konfigurasi
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);
  
  // Konfigurasi IP
  WiFi.softAPConfig(local_ip, gateway, netmask);
  delay(100);
  
  // Start AP dengan retry
  int apRetry = 0;
  bool apStarted = false;
  while (!apStarted && apRetry < 5) {
    apStarted = WiFi.softAP(configwifi.wifissid, configwifi.wifipass, 1, false, 4);
    if (!apStarted) {
      Serial.print("AP start failed, retry ");
      Serial.println(apRetry + 1);
      delay(500);
      apRetry++;
    }
  }
  
  if (apStarted) {
    Serial.println("WiFi AP started successfully!");
    digitalWrite(pin_led, LOW);
  } else {
    Serial.println("WiFi AP failed after retries, will retry in loop");
    digitalWrite(pin_led, HIGH);
  }
  
  // Set WiFi power untuk stabilitas
  WiFi.setOutputPower(20.5);  // Max power
  
  // Start DNS Server for Captive Portal
  dnsServer.start(DNS_PORT, "*", local_ip);
  Serial.println("DNS Captive Portal started");
  
  Serial.print("AP SSID: ");
  Serial.println(configwifi.wifissid);
  Serial.print("AP Password: ");
  Serial.println(configwifi.wifipass);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // Web Server Routes
  server.on("/", []() {
    yield();
    sendPageSafe(webpage);
    
    if (server.hasArg("date")) {
      yield();
      String sd = server.arg("date");
      uint16_t year = ((sd[0]-'0')*1000)+((sd[1]-'0')*100)+((sd[2]-'0')*10)+(sd[3]-'0');
      uint8_t month = ((sd[5]-'0')*10)+(sd[6]-'0');
      uint8_t day = ((sd[8]-'0')*10)+(sd[9]-'0');
      
      RtcDateTime now = Rtc.GetDateTime();
      Rtc.SetDateTime(RtcDateTime(year, month, day, now.Hour(), now.Minute(), 0));
      server.send(200, "text/html", "OK");
      yield();
    }
    
    if (server.hasArg("time")) {
      yield();
      String st = server.arg("time");
      uint8_t hour = ((st[0]-'0')*10)+(st[1]-'0');
      uint8_t minute = ((st[3]-'0')*10)+(st[4]-'0');
      
      RtcDateTime now = Rtc.GetDateTime();
      Rtc.SetDateTime(RtcDateTime(now.Year(), now.Month(), now.Day(), hour, minute, 0));
      server.send(200, "text/html", "OK");
      yield();
    }
  });

  server.on("/settingwifi", HTTP_POST, handleSettingWifiUpdate);
  server.on("/settinginfo", HTTP_POST, handleSettingInfoUpdate);
  server.on("/settingdisp", HTTP_POST, handleSettingDispUpdate);
  server.on("/xml", handleXML);
  
  server.begin();
  Serial.println("HTTP server started");

  // DMD - Initialize with saved panel count
  Disp_reinit();

  // BRANDING
  branding();
}

//----------------------------------------------------------------------
// CONFIG HANDLERS

void loadDispConfig(const char *fileconfigdisp, ConfigDisp &configdisp) {
  File configFileDisp = SPIFFS.open(fileconfigdisp, "r");

  if (!configFileDisp) {
    Serial.println("Failed to open display config");
    return;
  }

  size_t size = configFileDisp.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFileDisp.readBytes(buf.get(), size);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, buf.get());

  if (error) {
    Serial.println("Failed to parse display config");
    return;
  }
  
  configdisp.cerah = doc["cerah"];
  configdisp.panelCount = doc["panelCount"] | 2;
  configdisp.displayMode = doc["displayMode"] | 0;

  configFileDisp.close();
}

void handleSettingDispUpdate() {
  timer0_detachInterrupt();
  
  String datadisp = server.arg("plain");
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, datadisp);

  File configFileDisp = SPIFFS.open(fileconfigdisp, "w");
  
  if (!configFileDisp) {
    Serial.println("Failed to open display config for writing");
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  }
  
  serializeJson(doc, configFileDisp);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  } else {
    configFileDisp.close();
    Serial.println("Display config updated");
    
    loadDispConfig(fileconfigdisp, configdisp);
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    
    yield();
    Disp_reinit();
  }
}

void loadInfoConfig(const char *fileconfiginfo, ConfigInfo &configinfo) {
  File configFileInfo = SPIFFS.open(fileconfiginfo, "r");
  
  if (!configFileInfo) {
    Serial.println("Failed to open info config");
    return;
  }

  size_t size = configFileInfo.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFileInfo.readBytes(buf.get(), size);

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, buf.get());

  if (error) {
    Serial.println("Failed to parse info config");
    return;
  }

  strlcpy(configinfo.nama, doc["nama"] | "SEWU AUDIO", sizeof(configinfo.nama));
  strlcpy(configinfo.info1, doc["info1"] | "selamat datang di hajatan sewu audio", sizeof(configinfo.info1));
  strlcpy(configinfo.info2, doc["info2"] | "selamat menikmati", sizeof(configinfo.info2));
  strlcpy(configinfo.info3, doc["info3"] | "sewu audio sound system and lighting", sizeof(configinfo.info3));

  configFileInfo.close();
}

void handleSettingInfoUpdate() {
  timer0_detachInterrupt();
  yield();

  String datainfo = server.arg("plain");
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, datainfo);

  File configFileInfo = SPIFFS.open(fileconfiginfo, "w");
  
  if (!configFileInfo) {
    Serial.println("Failed to open info config for writing");
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  }
  
  serializeJson(doc, configFileInfo);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  } else {
    configFileInfo.close();
    Serial.println("Info config updated");
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    
    loadInfoConfig(fileconfiginfo, configinfo);
    
    yield();
    Disp_reinit();
  }
  
  timer0_attachInterrupt(refresh);
}

void loadWifiConfig(const char *fileconfigwifi, ConfigWifi &configwifi) {
  File configFileWifi = SPIFFS.open(fileconfigwifi, "r");
  
  if (!configFileWifi) {
    Serial.println("Failed to open wifi config");
    return;
  }

  size_t size = configFileWifi.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFileWifi.readBytes(buf.get(), size);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, buf.get());

  if (error) {
    Serial.println("Failed to parse wifi config");
    return;
  }

  strlcpy(configwifi.wifissid, doc["wifissid"] | "SEWU AUDIO", sizeof(configwifi.wifissid));
  strlcpy(configwifi.wifipass, doc["wifipass"] | "sewuaudio123", sizeof(configwifi.wifipass));

  configFileWifi.close();
}

void handleSettingWifiUpdate() {
  timer0_detachInterrupt();
  yield();
  
  String data = server.arg("plain");

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, data);

  File configFile = SPIFFS.open("/configwifi.json", "w");
  if (!configFile) {
    Serial.println("Error opening wifi config for writing");
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  }
  
  serializeJson(doc, configFile);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    server.send(500, "application/json", "{\"status\":\"error\"}");
    timer0_attachInterrupt(refresh);
    return;
  } else {
    configFile.close();
    Serial.println("WiFi config updated - restarting...");
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    loadWifiConfig(fileconfigwifi, configwifi);

    yield();
    delay(1000);
    ESP.restart();
  }
  
  timer0_attachInterrupt(refresh);
}

//----------------------------------------------------------------------
// LOOP

uint8_t tampilanMode;
uint8_t runningTextMode;  // For Running Text Only mode: 0=info1, 1=info2, 2=info3

void loop() {
  yield();
  
  dnsServer.processNextRequest();  // Captive Portal
  server.handleClient();
  yield();
  
  // WiFi AP Watchdog - Auto recovery
  checkWiFiAP();
  yield();

  // Check display mode
  if (configdisp.displayMode == 1) {
    // Running Text Only Mode
    switch(runningTextMode) {
      case 0:
        TeksJalanFullInfo1();
        break;
      case 1:
        TeksJalanFullInfo2();
        break;
      case 2:
        TeksJalanFullInfo3();
        break;
    }
  } else {
    // Normal Mode
    switch(tampilanMode) {
      case 0:
        TampilJamBesar();
        break;
      case 1:
        TampilTanggal();
        break;
      case 2:
        TampilSuhu();
        break;
      case 3:
        TeksJalanNama();
        break;
      case 4:
        TeksJalanInfo1();
        break;
      case 5:
        TeksJalanInfo2();
        break;
      case 6:
        TeksJalanInfo3();
        break;
    }
  }
  
  yield();
}

//----------------------------------------------------------------------
// DISPLAY MODES

void TampilJamBesar() {
  RtcDateTime now = Rtc.GetDateTime();
  char jam[3];
  char menit[3];
  char detik[3];
  sprintf(jam,"%02d", now.Hour());
  sprintf(menit,"%02d", now.Minute());
  sprintf(detik,"%02d", now.Second());

  static uint8_t y;
  static uint8_t d;
  static uint32_t pM;
  uint32_t cM = millis();

  static uint32_t pMPulse;
  static uint8_t pulse;
  
  if (cM - pMPulse >= 100) {
    pMPulse = cM;
    pulse++;
  }
  
  if (pulse > 8) {
    pulse=0;
  }

  if(cM - pM > 35) {
    if(d == 0 and y < 20) {
      pM=cM;
      y++;
    }
    if(d == 1 and y > 0) {
      pM=cM;
      y--;
    }
  }
  
  if(cM - pM > 10000 and y == 20) {
    d=1;
  }
  
  // Adjust position based on panel count
  int xOffset = (Disp->width() - 32) / 2;
  
  if(y == 20) {
    Disp->drawRect(xOffset+15,3+pulse,xOffset+18,11-pulse,0,1);
  }
  
  if(y < 20) {
    Disp->drawRect(xOffset+15,3,xOffset+18,11,0,0);
  }
   
  if(y == 0 and d == 1) {
    d=0;
    Disp->clear();
    tampilanMode = 1;
  }
  
  // JAM
  Disp->setFont(ElektronMart6x16);
  Disp->drawText(xOffset+1, y - 20, jam);

  // MENIT
  Disp->setFont(ElektronMart5x6);
  Disp->drawText(xOffset+20, y - 20, menit);

  // DETIK
  Disp->setFont(ElektronMart5x6);
  Disp->drawText(xOffset+20, y - 20 + 8, detik);
}

void TampilTanggal() {
  RtcDateTime now = Rtc.GetDateTime();
  static uint8_t d;
  static char hari[8];
  static char tanggal[18];

  static uint32_t pM;
  uint32_t cM = millis();

  if (cM - pM > 3000) {
    pM = cM;
    d++;
  
    sprintf(hari, "%s", weekDay[now.DayOfWeek()]);
    sprintf(tanggal, "%02d %s", now.Day(), monthYear[now.Month()]);
    
    Disp->setFont(ElektronMart5x6);
    textCenter(0,hari);
    textCenter(8,tanggal);

    if (d >= 2) {
      d = 0;
      Disp->clear();
      tampilanMode = 2;
    }
  }
}

void TampilSuhu(){
  RtcTemperature temp = Rtc.GetTemperature();
  int celsius = temp.AsFloatDegC();
  char suhu[6];
  int koreksisuhu = 2;

  static uint8_t d;
  static uint32_t pM;
  uint32_t cM = millis();

  if (cM - pM > 3000) {
    pM = cM;
    d++;
  
    Disp->setFont(ElektronMart5x6);
    textCenter(0, "SUHU");
    sprintf(suhu,"%dC*",celsius - koreksisuhu);
    textCenter(8, suhu);

    if (d >= 2) {
      d = 0;
      Disp->clear();
      tampilanMode = 3;
    }
  }
}

//----------------------------------------------------------------------
// SCROLLING TEXT

void TeksJalanNama() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 50;
  int width = Disp->width();
  Disp->setFont(ElektronMart6x8);
  textCenter(0,"SEWU AUDIO");
  Disp->drawRect(0,6,Disp->width(),6,1,1);
  int fullScroll = Disp->textWidth(configinfo.nama) + width;
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      tampilanMode = 4;
    }
    Disp->drawText(width - x, 8, configinfo.nama);
  }
}

void TeksJalanInfo1() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 50;
  int width = Disp->width();
  Disp->setFont(ElektronMart6x8);
  
  // Show small clock on top
  RtcDateTime now = Rtc.GetDateTime();
  char jam[16];
  sprintf(jam,"%02d:%02d:%02d", now.Hour(), now.Minute(), now.Second());
  Disp->setFont(ElektronMart5x6);
  textCenter(0,jam);
  
  Disp->setFont(ElektronMart6x8);
  int fullScroll = Disp->textWidth(configinfo.info1) + width;
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      tampilanMode = 5;
      return;
    }
    Disp->drawText(width - x, 8, configinfo.info1);
  }
}

void TeksJalanInfo2() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 50;
  int width = Disp->width();
  Disp->setFont(ElektronMart6x8);
  
  // Show small clock on top
  RtcDateTime now = Rtc.GetDateTime();
  char jam[16];
  sprintf(jam,"%02d:%02d:%02d", now.Hour(), now.Minute(), now.Second());
  Disp->setFont(ElektronMart5x6);
  textCenter(0,jam);
  
  Disp->setFont(ElektronMart6x8);
  int fullScroll = Disp->textWidth(configinfo.info2) + width;
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      tampilanMode = 6;
      return;
    }
    Disp->drawText(width - x, 8, configinfo.info2);
  }
}

void TeksJalanInfo3() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 50;
  int width = Disp->width();
  Disp->setFont(ElektronMart6x8);
  
  // Show small clock on top
  RtcDateTime now = Rtc.GetDateTime();
  char jam[16];
  sprintf(jam,"%02d:%02d:%02d", now.Hour(), now.Minute(), now.Second());
  Disp->setFont(ElektronMart5x6);
  textCenter(0,jam);
  
  Disp->setFont(ElektronMart6x8);
  int fullScroll = Disp->textWidth(configinfo.info3) + width;
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      tampilanMode = 0;
      return;
    }
    Disp->drawText(width - x, 8, configinfo.info3);
  }
}

//----------------------------------------------------------------------
// FULL SCREEN RUNNING TEXT (Running Text Only Mode)

void TeksJalanFullInfo1() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 40;
  int width = Disp->width();
  
  Disp->setFont(ElektronMart6x16);
  int fullScroll = Disp->textWidth(configinfo.info1) + width;
  
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      runningTextMode = 1;
      return;
    }
    Disp->drawText(width - x, 0, configinfo.info1);
  }
}

void TeksJalanFullInfo2() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 40;
  int width = Disp->width();
  
  Disp->setFont(ElektronMart6x16);
  int fullScroll = Disp->textWidth(configinfo.info2) + width;
  
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      runningTextMode = 2;
      return;
    }
    Disp->drawText(width - x, 0, configinfo.info2);
  }
}

void TeksJalanFullInfo3() {
  static uint32_t pM;
  static uint32_t x;
  static uint32_t Speed = 40;
  int width = Disp->width();
  
  Disp->setFont(ElektronMart6x16);
  int fullScroll = Disp->textWidth(configinfo.info3) + width;
  
  if((millis() - pM) > Speed) {
    pM = millis();
    if (x < fullScroll) {
      ++x;
    } else {
      x = 0;
      Disp->clear();
      runningTextMode = 0;
      return;
    }
    Disp->drawText(width - x, 0, configinfo.info3);
  }
}

//----------------------------------------------------------------------
// TEXT UTILITIES

void textCenter(int y,String Msg) {
  int center = int((Disp->width()-Disp->textWidth(Msg)) / 2);
  Disp->drawText(center,y,Msg);
}

//----------------------------------------------------------------------
// BRANDING ANIMATION

void branding() {
  Disp->clear();
  Disp->setFont(ElektronMart6x8);
  textCenter(0, "SEWU");
  textCenter(8, "AUDIO");
  delay(2000);

  Disp->clear();
  Disp->setFont(ElektronMart5x6);
  textCenter(3, "READY!");
  delay(1000);
  Disp->clear();
}

//----------------------------------------------------------------------
// WIFI AP WATCHDOG - Auto Recovery

void checkWiFiAP() {
  unsigned long currentMillis = millis();
  
  // Hanya check setiap WIFI_CHECK_INTERVAL ms
  if (currentMillis - lastWiFiCheck < WIFI_CHECK_INTERVAL) {
    return;
  }
  lastWiFiCheck = currentMillis;
  
  // Cek status WiFi AP
  WiFiMode_t currentMode = WiFi.getMode();
  IPAddress apIP = WiFi.softAPIP();
  int stationCount = WiFi.softAPgetStationNum();
  
  // WiFi AP dianggap DOWN jika:
  // 1. Mode bukan AP atau AP_STA
  // 2. Atau IP AP adalah 0.0.0.0
  bool wifiIsDown = (currentMode != WIFI_AP && currentMode != WIFI_AP_STA) || 
                    (apIP[0] == 0 && apIP[1] == 0 && apIP[2] == 0 && apIP[3] == 0);
  
  if (wifiIsDown) {
    if (!wifiWasDown) {
      // Baru saja down, catat waktu
      wifiDownSince = currentMillis;
      wifiWasDown = true;
      Serial.println("WARNING: WiFi AP appears to be down!");
    } else {
      // Sudah down, cek berapa lama
      unsigned long downDuration = currentMillis - wifiDownSince;
      Serial.print("WiFi AP down for ");
      Serial.print(downDuration / 1000);
      Serial.println(" seconds");
      
      // Jika sudah down lebih dari 3 detik, restart AP
      if (downDuration > 3000) {
        Serial.println("Attempting WiFi AP restart...");
        restartWiFiAP();
      }
    }
    digitalWrite(pin_led, HIGH);  // LED mati = WiFi problem
  } else {
    // WiFi OK
    if (wifiWasDown) {
      Serial.println("WiFi AP recovered!");
      wifiWasDown = false;
    }
    digitalWrite(pin_led, LOW);   // LED nyala = WiFi OK
  }
}

void restartWiFiAP() {
  Serial.println("=== RESTARTING WIFI AP ===");
  
  // Stop DNS server dulu
  dnsServer.stop();
  yield();
  
  // Disconnect dan matikan WiFi
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  yield();
  delay(200);
  
  WiFi.mode(WIFI_OFF);
  yield();
  delay(200);
  
  // Nyalakan kembali
  WiFi.mode(WIFI_AP);
  yield();
  delay(100);
  
  WiFi.softAPConfig(local_ip, gateway, netmask);
  yield();
  delay(100);
  
  // Start AP dengan retry
  int apRetry = 0;
  bool apStarted = false;
  while (!apStarted && apRetry < 3) {
    yield();
    apStarted = WiFi.softAP(configwifi.wifissid, configwifi.wifipass, 1, false, 4);
    if (!apStarted) {
      Serial.print("AP restart failed, retry ");
      Serial.println(apRetry + 1);
      delay(300);
      apRetry++;
    }
  }
  
  if (apStarted) {
    Serial.println("WiFi AP restarted successfully!");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    
    // Restart DNS server
    dnsServer.start(DNS_PORT, "*", local_ip);
    
    wifiWasDown = false;
    digitalWrite(pin_led, LOW);
  } else {
    Serial.println("WiFi AP restart FAILED - will retry next cycle");
    digitalWrite(pin_led, HIGH);
  }
  
  yield();
}

//----------------------------------------------------------------------
// I2C_ClearBus - Avoid RTC read failure

int I2C_ClearBus() {
  
#if defined(TWCR) && defined(TWEN)
  TWCR &= ~(_BV(TWEN));
#endif

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);

  delay(500);  // Dikurangi dari 2500ms untuk stabilitas WiFi

  boolean SCL_LOW = (digitalRead(SCL) == LOW);
  if (SCL_LOW) {
    return 1;
  }

  boolean SDA_LOW = (digitalRead(SDA) == LOW);
  int clockCount = 20;

  while (SDA_LOW && (clockCount > 0)) {
    clockCount--;
    pinMode(SCL, INPUT);
    pinMode(SCL, OUTPUT);
    delayMicroseconds(10);
    pinMode(SCL, INPUT);
    pinMode(SCL, INPUT_PULLUP);
    delayMicroseconds(10);
    SCL_LOW = (digitalRead(SCL) == LOW);
    int counter = 20;
    while (SCL_LOW && (counter > 0)) {
      counter--;
      delay(50);  // Dikurangi dari 100ms
      yield();    // Jaga WiFi stack
      SCL_LOW = (digitalRead(SCL) == LOW);
    }
    if (SCL_LOW) {
      return 2;
    }
    SDA_LOW = (digitalRead(SDA) == LOW);
  }
  if (SDA_LOW) {
    return 3;
  }

  pinMode(SDA, INPUT);
  pinMode(SDA, OUTPUT);
  delayMicroseconds(10);
  pinMode(SDA, INPUT);
  pinMode(SDA, INPUT_PULLUP);
  delayMicroseconds(10);
  pinMode(SDA, INPUT);
  pinMode(SCL, INPUT);
  return 0;
}
