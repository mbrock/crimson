// Modern controller play for the original, after the Python port's
// (src/crimson/gamepad_profile.py, src/crimson/ui/focus.py). The host delivers
// the pad as the Logitech Dual Action the original's pad schemes are named for
// (client/main.cpp pad): buttons 0x11F + n are west, south, east, north, LB, RB,
// LT, RT, back, start, and lX/lY (0x13F/0x140) and lZ/lRz (0x141/0x155) the left
// and right sticks.
#include "crimsonland_gameplay.h"
#include "host_input.h"
#include <string.h>

extern "C" {
HostInput *game_input();
void game_key_char(int character);
bool config_sync_from_grim(void);
extern int controls_rebind_slot_index;
extern int ui_focus_index, ui_focus_timer_ms, frame_dt_ms;
}

namespace {

// The stock crimson.cfg bindings are the DirectInput-era defaults
// (config_sync_from_grim): aim on the left stick with X and Y swapped, and
// movement on the right stick's Z plus lRx (0x153), which the Dual Action layout
// never fills, so a modern pad moves on one axis of the wrong stick.
const player_input_config_t stock = {17, 31, 30, 32, 0x100, 382, 382, 16, 18, 0x13f, 0x140, 0x141, 0x153, {382, 382, 382}};
constexpr int stock_move_scheme = 2, stock_aim_scheme = 0, pad_move_scheme = 3, pad_aim_scheme = 4;
constexpr int stock_reload = 0x102, stock_pick_perk = 0x101;
constexpr int pad_fire = 0x126, pad_reload = 0x123, pad_pick_perk = 0x122; // RT, LB, Y

bool pad_used(const HostInput &in) {
  for (int axis : in.pad_axes)
    if (axis > 500 || axis < -500)
      return true;
  for (unsigned char button : in.pad_buttons)
    if (button)
      return true;
  return in.pad_hat != ~0u;
}

} // namespace

// The first time the pad is used, player 1's stock bindings move to the twin-stick
// profile: left stick moves, right stick aims, RT fires, LB reloads, Y picks a
// perk. A fully stock player also switches both methods to Dual Action Pad, so a
// mouse player who rebound anything keeps the mouse; bindings the player changed
// are never touched. Each upgrade replaces a stock value, so after one pass
// nothing is pending. Python applies the axis upgrades once a pad connects; the
// module only learns of the pad when it is used, so both wait for that here.
extern "C" void game_pad_profile() {
  static bool done;
  if (done || !pad_used(*game_input()))
    return;
  done = true;
  player_input_config_t &keys = config_blob.input_config[0];
  bool changed = false;
  if (config_blob.movement_schemes[0] == stock_move_scheme && config_blob.aim_schemes[0] == stock_aim_scheme &&
      !memcmp(&keys, &stock, sizeof stock)) {
    config_blob.movement_schemes[0] = pad_move_scheme;
    config_blob.aim_schemes[0] = pad_aim_scheme;
    changed = true;
  }
  if (keys.axis_move_y == stock.axis_move_y && keys.axis_move_x == stock.axis_move_x) {
    keys.axis_move_y = 0x140;
    keys.axis_move_x = 0x13f;
    changed = true;
  }
  if (keys.axis_aim_y == stock.axis_aim_y && keys.axis_aim_x == stock.axis_aim_x) {
    keys.axis_aim_y = 0x155;
    keys.axis_aim_x = 0x141;
    changed = true;
  }
  bool aim_pad = config_blob.aim_schemes[0] == pad_aim_scheme;
  bool any_pad = aim_pad || config_blob.movement_schemes[0] == pad_move_scheme;
  if (aim_pad && keys.fire_key == stock.fire_key) {
    keys.fire_key = pad_fire;
    changed = true;
  }
  // Reload and Level Up are global codes owned by player 1.
  if (any_pad && config_blob.key_reload == stock_reload) {
    config_blob.key_reload = pad_reload;
    changed = true;
  }
  if (any_pad && config_blob.key_pick_perk == stock_pick_perk) {
    config_blob.key_pick_perk = pad_pick_perk;
    changed = true;
  }
  if (!changed)
    return;
  // crimsonland_main copies the bindings into the player at startup.
  player_input_t &input = player_state_table[0].input;
  input.fire_key = keys.fire_key;
  input.axis_aim_y = keys.axis_aim_y;
  input.axis_aim_x = keys.axis_aim_x;
  input.axis_move_y = keys.axis_move_y;
  input.axis_move_x = keys.axis_move_x;
  config_sync_from_grim();
}

// The original's menus are made for the mouse, with a keyboard focus (Tab,
// Enter) that reaches only some of them: the quest list, for one, takes only a
// click or a number key, so Enter on its one focusable widget, Back, backed out.
// The pad therefore drives the cursor. The D-pad or left stick moves it to the
// nearest place a click acts on in that direction (the anchors the widgets name,
// changes/pad-nav-anchors.patch) and keeps it there as the menu moves; the right
// stick steers it freely (changes/pad-menu-cursor.patch); A clicks where it is.
// A focused slider takes left and right, and left and right with nothing that way
// press the arrows, which page the quest stages. B and Start back out (Escape),
// LB/RB page (PageUp/PageDown). The directions press once, then repeat after
// 400 ms every 120 ms, as Python's focus.py does. In a run only Start presses
// Escape, to pause. While the Controls menu waits for a binding only Escape is
// pressed, to cancel; the capture never takes it, but would take any other key
// the pad pressed instead of the pad's own button.
extern "C" {
extern float ui_mouse_x, ui_mouse_y;
extern int ui_analog_cursor_active, ui_focus_count;
// The window cursor moved this frame (client/main.cpp), which hands the cursor
// back to the mouse (changes/pad-menu-cursor.patch).
extern unsigned char game_pad_mouse_moved;
unsigned char game_pad_mouse_moved;
}

namespace {

struct Anchor {
  float x, y;
  int kind, slot; // kind 0 a focusable widget, 1 a slider, 2 a click target without focus
};
Anchor anchors[2][96];
int anchor_count[2], current; // anchors[current] collects this frame's; the other holds the last frame's

} // namespace

extern "C" void game_nav_anchor(float x, float y, int kind) {
  if (anchor_count[current] < 96)
    anchors[current][anchor_count[current]++] = {x, y, kind, kind < 2 ? ui_focus_count - 1 : -1};
}

extern "C" void game_pad_menu() {
  enum { up, down, left, right };
  static const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
  static int direction_held = -1, repeat_ms, glued = -1, glued_count, last_state = -1;
  static bool was_accept, was_back, clicking, paging[2];
  HostInput &in = *game_input();
  current ^= 1;
  anchor_count[current] = 0;
  const Anchor *nav = anchors[current ^ 1];
  int nav_count = anchor_count[current ^ 1];
  bool run = game_state_id == GAME_STATE_GAMEPLAY || game_state_id == GAME_STATE_TYPO_GAMEPLAY ||
             game_state_id == GAME_STATE_DEMO_UPSELL_GAMEPLAY;
  bool locked = controls_rebind_slot_index != -1;
  bool menu = !run && !locked;
  game_pad_mouse_moved = in.mouse_moved;
  auto press = [&](unsigned char code, bool down) {
    in.keys[code] = down ? 0x80 : 0;
    if (in.key_event_count < 32)
      in.key_events[in.key_event_count++] = {code, (unsigned char)down};
  };
  // Release last frame's taps.
  static unsigned char tapped[4];
  for (unsigned char &code : tapped)
    if (code) {
      press(code, false);
      code = 0;
    }
  auto tap_once = [&](unsigned char code) {
    press(code, true);
    for (unsigned char &slot : tapped)
      if (!slot) {
        slot = code;
        break;
      }
  };
  if (clicking) {
    in.mouse_buttons[0] = 0;
    clicking = false;
  }

  // The held direction: the D-pad, else the left stick past half travel.
  int direction = -1;
  unsigned hat = in.pad_hat;
  if (hat != ~0u)
    direction = hat > 31500 || hat < 4500 ? up : hat < 13500 ? right : hat < 22500 ? down : left;
  else {
    int x = in.pad_axes[0], y = in.pad_axes[1];
    if (x > 500 || x < -500 || y > 500 || y < -500)
      direction = (y < 0 ? -y : y) >= (x < 0 ? -x : x) ? (y > 0 ? down : up) : (x > 0 ? right : left);
  }
  int step = -1;
  if (!menu || direction != direction_held) {
    // In a run only the direction is tracked, so a push held over from it steps no sooner than a repeat.
    if (menu)
      step = direction;
    direction_held = direction;
    repeat_ms = 400;
  } else if (direction >= 0 && (repeat_ms -= frame_dt_ms) <= 0) {
    step = direction;
    repeat_ms = 120;
  }

  // The cursor leaves its anchor once the right stick or the mouse moves it, or
  // the screen changes. When the anchors change on the same screen (a dropdown
  // opens), it keeps to the one nearest where it was.
  static bool pad_mode;
  static int stable_frames, last_count;
  static float glued_x, glued_y;
  bool pointing = menu && in.pad_axes[2] * in.pad_axes[2] + in.pad_axes[3] * in.pad_axes[3] > 200 * 200;
  if (pointing || in.mouse_moved)
    pad_mode = false;
  if (pointing || in.mouse_moved || !menu || game_state_id != last_state)
    glued = -1;
  if (glued >= 0 && nav_count != glued_count) {
    float nearest = 40.0f * 40.0f;
    glued = -1;
    for (int i = 0; i < nav_count; ++i) {
      float ox = nav[i].x - glued_x, oy = nav[i].y - glued_y;
      if (ox * ox + oy * oy < nearest) {
        nearest = ox * ox + oy * oy;
        glued = i;
      }
    }
    glued_count = nav_count;
  }
  last_state = game_state_id;
  stable_frames = nav_count == last_count ? stable_frames + 1 : 0;
  last_count = nav_count;
  // A screen the pad came to starts on its topmost place, once its anchors settle.
  if (menu && pad_mode && glued < 0 && step < 0 && nav_count > 0 && stable_frames >= 10) {
    int top = 0;
    for (int i = 1; i < nav_count; ++i)
      if (nav[i].y < nav[top].y - 2.0f || (nav[i].y < nav[top].y + 2.0f && nav[i].x < nav[top].x))
        top = i;
    glued = top;
    glued_count = nav_count;
  }

  if (step >= 0) {
    const Anchor *at = glued >= 0 ? &nav[glued] : nullptr;
    if (at && at->kind == 1 && (step == left || step == right)) {
      tap_once(step == left ? 0xcb : 0xcd);
    } else {
      // From the anchor the cursor holds, else from the cursor: the nearest
      // anchor that way, weighing sideways distance half, so a row's widget is
      // reached however far it sits from the one above. Left and right look only
      // within 56 degrees, and with nothing there press the arrows instead.
      float from_x = at ? at->x : ui_mouse_x, from_y = at ? at->y : ui_mouse_y;
      bool vertical = step == up || step == down;
      int target = -1;
      float best = 1e30f;
      for (int i = 0; i < nav_count; ++i) {
        float ox = nav[i].x - from_x, oy = nav[i].y - from_y;
        float along = ox * dx[step] + oy * dy[step], across = ox * dy[step] - oy * dx[step];
        if (across < 0)
          across = -across;
        if (along <= 4.0f || (!vertical && across > along * 1.5f))
          continue;
        float score = along + across * 0.5f;
        if (score < best) {
          best = score;
          target = i;
        }
      }
      if (target >= 0) {
        glued = target;
        glued_count = nav_count;
      } else if (step == left || step == right) {
        tap_once(step == left ? 0xcb : 0xcd);
      }
    }
  }
  if (step >= 0)
    pad_mode = true;
  if (glued >= 0) {
    glued_x = nav[glued].x;
    glued_y = nav[glued].y;
    // Hold the cursor on its anchor; the mouse's motion no longer reaches it until the mouse moves.
    ui_mouse_x = nav[glued].x;
    ui_mouse_y = nav[glued].y;
    ui_analog_cursor_active = 1;
    if (nav[glued].slot >= 0) {
      ui_focus_index = nav[glued].slot;
      ui_focus_timer_ms = 1000 + frame_dt_ms; // ui_focus_update takes a frame's time off before it draws
    }
  }

  bool accept = in.pad_buttons[1], back = in.pad_buttons[2] || in.pad_buttons[9], start = in.pad_buttons[9];
  if (menu && accept && !was_accept) {
    pad_mode = true;
    in.mouse_buttons[0] = 0x80;
    clicking = true;
  }
  if ((run ? start : back) && !was_back) // Escape also cancels a rebind, which never captures it
    tap_once(0x01);
  was_accept = accept;
  was_back = run ? start : back;
  for (int i = 0; i < 2; ++i) {
    bool want = menu && in.pad_buttons[4 + i];
    if (want != paging[i]) {
      press(i ? 0xd1 : 0xc9, want);
      paging[i] = want;
    }
  }
}
