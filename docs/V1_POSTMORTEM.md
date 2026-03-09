# MicroLink v1 Postmortem

## Summary

MicroLink v1 successfully implements the Tailscale protocol on ESP32-S3 - Noise IK
handshake, HTTP/2 over Noise, DISCO NAT traversal, WireGuard tunneling, and DERP
relay. It connects to a real Tailscale tailnet and exchanges data. However, the
system becomes unstable under real-world conditions with multiple peers, freezing
within 30-60 seconds of connecting to a tailnet with 9+ devices.

The root cause is architectural: a single-threaded main loop with blocking I/O and
shared mutexes, where any blocking operation freezes all other subsystems.

## Timeline of Discovery

1. System connects successfully to tailnet with 9-10 peers
2. DISCO discovery begins - all peers send CallMeMaybe simultaneously
3. WireGuard handshakes queue up for multiple peers
4. DERP TLS mutex becomes contended (reads block sends)
5. System appears to "freeze" - serial output stops
6. Initially attributed to USB Serial/JTAG 64-byte FIFO limitation
7. Confirmed NOT a serial issue by testing with external CP2102 UART bridge
8. Root cause identified: DERP mutex starvation + heartbeat killing long-poll

## Bug Inventory

### Bug 1: DERP TLS Mutex Starvation (CRITICAL)

**File:** `microlink_derp.c:960`
**Impact:** System deadlock with multiple peers

The DERP payload read used `DERP_CONNECT_TIMEOUT_MS` (10 seconds) instead of
`DERP_READ_TIMEOUT_MS` (100ms). While holding the TLS mutex for 10 seconds to
read a payload, ALL DERP sends were blocked - CallMeMaybe responses, DISCO PONGs,
WireGuard handshake initiations. With 6-7 peers doing simultaneous discovery,
this caused a cascade of timeouts and dropped packets.

**Band-aid fix:** Changed timeout to 100ms. Reduced but didn't eliminate the
fundamental single-mutex design flaw.

### Bug 2: Heartbeat Stream=false Kills Long-Poll (CRITICAL)

**File:** `microlink_coordination.c` (heartbeat function)
**Impact:** Server closes connection every heartbeat cycle

The v1.3.0 heartbeat sends a MapRequest with `Stream=false, OmitPeers=true` on
the same HTTP/2 connection that has an active `Stream=true` long-poll. Per
Tailscale protocol (capver >= 68), `Stream=false` causes the server to close
the connection.

Previously this was masked because STUN failed (no Google fallback), so
`ml->stun.public_ip == 0` and the heartbeat was skipped. Once STUN started
working via the Google fallback server, the heartbeat fired and killed the
connection every time.

**Fix:** Disabled heartbeat entirely. Long-poll maintains online status.

### Bug 3: STUN Blocking (18 seconds worst case)

**File:** `microlink_stun.c:267-320`
**Impact:** Total system freeze during STUN probe

Synchronous `recvfrom()` with 3-second timeout, 3 retries, 2 servers:
- Primary (derp1.tailscale.com:3478): up to 9 seconds
- Fallback (stun.l.google.com:19302): up to 9 seconds
- Total: 18 seconds where nothing else runs

This contributed to heat generation (CPU in tight recv loop) and made the
system appear unresponsive.

### Bug 4: GOAWAY 30-Second Delay

**File:** `microlink_connection.c:242-249`
**Impact:** 30-second total system freeze

```c
uint32_t backoff_ms = 1000 << (reconnect_attempts > 4 ? 4 : reconnect_attempts);
if (backoff_ms > 30000) backoff_ms = 30000;
vTaskDelay(pdMS_TO_TICKS(backoff_ms));
```

### Bug 5: Nonce Race Condition

**File:** `microlink_coordination.c`
**Impact:** Broken Noise encryption (security vulnerability)

`tx_nonce` (uint64_t) accessed from Core 0 (heartbeat) and Core 1 (poll task)
without proper synchronization. 64-bit operations are not atomic on ESP32.

### Bug 6: No DISCO Rate Limiting

**File:** `microlink_disco.c`
**Impact:** Network flooding, DERP congestion

Every CallMeMaybe triggers immediate parallel pings to ALL endpoints for ALL
peers simultaneously. No minimum interval between pings. No backoff.

### Bug 7: Private Key Logged

**File:** `microlink_derp.c:435-439`
**Impact:** Security vulnerability

```c
ESP_LOGI(TAG, "Our private key (first 8): %02x%02x...",
         ml->wireguard.private_key[0], ...);
```

8 bytes of WireGuard private key logged at INFO level.

### Bug 8: 825 Log Statements

**Impact:** UART flooding, timing disruption, heat

58 `log_hex()` calls in Noise handshake alone. Per-packet logging in DERP
receive and DISCO. At 115200 baud, a single 80-char log line takes ~7ms to
transmit. 58 calls = ~400ms of UART blocking during handshake.

### Bug 9: wireguardif_tmr Use-After-Free

**File:** `microlink_wireguard.c:348-351`
**Impact:** Crash on deinit

Timer callback fires after device struct is freed. Fixed by calling
`wireguardif_shutdown()` before freeing, reducing delay from 500ms to 100ms.

### Bug 10: Buffer Overflow on Large Tailnets

**File:** `microlink_coordination.c`
**Impact:** Truncated MapResponse, parse failure

32KB buffers insufficient for 300+ device tailnets (28KB+ JSON responses).
Fixed by bumping to 1MB PSRAM buffers.

### Bug 11: cJSON Internal Heap Exhaustion

**File:** `microlink_coordination.c`
**Impact:** Parse failure on large responses

cJSON allocations went to internal SRAM (due to SPIRAM_MALLOC_ALWAYSINTERNAL
threshold). Large MapResponse parsing exhausted 320KB internal heap. Fixed by
routing cJSON to PSRAM via `cJSON_InitHooks()`.

### Bug 12: Format String Crash

**File:** `microlink_coordination.c:2990`
**Impact:** Potential crash

`%td` (ptrdiff_t) format specifier not supported by ESP32 newlib nano.

## What v1 Did Well

- Correct Noise IK handshake implementation
- Working HTTP/2 framing over Noise
- DISCO protocol (ping/pong/CallMeMaybe) wire format correct
- WireGuard integration via wireguard-lwip
- NVS key persistence across reboots
- PSRAM utilization for large buffers
- Dual-core split (coord poll on Core 1)
- DERP frame protocol implementation

## Lessons for v2

1. **Never hold a mutex across I/O.** Use queues with dedicated writer tasks.
2. **Never block the main loop.** All I/O must be non-blocking or on dedicated tasks.
3. **Rate limit everything.** Copy tailscaled's timing constants exactly.
4. **Own state on one task.** If only one task touches a variable, no lock needed.
5. **Log sparingly.** Production builds should emit < 10 lines/second.
6. **Never log secrets.** Not even partial key bytes.
7. **Test with many peers.** Single-peer testing hid all concurrency bugs.
