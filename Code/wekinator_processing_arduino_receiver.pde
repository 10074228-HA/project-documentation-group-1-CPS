/*
  wekinator_processing_arduino_receiver.pde

  Paste into a clean Processing sketch (Java mode).
  Needs the oscP5 library:
    Sketch > Import Library... > Manage Libraries... > search "oscP5" > Install

  Receives on OSC port 9000:
    /hand x y          from Python  (fingertip position, 0..1)
    /wek/outputs class from Wekinator (class number 1..10)

  Shows an 8-bit wheat field with a pixel-art wind puff at the fingertip.
  Sends the class to Arduino as a newline-terminated integer (e.g. "3\n")
  when USE_ARDUINO = true.
*/

import oscP5.*;
import netP5.*;
import processing.serial.*;

// ---------------- Settings you may want to change ----------------
final boolean USE_ARDUINO  = true;   // set to true AFTER the OSC pipeline works
final String  ARDUINO_PORT = "COM8";  // use a name printed by Serial.list()
final int     ARDUINO_BAUD = 9600;
final int     OSC_PORT     = 9000;
final int     HAND_TIMEOUT_MS = 500;  // hide wind if no /hand for this long
// ------------------------------------------------------------------

// 8-bit look: draw on a tiny 120x90 canvas, then scale it up 8x
final int PIX_W = 120;
final int PIX_H = 90;
final int HORIZON = 30;               // row where the sky ends and wheat starts

// Pixel-art wind puff (w = white, g = grey-blue outline, . = empty)
final String[] WIND = {
  "..........gggg..",
  "........ggwwwwg.",
  ".......gwwggggwg",
  "ggggggwwg....gwg",
  "wwwwwwwg..ggwwg.",
  "ggggggg.gwwwwg..",
  "........gwwwwg..",
  "gggggggwwwwg....",
  "wwwwwwwggggg...."
};

OscP5 osc;
Serial arduino;
PGraphics pix;

float handX = 0.5;
float handY = 0.5;
boolean handVisible = false;
int lastHandTime = -100000;

int currentClass = 0;        // 0 = nothing received yet
int lastSentClass = -1;

int handMsgCount = 0;
int wekMsgCount = 0;

void setup() {
  size(960, 720);
  noSmooth();                              // hard pixel edges
  pix = createGraphics(PIX_W, PIX_H);
  pix.noSmooth();
  textFont(createFont("Courier New Bold", 24));

  osc = new OscP5(this, OSC_PORT);
  println("[OSC] Listening on port " + OSC_PORT);
  println("[OSC] Waiting for /hand (Python) and /wek/outputs (Wekinator)...");

  // Always list serial ports so you can find the Arduino's COM port
  println("[SERIAL] Available ports:");
  printArray(Serial.list());

  if (USE_ARDUINO) {
    try {
      arduino = new Serial(this, ARDUINO_PORT, ARDUINO_BAUD);
      println("[SERIAL] Opened " + ARDUINO_PORT + " at " + ARDUINO_BAUD + " baud.");
    }
    catch (Exception e) {
      println("[SERIAL] ERROR: could not open " + ARDUINO_PORT + ". "
        + "Check the port name above and close the Arduino Serial Monitor.");
      arduino = null;
    }
  } else {
    println("[SERIAL] Arduino support is OFF (USE_ARDUINO = false).");
  }
}

void draw() {
  // Hide the wind if Python stopped sending (hand lost or script closed)
  handVisible = (millis() - lastHandTime) < HAND_TIMEOUT_MS;

  pix.beginDraw();
  pix.noStroke();
  drawSky();
  drawWheat();
  if (handVisible) {
    drawWind();
  }
  pix.endDraw();

  image(pix, 0, 0, width, height);   // scale the tiny canvas up
  drawStatusText();
}

void drawSky() {
  pix.fill(135, 206, 235);
  pix.rect(0, 0, PIX_W, 12);
  pix.fill(165, 222, 240);
  pix.rect(0, 12, PIX_W, 10);
  pix.fill(195, 234, 242);
  pix.rect(0, 22, PIX_W, HORIZON - 22);

  // sun
  pix.fill(255, 230, 90);
  pix.rect(101, 6, 6, 8);
  pix.rect(100, 7, 8, 6);

  // clouds
  pix.fill(255);
  pix.rect(15, 8, 12, 3);
  pix.rect(18, 6, 6, 2);
  pix.rect(60, 14, 14, 3);
  pix.rect(63, 12, 7, 2);
}

void drawWheat() {
  // soil showing between the stalks
  pix.fill(170, 125, 45);
  pix.rect(0, HORIZON, PIX_W, PIX_H - HORIZON);

  float t = millis() * 0.003;
  float windX = handX * PIX_W;
  float windY = handY * PIX_H;

  // rows go from far (top) to near (bottom); nearer rows are taller
  for (int y = HORIZON + 3; y < PIX_H + 6; y += 4) {
    int h = 6 + (y - HORIZON) / 6;
    for (int x = ((y / 4) % 2); x < PIX_W; x += 3) {
      int sway = round(sin(t + x * 0.15 + y * 0.1));        // gentle breeze
      if (handVisible && dist(x, y - h, windX, windY) < 22) {
        sway += 2;                                          // blown by the wind puff
      }

      // stem (lower half straight, upper half leans)
      pix.fill(190, 160, 60);
      pix.rect(x, y - h / 2, 1, h / 2);
      pix.rect(x + sway / 2, y - h, 1, h - h / 2);

      // wheat head
      pix.fill(240, 200, 60);
      pix.rect(x + sway, y - h - 3, 2, 4);
      pix.fill(255, 225, 110);
      pix.rect(x + sway, y - h - 3, 1, 1);
    }
  }
}

void drawWind() {
  int left = round(handX * PIX_W) - 8;
  int top  = round(handY * PIX_H) - 4 + ((millis() / 200) % 2);  // little bob
  for (int r = 0; r < WIND.length; r++) {
    for (int c = 0; c < WIND[r].length(); c++) {
      char ch = WIND[r].charAt(c);
      if (ch == 'w') {
        pix.fill(255);
      } else if (ch == 'g') {
        pix.fill(120, 150, 190);
      } else {
        continue;
      }
      pix.rect(left + c, top + r, 1, 1);
    }
  }
}

void label(String s, float x, float y, color c) {
  fill(0);
  text(s, x + 2, y + 2);   // shadow so text is readable on the wheat
  fill(c);
  text(s, x, y);
}

void drawStatusText() {
  textAlign(LEFT, TOP);
  textSize(32);
  label("CLASS: " + (currentClass == 0 ? "-" : str(currentClass)), 20, 20, color(255));

  textSize(16);
  label("/hand msgs: " + handMsgCount
    + "  x=" + nf(handX, 1, 3) + " y=" + nf(handY, 1, 3), 20, 64, color(255));
  label("/wek/outputs msgs: " + wekMsgCount, 20, 86, color(255));
  label("Arduino: " + (USE_ARDUINO ? (arduino != null ? "ON (" + ARDUINO_PORT + ")" : "ERROR") : "OFF"),
    20, 108, color(255));

  if (handMsgCount == 0) {
    label("No /hand received yet - is the Python script running?", 20, height - 30, color(255, 80, 80));
  }
}

void oscEvent(OscMessage msg) {
  if (msg.checkAddrPattern("/hand")) {
    if (msg.arguments().length >= 2) {
      handX = msg.get(0).floatValue();
      handY = msg.get(1).floatValue();
      lastHandTime = millis();
      handMsgCount++;
      if (handMsgCount == 1) {
        println("[OSC] First /hand received from Python.");
      }
    }
  } else if (msg.checkAddrPattern("/wek/outputs")) {
    if (msg.arguments().length >= 1) {
      int cls = round(msg.get(0).floatValue());
      wekMsgCount++;
      if (cls != currentClass) {
        println("[OSC] /wek/outputs received. Class = " + cls);
      }
      currentClass = cls;
      sendClassToArduino(cls);
    }
  }
}

void sendClassToArduino(int cls) {
  if (cls < 1 || cls > 10) {
    return;
  }
  // Only send when the class changes, so the serial port isn't flooded
  if (cls == lastSentClass) {
    return;
  }
  lastSentClass = cls;

  if (USE_ARDUINO && arduino != null) {
    arduino.write(cls + "\n");
    println("[SERIAL] Sent to Arduino: " + cls);
  } else {
    println("[SERIAL] (Arduino off) would send: " + cls);
  }
}
