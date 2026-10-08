/*
  Horizon Blade - ESP8266 + ADXL345 tilt-saber controller  (USB / serial version)
  -------------------------------------------------------------------------------
  No WiFi, no extra libraries. The ESP8266 sends one JSON line per update over the USB cable;
  bridge/server.js on the PC reads that COM port and feeds the game.

  Board: Arduino IDE -> "NodeMCU 1.0 (ESP-12E Module)" (or LOLIN/WEMOS D1 mini).  Upload speed 115200 is fine.

  WIRING (ADXL345 breakout "GY-291")          BUTTONS (other leg of each button to GND)
    VCC -> 3V3                                  D5 (GPIO14) -> RECENTER
    GND -> GND                                  D6 (GPIO12) -> START / PLAY AGAIN
    SDA -> D2 (GPIO4)
    SCL -> D1 (GPIO5)
    CS  -> 3V3   (selects I2C mode - required!)
    SDO -> GND   (I2C address 0x53)

  IMPORTANT: the serial port carries game data, so do NOT keep the Arduino Serial Monitor open while the
  bridge is running (only one program can use the COM port), and stop the bridge before uploading.

  MOUNTING: sensor flat on top of the handle, X right, Y pointing toward the screen, Z up.
  USE: hold level, press RECENTER. Tilt nose up/down = blade up/down. Tilt like a steering wheel = sweep left/right.
*/

#include <Wire.h>
#include <EEPROM.h>

// Pins
const uint8_t PIN_SDA = D2;   // GPIO4
const uint8_t PIN_SCL = D1;   // GPIO5
const uint8_t PIN_BTN_RECENTER = D5;  // GPIO14
const uint8_t PIN_BTN_START    = D6;  // GPIO12
const uint8_t PIN_BTN_FLASH    = D3;  // GPIO0 = the on-board FLASH button (NodeMCU)
const uint8_t PIN_LED = LED_BUILTIN;  // active LOW

// Feel / tuning
const float PITCH_GAIN = 1.0f;    // blade elevation degrees per degree of tilt
const float YAW_GAIN   = 1.6f;    // blade sweep degrees per degree of roll (bigger = less wrist movement)
const float BASE_ELEV  = 30.0f;   // blade elevation (deg above horizon) when held level after recenter
const bool  INVERT_YAW = false;   // flip left/right if it moves the wrong way
const bool  INVERT_PITCH = false; // flip up/down if it moves the wrong way
// Filtering (accelerometer-only tilt is noisy, so these matter)
const float SMOOTH_MIN  = 0.04f;  // smoothing when held still (lower = steadier, more lag)
const float SMOOTH_MAX  = 0.50f;  // smoothing during fast tilts (higher = snappier)
const float SMOOTH_GAIN = 1.5f;   // how quickly smoothing opens up with movement
const float GRAVITY_TOL = 0.15f;  // ignore samples when |accel| differs from 1 g by more than this (swinging)
const float DEADZONE_DEG = 0.4f;  // ignore angle changes smaller than this (kills jitter at rest)
const uint16_t SEND_MS = 20;      // 50 updates per second
const uint32_t BAUD    = 115200;  // must match the bridge (default 115200)

const uint8_t ADXL_ADDR = 0x53;

float fx = 0, fy = 0, fz = 1;           // filtered acceleration in g
float pitchOff = 0, rollOff = 0;
bool  zeroNext = false, primed = false, bootZeroed = false;
uint32_t lastSend = 0, lastRead = 0, ledUntil = 0, lastBtn = 0;
float outElev = 30.0f, outYaw = 0.0f;

// ---------------- calibration (6-position, stored in flash) ----------------
struct Cal { uint32_t magic; float off[3]; float sc[3]; };
const uint32_t CAL_MAGIC = 0xADC03451;
Cal cal = { CAL_MAGIC, {0, 0, 0}, {1, 1, 1} };
int8_t calStep = -1;                    // -1 = normal operation, otherwise how many sides are captured so far (0..5)
uint8_t calMask = 0;                    // bit i set = side i already captured
float calPos[3], calNeg[3];
uint32_t startHeld = 0, fHeld = 0;
bool calTriggered = false, fCalDone = false;
// Six sides, in any order: bit = axis (0 X, 1 Y, 2 Z) for "up", axis + 3 for "down".
// "up" means that axis points at the ceiling (the arrow printed on the board).
const char* SIDE[6] = { "X arrow up", "Y arrow up", "chip face up", "X arrow down", "Y arrow down", "chip face down" };
int sideIndex(int axis, bool neg) { return axis + (neg ? 3 : 0); }

// Sends a calibration status line to the game page (via the bridge). Texts must not contain quotes.
void calSay(const char* st, const char* text) {
  Serial.printf("{\"t\":\"cal\",\"st\":\"%s\",\"step\":%d,\"text\":\"%s\"}\n", st, calStep + 1, text);
}

void calLoad() {
  EEPROM.begin(64);
  Cal c;
  EEPROM.get(0, c);
  if (c.magic == CAL_MAGIC) cal = c;
}

void calPrompt() {
  char buf[200];
  int n = snprintf(buf, sizeof(buf), "Rest the board still on a side. Still needed: ");
  for (int i = 0; i < 6 && n < (int)sizeof(buf) - 24; i++)
    if (!(calMask & (1 << i))) n += snprintf(buf + n, sizeof(buf) - n, "%s, ", SIDE[i]);
  if (n >= 2) buf[n - 2] = 0;                           // drop the trailing ", "
  calSay("prompt", buf);
}

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
  for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();
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

void calBegin() {
  calStep = 0;
  calMask = 0;
  calPrompt();
}

void calFinish() {
  Cal c = cal;
  c.magic = CAL_MAGIC;
  bool ok = true;
  float range[3];
  for (int a = 0; a < 3; a++) {
    range[a] = calPos[a] - calNeg[a];
    if (range[a] < 1.5f || range[a] > 2.5f) ok = false;
    c.off[a] = (calPos[a] + calNeg[a]) / 2.0f;
    c.sc[a] = 2.0f / range[a];
  }
  calStep = -1;
  if (!ok) {
    char buf[140];
    snprintf(buf, sizeof(buf), "Failed, nothing saved. Axis ranges X %.2f Y %.2f Z %.2f (should be about 2.00). Check the sensor and wiring, then try again.", range[0], range[1], range[2]);
    calSay("failed", buf);
    return;
  }
  cal = c;
  EEPROM.put(0, cal);
  EEPROM.commit();
  Serial.printf("# calibration saved: offsets %.3f %.3f %.3f scales %.3f %.3f %.3f\n",
                cal.off[0], cal.off[1], cal.off[2], cal.sc[0], cal.sc[1], cal.sc[2]);
  calSay("done", "saved");
  primed = false;
  zeroNext = true;
}

// Average 0.5 s of raw readings. If the sensor stops answering (a jumper wire got knocked loose while the
// board was being turned), re-initialise the bus and the chip, then try once more.
int calSample(float &sx, float &sy, float &sz) {
  int n = 0; sx = sy = sz = 0;
  for (int i = 0; i < 100; i++) {
    float x, y, z;
    if (adxlRead(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(5);
  }
  return n;
}

void calCapture() {
  float sx, sy, sz;
  int n = calSample(sx, sy, sz);
  if (n < 50) {                                        // lost the sensor: reconnect and retry
    Serial.println("# sensor not answering, re-initialising...");
    for (int t = 0; t < 5 && !adxlBegin(); t++) delay(100);
    n = calSample(sx, sy, sz);
  }
  if (n < 50) { calSay("error", "Sensor read failed: the ADXL345 stopped answering. A wire probably came loose when you moved the board. Press each jumper (SDA, SCL, 3V3, GND) firmly into the breadboard, then press Ready again."); return; }
  float v[3] = { sx / n, sy / n, sz / n };
  // Whichever axis reads closest to +-1 g is the one pointing up or down; its sign tells which side it is.
  int a = 0;
  for (int i = 1; i < 3; i++) if (fabsf(v[i]) > fabsf(v[a])) a = i;
  bool neg = v[a] < 0;
  bool level = fabsf(fabsf(v[a]) - 1.0f) <= 0.25f;
  for (int i = 0; i < 3; i++) if (i != a && fabsf(v[i]) > 0.35f) level = false;
  Serial.printf("# capture raw x=%.3f y=%.3f z=%.3f\n", v[0], v[1], v[2]);
  if (!level) { calSay("error", "The board is not resting on a side. Hold it steady so one side points straight up or down, then try again."); return; }
  int slot = sideIndex(a, neg);
  char buf[110];
  if (calMask & (1 << slot)) {
    snprintf(buf, sizeof(buf), "That side (%s) is already captured. Rotate the board onto a different side.", SIDE[slot]);
    calSay("error", buf);
    return;
  }
  if (neg) calNeg[a] = v[a]; else calPos[a] = v[a];
  calMask |= (1 << slot);
  snprintf(buf, sizeof(buf), "Captured: %s", SIDE[slot]);
  calSay("ok", buf);
  calStep++;
  if (calStep >= 6) calFinish(); else calPrompt();
}

void led(uint16_t ms) { ledUntil = millis() + ms; }

void setup() {
  Serial.begin(BAUD);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BTN_RECENTER, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_FLASH, INPUT_PULLUP);

  while (!adxlBegin()) {
    // lines starting with '#' are ignored by the game but shown in the bridge console
    Serial.println("# ADXL345 not found. Check SDA->D2 SCL->D1 CS->3V3 SDO->GND VCC->3V3");
    for (int i = 0; i < 6; i++) { digitalWrite(PIN_LED, i & 1); delay(80); }
    delay(1000);
  }
  Serial.println("# ADXL345 OK");
  calLoad();
  Serial.println(cal.sc[0] == 1.0f && cal.off[0] == 0.0f ? "# not calibrated (hold START 3 s to calibrate)" : "# calibration loaded");
}

void loop() {
  uint32_t now = millis();

  // Feedback from the game, sent by the bridge: 'h' = cube hit, 'b' = bomb, 'z' = zero now
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'h') led(60);
    else if (c == 'b') led(400);
    else if (c == 'z') zeroNext = true;
    else if (c == 'c' && calStep < 0) calBegin();                       // page: start calibration
    else if (c == 'n' && calStep >= 0) calCapture();                    // page: capture this position
    else if (c == 'x' && calStep >= 0) { calStep = -1; calSay("cancel", "cancelled"); }  // page: cancel
  }

  // LED: slow heartbeat, pulses on game feedback / button presses
  bool on = (now < ledUntil) ? true : (((now / 1000) & 1) && (now % 1000) < 100);
  digitalWrite(PIN_LED, on ? LOW : HIGH);

  // Buttons (pull-up, pressed = LOW). Two ways to control it, both work at the same time:
  //   on-board FLASH button (D3): tap = recenter | hold ~1 s and release = start game | hold 3 s = calibrate
  //   optional external buttons : D5 = recenter  | D6 = start (hold 3 s = calibrate)
  bool bR = digitalRead(PIN_BTN_RECENTER) == LOW;
  bool bS = digitalRead(PIN_BTN_START) == LOW;
  bool bF = digitalRead(PIN_BTN_FLASH) == LOW;
  bool evTap = false, evStart = false, evCal = false;

  if (bF) {
    if (!fHeld) fHeld = now ? now : 1;
    if (!fCalDone && now - fHeld > 3000) { fCalDone = true; evCal = true; }
  } else {
    if (fHeld && !fCalDone && now - fHeld > 40) { if (now - fHeld < 800) evTap = true; else evStart = true; }
    fHeld = 0; fCalDone = false;
  }
  if (bS) {
    if (!startHeld) startHeld = now ? now : 1;
    if (!calTriggered && now - startHeld > 3000) { calTriggered = true; evCal = true; }
  } else {
    if (startHeld && !calTriggered && now - startHeld > 40) evStart = true;
    startHeld = 0; calTriggered = false;
  }

  if (calStep >= 0) {                                   // calibration mode: no game data is sent
    digitalWrite(PIN_LED, ((now / 150) & 1) ? LOW : HIGH);
    if (evTap || (bR && now - lastBtn > 400)) { lastBtn = now; calCapture(); }
    else if (evCal || evStart) { calStep = -1; calSay("cancel", "cancelled"); }
    return;
  }

  if (evCal) { calBegin(); return; }
  if (evStart) { Serial.println("{\"t\":\"start\"}"); led(80); }
  if ((evTap || bR) && now - lastBtn > 250) { zeroNext = true; lastBtn = now; Serial.println("{\"t\":\"r\"}"); led(80); }

  // Sample the sensor at ~200 Hz and low-pass it; the filter opens up when you tilt quickly.
  if (now - lastRead >= 5) {
    lastRead = now;
    float x, y, z;
    if (adxlRead(x, y, z)) {
      x = (x - cal.off[0]) * cal.sc[0];
      y = (y - cal.off[1]) * cal.sc[1];
      z = (z - cal.off[2]) * cal.sc[2];
      if (!primed) { fx = x; fy = y; fz = z; primed = true; }
      float mag = sqrtf(x * x + y * y + z * z);
      if (fabsf(mag - 1.0f) < GRAVITY_TOL) {          // only trust it as "gravity" when not mid-swing
        float d = fabsf(x - fx) + fabsf(y - fy) + fabsf(z - fz);
        float a = SMOOTH_MIN + d * SMOOTH_GAIN;
        if (a > SMOOTH_MAX) a = SMOOTH_MAX;
        fx += (x - fx) * a;
        fy += (y - fy) * a;
        fz += (z - fz) * a;
      }
    }
  }

  if (now - lastSend < SEND_MS) return;
  lastSend = now;
  if (!primed) return;
  if (!bootZeroed && now > 1500) { zeroNext = true; bootZeroed = true; }   // zero to the pose held at power-up

  float pitch = atan2f(fy, sqrtf(fx * fx + fz * fz)) * 57.29578f;   // + = nose up
  float roll  = atan2f(fx, sqrtf(fy * fy + fz * fz)) * 57.29578f;   // + = right side up (tilted left)

  if (zeroNext) { pitchOff = pitch; rollOff = roll; zeroNext = false; }

  float dp = (pitch - pitchOff) * (INVERT_PITCH ? -1.0f : 1.0f);
  float dr = (roll - rollOff) * (INVERT_YAW ? -1.0f : 1.0f);

  float elev = BASE_ELEV + dp * PITCH_GAIN;
  if (elev > 75.0f) elev = 75.0f;
  if (elev < -30.0f) elev = -30.0f;
  float yaw = -dr * YAW_GAIN;               // right side down (negative roll) => sweep right
  if (yaw > 80.0f) yaw = 80.0f;
  if (yaw < -80.0f) yaw = -80.0f;

  // Deadzone: hold the last output until the angle really changes
  if (fabsf(elev - outElev) > DEADZONE_DEG) outElev = elev;
  if (fabsf(yaw - outYaw) > DEADZONE_DEG) outYaw = yaw;
  elev = outElev; yaw = outYaw;

  // W3C deviceorientation encoding so the game's saber code is unchanged: alpha = -yaw, beta = elevation, gamma = 0
  float alpha = fmodf(-yaw + 360.0f, 360.0f);

  char buf[80];
  snprintf(buf, sizeof(buf), "{\"t\":\"o\",\"a\":%.1f,\"b\":%.1f,\"g\":0}", alpha, elev);
  Serial.println(buf);
}
