//======= LIBRARIES =======
#include <Arduino.h> // Standard Arduino functions for GPIO, Serial communication, timing
#include "painlessMesh.h" // Mesh networking library for ESP-to-ESP wireless communication

//======= MESH NETWORK CONFIGURATION =======
#define   MESH_PREFIX     "PIMM" // Network name - all devices with same prefix connect together
#define   MESH_PASSWORD   "Odisee1234" // Network password for security
#define   MESH_PORT       5555 // Network port number (5555 is standard for painlessMesh)
painlessMesh  mesh; // Create mesh network object for communication

Scheduler userScheduler; // Task scheduler to manage timing of operations

#define DEVICE_NUM 1 // Unique device number for this module (1 for AanslagModule1, 2 for AanslagModule2)

//======= HEIGHT VARIABLES & LIMITS =======
int height = 0; // Current height position (in 0.1mm units, range 0-100mm)
int previousHeight = 0; // Tracks previous height to calculate motor movement needed
#define HEIGHT_MIN 0 // Minimum allowed height: 0mm
#define HEIGHT_MAX 1000 // Maximum allowed height: 100mm (1000 × 0.1mm units)
#define HEIGHT_STEP 1 // Each control command moves 0.1mm

//======= STEPPER MOTOR CONFIGURATION =======
#define DIR_PIN 26 // GPIO26: Direction control pin (HIGH=up, LOW=down)
#define STEP_PIN 25 // GPIO25: Step/pulse pin (pulse triggers one motor step)
#define MICROSTEP 4.0 // Define microstepping mode (1/4 step = 4x resolution) for smoother motor movement and higher resolution
#define STEPS_PER_1_75MM (200*MICROSTEP) // Hardware specification: 1.75mm travel = 200 steps × 4 (1/4 microstepping) = 800 steps
#define STEPS_PER_0_1MM (STEPS_PER_1_75MM / 17.5) // Calculate steps needed per 0.1mm (since 1.75mm = 17.5 × 0.1mm) = 45.71 steps per 0.1mm

//======= MOTOR TIMING CONFIGURATION =======
#define STEP_PULSE_DURATION 400 // Keep step pin HIGH for 5 microseconds
#define STEP_DELAY 400 // Wait 5 microseconds between pulses (controls motor speed)

// Function prototypes
void receivedCallback( uint32_t from, String &msg );
void moveMotor(int targetPosition);
void stepMotor(bool direction, int steps);

//======= MOTOR MOVEMENT FUNCTION =======
// Calculates steps needed and sends motor movement command
void moveMotor(int targetHeight) {
  // Calculate height change to determine motor movement
  int heightDifference = targetHeight - previousHeight; // Difference in 0.1mm units
  
  if (heightDifference == 0) { // No change in height?
    Serial.println("Motor already at target height");
    return; // Skip motor movement
  }
  
  // Convert height change to motor steps
  int stepsToMove = (int)(abs(heightDifference) * STEPS_PER_0_1MM + 0.5); // abs() ignores direction, +0.5 rounds to nearest int
  
  // Determine direction: positive = up, negative = down
  if (heightDifference > 0) { // Height increased?
    Serial.printf("Moving UP: %d steps\n", stepsToMove);
    stepMotor(HIGH, stepsToMove); // HIGH = move up
  } 
  else { // Height decreased?
    Serial.printf("Moving DOWN: %d steps\n", stepsToMove);
    stepMotor(LOW, stepsToMove); // LOW = move down
  }


  previousHeight = targetHeight; // Update reference for next movement calculation
}

//======= STEPPER MOTOR PULSE GENERATION =======
// Sends step pulses to motor driver to move motor in specified direction
void stepMotor(bool direction, int steps) {
  digitalWrite(DIR_PIN, direction); // Set direction pin: HIGH = move up, LOW = move down
  delayMicroseconds(125); // Wait 125µs for motor driver to register direction change
  
  for (int i = 0; i < steps; i++) { // Loop for each step required
    digitalWrite(STEP_PIN, LOW); // Pulse LOW
    delayMicroseconds(STEP_PULSE_DURATION); // Hold LOW for 5µs (driver measures this pulse)
    digitalWrite(STEP_PIN, HIGH); // Pulse HIGH
    delayMicroseconds(STEP_DELAY); // Wait 5µs before sending next pulse (controls motor speed)
  }
  
  Serial.printf("Motor completed %d steps in direction %s\n", steps, direction ? "UP" : "DOWN");
  
  // Broadcast height to mesh network
  String heightMsg = "device:" + String(DEVICE_NUM) + ":height:" + String(height);
  mesh.sendBroadcast(heightMsg);
  Serial.printf("Broadcasting: %s\n", heightMsg.c_str());
}

//======= MESH MESSAGE RECEIVER CALLBACK =======
// Executed whenever a message arrives from UserController via mesh network
void receivedCallback( uint32_t from, String &msg ) {
  Serial.printf("receivedCallback --> from: %u, msg: %s\n", from, msg.c_str()); // Log received message
  
  // Expected message format: "device:1:control:increase" or "device:1:control:decrease"
  // This module is device 1 (AanslagModule1)
  
  // Check if this message is for this device
  // Format: "device:1:control:increase" or "device:1:control:decrease"
  if (msg.startsWith("device:" + String(DEVICE_NUM) + ":control:")) {
    String command = msg.substring(17);  // Extract command after "device:1:control:" (17 chars)
    
    // Process the control command
    if (command == "increase") {
      if (height < HEIGHT_MAX) {
        height += HEIGHT_STEP;
        if (height > HEIGHT_MAX) {
          height = HEIGHT_MAX; // Clamp to maximum
        }
        Serial.printf(">>> Height INCREASED to: %.1fmm\n", height / 10.0); // Print height in mm with 1 decimal place (height stored in 0.1mm units, so divide by 10)
        moveMotor(height); // Move motor to new height
      } else {
        Serial.println(">>> Height already at maximum");
      }
    }
    else if (command == "decrease") {
      if (height > HEIGHT_MIN) {
        height -= HEIGHT_STEP;
        if (height < HEIGHT_MIN) {
          height = HEIGHT_MIN; // Clamp to minimum
        }
        Serial.printf(">>> Height DECREASED to: %.1fmm\n", height / 10.0);
        moveMotor(height); // Move motor to new height
      } else {
        Serial.println(">>> Height already at minimum");
      }
    }
    else {
      Serial.printf(">>> Unknown command: %s\n", command.c_str());
    }
  }
}

//======= SETUP FUNCTION - Runs once at startup =======
void setup() {
  Serial.begin(115200); // Start serial communication at 115200 baud for debugging
  delay(100); // Small delay for serial to stabilize
  Serial.println("\n\nStarting AanslagModule1...\n");
  
  // Configure stepper motor GPIO pins
  pinMode(DIR_PIN, OUTPUT); // Configure GPIO26 as output for direction control
  pinMode(STEP_PIN, OUTPUT); // Configure GPIO25 as output for step pulses
  digitalWrite(DIR_PIN, HIGH); // Initialize direction pin HIGH (up)
  digitalWrite(STEP_PIN, HIGH); // Initialize step pin HIGH (idle state)
  Serial.println("Stepper motor pins initialized");
  
  // Initialize mesh network communication
  mesh.setDebugMsgTypes( ERROR | STARTUP ); // Show error and startup messages
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT ); // Start mesh with configured credentials
  mesh.onReceive(&receivedCallback); // Register callback - runs when message arrives from UserController
  
  Serial.println("Mesh initialized. Waiting for control commands...\n");
}

//======= MAIN LOOP - Runs repeatedly =======
void loop() {
  mesh.update(); // Check for incoming messages (must be called frequently to receive commands)
}