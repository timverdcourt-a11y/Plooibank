// Input sender for DisplayGUI.
#include <Arduino.h>
#include "painlessMesh.h"

// Mesh network configuration shared with the GUI node.
#define MESH_PREFIX "PIMM"
#define MESH_PASSWORD "Odisee1234"
#define MESH_PORT 5555

// Pin definitions for the rotary encoder.
#define CLK_PIN 18  // Encoder CLK - encoder turning
#define DT_PIN 21   // Encoder DT - encoder direction
#define SW_PIN 26   // Encoder push button

// Button pins used to simulate touch points.
#define BTN1_PIN 25
#define BTN2_PIN 27
#define BTN3_PIN 32
#define BTN4_PIN 33

struct TouchButton {
  int gpio_pin;
  int x;
  int y;
  int last_state;
};

struct RotaryEncoder {
  int last_clk_state;
  int last_sw_state;
  uint32_t last_button_change_ms;
};

static painlessMesh mesh;
static Scheduler userScheduler;

static TouchButton touch_buttons[4] = {
  {BTN1_PIN, 100, 450, HIGH},
  {BTN2_PIN, 300, 450, HIGH},
  {BTN3_PIN, 500, 450, HIGH},
  {BTN4_PIN, 700, 450, HIGH}
};

static RotaryEncoder encoder = {
  HIGH,
  HIGH,
  0
};

static void sendTouch(int x, int y, int pressed)
{
  const String msg = String("touch:") + String(x) + ":" + String(y) + ":" + String(pressed);
  mesh.sendBroadcast(msg);
  Serial.println("Sent: " + msg);
}

static void sendEncoderTurn(bool clockwise)
{
  const String msg = String("encoder:turn:") + (clockwise ? "CW" : "CCW");
  mesh.sendBroadcast(msg);
  Serial.println("Sent: " + msg);
}

static void sendEncoderButtonPressed()
{
  const String msg = "encoder:button:pressed";
  mesh.sendBroadcast(msg);
  Serial.println("Sent: " + msg);
}

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println("Starting DisplayInput sender...");

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
  pinMode(SW_PIN, INPUT_PULLUP);

  for (int i = 0; i < 4; ++i) {
    pinMode(touch_buttons[i].gpio_pin, INPUT_PULLUP);
    touch_buttons[i].last_state = HIGH;
  }

  encoder.last_clk_state = digitalRead(CLK_PIN);
  encoder.last_sw_state = digitalRead(SW_PIN);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);

  Serial.println("Mesh initialized. Waiting for touch and encoder input...");
}

void loop()
{
  for (int i = 0; i < 4; ++i) {
    const int currentState = digitalRead(touch_buttons[i].gpio_pin);

    if (currentState != touch_buttons[i].last_state) {
      if (currentState == LOW) {
        sendTouch(touch_buttons[i].x, touch_buttons[i].y, 1);
        Serial.printf("Button %d pressed at X%d Y%d\n", i + 1, touch_buttons[i].x, touch_buttons[i].y);
      } else {
        sendTouch(touch_buttons[i].x, touch_buttons[i].y, 0);
        Serial.printf("Button %d released at X%d Y%d\n", i + 1, touch_buttons[i].x, touch_buttons[i].y);
      }

      touch_buttons[i].last_state = currentState;
    }
  }

  const int currentClkState = digitalRead(CLK_PIN);
  if (currentClkState != encoder.last_clk_state && currentClkState == HIGH) {
    const int currentDtState = digitalRead(DT_PIN);
    const bool clockwise = (currentDtState != currentClkState);

    sendEncoderTurn(clockwise);
    Serial.printf("Encoder turned %s\n", clockwise ? "CW" : "CCW");
  }

  encoder.last_clk_state = currentClkState;

  const int currentSwState = digitalRead(SW_PIN);
  if (currentSwState != encoder.last_sw_state) {
    const uint32_t now = millis();
    if (currentSwState == LOW && (now - encoder.last_button_change_ms) >= 30) {
      encoder.last_button_change_ms = now;
      sendEncoderButtonPressed();
      Serial.println("Encoder button pressed");
    }

    encoder.last_sw_state = currentSwState;
  }

  mesh.update();
}
