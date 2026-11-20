/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/sdlgl.h: Interface between the SDL3 platform layer and
 * the OpenGL / OpenGL ES rendering backend (src/sysdep/opengl), used when
 * building with SIL_USE_SDL_GL=1.  The functions mirror those in
 * src/sysdep/sdlgpu/sdlgpu.h.
 */

#ifndef SIL_SRC_SYSDEP_SDL_SDLGL_H
#define SIL_SRC_SYSDEP_SDL_SDLGL_H

struct SDL_Window;

/*************************************************************************/
/*************************************************************************/

/**
 * sdlgl_set_display_attr:  Handle an OpenGL-specific display attribute
 * ("opengl_debug", "opengl_version", "multisample").
 *
 * [Parameters]
 *     name: Attribute name.
 *     args: Attribute value(s).
 *     result_ret: Set to the result (true = success) if the attribute
 *         was handled.
 * [Return value]
 *     True if the attribute was handled, false if it is not an OpenGL
 *     attribute.
 */
extern int sdlgl_set_display_attr(const char *name, va_list args,
                                  int *result_ret);

/**
 * sdlgl_prepare_window:  Set the SDL OpenGL attributes for the window
 * about to be created.  Must be called before the window is created.
 *
 * [Parameters]
 *     depth_bits: Requested depth buffer size.
 *     stencil_bits: Requested stencil buffer size.
 * [Return value]
 *     True on success, false if the OpenGL library could not be loaded.
 */
extern int sdlgl_prepare_window(int depth_bits, int stencil_bits);

/**
 * sdlgl_init:  Create the OpenGL context for the given window and set up
 * the OpenGL backend.
 *
 * [Parameters]
 *     window: SDL window (created with SDL_WINDOW_OPENGL).
 *     vsync: True to synchronize buffer swaps to the display refresh.
 * [Return value]
 *     True on success, false on error.
 */
extern int sdlgl_init(struct SDL_Window *window, int vsync);

/**
 * sdlgl_cleanup:  Shut down the OpenGL backend and destroy the context.
 */
extern void sdlgl_cleanup(void);

/**
 * sdlgl_set_vsync:  Enable or disable vertical sync.
 */
extern void sdlgl_set_vsync(int vsync);

/**
 * sdlgl_start_frame:  Prepare for rendering a frame of the given size.
 */
extern void sdlgl_start_frame(int width, int height);

/**
 * sdlgl_finish_frame:  Draw any pending primitives and present the frame.
 */
extern void sdlgl_finish_frame(void);

/**
 * sdlgl_sync:  Wait for all pending rendering to complete.
 *
 * [Parameters]
 *     flush: True to also free resources pending deletion.
 */
extern void sdlgl_sync(int flush);

/*************************************************************************/
/*************************************************************************/

#endif  // SIL_SRC_SYSDEP_SDL_SDLGL_H
