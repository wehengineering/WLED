#pragma once
#include "wled.h"
#include "../lune_wake/lune_wake.h"

// ─────────────────────────────────────────────────────────────────────────────
// Lune Touch Usermod
//
// Capacitive touch on three hidden pads. Tap toggles segments, long press
// cycles brightness. Any touch cancels a running wake sequence.
//
// Hardware:  touchRead() — ESP32 capacitive sensing
// Sim mode:  digitalRead() with INPUT_PULLUP buttons via WOKWI_SIM shim
// ─────────────────────────────────────────────────────────────────────────────

#define TOUCH_PIN_LEFT     4
#define TOUCH_PIN_CENTRE  13
#define TOUCH_PIN_RIGHT   14
#define LUNE_TOUCH_THRESHOLD   40  // below this = touched (calibrate per pad)
#define TOUCH_SAMPLES      5       // rolling average sample count
#define LONG_PRESS_MS    700UL
#define BRIGHTNESS_STEP_MS 600UL

static const uint8_t BRIGHTNESS_LEVELS[] = {25, 64, 128, 192, 255};
#define BRIGHTNESS_LEVEL_COUNT 5

// ── Touch read shim ──────────────────────────────────────────────────────────
#ifdef WOKWI_SIM
  // Buttons wired GPIO → button → GND, INPUT_PULLUP set in setup()
  // LOW = pressed = "touched" → returns 0 (below threshold)
  // HIGH = released           → returns 80 (above threshold)
  inline uint32_t readPad(uint8_t pin) {
    return (digitalRead(pin) == LOW) ? 0 : 80;
  }
#else
  inline uint32_t readPad(uint8_t pin) {
    return touchRead(pin);
  }
#endif

class LuneTouchUsermod : public Usermod {
private:
  enum PadState { IDLE, PRESSED, LONG_PRESS };

  struct Pad {
    uint8_t  pin;
    PadState state      = IDLE;
    uint32_t pressStart = 0;
    uint32_t lastStep   = 0;
    // Rolling average buffer
    uint32_t samples[TOUCH_SAMPLES];
    uint8_t  sampleIdx  = 0;
    bool     initialised = false;
  } pads[3];

  // Per-segment brightness index (independent for L and R)
  uint8_t segBriIdx[2] = {2, 2}; // index 2 = 128 = 50%

  // ── Helpers ─────────────────────────────────────────────────────────────────

  bool isTouched(Pad& p) {
    if (!p.initialised) {
      for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) p.samples[i] = 80;
      p.initialised = true;
    }
    p.samples[p.sampleIdx % TOUCH_SAMPLES] = readPad(p.pin);
    p.sampleIdx++;
    uint32_t avg = 0;
    for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) avg += p.samples[i];
    avg /= TOUCH_SAMPLES;
    return avg < LUNE_TOUCH_THRESHOLD;
  }

  void toggleSegment(uint8_t segId) {
    Segment& seg = strip.getSegment(segId);
    if (!seg.isActive()) return;
    if (seg.on) {
      seg.on = false;
    } else {
      seg.on      = true;
      seg.opacity = BRIGHTNESS_LEVELS[segBriIdx[segId]];
    }
    stateChanged = true;
    colorUpdated(CALL_MODE_DIRECT_CHANGE);
  }

  void stepBrightness(uint8_t segId) {
    segBriIdx[segId] = (segBriIdx[segId] + 1) % BRIGHTNESS_LEVEL_COUNT;
    Segment& seg = strip.getSegment(segId);
    if (!seg.isActive()) return;
    seg.opacity = BRIGHTNESS_LEVELS[segBriIdx[segId]];
    seg.on      = true;
    stateChanged = true;
    colorUpdated(CALL_MODE_DIRECT_CHANGE);
  }

  // ── Gesture handlers ─────────────────────────────────────────────────────────

  void onTap(uint8_t padIdx) {
    if (LuneWake::isActive()) { LuneWake::cancel(); return; } // first tap = cancel wake only
    switch (padIdx) {
      case 0: toggleSegment(0);                          break; // Left  → seg 0
      case 1: {                                                    // Centre → off if either on, else both on
        bool eitherOn = strip.getSegment(0).on || strip.getSegment(1).on;
        if (eitherOn) {
          strip.getSegment(0).on = false;
          strip.getSegment(1).on = false;
          stateChanged = true;
          colorUpdated(CALL_MODE_DIRECT_CHANGE);
        } else {
          toggleSegment(0); toggleSegment(1);
        }
        break;
      }
      case 2: toggleSegment(1);                          break; // Right → seg 1
    }
  }

  void onLongPressStep(uint8_t padIdx, uint32_t now) {
    if (LuneWake::isActive()) LuneWake::cancel();
    Pad& p = pads[padIdx];
    if (now - p.lastStep < BRIGHTNESS_STEP_MS) return;
    p.lastStep = now;
    switch (padIdx) {
      case 0: stepBrightness(0);                     break;
      case 1: stepBrightness(0); stepBrightness(1);  break;
      case 2: stepBrightness(1);                     break;
    }
  }

public:
  void setup() override {
    #ifdef WOKWI_SIM
    pinMode(TOUCH_PIN_LEFT,   INPUT_PULLUP);
    pinMode(TOUCH_PIN_CENTRE, INPUT_PULLUP);
    pinMode(TOUCH_PIN_RIGHT,  INPUT_PULLUP);
    Serial.println(F("[Lune] Touch usermod ready (Wokwi sim mode)."));
    #endif
    pads[0].pin = TOUCH_PIN_LEFT;
    pads[1].pin = TOUCH_PIN_CENTRE;
    pads[2].pin = TOUCH_PIN_RIGHT;
  }

  void loop() override {
    uint32_t now = millis();

    for (uint8_t i = 0; i < 3; i++) {
      Pad& p     = pads[i];
      bool touch = isTouched(p);

      switch (p.state) {
        case IDLE:
          if (touch) {
            p.state      = PRESSED;
            p.pressStart = now;
          }
          break;

        case PRESSED:
          if (!touch) {
            // Released before long-press threshold → tap
            onTap(i);
            p.state = IDLE;
          } else if (now - p.pressStart >= LONG_PRESS_MS) {
            // Held past threshold → enter long press
            p.state    = LONG_PRESS;
            p.lastStep = now;
          }
          break;

        case LONG_PRESS:
          if (!touch) {
            // Released — hold current brightness, no tap event
            p.state = IDLE;
          } else {
            onLongPressStep(i, now);
          }
          break;
      }
    }
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
