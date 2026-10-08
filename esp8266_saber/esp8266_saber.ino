/*
  Horizon Blade - ESP8266 + ADXL345 tilt-saber controller
  ------------------------------------------------------
  Reads tilt from an ADXL345 accelerometer (I2C) and streams it over WiFi (WebSocket)
  to the bridge server running on your PC (bridge/server.js). The game treats it as one more player.

  Board : any ESP8266 (NodeMCU, Wemos D1 mini ...).  Arduino IDE board: "NodeMCU 1.0 (ESP-12E Module)" or "LOLIN(WEMOS) D1 R2 & mini"
  Library to install (Library Manager): "WebSockets" by Markus Sattler (links2004)  -> include <WebSocketsClient.h>
  No ADXL345 library needed: the sensor is read directly over Wire.

  WIRING (ADXL345 breakout "GY-291")          BUTTONS (other leg of each button to GND)
    VCC -> 3V3                                  D5 (GPIO14) -> RECENTER
    GND -> GND                                  D6 (GPIO12) -> START / PLAY AGAIN
    SDA -> D2 (GPIO4)
    SCL -> D1 (GPIO5)
    CS  -> 3V3   (this selects I2C mode - required!)
    SDO -> GND   (I2C address 0x53)
    INT1/INT2 -> not connected

  MOUNTING: sensor flat on top of the handle, X axis to the RIGHT, Y axis pointing FORWARD (toward the screen), Z up.
  USE:  hold level, press RECENTER.  Tilt nose up/down = blade up/down.  Tilt like a steering wheel = blade sweeps left/right.
*/

#include <ESP8266WiFi.h>
#include <Wire.h>
#include <WebSocketsClient.h>

// ======================= EDIT THESE =======================
const char*    WIFI_SSID = "YOUR_WIFI_NAME";
const char*    WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char*    PC_HOST   = "192.168.1.50";   // PC IP printed by the bridge server on start
const uint16_t PC_PORT   = 5173;
const char*    DEVICE_ID = "esp1";           // use a different id (esp2...) for a second controller
// ==========================================================

// Pins
const uint8_t PIN_SDA = D2;   // GPIO4
const uint8_t PIN_SCL = D1;   // GPIO5
const uint8_t PIN_BTN_RECENTER = D5;  // GPIO14
const uint8_t PIN_BTN_START    = D6;  // GPIO12
const uint8_t PIN_LED = LED_BUILTIN;  // active LOW

// Feel / tuning
const float PITCH_GAIN = 1.0f;    // blade elevation degrees per degree of tilt
const float YAW_GAIN   = 1.6f;    // blade sweep degrees per degree of roll (bigger = less wrist movement)
const float BASE_ELEV  = 30.0f;   // blade elevation (deg above horizon) when held level after recenter
const bool  INVERT_YAW = false;   // flip left/right if it moves the wrong way
const bool  INVERT_PITCH = false; // flip up/down if it moves the wrong way
const float SMOOTH     = 0.30f;   // 0..1, higher = faster but noisier
const uint16_t SEND_MS = 20;      // 50 updates per second
const bool  DEBUG_SERIAL = true;

const uint8_t ADXL_ADDR = 0x53;
WebSocketsClient ws;
bool wsUp = false;

float fx = 0, fy = 0, fz = 1;           // filtered acceleration in g
float pitchOff = 0, rollOff = 0;
bool  zeroNext = true;
uint32_t lastSend = 0, lastDbg = 0, ledUntil = 0, lastBtn = 0;

// ---------------- ADXL345 ----------------
void adxlWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool adxlRead(float &x, float &y, float &z) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(0x32);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADXL_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();   // read in order (C++ doesn't guarantee operand order)
  int16_t rx = (int16_t)(b[0] | (b[1] << 8));
  int16_t ry = (int16_t)(b[2] | (b[3] << 8));
  int16_t rz = (int16_t)(b[4] | (b[5] << 8));
  const float S = 0.0039f;  // full resolution: 3.9 mg/LSB
  x = rx * S; y = ry * S; z = rz * S;
  return true;
}

bool adxlBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(0x00);                                   // DEVID
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADXL_ADDR, (uint8_t)1) != 1) return false;
  if (Wire.read() != 0xE5) return false;
  adxlWrite(0x2D, 0x00);   // standby while configuring
  adxlWrite(0x31, 0x09);   // FULL_RES, +-4g
  adxlWrite(0x2C, 0x0B);   // 200 Hz output rate
  adxlWrite(0x2D, 0x08);   // measure mode
  return true;
}

// ---------------- helpers ----------------
void led(uint16_t ms) { ledUntil = millis() + ms; }

void sendText(const char* s) { if (wsUp) ws.sendTXT(s); }

void onWs(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsUp = true;
      zeroNext = true;                 // zero to whatever pose you hold at connect time
      Serial.println("[ws] connected to bridge");
      break;
    case WStype_DISCONNECTED:
      wsUp = false;
      Serial.println("[ws] disconnected");
      break;
    case WStype_TEXT:
      if (strstr((char*)payload, "\"bomb\"")) led(400);       // game says: you hit a bomb
      else if (strstr((char*)payload, "\"h\"")) led(60);      // game says: you hit a cube
      break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nHorizon Blade controller");
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BTN_RECENTER, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);

  while (!adxlBegin()) {
    Serial.println("ADXL345 not found! Check wiring: SDA->D2, SCL->D1, CS->3V3, SDO->GND, VCC->3V3");
    for (int i = 0; i < 6; i++) { digitalWrite(PIN_LED, i & 1); delay(80); }   // fast blink = sensor error
    delay(1000);
  }
  Serial.println("ADXL345 OK");

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // lower latency
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300); Serial.print('.');
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
  }
  Serial.print("\nESP IP: "); Serial.println(WiFi.localIP());

  String path = String("/ws?role=ctrl&id=") + DEVICE_ID;
  ws.begin(PC_HOST, PC_PORT, path);
  ws.onEvent(onWs);
  ws.setReconnectInterval(2000);
  ws.enableHeartbeat(15000, 3000, 2);
}

void loop() {
  ws.loop();
  uint32_t now = millis();

  // LED: on = connected, slow blink = waiting for bridge, pulses when the game sends feedback
  bool on = (now < ledUntil) ? false : (wsUp ? true : ((now / 500) & 1));
  digitalWrite(PIN_LED, on ? LOW : HIGH);

  // Buttons (pull-up, pressed = LOW)
  if (now - lastBtn > 250) {
    if (digitalRead(PIN_BTN_RECENTER) == LOW) { zeroNext = true; lastBtn = now; sendText("{\"t\":\"r\"}"); led(80); }
    else if (digitalRead(PIN_BTN_START) == LOW) { lastBtn = now; sendText("{\"t\":\"start\"}"); led(80); }
  }

  if (now - lastSend < SEND_MS) return;
  lastSend = now;

  float x, y, z;
  if (!adxlRead(x, y, z)) return;

  // Only trust the reading as "gravity" when total acceleration is close to 1 g (not mid-swing)
  float mag = sqrtf(x * x + y * y + z * z);
  if (mag > 0.7f && mag < 1.35f) {
    fx += (x - fx) * SMOOTH;
    fy += (y - fy) * SMOOTH;
    fz += (z - fz) * SMOOTH;
  }

  float pitch = atan2f(fy, sqrtf(fx * fx + fz * fz)) * 57.29578f;   // + = nose up
  float roll  = atan2f(fx, sqrtf(fy * fy + fz * fz)) * 57.29578f;   // + = right side up (tilted left)

  if (zeroNext) { pitchOff = pitch; rollOff = roll; zeroNext = false; }

  float dp = (pitch - pitchOff) * (INVERT_PITCH ? -1.0f : 1.0f);
  float dr = (roll - rollOff) * (INVERT_YAW ? -1.0f : 1.0f);

  float elev = BASE_ELEV + dp * PITCH_GAIN;
  if (elev > 75.0f) elev = 75.0f;           // the game's calibration needs the blade not to point straight up
  if (elev < -30.0f) elev = -30.0f;
  float yaw = -dr * YAW_GAIN;               // roll right-side-down (negative) => sweep right (positive yaw)
  if (yaw > 80.0f) yaw = 80.0f;
  if (yaw < -80.0f) yaw = -80.0f;

  // Encode as W3C deviceorientation angles so the game's saber code can use it unchanged:
  // blade direction = (-sin a cos b, cos a cos b, sin b)  ->  alpha = -yaw, beta = elevation, gamma = 0
  float alpha = fmodf(-yaw + 360.0f, 360.0f);

  char buf[80];
  snprintf(buf, sizeof(buf), "{\"t\":\"o\",\"a\":%.1f,\"b\":%.1f,\"g\":0}", alpha, elev);
  sendText(buf);

  if (DEBUG_SERIAL && now - lastDbg > 250) {
    lastDbg = now;
    Serial.printf("g=(%.2f %.2f %.2f) pitch=%.1f roll=%.1f -> elev=%.1f yaw=%.1f %s\n",
                  fx, fy, fz, pitch, roll, elev, yaw, wsUp ? "[online]" : "[offline]");
  }
}
