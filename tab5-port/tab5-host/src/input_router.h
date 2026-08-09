// input_router.h — unifies Tab5 input into DIV-style nav events. Capacitive
// touch via M5.Touch (fully wired). The detachable A164 keyboard (M5Unit-KEYBOARD
// on Wire1 0/1, INT G50, Normal mode; §6.5) is integrated behind HOST_HAVE_A164
// once its library API is verified against the installed version.
#ifndef INPUT_ROUTER_H
#define INPUT_ROUTER_H

#include <M5Unified.h>
#include "divproto.h"

struct InputEventOut {
  bool     has;          // an event this poll
  divproto::InputKind kind;
  divproto::Btn btn;
  int      x, y;
  bool     pressed;
};

class InputRouter {
public:
  void begin();
  InputEventOut poll();  // call each frame after M5.update()

private:
  bool touching_ = false;
  uint32_t lastTouchMs_ = 0;
};

#endif // INPUT_ROUTER_H
