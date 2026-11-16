// =====================================================================
//  WHEAT SWAY CONTROLLER 
//  3 x 28BYJ-48 stepper motors, 74HC595 shift register, Arduino Nano Every
//
// =====================================================================
//
//  THE BIG PICTURE (read this first!)
//  ----------------------------------
//  MediaPipe Hand reads the position of a hand and sends it to wekinator, wekinator is trained to idendify 
// position as a state, which is sent to processing and then to arduino.
//  the hand is doing, and sends the Arduino a NUMBER (1 to 5) over the USB
//  cable. The Arduino reads that number and moves three motors that pull
//  rubber bands, which tilt the wheat.
//
//     Number 1 = ONE single sway (then ignore more 1s until a 2-5 arrives)
//     Number 2 = keep swaying, all three motors together
//     Number 3 = keep swaying as a WAVE, left -> right
//     Number 4 = keep swaying as a WAVE, right -> left
//     Number 5 = STOP (go home, then sit at the rest position)
//     Anything else = ignored
//
//
//  THE MOTOR "LIFE CYCLE" (what each motor can be doing)
//  -----------------------------------------------------
//     IDLE      -> doing nothing
//     WAITING   -> waiting for its turn (this creates the wave delay)
//     PULLING   -> pulling the rubber band (wheat tilts back)
//     RELEASING -> letting the band go (wheat swings forward)
//     HOMING    -> pushing forward until it hits the end stop = "origin"
//     TO_REST   -> moving to the resting spot
//
//  HOMING (CALIBRATION)
//  --------------------------------------
//  Stepper motors have no idea where they are. So at start-up each motor
//  pushes against the wheat as hard as it can until it can't go any
//  further. That spot is called the ORIGIN (position 0). Every movement
//  after that is counted from the origin.
//
//  TESTING WITHOUT THE CAMERA
//  --------------------------
//  Open the Arduino Serial Monitor, set it to 9600 baud, set the line
//  ending dropdown to "Newline", type a number 1-5 and press Send.
// =====================================================================


// =====================================================================
// SECTION 1: PIN SETTINGS
// "Pins" are the metal legs on the Arduino. We tell the code which pin
// is wired to which motor wire.
// =====================================================================

// How many motors we have. Used by loops so we can say "for each motor".
const int NUM_MOTORS = 3;

// Motor 1 is wired straight to Arduino pins 8, 9, 10, 11 (one per coil).
const int motor1Pins[4] = {8, 9, 10, 11};

// Motor 2 is wired straight to Arduino pins 4, 5, 6, 7.
const int motor2Pins[4] = {4, 5, 6, 7};

// Motor 3 does NOT connect directly. It goes through a chip called a
// 74HC595 "shift register", which lets us control extra pins using only 3.
// We send it a byte (8 on/off values) and it switches the motor's coils.
const int DATA_PIN  = 2;    // the data we send to the chip
const int LATCH_PIN = 3;    // "apply it now" signal
const int CLOCK_PIN = 12;   // ticks once for each bit sent


// =====================================================================
// SECTION 2: MOVEMENT SETTINGS 
// =====================================================================

// How many half-steps a motor pulls the wheat back on each sway.
// Bigger = bigger sway.
// (Roughly 1024 half-steps = 90 degrees of motor rotation.)
const int MOVE_STEPS = 400;

// Time between steps, in MICROseconds (1,000,000 microseconds = 1 second).
// Smaller number = faster motors.
const unsigned long MID_INTERVAL_US  = 3500;

const unsigned long SINGLE_INTERVAL_US = MID_INTERVAL_US;   // number 1
const unsigned long SWING_INTERVAL_US  = MID_INTERVAL_US;   // number 2
const unsigned long WAVE_INTERVAL_US   = MID_INTERVAL_US;   // numbers 3 and 4

// In a wave, how long (in MILLIseconds, 1000 = 1 second) each motor waits
// after the one before it. 500 = half a second between motors.
const unsigned long WAVE_DELAY_MS = 500;

// Where each motor physically sits, counted from the LEFT (0 = far left).
// The list order is {motor 1, motor 2, motor 3}.
// {2, 0, 1} means: motor 1 is on the right, motor 2 on the left,
//                  motor 3 in the middle.
// If your wave goes the wrong way, change the numbers to match your build.
const int motorPositionFromLeft[NUM_MOTORS] = {2, 0, 1};

// In the continuous modes, at the end of each sway the motor pushes a
// few EXTRA steps into the end stop. This "re-syncs" the motor every cycle,
// so any skipped steps do not slowly build up. Raise to 100 if they drift.
const int SETTLE_STEPS = 60;


// =====================================================================
// SECTION 3: DIRECTION SETTINGS
// A direction is just +1 or -1 (which way through the step table we go).
// =====================================================================

// Pulling the wheat backwards.
const int PULL_DIRECTION = 1;

// Releasing the wheat forwards.
const int RELEASE_DIRECTION = -1;

// If the motors spin the wrong way, swap the 1 and -1 above.


// =====================================================================
// SECTION 4: HOMING (CALIBRATION) SETTINGS
// =====================================================================

// Homing pushes FORWARD, which is the release direction.
const int HOMING_DIRECTION = RELEASE_DIRECTION;

// Time between steps while homing. It is much SLOWER than normal because
// a slow stepper is a STRONGER stepper. If motors skip before reaching the
// end stop, try 12000.
const unsigned long HOMING_INTERVAL_US = 10000;

// Extra half-steps to push past where we think the origin is, just to be
// sure we really have hit the end stop.
const int HOMING_EXTRA_STEPS = 150;

// The REST position is this many half-steps back from the origin.
// (The original comment said 1/8 turn = 512, but the actual value is 250.
//  The value is what counts, so the real rest distance is 250 half-steps.)
const int REST_OFFSET_STEPS = 250;

// At power-up a motor could be ANYWHERE, so the very first homing pushes
// for the longest possible distance to be safe:
// rest distance + a full sway + a little extra.
const int STARTUP_HOMING_STEPS = REST_OFFSET_STEPS + MOVE_STEPS + HOMING_EXTRA_STEPS;


// =====================================================================
// SECTION 5: THE STEP SEQUENCE
// Each line is one "coil pattern". The four right-most digits are the
// four coils: 1 = coil on, 0 = coil off.
// Going down the list (0 -> 7) turns the motor one way, going up turns it
// the other way. When we reach the end we loop back to the start.
//
// Entries 0, 2, 4, 6 turn on ONE coil (gentle).
// Entries 1, 3, 5, 7 turn on TWO coils (strongest grip). Homing uses only
// these odd ones because it needs maximum strength.
// =====================================================================

const byte stepSequence[8] = {
  0b00000001,   // entry 0: one coil
  0b00000011,   // entry 1: two coils
  0b00000010,   // entry 2: one coil
  0b00000110,   // entry 3: two coils
  0b00000100,   // entry 4: one coil
  0b00001100,   // entry 5: two coils
  0b00001000,   // entry 6: one coil
  0b00001001    // entry 7: two coils
};


// =====================================================================
// SECTION 6: CUSTOM TYPES
// These let us create our own named "labels" and "bundles" of data so the
// code reads like English.
// =====================================================================

// The list of things a single motor can be doing (see top of the file).
enum MotorPhase {
  MOTOR_IDLE,        // not moving
  MOTOR_WAITING,     // waiting for its turn (this is what makes the wave)
  MOTOR_PULLING,     // pulling the wheat backward
  MOTOR_RELEASING,   // releasing the wheat forward, back to the origin
  MOTOR_HOMING,      // pushing against the wheat to find the origin
  MOTOR_TO_REST      // moving back to the rest position
};

// The list of overall "shows" the whole machine can run.
enum Mode {
  MODE_STOP,         // number 5
  MODE_SINGLE,       // number 1
  MODE_SWING,        // number 2
  MODE_WAVE_RIGHT,   // number 3
  MODE_WAVE_LEFT     // number 4
};

// Everything we need to remember about ONE motor, bundled together.
// We make three copies of this, one per motor.
struct MotorState {
  MotorPhase phase;            // what is this motor doing right now?
  int stepIndex;               // which line of stepSequence it is on (0-7)
  int position;                // steps pulled back from the origin (0 = origin)
  int homingRemaining;         // steps still left to push while homing
  unsigned long startAtMs;     // the clock time a WAITING motor should start
};


// =====================================================================
// SECTION 7: VARIABLES (the program's memory)
// =====================================================================

// The three motors' info. motors[0] = motor 1, motors[1] = motor 2, etc.
MotorState motors[NUM_MOTORS];

Mode currentMode = MODE_STOP;      // the show running now
Mode pendingMode = MODE_STOP;      // the show waiting in line
bool hasPending = false;           // true = a new show is queued, waiting
                                   //        for the motors to get home first
bool repeatCycles = false;         // true = keep swaying again and again
bool announceWhenIdle = false;     // true = print a message once all is home

// Number 1 should only work ONCE until a 2-5 comes in. This remembers
// whether it has already been used.
bool singleSwayTriggered = false;

bool needsHoming = true;           // do a calibration next time all are idle?
bool fullRangeHoming = true;       // true only for the first calibration after
                                   //   power-up (the long safe one)
bool homingActive = false;         // true while calibration is in progress
bool calibrated = false;           // true once the first calibration is done
bool atRest = false;               // true while sitting at the rest position

// How long to wait between steps right now (microseconds).
unsigned long stepIntervalUs = MID_INTERVAL_US;

// The clock time of the last step. Used to know when the next one is due.
unsigned long lastStepMicros = 0;

// Small notepad used when reading numbers from the computer.
// Holds up to 7 digits (the 8th space is for an "end of text" marker).
char lineBuffer[8];
byte lineLength = 0;               // how many digits are in the notepad


// =====================================================================
// SECTION 8: LOW-LEVEL MOTOR FUNCTIONS
// These are the small "hands-on" jobs that actually touch the pins.
// A "function" is a named recipe of instructions we can reuse.
// =====================================================================

// ---------------------------------------------------------------------
// Switch the coils of motor 1 or 2 (wired straight to the Arduino).
// "pattern" is one entry from stepSequence, e.g. 0b00000011.
// ---------------------------------------------------------------------
void writeDirectMotor(const int pins[], byte pattern) {

  // Go through the 4 coils one by one.
  for (int i = 0; i < 4; i++) {
    // bitRead looks at digit number i of the pattern (1 or 0).
    // digitalWrite sets that pin HIGH (on) if it's 1, LOW (off) if it's 0.
    digitalWrite(pins[i], bitRead(pattern, i));
  }
}

// ---------------------------------------------------------------------
// Switch the coils of motor 3 (goes through the 74HC595 chip).
// ---------------------------------------------------------------------
void writeMotor3(byte pattern) {

  digitalWrite(LATCH_PIN, LOW);    // 1. tell the chip "I'm about to send data"

  // 2. shiftOut sends the 8 bits of the pattern one at a time down the data
  //    wire, ticking the clock wire for each bit.
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern);

  digitalWrite(LATCH_PIN, HIGH);   // 3. tell the chip "apply it to the pins NOW"
}

// ---------------------------------------------------------------------
// Send a coil pattern to whichever motor we choose (0, 1 or 2).
// This hides the difference between the two wiring styles, so the rest
// of the code can treat all three motors the same.
// ---------------------------------------------------------------------
void writeMotor(int motor, byte pattern) {

  if (motor == 0) {
    writeDirectMotor(motor1Pins, pattern);   // motor 1
  }
  else if (motor == 1) {
    writeDirectMotor(motor2Pins, pattern);   // motor 2
  }
  else {
    writeMotor3(pattern);                    // motor 3 (via the chip)
  }
}

// ---------------------------------------------------------------------
// Move one motor ONE half-step.
// direction = +1 (pull) or -1 (release).
// ---------------------------------------------------------------------
void stepMotor(int motor, int direction) {

  // Move to the next (or previous) line in the step table.
  motors[motor].stepIndex += direction;

  // The table only has lines 0-7, so wrap around at the ends:
  // going past 7 goes back to 0, and going below 0 goes to 7.
  if (motors[motor].stepIndex >= 8) {
    motors[motor].stepIndex = 0;
  }

  if (motors[motor].stepIndex < 0) {
    motors[motor].stepIndex = 7;
  }

  // Turn on the coils listed on that line. The motor shaft moves.
  writeMotor(motor, stepSequence[motors[motor].stepIndex]);
}

// ---------------------------------------------------------------------
// Move one motor in a "full step", which skips the one-coil lines so
// TWO coils are always on = maximum strength. Used only for homing.
//
// It returns how many half-steps it moved:
//   - 1 the very first time (to line up on a two-coil entry)
//   - 2 every time after that
// ---------------------------------------------------------------------
int stepMotorFullPower(int motor, int direction) {

  // Even entries (0,2,4,6) are one-coil, so move just 1 to reach a
  // two-coil entry. If we are already on a two-coil entry, jump 2.
  // (The % symbol means "remainder after dividing", so "% 2 == 0"
  //  is the way of checking "is this an even number?")
  int delta = (motors[motor].stepIndex % 2 == 0) ? 1 : 2;

  // Move, then wrap around inside 0-7. The "+ 8" and "% 8" keep the
  // answer from ever going negative.
  motors[motor].stepIndex =
    (motors[motor].stepIndex + direction * delta + 8) % 8;

  writeMotor(motor, stepSequence[motors[motor].stepIndex]);

  return delta;
}


// =====================================================================
// SECTION 9: HELPER QUESTIONS
// Each of these answers a yes/no question about the motors.
// =====================================================================

// "Are ALL motors doing nothing?"
bool allMotorsIdle() {

  for (int i = 0; i < NUM_MOTORS; i++) {
    if (motors[i].phase != MOTOR_IDLE) {
      return false;    // found one that is busy, so the answer is no
    }
  }

  return true;         // checked all of them, none were busy
}

// "Is the wheat currently being swayed?" (waiting, pulling or releasing)
bool movementActive() {

  for (int i = 0; i < NUM_MOTORS; i++) {
    if (
      motors[i].phase == MOTOR_WAITING ||
      motors[i].phase == MOTOR_PULLING ||
      motors[i].phase == MOTOR_RELEASING
    ) {
      return true;
    }
  }

  return false;
}

// "Is ANY motor calibrating right now?"
bool anyMotorHoming() {

  for (int i = 0; i < NUM_MOTORS; i++) {
    if (motors[i].phase == MOTOR_HOMING) {
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------
// Tell everything to stop repeating and head home safely.
//  - A motor that is WAITING simply cancels (never starts).
//  - A motor that is PULLING turns around and RELEASES back to the origin.
//  - Motors already releasing carry on and finish as normal.
// ---------------------------------------------------------------------
void returnAllToHome() {

  repeatCycles = false;      // no more repeats

  for (int i = 0; i < NUM_MOTORS; i++) {

    if (motors[i].phase == MOTOR_WAITING) {
      motors[i].phase = MOTOR_IDLE;
    }
    else if (motors[i].phase == MOTOR_PULLING) {
      motors[i].phase = MOTOR_RELEASING;
    }
  }
}


// =====================================================================
// SECTION 10: STARTING THE BIG ACTIONS
// =====================================================================

// ---------------------------------------------------------------------
// Start CALIBRATION. All motors push forward against the wheat to find
// the origin. Only called when all motors are idle.
// ---------------------------------------------------------------------
void startHoming() {

  int steps;   // how far to push

  if (fullRangeHoming) {
    // First time after power-up: position unknown, push the longest distance.
    steps = STARTUP_HOMING_STEPS;
  }
  else if (atRest) {
    // We're sitting at the rest position, so push back past that distance.
    steps = REST_OFFSET_STEPS + HOMING_EXTRA_STEPS;
  }
  else {
    // We just finished a sway so we should be near the origin already.
    steps = HOMING_EXTRA_STEPS;
  }

  // Put every motor in HOMING mode with that many steps to go.
  for (int i = 0; i < NUM_MOTORS; i++) {
    motors[i].phase = MOTOR_HOMING;
    motors[i].homingRemaining = steps;
  }

  stepIntervalUs = HOMING_INTERVAL_US;   // switch to the slow, strong speed
  lastStepMicros = micros();             // note the time (starts the step timer)

  needsHoming = false;       // calibration is now underway
  fullRangeHoming = false;   // later calibrations don't need the long one
  homingActive = true;
  atRest = false;

  Serial.println("CALIBRATING: pushing against the wheat to find the origin");
}

// ---------------------------------------------------------------------
// Start moving all motors from the origin to the rest position.
// ---------------------------------------------------------------------
void startMoveToRest() {

  for (int i = 0; i < NUM_MOTORS; i++) {
    motors[i].phase = MOTOR_TO_REST;
    motors[i].position = 0;              // we are starting from the origin
  }

  stepIntervalUs = MID_INTERVAL_US;      // normal speed again
  lastStepMicros = micros();
  atRest = true;

  Serial.println("MOVING TO REST");
}

// ---------------------------------------------------------------------
// Start one of the "shows" (single, swing, wave right, wave left).
// Only called when all motors are home.
// ---------------------------------------------------------------------
void startMode(Mode mode) {

  currentMode = mode;

  unsigned long now = millis();    // millis() = milliseconds since power-on

  for (int i = 0; i < NUM_MOTORS; i++) {

    unsigned long delayMs = 0;                 // normally no delay
    int fromLeft = motorPositionFromLeft[i];   // where this motor sits

    if (mode == MODE_WAVE_RIGHT) {
      // Left -> right: the further right a motor is, the longer it waits.
      // Left motor waits 0, middle waits 500 ms, right waits 1000 ms.
      delayMs = (unsigned long)fromLeft * WAVE_DELAY_MS;
    }
    else if (mode == MODE_WAVE_LEFT) {
      // Right -> left: same idea but reversed, so the RIGHT motor goes first.
      delayMs = (unsigned long)(NUM_MOTORS - 1 - fromLeft) * WAVE_DELAY_MS;
    }

    // Every motor goes to WAITING with a start time. For swing and single
    // the delay is 0, so they all begin immediately and move together.
    motors[i].phase = MOTOR_WAITING;
    motors[i].startAtMs = now + delayMs;
  }

  // When this show ends, calibrate again so the origin stays accurate.
  needsHoming = true;

  // Set the speed and repeat behaviour for the chosen show.
  if (mode == MODE_SINGLE) {
    stepIntervalUs = SINGLE_INTERVAL_US;
    repeatCycles = false;            // do it once
    announceWhenIdle = true;         // print "SINGLE SWAY COMPLETED" afterwards
    Serial.println("RUNNING: SINGLE SWAY");
  }
  else if (mode == MODE_SWING) {
    stepIntervalUs = SWING_INTERVAL_US;
    repeatCycles = true;             // keep going
    Serial.println("RUNNING: CONTINUOUS SWAY");
  }
  else if (mode == MODE_WAVE_RIGHT) {
    stepIntervalUs = WAVE_INTERVAL_US;
    repeatCycles = true;
    Serial.println("RUNNING: WAVE LEFT TO RIGHT");
  }
  else if (mode == MODE_WAVE_LEFT) {
    stepIntervalUs = WAVE_INTERVAL_US;
    repeatCycles = true;
    Serial.println("RUNNING: WAVE RIGHT TO LEFT");
  }

  lastStepMicros = micros();
}


// =====================================================================
// SECTION 11: DECIDING WHAT TO DO WITH A NUMBER FROM THE COMPUTER
// =====================================================================

void handleCommand(int command) {

  // Echo the number back so you can see it in the Serial Monitor.
  Serial.print("RECEIVED CLASS: ");
  Serial.println(command);

  // ---- Rule for number 1: only allowed ONCE until a 2-5 comes in ----
  // (This stops the camera spamming "1" and restarting the sway nonstop.)
  if (command == 1) {
    if (singleSwayTriggered) {
      Serial.println("STATE 1 IGNORED: ALREADY TRIGGERED");
      return;                          // "return" = leave the function now
    }

    singleSwayTriggered = true;        // remember we've used it
  }
  // Any other valid number (2-5) "re-arms" number 1.
  else if (command >= 2 && command <= 5) {
    singleSwayTriggered = false;
  }

  // ---- Number 5 = STOP ----
  if (command == 5) {

    hasPending = false;        // cancel anything waiting in line
    returnAllToHome();         // finish moving and go home
    currentMode = MODE_STOP;
    announceWhenIdle = true;   // print "STOPPED" when it's done

    Serial.println("STOP (returning to the origin, then rest)");
    return;
  }

  // ---- Turn the number into a Mode ----
  Mode requested;

  if (command == 1) {
    requested = MODE_SINGLE;
  }
  else if (command == 2) {
    requested = MODE_SWING;
  }
  else if (command == 3) {
    requested = MODE_WAVE_RIGHT;
  }
  else if (command == 4) {
    requested = MODE_WAVE_LEFT;
  }
  else {
    // Not 1-5, so ignore it completely.
    Serial.println("IGNORED (not a class from 1 to 5)");
    return;
  }

  // ---- Skip the request if it would change nothing ----

  // Already waiting in line? Nothing more to do.
  if (hasPending && pendingMode == requested) {
    return;
  }

  // Already running exactly this show? Nothing more to do.
  // (The "!" means "not", and "&&" means "and".)
  if (
    !hasPending &&
    currentMode == requested &&
    movementActive() &&
    (repeatCycles || requested == MODE_SINGLE)
  ) {
    return;
  }

  // ---- Otherwise: queue the new show ----
  // The motors first finish what they're doing and head home. Later,
  // manageStates() sees the queued show and starts it.
  returnAllToHome();
  pendingMode = requested;
  hasPending = true;
}


// =====================================================================
// SECTION 12: THE "MANAGER" - DECIDES WHAT HAPPENS NEXT
// This is checked over and over. It only acts once ALL motors are idle.
// Order of priority:
//   1. Calibrate (after a show ends, or at power-up)
//   2. Start the queued show (calibrating first if we're at rest)
//   3. If nothing is queued, go to the rest position
//   4. Print a "done" message
// =====================================================================

void manageStates() {

  // If any motor is still busy, there is nothing to decide yet.
  if (!allMotorsIdle()) {
    return;
  }

  // 1. A show just finished (or we just powered on): calibrate.
  if (needsHoming) {
    startHoming();
    return;
  }

  // 2. A new show is waiting in line.
  if (hasPending) {
    if (atRest) {
      startHoming();        // we're at rest, so find the origin first
      return;
    }
    hasPending = false;
    startMode(pendingMode); // all set, start the show!
    return;
  }

  // 3. Calibrated and nothing queued: go and sit at the rest position.
  if (!atRest && calibrated) {
    startMoveToRest();
    return;
  }

  // 4. Everything is finished. Print a message if we owe one.
  if (announceWhenIdle) {
    announceWhenIdle = false;

    if (currentMode == MODE_SINGLE) {
      Serial.println("SINGLE SWAY COMPLETED");
    }
    else {
      Serial.println("STOPPED");
    }
    Serial.println("READY");
  }
}


// =====================================================================
// SECTION 13: THE "MOVER" - ACTUALLY MOVES THE MOTORS
// Called constantly. It takes tiny steps whenever it's time to.
// =====================================================================

void updateMotors() {

  // ---- Part A: wake up WAITING motors whose start time has arrived ----
  // This is what makes the wave: each motor has its own start time.
  unsigned long nowMs = millis();

  for (int i = 0; i < NUM_MOTORS; i++) {
    // The "(long)" trick keeps the maths correct even when the millisecond
    // clock eventually rolls over back to zero (after about 50 days).
    if (
      motors[i].phase == MOTOR_WAITING &&
      (long)(nowMs - motors[i].startAtMs) >= 0
    ) {
      motors[i].phase = MOTOR_PULLING;
    }
  }

  // ---- Part B: only step if enough time has passed since the last step ----
  // One shared timer keeps all three motors stepping at the same pace.
  unsigned long nowUs = micros();

  if ((unsigned long)(nowUs - lastStepMicros) < stepIntervalUs) {
    return;   // too soon, leave and try again next time round
  }

  lastStepMicros = nowUs;   // reset the timer

  // ---- Part C: give each motor ONE step according to its phase ----
  for (int i = 0; i < NUM_MOTORS; i++) {

    // PULLING: pull the wheat backward
    if (motors[i].phase == MOTOR_PULLING) {

      stepMotor(i, PULL_DIRECTION);   // one step back
      motors[i].position++;           // we're one step further from the origin

      // Pulled far enough? Switch to releasing.
      if (motors[i].position >= MOVE_STEPS) {
        motors[i].phase = MOTOR_RELEASING;
      }
    }

    // RELEASING: let the wheat swing forward, back toward the origin
    else if (motors[i].phase == MOTOR_RELEASING) {

      // Where do we stop? Normally at 0 (the origin).
      // In continuous modes, go a little PAST it (-60) to re-sync.
      int floorPosition = repeatCycles ? -SETTLE_STEPS : 0;

      // Still above the stopping point? Take one step forward.
      if (motors[i].position > floorPosition) {
        stepMotor(i, RELEASE_DIRECTION);
        motors[i].position--;
      }

      // Reached the stopping point?
      if (motors[i].position <= floorPosition) {
        motors[i].position = 0;   // call this spot the origin again

        if (repeatCycles) {
          motors[i].phase = MOTOR_PULLING;   // go round again
        }
        else {
          motors[i].phase = MOTOR_IDLE;      // all done
        }
      }
    }

    // HOMING: push hard against the wheat to find the origin
    else if (motors[i].phase == MOTOR_HOMING) {

      // Strong two-coil step. "moved" is how many half-steps it travelled.
      int moved = stepMotorFullPower(i, HOMING_DIRECTION);
      motors[i].homingRemaining -= moved;   // count down the push distance

      if (motors[i].homingRemaining <= 0) {
        motors[i].position = 0;     // this spot IS the origin now
        motors[i].phase = MOTOR_IDLE;
      }
    }

    // TO_REST: pull back a small distance from the origin and park
    else if (motors[i].phase == MOTOR_TO_REST) {

      stepMotor(i, PULL_DIRECTION);
      motors[i].position++;

      if (motors[i].position >= REST_OFFSET_STEPS) {
        motors[i].phase = MOTOR_IDLE;   // arrived at rest
      }
    }
  }

  // ---- Part D: check whether calibration just finished ----
  if (homingActive && !anyMotorHoming()) {

    homingActive = false;
    Serial.println("ORIGIN SET");

    // The very first time, announce that we're ready for commands.
    if (!calibrated) {
      calibrated = true;
      Serial.println("CALIBRATION COMPLETE");
      Serial.println("READY");
    }
  }
}


// =====================================================================
// SECTION 14: LISTENING TO THE COMPUTER (SERIAL)
// Characters arrive one at a time. For "3" followed by Enter we receive
// '3' then a newline. We collect digits into lineBuffer, and when the
// newline arrives we turn the collected text into a real number.
// =====================================================================

void receiveSerialCommand() {

  // While there are unread characters waiting...
  while (Serial.available() > 0) {

    char c = Serial.read();   // grab the next character

    // '?' is a "are you there?" check from the computer.
    // We answer READY or CALIBRATING.
    if (c == '?') {
      Serial.println(calibrated ? "READY" : "CALIBRATING");
      continue;               // skip to the next character
    }

    // A newline means "that's the end of the number".
    if (c == '\n' || c == '\r') {

      if (lineLength > 0) {                 // only if we collected something
        lineBuffer[lineLength] = '\0';      // mark the end of the text
        int command = atoi(lineBuffer);     // convert text "3" into number 3
        lineLength = 0;                     // empty the notepad
        handleCommand(command);             // act on the number
      }

      continue;
    }

    // If the character is a digit 0-9, add it to the notepad.
    if (c >= '0' && c <= '9') {
      // Only if there's still room in the notepad.
      if (lineLength < sizeof(lineBuffer) - 1) {
        lineBuffer[lineLength] = c;
        lineLength++;
      }
    }
    // Any other character is just ignored.
  }
}


// =====================================================================
// SECTION 15: SETUP  (runs ONCE when the Arduino powers on)
// =====================================================================

void setup() {

  Serial.begin(9600);   // open USB communication at 9600 baud (speed)

  // Tell the Arduino that motor 1 and 2 pins are OUTPUTS
  // (the Arduino sends signals out of them).
  for (int i = 0; i < 4; i++) {
    pinMode(motor1Pins[i], OUTPUT);
    pinMode(motor2Pins[i], OUTPUT);
  }

  // Same for the three pins that talk to the 74HC595 chip.
  pinMode(DATA_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);

  digitalWrite(LATCH_PIN, HIGH);   // start with the latch in its resting state

  // Give every motor a clean starting state.
  // We don't know where the motors really are yet, so position is just a
  // placeholder until calibration finishes.
  for (int i = 0; i < NUM_MOTORS; i++) {

    motors[i].phase = MOTOR_IDLE;
    motors[i].stepIndex = 0;
    motors[i].position = 0;
    motors[i].homingRemaining = 0;
    motors[i].startAtMs = 0;

    writeMotor(i, stepSequence[0]);   // energise the first coil pattern
  }

  Serial.println("THREE MOTORS STARTING");

  // Make sure the first thing that happens in loop() is a full calibration.
  needsHoming = true;
  fullRangeHoming = true;
}


// =====================================================================
// SECTION 16: MAIN LOOP  (runs over and over, forever)
// The three jobs below repeat thousands of times per second:
//   1. Listen   - did the computer send a number?
//   2. Decide   - what should happen next?
//   3. Move     - take a motor step if it's time
// Because none of them "wait" or "pause", all three motors move smoothly
// at once and the Arduino can still hear new commands instantly.
// =====================================================================

void loop() {

  receiveSerialCommand();   // 1. listen

  manageStates();           // 2. decide

  updateMotors();           // 3. move
}
