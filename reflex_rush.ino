#include <MultiFuncShield.h>
#include <EEPROM.h>

/*
  Reflex Rush - Clean Rewrite (Trickier 6OR7)
  Board: Arduino Uno R3
  Shield: Multi-Function Shield
  Library: MultiFuncShield (global MFS: initialize(), write(), writeLeds(), getButton(), ...)

  ATTRACT selector:
    S1 prev, S3 next, S2 start

  Games:
    CLSC, HARD, NOT, CORD, 6OR7

  Notes:
    - This version uses ONLY BUTTON_PRESSED_IND (button-down). Your library
      doesn't define BUTTON_RELEASED_IND, so we ignore release events entirely.
*/

//
// =============================================================================
// Constants
// =============================================================================
//

static const uint16_t WINDOW_START_MS      = 1800;   // CLSC / NOT / 6OR7 start
static const uint16_t HARD_START_MS        = 1000;   // HARD start
static const uint16_t WINDOW_MIN_MS        = 360;
static const uint16_t WINDOW_STEP_MS       = 20;

static const uint16_t GAMEOVER_SCORE_MS    = 2000;
static const uint16_t GAMEOVER_HI_MS       = 2000;
static const uint16_t SCORE_BLINK_MS       = 300;

static const uint16_t CORD_TIMEOUT_MS      = 1000;   // per-press timeout in CORD
static const uint16_t CORD_SHOW_STEP_MS    = 260;    // per symbol show
static const uint16_t CORD_GAP_MS          = 120;    // gap between symbols
static const uint8_t  CORD_MAX_SEQ         = 20;

static const uint16_t BOOT_CLEAR_DELAY_MS  = 200;
static const uint16_t BOOT_HI0_MS          = 1000;

// 6OR7 specific
static const uint16_t SIX7_FLASH_MS        = 140;    // short flash modes
static const uint16_t SIX7_GHOST_MS        = 90;     // ultra brief flash
static const uint16_t SIX7_CHORD_WINDOW_MS = 500;    // chord entry window (trickier but fair)
static const uint16_t SIX7_SEQ_WINDOW_MS   = 900;    // time to hit second press in seq modes

static const int EEPROM_BASE_ADDR          = 0;      // each game: 2 bytes (uint16_t)

//
// =============================================================================
// Types
// =============================================================================
//

enum Mode : uint8_t { MODE_ATTRACT, MODE_PLAYING, MODE_GAMEOVER };

enum GameId : uint8_t {
  GAME_CLSC = 0,
  GAME_HARD,
  GAME_NOT,
  GAME_CORD,
  GAME_6OR7,
  GAME_COUNT
};

struct ButtonEvent {
  uint8_t num;   // 1..3
  bool hasDown;  // true only on BUTTON_PRESSED_IND
};

struct GameCommon {
  uint16_t score;
  uint16_t hi[GAME_COUNT];

  // reflex window for current game
  uint16_t windowMs;
  uint32_t roundStartMs;

  // expected for simple single-press rounds
  uint8_t expectedBtn;   // 1..3 (0 = special)
  uint8_t auxA;          // misc (NOT: forbidA or seq second button)
  uint8_t auxB;          // misc (NOT: forbidB)
};

struct Timer {
  uint32_t t0 = 0;
  uint32_t dur = 0;

  void start(uint32_t now, uint32_t duration) { t0 = now; dur = duration; }
  bool active(uint32_t now) const { return dur != 0 && (now - t0) < dur; }
  bool done(uint32_t now) const { return dur != 0 && (now - t0) >= dur; }
  uint32_t remain(uint32_t now) const {
    if (dur == 0) return 0;
    uint32_t e = now - t0;
    return (e >= dur) ? 0 : (dur - e);
  }
};

//
// =============================================================================
// Globals
// =============================================================================
//

static Mode       gMode = MODE_ATTRACT;
static GameId     gGame = GAME_CLSC;
static GameCommon G;
static ButtonEvent gBtn;

static uint32_t gModeStartMs = 0;

// Attract pulse: alternate game label and HI
static Timer attractPulse;
static bool  attractShowHi = false;

// Gameover blink
static Timer scoreBlinkTimer;
static bool  scoreBlinkOn = true;

// --- CORD ---
enum CordPhase : uint8_t { CORD_SHOWING, CORD_INPUT };
static CordPhase cordPhase = CORD_SHOWING;
static uint8_t  cordSeq[CORD_MAX_SEQ];
static uint8_t  cordLen = 1;
static uint8_t  cordIndex = 0;
static Timer    cordShowTimer;
static uint8_t  cordShowPos = 0;
static bool     cordShowingDigit = true;
static Timer    cordInputTimer;  // per-press timeout; resets each correct press

// --- 6OR7 ---
enum Six7Stage : uint8_t {
  // classic-ish but still tricky:
  S7_SWAP = 0,           // swap sides (6 right / 7 left)
  S7_NOT,                // invert mapping with 'n' hint
  S7_FLASH,              // flash then blank
  S7_FLASH_NOT,          // flash + NOT
  S7_GHOST,              // ultra brief flash
  S7_BLINK,              // blink 2x then steady
  S7_MID65,              // sometimes 6.5 -> middle
  S7_MID65_NOT,          // 6.5 + NOT
  S7_CHORD,              // press 1+3 as a chord (two distinct presses quickly)
  S7_CHORD_SWAP,         // chord but digits swapped (purely visual misdirection)
  S7_SEQ_67,             // press 6 then 7 (order)
  S7_SEQ_76,             // press 7 then 6 (order)
  S7_SHORT_WINDOW,       // reaction window cut to ~60%
  S7_TRICK_MIX,          // weighted random “party mix” per round
  S7_COUNT
};

static uint8_t six7Stage = S7_TRICK_MIX;
static uint8_t six7StageBlock = 0xFF; // score/5 bucket

// per-round visuals/state
static uint8_t six7Digit = 6;       // 6 or 7
static uint8_t six7Side = 0;        // 0 left, 3 right
static bool    six7NotFlag = false;

static bool    six7FlashActive = false;
static Timer   six7FlashTimer;

static bool    six7BlinkActive = false;
static uint8_t six7BlinkCount = 0;
static Timer   six7BlinkTimer;

// chord entry
static bool    six7ChordWaitingSecond = false;
static uint8_t six7ChordFirstBtn = 0;
static Timer   six7ChordTimer;
static bool    six7RoundIsChord = false;

// seq entry
static bool    six7SeqWaitingSecond = false;
static uint8_t six7SeqFirstBtn = 0;
static Timer   six7SeqTimer;
static bool    six7RoundIsSeq = false;

//
// =============================================================================
// EEPROM High Score
// =============================================================================
//

static int hiAddr(GameId id) { return EEPROM_BASE_ADDR + (int)id * 2; }

static uint16_t eepromLoadHi(GameId id) {
  uint16_t v;
  EEPROM.get(hiAddr(id), v);
  return (v == 0xFFFF) ? 0 : v;
}

static void eepromSaveHi(GameId id, uint16_t v) {
  EEPROM.put(hiAddr(id), v);
}

static void eepromClearAllHi() {
  uint16_t sentinel = 0xFFFF;
  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    EEPROM.put(hiAddr((GameId)i), sentinel);
    G.hi[i] = 0;
  }
}

//
// =============================================================================
// Display / LEDs
// =============================================================================
//

static void ledsCount(uint8_t n) {
  byte mask = 0;
  if (n >= 1) mask |= LED_1;
  if (n >= 2) mask |= LED_2;
  if (n >= 3) mask |= LED_3;
  if (n >= 4) mask |= LED_4;
  MFS.writeLeds(LED_ALL, OFF);
  if (mask) MFS.writeLeds(mask, ON);
}

static void ledsBarFromRemaining(uint32_t remainMs, uint32_t totalMs) {
  if (totalMs == 0) { ledsCount(0); return; }
  if (remainMs == 0) { ledsCount(0); return; }
  float frac = (float)remainMs / (float)totalMs;
  if (frac > 0.75f) ledsCount(4);
  else if (frac > 0.50f) ledsCount(3);
  else if (frac > 0.25f) ledsCount(2);
  else ledsCount(1);
}

static void showText(const char* s) { MFS.write(s); }
static void showNumber(int v) { MFS.write(v); }
static void clearDisp() { MFS.write("    "); }

static void showHi(uint16_t hi) {
  if (hi < 100) {
    char b[5];
    b[0] = 'H';
    b[1] = 'I';
    b[2] = '0' + (hi / 10);
    b[3] = '0' + (hi % 10);
    b[4] = 0;
    showText(b);
  } else {
    showNumber((int)hi);
  }
}

//
// =============================================================================
// Input (button down only)
// =============================================================================
//

static void readButtons() {
  gBtn.hasDown = false;
  byte raw = MFS.getButton();
  if (!raw) return;

  uint8_t num = raw & B00111111;
  uint8_t act = raw & B11000000;

  // Your library defines BUTTON_PRESSED_IND; it may not define RELEASED.
  if (act == BUTTON_PRESSED_IND) {
    gBtn.hasDown = true;
    gBtn.num = num; // 1..3
  }
}

//
// =============================================================================
// Common gameplay helpers
// =============================================================================
//

static void shrinkWindow() {
  if (G.windowMs <= WINDOW_MIN_MS) { G.windowMs = WINDOW_MIN_MS; return; }
  uint16_t nw = G.windowMs;
  if (nw > WINDOW_MIN_MS + WINDOW_STEP_MS) nw -= WINDOW_STEP_MS;
  else nw = WINDOW_MIN_MS;
  G.windowMs = nw;
}

static void startReflexRound(uint32_t now, uint8_t expectedBtn, uint16_t windowMs) {
  G.expectedBtn  = expectedBtn;
  G.roundStartMs = now;
  G.windowMs     = windowMs;
  ledsCount(4);
}

static const char* GAME_LABELS[GAME_COUNT] = { "CLSC", "HARD", "NOT ", "CORD", "6OR7" };

static void enterAttract(uint32_t now) {
  gMode = MODE_ATTRACT;
  gModeStartMs = now;

  G.score = 0;
  ledsCount(0);

  showText(GAME_LABELS[gGame]);
  attractPulse.start(now, 900);
  attractShowHi = false;
}

static void enterPlaying(uint32_t now);

static void enterGameOver(uint32_t now) {
  gMode = MODE_GAMEOVER;
  gModeStartMs = now;

  if (G.score > G.hi[gGame]) {
    G.hi[gGame] = G.score;
    eepromSaveHi(gGame, G.hi[gGame]);
  }

  ledsCount(0);
  scoreBlinkTimer.start(now, SCORE_BLINK_MS);
  scoreBlinkOn = true;
  showNumber(G.score);
}

//
// =============================================================================
// Game: CLSC / HARD / NOT
// =============================================================================
//

static void newRoundClassic(uint32_t now, bool hard) {
  uint8_t d = (uint8_t)random(1, 4); // 1..3

  if (!hard) {
    showNumber(d);
  } else {
    char buf[5] = "    ";
    uint8_t p = (uint8_t)random(0, 4);
    buf[p] = (char)('0' + d);
    showText(buf);
  }

  uint16_t startW = hard ? HARD_START_MS : WINDOW_START_MS;
  uint16_t w = (G.score == 0) ? startW : G.windowMs;
  startReflexRound(now, d, w);
}

static void newRoundNot(uint32_t now) {
  uint8_t a = (uint8_t)random(1, 4);
  uint8_t b;
  do { b = (uint8_t)random(1, 4); } while (b == a);
  if (a > b) { uint8_t t = a; a = b; b = t; }

  G.auxA = a;
  G.auxB = b;

  char buf[5] = "NO12";
  buf[2] = '0' + a;
  buf[3] = '0' + b;
  showText(buf);

  uint8_t exp = 1;
  for (uint8_t k = 1; k <= 3; k++) if (k != a && k != b) exp = k;
  uint16_t w = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
  startReflexRound(now, exp, w);
}

//
// =============================================================================
// Game: CORD (memory with 1s timeout and LED bar, resets each correct press)
// =============================================================================
//

static void cordReset() {
  cordLen = 1;
  cordIndex = 0;
  for (uint8_t i = 0; i < CORD_MAX_SEQ; i++) cordSeq[i] = 0;
  cordSeq[0] = (uint8_t)random(1, 4);
}

static void cordExtend() {
  if (cordLen < CORD_MAX_SEQ) {
    cordSeq[cordLen] = (uint8_t)random(1, 4);
    cordLen++;
  }
}

static void cordBeginShow(uint32_t now) {
  cordPhase = CORD_SHOWING;
  cordShowPos = 0;
  cordShowingDigit = true;
  cordShowTimer.start(now, CORD_SHOW_STEP_MS);
  ledsCount(0);
}

static void cordBeginInput(uint32_t now) {
  cordPhase = CORD_INPUT;
  cordIndex = 0;
  cordInputTimer.start(now, CORD_TIMEOUT_MS);
  ledsCount(4);
}

static void cordTick(uint32_t now) {
  if (cordPhase == CORD_SHOWING) {
    if (cordShowPos >= cordLen) {
      clearDisp();
      cordBeginInput(now);
      return;
    }

    if (cordShowTimer.done(now)) {
      if (cordShowingDigit) {
        clearDisp();
        cordShowingDigit = false;
        cordShowTimer.start(now, CORD_GAP_MS);
      } else {
        cordShowPos++;
        cordShowingDigit = true;
        cordShowTimer.start(now, CORD_SHOW_STEP_MS);
      }
    } else {
      if (cordShowingDigit) {
        char buf[5] = "    ";
        buf[1] = (char)('0' + cordSeq[cordShowPos]); // position 2
        showText(buf);
      }
    }
    return;
  }

  // input phase
  if (cordInputTimer.done(now)) {
    enterGameOver(now);
    return;
  }
  ledsBarFromRemaining(cordInputTimer.remain(now), CORD_TIMEOUT_MS);
}

static void cordHandleDown(uint32_t now, uint8_t btn) {
  if (cordPhase != CORD_INPUT) return;

  if (cordInputTimer.done(now)) {
    enterGameOver(now);
    return;
  }

  if (btn != cordSeq[cordIndex]) {
    enterGameOver(now);
    return;
  }

  // correct press
  cordIndex++;
  cordInputTimer.start(now, CORD_TIMEOUT_MS); // reset timeout
  ledsCount(4);

  if (cordIndex >= cordLen) {
    G.score++;
    cordExtend();
    cordBeginShow(now);
  }
}

//
// =============================================================================
// Game: 6OR7 (more trick-based)
// =============================================================================
//

static uint8_t btnForSide(uint8_t side) { return (side == 0) ? 1 : 3; }
static uint8_t oppSide(uint8_t side) { return (side == 0) ? 3 : 0; }
static uint8_t oppBtn(uint8_t b) { return (b == 1) ? 3 : (b == 3 ? 1 : 2); }

// Weighted stage picker: very trick-heavy.
// Called every time score crosses a multiple of 5.
static uint8_t pickTrickStageWeighted() {
  // Total weight = 100
  // Heaviest: TRICK_MIX, NOT variants, FLASH variants, MID65 variants, CHORD/SEQ, etc.
  uint8_t r = (uint8_t)random(0, 100);

  if (r < 18) return S7_TRICK_MIX;      // 18%
  if (r < 32) return S7_FLASH_NOT;      // 14%
  if (r < 44) return S7_NOT;            // 12%
  if (r < 54) return S7_MID65_NOT;      // 10%
  if (r < 62) return S7_CHORD;          // 8%
  if (r < 70) return S7_SEQ_67;         // 8%
  if (r < 78) return S7_SEQ_76;         // 8%
  if (r < 85) return S7_GHOST;          // 7%
  if (r < 91) return S7_SHORT_WINDOW;   // 6%
  if (r < 95) return S7_SWAP;           // 4%
  if (r < 98) return S7_FLASH;          // 3%
  return S7_BLINK;                       // 2%
}

static void six7UpdateStageIfNeeded() {
  uint8_t block = (uint8_t)(G.score / 5);
  if (block != six7StageBlock) {
    six7StageBlock = block;
    six7Stage = pickTrickStageWeighted();
  }
}

static void six7ShowSingle(uint8_t digit, uint8_t side, bool notFlag, bool blankAfter, uint16_t blankAfterMs, uint32_t now) {
  char buf[5] = "    ";
  buf[side] = (char)('0' + digit);
  if (notFlag) {
    if (side == 0) buf[1] = 'n';
    else buf[2] = 'n';
  }
  showText(buf);

  if (blankAfter) {
    six7FlashActive = true;
    six7FlashTimer.start(now, blankAfterMs);
  } else {
    six7FlashActive = false;
  }
}

static void six7ShowMid65(bool notFlag, bool blankAfter, uint16_t blankAfterMs, uint32_t now) {
  char buf[5] = "    ";
  buf[0] = '6';
  buf[1] = '.';
  buf[2] = '5';
  if (notFlag) buf[3] = 'n';
  showText(buf);

  if (blankAfter) {
    six7FlashActive = true;
    six7FlashTimer.start(now, blankAfterMs);
  } else {
    six7FlashActive = false;
  }
}

static void six7ShowChord(uint32_t now, bool swap) {
  char buf[5] = "    ";
  if (!swap) { buf[0] = '6'; buf[3] = '7'; }
  else       { buf[0] = '7'; buf[3] = '6'; }
  showText(buf);

  six7RoundIsChord = true;
  six7RoundIsSeq = false;
  six7ChordWaitingSecond = false;
  six7ChordFirstBtn = 0;
  six7ChordTimer.start(now, SIX7_CHORD_WINDOW_MS);
}

static void six7ShowSeq(uint32_t now, uint8_t firstDigit, uint8_t secondDigit) {
  char buf[5] = "    ";
  buf[1] = (char)('0' + firstDigit);
  showText(buf);

  six7FlashActive = true;
  six7FlashTimer.start(now, 220);

  six7RoundIsSeq = true;
  six7RoundIsChord = false;

  six7SeqWaitingSecond = false;
  six7SeqFirstBtn = (firstDigit == 6) ? 1 : 3;
  G.auxA = (secondDigit == 6) ? 1 : 3;     // store second expected button in auxA
  six7SeqTimer.start(now, SIX7_SEQ_WINDOW_MS);
}

static void six7BeginRound(uint32_t now) {
  six7UpdateStageIfNeeded();

  // reset per-round flags
  six7FlashActive = false;
  six7BlinkActive = false;
  six7BlinkCount = 0;
  six7RoundIsChord = false;
  six7RoundIsSeq = false;
  six7NotFlag = false;

  // base digit and side: 6-left or 7-right
  bool showSix = (random(0, 2) == 0);
  six7Digit = showSix ? 6 : 7;
  uint8_t baseSide = showSix ? 0 : 3;

  // stage knobs (and TRICK_MIX knobs)
  bool swap = false;
  bool notMap = false;
  bool flash = false;
  bool ghost = false;
  bool blink = false;
  bool mid65 = false;
  bool chord = false;
  bool chordSwap = false;
  bool seq = false;
  bool seq76 = false;
  bool shortWin = false;
  bool trickMix = false;

  switch (six7Stage) {
    case S7_SWAP: swap = true; break;
    case S7_NOT: notMap = true; break;
    case S7_FLASH: flash = true; break;
    case S7_FLASH_NOT: flash = true; notMap = true; break;
    case S7_GHOST: ghost = true; break;
    case S7_BLINK: blink = true; break;
    case S7_MID65: mid65 = true; break;
    case S7_MID65_NOT: mid65 = true; notMap = true; break;
    case S7_CHORD: chord = true; break;
    case S7_CHORD_SWAP: chord = true; chordSwap = true; break;
    case S7_SEQ_67: seq = true; seq76 = false; break;
    case S7_SEQ_76: seq = true; seq76 = true; break;
    case S7_SHORT_WINDOW: shortWin = true; break;
    case S7_TRICK_MIX: trickMix = true; break;
    default: break;
  }

  // TRICK_MIX = per-round weighted mischief
  if (trickMix) {
    // Bias toward NOT/FLASH/MID/chord/seq—very trick-heavy
    uint8_t r = (uint8_t)random(0, 100);
    if (r < 20) notMap = true;
    else if (r < 38) { flash = true; notMap = true; }
    else if (r < 52) { mid65 = true; notMap = true; }
    else if (r < 64) chord = true;
    else if (r < 76) { seq = true; seq76 = (random(0,2)==0); }
    else if (r < 86) ghost = true;
    else if (r < 94) shortWin = true;
    else blink = true;

    // occasional swap sprinkle
    if (random(0, 5) == 0) swap = true;
  }

  // window selection
  uint16_t w = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
  if (shortWin) w = (uint16_t)(w * 0.60f);

  // MID65 has ~40% chance to appear when enabled
  if (mid65 && random(0, 5) < 2) {
    if (!notMap) {
      G.expectedBtn = 2;
      six7ShowMid65(false, flash || ghost, ghost ? SIX7_GHOST_MS : SIX7_FLASH_MS, now);
    } else {
      // "not 6.5" means NOT middle; choose a side as the correct answer this round
      bool left = (random(0,2)==0);
      G.expectedBtn = left ? 1 : 3;
      six7ShowMid65(true, flash || ghost, ghost ? SIX7_GHOST_MS : SIX7_FLASH_MS, now);
    }
    startReflexRound(now, G.expectedBtn, w);
    return;
  }

  // CHORD stage
  if (chord) {
    six7ShowChord(now, chordSwap || swap);
    startReflexRound(now, 0, w); // 0 indicates special
    return;
  }

  // SEQ stage
  if (seq) {
    uint8_t firstD = seq76 ? 7 : 6;
    uint8_t secondD = seq76 ? 6 : 7;

    // if swap toggled, swap digits in display order (keeps it chaotic but learnable)
    if (swap) { uint8_t t = firstD; firstD = secondD; secondD = t; }

    six7ShowSeq(now, firstD, secondD);
    startReflexRound(now, 0, w);
    return;
  }

  // SINGLE digit stage
  if (swap) baseSide = oppSide(baseSide);
  six7Side = baseSide;

  uint8_t baseBtn = btnForSide(baseSide);
  if (notMap) {
    six7NotFlag = true;
    baseBtn = oppBtn(baseBtn);
  } else {
    six7NotFlag = false;
  }

  // BLINK stage logic
  if (blink) {
    six7BlinkActive = true;
    six7BlinkCount = 0;
    six7BlinkTimer.start(now, 140);
  }

  bool blankAfter = flash || ghost;
  uint16_t blankMs = ghost ? SIX7_GHOST_MS : SIX7_FLASH_MS;

  G.expectedBtn = baseBtn;
  six7ShowSingle(six7Digit, baseSide, six7NotFlag, blankAfter, blankMs, now);
  startReflexRound(now, G.expectedBtn, w);
}

static void six7Tick(uint32_t now) {
  // handle flash blanking
  if (six7FlashActive && six7FlashTimer.done(now)) {
    six7FlashActive = false;
    clearDisp();
  }

  // blink behavior: 2 blinks then steady
  if (six7BlinkActive && six7BlinkTimer.done(now)) {
    six7BlinkTimer.start(now, 140);
    six7BlinkCount++;

    if (six7BlinkCount <= 4) {
      if (six7BlinkCount % 2 == 1) {
        clearDisp();
      } else {
        six7ShowSingle(six7Digit, six7Side, six7NotFlag, false, 0, now);
      }
    } else {
      six7BlinkActive = false;
      six7ShowSingle(six7Digit, six7Side, six7NotFlag, false, 0, now);
    }
  }
}

static void six7HandleDown(uint32_t now, uint8_t btn) {
  // chord round
  if (six7RoundIsChord) {
    if (!six7ChordWaitingSecond) {
      six7ChordWaitingSecond = true;
      six7ChordFirstBtn = btn;
      six7ChordTimer.start(now, SIX7_CHORD_WINDOW_MS);
      return;
    } else {
      if (six7ChordTimer.done(now)) { enterGameOver(now); return; }
      if (btn == six7ChordFirstBtn) { enterGameOver(now); return; }

      bool ok = ((six7ChordFirstBtn == 1 && btn == 3) || (six7ChordFirstBtn == 3 && btn == 1));
      if (!ok) { enterGameOver(now); return; }

      // success chord
      G.score++;
      shrinkWindow();
      six7BeginRound(now);
      return;
    }
  }

  // seq round
  if (six7RoundIsSeq) {
    if (!six7SeqWaitingSecond) {
      if (btn != six7SeqFirstBtn) { enterGameOver(now); return; }
      six7SeqWaitingSecond = true;
      six7SeqTimer.start(now, SIX7_SEQ_WINDOW_MS);

      // show second digit briefly as hint
      char buf[5] = "    ";
      buf[2] = (char)('0' + ((G.auxA == 1) ? 6 : 7));
      showText(buf);
      six7FlashActive = true;
      six7FlashTimer.start(now, 220);
      return;
    } else {
      if (six7SeqTimer.done(now)) { enterGameOver(now); return; }
      if (btn != G.auxA) { enterGameOver(now); return; }

      G.score++;
      shrinkWindow();
      six7BeginRound(now);
      return;
    }
  }

  // normal single-press
  if (btn != G.expectedBtn) { enterGameOver(now); return; }
  G.score++;
  shrinkWindow();
  six7BeginRound(now);
}

//
// =============================================================================
// Playing dispatcher
// =============================================================================
//

static void enterPlaying(uint32_t now) {
  gMode = MODE_PLAYING;
  gModeStartMs = now;

  G.score = 0;
  G.windowMs = WINDOW_START_MS;
  G.roundStartMs = now;
  G.expectedBtn = 0;
  G.auxA = G.auxB = 0;

  if (gGame == GAME_CORD) {
    cordReset();
    cordBeginShow(now);
  } else if (gGame == GAME_6OR7) {
    six7StageBlock = 0xFF;
    six7Stage = S7_TRICK_MIX; // start spicy
    six7BeginRound(now);
  } else if (gGame == GAME_NOT) {
    newRoundNot(now);
  } else if (gGame == GAME_HARD) {
    newRoundClassic(now, true);
  } else {
    newRoundClassic(now, false);
  }
}

static void updateReflexCountdown(uint32_t now) {
  uint32_t elapsed = now - G.roundStartMs;
  if (elapsed >= G.windowMs) { ledsCount(0); return; }
  uint32_t remain = G.windowMs - elapsed;
  ledsBarFromRemaining(remain, G.windowMs);
}

static void playingTick(uint32_t now) {
  if (gGame == GAME_CORD) {
    cordTick(now);
    return;
  }

  if (gGame == GAME_6OR7) {
    six7Tick(now);
  }

  updateReflexCountdown(now);
  if ((now - G.roundStartMs) >= G.windowMs) {
    enterGameOver(now);
  }
}

static void playingHandleDown(uint32_t now, uint8_t btn) {
  if (gGame == GAME_CORD) {
    cordHandleDown(now, btn);
    return;
  }

  if (gGame == GAME_6OR7) {
    six7HandleDown(now, btn);
    return;
  }

  // CLSC/HARD/NOT
  if (btn != G.expectedBtn) {
    enterGameOver(now);
    return;
  }

  G.score++;
  shrinkWindow();

  if (gGame == GAME_NOT) newRoundNot(now);
  else if (gGame == GAME_HARD) newRoundClassic(now, true);
  else newRoundClassic(now, false);
}

//
// =============================================================================
// Attract selector
// =============================================================================
//

static void attractTick(uint32_t now) {
  if (attractPulse.done(now)) {
    attractPulse.start(now, 900);
    attractShowHi = !attractShowHi;
    if (attractShowHi) showHi(G.hi[gGame]);
    else showText(GAME_LABELS[gGame]);
  }
}

static void attractHandleDown(uint32_t now, uint8_t btn) {
  if (btn == 1) {
    gGame = (GameId)((gGame == 0) ? (GAME_COUNT - 1) : (gGame - 1));
    showText(GAME_LABELS[gGame]);
  } else if (btn == 3) {
    gGame = (GameId)((gGame + 1) % GAME_COUNT);
    showText(GAME_LABELS[gGame]);
  } else if (btn == 2) {
    enterPlaying(now);
  }
  attractShowHi = false;
  attractPulse.start(now, 900);
}

//
// =============================================================================
// Gameover
// =============================================================================
//

static void gameOverTick(uint32_t now) {
  uint32_t e = now - gModeStartMs;

  if (e < GAMEOVER_SCORE_MS) {
    if (scoreBlinkTimer.done(now)) {
      scoreBlinkTimer.start(now, SCORE_BLINK_MS);
      scoreBlinkOn = !scoreBlinkOn;
      if (scoreBlinkOn) showNumber(G.score);
      else clearDisp();
    }
  } else if (e < (GAMEOVER_SCORE_MS + GAMEOVER_HI_MS)) {
    showHi(G.hi[gGame]);
  } else {
    enterAttract(now);
  }
}

//
// =============================================================================
// Setup / Loop
// =============================================================================
//

void setup() {
  MFS.initialize();
  randomSeed(analogRead(A0));

  // load highs
  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    G.hi[i] = eepromLoadHi((GameId)i);
  }

  // boot clear
  delay(BOOT_CLEAR_DELAY_MS);
  byte raw = MFS.getButton();
  if (raw) {
    uint8_t num = raw & B00111111;
    if (num == 1) {
      eepromClearAllHi();
      showText("HI 0");
      ledsCount(0);
      delay(BOOT_HI0_MS);
    }
  }

  enterAttract(millis());
}

void loop() {
  uint32_t now = millis();

  // 1) input
  readButtons();

  // 2) update
  switch (gMode) {
    case MODE_ATTRACT:
      attractTick(now);
      if (gBtn.hasDown) attractHandleDown(now, gBtn.num);
      break;

    case MODE_PLAYING:
      playingTick(now);
      if (gBtn.hasDown) playingHandleDown(now, gBtn.num);
      break;

    case MODE_GAMEOVER:
      gameOverTick(now);
      break;
  }
}
