/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WSK stream and datagram echo over loopback, declarations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _KMTEST_WSK_ECHO_H_
#define _KMTEST_WSK_ECHO_H_

#define IOCTL_WSKECHO_RUN 1
#define WSKECHO_STREAM_BYTES 65536
#define WSKECHO_LARGE_STREAM_BYTES (1024 * 1024)
#define WSKECHO_DATAGRAM_BYTES 1024
#define WSKECHO_CAPTURE_TIMEOUT_MS 10000
#define WSKECHO_IRP_TIMEOUT_MS 15000

#endif
