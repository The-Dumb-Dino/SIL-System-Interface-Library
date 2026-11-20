/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/internal.h: Internal header for the SDL3 platform layer
 * (window management, input and program entry point).  The SDL3 platform
 * layer is used together with the SDL_GPU rendering backend
 * (src/sysdep/sdlgpu) when building with SIL_USE_SDLGPU=1.
 */

#ifndef SIL_SRC_SYSDEP_SDL_INTERNAL_H
#define SIL_SRC_SYSDEP_SDL_INTERNAL_H

#include <SDL3/SDL.h>

/*************************************************************************/
/*************************************************************************/

/**
 * sdl_window:  The SDL window used for the display, or NULL if no display
 * mode has been set.
 */
extern SDL_Window *sdl_window;

/**
 * sdl_quit_requested:  Set when the user or system asks the program to quit
 * (window close button, Cmd-Q, SIGINT, ...).
 */
extern uint8_t sdl_quit_requested;

/**
 * sdl_run_on_main_thread:  Run the given function on the main (UI) thread
 * and wait for it to complete.  If called on the main thread, the function
 * is called directly.  SDL requires window and event operations to be
 * performed on the main thread on macOS.
 */
extern void sdl_run_on_main_thread(void (*function)(void *), void *userdata);

/**
 * sdl_graphics_handle_event:  Process a window-related SDL event.  Called
 * from the input event loop.
 */
extern void sdl_graphics_handle_event(const SDL_Event *event);

/**
 * sdl_graphics_window_size:  Return the current drawable size of the
 * window, in pixels.
 */
extern void sdl_graphics_window_size(int *width_ret, int *height_ret);

/*************************************************************************/
/*************************************************************************/

#endif  // SIL_SRC_SYSDEP_SDL_INTERNAL_H
