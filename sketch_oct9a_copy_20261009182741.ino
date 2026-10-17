#include <WiFi.h>
#include <PubSubClient.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <time.h> // ไลบรารีสำหรับจัดการเวลา

// ================= กำหนดขาเชื่อมต่อ Hardware =================
#define PZEM_RX_PIN 16   // ขา RX ของ ESP32 ต่อเข้า TX ของ PZEM
#define PZEM_TX_PIN 17   // ขา TX ของ ESP32 ต่อเข้า RX ของ PZEM

#define RELAY_PIN 4      // ขาควบคุมโมดูลรีเลย์ (Opto-isolated)
#define BUTTON_PIN 32    // สวิตช์ปุ่มกดที่ตัวเครื่อง (Active LOW)

// ================= ตั้งค่า WiFi และ MQTT =================
const char* ssid = "PRinter love u จุ๊บๆ";
const char* password = "printerz";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

// Topic MQTT
const char* topic_telemetry = "smartplug_01/telemetry";     // ส่งค่า V, A, W, kWh
const char* topic_status    = "smartplug_01/relay/status";  // ส่งสถานะรีเลย์ (ON/OFF)
const char* topic_command   = "smartplug_01/relay/set";     // รับคำสั่งเปิด/ปิด (ON/OFF)
const char* topic_alert     = "smartplug_01/alert";         // แจ้งเตือนเหตุการณ์ผิดปกติ
const char* topic_schedule  = "smartplug_01/schedule/set";  // รับค่าตั้งเวลาทำงาน

// ================= ตั้งค่าเวลา NTP (ประเทศไทย GMT+7) =================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // 7 ชั่วโมง * 3600 วินาที
const int   daylightOffset_sec = 0;

// ================= ตัวแปรระบบตั้งเวลา (Schedule) =================
bool scheduleEnabled = false;
int onHour = 0, onMinute = 0;
int offHour = 0, offMinute = 0;
int lastActionMinute = -1; // ป้องกันการทำงานซ้ำซ้อนในนาทีเดียวกัน

// ================= เงื่อนไขตัดไฟอัตโนมัติ (Energy Saving) =================
const float LOW_POWER_THRESHOLD = 5.0;      
const unsigned long AUTO_OFF_DELAY = 30000; 
unsigned long lowPowerStartTime = 0;
bool lowPowerTimerRunning = false;

// ================= ตัวแปรระบบ =================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

bool relayState = true; 
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

unsigned long lastTelemetrySend = 0;
const unsigned long telemetryInterval = 3000;

// ฟังก์ชันควบคุม Relay
void setRelay(bool state, bool publishUpdate = true) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH);
  Serial.print("Relay is now: ");
  Serial.println(relayState ? "ON" : "OFF");

  if (publishUpdate && mqttClient.connected()) {
    mqttClient.publish(topic_status, relayState ? "ON" : "OFF", true);
  }
}

// Callback รับคำสั่ง MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  // ควบคุมเปิด-ปิด ทันที
  if (String(topic) == topic_command) {
    if (message.equalsIgnoreCase("ON")) {
      setRelay(true);
    } else if (message.equalsIgnoreCase("OFF")) {
      setRelay(false);
    }
  }
  // รับค่าตั้งเวลาจาก MQTT รูปแบบ JSON
  else if (String(topic) == topic_schedule) {
    StaticJsonDocument<256> schedDoc;
    DeserializationError error = deserializeJson(schedDoc, message);
    
    if (error) {
      Serial.println("JSON Parse Error! โปรดเช็ครูปแบบ JSON");
      return;
    }

    scheduleEnabled = schedDoc["enable"]; 
    
    if (scheduleEnabled) {
      const char* onTime = schedDoc["on"];
      const char* offTime = schedDoc["off"];
      
      // ดึงตัวเลขหน้าและหลังเครื่องหมาย : อย่างแม่นยำ (รองรับทั้ง 6:30 และ 06:30)
      if (onTime != nullptr) sscanf(onTime, "%d:%d", &onHour, &onMinute);
      if (offTime != nullptr) sscanf(offTime, "%d:%d", &offHour, &offMinute);
      
      Serial.printf("Schedule Updated -> ON: %02d:%02d | OFF: %02d:%02d\n", onHour, onMinute, offHour, offMinute);
    } else {
      Serial.println("Schedule Disabled");
    }
  }
}

// ฟังก์ชันเชื่อมต่อ MQTT
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32_Plug-" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected!");
      mqttClient.subscribe(topic_command);
      mqttClient.subscribe(topic_schedule); 
      mqttClient.publish(topic_status, relayState ? "ON" : "OFF", true);
    } else {
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setRelay(true, false); 

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  // เริ่มทำงานและดึงเวลาจาก NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP Time Sync started...");

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
  }

  // ================= 1. ตรวจสอบเวลาปัจจุบันสำหรับระบบตั้งเวลา =================
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int currentHour = timeinfo.tm_hour;
    int currentMinute = timeinfo.tm_min;

    if (scheduleEnabled && currentMinute != lastActionMinute) {
      // ตรวจสอบเวลาเปิด
      if (currentHour == onHour && currentMinute == onMinute) {
        Serial.println("Scheduled Trigger: ON");
        setRelay(true);
        lastActionMinute = currentMinute;
      }
      // ตรวจสอบเวลาปิด
      else if (currentHour == offHour && currentMinute == offMinute) {
        Serial.println("Scheduled Trigger: OFF");
        setRelay(false);
        lastActionMinute = currentMinute;
      }
    }
  }

  // ================= 2. ตรวจจับการกดปุ่ม =================
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static int buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) { 
        Serial.println("Physical Button Pressed!");
        setRelay(!relayState);
      }
    }
  }
  lastButtonState = reading;

  // ================= 3. อ่านค่าจาก PZEM และส่งขึ้น MQTT =================
  if (millis() - lastTelemetrySend >= telemetryInterval) {
    lastTelemetrySend = millis();

    float voltage = pzem.voltage();
    float current = pzem.current();
    float power   = pzem.power();
    float energy  = pzem.energy();

    if (isnan(voltage)) voltage = 0.0;
    if (isnan(current)) current = 0.0;
    if (isnan(power))   power   = 0.0;
    if (isnan(energy))  energy  = 0.0;

    StaticJsonDocument<256> doc;
    doc["voltage"] = voltage;
    doc["current"] = current;
    doc["power"]   = power;
    doc["energy"]  = energy;
    doc["relay"]   = relayState ? "ON" : "OFF";
    doc["sch_enabled"] = scheduleEnabled; // แนบสถานะการตั้งเวลากลับไปด้วย
    
    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);
    mqttClient.publish(topic_telemetry, jsonBuffer);

    // ดึงเวลาปัจจุบันมาเช็คใน Serial Monitor
    if (getLocalTime(&timeinfo)) {
      Serial.printf("Current Time: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      Serial.println("Failed to obtain time (NTP syncing...)");
    }

    // ================= 4. ระบบตัดไฟอัตโนมัติ =================
    if (relayState && power > 0.0 && power < LOW_POWER_THRESHOLD) {
      if (!lowPowerTimerRunning) {
        lowPowerStartTime = millis();
        lowPowerTimerRunning = true;
      } else if (millis() - lowPowerStartTime >= AUTO_OFF_DELAY) {
        setRelay(false); 
        lowPowerTimerRunning = false;
        mqttClient.publish(topic_alert, "Auto-cutoff: Power below threshold");
      }
    } else {
      lowPowerTimerRunning = false;
    }
  }
}