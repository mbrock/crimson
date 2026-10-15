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

// Once the pad navigates, the focus marker and the focused button's highlight
// stay up instead of fading a second after each move, and the sticks stop
// steering the menus' analog cursor (changes/pad-menu-cursor.patch), as in the
// Python port's focus.py; moving the mouse returns to the original's.
extern "C" unsigned char game_pad_navigating;
unsigned char game_pad_navigating;

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

// The original's menus take no pad, only the mouse and keyboard focus: Tab and
// Shift+Tab move it, Enter activates (ui_focus_update, ui_button_update), the
// arrows page quest tiers and step sliders, PageUp/PageDown scroll, and Escape
// backs out. Outside a run the pad presses those keys as Python's focus.py
// does: the D-pad or left stick moves, A accepts, B and Start go back, LB/RB
// page. A, B, Start and the stick tap once rather than hold, so a held button
// never auto-repeats into the next screen; the stick repeats on its own clock,
// as Python's does. In a run only Start presses Escape, to pause. Nothing is
// pressed while the Controls menu waits for a binding, which would otherwise
// capture the key the pad pressed instead of the pad's own button.
extern "C" void game_pad_menu() {
  enum { up, down, left, right, accept, back, page_up, page_down, count };
  static const unsigned char codes[count] = {0x0f, 0x0f, 0xcb, 0xcd, 0x1c, 0x01, 0xc9, 0xd1};
  static bool held[count], was[count];
  static int stick, stick_repeat_ms, last_state = -1;
  HostInput &in = *game_input();
  bool run = game_state_id == GAME_STATE_GAMEPLAY || game_state_id == GAME_STATE_TYPO_GAMEPLAY ||
             game_state_id == GAME_STATE_DEMO_UPSELL_GAMEPLAY;
  bool locked = controls_rebind_slot_index != -1;
  bool now[count] = {};
  unsigned hat = in.pad_hat;
  if (hat != ~0u) {
    now[up] = hat > 31500 || hat < 4500;
    now[right] = hat > 4500 && hat < 13500;
    now[down] = hat > 13500 && hat < 22500;
    now[left] = hat > 22500 && hat < 31500;
  }
  now[accept] = in.pad_buttons[1];
  now[back] = in.pad_buttons[2] || in.pad_buttons[9];
  now[page_up] = in.pad_buttons[4];
  now[page_down] = in.pad_buttons[5];
  // The left stick as a D-pad past half travel: a press, then repeats after
  // 400 ms every 120 ms. In a run it moves the player, and only its direction
  // is tracked, so a push held over from the run steps no sooner than a repeat.
  int x = in.pad_axes[0], y = in.pad_axes[1], direction = -1;
  if ((x > 500 || x < -500 || y > 500 || y < -500))
    direction = (y < 0 ? -y : y) >= (x < 0 ? -x : x) ? (y > 0 ? down : up) : (x > 0 ? right : left);
  int stick_press = -1;
  if (run || locked || direction != stick) {
    if (!run && !locked && direction != stick)
      stick_press = direction;
    stick = direction;
    stick_repeat_ms = 400;
  } else if (direction >= 0 && (stick_repeat_ms -= frame_dt_ms) <= 0) {
    stick_press = direction;
    stick_repeat_ms = 120;
  }
  bool want[count] = {};
  if (!locked) {
    for (int i = 0; i < count; ++i)
      want[i] = !run && now[i];
    if (run)
      want[back] = in.pad_buttons[9];
    // Taps: down on the press, up the next frame.
    want[accept] = want[accept] && !was[accept];
    want[back] = want[back] && !was[back];
  }
  bool navigated = false;
  for (int i = 0; i < count; ++i)
    navigated |= !run && !locked && now[i] && !was[i];
  for (int i = 0; i < count; ++i)
    was[i] = i == back && run ? in.pad_buttons[9] : now[i];
  auto press = [&](unsigned char code, bool down) {
    in.keys[code] = down ? 0x80 : 0;
    if (in.key_event_count < 32)
      in.key_events[in.key_event_count++] = {code, (unsigned char)down};
  };
  // A stick step taps its key, released next frame, unless the D-pad holds it.
  static bool stick_tapped[count];
  for (int i = up; i <= right; ++i) {
    if (stick_tapped[i] && !want[i] && held[i]) {
      press(codes[i], false);
      if (i == up)
        press(0x2a, false);
      held[i] = false;
    }
    stick_tapped[i] = false;
  }
  if (stick_press >= 0 && !held[stick_press]) {
    if (stick_press == up)
      press(0x2a, true);
    press(codes[stick_press], true);
    held[stick_press] = stick_tapped[stick_press] = true;
    navigated = true;
  }
  for (int i = 0; i < count; ++i) {
    if (want[i] == held[i] || stick_tapped[i])
      continue;
    held[i] = want[i];
    if (i == up && want[i])
      press(0x2a, true); // Shift+Tab
    press(codes[i], want[i]);
    if (i == up && !want[i])
      press(0x2a, false);
    // WM_CHAR delivers Enter too, which name entry reads.
    if (i == accept && want[i])
      game_key_char(13);
  }

  if (navigated)
    game_pad_navigating = 1;
  else if (in.mouse_dx || in.mouse_dy)
    game_pad_navigating = 0;
  // A newly shown screen starts on its first widget, not on the previous
  // screen's focus index (Python's UiFocus.screen_changed).
  if (game_state_id != last_state) {
    last_state = game_state_id;
    ui_focus_index = 0;
  }
  // ui_focus_update takes a frame's time off before it draws.
  if (game_pad_navigating && !run)
    ui_focus_timer_ms = 1000 + frame_dt_ms;
}
