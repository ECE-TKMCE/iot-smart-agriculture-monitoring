/*
 * PROJECT: Kollam Smart Agri-Monitor (Cloud Client - Clean Version)
 * ARCHITECTURE: ESP32 -> ThingSpeak (Data) | App -> ThingSpeak (Command)
 * ALERTING: Handled by ThingSpeak React (Cloud) -> Twilio
 */

#include <HardwareSerial.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- 1. CREDENTIALS ---
const char* ssid = "xxxx";
const char* password = "xxxx";

// ThingSpeak Settings
const char* THINGSPEAK_CHANNEL_ID = "xxx"; // <--- FILL THIS
const char* THINGSPEAK_READ_KEY   = "xx"; // <--- FILL THIS
const char* THINGSPEAK_WRITE_KEY  = "xx";  

// --- 2. HARDWARE PINS ---
#define RS485_RX_PIN 16   
#define RS485_TX_PIN 17   
#define RS485_DE_RE_PIN 4   

#define SENSOR_SLAVE_ID 0x01
#define BAUD_RATE 4800
#define START_REGISTER 0x0000
#define NUM_REGISTERS_TO_READ 7

// --- 3. CROP PROFILES (KAU KOLLAM VERIFIED) ---
struct CropProfile {
  String name;
  float min_pH; float max_pH;
  int min_N;    int min_K; 
  int critical_moisture; 
};

CropProfile profiles[] = {
  {"Black Pepper", 5.0, 6.5, 110, 90,  35},
  {"Chilli",       6.0, 7.0, 100, 80,  40},
  {"Cashew",       5.5, 7.0, 90,  60,  25},
  {"Tapioca",      5.5, 7.5, 120, 110, 35},
  {"Banana",       6.0, 7.5, 150, 140, 50},
  {"Coconut",      5.5, 8.0, 130, 120, 30},
  {"Turmeric",     6.0, 7.5, 150, 110, 45}
};

int currentCropIndex = 4; // Default to Banana

// --- 4. GLOBAL VARIABLES ---
ModbusMaster node;
HardwareSerial& ModbusSerial = Serial2;

float g_moisture=0, g_temperature=0, g_pH=0;
float g_ec=0, g_nitrogen=0, g_phosphorus=0, g_potassium=0;
int g_healthScore = 100; // Value uploaded to Field 8

// --- NPK CALIBRATION OFFSETS ---

float nitrogen_scale = 1.0;
float phosphorus_scale = 1.0;
float potassium_scale = 1.0;

float soil_detect_threshold = 5.0;   // moisture threshold
float nitrogen_offset = 22;      // replace with your lab value difference
float phosphorus_offset = 2;
float potassium_offset = 30;
float ec_offset = 12;
// --- Adaptive Smoothing ---
float alpha = 0.1;   // 0.05 = very slow, 0.2 = faster response
unsigned long lastCloudUpload = 0;
unsigned long lastCloudCommandRead = 0;
unsigned long lastModbusPoll = 0;

const long modbusInterval = 3000; 
const long cloudInterval = 30000; 
const long commandInterval = 15000; 

// --- CALLBACKS ---
void preTransmission() { digitalWrite(RS485_DE_RE_PIN, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE_PIN, LOW); }

// --- LOGIC: READ CROP FROM CLOUD (STATUS FIELD) ---
void checkCloudForCropChange() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  // Reads the 'status' field where the App writes the Crop ID
  String url = "http://api.thingspeak.com/channels/" + String(THINGSPEAK_CHANNEL_ID) + "/status/last.txt?api_key=" + String(THINGSPEAK_READ_KEY);
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    int newIndex = payload.toInt();
    
    // Validate range (0 to 6)
    if (newIndex >= 0 && newIndex <= 6 && newIndex != currentCropIndex) {
      currentCropIndex = newIndex;
      Serial.println("Crop Changed from App to: " + profiles[currentCropIndex].name);
      // Force immediate re-calculation so the next upload is correct
      calculateSoilHealth();
    }
  }
  http.end();
}

// --- LOGIC: CALCULATE SCORE ---
void calculateSoilHealth() {
  CropProfile p = profiles[currentCropIndex];
  int score = 100;

  // Penalties based on verified thresholds
  if (g_pH < p.min_pH || g_pH > p.max_pH) score -= 25;
  if (g_nitrogen < p.min_N) score -= 20;
  if (g_potassium < p.min_K) score -= 35;
  if (g_moisture < p.critical_moisture) score -= 20;

  if (score < 0) score = 0;
  g_healthScore = score; 
  // NOTE: No SMS code here. ThingSpeak React will see this Score < 50 and send the SMS.
}

// --- UPLOAD TO THINGSPEAK ---
void uploadData() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + String(THINGSPEAK_WRITE_KEY);
  url += "&field1=" + String(g_temperature, 1);
  url += "&field2=" + String(g_moisture, 1);
  url += "&field3=" + String(g_pH, 1);
  url += "&field4=" + String(g_ec);
  url += "&field5=" + String(g_nitrogen);
  url += "&field6=" + String(g_phosphorus);
  url += "&field7=" + String(g_potassium);
  url += "&field8=" + String(g_healthScore); 
  
  http.begin(url);
  http.GET();
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);
  ModbusSerial.begin(BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  node.begin(SENSOR_SLAVE_ID, ModbusSerial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("Connected!");
}

void loop() {
  unsigned long now = millis();

  // 1. Read Sensor
  if (now - lastModbusPoll > modbusInterval) {
    lastModbusPoll = now;
    uint8_t result = node.readHoldingRegisters(START_REGISTER, NUM_REGISTERS_TO_READ);
    if (result == node.ku8MBSuccess) {
      g_moisture = (float)node.getResponseBuffer(0)/10.0;
      g_temperature = (float)(int16_t)node.getResponseBuffer(1)/10.0;
      g_pH = (float)node.getResponseBuffer(3)/10.0;
      uint16_t rawN = node.getResponseBuffer(4);
      uint16_t rawP = node.getResponseBuffer(5);
      uint16_t rawK = node.getResponseBuffer(6);
      uint16_t rawEc = node.getResponseBuffer(2);
      if (g_moisture > soil_detect_threshold) {

        float targetN  = rawN  + nitrogen_offset;
        float targetP  = rawP  + phosphorus_offset;
        float targetK  = rawK  + potassium_offset;
        float targetEC = rawEc + ec_offset;

  // Exponential smoothing (adaptive behaviour)
        g_nitrogen   = alpha * targetN  + (1 - alpha) * g_nitrogen;
        g_phosphorus = alpha * targetP  + (1 - alpha) * g_phosphorus;
        g_potassium  = alpha * targetK  + (1 - alpha) * g_potassium;
        g_ec         = alpha * targetEC + (1 - alpha) * g_ec;}

      else {

  // In air → show raw values
        g_nitrogen = rawN;
        g_phosphorus = rawP;
        g_potassium = rawK;
        g_ec = rawEc;
      }
      calculateSoilHealth(); 
    }
  }

  // 2. Check for App Command (Crop Change)
  if (now - lastCloudCommandRead > commandInterval) {
    lastCloudCommandRead = now;
    checkCloudForCropChange();
  }

  // 3. Upload Data
  if (now - lastCloudUpload > cloudInterval) {
    lastCloudUpload = now;
    uploadData();
  }
}