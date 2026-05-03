# Baby Piano Keys

A 4-key mini piano arcade game for ESP32 — a DIY version of the arcade classic
[Grand Piano Keys](https://www.baytekent.com/grand-piano-keys/).

Press the highlighted key before time runs out, chain correct hits for tickets,
and try to beat the high score!  All navigation works with the four physical
buttons.  A Wi-Fi web server lets you monitor the game and adjust the bonus in
real time.

---

## Files

```
BabyPianoKeys/
├── BabyPianoKeys.ino          ← Arduino sketch (ESP32 framework)
└── data/
    ├── index.html             ← Web UI (upload to SPIFFS)
    └── audio/                 ← Place WAV files here before uploading
        ├── key0.wav
        ├── key1.wav
        ├── key2.wav
        ├── key3.wav
        ├── success.wav
        └── fail.wav
```

---

## Wiring Table

| Component               | ESP32 GPIO | Notes                                    |
|-------------------------|-----------|------------------------------------------|
| Button 0 (leftmost)     | 12        | Other side to GND; `INPUT_PULLUP` used   |
| Button 1                | 13        | Other side to GND; `INPUT_PULLUP` used   |
| Button 2                | 14        | Other side to GND; `INPUT_PULLUP` used   |
| Button 3 (rightmost)    | 27        | Other side to GND; `INPUT_PULLUP` used   |
| LED 0                   | 19        | 220 Ω series resistor → GND             |
| LED 1                   | 21        | 220 Ω series resistor → GND             |
| LED 2                   | 33        | 220 Ω series resistor → GND             |
| LED 3                   | 32        | 220 Ω series resistor → GND             |
| MAX7219 DIN (MOSI)      | 23        | Hardware SPI                             |
| MAX7219 CLK (SCK)       | 18        | Hardware SPI                             |
| MAX7219 CS / LOAD       | 5         | Active-LOW chip select                   |
| TM1637 Score CLK        | 16        | Score 7-segment display                  |
| TM1637 Score DIO        | 17        | Score 7-segment display                  |
| TM1637 Time CLK         | 4         | Countdown 7-segment display              |
| TM1637 Time DIO         | 2         | Boot pin — safe after boot; change to 15 if issues |
| I2S BCK (bit clock)     | 26        | To I2S DAC/amp (e.g. MAX98357A)          |
| I2S WS (word select)    | 25        | To I2S DAC/amp                           |
| I2S DOUT (serial data)  | 22        | To I2S DAC/amp                           |
| 5 V power rail          | —         | MAX7219 chain + I2S amp                  |
| 3.3 V power rail        | —         | ESP32 (on-board regulator)               |
| GND                     | GND       | Common ground for all components         |

> **Note:** GPIO 25 & 26 are used for I2S audio.  The LEDs were therefore
> moved from the original spec (25, 26) to 19 and 21.  
> The four MAX7219 modules are daisy-chained on one SPI bus and treated
> as a single 32 × 8 display.

---

## Required Libraries

Install all via the **Arduino IDE Library Manager**:

| Library         | Author           | Notes                  |
|-----------------|------------------|------------------------|
| `MD_MAX72XX`    | majicDesigns     | MAX7219 matrix driver  |
| `TM1637`        | avishorp         | 7-segment display      |
| `ArduinoJson`   | Benoit Blanchon  | v6 or v7               |

The ESP32 Arduino core, `SPIFFS`, `WiFi`, `WebServer`, and `driver/i2s.h` are
all included with the ESP32 Arduino core — no extra installation needed.

---

## WAV File Specifications

| Setting      | Value             |
|--------------|-------------------|
| Format       | PCM (uncompressed)|
| Channels     | Mono              |
| Bit depth    | 16-bit (8-bit also supported) |
| Sample rate  | 22 050 Hz         |
| Duration     | ≤ 1 second        |

Trim silence from the start and end.  Use
[Audacity](https://www.audacityteam.org/) (free) or `ffmpeg`:

```bash
# Convert any audio file to the correct WAV format
ffmpeg -i input.mp3 -ar 22050 -ac 1 -sample_fmt s16 key0.wav
```

Place the six WAV files inside `BabyPianoKeys/data/audio/` before uploading.

---

## Uploading SPIFFS Assets

### Arduino IDE 2.x (recommended)

1. Install the **ESP32 SPIFFS Data Upload** plugin:  
   [https://github.com/me-no-dev/arduino-esp32fs-plugin](https://github.com/me-no-dev/arduino-esp32fs-plugin)
2. Restart the IDE.
3. Open `BabyPianoKeys.ino`.
4. Place all files into `BabyPianoKeys/data/` (WAV files in the `audio/` subfolder).
5. Select your board and port, then choose  
   **Tools → ESP32 Sketch Data Upload**.

### PlatformIO

Add to `platformio.ini`:

```ini
board_build.filesystem = spiffs
```

Then run:

```bash
pio run --target uploadfs
```

### Fallback (no WAV files)

If WAV files are missing the sketch automatically generates square-wave tones
via I2S as a fallback — the game is fully playable without any WAV files.

---

## Menu Navigation (4 buttons only)

All navigation uses only the four physical buttons.  Each MAX7219 matrix
displays a label showing what its corresponding button does.

### Main Menu

| Button / Matrix | Action          | Display shows |
|-----------------|-----------------|---------------|
| 0 (left)        | **Start game**  | `Go`          |
| 1               | Time options    | `Ti`          |
| 2               | Difficulty      | `Di`          |
| 3 (right)       | High scores     | `Hi`          |

### Time Submenu (press 1 from main menu)

Pressing any button selects that duration and returns to the main menu.

| Button | Duration |
|--------|----------|
| 0      | 30 s     |
| 1      | 45 s     |
| 2      | 60 s     |
| 3      | 75 s     |

### Difficulty Submenu (press 2 from main menu)

| Button | Difficulty | Note-show delay |
|--------|------------|-----------------|
| 0      | Easy       | 600 ms          |
| 1      | Normal     | 350 ms          |
| 2      | Hard       | 150 ms          |
| 3      | ← Back     | —               |

### High-Score Screen (press 3 from main menu)

- Cycles through the top-3 scores automatically.
- Press **any button** to return to the main menu.
- Hold **all 4 buttons** for **≥ 15 seconds** to clear all saved high scores.

### Initial Entry (new high score)

- Buttons **0 / 1 / 2** cycle A → Z for the corresponding initial letter.
- Button **3** confirms and saves.

---

## Gameplay

1. Select time and difficulty from the menu, then press **Start (Button 0)**.
2. A 3-second countdown appears on the time display.
3. The 32 × 8 matrix shows a scrolling "piano roll":
   - **Bottom row** = key to press right now.
   - Rows above = upcoming notes (preview).
4. Press the **matching button** before time runs out.
   - ✅ Correct → score +1, brief flash, next note appears.
   - ❌ Wrong → fail flash, game ends immediately.
5. Every **3 correct notes** awards **1 ticket**.
6. At game end, final score = notes hit + bonus value.
7. Top-3 scores are saved to flash (NVS) across power cycles.

---

## Web Interface

By default the ESP32 creates a Wi-Fi access point:

| Setting  | Value              |
|----------|--------------------|
| SSID     | `BabyPiano`        |
| Password | `piano1234`        |
| URL      | http://192.168.4.1 |

To join an existing network instead, edit `WIFI_AP_MODE`, `WIFI_STA_SSID`,
and `WIFI_STA_PASS` near the top of the sketch.

### Endpoints

| Method | Path       | Description                                           |
|--------|------------|-------------------------------------------------------|
| GET    | `/`        | Web UI (served from SPIFFS `/index.html`)             |
| GET    | `/status`  | JSON: state, score, tickets, time, bonus, events      |
| GET    | `/debug`   | HTML: verbose internal variables + recent event log   |
| POST   | `/bonus`   | JSON body `{"bonus": 50}` — sets the bonus value      |
| POST   | `/command` | JSON body `{"command": "reset"\|"start"\|"stop"}`     |

---

## Customising the Sketch

All user-adjustable values are grouped at the top of `BabyPianoKeys.ino`
under clearly labelled `// ← edit` sections:

- **Pin defines** — remap any GPIO
- **WiFi settings** — credentials / AP vs STA mode
- **Game constants** — debounce time, note delays, animation durations
- **Audio paths** — rename WAV files if needed
- **`// FUTURE HOOK` comments** — placeholder spots for:
  - Remote high-score syncing
  - Ticket redemption endpoint
  - Persistent leaderboard

---

## Credits

Inspired by [tasty-cakes1/piano-game](https://github.com/tasty-cakes1/piano-game)
(Arduino Micro implementation).  Key ideas adapted: piano-roll matrix display,
`printText`/`scrollText` using MD_MAX72XX built-in font, and the initial-entry
interaction model.
