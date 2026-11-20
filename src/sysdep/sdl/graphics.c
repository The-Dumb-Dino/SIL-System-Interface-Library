/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/graphics.c: Display and window management for the SDL3
 * platform layer.  Rendering itself is handled by the SDL_GPU backend
 * (src/sysdep/sdlgpu); this file implements the window-related
 * sys_graphics_*() functions and forwards frame control to the backend.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/graphics.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/sdl/internal.h"
#include "src/sysdep/sdlgpu/sdlgpu.h"
#include "src/texture.h"

#include <stdarg.h>

#if defined(SIL_PLATFORM_MACOSX) && defined(DEBUG)
# include <objc/message.h>
# include <objc/runtime.h>
#endif

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

SDL_Window *sdl_window;

/* Graphics capability structure returned to the high-level code. */
static SysGraphicsInfo graphics_info;

/* Display attributes requested by the client. */
static uint8_t attr_center_window = 1;
static uint8_t attr_window = 0;
static uint8_t attr_resizable = 0;
static uint8_t attr_vsync = 1;
static int attr_depth_bits = 16;
static int attr_stencil_bits = 0;
static int attr_device = 0;
static int attr_refresh_rate = 0;

/* Is the backend initialized? */
static uint8_t gpu_initialized;

/* Window state. */
static uint8_t has_focus = 1;
static uint8_t show_pointer = 1;
static int resize_min_w, resize_min_h, resize_max_w, resize_max_h;
static char *window_title;

/*************************************************************************/
/************************ Main-thread trampolines ************************/
/*************************************************************************/

typedef struct CreateWindowParams {
    int width, height;
    int ok;
} CreateWindowParams;

static void do_create_window(void *userdata)
{
    CreateWindowParams *params = userdata;

    const SDL_DisplayID *displays = SDL_GetDisplays(NULL);
    SDL_DisplayID display = 0;
    if (displays) {
        for (int i = 0; displays[i]; i++) {
            if (i == attr_device) {
                display = displays[i];
                break;
            }
        }
        SDL_free((void *)displays);
    }

    if (sdl_window) {
        SDL_SetWindowFullscreen(sdl_window, !attr_window);
        if (attr_window) {
            SDL_SetWindowSize(sdl_window, params->width, params->height);
            if (attr_center_window) {
                SDL_SetWindowPosition(
                    sdl_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                    SDL_WINDOWPOS_CENTERED_DISPLAY(display));
            }
        } else {
            SDL_DisplayMode mode;
            if (SDL_GetClosestFullscreenDisplayMode(
                    display, params->width, params->height,
                    (float)attr_refresh_rate, false, &mode)) {
                SDL_SetWindowFullscreenMode(sdl_window, &mode);
            }
        }
        SDL_SetWindowResizable(sdl_window, attr_resizable);
        SDL_SyncWindow(sdl_window);
        params->ok = 1;
        return;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          window_title ? window_title : "");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          params->width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          params->height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                           attr_resizable);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           !attr_window);
    /* The window type must match the GPU driver (Direct3D 12 needs no
     * special window type). */
    const char *driver = sdlgpu_driver_name();
    if (driver && strcmp(driver, "vulkan") == 0) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN,
                               true);
    } else if (driver && strcmp(driver, "metal") == 0) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN,
                               true);
    }
    sdl_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!sdl_window) {
        DLOG("SDL_CreateWindow() failed: %s", SDL_GetError());
        params->ok = 0;
        return;
    }
    if (!attr_window) {
        SDL_DisplayMode mode;
        if (SDL_GetClosestFullscreenDisplayMode(
                display, params->width, params->height,
                (float)attr_refresh_rate, false, &mode)) {
            SDL_SetWindowFullscreenMode(sdl_window, &mode);
        }
    }
    if (resize_min_w > 0) {
        SDL_SetWindowMinimumSize(sdl_window, resize_min_w, resize_min_h);
    }
    if (resize_max_w > 0) {
        SDL_SetWindowMaximumSize(sdl_window, resize_max_w, resize_max_h);
    }
    SDL_ShowWindow(sdl_window);
    SDL_RaiseWindow(sdl_window);
    SDL_SyncWindow(sdl_window);
#if defined(SIL_PLATFORM_MACOSX) && defined(DEBUG)
    {
        /* Log the window number so that tests can capture the window
         * contents with "screencapture -l<number>". */
        void *nswindow = SDL_GetPointerProperty(
            SDL_GetWindowProperties(sdl_window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
        if (nswindow) {
            long (*msg)(void *, SEL) = (long (*)(void *, SEL))objc_msgSend;
            DLOG("Window created, Cocoa window number %ld",
                 msg(nswindow, sel_registerName("windowNumber")));
        }
    }
#endif
    params->ok = 1;
}

static void do_destroy_window(UNUSED void *userdata)
{
    if (sdl_window) {
        SDL_DestroyWindow(sdl_window);
        sdl_window = NULL;
    }
}

static void do_set_title(UNUSED void *userdata)
{
    if (sdl_window) {
        SDL_SetWindowTitle(sdl_window, window_title ? window_title : "");
    }
}

static void do_update_cursor(UNUSED void *userdata)
{
    if (show_pointer) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

static void do_set_resize_limits(UNUSED void *userdata)
{
    if (sdl_window) {
        SDL_SetWindowMinimumSize(sdl_window, resize_min_w, resize_min_h);
        SDL_SetWindowMaximumSize(sdl_window, resize_max_w, resize_max_h);
    }
}

static void do_set_icon(void *userdata)
{
    if (sdl_window) {
        SDL_SetWindowIcon(sdl_window, (SDL_Surface *)userdata);
    }
}

/*************************************************************************/
/************************** Interface routines ***************************/
/*************************************************************************/

const SysGraphicsInfo *sys_graphics_init(void)
{
    const SDL_DisplayID *displays = SDL_GetDisplays(&graphics_info.num_devices);
    if (!displays || graphics_info.num_devices <= 0) {
        DLOG("No displays found: %s", SDL_GetError());
        SDL_free((void *)displays);
        return NULL;
    }

    /* Collect the fullscreen modes of all displays. */
    int total = 0;
    for (int i = 0; displays[i]; i++) {
        int count = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(displays[i],
                                                                &count);
        total += count;
        SDL_free(modes);
    }
    if (total == 0) {
        total = 1;
    }
    GraphicsDisplayModeEntry *list =
        mem_alloc(sizeof(*list) * total, 0, MEM_ALLOC_CLEAR);
    if (!list) {
        SDL_free((void *)displays);
        return NULL;
    }
    int n = 0;
    for (int i = 0; displays[i]; i++) {
        int count = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(displays[i],
                                                                &count);
        for (int j = 0; j < count && n < total; j++, n++) {
            list[n].device = i;
            list[n].device_name = NULL;
            list[n].width = modes[j]->w;
            list[n].height = modes[j]->h;
            list[n].refresh = modes[j]->refresh_rate;
        }
        SDL_free(modes);
    }
    if (n == 0) {
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(displays[0]);
        list[0].width = desktop ? desktop->w : 1280;
        list[0].height = desktop ? desktop->h : 720;
        list[0].refresh = desktop ? desktop->refresh_rate : 60;
        n = 1;
    }
    SDL_free((void *)displays);

    graphics_info.has_windowed_mode = 1;
    graphics_info.num_modes = n;
    graphics_info.modes = list;
    return &graphics_info;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_cleanup(void)
{
    if (gpu_initialized) {
        sdlgpu_cleanup();
        gpu_initialized = 0;
    }
    sdl_run_on_main_thread(do_destroy_window, NULL);
    mem_free((void *)graphics_info.modes);
    graphics_info.modes = NULL;
    mem_free(window_title);
    window_title = NULL;
}

/*-----------------------------------------------------------------------*/

int sys_graphics_device_width(void)
{
    const SDL_DisplayMode *mode =
        SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return mode ? mode->w : 0;
}

int sys_graphics_device_height(void)
{
    const SDL_DisplayMode *mode =
        SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return mode ? mode->h : 0;
}

/*-----------------------------------------------------------------------*/

int sys_graphics_set_display_attr(const char *name, va_list args)
{
    if (strcmp(name, "center_window") == 0) {
        attr_center_window = (va_arg(args, int) != 0);
        return 1;
    }
    if (strcmp(name, "depth_bits") == 0) {
        const int value = va_arg(args, int);
        if (value < 0) {
            return 0;
        }
        attr_depth_bits = value;
        return 1;
    }
    if (strcmp(name, "device") == 0) {
        const int value = va_arg(args, int);
        if (value < 0 || value >= graphics_info.num_devices) {
            return 0;
        }
        attr_device = value;
        return 1;
    }
    if (strcmp(name, "gpu_driver") == 0) {
        /* SDL_GPU driver name ("metal", "vulkan", "direct3d12"), or NULL
         * for SDL's default choice.  Only effective before the first
         * graphics_set_display_mode() call. */
        const char *value = va_arg(args, const char *);
        if (gpu_initialized) {
            return 0;
        }
        SDL_SetHint(SDL_HINT_GPU_DRIVER, value);
        return 1;
    }
    if (strcmp(name, "multisample") == 0) {
        const int value = va_arg(args, int);
        return value == 1;  // Multisampling is not (yet) supported.
    }
    if (strcmp(name, "refresh_rate") == 0) {
        const float value = (float)va_arg(args, double);
        if (value < 0) {
            return 0;
        }
        attr_refresh_rate = (int)value;
        return 1;
    }
    if (strcmp(name, "stencil_bits") == 0) {
        const int value = va_arg(args, int);
        if (value < 0) {
            return 0;
        }
        attr_stencil_bits = value;
        return 1;
    }
    if (strcmp(name, "vsync") == 0) {
        attr_vsync = (va_arg(args, int) != 0);
        if (gpu_initialized) {
            sdlgpu_set_vsync(attr_vsync);
        }
        return 1;
    }
    if (strcmp(name, "window") == 0) {
        attr_window = (va_arg(args, int) != 0);
        return 1;
    }
    if (strcmp(name, "window_resizable") == 0) {
        attr_resizable = (va_arg(args, int) != 0);
        return 1;
    }
    /* OpenGL-specific attributes are accepted and ignored so that
     * clients written for the OpenGL backend keep working. */
    if (strcmp(name, "opengl_debug") == 0
     || strcmp(name, "opengl_version") == 0) {
        return 1;
    }
    return 0;
}

/*-----------------------------------------------------------------------*/

GraphicsError sys_graphics_set_display_mode(int width, int height)
{
    /* The GPU device must exist before the window is created. */
    if (!gpu_initialized && !sdlgpu_create_device()) {
        return GRAPHICS_ERROR_BACKEND_NOT_FOUND;
    }

    CreateWindowParams params = {.width = width, .height = height, .ok = 0};
    sdl_run_on_main_thread(do_create_window, &params);
    if (!params.ok) {
        return GRAPHICS_ERROR_MODE_NOT_SUPPORTED;
    }

    if (!gpu_initialized) {
        if (!sdlgpu_init(sdl_window, attr_depth_bits, attr_stencil_bits,
                         attr_vsync)) {
            sdl_run_on_main_thread(do_destroy_window, NULL);
            return GRAPHICS_ERROR_BACKEND_NOT_FOUND;
        }
        gpu_initialized = 1;
    } else {
        sdlgpu_set_vsync(attr_vsync);
    }
    sdl_run_on_main_thread(do_update_cursor, NULL);
    return GRAPHICS_ERROR_SUCCESS;
}

/*-----------------------------------------------------------------------*/

int sys_graphics_display_is_window(void)
{
    return sdl_window != NULL
        && !(SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_set_window_title(const char *title)
{
    mem_free(window_title);
    window_title = mem_strdup(title, 0);
    sdl_run_on_main_thread(do_set_title, NULL);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_set_window_icon(SysTexture *texture)
{
    const int width = sys_texture_width(texture);
    const int height = sys_texture_height(texture);
    uint8_t *pixels = sys_texture_lock(texture, SYS_TEXTURE_LOCK_NORMAL,
                                       0, 0, width, height);
    if (!pixels) {
        return;
    }
    /* SIL texture data is stored bottom-up relative to the icon image
     * (row 0 = top of the image as loaded), which matches SDL. */
    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        width, height, SDL_PIXELFORMAT_ABGR8888, pixels, width * 4);
    if (surface) {
        sdl_run_on_main_thread(do_set_icon, surface);
        SDL_DestroySurface(surface);
    }
    sys_texture_unlock(texture, 0);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_set_window_resize_limits(
    int min_width, int min_height, int max_width, int max_height,
    UNUSED int min_aspect_x, UNUSED int min_aspect_y,
    UNUSED int max_aspect_x, UNUSED int max_aspect_y)
{
    resize_min_w = min_width;
    resize_min_h = min_height;
    resize_max_w = max_width;
    resize_max_h = max_height;
    sdl_run_on_main_thread(do_set_resize_limits, NULL);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_show_mouse_pointer(int on)
{
    show_pointer = (on != 0);
    sdl_run_on_main_thread(do_update_cursor, NULL);
}

int sys_graphics_get_mouse_pointer_state(void)
{
    return show_pointer;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_get_frame_period(int *numerator_ret, int *denominator_ret)
{
    *numerator_ret = 0;
    *denominator_ret = 1;
    if (!attr_vsync || !sdl_window) {
        return;
    }
    const SDL_DisplayMode *mode =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl_window));
    if (mode && mode->refresh_rate_numerator > 0) {
        *numerator_ret = mode->refresh_rate_denominator;
        *denominator_ret = mode->refresh_rate_numerator;
    } else if (mode && mode->refresh_rate > 0) {
        *numerator_ret = 1000;
        *denominator_ret = (int)(mode->refresh_rate * 1000);
    }
}

/*-----------------------------------------------------------------------*/

int sys_graphics_has_focus(void)
{
    return has_focus;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_start_frame(int *width_ret, int *height_ret)
{
    if (!sdl_window || !gpu_initialized) {
        *width_ret = 0;
        *height_ret = 0;
        return;
    }
    sdl_graphics_window_size(width_ret, height_ret);
    sdlgpu_start_frame(*width_ret, *height_ret);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_finish_frame(void)
{
    if (gpu_initialized) {
        sdlgpu_finish_frame();
    }
}

/*-----------------------------------------------------------------------*/

void sys_graphics_sync(int flush)
{
    if (gpu_initialized) {
        sdlgpu_sync(flush);
    }
}

/*************************************************************************/
/************* Internal interface routines (SDL layer only) **************/
/*************************************************************************/

void sdl_graphics_handle_event(const SDL_Event *event)
{
    switch (event->type) {
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        has_focus = 1;
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        has_focus = 0;
        break;
      default:
        break;
    }
}

/*-----------------------------------------------------------------------*/

void sdl_graphics_window_size(int *width_ret, int *height_ret)
{
    if (!sdl_window
     || !SDL_GetWindowSizeInPixels(sdl_window, width_ret, height_ret)) {
        *width_ret = 0;
        *height_ret = 0;
    }
}

/*************************************************************************/
/********* Compatibility with the Windows sysdep layer (misc.c) **********/
/*************************************************************************/

#ifdef SIL_PLATFORM_WINDOWS

/* These are normally provided by src/sysdep/windows/graphics.c, which the
 * SDL layer replaces. */

const char *windows_window_title(void);
const char *windows_window_title(void)
{
    return window_title;
}

void windows_show_mouse_pointer(int override);
void windows_show_mouse_pointer(int override)
{
    if (override >= 0) {
        show_pointer = (override != 0);
    }
    sdl_run_on_main_thread(do_update_cursor, NULL);
}

#endif  // SIL_PLATFORM_WINDOWS

/*************************************************************************/
/*************************************************************************/
