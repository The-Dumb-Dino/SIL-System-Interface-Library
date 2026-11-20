/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdl/main.c: Program entry point for the SDL3 platform layer.
 *
 * Unlike the Cocoa platform layer (which runs sil_main() on a secondary
 * thread while the main thread runs the NSApplication event loop), the
 * SDL3 layer runs sil_main() directly on the main thread; SDL processes
 * system events whenever sys_input_update() pumps the event queue.
 */

#define IN_SYSDEP
#undef SIL_MEMORY_FORBID_MALLOC  // We use malloc() for the resource path.

#include "src/base.h"
#include "src/main.h"
#include "src/math/fpu.h"
#include "src/sysdep.h"
#include "src/sysdep/sdl/internal.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
# include <dispatch/dispatch.h>
# include <pthread.h>
#endif

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

/* Resource directory path. */
static char *resource_dir;

/*************************************************************************/
/*************************** Signal handlers *****************************/
/*************************************************************************/

static void term_signal_handler(UNUSED int signum)
{
    sdl_quit_requested = 1;
}

/*************************************************************************/
/************************** Interface routines ***************************/
/*************************************************************************/

void sdl_run_on_main_thread(void (*function)(void *), void *userdata)
{
#ifdef __APPLE__
    if (pthread_main_np()) {
        (*function)(userdata);
    } else {
        dispatch_sync_f(dispatch_get_main_queue(), userdata, function);
    }
#else
    if (SDL_IsMainThread()) {
        (*function)(userdata);
    } else {
        SDL_RunOnMainThread((SDL_MainThreadCallback)function, userdata, true);
    }
#endif
}

/*-----------------------------------------------------------------------*/

#ifdef SIL_PLATFORM_MACOSX

/* The Cocoa misc.c and userdata code expect these. */
const char *macosx_resource_dir(void);
const char *macosx_resource_dir(void)
{
    return resource_dir ? resource_dir : ".";
}

/* Hooks called by the POSIX thread implementation (src/sysdep/posix/
 * thread.c); the Cocoa layer uses these to manage per-thread OpenGL
 * contexts, which we don't need. */
struct SysThread;
void posix_thread_runner_init(struct SysThread *thread);
void posix_thread_runner_cleanup(struct SysThread *thread);
void posix_thread_runner_init(UNUSED struct SysThread *thread) {}
void posix_thread_runner_cleanup(UNUSED struct SysThread *thread) {}

#endif  // SIL_PLATFORM_MACOSX

/*-----------------------------------------------------------------------*/

#ifdef SIL_PLATFORM_WINDOWS

/* The Windows sysdep files (src/sysdep/windows/{files,misc,userdata}.c)
 * expect these from main.c, which the SDL layer replaces.  The path is
 * returned in Windows format (backslash separators). */
const char *windows_executable_dir(void);
const char *windows_executable_dir(void)
{
    return resource_dir ? resource_dir : ".";
}

#endif  // SIL_PLATFORM_WINDOWS

/*************************************************************************/
/***************************** Entry point *******************************/
/*************************************************************************/

int main(int argc, char **argv_)
{
    const char **argv = (const char **)argv_;

    signal(SIGINT,  term_signal_handler);
    signal(SIGTERM, term_signal_handler);
#ifndef SIL_PLATFORM_WINDOWS  // Not supported by the Windows runtime.
    signal(SIGHUP,  term_signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    fpu_configure();

    const char *argv_dummy[2] = {"SIL", NULL};
    if (argc == 0 || !argv[0]) {
        argc = 1;
        argv = argv_dummy;
    }
#ifdef __APPLE__
    /* Strip the Finder's process serial number argument, if any. */
    if (argc > 1 && strncmp(argv[1], "-psn", 4) == 0) {
        argc = 1;
    }
#endif

    SDL_SetHint(SDL_HINT_APP_NAME, "SIL");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        DLOG("SDL_Init() failed: %s", SDL_GetError());
        return 2;
    }

    /* Resources live in the app bundle's Resources directory if we're
     * running from a bundle, else next to the executable.  (On Windows we
     * strip the trailing separator from the base path so that the result
     * can be used directly as a directory name.) */
    const char *base = SDL_GetBasePath();
    if (base) {
        const char *contents = strstr(base, ".app/Contents/");
        if (contents) {
            const size_t len = (contents - base) + 15;
            resource_dir = malloc(len + 10);
            if (resource_dir) {
                memcpy(resource_dir, base, len);
                strcpy(resource_dir + len, "Resources");
            }
        } else {
            resource_dir = strdup(base);
            if (resource_dir) {
                size_t len = strlen(resource_dir);
                while (len > 1 && (resource_dir[len-1] == '/'
                                || resource_dir[len-1] == '\\')) {
                    /* Don't strip the separator from a drive root ("C:\")
                     * or a UNC share root. */
                    if (len == 3 && resource_dir[1] == ':') {
                        break;
                    }
                    resource_dir[--len] = 0;
                }
            }
        }
    }

    const int exit_code = sil__main(argc, argv);

    SDL_Quit();
    free(resource_dir);
    resource_dir = NULL;
    return exit_code;
}

/*************************************************************************/
/*************************************************************************/
