/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/opengl.c: OpenGL / OpenGL ES context management for the
 * SDL3 platform layer (SIL_USE_SDL_GL=1).  SDL creates the window and the
 * GL context (via GLX, EGL on Wayland/KMSDRM, ...); rendering is done by
 * the regular OpenGL backend in src/sysdep/opengl, including batching of
 * immediate-mode primitives.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/sysdep.h"
#include "src/sysdep/opengl/opengl.h"
#include "src/sysdep/sdl/internal.h"
#include "src/sysdep/sdl/sdlgl.h"

#include <stdarg.h>

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

/* Window and GL context. */
static SDL_Window *gl_window;
static SDL_GLContext gl_context;

/* Requested context parameters (display attributes). */
static int desired_major, desired_minor;
static uint8_t use_debug;
static int multisample_samples = 1;

/* Have we looked up the GL functions yet? */
static uint8_t did_lookup;

/* Current display (default framebuffer) size. */
static int display_width, display_height;

/*************************************************************************/
/**************************** Local routines *****************************/
/*************************************************************************/

/**
 * lookup_function:  Wrapper for SDL_GL_GetProcAddress() with the signature
 * expected by opengl_lookup_functions().
 */
static void *lookup_function(const char *name)
{
    return (void *)SDL_GL_GetProcAddress(name);
}

/*-----------------------------------------------------------------------*/

/**
 * create_context:  Create a GL context of the given version for the
 * window and make it current.
 *
 * [Parameters]
 *     major, minor: Context version (0 = SDL's default).
 * [Return value]
 *     True on success, false on error.
 */
static int create_context(int major, int minor)
{
#ifdef SIL_OPENGL_ES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        major >= 3 ? SDL_GL_CONTEXT_PROFILE_CORE : 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        use_debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    gl_context = SDL_GL_CreateContext(gl_window);
    if (!gl_context) {
        DLOG("Failed to create OpenGL%s %d.%d context: %s",
#ifdef SIL_OPENGL_ES
             " ES",
#else
             "",
#endif
             major, minor, SDL_GetError());
        return 0;
    }
    return 1;
}

/*************************************************************************/
/************************** Interface routines ***************************/
/*************************************************************************/

int sdlgl_set_display_attr(const char *name, va_list args, int *result_ret)
{
    if (strcmp(name, "multisample") == 0) {
        const int value = va_arg(args, int);
        if (value <= 0) {
            *result_ret = 0;
        } else {
            /* Only effective for the first window (SDL picks the pixel
             * format when the window is created). */
            multisample_samples = value;
            *result_ret = 1;
        }
        return 1;
    }
    if (strcmp(name, "opengl_debug") == 0) {
        use_debug = (va_arg(args, int) != 0);
        if (gl_context) {
            opengl_enable_debug(use_debug);
        }
        *result_ret = 1;
        return 1;
    }
    if (strcmp(name, "opengl_version") == 0) {
        desired_major = va_arg(args, int);
        desired_minor = va_arg(args, int);
        *result_ret = 1;
        return 1;
    }
    return 0;
}

/*-----------------------------------------------------------------------*/

int sdlgl_prepare_window(int depth_bits, int stencil_bits)
{
#ifdef SIL_OPENGL_ES
    /* Make SDL use EGL with the GLES library even on X11.  This must be
     * set before the GL library is loaded. */
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_ES);
#endif
    if (!SDL_GL_LoadLibrary(NULL)) {
        DLOG("Failed to load the OpenGL library: %s", SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth_bits);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil_bits);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, multisample_samples > 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,
                        multisample_samples > 1 ? multisample_samples : 0);
    return 1;
}

/*-----------------------------------------------------------------------*/

int sdlgl_init(SDL_Window *window, int vsync)
{
    PRECOND(window != NULL, return 0);
    PRECOND(!gl_context, return 0);

    gl_window = window;

    int ok;
#ifdef SIL_OPENGL_ES
    /* Prefer GLES 3.x (like the Android port), fall back to 2.0. */
    if (desired_major > 0) {
        ok = create_context(desired_major, desired_minor);
    } else {
        ok = create_context(3, 0) || create_context(2, 0);
    }
#else
    ok = create_context(desired_major >= 3 ? desired_major : 0,
                        desired_major >= 3 ? desired_minor : 0);
#endif
    if (!ok) {
        gl_window = NULL;
        return 0;
    }
    if (!SDL_GL_MakeCurrent(gl_window, gl_context)) {
        DLOG("SDL_GL_MakeCurrent() failed: %s", SDL_GetError());
        goto error_destroy_context;
    }
    sdlgl_set_vsync(vsync);

    if (!did_lookup) {
        opengl_lookup_functions(lookup_function);
        did_lookup = 1;
    }
    opengl_enable_debug(use_debug);

    sdl_graphics_window_size(&display_width, &display_height);
    const uint32_t gl_flags =
#ifdef SIL_OPENGL_ES
        OPENGL_FEATURE_DELAYED_DELETE
      | OPENGL_FEATURE_FAST_STATIC_VBO
      | OPENGL_FEATURE_FRAMEBUFFERS
      | OPENGL_FEATURE_GENERATEMIPMAP;
#else
        /* As for GLX (see src/sysdep/linux/graphics.c), immediate-mode
         * data is passed in client memory except with a core profile. */
        OPENGL_FEATURE_FAST_STATIC_VBO
      | (desired_major >= 3 ? OPENGL_FEATURE_FAST_DYNAMIC_VBO : 0)
      | OPENGL_FEATURE_NATIVE_QUADS
      | OPENGL_FEATURE_GENERATEMIPMAP;
#endif
    if (!opengl_init(display_width, display_height, gl_flags)) {
        DLOG("Failed to set up OpenGL!");
        goto error_destroy_context;
    }

    DLOG("OpenGL context: %s / %s / %s",
         (const char *)glGetString(GL_VERSION),
         (const char *)glGetString(GL_RENDERER),
         SDL_GetCurrentVideoDriver());
    return 1;

  error_destroy_context:
    SDL_GL_MakeCurrent(gl_window, NULL);
    SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    gl_window = NULL;
    return 0;
}

/*-----------------------------------------------------------------------*/

void sdlgl_cleanup(void)
{
    if (!gl_context) {
        return;
    }
    opengl_cleanup();
    SDL_GL_MakeCurrent(gl_window, NULL);
    SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    gl_window = NULL;
}

/*-----------------------------------------------------------------------*/

void sdlgl_set_vsync(int vsync)
{
    if (!gl_context) {
        return;
    }
    if (vsync) {
        /* Prefer adaptive vsync if available. */
        if (!SDL_GL_SetSwapInterval(-1)) {
            SDL_GL_SetSwapInterval(1);
        }
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

/*-----------------------------------------------------------------------*/

void sdlgl_start_frame(int width, int height)
{
    if (width != display_width || height != display_height) {
        display_width = width;
        display_height = height;
        opengl_set_display_size(width, height);
    }
    opengl_start_frame();
}

/*-----------------------------------------------------------------------*/

void sdlgl_finish_frame(void)
{
    /* Draw any batched primitives before presenting the frame. */
    opengl_batch_flush();
    SDL_GL_SwapWindow(gl_window);
}

/*-----------------------------------------------------------------------*/

void sdlgl_sync(int flush)
{
    opengl_sync();
    if (flush) {
        opengl_free_dead_resources(1);
    }
}

/*************************************************************************/
/*************************************************************************/
