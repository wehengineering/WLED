#pragma once
#include "wled.h"

// ─────────────────────────────────────────────────────────────────────────────
// Lune Wake Sequence Usermod
//
// A 30-minute circadian ramp from candlelight amber to clean warm white.
// Triggered via JSON API:  POST /json/state  { "lune": { "wake": true } }
// In WOKWI_SIM mode:       Serial Monitor → type "wake" or "cancel"
//
// Exposes: LuneWake::isActive(), LuneWake::cancel() — used by lune_touch.h
// ─────────────────────────────────────────────────────────────────────────────

#ifdef WOKWI_SIM
  #define WAKE_DURATION_MS 30000UL   // 30 seconds compressed
#else
  #define WAKE_DURATION_MS 1800000UL // 30 minutes real
#endif

// Keyframes — time in ms, RGBW values 0–255
struct WakeKeyframe {
  uint32_t timeMs;
  uint8_t  w, r, g, b;
};

static const WakeKeyframe WAKE_SEQUENCE[] = {
  {           0,   2,  4, 0, 0 }, // candlelight ember — barely visible
  {  300000UL,   12, 18, 0, 0 }, // candlelight amber (scaled in sim)
  {  600000UL,   80,  8, 0, 0 }, // soft warm white
  { 1200000UL,  160,  3, 0, 0 }, // bright warm white
  { 1800000UL,  220,  0, 0, 0 }, // full clean warm white
};
static const uint8_t WAKE_KEYFRAME_COUNT = sizeof(WAKE_SEQUENCE) / sizeof(WakeKeyframe);

// Shared state — read by lune_touch.h without needing a class pointer
namespace LuneWake {
  static bool active      = false;
  static bool cancelFlag  = false;

  inline bool isActive() { return active; }
  inline void cancel()   { cancelFlag = true; }
}

class LuneWakeUsermod : public Usermod {
private:
  uint32_t startTimeMs = 0;

  // Interpolate a single uint8_t value between two keyframes
  uint8_t lerpU8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)(a + (b - a) * t);
  }

  // Get interpolated RGBW for elapsed time (in real ms)
  void getColor(uint32_t elapsedMs, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& w) {
    // Find bracketing keyframes
    const WakeKeyframe* k0 = &WAKE_SEQUENCE[0];
    const WakeKeyframe* k1 = &WAKE_SEQUENCE[1];
    for (uint8_t i = 0; i < WAKE_KEYFRAME_COUNT - 1; i++) {
      if (elapsedMs >= WAKE_SEQUENCE[i].timeMs && elapsedMs <= WAKE_SEQUENCE[i+1].timeMs) {
        k0 = &WAKE_SEQUENCE[i];
        k1 = &WAKE_SEQUENCE[i+1];
        break;
      }
    }
    uint32_t span = k1->timeMs - k0->timeMs;
    float alpha = (span == 0) ? 1.0f : (float)(elapsedMs - k0->timeMs) / (float)span;
    alpha = constrain(alpha, 0.0f, 1.0f);
    r = lerpU8(k0->r, k1->r, alpha);
    g = lerpU8(k0->g, k1->g, alpha);
    b = lerpU8(k0->b, k1->b, alpha);
    w = lerpU8(k0->w, k1->w, alpha);
  }

  // Scale real keyframe times for sim (proportional to WAKE_DURATION_MS)
  uint32_t realToElapsed(uint32_t realMs) {
    #ifdef WOKWI_SIM
    return (uint32_t)((float)realMs / 1800000UL * WAKE_DURATION_MS);
    #else
    return realMs;
    #endif
  }

  void applyToSegments(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t bri) {
    for (uint8_t s = 0; s <= 1; s++) {
      Segment& seg = strip.getSegment(s);
      if (!seg.isActive()) continue;
      seg.setColor(0, RGBW32(r, g, b, w));
      seg.opacity = bri;
      seg.on      = true;
    }
    stateChanged = true;
    colorUpdated(CALL_MODE_DIRECT_CHANGE);
  }

  void startWake() {
    startTimeMs     = millis();
    LuneWake::active      = true;
    LuneWake::cancelFlag  = false;
  }

  void holdAndStop() {
    // Hold current LED state, just stop the ramp
    LuneWake::active     = false;
    LuneWake::cancelFlag = false;
  }

public:
  void setup() override {
    #ifdef WOKWI_SIM
    Serial.begin(115200);
    Serial.println(F("[Lune] Wake usermod ready. Type 'wake' or 'cancel'."));
    #endif
  }

  void loop() override {
    // Handle cancel request from touch usermod
    if (LuneWake::cancelFlag) {
      holdAndStop();
      return;
    }

    #ifdef WOKWI_SIM
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd == "wake")   startWake();
      if (cmd == "cancel") holdAndStop();
    }
    #endif

    if (!LuneWake::active) return;

    // millis() rollover safe: unsigned subtraction wraps correctly
    uint32_t elapsed = millis() - startTimeMs;

    if (elapsed >= WAKE_DURATION_MS) {
      // Sequence complete — hold final state
      uint8_t r, g, b, w;
      uint32_t realElapsed = realToElapsed(WAKE_SEQUENCE[WAKE_KEYFRAME_COUNT - 1].timeMs);
      getColor(realElapsed, r, g, b, w);
      applyToSegments(r, g, b, w, 255);
      holdAndStop();
      return;
    }

    // Map compressed elapsed back to real keyframe space
    uint32_t realElapsed;
    #ifdef WOKWI_SIM
    realElapsed = (uint32_t)((float)elapsed / WAKE_DURATION_MS * 1800000UL);
    #else
    realElapsed = elapsed;
    #endif

    uint8_t r, g, b, w;
    getColor(realElapsed, r, g, b, w);

    // Brightness ramps from dim to full over the sequence
    uint8_t bri = (uint8_t)constrain((float)elapsed / WAKE_DURATION_MS * 255, 3, 255);
    applyToSegments(r, g, b, w, bri);
  }

  void addToJsonState(JsonObject& root) override {
    JsonObject lune = root.createNestedObject("lune");
    lune["wake_active"]     = LuneWake::active;
    lune["wake_elapsed_ms"] = LuneWake::active ? (millis() - startTimeMs) : 0;
  }

  void readFromJsonState(JsonObject& root) override {
    JsonObject lune = root["lune"];
    if (lune.isNull()) return;
    if (lune["wake"] == true)   startWake();
    if (lune["cancel"] == true) LuneWake::cancel();
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
