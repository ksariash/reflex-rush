/*
  Reflex Rush - MFS Edition (Adaptive Speed, Simple Chord-Clear)
  --------------------------------------------------------------
  Arduino Uno R3 + Multi Function Shield Library (global MFS object)

  Behavior per latest request:
  - Buttons must be recognized reliably.
  - No long-press logic.
  - In ATTRACT mode: pressing ANY SINGLE button starts a new game.
  - If TWO DIFFERENT buttons are pressed "at once" (detected as a quick chord
    within 200 ms), the HIGH SCORE is CLEARED immediately and "HI 0" is shown briefly.
  - Gameplay: reaction window starts at 1000 ms and gets 10 ms faster after each
    correct press, down to a minimum of 300 ms.
  - Target digit 1..3 appears; press matching S1..S3 before time runs out.
  - LEDs D10..D13 are a countdown that turn OFF one-by-one as time elapses.
  - Silent gameplay (no beeps/tone) and no blinking during play.
  - After Game Over: show final score for 2 s, then "HInn" (nn=0..99) for 2 s,
    then return to ATTRACT ("PLAY").
  - Button presses in GAME OVER are ignored (until back in ATTRACT).

  Note on "two buttons at once":
  The MFS library typically reports a single button ID at a time. To emulate
  simultaneous detection without accessing raw pins, we treat two distinct
  button presses that occur within 200 ms (without starting the game) as a chord.
*/

#include <Arduino.h>
#include <EEPROM.h>
#include <MultiFuncShield.h>  // global MFS: initialize(), write(), writeLeds(), getButton(), ...

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
static const uint16_t WINDOW_START_MS   = 1000;  // initial reaction window
static const uint16_t WINDOW_MIN_MS     = 300;   // minimum reaction window
static const uint16_t WINDOW_STEP_MS    = 10;    // reduce by 10 ms after each correct press

static const uint16_t GAMEOVER_SCORE_MS = 2000;  // show score for 2 s
static const uint16_t GAMEOVER_HI_MS    = 2000;  // then show HI for 2 s

static const uint16_t HI0_SHOW_MS       = 800;   // show "HI 0" for 0.8 s after clearing HS

// "Simultaneous" two-button chord window (ms)
static const uint16_t CHORD_WINDOW_MS   = 200;

// EEPROM address for single uint16_t high score
static const uint16_t HS_EE_ADDR = 0;

#ifndef ON
#define ON  1
#endif
#ifndef OFF
#define OFF 0
#endif
static const uint8_t LED_MASK_ALL = 0x0F; // D10..D13 as bit0..bit3

// -----------------------------------------------------------------------------
// Types & Globals
// -----------------------------------------------------------------------------
enum GameState : uint8_t { ATTRACT = 0, PLAYING = 1, GAMEOVER = 2 };

GameState g_state = ATTRACT;

uint16_t g_highScore = 0;
uint16_t g_score     = 0;

uint16_t g_windowMs  = WINDOW_START_MS;  // adaptive reaction window

uint8_t  g_targetDigit  = 0;             // 1..3
uint32_t g_roundStartMs = 0;

uint8_t  g_ledsShown = 0;

// Button tracking (edge-based)
int8_t   g_btnNow  = 0;       // 0 if none; 1..3 if pressed
int8_t   g_btnPrev = 0;

// Short-click start (ATTRACT)
int8_t   g_shortClickId = 0;  // 1..3 on short click; consumed by ATTRACT logic

// "Two-button chord" detection (ATTRACT)
int8_t   g_chordFirstId   = 0;    // first button id in potential chord
uint32_t g_chordFirstTime = 0;    // when first button was pressed
bool     g_chordConsumed  = false; // true if chord used to clear HS

// Simple debouncing/windowing for single-click detection
uint32_t g_pressStartMs = 0;

// State timestamps
uint32_t g_stateTs = 0;

// After clearing HS, show "HI 0" briefly in ATTRACT
bool     g_showHi0 = false;

// -----------------------------------------------------------------------------
// EEPROM helpers
// -----------------------------------------------------------------------------
static void loadHighScore() {
  uint16_t val;
  EEPROM.get(HS_EE_ADDR, val);
  if (val == 0xFFFF) val = 0;
  g_highScore = val;
}

static void saveHighScore(uint16_t val) {
  EEPROM.put(HS_EE_ADDR, val);
  g_highScore = val;
}

// -----------------------------------------------------------------------------
// Display helpers
// -----------------------------------------------------------------------------
static void displayText(const char* s) { MFS.write(s); }
static void displayNumber(int value)   { MFS.write(value); }

static void ledsSetCount(uint8_t countOn) {
  // countOn 0..4 -> set that many LEDs ON (from D10 upward)
  uint8_t mask = 0;
  if (countOn >= 1) mask |= (1 << 0);
  if (countOn >= 2) mask |= (1 << 1);
  if (countOn >= 3) mask |= (1 << 2);
  if (countOn >= 4) mask |= (1 << 3);
  MFS.writeLeds(LED_MASK_ALL, OFF);
  if (mask) MFS.writeLeds(mask, ON);
  g_ledsShown = countOn;
}
static void ledsAllOff() { MFS.writeLeds(LED_MASK_ALL, OFF); g_ledsShown = 0; }

// -----------------------------------------------------------------------------
// Buttons
// -----------------------------------------------------------------------------
static int8_t readButtonRaw() {
  // Expectation: 0 if none, else 1..3 for S1..S3
  int v = MFS.getButton();
  if (v < 0 || v > 3) v = 0;
  return (int8_t)v;
}

static void updateButtons() {
  uint32_t now = millis();
  int8_t btn = readButtonRaw();

  // Rising edge
  if (g_btnPrev == 0 && btn != 0) {
    g_pressStartMs = now;

    if (g_state == ATTRACT) {
      // Handle potential chord
      if (g_chordFirstId == 0) {
        g_chordFirstId   = btn;
        g_chordFirstTime = now;
        g_chordConsumed  = false;
      } else if (btn != g_chordFirstId && (now - g_chordFirstTime) <= CHORD_WINDOW_MS) {
        // Two distinct buttons within chord window -> clear high score
        saveHighScore(0);
        displayText("HI 0");
        g_showHi0 = true;
        g_stateTs = now;
        g_chordConsumed = true;
        // Reset chord tracking
        g_chordFirstId = 0;
      } else {
        // Not a valid chord; reset with new first
        g_chordFirstId   = btn;
        g_chordFirstTime = now;
        g_chordConsumed  = false;
      }
    }
  }

  // Falling edge -> treat as short click if not part of a chord-clear
  if (g_btnPrev != 0 && btn == 0) {
    if (g_state == ATTRACT) {
      if (!g_chordConsumed) {
        // Start game on the button that was released
        g_shortClickId = g_btnPrev;
      }
      // Clear chord tracking after release (unless just set by another press)
      g_chordFirstId = 0;
    }
  }

  g_btnPrev = btn;
  g_btnNow  = btn;
}

// -----------------------------------------------------------------------------
// Game mechanics
// -----------------------------------------------------------------------------
static uint8_t pickTargetDigit() { return (uint8_t)random(1, 4); } // 1..3

static void resetSpeed() { g_windowMs = WINDOW_START_MS; }

static void makeFaster() {
  if (g_windowMs > WINDOW_MIN_MS) {
    uint16_t next = (g_windowMs > WINDOW_STEP_MS) ? (g_windowMs - WINDOW_STEP_MS) : WINDOW_MIN_MS;
    g_windowMs = (next < WINDOW_MIN_MS) ? WINDOW_MIN_MS : next;
  }
}

static void startRound() {
  g_targetDigit = pickTargetDigit();
  g_roundStartMs = millis();
  displayNumber(g_targetDigit); // show target; no blink
  ledsSetCount(4);              // full countdown at start
}

static void enterAttract() {
  g_state = ATTRACT;
  g_stateTs = millis();
  g_score = 0;
  ledsAllOff();
  if (!g_showHi0) {
    displayText("PLAY");
  }
  // Reset chord window tracking when arriving in ATTRACT
  g_chordFirstId = 0;
  g_chordConsumed = false;
}

static void enterPlaying() {
  g_state = PLAYING;
  g_stateTs = millis();
  g_score = 0;
  resetSpeed();
  startRound();
}

static void enterGameOver() {
  if (g_score > g_highScore) {
    saveHighScore(g_score);
  }
  g_state = GAMEOVER;
  g_stateTs = millis();
  ledsAllOff();
  displayNumber(g_score); // Show final score first
}

// -----------------------------------------------------------------------------
// State ticks
// -----------------------------------------------------------------------------
static void tickAttract() {
  uint32_t now = millis();

  // After clearing HS, keep "HI 0" visible briefly
  if (g_showHi0) {
    if (now - g_stateTs >= HI0_SHOW_MS) {
      g_showHi0 = false;
      displayText("PLAY");
    }
  }

  // Start game on short click (unless we just performed a chord-clear)
  if (!g_showHi0 && g_shortClickId != 0) {
    g_shortClickId = 0; // consume
    enterPlaying();
  }
}

static void tickPlaying() {
  uint32_t now = millis();
  uint32_t elapsed = now - g_roundStartMs;

  // LED countdown (4->0) at 25% increments
  uint8_t shouldOn = 4;
  if (elapsed >= g_windowMs) {
    shouldOn = 0;
  } else {
    float f = (float)elapsed / (float)g_windowMs;
    if (f >= 0.75f)      shouldOn = 1;
    else if (f >= 0.50f) shouldOn = 2;
    else if (f >= 0.25f) shouldOn = 3;
    else                 shouldOn = 4;
  }
  if (shouldOn != g_ledsShown) ledsSetCount(shouldOn);

  // Timeout -> game over
  if (elapsed >= g_windowMs) {
    enterGameOver();
    return;
  }

  // Simple "rising edge" reaction: if a button just went down, handle it
  if (g_btnPrev == 0 && g_btnNow != 0) {
    // (This branch executes only on the exact loop where the press happened;
    //  because we update g_btnPrev at the end of updateButtons(), we emulate
    //  the edge by checking the current vs. last cached values.)
  }

  // Accept current press if any is down (responsive even if edge missed)
  if (g_btnNow != 0) {
    int8_t pressed = g_btnNow;
    if (pressed == g_targetDigit) {
      g_score++;
      makeFaster();
      startRound();
    } else {
      enterGameOver();
    }
  }
}

static void tickGameOver() {
  // Ignore all presses in GAME OVER; wait until ATTRACT is re-entered
  g_shortClickId = 0;
  g_chordFirstId = 0;
  g_chordConsumed = false;

  uint32_t now = millis();
  uint32_t dt = now - g_stateTs;

  if (dt < GAMEOVER_SCORE_MS) {
    // still showing final score
    return;
  } else if (dt < (GAMEOVER_SCORE_MS + GAMEOVER_HI_MS)) {
    // Show high score as "HInn"
    char buf[5];
    uint16_t shown = g_highScore % 100;
    snprintf(buf, sizeof(buf), "HI%02u", shown);
    displayText(buf);
  } else {
    enterAttract();
  }
}

// -----------------------------------------------------------------------------
// Arduino core
// -----------------------------------------------------------------------------
void setup() {
  MFS.initialize();
  randomSeed(analogRead(A0));

  // If S1 held during startup (immediately after reset), clear high score
  delay(200);  // short debounce after power-up
  int btn = MFS.getButton();
  if (btn == 1) {
    saveHighScore(0);
    displayText("HI 0");
    delay(1000);
  }

  loadHighScore();
  enterAttract();
}


void loop() {
  updateButtons();

  switch (g_state) {
    case ATTRACT:  tickAttract();  break;
    case PLAYING:  tickPlaying();  break;
    case GAMEOVER: tickGameOver(); break;
  }
}
