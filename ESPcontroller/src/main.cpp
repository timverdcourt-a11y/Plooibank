#include <Arduino.h>
#include "painlessMesh.h"

#define   MESH_PREFIX     "PIMM"
#define   MESH_PASSWORD   "Odisee1234"
#define   MESH_PORT       5555
#define   CLK_PIN         32  // Encoder CLK-encoder turning
#define   DT_PIN          33  // Encoder DT-encoder direction
#define   SW_PIN          27  // Encoder push button

Scheduler userScheduler; // to control your personal task
painlessMesh  mesh;

bool buttonPressed = false;
int lastCLK = HIGH;

// Device selection and status tracking
int selectedDevice = 1;  // 1 for aanslag1, 2 for aanslag2
int value_aanslag1 = 0;  // For aanslag1: motor step count
int value_aanslag2 = 0;
const int MAX_VALUE = 255;
const int MIN_VALUE = 0;

// User stub
void sendMessage() ; // Prototype so PlatformIO doesn't complain
void sendRotation(String direction) ; // Prototype for rotation messages
void sendControl(String command) ; // Prototype for control messages
void displayStatus() ; // Prototype for display function

void sendMessage() {
  String msg = "Hello from node ";
  msg += mesh.getNodeId();
  mesh.sendBroadcast( msg );
  Serial.println("Message sent!");
}

void sendRotation(String direction) {
  String msg = "rotation:" + direction;
  mesh.sendBroadcast( msg );
  Serial.println("Sent: " + msg);
}

void sendControl(String command) {
  // Send control command to selected device
  String msg = "device:" + String(selectedDevice) + ":control:" + command;
  mesh.sendBroadcast( msg );
  Serial.println("Sent: " + msg);
}

void displayStatus() {
  // Display current device and its value
  Serial.printf("\n=== STATUS ===\n");
  Serial.printf("Selected Device: ESPaanslag%d\n", selectedDevice);
  if (selectedDevice == 1) {
    Serial.printf("Motor Steps: %d\n", value_aanslag1);
  } else {
    Serial.printf("Value: %d/%d\n", value_aanslag2, MAX_VALUE);
  }
  Serial.printf("==============\n\n");
}

// Needed for painless library
void receivedCallback( uint32_t from, String &msg ) {
  Serial.printf("[MSG] From %u: %s\n", from, msg.c_str());
  
  // Parse status messages from aanslag devices
  // Format: "status:device:1:steps:150" (for stepper motor) or "status:device:2:value:75" (for other devices)
  if (msg.startsWith("status:device:")) {
    int deviceNum = msg.charAt(14) - '0';  // Extract device number
    
    // Check for "steps:" format (aanslag1 with stepper motor)
    int stepsPos = msg.indexOf("steps:");
    if (stepsPos > 0) {
      String stepsStr = msg.substring(stepsPos + 6);
      int steps = stepsStr.toInt();
      
      Serial.printf("  ✓ Parsed: Device %d, Steps = %d\n", deviceNum, steps);
      
      if (deviceNum == 1) {
        value_aanslag1 = steps;
      }
      displayStatus();
      return;
    }
    
    // Check for "value:" format (other devices)
    int colonPos = msg.indexOf("value:");
    if (colonPos > 0) {
      String valueStr = msg.substring(colonPos + 6);
      int value = valueStr.toInt();
      
      Serial.printf("  ✓ Parsed: Device %d, Value = %d\n", deviceNum, value);
      
      if (deviceNum == 1) {
        value_aanslag1 = value;
      } else if (deviceNum == 2) {
        value_aanslag2 = value;
      }
      
      displayStatus();
    } else {
      Serial.println("  ✗ Error: 'steps:' or 'value:' not found in message");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\nStarting ESPcontroller...\n");
  
  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN, INPUT);
  pinMode(SW_PIN, INPUT_PULLUP);

  mesh.setDebugMsgTypes( ERROR | STARTUP );
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);
  
  Serial.println("Mesh initialized. Waiting for devices...\n");
}

void loop() {
  // Check encoder rotation
  int currentCLK = digitalRead(CLK_PIN);
  
  if (currentCLK == LOW && lastCLK == HIGH) {
    if (digitalRead(DT_PIN) == HIGH) {
      Serial.println(">>> Rotating CLOCKWISE - sending increase");
      sendControl("increase");
    } else {
      Serial.println(">>> Rotating COUNTER-CLOCKWISE - sending decrease");
      sendControl("decrease");
    }
  }
  
  lastCLK = currentCLK;
  
  // Check if encoder button is pressed (switch device)
  if (digitalRead(SW_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      delay(20);  // Debounce
      // Switch device
      selectedDevice = (selectedDevice == 1) ? 2 : 1;
      Serial.printf("\n>>> SWITCHED to ESPaanslag%d\n", selectedDevice);
      displayStatus();
    }
  } else {
    buttonPressed = false;
  }
  
  // it will run the user scheduler as well
  mesh.update();
  delay(5);
}
