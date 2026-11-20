/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/sdlgpu.h: Interface between the platform layer and the
 * SDL_GPU rendering backend.  The backend implements all rendering-related
 * sys_graphics_*(), sys_texture_*(), sys_framebuffer_*() and sys_shader_*()
 * functions; the platform layer owns the window and calls these functions
 * to control the backend.
 */

#ifndef SIL_SRC_SYSDEP_SDLGPU_SDLGPU_H
#define SIL_SRC_SYSDEP_SDLGPU_SDLGPU_H

struct SDL_Window;

/*************************************************************************/
/*************************************************************************/

/**
 * sdlgpu_create_device:  Create the SDL_GPU device, if not yet created.
 * Must be called before the window is created, since the window type
 * depends on the driver (see sdlgpu_driver_is_vulkan()).  The driver can
 * be chosen with the SDL hint SDL_GPU_DRIVER ("metal", "vulkan", ...).
 *
 * [Return value]
 *     True on success, false on error.
 */
extern int sdlgpu_create_device(void);

/**
 * sdlgpu_driver_is_vulkan:  Return whether the created device uses the
 * Vulkan driver (the window must then be created with SDL_WINDOW_VULKAN
 * instead of SDL_WINDOW_METAL).
 */
extern int sdlgpu_driver_is_vulkan(void);

/**
 * sdlgpu_init:  Attach the SDL_GPU device (see sdlgpu_create_device())
 * to the given window and initialize the rendering state.
 *
 * [Parameters]
 *     window: Window to render into.
 *     depth_bits: Requested depth buffer size for the display (0 = none).
 *     stencil_bits: Requested stencil buffer size for the display (0 = none).
 *     vsync: True to synchronize presentation to the display refresh.
 * [Return value]
 *     True on success, false on error.
 */
extern int sdlgpu_init(struct SDL_Window *window, int depth_bits,
                       int stencil_bits, int vsync);

/**
 * sdlgpu_cleanup:  Destroy all rendering resources and the GPU device.
 */
extern void sdlgpu_cleanup(void);

/**
 * sdlgpu_set_vsync:  Change the vertical sync setting.
 */
extern void sdlgpu_set_vsync(int vsync);

/**
 * sdlgpu_start_frame:  Begin a new frame with the given display size
 * (in pixels).
 */
extern void sdlgpu_start_frame(int width, int height);

/**
 * sdlgpu_finish_frame:  Submit all pending rendering and present the frame.
 */
extern void sdlgpu_finish_frame(void);

/**
 * sdlgpu_sync:  Wait for all submitted GPU work to complete.
 */
extern void sdlgpu_sync(int flush);

/*************************************************************************/
/*************************************************************************/

#endif  // SIL_SRC_SYSDEP_SDLGPU_SDLGPU_H
