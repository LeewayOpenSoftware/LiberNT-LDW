/*
 * PROJECT:     LiberNT WordPad
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Host (Win11 standalone) fill-ins for macros the LiberNT
 *              toolchain headers provide tree-wide; each is defined only
 *              when the real header did not
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _WORDPAD_HOST_SHIM_H_
#define _WORDPAD_HOST_SHIM_H_

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef RICHEDIT_CLASS20W
#define RICHEDIT_CLASS20W L"RichEdit20W"
#endif

#endif
