#include "input_router.h"

using namespace divproto;

// Touch is throttled to ~200ms taps like the DIV InputHandler (plan §6.5).
static const uint32_t TOUCH_THROTTLE_MS = 200;

void InputRouter::begin() {
#ifdef HOST_HAVE_A164
  // A164 keyboard bring-up (verify against installed M5Unit-KEYBOARD API):
  //   Wire1.begin(0, 1, 400000);
  //   cfg.mode = Normal; cfg.irq_pin = GPIO_NUM_50; units.add(kb, Wire1); units.begin();
  // Map arrows -> Btn::UP/DOWN/LEFT/RIGHT, Enter -> SELECT, Esc -> BACK,
  // and fill printable ASCII for on-screen text fields.
#endif
}

InputEventOut InputRouter::poll() {
  InputEventOut out{}; out.has = false;

  auto det = M5.Touch.getDetail();
  bool down = det.isPressed();
  uint32_t now = millis();

  if (down && !touching_ && (now - lastTouchMs_) > TOUCH_THROTTLE_MS) {
    touching_ = true; lastTouchMs_ = now;
    out.has = true; out.kind = InputKind::TOUCH;
    out.x = det.x; out.y = det.y; out.pressed = true;
  } else if (!down && touching_) {
    touching_ = false;
  }

#ifdef HOST_HAVE_A164
  // units.update(); translate wasPressed(idx) -> Btn / ASCII here.
#endif
  return out;
}
