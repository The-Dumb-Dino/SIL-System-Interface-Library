/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/input.c: Input device handling for the SDL3 platform
 * layer (keyboard, mouse, text input and gamepads).
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/input.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/sdl/internal.h"
#include "src/time.h"

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

uint8_t sdl_quit_requested;

/* For the Cocoa misc/dialog code. */
#ifdef SIL_PLATFORM_MACOSX
uint8_t macosx_quit_requested;
#endif

/* Event callback passed to sys_input_init(). */
static void (*event_callback)(const InputEvent *);

/* Joystick (gamepad) state.  SIL joystick indices are slots in this array;
 * a slot keeps its index while the device is disconnected. */
#define MAX_JOYSTICKS  8
#define NUM_STICKS     2
static SysInputJoystick joystick_info[MAX_JOYSTICKS];
static SDL_Gamepad *gamepads[MAX_JOYSTICKS];
static SDL_JoystickID gamepad_ids[MAX_JOYSTICKS];
static int num_joysticks;
static uint8_t unfocused_joystick = 1;
static uint8_t window_focused = 1;
static float stick_state[MAX_JOYSTICKS][NUM_STICKS][2];
static int dpad_state[MAX_JOYSTICKS][2];

/* Text input state. */
static uint8_t text_active;

/* Mouse grab state. */
static uint8_t grab_requested;

/*************************************************************************/
/************************** Local declarations ***************************/
/*************************************************************************/

static int convert_scancode(SDL_Scancode scancode);
static void handle_event(const SDL_Event *event, double timestamp);
static void send_mouse_event(InputEventDetail detail, float x, float y,
                             float scroll, double timestamp);

/*************************************************************************/
/************************** Interface routines ***************************/
/*************************************************************************/

int sys_input_init(void (*event_callback_)(const InputEvent *))
{
    event_callback = event_callback_;
    num_joysticks = 0;
    mem_clear(joystick_info, sizeof(joystick_info));
    mem_clear(gamepads, sizeof(gamepads));
    sdl_quit_requested = 0;
    text_active = 0;
    return 1;
}

/*-----------------------------------------------------------------------*/

void sys_input_cleanup(void)
{
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (gamepads[i]) {
            SDL_CloseGamepad(gamepads[i]);
            gamepads[i] = NULL;
        }
    }
    num_joysticks = 0;
    event_callback = NULL;
}

/*-----------------------------------------------------------------------*/

static void do_pump_events(UNUSED void *userdata)
{
    const double now = time_now();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        handle_event(&event, now);
    }
}

void sys_input_update(void)
{
    sdl_run_on_main_thread(do_pump_events, NULL);
}

/*-----------------------------------------------------------------------*/

void sys_input_info(SysInputInfo *info_ret)
{
    info_ret->has_joystick = 1;
    info_ret->num_joysticks = num_joysticks;
    info_ret->joysticks = joystick_info;
    info_ret->has_keyboard = 1;
    info_ret->keyboard_is_full = 1;
    info_ret->has_mouse = 1;
    info_ret->has_text = 1;
    info_ret->text_uses_custom_interface = 0;
    info_ret->text_has_prompt = 0;
    info_ret->has_touch = 0;
}

/*-----------------------------------------------------------------------*/

static void do_update_grab(UNUSED void *userdata)
{
    if (sdl_window) {
        SDL_SetWindowMouseGrab(sdl_window, grab_requested && window_focused);
    }
}

void sys_input_grab(int grab)
{
    grab_requested = (grab != 0);
    sdl_run_on_main_thread(do_update_grab, NULL);
}

/*-----------------------------------------------------------------------*/

int sys_input_is_quit_requested(void)
{
    return sdl_quit_requested
#ifdef SIL_PLATFORM_MACOSX
        || macosx_quit_requested
#endif
        ;
}

/*-----------------------------------------------------------------------*/

int sys_input_is_suspend_requested(void)
{
    return 0;
}

void sys_input_acknowledge_suspend_request(void)
{
    /* Nothing to do. */
}

/*-----------------------------------------------------------------------*/

void sys_input_enable_unfocused_joystick(int enable)
{
    unfocused_joystick = (enable != 0);
}

/*-----------------------------------------------------------------------*/

char *sys_input_joystick_copy_name(int index)
{
    if (index < 0 || index >= num_joysticks || !gamepads[index]) {
        return NULL;
    }
    const char *name = SDL_GetGamepadName(gamepads[index]);
    return name ? mem_strdup(name, 0) : NULL;
}

/*-----------------------------------------------------------------------*/

int sys_input_joystick_button_mapping(UNUSED int index, int name)
{
    /* We report buttons using SDL's standard gamepad button numbering, so
     * the mapping is fixed. */
    switch (name) {
      case INPUT_JOYBUTTON_HOME:       return SDL_GAMEPAD_BUTTON_GUIDE;
      case INPUT_JOYBUTTON_START:      return SDL_GAMEPAD_BUTTON_START;
      case INPUT_JOYBUTTON_SELECT:     return SDL_GAMEPAD_BUTTON_BACK;
      case INPUT_JOYBUTTON_FACE_UP:    return SDL_GAMEPAD_BUTTON_NORTH;
      case INPUT_JOYBUTTON_FACE_LEFT:  return SDL_GAMEPAD_BUTTON_WEST;
      case INPUT_JOYBUTTON_FACE_RIGHT: return SDL_GAMEPAD_BUTTON_EAST;
      case INPUT_JOYBUTTON_FACE_DOWN:  return SDL_GAMEPAD_BUTTON_SOUTH;
      case INPUT_JOYBUTTON_L1:         return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
      case INPUT_JOYBUTTON_R1:         return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
      /* Triggers are reported as extra buttons after SDL's buttons. */
      case INPUT_JOYBUTTON_L2:         return SDL_GAMEPAD_BUTTON_COUNT + 0;
      case INPUT_JOYBUTTON_R2:         return SDL_GAMEPAD_BUTTON_COUNT + 1;
      case INPUT_JOYBUTTON_L_STICK:    return SDL_GAMEPAD_BUTTON_LEFT_STICK;
      case INPUT_JOYBUTTON_R_STICK:    return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
      case INPUT_JOYBUTTON__NUM:       break;
    }
    return -1;
}

/*-----------------------------------------------------------------------*/

void sys_input_joystick_rumble(int index, float left, float right, float time)
{
    if (index >= 0 && index < num_joysticks && gamepads[index]) {
        SDL_RumbleGamepad(gamepads[index], (Uint16)(left * 65535),
                          (Uint16)(right * 65535), (Uint32)(time * 1000));
    }
}

/*-----------------------------------------------------------------------*/

typedef struct { float x, y; } WarpParams;
static void do_warp_mouse(void *userdata)
{
    const WarpParams *params = userdata;
    if (sdl_window) {
        int w, h;
        SDL_GetWindowSize(sdl_window, &w, &h);
        SDL_WarpMouseInWindow(sdl_window, params->x * w, params->y * h);
    }
}

void sys_input_mouse_set_position(float x, float y)
{
    WarpParams params = {x, y};
    sdl_run_on_main_thread(do_warp_mouse, &params);
}

/*-----------------------------------------------------------------------*/

static void do_set_text_input(UNUSED void *userdata)
{
    if (!sdl_window) {
        return;
    }
    if (text_active) {
        SDL_StartTextInput(sdl_window);
    } else {
        SDL_StopTextInput(sdl_window);
    }
}

void sys_input_text_set_state(int on, UNUSED const char *text,
                              UNUSED const char *prompt)
{
    text_active = (on != 0);
    sdl_run_on_main_thread(do_set_text_input, NULL);
}

/*************************************************************************/
/**************************** Local routines *****************************/
/*************************************************************************/

static void send_event(InputEvent *event)
{
    if (event_callback) {
        (*event_callback)(event);
    }
}

/*-----------------------------------------------------------------------*/

static void send_mouse_event(InputEventDetail detail, float x, float y,
                             float scroll, double timestamp)
{
    int w = 1, h = 1;
    if (sdl_window) {
        SDL_GetWindowSize(sdl_window, &w, &h);
    }
    InputEvent event = {
        .type = INPUT_EVENT_MOUSE, .detail = detail, .timestamp = timestamp,
        .mouse = {.x = bound(x / lbound(w, 1), 0.0f, 65535.0f/65536.0f),
                  .y = bound(y / lbound(h, 1), 0.0f, 65535.0f/65536.0f),
                  .scroll = scroll}};
    send_event(&event);
}

/*-----------------------------------------------------------------------*/

static int find_joystick(SDL_JoystickID id)
{
    for (int i = 0; i < num_joysticks; i++) {
        if (gamepads[i] && gamepad_ids[i] == id) {
            return i;
        }
    }
    return -1;
}

/*-----------------------------------------------------------------------*/

static void send_text_char(int32_t ch, double timestamp)
{
    InputEvent event = {.type = INPUT_EVENT_TEXT, .detail = INPUT_TEXT_INPUT,
                        .timestamp = timestamp, .text = {.ch = ch}};
    send_event(&event);
}

static void send_text_detail(InputEventDetail detail, double timestamp)
{
    InputEvent event = {.type = INPUT_EVENT_TEXT, .detail = detail,
                        .timestamp = timestamp};
    send_event(&event);
}

/*-----------------------------------------------------------------------*/

static void handle_event(const SDL_Event *event, double timestamp)
{
    switch (event->type) {

      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        sdl_quit_requested = 1;
        break;

      case SDL_EVENT_WINDOW_FOCUS_GAINED:
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        window_focused = (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        sdl_graphics_handle_event(event);
        do_update_grab(NULL);
        break;

      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        const int key = convert_scancode(event->key.scancode);
        const int down = (event->type == SDL_EVENT_KEY_DOWN);
        InputEvent ev = {
            .type = INPUT_EVENT_KEYBOARD,
            .detail = key ? (down ? INPUT_KEYBOARD_KEY_DOWN
                                  : INPUT_KEYBOARD_KEY_UP)
                          : (down ? INPUT_KEYBOARD_SYSTEM_KEY_DOWN
                                  : INPUT_KEYBOARD_SYSTEM_KEY_UP),
            .timestamp = timestamp,
            .keyboard = {.key = key, .system_key = event->key.scancode,
                         .is_repeat = down && event->key.repeat}};
        send_event(&ev);
        /* Text editing keys while text entry is active. */
        if (text_active && down) {
            switch (key) {
              case KEY_BACKSPACE: send_text_detail(INPUT_TEXT_BACKSPACE, timestamp); break;
              case KEY_DELETE:    send_text_detail(INPUT_TEXT_DELETE, timestamp); break;
              case KEY_LEFT:      send_text_detail(INPUT_TEXT_CURSOR_LEFT, timestamp); break;
              case KEY_RIGHT:     send_text_detail(INPUT_TEXT_CURSOR_RIGHT, timestamp); break;
              case KEY_HOME:      send_text_detail(INPUT_TEXT_CURSOR_HOME, timestamp); break;
              case KEY_END:       send_text_detail(INPUT_TEXT_CURSOR_END, timestamp); break;
              case KEY_ENTER:
              case KEY_NUMPAD_ENTER: send_text_detail(INPUT_TEXT_DONE, timestamp); break;
              case KEY_ESCAPE:    send_text_detail(INPUT_TEXT_CANCELLED, timestamp); break;
              default: break;
            }
        }
        break;
      }

      case SDL_EVENT_TEXT_INPUT:
        if (text_active) {
            const unsigned char *s = (const unsigned char *)event->text.text;
            while (*s) {
                int32_t ch = *s++;
                int extra = 0;
                if (ch >= 0xF0) { ch &= 0x07; extra = 3; }
                else if (ch >= 0xE0) { ch &= 0x0F; extra = 2; }
                else if (ch >= 0xC0) { ch &= 0x1F; extra = 1; }
                for (; extra > 0 && (*s & 0xC0) == 0x80; extra--) {
                    ch = (ch << 6) | (*s++ & 0x3F);
                }
                send_text_char(ch, timestamp);
            }
        }
        break;

      case SDL_EVENT_MOUSE_MOTION:
        send_mouse_event(INPUT_MOUSE_MOVE, event->motion.x, event->motion.y,
                         0, timestamp);
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        const int down = (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        InputEventDetail detail;
        switch (event->button.button) {
          case SDL_BUTTON_LEFT:
            detail = down ? INPUT_MOUSE_LMB_DOWN : INPUT_MOUSE_LMB_UP; break;
          case SDL_BUTTON_MIDDLE:
            detail = down ? INPUT_MOUSE_MMB_DOWN : INPUT_MOUSE_MMB_UP; break;
          case SDL_BUTTON_RIGHT:
            detail = down ? INPUT_MOUSE_RMB_DOWN : INPUT_MOUSE_RMB_UP; break;
          default:
            return;
        }
        send_mouse_event(detail, event->button.x, event->button.y, 0,
                         timestamp);
        break;
      }

      case SDL_EVENT_MOUSE_WHEEL: {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        if (event->wheel.y != 0) {
            send_mouse_event(INPUT_MOUSE_SCROLL_V, mx, my, -event->wheel.y,
                             timestamp);
        }
        if (event->wheel.x != 0) {
            send_mouse_event(INPUT_MOUSE_SCROLL_H, mx, my, event->wheel.x,
                             timestamp);
        }
        break;
      }

      case SDL_EVENT_GAMEPAD_ADDED: {
        if (num_joysticks >= MAX_JOYSTICKS
         && find_joystick(event->gdevice.which) < 0) {
            /* Reuse a disconnected slot if possible. */
            int slot = -1;
            for (int i = 0; i < num_joysticks; i++) {
                if (!gamepads[i]) { slot = i; break; }
            }
            if (slot < 0) {
                break;
            }
        }
        int slot = -1;
        for (int i = 0; i < num_joysticks; i++) {
            if (!gamepads[i]) { slot = i; break; }
        }
        if (slot < 0) {
            slot = num_joysticks++;
        }
        gamepads[slot] = SDL_OpenGamepad(event->gdevice.which);
        if (!gamepads[slot]) {
            break;
        }
        gamepad_ids[slot] = event->gdevice.which;
        joystick_info[slot].connected = 1;
        joystick_info[slot].can_rumble = SDL_GetBooleanProperty(
            SDL_GetGamepadProperties(gamepads[slot]),
            SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        joystick_info[slot].num_buttons = SDL_GAMEPAD_BUTTON_COUNT + 2;
        joystick_info[slot].num_sticks = NUM_STICKS;
        InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                         .detail = INPUT_JOYSTICK_CONNECTED,
                         .timestamp = timestamp,
                         .joystick = {.device = slot}};
        send_event(&ev);
        break;
      }

      case SDL_EVENT_GAMEPAD_REMOVED: {
        const int slot = find_joystick(event->gdevice.which);
        if (slot >= 0) {
            SDL_CloseGamepad(gamepads[slot]);
            gamepads[slot] = NULL;
            joystick_info[slot].connected = 0;
            InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                             .detail = INPUT_JOYSTICK_DISCONNECTED,
                             .timestamp = timestamp,
                             .joystick = {.device = slot}};
            send_event(&ev);
        }
        break;
      }

      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const int slot = find_joystick(event->gbutton.which);
        if (slot < 0 || (!window_focused && !unfocused_joystick)) {
            break;
        }
        const int down = (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        const int button = event->gbutton.button;
        /* D-pad buttons are reported as D-pad changes. */
        if (button >= SDL_GAMEPAD_BUTTON_DPAD_UP
         && button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
            int *dpad = dpad_state[slot];
            switch (button) {
              case SDL_GAMEPAD_BUTTON_DPAD_UP:    dpad[1] = down ? -1 : (dpad[1] == -1 ? 0 : dpad[1]); break;
              case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  dpad[1] = down ? +1 : (dpad[1] == +1 ? 0 : dpad[1]); break;
              case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  dpad[0] = down ? -1 : (dpad[0] == -1 ? 0 : dpad[0]); break;
              case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dpad[0] = down ? +1 : (dpad[0] == +1 ? 0 : dpad[0]); break;
            }
            InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                             .detail = INPUT_JOYSTICK_DPAD_CHANGE,
                             .timestamp = timestamp,
                             .joystick = {.device = slot,
                                          .x = (float)dpad[0],
                                          .y = (float)dpad[1]}};
            send_event(&ev);
            break;
        }
        InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                         .detail = down ? INPUT_JOYSTICK_BUTTON_DOWN
                                        : INPUT_JOYSTICK_BUTTON_UP,
                         .timestamp = timestamp,
                         .joystick = {.device = slot, .index = button}};
        send_event(&ev);
        break;
      }

      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int slot = find_joystick(event->gaxis.which);
        if (slot < 0 || (!window_focused && !unfocused_joystick)) {
            break;
        }
        const float value = event->gaxis.value / 32767.0f;
        int stick = -1, component = 0;
        switch (event->gaxis.axis) {
          case SDL_GAMEPAD_AXIS_LEFTX:  stick = 0; component = 0; break;
          case SDL_GAMEPAD_AXIS_LEFTY:  stick = 0; component = 1; break;
          case SDL_GAMEPAD_AXIS_RIGHTX: stick = 1; component = 0; break;
          case SDL_GAMEPAD_AXIS_RIGHTY: stick = 1; component = 1; break;
          case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
          case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
            /* Report triggers as digital buttons. */
            const int button = SDL_GAMEPAD_BUTTON_COUNT
                + (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            static uint8_t trigger_down[MAX_JOYSTICKS][2];
            const int index = button - SDL_GAMEPAD_BUTTON_COUNT;
            const int down = (value > 0.5f);
            if (down != trigger_down[slot][index]) {
                trigger_down[slot][index] = (uint8_t)down;
                InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                                 .detail = down ? INPUT_JOYSTICK_BUTTON_DOWN
                                                : INPUT_JOYSTICK_BUTTON_UP,
                                 .timestamp = timestamp,
                                 .joystick = {.device = slot, .index = button}};
                send_event(&ev);
            }
            break;
          }
        }
        if (stick >= 0) {
            stick_state[slot][stick][component] = bound(value, -1.0f, 1.0f);
            InputEvent ev = {.type = INPUT_EVENT_JOYSTICK,
                             .detail = INPUT_JOYSTICK_STICK_CHANGE,
                             .timestamp = timestamp,
                             .joystick = {.device = slot, .index = stick,
                                          .x = stick_state[slot][stick][0],
                                          .y = stick_state[slot][stick][1]}};
            send_event(&ev);
        }
        break;
      }

      default:
        sdl_graphics_handle_event(event);
        break;
    }
}

/*-----------------------------------------------------------------------*/

static int convert_scancode(SDL_Scancode scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return KEY_A + (scancode - SDL_SCANCODE_A);
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return KEY_1 + (scancode - SDL_SCANCODE_1);
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return KEY_F1 + (scancode - SDL_SCANCODE_F1);
    }
    if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F20) {
        return KEY_F13 + (scancode - SDL_SCANCODE_F13);
    }
    if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
        return KEY_NUMPAD_1 + (scancode - SDL_SCANCODE_KP_1);
    }
    switch (scancode) {
      case SDL_SCANCODE_0:            return KEY_0;
      case SDL_SCANCODE_RETURN:       return KEY_ENTER;
      case SDL_SCANCODE_ESCAPE:       return KEY_ESCAPE;
      case SDL_SCANCODE_BACKSPACE:    return KEY_BACKSPACE;
      case SDL_SCANCODE_TAB:          return KEY_TAB;
      case SDL_SCANCODE_SPACE:        return KEY_SPACE;
      case SDL_SCANCODE_MINUS:        return KEY_HYPHEN;
      case SDL_SCANCODE_EQUALS:       return KEY_EQUALS;
      case SDL_SCANCODE_LEFTBRACKET:  return KEY_LEFTBRACKET;
      case SDL_SCANCODE_RIGHTBRACKET: return KEY_RIGHTBRACKET;
      case SDL_SCANCODE_BACKSLASH:    return KEY_BACKSLASH;
      case SDL_SCANCODE_NONUSHASH:    return KEY_BACKSLASH;
      case SDL_SCANCODE_SEMICOLON:    return KEY_SEMICOLON;
      case SDL_SCANCODE_APOSTROPHE:   return KEY_APOSTROPHE;
      case SDL_SCANCODE_GRAVE:        return KEY_BACKQUOTE;
      case SDL_SCANCODE_COMMA:        return KEY_COMMA;
      case SDL_SCANCODE_PERIOD:       return KEY_PERIOD;
      case SDL_SCANCODE_SLASH:        return KEY_SLASH;
      case SDL_SCANCODE_CAPSLOCK:     return KEY_CAPSLOCK;
      case SDL_SCANCODE_PRINTSCREEN:  return KEY_PRINTSCREEN;
      case SDL_SCANCODE_SCROLLLOCK:   return KEY_SCROLLLOCK;
      case SDL_SCANCODE_PAUSE:        return KEY_PAUSE;
      case SDL_SCANCODE_INSERT:       return KEY_INSERT;
      case SDL_SCANCODE_HOME:         return KEY_HOME;
      case SDL_SCANCODE_PAGEUP:       return KEY_PAGEUP;
      case SDL_SCANCODE_DELETE:       return KEY_DELETE;
      case SDL_SCANCODE_END:          return KEY_END;
      case SDL_SCANCODE_PAGEDOWN:     return KEY_PAGEDOWN;
      case SDL_SCANCODE_RIGHT:        return KEY_RIGHT;
      case SDL_SCANCODE_LEFT:         return KEY_LEFT;
      case SDL_SCANCODE_DOWN:         return KEY_DOWN;
      case SDL_SCANCODE_UP:           return KEY_UP;
      case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUMLOCK;
      case SDL_SCANCODE_KP_DIVIDE:    return KEY_NUMPAD_DIVIDE;
      case SDL_SCANCODE_KP_MULTIPLY:  return KEY_NUMPAD_MULTIPLY;
      case SDL_SCANCODE_KP_MINUS:     return KEY_NUMPAD_SUBTRACT;
      case SDL_SCANCODE_KP_PLUS:      return KEY_NUMPAD_ADD;
      case SDL_SCANCODE_KP_ENTER:     return KEY_NUMPAD_ENTER;
      case SDL_SCANCODE_KP_0:         return KEY_NUMPAD_0;
      case SDL_SCANCODE_KP_PERIOD:    return KEY_NUMPAD_DECIMAL;
      case SDL_SCANCODE_KP_EQUALS:    return KEY_NUMPAD_EQUALS;
      case SDL_SCANCODE_NONUSBACKSLASH: return KEY_LESS;
      case SDL_SCANCODE_APPLICATION:  return KEY_MENU;
      case SDL_SCANCODE_MENU:         return KEY_MENU;
      case SDL_SCANCODE_UNDO:         return KEY_UNDO;
      case SDL_SCANCODE_INTERNATIONAL3: return KEY_YEN;
      case SDL_SCANCODE_LANG1:        return KEY_KANA;
      case SDL_SCANCODE_LANG2:        return KEY_KANJI;
      case SDL_SCANCODE_INTERNATIONAL4: return KEY_HENKAN;
      case SDL_SCANCODE_INTERNATIONAL5: return KEY_MUHENKAN;
      case SDL_SCANCODE_LCTRL:        return KEY_LEFTCONTROL;
      case SDL_SCANCODE_LSHIFT:       return KEY_LEFTSHIFT;
      case SDL_SCANCODE_LALT:         return KEY_LEFTALT;
      case SDL_SCANCODE_LGUI:         return KEY_LEFTMETA;
      case SDL_SCANCODE_RCTRL:        return KEY_RIGHTCONTROL;
      case SDL_SCANCODE_RSHIFT:       return KEY_RIGHTSHIFT;
      case SDL_SCANCODE_RALT:         return KEY_RIGHTALT;
      case SDL_SCANCODE_RGUI:         return KEY_RIGHTMETA;
      default:                        return 0;
    }
}

/*************************************************************************/
/*************************************************************************/
