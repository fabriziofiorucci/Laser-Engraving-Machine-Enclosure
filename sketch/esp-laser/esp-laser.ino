/*
 * ESP Laser - Laser CNC enclosure controller - 2022 Fabrizio Fiorucci
 */

// TFT_eSPI library used for 240x240 OLED display
// Based on this tutorial
// https://www.instructables.com/id/Converting-Images-to-Flash-Memory-Iconsimages-for-/
// https://github.com/STEMpedia/eviveProjects/blob/master/imageToFlashMemoryIconsForTFT/
// Don't forget to change User_Setup.h inside TFT_eSPI library !

// PCF8574 library by Renzo Mischianti - https://www.mischianti.org/2019/01/02/pcf8574_1-i2c-digital-i-o-expander-fast-easy-usage/
// APA102 by Pololu
// TFT_eSPI by Bodmer - Bitmaps generated with LCD Image Converter - https://sourceforge.net/projects/lcd-image-converter/
// PubSubClient by Nick O'Leary
// NTPClient by Fabrice Weinberg

#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TFT_eSPI.h>
#include "boot.h"
#include "aircompressor.h"
#include "fan.h"
#include "laserbeam.h"
#include "lasercnc.h"
#include "PCF8574.h"
#include <APA102.h>

// Custom parameters - TO BE EDITED
const char * ssid = "MY_ESSID";
const char * password = "MY_ESSID_PASSWORD";

const char * mqttServer = "MQTT_SERVER_IP_ADDRESS";
const char * mqttClientId = "esp-laser";
const char * mqttClientPassword = "MQTT_PASSWORD";
const char * otaName = "esp-laser";
const char * otaPassword = "OTA_PASSWORD";
const char * ntpServer = "NTP_SERVER_IP_ADDRESS";

const char * controlTopic = "esp-laser/control";
const char * debugTopic = "esp-laser/debug";

// WiFi
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, 3600, 60000);

// APA102
const uint8_t APA102_DATA = D3;
const uint8_t APA102_CLK = D4;

// Create an object for writing to the LED strip.
APA102<APA102_DATA, APA102_CLK> ledStrip;

// Set the number of LEDs to control.
const uint16_t ledCount = 38;
// Create a buffer for holding the colors (3 bytes per color).
rgb_color colors[ledCount];

// Set the brightness to use (the maximum is 31).
const uint8_t brightness = 8;

// TFT display
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite_boot = TFT_eSprite(&tft);
TFT_eSprite sprite_aircompressor = TFT_eSprite(&tft);
TFT_eSprite sprite_fan = TFT_eSprite(&tft);
TFT_eSprite sprite_laserbeam = TFT_eSprite(&tft);

#define TFT_MOSI  13  // D7
#define TFT_SCK 14    // D5
#define TFT_CS -1     // Not used
#define TFT_DC  12    // D6
#define TFT_RESET 16  // D0

// PCF8574 I2C port expanders
PCF8574 pcf8574_mainboard(0x27);
PCF8574 pcf8574_portextender(0x26);

long lastMsg = 0;
long pollInterval = 1000;

// Buttons are active low
int button1 = 1;
int button2 = 1;
int button3 = 1;

boolean isFanOn=false;
boolean isLaserOn=false;
boolean isWebcamOn=false;
boolean isAirAssistOn=false;

void setup_wifi() {
  delay(10);

  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  for (int i = 0; i < 500 && WiFi.status() != WL_CONNECTED; i++) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(25);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(25);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Restarting ESP due to loss of Wi-Fi");
    ESP.restart();
  }

  randomSeed(micros());

  Serial.println("");
  Serial.print("WiFi connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    String clientId = mqttClientId;

    Serial.print("Attempting MQTT connection...");

    if (mqttClient.connect((char * ) String(ESP.getChipId()).c_str(), mqttClientId, mqttClientPassword)) {
      Serial.println("connected to MQTT");
      mqttClient.subscribe(controlTopic);
      mqttClient.setBufferSize(1024);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 3 seconds");
      delay(3000);
    }
  }
}

void IRAM_ATTR callback(char * topic, byte * payload, unsigned int length) {
  if (strcmp(topic,controlTopic)==0) {
    char cmd = (char)payload[0];
    if (cmd=='R') {
      ESP.restart();
    }
  }
}

/* Converts a color from HSV to RGB.
 * h is hue, as a number between 0 and 360.
 * s is the saturation, as a number between 0 and 255.
 * v is the value, as a number between 0 and 255. */
rgb_color hsvToRgb(uint16_t h, uint8_t s, uint8_t v)
{
    uint8_t f = (h % 60) * 255 / 60;
    uint8_t p = (255 - s) * (uint16_t)v / 255;
    uint8_t q = (255 - f * (uint16_t)s / 255) * (uint16_t)v / 255;
    uint8_t t = (255 - (255 - f) * (uint16_t)s / 255) * (uint16_t)v / 255;
    uint8_t r = 0, g = 0, b = 0;
    switch((h / 60) % 6){
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return rgb_color(r, g, b);
}


void setup(void) {
  // EEPROM init
  EEPROM.begin(512);

  Serial.begin(115200, SERIAL_8N1);
  Serial.println("-----------------------------------------");
  Serial.printf("Chip ID     : %08X\n", ESP.getChipId());
  Serial.printf("Core version: %s\n", ESP.getCoreVersion().c_str());
  Serial.printf("SDK version : %s\n", ESP.getSdkVersion());
  Serial.printf("CPU speed   : %dMhz\n", ESP.getCpuFreqMHz());
  Serial.printf("Free Heap   : %d\n", ESP.getFreeHeap());
  Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());
  Serial.printf("EEPROM size : %d\n", EEPROM.length());
  Serial.println("-----------------------------------------");

  Serial.println("Laser CNC controller init");

  // Display initialization
  tft.begin();     // initialize a ST7789 chip
  tft.setSwapBytes(true); // Swap the byte order for pushImage() - corrects endianness
  tft.setRotation(3);
  tft.fillScreen(TFT_WHITE);

  tft.pushImage(0,0,240,240,lasercncimage);
  delay(1000);
  tft.fillScreen(TFT_WHITE);

  sprite_boot.setColorDepth(4); // Optionally set depth to 8 to halve RAM use
  sprite_boot.createSprite(240,240);
  sprite_boot.setTextSize(1);

  sprite_boot.fillSprite(TFT_WHITE);
  sprite_boot.setTextColor(TFT_BLACK, TFT_WHITE);
  sprite_boot.drawString("Laser CNC startup", 0, 0, 2);
  sprite_boot.drawString("Connecting to Wi-Fi...", 0, 16, 2);
  sprite_boot.pushSprite(0, 0);

  // WiFi init
  setup_wifi();

  // NTP init
  timeClient.begin();

  sprite_boot.drawString("Connecting to MQTT Broker...",0,32,2);
  sprite_boot.pushSprite(0,0);

  // MQTT init
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(1024);
  reconnectMqtt();

  pcf8574_mainboard.pinMode(P0,OUTPUT, HIGH);
  pcf8574_mainboard.pinMode(P1,INPUT);
  pcf8574_mainboard.pinMode(P2,INPUT);
  pcf8574_mainboard.pinMode(P3,INPUT);

  pcf8574_portextender.pinMode(P0,OUTPUT);
  pcf8574_portextender.pinMode(P1,OUTPUT);
  pcf8574_portextender.pinMode(P2,OUTPUT);
  pcf8574_portextender.pinMode(P3,OUTPUT);

  delay(250);
  sprite_boot.drawString("Initializing mainboard I2C port expander...",0,48,2);
  sprite_boot.pushSprite(0,0);

  if (!pcf8574_mainboard.begin()) {
    Serial.println("PCF8574 @ 0x27 - mainboard error");

    sprite_boot.drawString("Mainboard I2C port expander FAILED",0,64,2);
    sprite_boot.pushSprite(0,0);

    delay(5000);
  } else {
    Serial.println("PCF8572 @ 0x27 - mainboard initialized");
  }

  delay(250);
  sprite_boot.drawString("Initializing daughterboard I2C port expander...",0,64,2);
  sprite_boot.pushSprite(0,0);

  if (!pcf8574_portextender.begin()) {
    Serial.println("PCF8574 @ 0x26 - port extender error");
    sprite_boot.drawString("Daughterboard I2C port expander FAILED",0,80,2);
    sprite_boot.pushSprite(0,0);

    delay(5000);
  } else {
    Serial.println("PCF8572 @ 0x26 - port extender initialized");
  }

  delay(250);
  sprite_boot.drawString("Setting default status to off...",0,80,2);
  sprite_boot.pushSprite(0,0);

  // Front fan off
  pcf8574_mainboard.digitalWrite(0,LOW);

  // Rear fan off
  pcf8574_portextender.digitalWrite(1,LOW);

  // Webcam off
  pcf8574_portextender.digitalWrite(0,LOW);

  // Laser CNC off
  pcf8574_portextender.digitalWrite(2,HIGH);

  // Air Assist off
  pcf8574_portextender.digitalWrite(3,HIGH);

  delay(250);
  sprite_boot.drawString("Initializing OTA...",0,96,2);
  sprite_boot.pushSprite(0,0);

  // OTA init
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(otaName);
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA in progress %s", String(progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("OTA Auth failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("OTA Begin failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("OTA Connect failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("OTA Receive failed");
    else if (error == OTA_END_ERROR) Serial.println("OTA End failed");
  });
  ArduinoOTA.begin();

  delay(250);
  tft.fillScreen(TFT_WHITE);

/*
  sprite_aircompressor.setColorDepth(8);
  sprite_aircompressor.createSprite(110,110);
  sprite_aircompressor.pushImage(0, 0, 110, 110, (uint16_t *)aircompressorimage);

  sprite_fan.setColorDepth(4);
  sprite_fan.createSprite(110,110);
  sprite_fan.pushImage(0, 0, 110, 110, (uint16_t *)fanimage);

  sprite_laserbeam.setColorDepth(4);
  sprite_laserbeam.createSprite(110,110);
  sprite_laserbeam.pushImage(0, 0, 110, 110, (uint16_t *)laserbeamimage);*/
}

void loop() {
  ArduinoOTA.handle();
  timeClient.update();

  if (!mqttClient.connected()) {
    reconnectMqtt();
  }

  mqttClient.loop();

  PCF8574::DigitalInput di = pcf8574_mainboard.digitalReadAll();
  if (di.p1!=button1 || di.p2!=button2 || di.p3!=button3) {
    Serial.printf("Buttons: %d %d %d\n",di.p1,di.p2,di.p3);

    if(button1!=di.p1 && button1==0) {
      if(!isFanOn) {
       Serial.println("Fan on");

       // Front fan on
       pcf8574_mainboard.digitalWrite(0,HIGH);

       // Read fan on
       pcf8574_portextender.digitalWrite(1,HIGH);

       tft.pushImage(0,0,110,110,fanimage);
     } else {
       Serial.println("Fan off");

       // Front fan off
       pcf8574_mainboard.digitalWrite(0,LOW);

       // Rear fan off
       pcf8574_portextender.digitalWrite(1,LOW);

       tft.fillRect(0,0,110,110,TFT_WHITE);
      }
      isFanOn=!isFanOn;
    }

    /*
    if(button2!=di.p2 && button2==0) {
      if(!isWebcamOn) {
       Serial.println("Webcam on");

       // Webcam on
       pcf8574_portextender.digitalWrite(0,HIGH);
     } else {
       Serial.println("Webcam off");

       // Webcam off
       pcf8574_portextender.digitalWrite(0,LOW);
      }
      isWebcamOn=!isWebcamOn;
    }
    */

    if(button2!=di.p2 && button2==0) {
      if(!isAirAssistOn) {
       Serial.println("Air Assist on");

       // Air assist on
       pcf8574_portextender.digitalWrite(3,LOW);

       tft.pushImage(120,0,110,110,aircompressorimage);
       } else {
         Serial.println("Air Assist off");

         // Air assist off
         pcf8574_portextender.digitalWrite(3,HIGH);
         
         tft.fillRect(120,0,110,110,TFT_WHITE);
        }
        isAirAssistOn=!isAirAssistOn;
      }

    if(button3!=di.p3 && button3==0) {
          if(!isLaserOn) {
       Serial.println("Laser CNC on");

       // Laser CNC on
       pcf8574_portextender.digitalWrite(2,LOW);

       // Webcam on
       pcf8574_portextender.digitalWrite(0,HIGH);

       tft.pushImage(0,120,110,110,laserbeamimage);
     } else {
       Serial.println("Air/S9 off");

       // Laser CNC off
       pcf8574_portextender.digitalWrite(2,HIGH);

       // Webcam off
       pcf8574_portextender.digitalWrite(0,LOW);

       tft.fillRect(0,120,110,110,TFT_WHITE);
      }
      isLaserOn=!isLaserOn;
    }
    
    button1=di.p1;
    button2=di.p2;
    button3=di.p3;
  }

  uint8_t time = millis() >> 4;

  for(uint16_t i = 0; i < ledCount; i++)
  {
    uint8_t p = time - i * 8;
    colors[i] = hsvToRgb((uint32_t)p * 359 / 256, 255, 255);
  }

  ledStrip.write(colors, ledCount, brightness);

  delay(7);

  long now = millis();
  if (now - lastMsg > pollInterval) {
    lastMsg = now;
  }

}
