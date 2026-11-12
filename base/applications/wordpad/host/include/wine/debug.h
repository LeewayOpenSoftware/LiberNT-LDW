/*
 * PROJECT:     LiberNT WordPad
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Host (Win11 standalone) shim for Wine debug macros; the real
 *              <wine/debug.h> only exists in the LiberNT build tree
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _WINE_DEBUG_HOST_SHIM_H_
#define _WINE_DEBUG_HOST_SHIM_H_

#define WINE_TRACE(...) ((void)0)
#define WINE_FIXME(...) ((void)0)
#define WINE_WARN(...)  ((void)0)
#define WINE_ERR(...)   ((void)0)

#define WINE_DEFAULT_DEBUG_CHANNEL(channel) /* host shim: no debug channel */

static inline const char *wine_dbgstr_guid(const void *guid)
{
    (void)guid;
    return "{guid}";
}

#endif
