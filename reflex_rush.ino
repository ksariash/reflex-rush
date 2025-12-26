#include <MultiFuncShield.h>
#include <EEPROM.h>

/*
  Reflex Rush (MultiFuncShield global MFS)
  ---------------------------------------
  ATTRACT selector:
    - S1: previous game
    - S3: next game
    - S2: start selected game (except brt)
    - brtN: brightness setting
        * Internal brightness (stored + sent to library): 0=brightest .. 3=dimmest
        * Display brightness shown as: 3=brightest .. 0=dimmest (more intuitive)
        * On brt screen: S2 cycles brightness DISPLAY value UP: 0->1->2->3->0
          (internally this maps to: 3->2->1->0->3)

  Games:
    CLSC - classic reflex 1..3
    HARD - digit appears in random position (start window 1000 ms)
    NOT  - shows NOxy, press the remaining button
    Ptrn - pattern memory; per-press 1s timeout with LED bar; show digits at positions: 1->pos0, 2->pos1, 3->pos2
    67   - trick 6/7 game:
           - intro (first 10 points): 6-left => S1, 7-right => S3 (brief LED hint)
           - after intro: random tricks; stage changes every 6 or 7 successes

  Game Over:
    - Score blinks for 2s, then HIxx for 2s, then back to ATTRACT.

  EEPROM:
    - per-game high score: uint16 at address 0 + 2*gameId. 0xFFFF treated as 0
    - brightness: uint8 stored after highs.

  Boot:
    - after ~200 ms, if S1 held -> clear all highs + brightness reset to max (internal 0),
      show "HI 0" for 1s then go to ATTRACT.

  Library note:
    Uses MFS.setDisplayBrightness(uint8_t) with 0=max, 3=min (as you specified).
*/

static const uint16_t ATTRACT_PULSE_MS      = 2000;   // 2 seconds

// Reflex timers
static const uint16_t WINDOW_START_MS       = 1800;   // CLSC / NOT starting window
static const uint16_t HARD_START_MS         = 1000;   // HARD starting window
static const uint16_t WINDOW_MIN_MS         = 360;
static const uint16_t WINDOW_STEP_MS        = 20;

static const uint16_t GAMEOVER_SCORE_MS     = 2000;
static const uint16_t GAMEOVER_HI_MS        = 2000;
static const uint16_t SCORE_BLINK_MS        = 300;

// Ptrn
static const uint16_t PTRN_TIMEOUT_MS       = 1000;
static const uint16_t PTRN_SHOW_STEP_MS     = 260;
static const uint16_t PTRN_GAP_MS           = 120;
static const uint8_t  PTRN_MAX_SEQ          = 20;

// Boot
static const uint16_t BOOT_CLEAR_DELAY_MS   = 200;
static const uint16_t BOOT_HI0_MS           = 1000;

// 67
static const uint16_t G67_INTRO_POINTS      = 10;
static const uint16_t G67_START_INTRO_MS    = 2600;
static const uint16_t G67_START_TRICK_MS    = 2100;
static const uint16_t G67_MIN_MS            = 520;
static const uint16_t G67_STEP_MS           = 30;

static const uint16_t G67_FLASH_MS          = 140;
static const uint16_t G67_GHOST_MS          = 90;
static const uint16_t G67_CHORD_WIN_MS      = 520;
static const uint16_t G67_SEQ_WIN_MS        = 950;

static const uint16_t G67_NOT_CUE_MS        = 350;
static const uint16_t G67_TEACH_LED_MS      = 220;

// Brightness internal for your library
static const uint8_t  BRT_INTERNAL_MAX      = 0;  // brightest
static const uint8_t  BRT_INTERNAL_MIN      = 3;  // dimmest

// EEPROM layout
static const int EEPROM_BASE_ADDR           = 0;

// =============================================================================
// Types
// =============================================================================

enum Mode : uint8_t { MODE_ATTRACT, MODE_PLAYING, MODE_GAMEOVER };

enum GameId : uint8_t {
  GAME_CLSC = 0,
  GAME_HARD,
  GAME_NOT,
  GAME_PTRN,
  GAME_67,
  GAME_BRT,
  GAME_COUNT
};

struct ButtonEvent {
  uint8_t num;     // 1..3
  bool hasDown;    // true only on BUTTON_PRESSED_IND
};

struct GameCommon {
  uint16_t score;
  uint16_t hi[GAME_COUNT]; // hi[GAME_BRT] unused

  uint16_t windowMs;
  uint32_t roundStartMs;

  uint8_t expectedBtn; // 1..3, 0=special (67 chord/seq)
  uint8_t auxA;
  uint8_t auxB;
};

struct Timer {
  uint32_t t0 = 0;
  uint32_t dur = 0;
  void start(uint32_t now, uint32_t duration) { t0 = now; dur = duration; }
  bool done(uint32_t now) const { return dur != 0 && (now - t0) >= dur; }
  uint32_t remain(uint32_t now) const {
    if (dur == 0) return 0;
    uint32_t e = now - t0;
    return (e >= dur) ? 0 : (dur - e);
  }
};

// 67 stages
enum G67Stage : uint8_t {
  G67_INTRO = 0,
  G67_SWAP,
  G67_NOT,
  G67_FLASH,
  G67_FLASH_NOT,
  G67_GHOST,
  G67_BLINK,
  G67_CHORD,
  G67_SEQ_67,
  G67_SEQ_76,
  G67_SHORT_WIN,
  G67_TRICK_MIX,
  G67_STAGE_COUNT
};

// Ptrn phases
enum PtrnPhase : uint8_t { PTRN_SHOWING, PTRN_INPUT };

// =============================================================================
// Globals
// =============================================================================

static Mode        gMode = MODE_ATTRACT;
static GameId      gGame = GAME_CLSC;

static GameCommon  G;
static ButtonEvent gBtn;

static uint32_t gModeStartMs = 0;

// Attract pulse
static Timer attractPulse;
static bool  attractShowHi = false;

// Gameover blink
static Timer scoreBlinkTimer;
static bool  scoreBlinkOn = true;

// Brightness internal: 0..3 (0 brightest)
static uint8_t gBrightnessInternal = BRT_INTERNAL_MAX;

// Ptrn state
static PtrnPhase ptrnPhase = PTRN_SHOWING;
static uint8_t   ptrnSeq[PTRN_MAX_SEQ];
static uint8_t   ptrnLen = 1;
static uint8_t   ptrnIndex = 0;
static Timer     ptrnShowTimer;
static uint8_t   ptrnShowPos = 0;
static bool      ptrnShowingDigit = true;
static Timer     ptrnInputTimer;

// 67 state
static uint8_t g67Stage = G67_INTRO;
static uint8_t g67Digit = 6;
static uint8_t g67Side  = 0;
static bool    g67NotFlag = false;

static bool    g67PendingNotCue = false;
static Timer   g67NotCueTimer;

static bool    g67FlashActive = false;
static Timer   g67FlashTimer;

static bool    g67BlinkActive = false;
static uint8_t g67BlinkCount = 0;
static Timer   g67BlinkTimer;

static bool    g67RoundChord = false;
static bool    g67ChordWaitingSecond = false;
static uint8_t g67ChordFirstBtn = 0;
static Timer   g67ChordTimer;

static bool    g67RoundSeq = false;
static bool    g67SeqWaitingSecond = false;
static uint8_t g67SeqFirstBtn = 0;
static Timer   g67SeqTimer;

static Timer   g67TeachLedTimer;
static bool    g67TeachLedOn = false;

static uint16_t g67NextStageAtScore = 0;

// =============================================================================
// EEPROM helpers
// =============================================================================

static int hiAddr(GameId id) { return EEPROM_BASE_ADDR + (int)id * 2; }
static int brtAddr() { return EEPROM_BASE_ADDR + (int)GAME_COUNT * 2; }

static uint16_t eepromLoadHi(GameId id) {
  uint16_t v;
  EEPROM.get(hiAddr(id), v);
  return (v == 0xFFFF) ? 0 : v;
}

static void eepromSaveHi(GameId id, uint16_t v) {
  EEPROM.put(hiAddr(id), v);
}

static uint8_t eepromLoadBrightnessInternal() {
  uint8_t v;
  EEPROM.get(brtAddr(), v);
  if (v == 0xFF || v > BRT_INTERNAL_MIN) return BRT_INTERNAL_MAX;
  return v;
}

static void eepromSaveBrightnessInternal(uint8_t v) {
  EEPROM.put(brtAddr(), v);
}

static void eepromClearAllHiAndBrightness() {
  uint16_t sentinel = 0xFFFF;
  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    EEPROM.put(hiAddr((GameId)i), sentinel);
    G.hi[i] = 0;
  }
  gBrightnessInternal = BRT_INTERNAL_MAX;
  eepromSaveBrightnessInternal(gBrightnessInternal);
}

// =============================================================================
// Display / LEDs
// =============================================================================

static void applyBrightness() {
  MFS.setDisplayBrightness(gBrightnessInternal); // 0=max, 3=min
}

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
  if (totalMs == 0 || remainMs == 0) { ledsCount(0); return; }
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
    b[0] = 'H'; b[1] = 'I';
    b[2] = '0' + (hi / 10);
    b[3] = '0' + (hi % 10);
    b[4] = 0;
    showText(b);
  } else {
    showNumber((int)hi);
  }
}

// Brightness shown as 3=brightest .. 0=dimmest
static uint8_t brtDisplayValue() { return (uint8_t)(3 - gBrightnessInternal); }

static void setBrightnessFromDisplay(uint8_t dispVal) {
  if (dispVal > 3) dispVal = 3;
  gBrightnessInternal = (uint8_t)(3 - dispVal);
}

static void showBrtLabel() {
  char b[5] = "brt0";
  b[3] = (char)('0' + brtDisplayValue());
  showText(b);
}

// =============================================================================
// Input (down-only)
// =============================================================================

static void readButtons() {
  gBtn.hasDown = false;
  byte raw = MFS.getButton();
  if (!raw) return;

  uint8_t num = raw & B00111111;
  uint8_t act = raw & B11000000;

  if (act == BUTTON_PRESSED_IND) {
    gBtn.hasDown = true;
    gBtn.num = num;
  }
}

// =============================================================================
// Common gameplay helpers
// =============================================================================

static void shrinkWindowGeneric() {
  if (G.windowMs <= WINDOW_MIN_MS) { G.windowMs = WINDOW_MIN_MS; return; }
  uint16_t nw = G.windowMs;
  if (nw > WINDOW_MIN_MS + WINDOW_STEP_MS) nw -= WINDOW_STEP_MS;
  else nw = WINDOW_MIN_MS;
  G.windowMs = nw;
}

static void shrinkWindow67() {
  if (G.windowMs <= G67_MIN_MS) { G.windowMs = G67_MIN_MS; return; }
  uint16_t nw = G.windowMs;
  if (nw > G67_MIN_MS + G67_STEP_MS) nw -= G67_STEP_MS;
  else nw = G67_MIN_MS;
  G.windowMs = nw;
}

static void startReflexRound(uint32_t now, uint8_t expectedBtn, uint16_t windowMs) {
  G.expectedBtn  = expectedBtn;
  G.roundStartMs = now;
  G.windowMs     = windowMs;
  ledsCount(4);
}

// =============================================================================
// Modes / labels
// =============================================================================

static const char* GAME_LABELS[GAME_COUNT] = {
  "CLSC",
  "HARD",
  "NOT ",
  "Ptrn",
  " 67 ",
  "brt0" // placeholder; rendered dynamically
};

static void enterAttract(uint32_t now) {
  gMode = MODE_ATTRACT;
  gModeStartMs = now;

  G.score = 0;
  ledsCount(0);

  if (gGame == GAME_BRT) showBrtLabel();
  else showText(GAME_LABELS[gGame]);

  attractPulse.start(now, ATTRACT_PULSE_MS);
  attractShowHi = false;
}

static void enterPlaying(uint32_t now);
static void enterGameOver(uint32_t now) {
  gMode = MODE_GAMEOVER;
  gModeStartMs = now;

  if (gGame != GAME_BRT) {
    if (G.score > G.hi[gGame]) {
      G.hi[gGame] = G.score;
      eepromSaveHi(gGame, G.hi[gGame]);
    }
  }

  ledsCount(0);
  scoreBlinkTimer.start(now, SCORE_BLINK_MS);
  scoreBlinkOn = true;
  showNumber(G.score);
}

// =============================================================================
// Games: CLSC / HARD / NOT
// =============================================================================

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

// =============================================================================
// Game: Ptrn
// =============================================================================

static void ptrnReset() {
  ptrnLen = 1;
  ptrnIndex = 0;
  for (uint8_t i = 0; i < PTRN_MAX_SEQ; i++) ptrnSeq[i] = 0;
  ptrnSeq[0] = (uint8_t)random(1, 4);
}

static void ptrnExtend() {
  if (ptrnLen < PTRN_MAX_SEQ) {
    ptrnSeq[ptrnLen] = (uint8_t)random(1, 4);
    ptrnLen++;
  }
}

static void ptrnBeginShow(uint32_t now) {
  ptrnPhase = PTRN_SHOWING;
  ptrnShowPos = 0;
  ptrnShowingDigit = true;
  ptrnShowTimer.start(now, PTRN_SHOW_STEP_MS);
  ledsCount(0);
}

static void ptrnBeginInput(uint32_t now) {
  ptrnPhase = PTRN_INPUT;
  ptrnIndex = 0;
  ptrnInputTimer.start(now, PTRN_TIMEOUT_MS);
  ledsCount(4);
}

static void ptrnShowDigitPos(uint8_t digit) {
  // 1->pos0, 2->pos1, 3->pos2
  char buf[5] = "    ";
  if (digit >= 1 && digit <= 3) {
    uint8_t pos = digit - 1;
    buf[pos] = (char)('0' + digit);
  }
  showText(buf);
}

static void ptrnTick(uint32_t now) {
  if (ptrnPhase == PTRN_SHOWING) {
    if (ptrnShowPos >= ptrnLen) {
      clearDisp();
      ptrnBeginInput(now);
      return;
    }

    if (ptrnShowTimer.done(now)) {
      if (ptrnShowingDigit) {
        clearDisp();
        ptrnShowingDigit = false;
        ptrnShowTimer.start(now, PTRN_GAP_MS);
      } else {
        ptrnShowPos++;
        ptrnShowingDigit = true;
        ptrnShowTimer.start(now, PTRN_SHOW_STEP_MS);
      }
    } else if (ptrnShowingDigit) {
      ptrnShowDigitPos(ptrnSeq[ptrnShowPos]);
    }
    return;
  }

  if (ptrnInputTimer.done(now)) {
    enterGameOver(now);
    return;
  }
  ledsBarFromRemaining(ptrnInputTimer.remain(now), PTRN_TIMEOUT_MS);
}

static void ptrnHandleDown(uint32_t now, uint8_t btn) {
  if (ptrnPhase != PTRN_INPUT) return;
  if (ptrnInputTimer.done(now)) { enterGameOver(now); return; }
  if (btn != ptrnSeq[ptrnIndex]) { enterGameOver(now); return; }

  ptrnIndex++;
  ptrnInputTimer.start(now, PTRN_TIMEOUT_MS);
  ledsCount(4);

  if (ptrnIndex >= ptrnLen) {
    G.score++;
    ptrnExtend();
    ptrnBeginShow(now);
  }
}

// =============================================================================
// Game: 67
// =============================================================================

static uint8_t btnForSide(uint8_t side) { return (side == 0) ? 1 : 3; }
static uint8_t oppSide(uint8_t side) { return (side == 0) ? 3 : 0; }
static uint8_t oppBtn(uint8_t b) { return (b == 1) ? 3 : (b == 3 ? 1 : 2); }

static uint8_t pickTrickStageWeighted() {
  uint8_t r = (uint8_t)random(0, 100);
  if (r < 25) return G67_TRICK_MIX;
  if (r < 43) return G67_FLASH_NOT;
  if (r < 57) return G67_NOT;
  if (r < 68) return G67_CHORD;
  if (r < 78) return G67_SEQ_67;
  if (r < 88) return G67_SEQ_76;
  if (r < 94) return G67_GHOST;
  if (r < 98) return G67_BLINK;
  return G67_SHORT_WIN;
}

static void g67ScheduleNextStageChange() {
  uint8_t step = (random(0, 2) == 0) ? 6 : 7;
  g67NextStageAtScore = (uint16_t)(G.score + step);
}

static void g67UpdateStageIfNeeded() {
  if (G.score < G67_INTRO_POINTS) {
    g67Stage = G67_INTRO;
    return;
  }
  if (g67Stage == G67_INTRO) {
    g67Stage = pickTrickStageWeighted();
    g67ScheduleNextStageChange();
    return;
  }
  if (G.score >= g67NextStageAtScore) {
    g67Stage = pickTrickStageWeighted();
    g67ScheduleNextStageChange();
  }
}

static void g67ShowNotCue(uint8_t digit, uint32_t now) {
  char buf[5] = "not6";
  buf[3] = (digit == 7) ? '7' : '6';
  showText(buf);
  g67PendingNotCue = true;
  g67NotCueTimer.start(now, G67_NOT_CUE_MS);
}

static void g67ShowSingle(uint8_t digit, uint8_t side) {
  char buf[5] = "    ";
  buf[side] = (char)('0' + digit);
  showText(buf);
}

static void g67ShowChord(uint32_t now) {
  // show 6 on left and 7 on right; player must press two different buttons within window
  char buf[5] = "    ";
  buf[0] = '6'; buf[3] = '7';
  showText(buf);

  g67RoundChord = true;
  g67RoundSeq = false;
  g67ChordWaitingSecond = false;
  g67ChordFirstBtn = 0;
  g67ChordTimer.start(now, G67_CHORD_WIN_MS);
}

static void g67ShowSeq(uint32_t now, uint8_t firstDigit, uint8_t secondDigit) {
  // show first digit at pos2 (index 1), then second at pos3 (index 2) after a short delay
  char buf[5] = "    ";
  buf[1] = (char)('0' + firstDigit);
  showText(buf);

  g67FlashActive = true;
  g67FlashTimer.start(now, 220);

  g67RoundSeq = true;
  g67RoundChord = false;

  g67SeqWaitingSecond = false;
  g67SeqFirstBtn = (firstDigit == 6) ? 1 : 3;
  G.auxA = (secondDigit == 6) ? 1 : 3;
  g67SeqTimer.start(now, G67_SEQ_WIN_MS);
}

static void g67BeginRound(uint32_t now) {
  g67UpdateStageIfNeeded();

  g67FlashActive = false;
  g67BlinkActive = false;
  g67PendingNotCue = false;
  g67RoundChord = false;
  g67RoundSeq = false;
  g67NotFlag = false;

  bool showSix = (random(0, 2) == 0);
  g67Digit = showSix ? 6 : 7;
  uint8_t baseSide = showSix ? 0 : 3;

  uint16_t w;
  if (g67Stage == G67_INTRO) {
    w = (G.score == 0) ? G67_START_INTRO_MS : G.windowMs;
  } else {
    w = (G.score == G67_INTRO_POINTS) ? G67_START_TRICK_MS : G.windowMs;
  }

  if (g67Stage == G67_INTRO) {
    g67Side = baseSide;
    G.expectedBtn = btnForSide(baseSide);

    g67ShowSingle(g67Digit, baseSide);

    // quick LED teaching hint (left LED for 6, right LED for 7)
    MFS.writeLeds(LED_ALL, OFF);
    MFS.writeLeds((g67Digit == 6) ? LED_1 : LED_4, ON);
    g67TeachLedOn = true;
    g67TeachLedTimer.start(now, G67_TEACH_LED_MS);

    startReflexRound(now, G.expectedBtn, w);
    return;
  }

  bool swap = false, notMap = false, flash = false, ghost = false, blink = false;
  bool chord = false, seq = false, seq76 = false, shortWin = false, trickMix = false;

  switch (g67Stage) {
    case G67_SWAP: swap = true; break;
    case G67_NOT: notMap = true; break;
    case G67_FLASH: flash = true; break;
    case G67_FLASH_NOT: flash = true; notMap = true; break;
    case G67_GHOST: ghost = true; break;
    case G67_BLINK: blink = true; break;
    case G67_CHORD: chord = true; break;
    case G67_SEQ_67: seq = true; seq76 = false; break;
    case G67_SEQ_76: seq = true; seq76 = true; break;
    case G67_SHORT_WIN: shortWin = true; break;
    case G67_TRICK_MIX: trickMix = true; break;
    default: break;
  }

  if (trickMix) {
    uint8_t r = (uint8_t)random(0, 100);
    if (r < 22) notMap = true;
    else if (r < 44) { flash = true; notMap = true; }
    else if (r < 58) chord = true;
    else if (r < 78) { seq = true; seq76 = (random(0,2)==0); }
    else if (r < 90) ghost = true;
    else { shortWin = true; blink = true; }
    if (random(0, 5) == 0) swap = true;
  }

  if (shortWin) w = (uint16_t)(w * 0.62f);

  if (chord) {
    g67ShowChord(now);
    startReflexRound(now, 0, w);
    return;
  }

  if (seq) {
    uint8_t firstD  = seq76 ? 7 : 6;
    uint8_t secondD = seq76 ? 6 : 7;
    if (swap) { uint8_t t = firstD; firstD = secondD; secondD = t; }
    g67ShowSeq(now, firstD, secondD);
    startReflexRound(now, 0, w);
    return;
  }

  // normal 67 mapping round
  if (swap) baseSide = oppSide(baseSide);
  g67Side = baseSide;

  uint8_t baseBtn = btnForSide(baseSide);
  if (notMap) { g67NotFlag = true; baseBtn = oppBtn(baseBtn); }

  if (blink) {
    g67BlinkActive = true;
    g67BlinkCount = 0;
    g67BlinkTimer.start(now, 140);
  }

  if (g67NotFlag) g67ShowNotCue(g67Digit, now);
  else g67ShowSingle(g67Digit, baseSide);

  if (!g67NotFlag && (flash || ghost)) {
    g67FlashActive = true;
    g67FlashTimer.start(now, ghost ? G67_GHOST_MS : G67_FLASH_MS);
  }

  G.expectedBtn = baseBtn;
  startReflexRound(now, G.expectedBtn, w);
}

static void g67Tick(uint32_t now) {
  if (g67TeachLedOn && g67TeachLedTimer.done(now)) {
    g67TeachLedOn = false;
    MFS.writeLeds(LED_ALL, OFF);
  }

  if (g67PendingNotCue && g67NotCueTimer.done(now)) {
    g67PendingNotCue = false;
    g67ShowSingle(g67Digit, g67Side);
  }

  if (g67FlashActive && g67FlashTimer.done(now)) {
    g67FlashActive = false;
    clearDisp();
  }

  if (g67BlinkActive && g67BlinkTimer.done(now)) {
    g67BlinkTimer.start(now, 140);
    g67BlinkCount++;
    if (g67BlinkCount <= 4) {
      if (g67BlinkCount % 2 == 1) clearDisp();
      else g67ShowSingle(g67Digit, g67Side);
    } else {
      g67BlinkActive = false;
      g67ShowSingle(g67Digit, g67Side);
    }
  }
}

static void g67HandleDown(uint32_t now, uint8_t btn) {
  // chord: two different buttons within window
  if (g67RoundChord) {
    if (!g67ChordWaitingSecond) {
      g67ChordWaitingSecond = true;
      g67ChordFirstBtn = btn;
      g67ChordTimer.start(now, G67_CHORD_WIN_MS);
      return;
    } else {
      if (g67ChordTimer.done(now)) { enterGameOver(now); return; }
      if (btn == g67ChordFirstBtn) { enterGameOver(now); return; }
      bool ok = ((g67ChordFirstBtn == 1 && btn == 3) || (g67ChordFirstBtn == 3 && btn == 1));
      if (!ok) { enterGameOver(now); return; }

      G.score++;
      shrinkWindow67();
      g67BeginRound(now);
      return;
    }
  }

  // seq: press first then second within window (we reveal 2nd after first press)
  if (g67RoundSeq) {
    if (!g67SeqWaitingSecond) {
      if (btn != g67SeqFirstBtn) { enterGameOver(now); return; }
      g67SeqWaitingSecond = true;
      g67SeqTimer.start(now, G67_SEQ_WIN_MS);

      char buf[5] = "    ";
      buf[2] = (char)('0' + ((G.auxA == 1) ? 6 : 7));
      showText(buf);

      g67FlashActive = true;
      g67FlashTimer.start(now, 220);
      return;
    } else {
      if (g67SeqTimer.done(now)) { enterGameOver(now); return; }
      if (btn != G.auxA) { enterGameOver(now); return; }

      G.score++;
      shrinkWindow67();
      g67BeginRound(now);
      return;
    }
  }

  // normal mapping
  if (btn != G.expectedBtn) { enterGameOver(now); return; }
  G.score++;
  shrinkWindow67();
  g67BeginRound(now);
}

// =============================================================================
// Playing / Attract / Gameover
// =============================================================================

static void enterPlaying(uint32_t now) {
  gMode = MODE_PLAYING;
  gModeStartMs = now;

  G.score = 0;
  G.expectedBtn = 0;
  G.auxA = G.auxB = 0;

  if (gGame == GAME_67) {
    G.windowMs = G67_START_INTRO_MS;
    g67Stage = G67_INTRO;
    g67NextStageAtScore = 0;
    g67BeginRound(now);
    return;
  }

  if (gGame == GAME_PTRN) {
    G.windowMs = WINDOW_START_MS;
    ptrnReset();
    ptrnBeginShow(now);
    return;
  }

  if (gGame == GAME_NOT) {
    G.windowMs = WINDOW_START_MS;
    newRoundNot(now);
    return;
  }

  if (gGame == GAME_HARD) {
    G.windowMs = WINDOW_START_MS;
    newRoundClassic(now, true);
    return;
  }

  if (gGame == GAME_CLSC) {
    G.windowMs = WINDOW_START_MS;
    newRoundClassic(now, false);
    return;
  }

  // brt not playable
  enterAttract(now);
}

static void updateReflexCountdown(uint32_t now) {
  uint32_t elapsed = now - G.roundStartMs;
  if (elapsed >= G.windowMs) { ledsCount(0); return; }
  uint32_t remain = G.windowMs - elapsed;
  ledsBarFromRemaining(remain, G.windowMs);
}

static void playingTick(uint32_t now) {
  if (gGame == GAME_PTRN) {
    ptrnTick(now);
    return;
  }

  if (gGame == GAME_67) {
    g67Tick(now);
  }

  updateReflexCountdown(now);
  if ((now - G.roundStartMs) >= G.windowMs) {
    enterGameOver(now);
  }
}

static void playingHandleDown(uint32_t now, uint8_t btn) {
  if (gGame == GAME_PTRN) { ptrnHandleDown(now, btn); return; }
  if (gGame == GAME_67)   { g67HandleDown(now, btn); return; }

  if (btn != G.expectedBtn) { enterGameOver(now); return; }

  G.score++;
  shrinkWindowGeneric();

  if (gGame == GAME_NOT) newRoundNot(now);
  else if (gGame == GAME_HARD) newRoundClassic(now, true);
  else newRoundClassic(now, false);
}

static void attractTick(uint32_t now) {
  if (gGame == GAME_BRT) {
    showBrtLabel();
    return;
  }

  if (attractPulse.done(now)) {
    attractPulse.start(now, ATTRACT_PULSE_MS);
    attractShowHi = !attractShowHi;
    if (attractShowHi) showHi(G.hi[gGame]);
    else showText(GAME_LABELS[gGame]);
  }
}

static void attractHandleDown(uint32_t now, uint8_t btn) {
  (void)now;

  if (gGame == GAME_BRT) {
    // S2 counts UP on DISPLAY: 0->1->2->3->0
    if (btn == 2) {
      uint8_t disp = brtDisplayValue();     // 0..3 (3=brightest)
      disp = (uint8_t)((disp + 1) & 0x03);  // wrap
      setBrightnessFromDisplay(disp);       // updates internal
      applyBrightness();
      eepromSaveBrightnessInternal(gBrightnessInternal);
      showBrtLabel();
      return;
    }

    // navigation with S1/S3
    if (btn == 1) gGame = (GameId)((gGame == 0) ? (GAME_COUNT - 1) : (gGame - 1));
    else if (btn == 3) gGame = (GameId)((gGame + 1) % GAME_COUNT);

    if (gGame == GAME_BRT) showBrtLabel();
    else showText(GAME_LABELS[gGame]);

    attractShowHi = false;
    attractPulse.start(millis(), ATTRACT_PULSE_MS);
    return;
  }

  // normal selector
  if (btn == 1) gGame = (GameId)((gGame == 0) ? (GAME_COUNT - 1) : (gGame - 1));
  else if (btn == 3) gGame = (GameId)((gGame + 1) % GAME_COUNT);
  else if (btn == 2) { enterPlaying(millis()); return; }

  attractShowHi = false;
  attractPulse.start(millis(), ATTRACT_PULSE_MS);

  if (gGame == GAME_BRT) showBrtLabel();
  else showText(GAME_LABELS[gGame]);
}

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
    if (gGame != GAME_BRT) showHi(G.hi[gGame]);
  } else {
    enterAttract(now);
  }
}

// =============================================================================
// Setup / Loop
// =============================================================================

void setup() {
  MFS.initialize();
  randomSeed(analogRead(A0));

  // highs
  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    G.hi[i] = eepromLoadHi((GameId)i);
  }

  // brightness
  gBrightnessInternal = eepromLoadBrightnessInternal();
  applyBrightness();

  // boot clear: hold S1
  delay(BOOT_CLEAR_DELAY_MS);
  byte raw = MFS.getButton();
  if (raw) {
    uint8_t num = raw & B00111111;
    if (num == 1) {
      eepromClearAllHiAndBrightness();
      applyBrightness();
      showText("HI 0");
      ledsCount(0);
      delay(BOOT_HI0_MS);
    }
  }

  enterAttract(millis());
}

void loop() {
  uint32_t now = millis();

  readButtons();

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
