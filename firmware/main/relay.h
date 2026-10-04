// relay.h — Tesserae cloud-relay transport (mutually-exclusive remote-panel path).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
//
// Lets a panel live somewhere the home instance is not reachable from. Both ends
// talk *outbound* to a small Worker that is a per-device mailbox: home seals each
// rendered frame and PUTs it, the panel polls and decrypts it. Zero-knowledge --
// the relay holds ciphertext, two public keys and a token hash, never the frame
// key. This is an ALTERNATIVE to the direct REST/MQTT path: main.c prefers the
// relay when it is configured. Crypto lives in relay_crypto.[ch]; wire handling
// in relay_wire.[ch]; NVS state in config_store (the "relay" namespace).
#pragma once
#include "esp_system.h"   // esp_reset_reason_t
#include <stdbool.h>
#include <stdint.h>

#define RELAY_DEFAULT_URL "https://relay.tesserae.ink"

bool relay_ready(void);            // holds a complete mailbox identity + frame key
bool relay_pairing_pending(void);  // configured but not yet paired
// The relay answered this wake, including a pending revocation check.
bool relay_connection_ok(void);

typedef enum {
    RELAY_PAIR_IDLE = 0,   // nothing to do (no code, or already paired)
    RELAY_PAIR_WAITING,    // submitted; home has not completed it yet
    RELAY_PAIR_DONE,       // key derived and persisted; polling can start
    RELAY_PAIR_EXPIRED,    // code unknown/expired -- state cleared
    RELAY_PAIR_ERROR,      // transient (network/parse); safe to retry
} relay_pair_result_t;

// Advance pairing while the radio is up. Pending submissions reuse the saved
// key across retries and sleeps. Completed pairing is checked before resubmission.
relay_pair_result_t relay_pair_step(void);

// Full relay wake cycle: conditional frame fetch (staged into framebuf()),
// status POST (carrying a pending button, if any), config sync. Returns next_poll_s.
int relay_run_loop(const char *button, uint32_t button_event_id);

// Release the connection after pairing, mailbox requests and any button polling.
// Call before stopping WiFi, including when a request or pairing step failed.
void relay_end_wake(void);

// One conditional frame fetch into framebuf(); true if a NEW frame was staged
// (relay_pending_frame() then returns it). Used by the post-button window.
bool relay_poll_frame(void);

const uint8_t *relay_pending_frame(void);   // framebuf() if new this wake, else NULL
void relay_frame_painted(void);             // commit the relay ETag after a successful paint

// True once the relay answered a device-token route (frame/status/config) with 401
// on TWO consecutive wakes. Since server v0.240.0 a revoke deletes the token, so 401
// is unambiguous ("pairing gone"); 204 still means only "nothing published yet". Two
// wakes, not one, so a captive-portal/middlebox 401 can't unpair a working panel.
bool relay_pairing_revoked(void);

// Drop the stored pairing after relay_pairing_revoked() (keeps relay_url). Caller
// keeps the last image, shows setup, and a fresh code re-pairs cleanly.
void relay_forget_revoked_pairing(void);
