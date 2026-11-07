// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef MODAVD_DOWNLOAD_BATCH_H_
#define MODAVD_DOWNLOAD_BATCH_H_

// Called before guest/application code in a newly initialized app process.
// A nonzero errno refuses initialization. Active mode and startup recovery
// require a fresh modavd.download.session nonce armed by the isolated controller
// after force-stop and package-wide quiescence. The nonce records that external
// contract; this library cannot itself establish package-wide exclusivity.
extern "C" __attribute__((visibility("default")))
int modavd_download_sync_init(const char* package, const char* private_dir);

#endif  // MODAVD_DOWNLOAD_BATCH_H_
