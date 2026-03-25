//Libraries
#include <Arduino.h> //Library to use arduino based code
#include "painlessMesh.h" //Library to use MESH connection between ESPs

//Mesh network configuration
#define   MESH_PREFIX     "PIMM" //Prefix for the MESH network, all devices with the same prefix will connect to each other
#define   MESH_PASSWORD   "Odisee1234" //Password for the MESH network
#define   MESH_PORT       5555 //Port for the MESH network (5555 is default, can be changed if needed)
painlessMesh  mesh; // Create a painlessMesh object


Scheduler userScheduler; // to control your personal task

int lastCLK = HIGH; // Encoder tracking
int lastswpin = HIGH; // Encoder button tracking

int selectedDevice = 1;  // 1 for aanslag1, 2 for aanslag2

//Height variables for each module
int module1Height = 0;  // Height from AanslagModule1 (in 0.1mm units)
int module2Height = 0;  // Height from AanslagModule2 (in 0.1mm units)

//Pin definitions for the rotary encoder
#define   CLK_PIN         32  // Encoder CLK-encoder turning
#define   DT_PIN          33  // Encoder DT-encoder direction
#define   SW_PIN          27  // Encoder push button

//Function prototypes
void sendControl(String command);
void receivedCallback(uint32_t from, String &msg);


//Functions

void sendControl(String command) { // Send control command to selected device
  String msg = "device:" + String(selectedDevice) + ":control:" + command;
  mesh.sendBroadcast( msg );
  Serial.println("Sent: " + msg);
}

void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("Received from %u: %s\n", from, msg.c_str());
  
  // Parse height messages from modules
  // Expected format: "device:1:height:XXX" or "device:2:height:XXX"
  
  if (msg.startsWith("device:1:height:")) {
    // Extract height value for Module1
    String heightStr = msg.substring(16);  // Extract after "device:1:height:"
    int height = heightStr.toInt();
    module1Height = height;
    Serial.printf(">>> Module1 Height updated: %.1f mm\n", module1Height / 10.0);
  }
  else if (msg.startsWith("device:2:height:")) {
    // Extract height value for Module2
    String heightStr = msg.substring(16);  // Extract after "device:2:height:"
    int height = heightStr.toInt();
    module2Height = height;
    Serial.printf(">>> Module2 Height updated: %.1f mm\n", module2Height / 10.0);
  }
}





void setup() {
  Serial.begin(115200); // Start serial communication at 115200 baud rate
  delay(100);
  Serial.println("\n\nStarting ESPcontroller...\n");

  pinMode(CLK_PIN, INPUT); // Set CLK pin as input with pull-up resistor (high when unused)
  pinMode(DT_PIN, INPUT);  // Set DT pin as input with pull-up resistor (high when unused)
  pinMode(SW_PIN, INPUT_PULLUP);  // Set SW pin as input with pull-up resistor (high when unused)

  mesh.setDebugMsgTypes( ERROR | STARTUP ); // Enable debug messages for errors and startup
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT ); // Initialize the mesh network with the defined prefix, password, scheduler, and port
  mesh.onReceive(&receivedCallback); // Register callback to receive height messages from modules
  
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
 // The button is active LOW, so we check for LOW state to detect a press
  int currentSW = digitalRead(SW_PIN);
  if (currentSW == LOW && lastswpin == HIGH) { // Button pressed (transition from HIGH to LOW)
    selectedDevice = (selectedDevice == 1) ? 2 : 1; // Toggle between device 1 and 2
    Serial.printf("\n>>> SWITCHED to AanslagModule%d\n", selectedDevice);
  }
  
  lastswpin = currentSW; // Update last button state for next loop iteration


  // it will run the user scheduler as well
  mesh.update(); // Update the mesh network to process incoming messages and maintain connections
}
