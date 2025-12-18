/*
  Reflex Rush - Multi-Mode Party Edition
  Target: Arduino Uno R3 + Multi-Function Shield

  Games (each with its own high score):
    0: "CLSC" - Classic reflex (digits 1..3, normal center position)
    1: "HARD" - Classic but:
                 - START window = 1000 ms
                 - Digit appears in random position
    2: "NOT " - "NOxy" -> do NOT press x or y; press the third button
    3: "CORD" - Button memory game (Simon-style):
                 - Sequence of single buttons
                 - Starts with length 1, each success adds 1 button
                 - Must replay in order
                 - Max 1 second between button presses
                 - LEDs show this 1 s timeout and reset on each press
    4: "6OR7" - Classic-style “6 or 7” reflex game:
                 - Base idea: 6 ~ left button, 7 ~ right button
                 - Every 5 correct presses, the game switches to one of
                   15 challenge variants (swap sides, NOT logic, 6.5 = middle,
                   brief flash, combos of these, etc.)

  Controls in ATTRACT:
    - S1: select previous game
    - S3: select next game
    - S2: start selected game

  High scores:
    - Each game has its own 16-bit high score stored in EEPROM.
    - Hold S1 on reset/power to clear **all** high scores (shows "HI 0").

  Libraries:
    #include <MultiFuncShield.h>
    #include <EEPROM.h>
*/

#include <MultiFuncShield.h>
#include <EEPROM.h>

// -----------------------------------------------------------------------------
// Game & Timing Constants
// -----------------------------------------------------------------------------

// Timing & difficulty for reflex games (CLSC, NOT, 6OR7)
const uint16_t WINDOW_START_MS         = 1800;  // CLSC, NOT, 6OR7
const uint16_t HARD_WINDOW_START_MS    = 1000;  // HARD start window
const uint16_t WINDOW_MIN_MS           = 360;
const uint16_t WINDOW_STEP_MS          = 20;

const uint16_t GAME_OVER_SCORE_MS      = 2000;  // blinking final score display
const uint16_t GAME_OVER_HI_MS         = 2000;  // show "HIxx"
const uint16_t HI_ZERO_ON_BOOT_MS      = 1000;  // "HI 0" on S1-on-boot clear
const uint16_t SCORE_BLINK_INTERVAL_MS = 300;   // blinking cadence for score

// Memory game timing: max gap between button presses during replay (CORD)
// Also used for LED countdown in CORD
const uint16_t MEM_BUTTON_WINDOW_MS    = 1000;  // 1 second between presses

// EEPROM layout: 5 games * 2 bytes each
const int EEPROM_HI_BASE_ADDR = 0;              // game 0 @0, game 1 @2, ...

// Game modes (state machine)
enum GameMode {
  MODE_ATTRACT,
  MODE_PLAYING,
  MODE_GAME_OVER
};

// Game types
enum GameType {
  GAME_CLSC = 0,  // classic
  GAME_HARD,      // hard (random digit position, 1s start window)
  GAME_NOT,       // "NOxy" -> press remaining button
  GAME_CORD,      // button memory (Simon-like)
  GAME_6OR7,      // “6 or 7” classic-style game with twists
  GAME_COUNT
};

// Single-round challenge type
enum ChallengeType {
  CH_SINGLE_NORMAL,  // single-press, reflex mapping (CLSC, HARD, 6OR7)
  CH_NOT_SHOWN,      // "NOxy" -> NOT game
  CH_CHORD_MEMORY    // button memory (CORD)
};

// Round evaluation result
enum RoundResult {
  ROUND_NO_DECISION = 0,
  ROUND_SUCCESS,
  ROUND_FAIL
};

// Max sequence length for memory game (CORD)
const uint8_t MEM_MAX_SEQ = 16;

// -----------------------------------------------------------------------------
// Core Data Structures
// -----------------------------------------------------------------------------

struct Game {
  uint16_t score;
  uint16_t highScore;   // current game's high score (cached from highScores[])
  uint16_t windowMs;
  uint32_t roundStart;  // millis() at start of round (for reflex games)
  uint16_t totalCorrect;
};

// Encoded button event from MFS.getButton()
struct ButtonEvent {
  uint8_t number;   // 1..3
  uint8_t action;   // BUTTON_*_IND (upper bits)
  bool    hasEvent;
};

// Per-round challenge description
struct RoundRule {
  ChallengeType type;
  uint8_t       mainDigit; // base digit (1..3 or special)
  uint8_t       reqBtn1;   // required / forbidden #1 / first expected
  uint8_t       reqBtn2;   // forbidden #2 (NOT only)
  uint8_t       pos;       // digit position (0..3) or 255 for default
};

// Per-round progress / internal state
struct RoundProgress {
  // For memory game (CORD):
  uint8_t  seqIndex;     // which button index in sequence we're currently on
  uint32_t lastPressMs;  // time of last press / start of current 1 s window
};

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

Game          game;
ButtonEvent   btnEvt;
RoundRule     roundRule;
RoundProgress roundProg;

GameMode      gameMode;
uint32_t      modeStartMs;

// Which game is selected / active
uint8_t       currentGameIndex = GAME_CLSC;

// Per-game high scores (RAM mirror of EEPROM)
uint16_t      highScores[GAME_COUNT];

// Memory button sequence for CORD
uint8_t       memSeq[MEM_MAX_SEQ];
uint8_t       memSeqLen = 0;

// 6OR7 game progression state
uint8_t       six7StageIndex    = 0xFF; // stage = totalCorrect/5
uint8_t       six7ChallengeId   = 0;    // 0..14 (15 variants)

// Game name labels for ATTRACT screen
const char *GAME_LABELS[GAME_COUNT] = {
  "CLSC",
  "HARD",
  "NOT ",
  "CORD",
  "6OR7"
};

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------

// Button manager
void     updateButtons();

// High score manager
uint16_t loadHighScoreFromEEPROM(uint8_t gameIdx);
void     saveHighScoreToEEPROM(uint8_t gameIdx, uint16_t value);
void     clearHighScoreInEEPROM(uint8_t gameIdx);
void     clearAllHighScores();

// Display / LED manager
void     setLedsByCount(uint8_t countOn);
void     displayAttractScreen();
void     displayTargetValue(const RoundRule &rule);
void     displayScore(uint16_t score);
void     displayScoreBlinking(uint16_t score, bool visible);
void     displayHiScore(uint16_t hi);
void     displayHiZero();
void     clearDisplay();

// LED countdown for reflex games
void     updateLedCountdown(const Game &g, uint32_t now);

// CORD memory-game LED countdown
void     updateCordLedCountdown(uint32_t now);

// Memory game helpers (CORD)
void     resetMemoryGame();
void     growMemorySequence();
void     animateMemoryButton(uint8_t btn);
void     playMemorySequence();

// 6OR7 helpers
void     updateSixOrSevenMode();
void     setupSixOrSevenRound(uint32_t now);

// Game state controller
void     enterAttractMode(uint32_t now);
void     handleAttractMode(uint32_t now);
void     enterPlayingMode(uint32_t now);
void     handlePlayingMode(uint32_t now);
void     enterGameOverMode(uint32_t now);
void     handleGameOverMode(uint32_t now);

// Round controller
void     startNewRound(uint32_t now);
void     chooseRoundRule(uint32_t now);
void     resetRoundProgress();
RoundResult evaluateRoundEvent(uint32_t now, const ButtonEvent &evt);

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
  MFS.initialize();
  randomSeed(analogRead(A0));

  uint32_t now = millis();

  // Boot-time button check for global high score clear
  delay(200);
  byte bootBtn = MFS.getButton();
  if (bootBtn) {
    uint8_t num = bootBtn & B00111111;

    // S1 held on boot -> clear ALL high scores
    if (num == 1) {
      clearAllHighScores();
      displayHiZero();
      setLedsByCount(0);
      delay(HI_ZERO_ON_BOOT_MS);
    }
  }

  // Load per-game high scores from EEPROM
  for (uint8_t i = 0; i < GAME_COUNT; ++i) {
    highScores[i] = loadHighScoreFromEEPROM(i);
  }

  // Initialize game state
  game.score        = 0;
  game.windowMs     = WINDOW_START_MS;
  game.roundStart   = 0;
  game.totalCorrect = 0;
  game.highScore    = highScores[currentGameIndex];

  memSeqLen         = 0;

  six7StageIndex    = 0xFF;
  six7ChallengeId   = 0;

  enterAttractMode(now);
}

// -----------------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------------

void loop() {
  uint32_t now = millis();

  // 1. INPUT
  updateButtons();

  // 2. STATE UPDATE
  switch (gameMode) {
    case MODE_ATTRACT:
      handleAttractMode(now);
      break;

    case MODE_PLAYING:
      handlePlayingMode(now);
      break;

    case MODE_GAME_OVER:
      handleGameOverMode(now);
      break;
  }
}

// -----------------------------------------------------------------------------
// Button Manager
// -----------------------------------------------------------------------------

void updateButtons() {
  btnEvt.hasEvent = false;
  byte btn = MFS.getButton();
  if (btn) {
    btnEvt.number = btn & B00111111;
    btnEvt.action = btn & B11000000;
    btnEvt.hasEvent = true;
  }
}

// -----------------------------------------------------------------------------
// High Score Manager
// -----------------------------------------------------------------------------

uint16_t loadHighScoreFromEEPROM(uint8_t gameIdx) {
  uint16_t value;
  int addr = EEPROM_HI_BASE_ADDR + 2 * gameIdx;
  EEPROM.get(addr, value);
  if (value == 0xFFFF) {
    return 0;
  }
  return value;
}

void saveHighScoreToEEPROM(uint8_t gameIdx, uint16_t value) {
  int addr = EEPROM_HI_BASE_ADDR + 2 * gameIdx;
  EEPROM.put(addr, value);
}

void clearHighScoreInEEPROM(uint8_t gameIdx) {
  uint16_t sentinel = 0xFFFF;
  int addr = EEPROM_HI_BASE_ADDR + 2 * gameIdx;
  EEPROM.put(addr, sentinel);
}

void clearAllHighScores() {
  for (uint8_t i = 0; i < GAME_COUNT; ++i) {
    clearHighScoreInEEPROM(i);
    highScores[i] = 0;
  }
}

// -----------------------------------------------------------------------------
// Display / LED Manager
// -----------------------------------------------------------------------------

void setLedsByCount(uint8_t countOn) {
  byte mask = 0;
  if (countOn >= 1) mask |= LED_1;
  if (countOn >= 2) mask |= LED_2;
  if (countOn >= 3) mask |= LED_3;
  if (countOn >= 4) mask |= LED_4;

  MFS.writeLeds(LED_ALL, OFF);
  if (mask != 0) {
    MFS.writeLeds(mask, ON);
  }
}

void displayAttractScreen() {
  MFS.write(GAME_LABELS[currentGameIndex]);
}

void displayTargetValue(const RoundRule &rule) {
  switch (rule.type) {
    // CLSC / HARD: single digit 1..3
    case CH_SINGLE_NORMAL: {
      if (rule.pos == 255) {
        // default position (library decides)
        MFS.write((int)rule.mainDigit);
      } else {
        // digit in specific position 0..3
        char buf[5] = "    ";
        uint8_t p = rule.pos;
        if (p > 3) p = 3;
        buf[p] = '0' + rule.mainDigit;
        MFS.write(buf);
      }
      break;
    }

    // NOT game: "NOxy"
    case CH_NOT_SHOWN: {
      char buf[5] = "NO12";
      buf[2] = '0' + rule.reqBtn1;   // forbidden #1
      buf[3] = '0' + rule.reqBtn2;   // forbidden #2
      MFS.write(buf);
      break;
    }

    // CORD (memory) uses its own animation (playMemorySequence),
    // nothing to display statically per-round here.
    case CH_CHORD_MEMORY: {
      // no-op
      break;
    }
  }
}

void displayScore(uint16_t score) {
  MFS.write((int)score);
}

void displayScoreBlinking(uint16_t score, bool visible) {
  if (visible) {
    displayScore(score);
  } else {
    clearDisplay();
  }
}

void displayHiScore(uint16_t hi) {
  if (hi < 100) {
    char buf[5];
    buf[0] = 'H';
    buf[1] = 'I';
    buf[2] = (char)('0' + (hi / 10));
    buf[3] = (char)('0' + (hi % 10));
    buf[4] = '\0';
    MFS.write(buf);
  } else {
    MFS.write((int)hi);
  }
}

void displayHiZero() {
  MFS.write("HI 0");
}

void clearDisplay() {
  MFS.write("    ");
}

// -----------------------------------------------------------------------------
// LED Countdown for Reflex Games
// -----------------------------------------------------------------------------

void updateLedCountdown(const Game &g, uint32_t now) {
  uint32_t elapsed = now - g.roundStart;

  if (elapsed >= g.windowMs) {
    setLedsByCount(0);
    return;
  }

  float frac = (float)elapsed / (float)g.windowMs;
  uint8_t ledsOn;

  if (frac < 0.25f)       ledsOn = 4;
  else if (frac < 0.50f)  ledsOn = 3;
  else if (frac < 0.75f)  ledsOn = 2;
  else                    ledsOn = 1;

  setLedsByCount(ledsOn);
}

// -----------------------------------------------------------------------------
// LED Countdown for CORD (1s timeout between presses)
// -----------------------------------------------------------------------------

void updateCordLedCountdown(uint32_t now) {
  uint32_t elapsed = now - roundProg.lastPressMs;

  if (elapsed >= MEM_BUTTON_WINDOW_MS) {
    setLedsByCount(0);
    return;
  }

  float frac = (float)elapsed / (float)MEM_BUTTON_WINDOW_MS;
  uint8_t ledsOn;

  if (frac < 0.25f)       ledsOn = 4;
  else if (frac < 0.50f)  ledsOn = 3;
  else if (frac < 0.75f)  ledsOn = 2;
  else                    ledsOn = 1;

  setLedsByCount(ledsOn);
}

// -----------------------------------------------------------------------------
// Memory Game Helpers (CORD)
// -----------------------------------------------------------------------------

void resetMemoryGame() {
  memSeqLen = 1;
  memSeq[0] = random(1, 4); // 1..3
}

void growMemorySequence() {
  if (memSeqLen < MEM_MAX_SEQ) {
    memSeq[memSeqLen] = random(1, 4); // 1..3
    memSeqLen++;
  }
}

void animateMemoryButton(uint8_t btn) {
  // Show button digit in position 2 for a short time
  char buf[5] = "    ";
  buf[1] = '0' + btn;
  MFS.write(buf);
  delay(250);

  buf[1] = ' ';
  MFS.write(buf);
  delay(120);
}

void playMemorySequence() {
  for (uint8_t i = 0; i < memSeqLen; ++i) {
    animateMemoryButton(memSeq[i]);
  }
  clearDisplay();
}

// -----------------------------------------------------------------------------
// 6OR7 Helpers
// -----------------------------------------------------------------------------

// Update which challenge variant we're in based on totalCorrect.
// Every 5 correct presses advances the "stage" and picks a new challenge.
void updateSixOrSevenMode() {
  uint8_t stage = game.totalCorrect / 5; // 0,1,2,...

  if (stage != six7StageIndex) {
    six7StageIndex = stage;
    if (stage == 0) {
      six7ChallengeId = 0;        // simple, no tricks
    } else {
      six7ChallengeId = random(1, 15); // 1..14 (15 variants total inc. 0)
    }
  }
}

// Configure and display one round of 6OR7 based on the current challenge.
// We interpret 6OR7 challengeId bits as:
//
//   bit0 (1): swap sides          (6 appears on opposite side of base)
//   bit1 (2): NOT logic           (press opposite side instead)
//   bit2 (4): allow "6.5" middle  (sometimes show 6.5 -> press middle button)
//   bit3 (8): brief flash         (show for a short time then blank)
//
//   Base behavior (id=0):
//     - Randomly show 6 on left (btn1) or 7 on right (btn3)
//     - Full 1.8s reflex window (handled elsewhere)
void setupSixOrSevenRound(uint32_t now) {
  (void)now;

  updateSixOrSevenMode();

  uint8_t id          = six7ChallengeId;
  bool swapSides      = (id & 0x01);
  bool useNot         = (id & 0x02);
  bool allowMiddle    = (id & 0x04);
  bool briefFlash     = (id & 0x08);

  char buf[5] = "    ";
  uint8_t correctBtn  = 1;

  // Decide if we use the "6.5" middle variant this round
  bool useMiddleCase = false;
  if (allowMiddle) {
    // ~1/3 chance of middle; otherwise a normal left/right 6 or 7
    if (random(0, 3) == 0) {
      useMiddleCase = true;
    }
  }

  if (useMiddleCase) {
    // Show "6.5" across the middle; press middle button (btn2)
    buf[0] = '6';
    buf[1] = '.';
    buf[2] = '5';
    correctBtn = 2; // middle

    MFS.write(buf);
    if (briefFlash) {
      delay(200);
      clearDisplay();
    }

    roundRule.type      = CH_SINGLE_NORMAL;
    roundRule.reqBtn1   = correctBtn;
    roundRule.reqBtn2   = 0;
    roundRule.mainDigit = 0;
    roundRule.pos       = 255;
    return;
  }

  // Otherwise, standard 6 or 7 on a side
  bool showSix = (random(0, 2) == 0); // else show 7
  uint8_t displayPos;

  // Base: 6 on left (0), 7 on right (3)
  if (!swapSides) {
    if (showSix) displayPos = 0;
    else         displayPos = 3;
  } else {
    // Swapped sides: 6 on right, 7 on left
    if (showSix) displayPos = 3;
    else         displayPos = 0;
  }

  // Base mapping: press the button where the digit appears
  correctBtn = (displayPos == 0) ? 1 : 3;

  // NOT logic: press the opposite side instead (hinted by 'n')
  if (useNot) {
    if (correctBtn == 1)      correctBtn = 3;
    else if (correctBtn == 3) correctBtn = 1;
  }

  buf[displayPos] = showSix ? '6' : '7';

  if (useNot) {
    // Place an 'n' near the center as a visual "NOT" indicator
    if (displayPos == 0) buf[1] = 'n';
    else                 buf[2] = 'n';
  }

  MFS.write(buf);
  if (briefFlash) {
    delay(200);
    clearDisplay();
  }

  roundRule.type      = CH_SINGLE_NORMAL;
  roundRule.reqBtn1   = correctBtn;
  roundRule.reqBtn2   = 0;
  roundRule.mainDigit = 0;
  roundRule.pos       = 255;
}

// -----------------------------------------------------------------------------
// Game State Controller
// -----------------------------------------------------------------------------

void enterAttractMode(uint32_t now) {
  gameMode     = MODE_ATTRACT;
  modeStartMs  = now;

  game.score        = 0;
  game.windowMs     = WINDOW_START_MS;
  game.roundStart   = 0;
  game.totalCorrect = 0;
  game.highScore    = highScores[currentGameIndex];

  memSeqLen         = 0;

  six7StageIndex    = 0xFF;
  six7ChallengeId   = 0;

  setLedsByCount(0);
  displayAttractScreen();
}

void handleAttractMode(uint32_t now) {
  (void)now; // unused

  if (!btnEvt.hasEvent) return;

  uint8_t b   = btnEvt.number;
  uint8_t act = btnEvt.action;

  if (act != BUTTON_PRESSED_IND) return;

  if (b == 1) {
    // previous game
    if (currentGameIndex == 0) currentGameIndex = GAME_COUNT - 1;
    else currentGameIndex--;
    displayAttractScreen();
  } else if (b == 3) {
    // next game
    currentGameIndex = (currentGameIndex + 1) % GAME_COUNT;
    displayAttractScreen();
  } else if (b == 2) {
    // start selected game
    enterPlayingMode(now);
  }
}

void enterPlayingMode(uint32_t now) {
  gameMode    = MODE_PLAYING;
  modeStartMs = now;

  game.score        = 0;
  game.totalCorrect = 0;
  game.highScore    = highScores[currentGameIndex];

  if (currentGameIndex == GAME_CORD) {
    // Memory game: no global timeout, just 1s per-press window
    memSeqLen = 0;
    resetMemoryGame();
    game.windowMs   = 0;     // unused for CORD
    game.roundStart = now;   // unused for CORD timeout
  } else {
    // Reflex games: CLSC, HARD, NOT, 6OR7
    if (currentGameIndex == GAME_HARD) {
      game.windowMs = HARD_WINDOW_START_MS;
    } else {
      game.windowMs = WINDOW_START_MS;
    }
    game.roundStart = now;
  }

  startNewRound(now);
}

void handlePlayingMode(uint32_t now) {
  // CORD memory game: dedicated path
  if (currentGameIndex == GAME_CORD) {
    updateCordLedCountdown(now);

    // Immediate timeout if >1s since last allowed time reference
    if (now - roundProg.lastPressMs > MEM_BUTTON_WINDOW_MS) {
      enterGameOverMode(now);
      return;
    }

    if (!btnEvt.hasEvent) return;

    RoundResult rr = evaluateRoundEvent(now, btnEvt);
    if (rr == ROUND_SUCCESS) {
      game.score++;
      game.totalCorrect++;
      growMemorySequence();
      startNewRound(millis());
    } else if (rr == ROUND_FAIL) {
      enterGameOverMode(millis());
    }
    return;
  }

  // Reflex games: CLSC, HARD, NOT, 6OR7
  updateLedCountdown(game, now);

  // Timeout for reflex games
  if (game.windowMs > 0 && (now - game.roundStart >= game.windowMs)) {
    enterGameOverMode(now);
    return;
  }

  if (!btnEvt.hasEvent) return;

  RoundResult rr = evaluateRoundEvent(now, btnEvt);

  if (rr == ROUND_SUCCESS) {
    game.score++;
    game.totalCorrect++;

    // shrink window over time (except CORD, which doesn't use this path)
    if (game.windowMs > WINDOW_MIN_MS) {
      uint16_t newWindow = game.windowMs;
      if (newWindow > WINDOW_MIN_MS + WINDOW_STEP_MS) {
        newWindow -= WINDOW_STEP_MS;
      } else {
        newWindow = WINDOW_MIN_MS;
      }
      game.windowMs = newWindow;
    }

    startNewRound(now);
  } else if (rr == ROUND_FAIL) {
    enterGameOverMode(now);
  }
}

void enterGameOverMode(uint32_t now) {
  gameMode    = MODE_GAME_OVER;
  modeStartMs = now;

  // Per-game high score update
  if (game.score > game.highScore) {
    game.highScore = game.score;
    highScores[currentGameIndex] = game.highScore;
    saveHighScoreToEEPROM(currentGameIndex, game.highScore);
  }

  setLedsByCount(0);
  displayScoreBlinking(game.score, true);
}

void handleGameOverMode(uint32_t now) {
  uint32_t elapsed = now - modeStartMs;

  static bool     blinkOn     = true;
  static uint32_t lastBlinkMs = 0;

  if (elapsed < GAME_OVER_SCORE_MS) {
    // Blinking score
    if (now - lastBlinkMs >= SCORE_BLINK_INTERVAL_MS) {
      lastBlinkMs = now;
      blinkOn     = !blinkOn;
      displayScoreBlinking(game.score, blinkOn);
    }
  } else if (elapsed < (GAME_OVER_SCORE_MS + GAME_OVER_HI_MS)) {
    displayHiScore(game.highScore);
  } else {
    enterAttractMode(now);
  }
}

// -----------------------------------------------------------------------------
// Round Controller
// -----------------------------------------------------------------------------

void resetRoundProgress() {
  roundProg.seqIndex    = 0;
  roundProg.lastPressMs = 0;
}

void chooseRoundRule(uint32_t now) {
  resetRoundProgress();

  // CORD (memory game): show entire sequence, then wait for input
  if (currentGameIndex == GAME_CORD) {
    roundRule.type      = CH_CHORD_MEMORY;
    roundProg.seqIndex  = 0;
    playMemorySequence();
    // Start 1s timeout immediately after playback
    roundProg.lastPressMs = millis();
    setLedsByCount(4);
    game.roundStart = roundProg.lastPressMs; // not used further, but kept updated
    return;
  }

  // 6OR7: own setup function
  if (currentGameIndex == GAME_6OR7) {
    setupSixOrSevenRound(now);
    setLedsByCount(4);
    game.roundStart = now;
    return;
  }

  // REFLEX games (CLSC, HARD, NOT)
  roundRule.type = CH_SINGLE_NORMAL;
  roundRule.mainDigit = 1;
  roundRule.reqBtn1   = 1;
  roundRule.reqBtn2   = 0;
  roundRule.pos       = 255;

  if (currentGameIndex == GAME_CLSC || currentGameIndex == GAME_HARD) {
    // Single digit 1..3
    uint8_t d = random(1, 4); // 1..3
    roundRule.mainDigit = d;
    roundRule.reqBtn1   = d;
    roundRule.reqBtn2   = 0;

    if (currentGameIndex == GAME_CLSC) {
      roundRule.pos = 255; // default position
    } else {
      // HARD: digit can appear in any digit position
      roundRule.pos = (uint8_t)random(0, 4); // 0..3
    }
  } else if (currentGameIndex == GAME_NOT) {
    // NOT game: two forbidden buttons, one allowed
    roundRule.type = CH_NOT_SHOWN;

    uint8_t f1 = random(1, 4);
    uint8_t f2;
    do {
      f2 = random(1, 4);
    } while (f2 == f1);

    // sort for display: "NOxy" with x<y
    uint8_t a = f1;
    uint8_t b = f2;
    if (a > b) {
      uint8_t tmp = a; a = b; b = tmp;
    }

    roundRule.reqBtn1   = a;  // forbidden #1
    roundRule.reqBtn2   = b;  // forbidden #2
    roundRule.mainDigit = 0;
    roundRule.pos       = 255;
  }

  displayTargetValue(roundRule);
  setLedsByCount(4); // full time at start of round

  game.roundStart = now;
}

void startNewRound(uint32_t now) {
  chooseRoundRule(now);
}

// -----------------------------------------------------------------------------
// Round Evaluation (for CLSC / HARD / NOT / CORD / 6OR7)
// -----------------------------------------------------------------------------

RoundResult evaluateRoundEvent(uint32_t now, const ButtonEvent &evt) {
  uint8_t b   = evt.number;
  uint8_t act = evt.action;

  switch (roundRule.type) {
    // -----------------------------------------------------------------------
    // CLSC / HARD / 6OR7: single press of the correct button
    // -----------------------------------------------------------------------
    case CH_SINGLE_NORMAL: {
      if (act != BUTTON_PRESSED_IND) return ROUND_NO_DECISION;

      if (b == roundRule.reqBtn1) {
        return ROUND_SUCCESS;
      } else {
        return ROUND_FAIL;
      }
    }

    // -----------------------------------------------------------------------
    // NOT: "NOxy" -> press the remaining button
    // -----------------------------------------------------------------------
    case CH_NOT_SHOWN: {
      if (act != BUTTON_PRESSED_IND) return ROUND_NO_DECISION;

      if (b == roundRule.reqBtn1 || b == roundRule.reqBtn2) {
        return ROUND_FAIL;
      } else {
        return ROUND_SUCCESS;
      }
    }

    // -----------------------------------------------------------------------
    // CORD: button memory game
    //   - memSeq[0..memSeqLen-1] is expected sequence
    //   - Player must press them in order.
    //   - Each press must occur within MEM_BUTTON_WINDOW_MS of the previous
    //     reference time (roundProg.lastPressMs).
    //   - LED countdown and immediate timeout are handled elsewhere, but we
    //     still enforce the time window here for presses that arrive late.
    // -----------------------------------------------------------------------
    case CH_CHORD_MEMORY: {
      // Check timeout vs last allowed reference
      if (now - roundProg.lastPressMs > MEM_BUTTON_WINDOW_MS) {
        return ROUND_FAIL;
      }

      if (act != BUTTON_PRESSED_IND) return ROUND_NO_DECISION;
      if (roundProg.seqIndex >= memSeqLen) {
        // Shouldn't happen, but treat as fail
        return ROUND_FAIL;
      }

      if (b != memSeq[roundProg.seqIndex]) {
        return ROUND_FAIL;
      }

      roundProg.seqIndex++;

      if (roundProg.seqIndex >= memSeqLen) {
        // Completed full sequence correctly
        return ROUND_SUCCESS;
      } else {
        // Prepare for next button in sequence
        roundProg.lastPressMs = now;  // reset 1s timeout
        setLedsByCount(4);           // refill LED bar
        return ROUND_NO_DECISION;
      }
    }
  }

  return ROUND_NO_DECISION;
}
