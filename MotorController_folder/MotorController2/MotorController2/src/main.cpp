// Arduino core for setup/loop, Serial, and ESP32 runtime helpers.
#include <Arduino.h>
// Mesh networking library used to receive TargetHeight messages from the GUI.
#include <painlessMesh.h>
// NVS key-value storage used to persist the last known motor position across power cycles.
#include <Preferences.h>

// Mesh network settings shared with the GUI and the other controller.
#define MESH_PREFIX                  "PIMM"
// Shared mesh password so all nodes can join the same network.
#define MESH_PASSWORD                "Odisee1234"
// Shared mesh port so all nodes talk on the same channel.
#define MESH_PORT                    5555
// Name used to identify this controller in TargetHeight messages.
#define MOTOR_CONTROLLER_MESSAGE_TAG "TargetHeightMotor2"

// GPIO pin connected to the PU+ input of the ESS17-04 integrated stepper driver.
#define STEP_PIN         25
// GPIO pin connected to the DR+ input of the ESS17-04 integrated stepper driver.
#define DIR_PIN          26
// GPIO pin connected to the upper limit switch (wired to GND; active LOW with internal pull-up).
#define LIMIT_SWITCH_PIN 18
// GPIO pin connected to the ALM output of the ESS17-04 (active LOW through optocoupler; GND on ALM-).
#define ALM_PIN 32
// GPIO pin that pulses HIGH to clear a latched alarm on the ESS17-04.
#define ALM_CLEAR_PIN 35
// Duration of the alarm-clear pulse in milliseconds.
#define ALM_CLEAR_PULSE_MS 1000
// Height in tenths of mm that is established when the limit switch fires during calibration.
#define CALIB_HEIGHT_TENTHS_MM 860

// Step rate: 20 kHz gives 5 rev/s = 8.75 mm/s, well within the 200 kHz driver limit.
#define STEP_FREQ_HZ        20000UL
// Each half-period (rising edge duration and low-phase duration) at 20 kHz is 25 µs.
#define STEP_HALF_PERIOD_US (1000000UL / STEP_FREQ_HZ / 2)

// Direction pin level that raises the spindle (swap if the motor runs the wrong way).
#define DIR_INCREASE HIGH
// Direction pin level that lowers the spindle.
#define DIR_DECREASE LOW

// Target absolute position in motor pulses, written by the mesh callback and read by loop().
// Conversion: 4000 pulses/rev ÷ 17.5 tenths-of-mm/rev = 1600/7 pulses per 0.1 mm.
static volatile long target_position_pulses = 0;
// Current absolute position in pulses, tracked only inside loop() / step_motor().
static long              current_position_pulses = 0;
// True while the STEP pin is held HIGH (mid-pulse); false during the LOW phase.
// Volatile because the limit-switch ISR forces it LOW and must clear this flag atomically.
static volatile bool     step_pin_high           = false;
// Direction of the most recently commanded move; initialised to match DIR_INCREASE.
static bool              step_dir_up             = true;
// Timestamp in µs at which the next step-pin transition should occur.
static uint32_t          next_step_us            = 0;
// Timestamp in ms of the most recent ActualHeightMotor2 broadcast.
static uint32_t          last_broadcast_ms       = 0;
// Timestamp in ms of the most recent NVS position save.
static uint32_t          last_save_ms            = 0;
// How often (ms) to write the current position to NVS.
#define SAVE_INTERVAL_MS 1000

// NVS storage object for persisting the last known position.
static Preferences prefs;

// How often (ms) to re-broadcast an active alarm so the GUI recovers it after reconnect.
#define ALM_REBROADCAST_MS 5000

// True while the ALM pin is currently LOW (alarm active).
static bool     alarm_active       = false;
// True while movement is blocked because an unacknowledged alarm is active.
static bool     alarm_blocked      = false;
// millis() of the last "AlarmMotor2:1" broadcast.
static uint32_t alarm_broadcast_ms = 0;
// True while GPIO35 is being held HIGH to clear the latched alarm on the ESS17-04.
static bool     alm_clear_pulse_active   = false;
// millis() when the alarm-clear pulse started.
static uint32_t alm_clear_pulse_start_ms = 0;
// True while the motor is performing a calibration move toward the limit switch.
// Volatile because it is read inside the ISR.
static volatile bool     calibrating            = false;
// Set by the limit-switch ISR the instant the pin falls LOW during calibration.
// Checked by step_motor() to suppress further pulses before loop() can react.
static volatile bool     limit_switch_triggered = false;

// Scheduler required by painlessMesh.
static Scheduler    userScheduler;
// Mesh network object used for receive callbacks.
static painlessMesh mesh;

// Convert a target height in tenths of mm to an absolute pulse count from the home position.
// The exact ratio is 4000 pulses / 1.75 mm = 4000 / 17.5 tenths = 1600/7 pulses per 0.1 mm.
static long tenths_mm_to_pulses(int tenths_mm)
{
    return ((long)tenths_mm * 1600L) / 7L;
}

// Convert an absolute pulse count back to a height in tenths of mm (inverse of tenths_mm_to_pulses).
// Adding half the denominator (800) before dividing rounds to the nearest tenth instead of truncating,
// which compensates for the floor in tenths_mm_to_pulses and prevents a persistent -0.1 mm display offset.
static int pulses_to_tenths_mm(long pulses)
{
    return (int)((pulses * 7L + 800L) / 1600L);
}

// Store a new target position when a valid TargetHeightMotor2 message is received.
static void handle_target_height_message(int target_height_tenths_mm)
{
    target_position_pulses = tenths_mm_to_pulses(target_height_tenths_mm);
}

// Begin a calibration move: command the motor to drive upward past the physical maximum
// so that the loop can detect the limit switch and establish the true home position.
static void start_calibration()
{
    calibrating            = true;
    target_position_pulses = tenths_mm_to_pulses(CALIB_HEIGHT_TENTHS_MM + 1000); // Aim well above the limit so step_motor() keeps moving up.
}

// Parse incoming mesh messages and route them to the appropriate handler.
static void handle_mesh_message(uint32_t from, const String &msg)
{
    (void)from;

    if (msg == "CalibrateMotor2") { // Respond to a calibration command from the GUI.
        start_calibration();
        return;
    }

    if (msg == "ResetAlarmMotor2") { // Operator acknowledged the alarm — re-enable movement and clear the driver latch.
        alarm_blocked            = false;
        alm_clear_pulse_active   = true;
        alm_clear_pulse_start_ms = millis();
        digitalWrite(ALM_CLEAR_PIN, HIGH);
        return;
    }

    if (calibrating) { // Ignore all other commands until calibration is complete.
        return;
    }

    if (!msg.startsWith(MOTOR_CONTROLLER_MESSAGE_TAG)) { // Ignore messages not addressed to this controller.
        return;
    }

    const int sep = msg.indexOf(':'); // Locate the separator between the tag and the value.
    if (sep < 0) { // Reject malformed messages that contain no separator.
        return;
    }

    handle_target_height_message(msg.substring(sep + 1).toInt()); // Extract and forward the target height.
}

// ISR: fires on the falling edge of LIMIT_SWITCH_PIN (active LOW).
// Immediately halts pulse generation by forcing STEP_PIN low and setting the trigger flag.
// Runs in interrupt context — keep it minimal.
void IRAM_ATTR limit_switch_isr()
{
    if (!calibrating) return;         // Ignore limit switch outside of a calibration move.
    digitalWrite(STEP_PIN, LOW);      // Abort any in-progress high pulse right now.
    step_pin_high          = false;   // Tell step_motor() the pin is already low.
    limit_switch_triggered = true;    // Signal loop() to finalise calibration.
}

// Advance the stepper by one half-step when the step timer has elapsed.
// This function is non-blocking: it returns immediately if no transition is due.
// The direction pin is updated during the LOW phase so it settles for a full
// STEP_HALF_PERIOD_US (25 µs) before the next rising edge, which satisfies the
// ESS17-04 requirement of at least 2.5 µs setup time before the pulse rising edge.
static void step_motor()
{
    if (limit_switch_triggered) return; // ISR already stopped the pulse; wait for loop() to update state.

    const long target = target_position_pulses; // Snapshot the volatile target once per call.

    if (!step_pin_high && target == current_position_pulses) { // Nothing to do when already at the target.
        return;
    }

    const uint32_t now = micros();
    if ((int32_t)(now - next_step_us) < 0) { // Return early when it is not yet time for the next transition.
        return;
    }

    if (!step_pin_high) {
        const bool need_up = (target > current_position_pulses); // Determine the required direction.

        if (need_up != step_dir_up) {
            // Direction change: update the direction pin now while the STEP pin is LOW.
            // The next rising edge will not be generated until one more half-period has elapsed,
            // guaranteeing that the direction signal is stable for at least 25 µs beforehand.
            digitalWrite(DIR_PIN, need_up ? DIR_INCREASE : DIR_DECREASE);
            step_dir_up  = need_up;
            next_step_us = now + STEP_HALF_PERIOD_US; // Wait for direction to settle before stepping.
            return;
        }

        // Direction is already correct – generate the rising edge of the step pulse.
        digitalWrite(STEP_PIN, HIGH);
        step_pin_high = true;
    } else {
        // Generate the falling edge and count the completed step.
        digitalWrite(STEP_PIN, LOW);
        step_pin_high           = false;
        current_position_pulses += step_dir_up ? 1L : -1L; // Advance position by one pulse in the active direction.
    }

    next_step_us = now + STEP_HALF_PERIOD_US; // Schedule the next pin transition.
}

// Configure the mesh network and register the receive callback.
static void init_mesh()
{
    mesh.setDebugMsgTypes(ERROR | STARTUP); // Keep mesh logging limited to startup and errors.
    mesh.onReceive(handle_mesh_message); // Register the callback that filters TargetHeight messages.
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT); // Join the shared mesh network.
}

// Arduino startup entry point.
void setup()
{
    Serial.begin(115200);
    delay(100);

    pinMode(STEP_PIN, OUTPUT);         // Configure the step pin as a digital output.
    pinMode(DIR_PIN, OUTPUT);          // Configure the direction pin as a digital output.
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP); // Enable the internal pull-up; the switch pulls the pin LOW when pressed.
    pinMode(ALM_PIN, INPUT_PULLUP);    // ALM output from ESS17-04 pulls this LOW through its optocoupler when an alarm fires.
    pinMode(ALM_CLEAR_PIN, OUTPUT);    // Alarm-clear output: pulse HIGH to release the latched fault on the ESS17-04.
    digitalWrite(ALM_CLEAR_PIN, LOW);  // Keep the clear pin LOW at startup so it does not trigger a spurious clear.
    digitalWrite(STEP_PIN, LOW);       // Hold the step pin low at startup.
    digitalWrite(DIR_PIN, DIR_INCREASE); // Set an initial direction matching step_dir_up = true.
    attachInterrupt(digitalPinToInterrupt(LIMIT_SWITCH_PIN), limit_switch_isr, FALLING); // Stop pulsing the instant the switch activates.

    prefs.begin("mc2", false); // Open the NVS namespace for this controller.
    current_position_pulses = prefs.getLong("pos", 0); // Restore the last saved position (default 0 if never saved).
    target_position_pulses  = current_position_pulses; // Stay at the restored position until the GUI sends a new target.

    init_mesh(); // Bring up the mesh network.
    Serial.println("MotorController2 ready for TargetHeightMotor2 messages");
}

// Arduino loop services the mesh stack, drives the stepper motor, and reports actual height.
void loop()
{
    mesh.update(); // Let painlessMesh process incoming and outgoing traffic.

    // The ISR sets limit_switch_triggered the instant the pin falls LOW, suppressing all further
    // pulses before loop() even gets here.  Fall back to digitalRead() as a secondary check in
    // case the switch was already LOW before the interrupt was attached (e.g. held at startup).
    if (calibrating && (limit_switch_triggered || digitalRead(LIMIT_SWITCH_PIN) == LOW)) {
        // Limit switch fired: the physical position is now known to be exactly CALIB_HEIGHT_TENTHS_MM.
        digitalWrite(STEP_PIN, LOW);            // Ensure the step pin is low (defensive, ISR may have done this).
        step_pin_high           = false;
        const long calib_pulses = tenths_mm_to_pulses(CALIB_HEIGHT_TENTHS_MM);
        current_position_pulses = calib_pulses; // Establish the calibrated position in the pulse counter.
        target_position_pulses  = calib_pulses; // Stop the motor at the calibrated position.
        limit_switch_triggered  = false;        // Clear the ISR flag before re-enabling normal operation.
        calibrating             = false;
        prefs.putLong("pos", current_position_pulses); // Position is exactly known after calibration — save immediately.
        last_save_ms = millis();
        mesh.sendBroadcast("CalibratedMotor2"); // Tell the GUI that calibration is done and the height is 86.0 mm.
    }

    if (!alarm_blocked) step_motor(); // Suppress all stepping while an unacknowledged alarm is active.

    const uint32_t now_ms = millis();

    // End the alarm-clear pulse after ALM_CLEAR_PULSE_MS.
    if (alm_clear_pulse_active && (now_ms - alm_clear_pulse_start_ms) >= ALM_CLEAR_PULSE_MS) {
        digitalWrite(ALM_CLEAR_PIN, LOW);
        alm_clear_pulse_active = false;
    }

    // --- ALM pin detection: LOW = alarm active, HIGH = no alarm ---
    const bool alm_low = (digitalRead(ALM_PIN) == LOW);

    if (alm_low && !alarm_active) {
        alarm_active       = true;
        alarm_blocked      = true;
        alarm_broadcast_ms = now_ms;
        mesh.sendBroadcast("AlarmMotor2:1");
    } else if (!alm_low && alarm_active) {
        alarm_active  = false;
        alarm_blocked = false;
        mesh.sendBroadcast("AlarmMotor2:0");
    }

    if (alarm_active && (now_ms - alarm_broadcast_ms) > ALM_REBROADCAST_MS) {
        mesh.sendBroadcast("AlarmMotor2:1");
        alarm_broadcast_ms = now_ms;
    }

    if (now_ms - last_save_ms >= SAVE_INTERVAL_MS) { // Persist the current position to NVS every second.
        prefs.putLong("pos", current_position_pulses);
        last_save_ms = now_ms;
    }

    if (now_ms - last_broadcast_ms >= 100) { // Broadcast the actual height to the GUI every 100 ms.
        const int actual_tenths_mm = pulses_to_tenths_mm(current_position_pulses);
        mesh.sendBroadcast("ActualHeightMotor2:" + String(actual_tenths_mm));
        last_broadcast_ms = now_ms;
    }
}
