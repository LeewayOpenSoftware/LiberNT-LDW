/*
 * PROJECT:     LiberNT Cache Manager
 * FILE:        ntoskrnl/cc/include/ccdef.h
 * PURPOSE:     Cache manager types and constants
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#define CC_VIEW_SHIFT               18
#define CC_VIEW_SIZE                ((ULONG)1 << CC_VIEW_SHIFT)
#define CC_VIEW_MASK                (CC_VIEW_SIZE - 1)
#define CC_VIEW_PAGES               (CC_VIEW_SIZE >> PAGE_SHIFT)

#define CC_LEAF_SHIFT               4
#define CC_LEAF_SLOTS               (1u << CC_LEAF_SHIFT)
#define CC_LEAF_MASK                (CC_LEAF_SLOTS - 1)

#define CC_LRU_SHARDS               8
#define CC_MIN_VIEWS                64
#define CC_MAX_VIEWS                8192

#define CC_READ_AHEAD_DEFAULT       (64 * 1024)
#define CC_READ_AHEAD_MAX           (8 * 1024 * 1024)
#define CC_SEQUENTIAL_HITS          2

#define CC_LAZY_DIVISOR             8
#define CC_LAZY_MIN_PAGES           16
/* A map whose oldest dirty data is this old, or whose last handle is gone,
 * is written back completely on the next lazy pass, budget or not. */
#define CC_LAZY_MAX_AGE_SECONDS     4
/* Pages the age-out may write in one pass; what is left waits for the next. */
#define CC_LAZY_AGED_MAX_PAGES      2048
/* A cached write that takes a map's dirty page count across this many
 * pages wakes the lazy writer instead of waiting for its tick. */
#define CC_LAZY_KICK_PAGES          64

C_ASSERT(CC_VIEW_PAGES <= 64);
C_ASSERT((CC_LRU_SHARDS & (CC_LRU_SHARDS - 1)) == 0);
