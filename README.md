# Horizon Blade: ESP8266 + ADXL345 tilt-saber controller

```
ADXL345 --I2C--> ESP8266 --WiFi/WebSocket--> bridge/server.js (PC) --WebSocket--> game page (localhost:5173)
```

The game sees the ESP as one more player (P1..P4), next to phones and the mouse player.

## Setup
This repo is the hardware side. The game itself is https://github.com/A7medAmine/saber-game (its `main` already contains the ESP hook and the calibrate button).
```
git clone https://github.com/A7medAmine/saber-game
git clone https://github.com/A7medAmine/saber-game-eps-bridge
cd saber-game-eps-bridge/bridge
npm install
node server.js --serial COM3      # Linux: --serial /dev/ttyUSB0 ; WiFi controllers: just `node server.js`
```
The bridge serves `../../saber-game` by default (a sibling folder); use `--dir <path>` for another location.
Layout: `bridge/` (server), `esp8266_saber_usb/` (USB firmware, recommended), `esp8266_saber/` (WiFi firmware).

## USB version (recommended: no WiFi needed)
Use `esp8266_saber_usb/esp8266_saber_usb.ino` instead of the WiFi sketch. Same wiring, **no extra Arduino library**.
1. Upload the sketch, then **close the Serial Monitor** (only one program can use the COM port).
2. Find the port: Arduino IDE Tools > Port, or `npm.cmd run ports` in `bridge/`.
3. In `bridge/` run `node server.js --serial COM3` (replace COM3 with yours), or `npm.cmd run usb` for COM3.
4. Open http://localhost:5173. The console shows `[usb] COM3 open`, then `[ctrl] usb1 connected`; P1 appears in the game.
Stop the bridge before uploading a new sketch (it holds the port). Lines starting with `#` from the ESP (like `# ADXL345 OK`) are printed in the bridge console.
The WiFi sketch below still works and both can run together.

## Buttons (USB sketch)
You do **not** need to wire buttons. The NodeMCU's on-board **FLASH** button (next to the USB port) does everything:
- **tap** = recenter (do it holding the board level)
- **hold about 1 second, then release** = start game / play again
- **hold 3 seconds** = calibration (see below)
(The RST button only resets the board and can't be used.) Optional external buttons still work: D5-GND = recenter, D6-GND = start (hold 3 s = calibrate). Don't hold FLASH while plugging the board in or powering it up.

## 3D setup page (easiest way to calibrate)
With the bridge running, open **http://localhost:5173/setup** (also linked from the game). It shows a live 3D model of your controller that follows the real board, and a second model showing how to hold it for each calibration step. When the board is in position and still for 1.5 s it captures by itself (untick auto-capture to use the Capture now button). It has Quick (1 position) and Full (6 positions) calibration, Recenter, and tells you in words if the controller or sensor data is missing. It needs the newest `esp8266_saber_usb` sketch (it sends the live sensor values).

## Quick calibration (from the game page)
In the game page click **Quick calibrate (lay flat)**: put the controller flat on the table, ADXL345 chip facing the ceiling, click Ready and don't touch it for a second. This zeroes the X/Y offsets, which is what matters most for a tilt stick. The full 6-position calibration below is optional and also fixes per-axis scale.

## Calibration (USB sketch, 6 positions, optional)
Cheap ADXL345 boards are often a few percent off on each axis, which makes the saber inaccurate. The sketch has a built-in 6-position calibration that is saved in the ESP's flash.
**Easiest: from the game page.** With the bridge running and the controller connected, open the game and click **Calibrate controller** in the lobby. It shows each position, counts down 3 s, then captures. (Or press N to capture.)

Or with the board's own button:
1. Run the bridge with `--serial COM3` (messages appear in its console), or use the Arduino Serial Monitor instead of the bridge.
2. **Hold the FLASH button (or START) for 3 seconds.** The LED blinks fast.
3. Rest the board still on one of its six sides, **in any order** (no need to match arrow names exactly: the board detects which side is facing up), then **tap FLASH** (or press RECENTER / click Ready on the page). It reports the side it captured and which are still needed. The six sides are: X arrow up, X arrow down, Y arrow up, Y arrow down, chip face up, chip face down. The arrows are printed on the ADXL345 board (in the usual photo, with the pin header on the right: X points to the top of the board and Y to the left).
   - Use a book, box corner or table edge to hold the board upright; the sides must be nearly vertical or flat. A tilted board or a side that was already captured is rejected with a message.
4. After the sixth side it reports `saved`, or `Failed` with the measured axis ranges (each should be about 2.00; if one is far off, check the wiring and that the sensor is solid). Holding FLASH for 1 second or more cancels.
It is remembered after power-off. Re-run it any time.

## Important: the ADXL345 is not a gyroscope
It is a 3-axis **accelerometer**. It measures tilt (pitch and roll) from gravity, but it cannot measure
rotation around the vertical axis (yaw) and it gets noisy during fast swings. So this controller works as a
**tilt stick**:

| Move the controller | Blade |
|---|---|
| tilt nose up / down | blade goes up / down |
| tilt like a steering wheel (right side down) | blade sweeps right (left side down = left) |

For a "real" saber (any direction, fast swings, no drift) swap the sensor for an **MPU6050 / MPU9250** later.
The bridge and game hook stay identical; only the firmware math changes.

## Parts
- ESP8266 board (NodeMCU or Wemos D1 mini), ADXL345 breakout (GY-291), jumper wires (push buttons are optional), USB cable
- Optional: LiPo + charger/boost module for wireless use

## Pinout

| ADXL345 (GY-291) | ESP8266 pin | GPIO |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SDA | D2 | GPIO4 |
| SCL | D1 | GPIO5 |
| CS | 3V3 (selects I2C mode, required) | |
| SDO | GND (I2C address 0x53) | |
| INT1 / INT2 | not connected | |

| Button | ESP8266 pin | Other leg |
|---|---|---|
| RECENTER | D5 (GPIO14) | GND |
| START / PLAY AGAIN | D6 (GPIO12) | GND |

Built-in LED (D4): solid = connected to the bridge, slow blink = looking for it, flicker = you hit a cube, long = bomb.
Power the ADXL345 from **3.3 V**, never 5 V.

Mounting: sensor flat on the handle, X to the right, Y pointing toward the screen, Z up. If the blade moves the wrong way, set `INVERT_YAW` / `INVERT_PITCH` in the sketch.

## Build steps
1. Wire everything as above (power off). Double-check CS->3V3 and SDO->GND.
2. Arduino IDE: add ESP8266 boards (Boards Manager, "esp8266 by ESP8266 Community").
   Library Manager: install **WebSockets** by Markus Sattler.
3. Open `esp8266_saber/esp8266_saber.ino`, set `WIFI_SSID`, `WIFI_PASS`, `PC_HOST` (step 5 shows how to find it).
4. Pick your board and COM port, Upload. Open Serial Monitor at 115200.
5. On the PC: install Node.js, then in `bridge/` run `npm install` once and `npm start`. It prints the PC's IP addresses; put the one on the same network as the ESP into `PC_HOST` and re-upload.
   (No router? Turn on the Windows **Mobile hotspot**, join the ESP to it, and use `192.168.137.1` as `PC_HOST`.)
6. Allow **Node.js** through Windows Firewall (Private networks) when the prompt appears; the ESP cannot connect otherwise.
7. Open http://localhost:5173 on the PC (the game page must be served by the bridge, not by another server).

## How to test (in this order)
1. **Sensor**: Serial Monitor must show `ADXL345 OK`, then lines like `pitch=.. roll=..`. Tilt the board: pitch changes when the nose goes up/down, roll when it leans sideways. Flat on the table: `g=(0.00 0.00 1.00)`.
   If you see `ADXL345 not found`, recheck wiring (SDA/SCL swapped, CS not on 3V3).
2. **WiFi**: Serial shows `ESP IP: ...` then `[ws] connected to bridge`; the bridge console prints `[ctrl] esp1 connected` and every 5 s a msg/s line (~50).
3. **Game**: with http://localhost:5173 open, a **P1** player appears in the lobby ("P1 joined") and the top-left net HUD shows `~50 Hz`. Tilt the board, the saber follows. Press the RECENTER button with the board level.
4. **Play**: press the START button (or Enter on the PC). Cutting a cube flickers the ESP LED; a bomb gives a long flash.
5. **No hardware yet?** Test the pipeline with a fake controller: with the bridge running, run in another terminal
   `node tools/fake_esp.js` (sweeps the saber left and right).

## Troubleshooting
| Symptom | Fix |
|---|---|
| `[ws] disconnected` forever | wrong `PC_HOST`, bridge not running, firewall blocking Node, ESP and PC on different networks (guest WiFi isolates devices) |
| Saber jitters | lower `SMOOTH` (0.15), hold the board still when pressing RECENTER |
| Saber drifts after a swing | normal for an accelerometer; press RECENTER |
| Blade goes the wrong way | flip `INVERT_YAW` / `INVERT_PITCH` |
| Too much wrist movement needed | raise `YAW_GAIN` |
| Phones still need HTTPS | the phone controller is unchanged; deploy as before. The bridge only serves the local ESP path |
