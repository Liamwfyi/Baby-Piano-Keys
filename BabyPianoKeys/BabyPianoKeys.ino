/**
 * BabyPianoKeys.ino
 * =================
 * ESP32 Mini Piano Arcade — 4-key timed challenge with web control.
 *
 * Inspired by tasty-cakes1/piano-game (Arduino Micro / Adafruit 7-seg).
 * Adapted for ESP32: I2S audio, SPIFFS WAV, WiFi web server, TM1637 7-seg,
 * non-blocking state machine, Preferences-based high-score storage.
 *
 * ═══════════════════════════ WIRING TABLE ═══════════════════════════
 * Component               | ESP32 GPIO | Notes
 * ------------------------|------------|--------------------------------
 * Button 0  (leftmost)    |  12        | To GND; INPUT_PULLUP enabled
 * Button 1                |  13        | To GND; INPUT_PULLUP enabled
 * Button 2                |  14        | To GND; INPUT_PULLUP enabled
 * Button 3  (rightmost)   |  27        | To GND; INPUT_PULLUP enabled
 * LED 0                   |  19        | 220 Ω series → GND
 * LED 1                   |  21        | 220 Ω series → GND
 * LED 2                   |  33        | 220 Ω series → GND
 * LED 3                   |  32        | 220 Ω series → GND
 * MAX7219 DIN  (MOSI)     |  23        | Hardware SPI
 * MAX7219 CLK  (SCK)      |  18        | Hardware SPI
 * MAX7219 CS / LOAD       |   5        | Active-LOW
 * TM1637 Score CLK        |  16        | Score 7-seg display
 * TM1637 Score DIO        |  17        | Score 7-seg display
 * TM1637 Time  CLK        |   4        | Countdown 7-seg display
 * TM1637 Time  DIO        |   2        | Boot pin — safe after boot
 * I2S BCK  (bit clock)    |  26        | To I2S DAC / amp (e.g. MAX98357A)
 * I2S WS   (word select)  |  25        | To I2S DAC / amp
 * I2S DOUT (serial data)  |  22        | To I2S DAC / amp
 * 5 V rail                |  —         | MAX7219 chain + I2S amp
 * 3.3 V rail              |  —         | ESP32 (on-board regulator)
 * GND                     |  GND       | Common ground for all
 *
 * NOTE: GPIO 2 is a strapping pin; it works fine as I/O after boot.
 *       If boot fails, change TM_TIME_DIO to another free GPIO (e.g. 15).
 * NOTE: GPIO 25 & 26 are I2S lines; LEDs moved from spec pins to 19 & 21.
 * NOTE: MAX7219 chain — four 8×8 modules daisy-chained on one SPI bus,
 *       treated as a single 32×8 display (device 0 = leftmost / key 0).
 * ════════════════════════════════════════════════════════════════════
 *
 * MENU BUTTON MAPPING (4 buttons, 4 matrices)
 * ─────────────────────────────────────────────
 * Each matrix shows what its button does in the current context.
 *
 *  Main menu:    [0] START    [1] TIME     [2] DIFF     [3] HIGH-SCORE
 *  Time submenu: [0] 30 s     [1] 45 s     [2] 60 s     [3] 75 s
 *  Diff submenu: [0] EASY     [1] NORM     [2] HARD     [3] BACK
 *  High scores:  any button → return to main menu
 *  Initials:     [0-2] cycle A-Z for each letter;  [3] confirm
 *
 * REQUIRED LIBRARIES (install via Arduino Library Manager)
 * ─────────────────────────────────────────────────────────
 *   MD_MAX72XX  by majicDesigns   https://github.com/MajicDesigns/MD_MAX72XX
 *   TM1637      by avishorp       https://github.com/avishorp/TM1637
 *   ArduinoJson by Benoit Blanchon https://arduinojson.org  (v6 or v7)
 */

// ════════════════════════════════════════════════════════════════════
//  INCLUDES
// ════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <Preferences.h>          // NVS high-score storage
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>          // JSON for web API
#include <MD_MAX72xx.h>           // MAX7219 matrix driver
#include <SPI.h>
#include <TM1637Display.h>        // TM1637 7-segment driver
#include "driver/i2s.h"           // ESP32 I2S peripheral

// Storage backend — uncomment USE_SD_CARD to switch from SPIFFS to SD
// #define USE_SD_CARD
#ifdef USE_SD_CARD
  #include <SD.h>
  #define FS_OBJ   SD
  #define SD_CS    15
#else
  #include <SPIFFS.h>
  #define FS_OBJ   SPIFFS
#endif

// ════════════════════════════════════════════════════════════════════
//  PIN DEFINITIONS  ← edit to match your wiring
// ════════════════════════════════════════════════════════════════════
constexpr uint8_t PIN_BTN[4] = { 12, 13, 14, 27 }; // active-LOW, INPUT_PULLUP
constexpr uint8_t PIN_LED[4] = { 19, 21, 33, 32 }; // active-HIGH, 220 Ω → GND
#define PIN_MAX_CS    5   // MAX7219 LOAD / CS (hardware SPI: MOSI=23, SCK=18)
#define TM_SCORE_CLK 16
#define TM_SCORE_DIO 17
#define TM_TIME_CLK   4
#define TM_TIME_DIO   2   // GPIO 2 — OK after boot; change if boot issues
#define I2S_BCK      26
#define I2S_WS       25
#define I2S_DOUT     22

// ════════════════════════════════════════════════════════════════════
//  WIFI SETTINGS  ← edit credentials or set AP_MODE false to join net
// ════════════════════════════════════════════════════════════════════
constexpr bool  WIFI_AP_MODE  = true;
constexpr char  WIFI_AP_SSID[] = "BabyPiano";   // hotspot SSID
constexpr char  WIFI_AP_PASS[] = "piano1234";   // hotspot password (≥8 chars)
constexpr char  WIFI_STA_SSID[] = "";            // set to join existing network
constexpr char  WIFI_STA_PASS[] = "";

// ════════════════════════════════════════════════════════════════════
//  GAME CONSTANTS  ← tune here
// ════════════════════════════════════════════════════════════════════
constexpr int  NUM_KEYS         =  4;
constexpr int  DEBOUNCE_MS      = 50;
constexpr int  NOTES_PER_TICKET =  3;   // correct presses to earn 1 ticket
constexpr int  KEY_QUEUE_LEN    =  8;   // notes visible on the piano-roll
constexpr int  BLINK_MS         = 250;  // LED blink half-period (ms)
constexpr int  SUCCESS_ANIM_MS  = 500;  // brief correct-press flash
constexpr int  FAIL_FLASH_MS    = 1200; // wrong-key flash duration
constexpr int  SCORE_SHOW_MS    = 6000; // score screen duration before menu
constexpr int  COUNTDOWN_STEP   = 1000; // countdown tick (ms)
constexpr int  SCROLL_DELAY_MS  = 80;   // scrollText column delay
constexpr int  NUM_HIGH_SCORES  =  3;   // top-N stored

// Inter-note show delay by difficulty (ms before input is accepted)
//   Easy = longer look time, Hard = react immediately
constexpr int NOTE_DELAY[3]  = { 600, 350, 150 }; // easy / normal / hard

// Selectable game durations (seconds)
constexpr int GAME_TIMES[4]  = { 30, 45, 60, 75 };
constexpr int NUM_GAME_TIMES =  4;

// Fallback tone frequencies when WAV file missing (C4 E4 G4 C5)
constexpr int FALLBACK_FREQ[4] = { 262, 330, 392, 523 };
constexpr int FALLBACK_DUR_MS  = 180;

// ════════════════════════════════════════════════════════════════════
//  SPIFFS AUDIO PATHS  ← match your uploaded WAV filenames
//  Recommend: PCM, Mono, 16-bit, 22050 Hz, trimmed to ≤1 s
// ════════════════════════════════════════════════════════════════════
const char* AUDIO_KEY[4]  = { "/audio/key0.wav", "/audio/key1.wav",
                               "/audio/key2.wav", "/audio/key3.wav" };
const char* AUDIO_SUCCESS = "/audio/success.wav";
const char* AUDIO_FAIL    = "/audio/fail.wav";

// ════════════════════════════════════════════════════════════════════
//  HARDWARE OBJECTS
// ════════════════════════════════════════════════════════════════════
// 4 × 8×8 matrices chained as a single 32×8 display on hardware SPI.
// Device 0 = first module data reaches (closest to CS, leftmost visually
// for FC16_HW). Swap device numbers in printKeys/printText if your chain
// is physically reversed.
MD_MAX72xx mx(MD_MAX72XX::FC16_HW, PIN_MAX_CS, 4);

TM1637Display dispScore(TM_SCORE_CLK, TM_SCORE_DIO);
TM1637Display dispTime (TM_TIME_CLK,  TM_TIME_DIO);

WebServer      httpSrv(80);
Preferences    prefs;

// ════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ════════════════════════════════════════════════════════════════════
enum class State : uint8_t {
  BOOT,           // power-on animation
  MENU,           // top-level button menu (time / diff / high scores)
  SHOW_HIGH_SCORES, // cycle top-3 scores; any key → MENU
  COUNTDOWN,      // 3-2-1 before game starts
  SHOW_SEQUENCE,  // highlight next note for NOTE_DELAY[diff] ms (no input)
  WAIT_INPUT,     // accept button presses; track remaining time
  SUCCESS,        // correct key flash + audio
  FAIL,           // wrong key flash + audio → game over
  SCORE_DISPLAY,  // show final score; check high score
  ENTER_INITIALS  // new high-score initial entry
};

State         gs      = State::BOOT;
unsigned long stateMs = 0; // millis() when current state was entered

const char* stateStr(State s) {
  switch (s) {
    case State::BOOT:             return "BOOT";
    case State::MENU:             return "MENU";
    case State::SHOW_HIGH_SCORES: return "HIGH_SCORES";
    case State::COUNTDOWN:        return "COUNTDOWN";
    case State::SHOW_SEQUENCE:    return "SHOW_SEQUENCE";
    case State::WAIT_INPUT:       return "WAIT_INPUT";
    case State::SUCCESS:          return "SUCCESS";
    case State::FAIL:             return "FAIL";
    case State::SCORE_DISPLAY:    return "SCORE_DISPLAY";
    case State::ENTER_INITIALS:   return "ENTER_INITIALS";
  }
  return "UNKNOWN";
}

// ════════════════════════════════════════════════════════════════════
//  GAME VARIABLES
// ════════════════════════════════════════════════════════════════════
int     score         = 0;
int     tickets       = 0;
int     bonusValue    = 0;   // adjustable via web UI or future menu
int     difficulty    = 1;   // 0=easy  1=normal  2=hard
int     gameTimeSec   = 30;  // selected duration (seconds)
int     timeRemaining = 0;   // live countdown (seconds)

uint8_t keyQueue[KEY_QUEUE_LEN]; // circular buffer of upcoming notes
int     keyQueueHead  = 0;       // index of next note to press

// High-score table (loaded from / saved to NVS)
unsigned int hsScore[NUM_HIGH_SCORES]    = { 0, 0, 0 };
char         hsInitials[NUM_HIGH_SCORES][4] = { "---", "---", "---" };

// Menu navigation
int  menuLevel = 0; // 0=main  1=time submenu  2=diff submenu

// Countdown state
int  countdownVal = 3;

// Shared game clock — set once when the countdown ends;
// used by SHOW_SEQUENCE, WAIT_INPUT, and SUCCESS to track elapsed time.
unsigned long gameClockMs = 0;

// ════════════════════════════════════════════════════════════════════
//  DEBUG LOG
// ════════════════════════════════════════════════════════════════════
constexpr int LOG_SIZE = 20;
String   logBuf[LOG_SIZE];
int      logHead = 0;

void logEv(const char* msg) {
  logBuf[logHead % LOG_SIZE] = String(millis()) + "ms " + msg;
  logHead++;
  Serial.println(msg);
}

void logEvf(const char* fmt, ...) {
  char tmp[96];
  va_list a;
  va_start(a, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, a);
  va_end(a);
  logEv(tmp);
}

// ════════════════════════════════════════════════════════════════════
//  BUTTON DEBOUNCE
// ════════════════════════════════════════════════════════════════════
struct Btn { uint8_t pin; bool raw, stable; unsigned long debMs; bool fired; };
Btn btn[NUM_KEYS];

void initButtons() {
  for (int i = 0; i < NUM_KEYS; i++) {
    btn[i] = { PIN_BTN[i], HIGH, HIGH, 0, false };
    pinMode(PIN_BTN[i], INPUT_PULLUP);
  }
}

// Call every loop — sets btn[i].fired=true for exactly one cycle on press edge
void pollButtons() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_KEYS; i++) {
    bool r = digitalRead(btn[i].pin);
    if (r != btn[i].raw) { btn[i].raw = r; btn[i].debMs = now; }
    btn[i].fired = false;
    if ((now - btn[i].debMs) >= DEBOUNCE_MS && r != btn[i].stable) {
      btn[i].stable = r;
      if (r == LOW) btn[i].fired = true; // falling edge = press
    }
  }
}

// Returns index of the first pressed button this cycle, or -1
int anyPressed() {
  for (int i = 0; i < NUM_KEYS; i++) if (btn[i].fired) return i;
  return -1;
}

// Blocking read: waits until all buttons are released
void waitAllReleased() {
  bool any = true;
  while (any) {
    any = false;
    for (int i = 0; i < NUM_KEYS; i++) if (digitalRead(btn[i].pin) == LOW) any = true;
    delay(10);
  }
  delay(20);
}

// ════════════════════════════════════════════════════════════════════
//  LED HELPERS
// ════════════════════════════════════════════════════════════════════
void initLeds() {
  for (int i = 0; i < NUM_KEYS; i++) { pinMode(PIN_LED[i], OUTPUT); digitalWrite(PIN_LED[i], LOW); }
}
void setLed(int i, bool on) { if (i >= 0 && i < 4) digitalWrite(PIN_LED[i], on ? HIGH : LOW); }
void allLedsOff()            { for (int i = 0; i < 4; i++) setLed(i, false); }
void allLedsOn()             { for (int i = 0; i < 4; i++) setLed(i, true);  }

// ════════════════════════════════════════════════════════════════════
//  7-SEGMENT HELPERS
// ════════════════════════════════════════════════════════════════════
void showScore(int v)    { dispScore.showNumberDec(v, false); }
void showTime(int v)     { dispTime.showNumberDec(v, false);  }

void showScoreDash() {
  const uint8_t d = SEG_G;
  uint8_t s[4] = { d, d, d, d };
  dispScore.setSegments(s);
}

// ════════════════════════════════════════════════════════════════════
//  MATRIX DISPLAY — 4 modules treated as single 32×8 strip
//  (Inspired by tasty-cakes1/piano-game printKeys approach)
// ════════════════════════════════════════════════════════════════════
// Column spacing: each key occupies a 5-pixel-wide bar separated by
// 4 blank columns → key k starts at column k*9.
//   Key 0: cols  0– 4
//   Key 1: cols  9–13
//   Key 2: cols 18–22
//   Key 3: cols 27–31
constexpr int KEY_COL_STRIDE = 9;
constexpr int KEY_BAR_WIDTH  = 5;
constexpr int CHAR_SPACING   = 1;  // pixels between font characters

// Render the 8-note piano-roll queue onto the 32×8 display.
// Row 7 (bottom) = next note to press; row 0 (top) = 7th upcoming note.
// Adapted from ArcadePiano::printKeys().
void printKeys() {
  mx.clear();
  for (int i = 0; i < KEY_QUEUE_LEN; i++) {
    int noteKey = keyQueue[(keyQueueHead + i) % KEY_QUEUE_LEN];
    int baseCol  = noteKey * KEY_COL_STRIDE;
    for (int j = 0; j < KEY_BAR_WIDTH; j++) {
      mx.setPoint(7 - i, baseCol + j, true);
    }
  }
}

// Print text into a range of matrix devices using the built-in font.
// Adapted from ArcadePiano::printText().
void printText(uint8_t modStart, uint8_t modEnd, const char* pMsg) {
  constexpr uint8_t COL_SIZE = 8;
  constexpr uint8_t ROW_SIZE = 8;
  uint8_t   state = 0, curLen;
  uint16_t  showLen;
  uint8_t   cBuf[8];
  int16_t   col = ((modEnd + 1) * COL_SIZE) - 1;

  mx.control(modStart, modEnd, MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);

  do {
    switch (state) {
      case 0: // load next character
        if (*pMsg == '\0') { showLen = col - (modEnd * COL_SIZE); state = 2; break; }
        showLen = mx.getChar(*pMsg++, sizeof(cBuf) / sizeof(cBuf[0]), cBuf);
        curLen  = 0;
        state++;
        // fall through
      case 1: // display font columns
        mx.setColumn(col--, cBuf[curLen++]);
        if (curLen == showLen) { showLen = CHAR_SPACING; state = 2; }
        break;
      case 2: curLen = 0; state++; // fall through
      case 3: // inter-character spacing
        mx.setColumn(col--, 0);
        curLen++;
        if (curLen == showLen) state = 0;
        break;
      default: col = -1;
    }
  } while (col >= (modStart * COL_SIZE));

  mx.control(modStart, modEnd, MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

// Scroll text across all 4 modules (briefly blocking; ≤ ~2 s for short strings).
// Adapted from ArcadePiano::scrollText().
void scrollText(const char* p) {
  uint8_t charWidth, cBuf[8];
  mx.clear();
  while (*p != '\0') {
    charWidth = mx.getChar(*p++, sizeof(cBuf) / sizeof(cBuf[0]), cBuf);
    for (uint8_t i = 0; i <= charWidth; i++) {
      mx.transform(MD_MAX72XX::TSL);
      if (i < charWidth) mx.setColumn(0, cBuf[i]);
      delay(SCROLL_DELAY_MS);
    }
  }
}

// Show a single number (0–9999) centred across all 4 modules using printText.
void printNumber(unsigned int val) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", val);
  // Right-align in the 4-module space by left-padding with spaces
  char padded[8] = "    "; // 4 spaces
  int len = strlen(buf);
  if (len < 4) { strncpy(padded + (4 - len), buf, len + 1); } else { strncpy(padded, buf, 5); }
  mx.clear();
  printText(0, 3, padded);
}

// ════════════════════════════════════════════════════════════════════
//  I2S AUDIO SETUP
// ════════════════════════════════════════════════════════════════════
#define I2S_PORT I2S_NUM_0
#define I2S_SR   22050

void setupI2S() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = I2S_SR;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 512;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCK;
  pins.ws_io_num    = I2S_WS;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_PORT, &pins);
}

// ════════════════════════════════════════════════════════════════════
//  AUDIO TASK  — runs on Core 0; game logic stays on Core 1
//  Queue a path like "/audio/key0.wav" or "tone:262:180" for a fallback.
// ════════════════════════════════════════════════════════════════════
struct AudioReq { char path[64]; };
QueueHandle_t audioQ;

// Stream a WAV file (PCM, mono, 8- or 16-bit) to I2S.
void playWav(const char* path) {
  File f = FS_OBJ.open(path, "r");
  if (!f) { logEvf("WAV missing: %s", path); return; }

  uint8_t hdr[44];
  if (f.read(hdr, 44) < 44 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
    f.close(); logEvf("Bad WAV header: %s", path); return;
  }
  uint32_t sr  = hdr[24] | ((uint32_t)hdr[25] << 8) | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
  uint16_t bps = hdr[34] | ((uint16_t)hdr[35] << 8);

  if (sr != I2S_SR) i2s_set_sample_rates(I2S_PORT, sr);

  static uint8_t buf[512];
  size_t written;
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (bps == 8) {
      // Convert 8-bit unsigned PCM → 16-bit signed
      int16_t s16[256];
      for (int i = 0; i < n && i < 256; i++) s16[i] = ((int16_t)buf[i] - 128) << 8;
      i2s_write(I2S_PORT, s16, n * 2, &written, portMAX_DELAY);
    } else {
      i2s_write(I2S_PORT, buf, n, &written, portMAX_DELAY);
    }
  }
  f.close();
  if (sr != I2S_SR) i2s_set_sample_rates(I2S_PORT, I2S_SR);
}

// Generate a square-wave tone (fallback when WAV unavailable).
void playTone(int freq, int durMs) {
  if (freq <= 0) return;
  int spc = I2S_SR / freq;
  int tot = (I2S_SR / 1000) * durMs;
  static int16_t buf[256];
  size_t written;
  for (int pos = 0; pos < tot; ) {
    int chunk = min((int)(sizeof(buf) / 2), tot - pos);
    for (int i = 0; i < chunk; i++)
      buf[i] = ((pos + i) % spc < spc / 2) ? 8000 : -8000;
    i2s_write(I2S_PORT, buf, chunk * 2, &written, portMAX_DELAY);
    pos += chunk;
  }
}

// Audio task entry point (pinned to Core 0).
void audioTaskFn(void*) {
  AudioReq req;
  for (;;) {
    if (xQueueReceive(audioQ, &req, portMAX_DELAY) == pdTRUE) {
      if (strncmp(req.path, "tone:", 5) == 0) {
        int f = 0, d = 0;
        sscanf(req.path + 5, "%d:%d", &f, &d);
        playTone(f, d);
      } else {
        playWav(req.path);
      }
    }
  }
}

// Queue a WAV path or "tone:FREQ:DUR_MS" — non-blocking; drops if queue full.
void queueAudio(const char* path) {
  AudioReq req;
  strncpy(req.path, path, sizeof(req.path) - 1);
  req.path[sizeof(req.path) - 1] = '\0';
  xQueueSend(audioQ, &req, 0);
}

void playKeyAudio(int key) {
  if (key < 0 || key >= NUM_KEYS) return;
  if (FS_OBJ.exists(AUDIO_KEY[key])) {
    queueAudio(AUDIO_KEY[key]);
  } else {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "tone:%d:%d", FALLBACK_FREQ[key], FALLBACK_DUR_MS);
    queueAudio(tmp);
  }
}
void playSuccessAudio() {
  if (FS_OBJ.exists(AUDIO_SUCCESS)) queueAudio(AUDIO_SUCCESS); else queueAudio("tone:784:300");
}
void playFailAudio() {
  if (FS_OBJ.exists(AUDIO_FAIL)) queueAudio(AUDIO_FAIL); else queueAudio("tone:196:500");
}

// ════════════════════════════════════════════════════════════════════
//  HIGH SCORES  (Preferences / NVS)
// ════════════════════════════════════════════════════════════════════
void loadHighScores() {
  prefs.begin("bpk", true); // read-only
  for (int i = 0; i < NUM_HIGH_SCORES; i++) {
    char kScore[8], kInit[8];
    snprintf(kScore, sizeof(kScore), "hs%d", i);
    snprintf(kInit,  sizeof(kInit),  "hi%d", i);
    hsScore[i] = prefs.getUInt(kScore, 0);
    String ini = prefs.getString(kInit, "---");
    strncpy(hsInitials[i], ini.c_str(), 3);
    hsInitials[i][3] = '\0';
  }
  prefs.end();
}

void saveHighScores() {
  prefs.begin("bpk", false); // read-write
  for (int i = 0; i < NUM_HIGH_SCORES; i++) {
    char kScore[8], kInit[8];
    snprintf(kScore, sizeof(kScore), "hs%d", i);
    snprintf(kInit,  sizeof(kInit),  "hi%d", i);
    prefs.putUInt(kScore, hsScore[i]);
    prefs.putString(kInit, hsInitials[i]);
  }
  prefs.end();
}

void clearHighScores() {
  for (int i = 0; i < NUM_HIGH_SCORES; i++) {
    hsScore[i] = 0;
    strncpy(hsInitials[i], "---", 4);
  }
  saveHighScores();
  logEv("High scores cleared");
}

// Insert a new score into the sorted high-score table; returns rank (0-based) or -1.
// Adapted from ArcadePiano::startGame() score insertion logic.
int insertHighScore(unsigned int newScore, const char* ini) {
  if (newScore <= hsScore[NUM_HIGH_SCORES - 1]) return -1;
  // Shift scores down from insertion point
  for (int i = NUM_HIGH_SCORES - 1; i >= 0; i--) {
    if (i == 0 || newScore <= hsScore[i - 1]) {
      hsScore[i] = newScore;
      strncpy(hsInitials[i], ini, 3);
      hsInitials[i][3] = '\0';
      saveHighScores();
      return i;
    }
    hsScore[i]    = hsScore[i - 1];
    memcpy(hsInitials[i], hsInitials[i - 1], 4);
  }
  return -1;
}

// ════════════════════════════════════════════════════════════════════
//  INITIAL ENTRY (blocking — brief user interaction)
//  Buttons 0/1/2 cycle A–Z for three letters; Button 3 confirms.
//  Inspired directly by ArcadePiano::getInitials().
// ════════════════════════════════════════════════════════════════════
void getInitials(char* buf) {
  mx.clear();
  buf[0] = buf[1] = buf[2] = 'A';
  buf[3] = '\0';

  // Show arrow pointing left on all matrices as prompt
  char arrow[2] = { char(16), '\0' };
  printText(0, 3, arrow);

  bool done = false;
  while (!done) {
    // Debounce manually here (blocking context)
    bool anyRaw = false;
    for (int i = 0; i < NUM_KEYS; i++) if (digitalRead(btn[i].pin) == LOW) anyRaw = true;
    if (!anyRaw) { delay(10); continue; }
    delay(DEBOUNCE_MS);

    for (int i = 0; i < 3; i++) {
      if (digitalRead(btn[i].pin) == LOW) {
        buf[i] = (char)(((buf[i] - 'A' + 1) % 26) + 'A');
        // Show current initials on matrices 0-2, arrow on 3
        mx.clear();
        for (int j = 0; j < 3; j++) {
          char tmp[2] = { buf[j], '\0' };
          printText(j, j, tmp);
        }
        printText(3, 3, arrow);
        delay(200); // brief repeat-guard
        waitAllReleased();
      }
    }
    if (digitalRead(btn[3].pin) == LOW) {
      waitAllReleased();
      done = true;
    }
  }
}

// ════════════════════════════════════════════════════════════════════
//  STATE TRANSITION
// ════════════════════════════════════════════════════════════════════
void transitionTo(State s) {
  logEvf("→ %s", stateStr(s));
  gs      = s;
  stateMs = millis();
}

// ════════════════════════════════════════════════════════════════════
//  DISPLAY HELPERS FOR MENU STATES
// ════════════════════════════════════════════════════════════════════
// Show short label on each module using printText.
// Labels are 2–3 chars; each module is 8 columns wide (≈ 2 chars of font).
void showMainMenu() {
  mx.clear();
  char buf[4];
  // Module 0: START ("Go"), Module 1: TIME ("TI"), Module 2: DIFF ("DI"), Module 3: HS ("HS")
  const char* labels[4] = { "Go", "Ti", "Di", "Hi" };
  for (int i = 0; i < 4; i++) {
    strncpy(buf, labels[i], sizeof(buf));
    printText(i, i, buf);
    // Highlight selected: full brightness; others dimmer
    mx.control(i, MD_MAX72XX::INTENSITY, (menuLevel == 0) ? 8 : 3);
  }
  showScore(gameTimeSec);
  showTime(difficulty);
}

void showTimeMenu() {
  mx.clear();
  char buf[4];
  for (int i = 0; i < NUM_GAME_TIMES; i++) {
    snprintf(buf, sizeof(buf), "%d", GAME_TIMES[i]);
    printText(i, i, buf);
    mx.control(i, MD_MAX72XX::INTENSITY, (GAME_TIMES[i] == gameTimeSec) ? 12 : 3);
  }
  showScore(gameTimeSec);
  showTime(0);
}

void showDiffMenu() {
  mx.clear();
  const char* labels[4] = { "E", "N", "H", "<" }; // Easy/Normal/Hard/Back
  for (int i = 0; i < 4; i++) {
    printText(i, i, const_cast<char*>(labels[i]));
    mx.control(i, MD_MAX72XX::INTENSITY, (i < 3 && i == difficulty) ? 12 : 3);
  }
  mx.control(3, MD_MAX72XX::INTENSITY, 5);
  showScore(0);
  showTime(difficulty);
}

// ════════════════════════════════════════════════════════════════════
//  NOTE QUEUE HELPERS
// ════════════════════════════════════════════════════════════════════
void initQueue() {
  keyQueueHead = 0;
  uint8_t last = 255;
  for (int i = 0; i < KEY_QUEUE_LEN; i++) {
    uint8_t k;
    do { k = (uint8_t)random(NUM_KEYS); } while (k == last);
    keyQueue[i] = k;
    last = k;
  }
}

// Advance to next note: replace the consumed slot with a fresh random note.
void advanceQueue() {
  uint8_t last   = keyQueue[(keyQueueHead + KEY_QUEUE_LEN - 1) % KEY_QUEUE_LEN];
  uint8_t newKey;
  do { newKey = (uint8_t)random(NUM_KEYS); } while (newKey == last);
  keyQueue[keyQueueHead] = newKey;
  keyQueueHead           = (keyQueueHead + 1) % KEY_QUEUE_LEN;
}

// ════════════════════════════════════════════════════════════════════
//  STATE HANDLERS
// ════════════════════════════════════════════════════════════════════

// ── BOOT ──────────────────────────────────────────────────────────
// Non-blocking intro: light each module sequentially, then proceed to MENU.
static int bootPhase = 0;

void updateBoot() {
  unsigned long e = millis() - stateMs;
  if (bootPhase < 4) {
    unsigned long threshold = bootPhase * 150UL;
    if (e >= threshold) {
      mx.control(bootPhase, MD_MAX72XX::INTENSITY, 12);
      for (int r = 0; r < 8; r++) mx.setRow(bootPhase, r, 0xFF);
      setLed(bootPhase, true);
      bootPhase++;
    }
  } else if (bootPhase == 4 && e >= 700) {
    allLedsOff(); mx.clear();
    bootPhase = 0;
    showMainMenu();
    transitionTo(State::MENU);
  }
}

// ── MENU ──────────────────────────────────────────────────────────
void updateMenu() {
  int p = anyPressed();
  if (p < 0) return;

  if (menuLevel == 0) {
    switch (p) {
      case 0: // START GAME
        logEv("Menu: Start");
        score  = 0; tickets = 0;
        initQueue();
        countdownVal = 3;
        dispTime.showNumberDec(3, false);
        showScoreDash();
        mx.clear();
        transitionTo(State::COUNTDOWN);
        break;
      case 1: // TIME SUBMENU
        menuLevel = 1;
        showTimeMenu();
        break;
      case 2: // DIFFICULTY SUBMENU
        menuLevel = 2;
        showDiffMenu();
        break;
      case 3: // HIGH SCORES
        transitionTo(State::SHOW_HIGH_SCORES);
        break;
    }
  } else if (menuLevel == 1) {
    // Any button selects that duration
    gameTimeSec = GAME_TIMES[p];
    logEvf("Time set: %ds", gameTimeSec);
    menuLevel = 0;
    showMainMenu();
  } else if (menuLevel == 2) {
    if (p == 3) {
      menuLevel = 0; showMainMenu();
    } else {
      difficulty = p; // 0=easy 1=normal 2=hard
      logEvf("Difficulty: %d", difficulty);
      menuLevel = 0; showMainMenu();
    }
  }
}

// ── SHOW HIGH SCORES ─────────────────────────────────────────────
// Cycle through top-3 scores; any button returns to menu.
// Hold all 4 buttons ≥15 s to clear scores (mirrors inspiration behaviour).
static int  hsIdx       = 0;
static bool hsClearArmed = false;
static unsigned long hsAllHeldMs = 0;

void updateShowHighScores() {
  unsigned long e = millis() - stateMs;

  // Check "hold all 4 keys" for clear gesture
  bool allHeld = true;
  for (int i = 0; i < NUM_KEYS; i++) if (digitalRead(btn[i].pin) == HIGH) { allHeld = false; break; }
  if (allHeld) {
    if (!hsClearArmed) { hsClearArmed = true; hsAllHeldMs = millis(); }
    if (millis() - hsAllHeldMs > 10000) { mx.clear(); printText(0, 3, "Clr?"); }
    if (millis() - hsAllHeldMs > 15000) {
      clearHighScores();
      mx.clear(); printText(0, 3, "Clrd");
      delay(1500);
      hsClearArmed = false;
      menuLevel = 0; showMainMenu(); transitionTo(State::MENU); return;
    }
  } else {
    hsClearArmed = false;
  }

  // Any button (while not all held) → return to menu
  if (anyPressed() >= 0) { menuLevel = 0; showMainMenu(); transitionTo(State::MENU); return; }

  // Cycle scores every 2 s
  if (e < 2000) return;
  stateMs = millis(); // reset timer for next cycle

  hsIdx = (hsIdx + 1) % NUM_HIGH_SCORES;
  if (hsScore[hsIdx] == 0) { hsIdx = 0; } // skip empty slots, wrap to 0

  mx.clear();
  if (hsScore[0] == 0) {
    scrollText("No scores  "); return;
  }
  // Show initials on modules 0-2, score on 7-seg
  for (int j = 0; j < 3; j++) {
    char tmp[2] = { hsInitials[hsIdx][j], '\0' };
    printText(j, j, tmp);
  }
  mx.control(0, 3, MD_MAX72XX::INTENSITY, MD_MAX72XX::ON); // use default intensity
  showScore(hsScore[hsIdx]);
  showTime(hsIdx + 1); // rank 1-3
}

// ── COUNTDOWN ─────────────────────────────────────────────────────
void updateCountdown() {
  unsigned long e = millis() - stateMs;
  int step = (int)(e / COUNTDOWN_STEP);
  if (step >= 3) {
    // Start game — record the clock before transitioning
    timeRemaining = gameTimeSec;
    gameClockMs   = millis();
    showTime(gameTimeSec);
    showScore(0);
    seqBlinkOn = false; seqBlinkMs = millis();
    printKeys();
    transitionTo(State::SHOW_SEQUENCE);
    return;
  }
  int display = 3 - step;
  if (display != countdownVal) {
    countdownVal = display;
    dispTime.showNumberDec(display, false);
    mx.clear();
    char buf[4]; snprintf(buf, sizeof(buf), "%d", display);
    printText(0, 3, buf);
  }
}

// ── SHOW SEQUENCE ─────────────────────────────────────────────────
// Blink the current required key for NOTE_DELAY[difficulty] ms; no input.
static bool seqBlinkOn = false;
static unsigned long seqBlinkMs = 0;

void updateShowSequence() {
  unsigned long e = millis() - stateMs;

  // Update remaining time on 7-seg once per second
  int newRemaining = gameTimeSec - (int)((millis() - gameClockMs) / 1000);
  if (newRemaining < 0) newRemaining = 0;
  if (newRemaining != timeRemaining) {
    timeRemaining = newRemaining;
    showTime(timeRemaining);
  }
  if (timeRemaining <= 0) {
    logEv("Time up during show");
    allLedsOff();
    transitionTo(State::SCORE_DISPLAY);
    return;
  }

  // Blink the current key's LED
  if (millis() - seqBlinkMs >= BLINK_MS) {
    seqBlinkMs = millis();
    seqBlinkOn = !seqBlinkOn;
    setLed(keyQueue[keyQueueHead], seqBlinkOn);
  }

  // After delay, start accepting input
  if (e >= (unsigned long)NOTE_DELAY[difficulty]) {
    setLed(keyQueue[keyQueueHead], true); // keep LED lit during input
    transitionTo(State::WAIT_INPUT);
  }
}

// ── WAIT INPUT ────────────────────────────────────────────────────
// Accept button press; track time; award/penalise.

void updateWaitInput() {
  // Update countdown using the shared game clock
  int newRemaining = gameTimeSec - (int)((millis() - gameClockMs) / 1000);
  if (newRemaining < 0) newRemaining = 0;
  if (newRemaining != timeRemaining) {
    timeRemaining = newRemaining;
    showTime(timeRemaining);
  }
  if (timeRemaining <= 0) {
    logEv("Time up");
    allLedsOff();
    transitionTo(State::SCORE_DISPLAY);
    return;
  }

  int p = anyPressed();
  if (p < 0) return;

  int expected = keyQueue[keyQueueHead];
  if (p == expected) {
    score++;
    tickets = score / NOTES_PER_TICKET;
    setLed(p, false);
    playKeyAudio(p);
    logEvf("Correct! key=%d score=%d", p, score);
    showScore(score);
    advanceQueue();
    printKeys();
    transitionTo(State::SUCCESS);
  } else {
    logEvf("Wrong! got=%d expected=%d", p, expected);
    allLedsOff();
    playFailAudio();
    transitionTo(State::FAIL);
  }
}

// ── SUCCESS ───────────────────────────────────────────────────────
void updateSuccess() {
  unsigned long e = millis() - stateMs;
  // Flash all LEDs at 10 Hz
  bool on = ((e / 100) % 2) == 0;
  for (int i = 0; i < NUM_KEYS; i++) setLed(i, on);
  if (e >= (unsigned long)SUCCESS_ANIM_MS) {
    allLedsOff();
    seqBlinkOn = false; seqBlinkMs = millis();
    transitionTo(State::SHOW_SEQUENCE);
  }
}

// ── FAIL ──────────────────────────────────────────────────────────
void updateFail() {
  unsigned long e = millis() - stateMs;
  // Flash matrix on/off
  bool on = ((e / 200) % 2) == 0;
  if (on) { printKeys(); } else { mx.clear(); }
  allLedsOff();
  if (e >= (unsigned long)FAIL_FLASH_MS) {
    mx.clear();
    transitionTo(State::SCORE_DISPLAY);
  }
}

// ── SCORE DISPLAY ─────────────────────────────────────────────────
static bool scoreHandled = false;

void updateScoreDisplay() {
  if (!scoreHandled) {
    scoreHandled = true;
    unsigned int finalScore = (unsigned int)(score + bonusValue);
    showScore(finalScore);
    showTime(tickets);
    // Scroll "Score: NNN" briefly across matrices
    char msg[32];
    snprintf(msg, sizeof(msg), "Score %u  Tickets %d  ", finalScore, tickets);
    scrollText(msg);
    printNumber(finalScore);

    // Check high score
    if (finalScore > hsScore[NUM_HIGH_SCORES - 1]) {
      scrollText("HiScore!  ");
      char ini[4] = "AAA";
      getInitials(ini);
      insertHighScore(finalScore, ini);
    }
  }

  unsigned long e = millis() - stateMs;
  int p = anyPressed();
  if (e >= (unsigned long)SCORE_SHOW_MS || p >= 0) {
    scoreHandled = false;
    allLedsOff(); mx.clear();
    menuLevel = 0; showMainMenu();
    transitionTo(State::MENU);
  }
}

// ════════════════════════════════════════════════════════════════════
//  WEB SERVER HANDLERS
//  GET  /         — serves /index.html from SPIFFS (inline fallback)
//  GET  /status   — JSON with game state, score, tickets, time, bonus, log
//  GET  /debug    — human-readable HTML status page
//  POST /bonus    — JSON { "bonus": N }  sets bonus value
//  POST /command  — JSON { "command": "reset"|"start"|"stop" }
//
//  FUTURE HOOK: GET  /highscores — return JSON high-score table
//  FUTURE HOOK: POST /redeem     — remote ticket redemption endpoint
//  FUTURE HOOK: POST /highscore  — sync HTML high scores from remote
// ════════════════════════════════════════════════════════════════════
void handleRoot() {
  if (FS_OBJ.exists("/index.html")) {
    File f = FS_OBJ.open("/index.html", "r");
    httpSrv.streamFile(f, "text/html");
    f.close();
    return;
  }
  httpSrv.send(200, "text/html",
    "<html><body><h2>BabyPianoKeys</h2>"
    "<p>Upload <code>/index.html</code> to SPIFFS for the full UI.</p>"
    "<p><a href='/status'>Status JSON</a> &nbsp; <a href='/debug'>Debug</a></p>"
    "</body></html>");
}

void handleStatus() {
  StaticJsonDocument<768> doc;
  doc["state"]          = stateStr(gs);
  doc["score"]          = score;
  doc["tickets"]        = tickets;
  doc["time_remaining"] = timeRemaining;
  doc["bonus"]          = bonusValue;
  doc["game_time"]      = gameTimeSec;
  doc["difficulty"]     = difficulty;
  doc["uptime_ms"]      = (uint32_t)millis();
  doc["seq_index"]      = keyQueueHead;

  JsonArray ev = doc.createNestedArray("events");
  int count = min(logHead, LOG_SIZE);
  int start = logHead > LOG_SIZE ? logHead % LOG_SIZE : 0;
  for (int i = 0; i < count; i++) ev.add(logBuf[(start + i) % LOG_SIZE]);

  String out;
  serializeJson(doc, out);
  httpSrv.send(200, "application/json", out);
}

void handleDebug() {
  String out = "<html><head><meta charset='utf-8'></head><body><h2>BabyPianoKeys Debug</h2><pre>";
  out += "State:     "; out += stateStr(gs);     out += "\n";
  out += "Score:     "; out += score;             out += "\n";
  out += "Tickets:   "; out += tickets;           out += "\n";
  out += "Time rem:  "; out += timeRemaining;     out += " s\n";
  out += "Bonus:     "; out += bonusValue;        out += "\n";
  out += "GameTime:  "; out += gameTimeSec;       out += " s\n";
  out += "Difficulty:"; out += difficulty;        out += " (0=E 1=N 2=H)\n";
  out += "Uptime:    "; out += (millis() / 1000); out += " s\n";
  out += "QueueHead: "; out += keyQueueHead;      out += "\n";
  out += "\n--- High Scores ---\n";
  for (int i = 0; i < NUM_HIGH_SCORES; i++) {
    out += "#"; out += (i+1); out += "  "; out += hsInitials[i];
    out += "  "; out += hsScore[i]; out += "\n";
  }
  out += "\n--- Recent Events ---\n";
  int count = min(logHead, LOG_SIZE);
  int start = logHead > LOG_SIZE ? logHead % LOG_SIZE : 0;
  for (int i = 0; i < count; i++) out += logBuf[(start + i) % LOG_SIZE] + "\n";
  out += "</pre></body></html>";
  httpSrv.send(200, "text/html", out);
}

void handleSetBonus() {
  String body = httpSrv.arg("plain");
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, body) || !doc.containsKey("bonus")) {
    httpSrv.send(400, "application/json", "{\"error\":\"expected {bonus:N}\"}"); return;
  }
  bonusValue = doc["bonus"].as<int>();
  logEvf("Bonus set: %d", bonusValue);
  httpSrv.send(200, "application/json", "{\"ok\":true}");
}

void handleCommand() {
  String body = httpSrv.arg("plain");
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, body)) { httpSrv.send(400, "application/json", "{\"error\":\"bad JSON\"}"); return; }
  const char* cmd = doc["command"] | "";
  if (strcmp(cmd, "reset") == 0) {
    score = 0; tickets = 0; keyQueueHead = 0;
    menuLevel = 0; showMainMenu(); transitionTo(State::MENU);
    httpSrv.send(200, "application/json", "{\"ok\":true,\"cmd\":\"reset\"}");
  } else if (strcmp(cmd, "start") == 0) {
    // FUTURE HOOK: remote start (currently just acknowledges)
    httpSrv.send(200, "application/json", "{\"ok\":true,\"cmd\":\"start\"}");
  } else if (strcmp(cmd, "stop") == 0) {
    menuLevel = 0; showMainMenu(); transitionTo(State::MENU);
    httpSrv.send(200, "application/json", "{\"ok\":true,\"cmd\":\"stop\"}");
  } else {
    httpSrv.send(400, "application/json", "{\"error\":\"unknown command\"}");
  }
}

// ════════════════════════════════════════════════════════════════════
//  WIFI & WEB SERVER SETUP
// ════════════════════════════════════════════════════════════════════
void setupWifi() {
  if (WIFI_AP_MODE) {
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    logEvf("AP  → http://%s", WiFi.softAPIP().toString().c_str());
  } else {
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      logEvf("STA → http://%s", WiFi.localIP().toString().c_str());
    } else {
      // Fall back to AP mode
      WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
      logEvf("AP fallback → http://%s", WiFi.softAPIP().toString().c_str());
    }
  }
}

void setupWebServer() {
  httpSrv.on("/",        HTTP_GET,  handleRoot);
  httpSrv.on("/status",  HTTP_GET,  handleStatus);
  httpSrv.on("/debug",   HTTP_GET,  handleDebug);
  httpSrv.on("/bonus",   HTTP_POST, handleSetBonus);
  httpSrv.on("/command", HTTP_POST, handleCommand);
  httpSrv.begin();
  logEv("Web server started on :80");
}

// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  logEv("BabyPianoKeys v1.0 booting");

  // Storage
#ifdef USE_SD_CARD
  if (!SD.begin(SD_CS))    logEv("SD init failed");   else logEv("SD ready");
#else
  if (!SPIFFS.begin(true)) logEv("SPIFFS init failed"); else logEv("SPIFFS ready");
#endif

  initButtons();
  initLeds();

  // MAX7219 — 4 × 8×8 modules
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 5);
  mx.control(MD_MAX72XX::TEST, MD_MAX72XX::OFF);
  mx.clear();

  // TM1637 7-seg displays
  dispScore.setBrightness(5);
  dispTime.setBrightness(5);
  showScoreDash();
  showTime(0);

  // I2S audio
  setupI2S();

  // Audio background task pinned to Core 0
  audioQ = xQueueCreate(4, sizeof(AudioReq));
  xTaskCreatePinnedToCore(audioTaskFn, "audio", 8192, NULL, 2, NULL, 0);

  // Load saved high scores
  loadHighScores();

  // WiFi + web server
  setupWifi();
  setupWebServer();

  randomSeed(esp_random());

  logEv("Setup complete — entering BOOT");
  transitionTo(State::BOOT);
}

// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════
void loop() {
  pollButtons();

  // State machine dispatch
  switch (gs) {
    case State::BOOT:             updateBoot();             break;
    case State::MENU:             updateMenu();             break;
    case State::SHOW_HIGH_SCORES: updateShowHighScores();   break;
    case State::COUNTDOWN:        updateCountdown();        break;
    case State::SHOW_SEQUENCE:    updateShowSequence();     break;
    case State::WAIT_INPUT:       updateWaitInput();        break;
    case State::SUCCESS:          updateSuccess();          break;
    case State::FAIL:             updateFail();             break;
    case State::SCORE_DISPLAY:    updateScoreDisplay();     break;
    case State::ENTER_INITIALS:   /* handled inside SCORE_DISPLAY */ break;
  }

  // Web server — handles one request per loop iteration (non-blocking)
  httpSrv.handleClient();
}
