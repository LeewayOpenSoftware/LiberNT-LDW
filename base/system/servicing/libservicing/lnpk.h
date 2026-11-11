/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LNPK: the standalone package .exe container (S1-PACKAGE-EXE-DESIGN.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A package .exe is a launcher stub followed by an overlay that is the S1 package directory serialized:
 * the entry bytes (manifest.json, manifest.sig, catalog.cat, payload\...), then a table of contents, then
 * a 64-byte trailer at the very end of the file. Everything is little-endian. The container carries, it
 * does not vouch: trust comes from manifest.sig and the payload hashes inside, verified by libservicing
 * exactly as for a directory package. The TOC hashes only catch a corrupt or truncated download early.
 * The stub (base/applications/libernt-update-package) shares this header and nothing else.
 */
#pragma once
#include <stdint.h>

#define LNPK_MAGIC          "LNPK"
#define LNPK_VERSION        1u
#define LNPK_TRAILER_SIZE   64u
#define LNPK_MAX_ENTRIES    4100u            /* LN_MAX_ITEMS payloads + manifest, signature, catalog */
#define LNPK_MAX_NAME       520u             /* utf-8 bytes of a relative entry name */
#define LNPK_MAX_TOC        (LNPK_MAX_ENTRIES * (50u + LNPK_MAX_NAME))

#pragma pack(push, 1)
typedef struct _LNPK_TRAILER
{
    char magic[4];                           /* "LNPK" */
    uint32_t version;                        /* LNPK_VERSION */
    uint64_t overlayOffset;                  /* file offset of the first entry's bytes (end of the stub) */
    uint64_t tocOffset;                      /* file offset of the table of contents */
    uint32_t tocCount;                       /* entries in the table */
    uint32_t reserved;                       /* 0 */
    uint8_t tocSha256[32];                   /* SHA-256 of the table bytes (tocOffset .. trailer) */
} LNPK_TRAILER;

/* One table entry: this fixed head, then nameLength bytes of utf-8 name ("manifest.json", "payload\f0.bin") */
typedef struct _LNPK_ENTRY_HEAD
{
    uint64_t offset;                         /* absolute file offset of the bytes */
    uint64_t size;
    uint8_t sha256[32];
    uint16_t nameLength;
} LNPK_ENTRY_HEAD;
#pragma pack(pop)
