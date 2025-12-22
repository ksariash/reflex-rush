#include <MultiFuncShield.h>
#include <EEPROM.h>

/*
  Reflex Rush - Clean Rewrite (Party Edition)
  Board: Arduino Uno R3
  Shield: Multi-Function Shield
  Library: MultiFuncShield (global MFS: initialize(), write(), writeLeds(), getButton(), ...)
*/

// =============================================================================
// Constants
// =============================================================================

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

static const uint16_t SIX7_FLASH_MS        = 150;    // short flash modes
static const uint16_t SIX7_CHORD_WINDOW_MS = 450;    // chord entry window for 6OR7 chord stages
static const uint16_t SIX7_SEQ_WINDOW_MS   = 900;    // sequence stages: time to hit second press

static const int EEPROM_BASE_ADDR          = 0;      // each game: 2 bytes (uint16_t)

// =============================================================================
// Types
// =============================================================================

enum Mode : uint8_t { MODE_ATTRACT, MODE_PLAYING, MODE_GAMEOVER };

enum GameId : uint8_t {
  GAME_CLSC = 0,
  GAME_HARD,
  GAME_NOT,
  GAME_CORD,
  GAME_6OR7,
  GAME_COUNT
};

enum BtnAction : uint8_t {
  BTN_NONE = 0,
  BTN_DOWN,
  BTN_UP
};

struct ButtonEvent {
  uint8_t num;      // 1..3
  BtnAction act;
  bool has;
};

struct GameCommon {
  uint16_t score;
  uint16_t hi[GAME_COUNT];
  uint16_t windowMs;
  uint32_t roundStartMs;

  // generic per-round “expected” for single-press style
  uint8_t expectedBtn;   // 1..3 (0 means unused)
  uint8_t forbidA;       // NOT game
  uint8_t forbidB;       // NOT game
};

struct Timer {
  uint32_t t0;
  uint32_t dur;
  void start(uint32_t now, uint32_t duration) { t0 = now; dur = duration; }
  uint32_t elapsed(uint32_t now) const { return now - t0; }
  bool done(uint32_t now) const { return (now - t0) >= dur; }
  uint32_t remain(uint32_t now) const {
    uint32_t e = now - t0;
    return (e >= dur) ? 0 : (dur - e);
  }
};

// =============================================================================
// Globals
// =============================================================================

static Mode     gMode = MODE_ATTRACT;
static GameId   gGame = GAME_CLSC;

static GameCommon G;
static ButtonEvent gBtn;

static uint32_t gModeStartMs = 0;

// Attract scroll (tiny polish)
static Timer attractPulse;
static bool  attractShowHi = false;

// Gameover blink
static Timer scoreBlinkTimer;
static bool  scoreBlinkOn = true;

// --- CORD (memory) state ---
enum CordPhase : uint8_t { CORD_SHOWING, CORD_INPUT };
static CordPhase cordPhase = CORD_SHOWING;
static uint8_t  cordSeq[CORD_MAX_SEQ];
static uint8_t  cordLen = 1;
static uint8_t  cordIndex = 0;
static Timer    cordShowTimer;
static uint8_t  cordShowPos = 0;    // which element showing
static bool     cordShowingDigit = true;
static Timer    cordInputTimer;     // per-press timeout

// --- 6OR7 state ---
enum Six7Stage : uint8_t {
  S7_NORMAL = 0,
  S7_SWAP,
  S7_NOT,            // invert mapping (indicator 'n')
  S7_FLASH,          // show briefly then blank
  S7_FLASH_NOT,
  S7_MID65,          // sometimes show 6.5 -> middle button
  S7_MID65_NOT,
  S7_CHORD,          // show 6 & 7 ends -> chord (left+right)
  S7_CHORD_SWAP,
  S7_SEQ_67,         // show 6 then 7 -> press in order
  S7_SEQ_76,         // show 7 then 6 -> press in order
  S7_SHORT_WINDOW,   // window is cut (~60%) this stage
  S7_BLINK,          // digit blinks twice before staying
  S7_GHOST,          // ultra-brief flash
  S7_TRICK_MIX,      // “party mix” weighted random within stage
  S7_COUNT
};

static uint8_t six7Stage = S7_NORMAL;
static uint8_t six7StageBlock = 0xFF;   // score/5 bucket
static uint8_t six7RoundKind = 0;       // internal: SINGLE/MID/CHORD/SEQ
static uint8_t six7ShowDigit = 6;       // 6 or 7
static uint8_t six7DisplaySide = 0;     // 0=left, 3=right
static bool    six7Not = false;
static bool    six7Flash = false;
static bool    six7Blink = false;
static uint8_t six7BlinkCount = 0;
static Timer   six7FlashTimer;

// chord entry
static bool    six7ChordWaitingSecond = false;
static uint8_t six7ChordFirstBtn = 0;
static Timer   six7ChordTimer;

// seq entry
static bool    six7SeqWaitingSecond = false;
static uint8_t six7SeqFirstBtn = 0;
static Timer   six7SeqTimer;

// =============================================================================
// Helpers: EEPROM high score
// =============================================================================

static int hiAddr(GameId id) { return EEPROM_BASE_ADDR + (int)id * 2; }

static uint16_t eepromLoadHi(GameId id) {
  uint16_t v;
  EEPROM.get(hiAddr(id), v);
  if (v == 0xFFFF) return 0;
  return v;
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

// =============================================================================
// Helpers: LEDs & Display
// =============================================================================

static void ledsCount(uint8_t n) {
  byte mask = 0;
  if (n >= 1) mask |= LED_1;
  if (n >= 2) mask |= LED_2;
  if (n >= 3) mask |= LED_3;
  if (n >= 4) mask |= LED_4;
  MFS.writeLeds(LED_ALL, OFF);
  if (mask) MFS.writeLeds(mask, ON);
}

static void ledsBarFromFraction(float fracRemaining) {
  if (fracRemaining <= 0.0f) { ledsCount(0); return; }
  if (fracRemaining >= 1.0f) { ledsCount(4); return; }
  // map remaining fraction -> 4..1
  if (fracRemaining > 0.75f) ledsCount(4);
  else if (fracRemaining > 0.50f) ledsCount(3);
  else if (fracRemaining > 0.25f) ledsCount(2);
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

// =============================================================================
// Input: Button Manager
// =============================================================================

static void readButtons() {
  gBtn.has = false;
  byte raw = MFS.getButton();
  if (!raw) return;

  uint8_t num = raw & B00111111;
  uint8_t act = raw & B11000000;

  gBtn.num = num;
  if (act == BUTTON_PRESSED_IND) gBtn.act = BTN_DOWN;
  else if (act == BUTTON_RELEASED_IND) gBtn.act = BTN_UP;
  else gBtn.act = BTN_NONE;

  gBtn.has = true;
}

// =============================================================================
// Mode transitions
// =============================================================================

static const char* GAME_LABELS[GAME_COUNT] = { "CLSC", "HARD", "NOT ", "CORD", "6OR7" };

static void enterAttract(uint32_t now) {
  gMode = MODE_ATTRACT;
  gModeStartMs = now;

  G.score = 0;
  G.windowMs = WINDOW_START_MS;
  G.roundStartMs = now;

  ledsCount(0);
  showText(GAME_LABELS[gGame]);

  attractPulse.start(now, 900);
  attractShowHi = false;
}

static void enterGameOver(uint32_t now) {
  gMode = MODE_GAMEOVER;
  gModeStartMs = now;

  // update high score for current game
  if (G.score > G.hi[gGame]) {
    G.hi[gGame] = G.score;
    eepromSaveHi(gGame, G.hi[gGame]);
  }

  ledsCount(0);
  scoreBlinkTimer.start(now, SCORE_BLINK_MS);
  scoreBlinkOn = true;
  showNumber(G.score);
}

static void startReflexRound(uint32_t now, uint8_t expectedBtn, uint16_t windowMs) {
  G.expectedBtn = expectedBtn;
  G.roundStartMs = now;
  G.windowMs = windowMs;
  ledsCount(4);
}

static void shrinkWindow() {
  if (G.windowMs <= WINDOW_MIN_MS) { G.windowMs = WINDOW_MIN_MS; return; }
  uint16_t nw = G.windowMs;
  if (nw > WINDOW_MIN_MS + WINDOW_STEP_MS) nw -= WINDOW_STEP_MS;
  else nw = WINDOW_MIN_MS;
  G.windowMs = nw;
}

// =============================================================================
// Game: CLSC / HARD
// =============================================================================

static void newRoundClassic(uint32_t now, bool hard) {
  uint8_t d = (uint8_t)random(1, 4); // 1..3

  if (!hard) {
    showNumber(d);
  } else {
    // random position
    char buf[5] = "    ";
    uint8_t p = (uint8_t)random(0, 4);
    buf[p] = (char)('0' + d);
    showText(buf);
  }

  uint16_t startW = hard ? HARD_START_MS : WINDOW_START_MS;
  if (G.score == 0) startReflexRound(now, d, startW);
  else startReflexRound(now, d, G.windowMs); // keep shrinking
}

// =============================================================================
// Game: NOT
// =============================================================================

static void newRoundNot(uint32_t now) {
  uint8_t a = (uint8_t)random(1, 4);
  uint8_t b;
  do { b = (uint8_t)random(1, 4); } while (b == a);
  if (a > b) { uint8_t t = a; a = b; b = t; }

  G.forbidA = a;
  G.forbidB = b;

  char buf[5] = "NO12";
  buf[2] = '0' + a;
  buf[3] = '0' + b;
  showText(buf);

  // expected is the remaining button
  uint8_t exp = 1;
  for (uint8_t k = 1; k <= 3; k++) if (k != a && k != b) exp = k;
  startReflexRound(now, exp, (G.score == 0) ? WINDOW_START_MS : G.windowMs);
}

// =============================================================================
// Game: CORD (memory, 1s per-press timeout with LEDs)
// =============================================================================

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
  // LEDs start full
  ledsCount(4);
}

static void cordRenderTimeout(uint32_t now) {
  float frac = (float)cordInputTimer.remain(now) / (float)CORD_TIMEOUT_MS;
  ledsBarFromFraction(frac);
}

static void cordTick(uint32_t now) {
  if (cordPhase == CORD_SHOWING) {
    // show one digit in position 2 (index 1)
    if (cordShowPos >= cordLen) {
      clearDisp();
      cordBeginInput(now);
      return;
    }

    if (cordShowTimer.done(now)) {
      // toggle show/gap
      if (cordShowingDigit) {
        // go to gap
        clearDisp();
        cordShowingDigit = false;
        cordShowTimer.start(now, CORD_GAP_MS);
      } else {
        // next digit
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
  } else {
    // input phase
    if (cordInputTimer.done(now)) {
      // timed out between presses
      enterGameOver(now);
      return;
    }
    cordRenderTimeout(now);
  }
}

static bool cordHandleButton(uint32_t now, const ButtonEvent& e) {
  if (cordPhase != CORD_INPUT) return false;
  if (!e.has || e.act != BTN_DOWN) return false;

  uint8_t want = cordSeq[cordIndex];
  if (e.num != want) {
    enterGameOver(now);
    return true;
  }

  // correct
  cordIndex++;
  cordInputTimer.start(now, CORD_TIMEOUT_MS); // reset 1s timeout
  ledsCount(4);                                // refill LEDs immediately

  if (cordIndex >= cordLen) {
    // completed sequence
    G.score++;
    cordExtend();
    cordBeginShow(now);
  }
  return true;
}

// =============================================================================
// Game: 6OR7 (15 challenge stages, switch every 5 correct)
// =============================================================================

static void six7PickNewStage() {
  // stage 0 = always normal, after that random among 1..(S7_COUNT-1)
  if ((G.score / 5) == 0) {
    six7Stage = S7_NORMAL;
  } else {
    six7Stage = (uint8_t)random(1, S7_COUNT);
  }
}

static void six7UpdateStageIfNeeded() {
  uint8_t block = (uint8_t)(G.score / 5);
  if (block != six7StageBlock) {
    six7StageBlock = block;
    six7PickNewStage();
  }
}

// Utility: show 6 on left or 7 on right (but stages can swap)
static void six7ShowSingle(uint8_t digit, uint8_t side, bool notFlag, bool blankAfterFlash, uint16_t flashMs, uint32_t now) {
  char buf[5] = "    ";
  buf[side] = (char)('0' + digit);
  if (notFlag) {
    // place 'n' near center as a “not” hint
    if (side == 0) buf[1] = 'n';
    else buf[2] = 'n';
  }
  showText(buf);

  if (blankAfterFlash) {
    six7Flash = true;
    six7FlashTimer.start(now, flashMs);
  } else {
    six7Flash = false;
  }
}

static void six7ShowMid65(bool notFlag, bool blankAfterFlash, uint16_t flashMs, uint32_t now) {
  char buf[5] = "    ";
  buf[0] = '6';
  buf[1] = '.';
  buf[2] = '5';
  if (notFlag) buf[3] = 'n';
  showText(buf);

  if (blankAfterFlash) {
    six7Flash = true;
    six7FlashTimer.start(now, flashMs);
  } else {
    six7Flash = false;
  }
}

static void six7ShowChord(uint32_t now, bool swap) {
  // show 6 and 7 on ends
  char buf[5] = "    ";
  if (!swap) { buf[0] = '6'; buf[3] = '7'; }
  else       { buf[0] = '7'; buf[3] = '6'; }
  showText(buf);
  six7Flash = false;
  six7ChordWaitingSecond = false;
  six7ChordFirstBtn = 0;
  six7ChordTimer.start(now, SIX7_CHORD_WINDOW_MS);
}

static void six7ShowSeq(uint32_t now, uint8_t firstDigit) {
  // show first digit briefly, then blank; expect first press, then second press within window
  char buf[5] = "    ";
  buf[1] = (char)('0' + firstDigit);
  showText(buf);
  six7Flash = true;
  six7FlashTimer.start(now, 220);
  six7SeqWaitingSecond = false;
  six7SeqFirstBtn = 0;
  six7SeqTimer.start(now, SIX7_SEQ_WINDOW_MS);
}

static uint8_t six7BtnForSide(uint8_t side) { return (side == 0) ? 1 : 3; }
static uint8_t six7OppBtn(uint8_t b) { return (b == 1) ? 3 : (b == 3 ? 1 : 2); }

static void six7BeginRound(uint32_t now) {
  six7UpdateStageIfNeeded();

  // reset per-round submodes
  six7Flash = false;
  six7Not = false;
  six7Blink = false;
  six7BlinkCount = 0;
  six7ChordWaitingSecond = false;
  six7SeqWaitingSecond = false;

  // pick base digit and side (base: 6 left OR 7 right)
  bool showSix = (random(0, 2) == 0);
  six7ShowDigit = showSix ? 6 : 7;

  // base side: 6 left, 7 right
  uint8_t baseSide = showSix ? 0 : 3;

  // stage behavior knobs
  bool swapSides = false;
  bool invertMap = false;
  bool canMid65  = false;
  bool doFlash   = false;
  bool doGhost   = false;
  bool doBlink   = false;
  bool doChord   = false;
  bool doSeq     = false;
  bool seq76     = false;
  bool shortWin  = false;
  bool partyMix  = false;

  switch (six7Stage) {
    case S7_NORMAL: break;
    case S7_SWAP: swapSides = true; break;
    case S7_NOT: invertMap = true; break;
    case S7_FLASH: doFlash = true; break;
    case S7_FLASH_NOT: doFlash = true; invertMap = true; break;
    case S7_MID65: canMid65 = true; break;
    case S7_MID65_NOT: canMid65 = true; invertMap = true; break;
    case S7_CHORD: doChord = true; break;
    case S7_CHORD_SWAP: doChord = true; swapSides = true; break;
    case S7_SEQ_67: doSeq = true; seq76 = false; break;
    case S7_SEQ_76: doSeq = true; seq76 = true; break;
    case S7_SHORT_WINDOW: shortWin = true; break;
    case S7_BLINK: doBlink = true; break;
    case S7_GHOST: doGhost = true; break;
    case S7_TRICK_MIX: partyMix = true; break;
    default: break;
  }

  // “TRICK MIX”: weighted random sub-features each round
  if (partyMix) {
    uint8_t r = (uint8_t)random(0, 100);
    if (r < 15) swapSides = true;
    else if (r < 30) invertMap = true;
    else if (r < 45) doFlash = true;
    else if (r < 60) canMid65 = true;
    else if (r < 72) doBlink = true;
    else if (r < 84) doChord = true;
    else if (r < 95) doSeq = true;
    else doGhost = true;
  }

  // decide if mid65 overrides this round (about 1/3 chance if enabled)
  if (canMid65 && random(0, 3) == 0) {
    // mid case expects middle button unless inverted (then NOT means “not middle” -> choose left/right opposite of a hidden rule)
    if (!invertMap) {
      G.expectedBtn = 2;
      six7ShowMid65(false, doFlash || doGhost, doGhost ? 90 : SIX7_FLASH_MS, now);
    } else {
      // "not 6.5" = press a side (random but consistent display hint)
      bool pickLeft = (random(0, 2) == 0);
      G.expectedBtn = pickLeft ? 1 : 3;
      six7ShowMid65(true, doFlash || doGhost, doGhost ? 90 : SIX7_FLASH_MS, now);
    }
    // window
    uint16_t startW = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
    if (shortWin) startW = (uint16_t)(startW * 0.60f);
    startReflexRound(now, G.expectedBtn, startW);
    return;
  }

  // chord round
  if (doChord) {
    // chord always expects left+right within CHORD window
    // show ends (maybe swapped)
    six7ShowChord(now, swapSides);
    // expectedBtn=0 indicates "special handling"
    G.expectedBtn = 0;
    uint16_t startW = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
    if (shortWin) startW = (uint16_t)(startW * 0.60f);
    startReflexRound(now, 0, startW);
    return;
  }

  // sequence round
  if (doSeq) {
    // show two-step: first digit then second digit
    // 6->btn1, 7->btn3 (unless swapSides toggles mapping by swapping digits’ “home sides”)
    uint8_t firstD = seq76 ? 7 : 6;
    uint8_t secondD = seq76 ? 6 : 7;

    // if swapped, swap what appears first/second (keeps it intuitive)
    if (swapSides) { uint8_t t = firstD; firstD = secondD; secondD = t; }

    six7ShowSeq(now, firstD);
    // store expectation in special variables; use expectedBtn=0 to indicate special
    G.expectedBtn = 0;
    // encode in globals
    six7SeqFirstBtn = (firstD == 6) ? 1 : 3;
    // second expected button:
    // stash in forbidA as "second"
    G.forbidA = (secondD == 6) ? 1 : 3;

    uint16_t startW = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
    if (shortWin) startW = (uint16_t)(startW * 0.60f);
    startReflexRound(now, 0, startW);
    return;
  }

  // single-digit round
  if (swapSides) baseSide = (baseSide == 0) ? 3 : 0;
  six7DisplaySide = baseSide;

  // base mapping: press side where digit is displayed
  uint8_t baseBtn = six7BtnForSide(baseSide);

  // invert mapping: "NOT" means opposite side
  if (invertMap) {
    six7Not = true;
    baseBtn = six7OppBtn(baseBtn);
  } else {
    six7Not = false;
  }

  // blink stage: blink twice then stay (still uses same expectation)
  if (doBlink) {
    six7Blink = true;
    six7BlinkCount = 0;
  }

  // flash/ghost stages
  bool blankAfter = doFlash || doGhost;
  uint16_t flashMs = doGhost ? 90 : SIX7_FLASH_MS;

  G.expectedBtn = baseBtn;
  six7ShowSingle(six7ShowDigit, baseSide, six7Not, blankAfter, flashMs, now);

  uint16_t startW = (G.score == 0) ? WINDOW_START_MS : G.windowMs;
  if (shortWin) startW = (uint16_t)(startW * 0.60f);
  startReflexRound(now, G.expectedBtn, startW);
}

static void six7Tick(uint32_t now) {
  // handle flash blanking
  if (six7Flash && six7FlashTimer.done(now)) {
    six7Flash = false;
    clearDisp();
  }

  // handle blink
  if (six7Blink) {
    // blink twice: on/off/on/off then steady
    static Timer blinkT;
    static bool blinkInit = false;
    if (!blinkInit) { blinkT.start(now, 140); blinkInit = true; }
    if (blinkT.done(now)) {
      blinkT.start(now, 140);
      six7BlinkCount++;

      if (six7BlinkCount <= 4) {
        // toggle display
        if (six7BlinkCount % 2 == 1) clearDisp();
        else {
          // redraw the single pattern
          six7ShowSingle(six7ShowDigit, six7DisplaySide, six7Not, false, 0, now);
        }
      } else {
        // finished blinking, leave steady
        six7Blink = false;
        blinkInit = false;
        six7ShowSingle(six7ShowDigit, six7DisplaySide, six7Not, false, 0, now);
      }
    }
  }
}

static bool six7HandleButton(uint32_t now, const ButtonEvent& e) {
  if (!e.has || e.act != BTN_DOWN) return false;

  // chord stage active if expectedBtn==0 and display shows chord pattern (we can infer by stage)
  if (six7Stage == S7_CHORD || six7Stage == S7_CHORD_SWAP || six7Stage == S7_TRICK_MIX) {
    // chord only if current round was chord: we track by six7ChordTimer duration start plus expectedBtn==0 AND not seq
    // Distinguish chord vs seq by whether six7SeqTimer is “running” since round start; we’ll use flags:
    if (six7SeqTimer.dur == 0 || (now - six7SeqTimer.t0) > six7SeqTimer.dur + 5) {
      // potential chord path: require two distinct presses within window, order doesn't matter
      if (!six7ChordWaitingSecond) {
        six7ChordWaitingSecond = true;
        six7ChordFirstBtn = e.num;
        six7ChordTimer.start(now, SIX7_CHORD_WINDOW_MS);
        return true;
      } else {
        if (six7ChordTimer.done(now)) {
          enterGameOver(now);
          return true;
        }
        if (e.num == six7ChordFirstBtn) {
          enterGameOver(now);
          return true;
        }
        // success chord must be 1 and 3
        bool ok = ( (six7ChordFirstBtn == 1 && e.num == 3) || (six7ChordFirstBtn == 3 && e.num == 1) );
        if (!ok) {
          enterGameOver(now);
          return true;
        }
        // success
        G.score++;
        shrinkWindow();
        six7ChordWaitingSecond = false;
        six7ChordFirstBtn = 0;
        six7BeginRound(now);
        return true;
      }
    }
  }

  // sequence stage: expectedBtn==0 and seqTimer set, first btn stored in six7SeqFirstBtn, second in G.forbidA
  if (six7SeqTimer.dur != 0 && (now - six7SeqTimer.t0) <= six7SeqTimer.dur + 5) {
    // if we were in a sequence round, handle two-step
    if (!six7SeqWaitingSecond) {
      // first press must match
      if (e.num != six7SeqFirstBtn) {
        enterGameOver(now);
        return true;
      }
      six7SeqWaitingSecond = true;
      six7SeqTimer.start(now, SIX7_SEQ_WINDOW_MS);
      // show second digit as hint for a moment
      char buf[5] = "    ";
      buf[2] = (char)('0' + ((G.forbidA == 1) ? 6 : 7));
      showText(buf);
      six7Flash = true;
      six7FlashTimer.start(now, 220);
      return true;
    } else {
      if (six7SeqTimer.done(now)) {
        enterGameOver(now);
        return true;
      }
      if (e.num != G.forbidA) {
        enterGameOver(now);
        return true;
      }
      // success
      G.score++;
      shrinkWindow();
      six7SeqWaitingSecond = false;
      six7SeqFirstBtn = 0;
      six7SeqTimer.dur = 0; // stop
      six7BeginRound(now);
      return true;
    }
  }

  // normal single-press handling
  if (e.num != G.expectedBtn) {
    enterGameOver(now);
    return true;
  }

  // success
  G.score++;
  shrinkWindow();
  six7BeginRound(now);
  return true;
}

// =============================================================================
// Game: Playing mode dispatcher
// =============================================================================

static void enterPlaying(uint32_t now) {
  gMode = MODE_PLAYING;
  gModeStartMs = now;

  G.score = 0;
  G.windowMs = WINDOW_START_MS;
  G.roundStartMs = now;
  G.expectedBtn = 0;
  G.forbidA = G.forbidB = 0;

  // reset per-game substate
  if (gGame == GAME_CORD) {
    cordReset();
    cordBeginShow(now);
  } else if (gGame == GAME_6OR7) {
    six7StageBlock = 0xFF;
    six7SeqTimer.dur = 0;
    six7BeginRound(now);
  } else if (gGame == GAME_NOT) {
    newRoundNot(now);
  } else if (gGame == GAME_HARD) {
    newRoundClassic(now, true);
  } else {
    newRoundClassic(now, false);
  }
}

static void updateReflexLedCountdown(uint32_t now) {
  uint32_t elapsed = now - G.roundStartMs;
  if (elapsed >= G.windowMs) { ledsCount(0); return; }
  float fracRemain = 1.0f - ((float)elapsed / (float)G.windowMs);
  ledsBarFromFraction(fracRemain);
}

static void playingTick(uint32_t now) {
  // CORD tick
  if (gGame == GAME_CORD) {
    cordTick(now);
    return;
  }

  // 6OR7 tick (animations), and reflex timeout
  if (gGame == GAME_6OR7) {
    six7Tick(now);
  }

  // Reflex-type timeout + LED countdown (CLSC/HARD/NOT/6OR7)
  updateReflexLedCountdown(now);
  if ((now - G.roundStartMs) >= G.windowMs) {
    enterGameOver(now);
  }
}

static void playingHandleButton(uint32_t now, const ButtonEvent& e) {
  if (!e.has || e.act != BTN_DOWN) return;

  if (gGame == GAME_CORD) {
    cordHandleButton(now, e);
    return;
  }

  if (gGame == GAME_6OR7) {
    six7HandleButton(now, e);
    return;
  }

  // CLSC/HARD/NOT: single press correct/wrong
  if (e.num != G.expectedBtn) {
    enterGameOver(now);
    return;
  }

  // success
  G.score++;
  shrinkWindow();

  if (gGame == GAME_NOT) {
    newRoundNot(now);
  } else if (gGame == GAME_HARD) {
    newRoundClassic(now, true);
  } else {
    newRoundClassic(now, false);
  }
}

// =============================================================================
// Attract (selector)
// =============================================================================

static void attractTick(uint32_t now) {
  // tiny “pulse” between label and HI score, makes it feel like an arcade menu
  if (attractPulse.done(now)) {
    attractPulse.start(now, 900);
    attractShowHi = !attractShowHi;

    if (!attractShowHi) {
      showText(GAME_LABELS[gGame]);
    } else {
      showHi(G.hi[gGame]);
    }
  }
}

static void attractHandleButton(uint32_t now, const ButtonEvent& e) {
  if (!e.has || e.act != BTN_DOWN) return;

  if (e.num == 1) {
    gGame = (GameId)((gGame == 0) ? (GAME_COUNT - 1) : (gGame - 1));
    showText(GAME_LABELS[gGame]);
    attractShowHi = false;
    attractPulse.start(now, 900);
  } else if (e.num == 3) {
    gGame = (GameId)((gGame + 1) % GAME_COUNT);
    showText(GAME_LABELS[gGame]);
    attractShowHi = false;
    attractPulse.start(now, 900);
  } else if (e.num == 2) {
    enterPlaying(now);
  }
}

// =============================================================================
// Gameover
// =============================================================================

static void gameOverTick(uint32_t now) {
  uint32_t e = now - gModeStartMs;

  if (e < GAMEOVER_SCORE_MS) {
    // blink score
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

// =============================================================================
// Setup / Loop
// =============================================================================

void setup() {
  MFS.initialize();
  randomSeed(analogRead(A0));

  // preload highs
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

  uint32_t now = millis();
  enterAttract(now);
}

void loop() {
  uint32_t now = millis();

  // 1) INPUT
  readButtons();

  // 2) UPDATE
  switch (gMode) {
    case MODE_ATTRACT:
      attractTick(now);
      attractHandleButton(now, gBtn);
      break;

    case MODE_PLAYING:
      playingTick(now);
      playingHandleButton(now, gBtn);
      break;

    case MODE_GAMEOVER:
      gameOverTick(now);
      break;
  }

  // Note: rendering is performed within state handlers to keep things simple,
  // and LEDs are updated continuously via countdown helpers.
}
