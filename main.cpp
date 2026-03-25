#include <Arduino.h>
#include "painlessMesh.h"

#define   MESH_PREFIX     "PIMM"
#define   MESH_PASSWORD   "Odisee1234"
#define   MESH_PORT       5555
#define   DEVICE_NUM      1  // This is aanslag1

// Stepper motor driver pins (HY-DIV268N-5A)
#define   STEP_PIN        25  // Step signal
#define   DIR_PIN         26  // Direction signal

Scheduler userScheduler; // to control your personal task
painlessMesh  mesh;

int stepCount = 0;  // Track motor position
const unsigned long STEP_PULSE_DURATION = 10;  // Microseconds
const unsigned long STEP_DELAY = 10;  // Milliseconds between steps

unsigned long lastStatusSend = 0;
const unsigned long STATUS_INTERVAL = 10000;  // Send status every 10 seconds

void sendStatus() {
  String msg = "status:device:" + String(DEVICE_NUM) + ":steps:" + String(stepCount);
  mesh.sendBroadcast(msg);
  Serial.printf("Sent status: %s\n", msg.c_str());
}

void stepMotor(int direction) {
  // direction: 1 for clockwise, -1 for counter-clockwise
  if (direction > 0) {
    digitalWrite(DIR_PIN, HIGH);  // Clockwise
    Serial.println("Direction: CLOCKWISE");
  } else {
    digitalWrite(DIR_PIN, LOW);   // Counter-clockwise
    Serial.println("Direction: COUNTER-CLOCKWISE");
  }
  
  delay(1);  // Small delay for direction change to settle
  
  // Send step pulse
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(STEP_PULSE_DURATION);
  digitalWrite(STEP_PIN, LOW);
  
  stepCount += direction;
  Serial.printf("Motor stepped. Total steps: %d\n", stepCount);
  delay(STEP_DELAY);  // Delay between steps
}

// Needed for painless library
void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("Received from %u: %s\n", from, msg.c_str());
  
  // Check if this message is for this device
  // Format: "device:1:control:increase" or "device:1:control:decrease"
  if (msg.startsWith("device:" + String(DEVICE_NUM) + ":control:")) {
    String command = msg.substring(17);  // Extract command after "device:1:control:" (17 chars)
    
    if (command == "increase") {
      Serial.println(">>> Motor step forward (clockwise)");
      stepMotor(1);  // 1 for forward/clockwise
      sendStatus();
    } else if (command == "decrease") {
      Serial.println(">>> Motor step backward (counter-clockwise)");
      stepMotor(-1);  // -1 for backward/counter-clockwise
      sendStatus();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n\nStarting ESPaanslag%d (Stepper Motor Controller)...\n\n", DEVICE_NUM);

  // Initialize stepper motor control pins
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  
  Serial.printf("STEP pin: GPIO %d\n", STEP_PIN);
  Serial.printf("DIR pin: GPIO %d\n", DIR_PIN);

  mesh.setDebugMsgTypes( ERROR | STARTUP );
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);
  
  Serial.println("Mesh initialized. Stepper ready for commands...\n");
}

void loop() {
  // Send status periodically so controller knows current motor position
  if (millis() - lastStatusSend > STATUS_INTERVAL) {
    sendStatus();
    lastStatusSend = millis();
  }
  
  // it will run the user scheduler as well
  mesh.update();
}