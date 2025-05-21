#include <CapacitiveSensor.h>     // Library used to read capacitive input from sensor pins
#include <LiquidCrystal_I2C.h>    // Library used to control the LCD via I2C protocol

// ───── Pin definitions ─────
#define SEND_PIN     2           // Common send pin for all capacitive sensor pairs
#define BUZZER_PIN   11          // Pin connected to the buzzer
#define LED_PIN      12          // Pin controlling the LED indicator for record mode
#define BTN_TOGGLE   A1          // Button used to start/stop recording
#define BTN_PLAY     A0          // Button used to trigger playback

// ───── Capacitive sensor configuration ─────
const byte RECV_PIN[8] = {3, 4, 5, 6, 7, 8, 9, 10};   // Pins receiving capacitive input for each note (Do to Do2)
const uint8_t N_CH = 8;                              // Total number of capacitive input channels

// ───── Note configuration ─────
const uint16_t NOTE_FREQ[8] = {262, 294, 330, 349, 392, 440, 494, 523};   // Frequencies for notes C4 to C5
const char*    NOTE_NAME[8] = {"Do", "Re", "Mi", "Fa", "Sol", "La", "Si", "Do2"}; // Note names shown on the LCD

// ───── Touch detection thresholds ─────
const uint16_t TOUCH_THR   = 10;    // Minimum delta to register a valid touch
const uint16_t RELEASE_THR = 15;    // Minimum drop to consider a note released (for hysteresis)

// ───── Recording configuration ─────
const uint8_t MAX_RECORDS          = 5;   // Maximum number of songs stored in memory
const uint8_t MAX_NOTES_PER_RECORD = 50;  // Maximum number of notes per recorded song

// ───── Data structure representing a recorded note event ─────
struct NoteEvent {
  uint8_t  note;       // Note index: 0 to 7 (Do to Do2), 255 represents a pause
  uint16_t duration;   // Duration in milliseconds
};

// ───── Memory arrays for recorded songs ─────
NoteEvent records[MAX_RECORDS][MAX_NOTES_PER_RECORD]; // Storage for all recorded notes
uint16_t  recordLens[MAX_RECORDS] = {0};              // Length of each recorded song

// ───── Playback state ─────
uint8_t currentRecord = 0;      // Index of the current recording slot
uint8_t totalSaved    = 0;      // Total number of recordings saved
uint8_t playbackIndex = 0;      // Index used to rotate through saved recordings

// ───── Button state tracking ─────
bool ledState     = false;      // Indicates if the device is currently recording
bool lastRecBtn   = HIGH;       // Previous state of the record button
bool lastPlayBtn  = HIGH;       // Previous state of the play button

// ───── Playback state tracking ─────
bool    playing      = false;   // Indicates if a note is currently being played
int8_t  notePlaying  = -1;      // Index of the note being played (-1 if none)
bool    wasPlaying   = false;   // Indicates whether a note was previously being played

// ───── Timing state tracking ─────
uint32_t lastNoteTime    = 0;   // Timestamp when the last note started
uint32_t lastReleaseTime = 0;   // Timestamp when the last note was released

// ───── Array of capacitive sensor objects, one for each note ─────
CapacitiveSensor cs[N_CH] = {
  CapacitiveSensor(SEND_PIN, 3),  CapacitiveSensor(SEND_PIN, 4),
  CapacitiveSensor(SEND_PIN, 5),  CapacitiveSensor(SEND_PIN, 6),
  CapacitiveSensor(SEND_PIN, 7),  CapacitiveSensor(SEND_PIN, 8),
  CapacitiveSensor(SEND_PIN, 9),  CapacitiveSensor(SEND_PIN,10)
};

// ───── LCD object for I2C display ─────
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Address 0x27, 16 characters × 2 rows

// ───── Baseline values used to detect touch on each channel ─────
uint16_t zeroLevel[N_CH];  // Dynamic zero-reference level per channel


void setup() {
  // Initialize serial communication for debugging and touch signal visualization
  Serial.begin(115200);

  // Set LED and buzzer pins as OUTPUT
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Set button pins as INPUT with internal pull-up resistors
  pinMode(BTN_TOGGLE, INPUT_PULLUP);  // Button for toggling record mode
  pinMode(BTN_PLAY, INPUT_PULLUP);    // Button for playing back a recorded song

  // Ensure LED is initially turned off
  digitalWrite(LED_PIN, LOW);

  // Initialize each capacitive sensor:
  // - disable auto-calibration for 10 seconds after boot
  // - set a short timeout to discard noisy/stuck readings
  // - initialize baseline (zeroLevel) for each channel
  for (byte i = 0; i < N_CH; ++i) {
    cs[i].set_CS_AutocaL_Millis(10000);        // wait 10 seconds before recalibration
    cs[i].set_CS_Timeout_Millis(30);           // sensor read timeout
    long val = cs[i].capacitiveSensor(10);     // average 10 readings for initial baseline
    zeroLevel[i] = (val < 0) ? 0 : val;         // store valid zero reference
  }

  // Initialize the I2C LCD display
  lcd.init();                // Initialize LCD module
  lcd.backlight();           // Turn on the backlight

  // Display welcome message
  lcd.setCursor(0, 0);       // First row
  lcd.print("Fruit Piano");
  lcd.setCursor(0, 1);       // Second row
  lcd.print("Touch a fruit");
}

// Handles the logic when the RECORD button is pressed.
// Toggles recording mode and manages LED state, LCD messages, and recording initialization.
void handleRecordButton() {
  bool recNow = digitalRead(BTN_TOGGLE); // Read current state of the RECORD button

  // Detect a rising edge (button press)
  if (recNow == LOW && lastRecBtn == HIGH) {
    // Toggle recording state (on/off)
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState); // Turn LED on or off accordingly

    if (ledState) {
      // --- Begin a new recording session ---
      recordLens[currentRecord] = 0;      // Reset recording length
      lastNoteTime = millis();            // Reset timing reference
      wasPlaying = false;                 // No note is playing yet

      // Update LCD to show recording status
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Recording...");

      Serial.println("START RECORDING");
    } else {
      // --- End current recording session ---
      Serial.print("RECORD DONE, notes=");
      Serial.println(recordLens[currentRecord]);

      // Advance to next recording slot
      currentRecord = (currentRecord + 1) % MAX_RECORDS;

      // Update total number of saved recordings if needed
      if (totalSaved < MAX_RECORDS) totalSaved++;

      // Reset playback index to last recording
      playbackIndex = 0;
    }
  }

  // Save current button state for edge detection in next loop
  lastRecBtn = recNow;
}

// Handles playback when the PLAY button is pressed.
// Cycles through saved recordings and plays them one by one with tone and LCD feedback.
void handlePlayButton() {
  bool playNow = digitalRead(BTN_PLAY); // Read current state of the PLAY button

  // Detect button press (falling edge) only when not recording and there are saved melodies
  if (playNow == LOW && lastPlayBtn == HIGH && !ledState && totalSaved) {
    uint8_t tries = 0;

    // Calculate the index of the recording to be played based on playback index
    uint8_t slot = (currentRecord + MAX_RECORDS - 1 - playbackIndex) % MAX_RECORDS;

    // Skip empty slots in case of overwritten recordings
    while (recordLens[slot] == 0 && tries < totalSaved) {
      playbackIndex = (playbackIndex + 1) % totalSaved;
      slot = (currentRecord + MAX_RECORDS - 1 - playbackIndex) % MAX_RECORDS;
      tries++;
    }

    // Exit early if there's still no valid recording
    if (recordLens[slot] == 0) return;

    // Display playback info on LCD
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Play recording ");
    lcd.print(playbackIndex + 1);

    // Iterate through the notes in the selected recording
    for (int i = 0; i < recordLens[slot]; ++i) {
      uint8_t n   = records[slot][i].note;
      uint16_t dur= records[slot][i].duration;

      if (n == 255) {
        // Play a pause (no sound)
        noTone(BUZZER_PIN);
      } else {
        // Play a note and show its name
        tone(BUZZER_PIN, NOTE_FREQ[n]);
        lcd.setCursor(0, 1);
        lcd.print("Nota: "); lcd.print(NOTE_NAME[n]); lcd.print("  ");
      }

      // Wait for the duration of the note or pause
      delay(dur);
    }

    // Stop buzzer and reset display
    noTone(BUZZER_PIN);
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Touch a fruit");

    // Move to the next recording for next playback
    playbackIndex = (playbackIndex + 1) % totalSaved;
  }

  // Save current state for next edge detection
  lastPlayBtn = playNow;
}


int8_t getStrongestTouch() {
  int8_t highest = -1;      // Index of the channel with strongest touch
  long bestDelta = 0;       // Highest detected delta among all channels

  // Loop through each capacitive sensor channel
  for (byte i = 0; i < N_CH; ++i) {
    long raw = cs[i].capacitiveSensor(2);   // Read touch value
    if (raw < 0) raw = 0;
    long delta = raw - zeroLevel[i];        // Compute difference from baseline

    // Reset baseline if value is abnormally high (noise rejection)
    if (abs(delta) > 1000) {
      zeroLevel[i] = raw;
      delta = 0;
    }

    // Slowly update zero level when signal is below release threshold
    if (delta < RELEASE_THR)
      zeroLevel[i] = (zeroLevel[i] * 7 + raw) >> 3;  // IIR low-pass filter

    // Identify the strongest touch above the threshold
    if (delta > TOUCH_THR && delta > bestDelta) {
      bestDelta = delta;
      highest = i;
    }

    // Print delta for each sensor to Serial Monitor (for debugging)
    Serial.print(delta); Serial.print('\t');
  }
  Serial.println();
  return highest;
}

// Determine if the previously playing note is still being touched
bool isCurrentNoteStillPressed() {
  return (playing && notePlaying != -1 &&
    (cs[notePlaying].capacitiveSensor(1) - zeroLevel[notePlaying]) >= RELEASE_THR);
}

void handleNewTouch(int8_t highest) {
  bool newNote = (!playing || notePlaying != highest);  // Is it a new note?

  if (newNote) {
    // If recording and previously released, store a pause event
    if (ledState && recordLens[currentRecord] && !wasPlaying) {
      uint32_t now = millis();
      uint16_t pauseDur = now - lastReleaseTime;
      if (pauseDur > 10 && recordLens[currentRecord] < MAX_NOTES_PER_RECORD) {
        records[currentRecord][recordLens[currentRecord]].note = 255; // Pause
        records[currentRecord][recordLens[currentRecord]].duration = pauseDur;
        recordLens[currentRecord]++;
      }
      lastNoteTime = millis();  // Mark the time new note begins
    }

    // Play the detected note via buzzer and show it on the LCD
    tone(BUZZER_PIN, NOTE_FREQ[highest]);
    lcd.setCursor(0, 0);
    lcd.print("Nota: "); lcd.print(NOTE_NAME[highest]); lcd.print("  ");
    notePlaying = highest;
    playing = true;

    // If in recording mode, store the new note
    if (ledState && recordLens[currentRecord] < MAX_NOTES_PER_RECORD) {
      records[currentRecord][recordLens[currentRecord]].note = highest;
      records[currentRecord][recordLens[currentRecord]].duration = 0;
      recordLens[currentRecord]++;
      lastNoteTime = millis();  // Start timing the note
      wasPlaying = true;
    }
  }
}

void handleTouchRelease() {
  noTone(BUZZER_PIN);                    // Stop sound
  lcd.setCursor(0, 0); lcd.print("                "); // Clear LCD line
  playing = false;
  notePlaying = -1;

  // If recording was active, save the duration of the last note
  if (ledState && recordLens[currentRecord] && wasPlaying) {
    uint32_t now = millis();
    records[currentRecord][recordLens[currentRecord] - 1].duration = now - lastNoteTime;
    lastReleaseTime = now;
    wasPlaying = false;
  }
}

// Reads capacitive sensor inputs and handles real-time note detection, playback, and recording.
void detectTouchAndPlayNote() {
  int8_t highest = getStrongestTouch();
  bool curStillOn = isCurrentNoteStillPressed();

  if (highest != -1) {
    // If a new valid touch is detected
    handleNewTouch(highest);
  } else if (!curStillOn && playing) {
      // If no touch is currently active and a note was playing before
    handleTouchRelease();
  }
}

void loop() {
  handleRecordButton();
  handlePlayButton();
  detectTouchAndPlayNote();
}
