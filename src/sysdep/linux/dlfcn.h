/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/linux/dlfcn.h: Wrapper for system header <dlfcn.h>.
 */

#ifndef SIL_SRC_SYSDEP_LINUX_DLFCN_H
#define SIL_SRC_SYSDEP_LINUX_DLFCN_H

#include <dlfcn.h>

/* In glibc before version 2.36, these constants are protected by
 * _GNU_SOURCE, which we don't want to enable globally, so we define the
 * constants manually. */
#ifndef RTLD_DEFAULT
# define RTLD_DEFAULT  NULL
# define RTLD_NEXT     ((void *)(intptr_t)-1)
#endif

#endif  // SIL_SRC_SYSDEP_LINUX_DLFCN_H
