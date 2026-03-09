# MicroLink v2 Architecture Design

## Overview

MicroLink v2 is a complete rearchitecture of the MicroLink ESP32 Tailscale client.
The protocol implementation (Noise IK handshake, HTTP/2 framing, DISCO wire format,
WireGuard integration) carries over from v1. What changes is how the pieces connect:
blocking calls and mutexes are replaced with FreeRTOS queues and dedicated tasks,
modeled after the native Tailscale Go client (tailscaled).

**Reference code:** `../tailscale-reference/tailscale/`

---

## What Went Wrong with v1

### Root Cause: Single-Threaded Architecture with Blocking I/O

MicroLink v1 runs almost everything on a single main loop (`microlink_update()`).
DERP sends, DISCO probes, STUN queries, WireGuard handshakes, and control plane
operations all compete for the same execution context. Any blocking call freezes
everything else.

### Critical Bugs (System Killers)

#### 1. DERP TLS Mutex Starvation
**v1 file:** `microlink_derp.c`
**Tailscale reference:** `derp/derphttp/derphttp_client.go`, `wgengine/magicsock/derp.go`

v1 uses a single `derp_tls_mutex` protecting both TLS reads and writes. When
`microlink_derp_receive()` holds the mutex during a payload read (up to 10s timeout
in the original code, reduced to 100ms as a band-aid), all DERP sends are blocked.
With 9+ peers doing simultaneous DISCO and WireGuard handshakes, the send side
starves completely.

Tailscale solves this with a **dedicated writer goroutine per DERP region** that drains
a 32-deep channel. Reads happen on a separate goroutine. No mutex needed between
read and write paths.

#### 2. STUN Blocks Main Loop for 18 Seconds
**v1 file:** `microlink_stun.c`
**Tailscale reference:** `net/netcheck/netcheck.go`

v1's STUN probe uses synchronous `recvfrom()` with a 3-second timeout, 3 retries,
across 2 servers sequentially. Worst case: 18 seconds of total system freeze. During
this time, no DERP packets are processed, no DISCO responses sent, no state machine
progress.

Tailscale runs STUN probes asynchronously with 100ms retransmit intervals and a
5-second total timeout. Responses are matched by TxID via callbacks.

#### 3. Heartbeat Stream=false Kills Long-Poll
**v1 file:** `microlink_coordination.c`

The v1 heartbeat sends a MapRequest with `Stream=false` on a connection established
with `Stream=true`. Per Tailscale protocol (capver >= 68), this causes the server to
close the connection. Once STUN started working (via Google fallback), the conditional
skip no longer triggered, and the heartbeat killed the long-poll every time.

Tailscale uses three separate goroutines for auth, map polling, and endpoint updates.
Endpoint updates go through the `updateRoutine`, never touching the long-poll stream.

#### 4. GOAWAY 30-Second vTaskDelay
**v1 file:** `microlink_connection.c`

When the server sends GOAWAY, v1 calls `vTaskDelay(pdMS_TO_TICKS(backoff_ms))` with
up to 30 seconds of backoff. This freezes the entire main loop.

Tailscale uses non-blocking exponential backoff (30s max) in each independent goroutine.

#### 5. Coordination Registration Blocks 30+ Seconds
**v1 file:** `microlink_coordination.c`

The entire registration flow (DNS, TCP connect, HTTP upgrade, Noise handshake, HTTP/2
preface, RegisterRequest, response) runs synchronously in one giant function. No DERP
or DISCO processing happens during this time.

#### 6. Nonce Race Condition
**v1 file:** `microlink_coordination.c`

`tx_nonce` (uint64_t) is accessed from both Core 0 (heartbeat) and Core 1 (poll task).
64-bit operations are NOT atomic on ESP32. Concurrent increment = nonce collision =
broken Noise encryption.

### High-Severity Issues

#### 7. No DISCO Rate Limiting
**v1 file:** `microlink_disco.c`
**Tailscale reference:** `wgengine/magicsock/endpoint.go`

v1 has zero rate limiting on DISCO. Every peer's CallMeMaybe triggers immediate
parallel pings to ALL endpoints. With 9 peers, this creates a thundering herd of
DISCO packets that overwhelm DERP and the ESP32's network stack.

Tailscale enforces:
- 5s minimum between pings to same endpoint
- 3s heartbeat interval
- 6.5s trust window for confirmed paths
- 1-minute interval for path upgrade attempts

#### 8. 825 Log Statements (58 log_hex in Handshake)
Excessive ESP_LOGI calls flood UART, disrupt timing, and waste CPU. The `log_hex()`
function is called 58 times during the Noise handshake alone. Private key material
is logged at INFO level (security vulnerability).

#### 9. coord_poll Task at MAX_PRIORITY
The coordination poll task runs at `configMAX_PRIORITIES - 1`, which is higher than
WiFi driver tasks. This can starve WiFi on Core 1.

#### 10. Peer Array Accessed Without Synchronization
The DISCO task iterates `ml->peers[]` while the main task can modify it during
MapResponse parsing. No lock protects this shared state.

#### 11. IPv6 Socket Warning Spam
`disco_send_udp6()` logs a warning every time it's called without an IPv6 socket.
Since DISCO probes fire for every peer endpoint including IPv6 ones, this floods
the log continuously.

#### 12. WireGuard Peer Limit Mismatch
`MICROLINK_MAX_PEERS = 16` but `wireguard-lwip`'s internal `WIREGUARD_MAX_PEERS`
may default to a smaller value, causing "Failed to add peer: lwIP error -1".

#### 13. wireguardif_tmr Use-After-Free
v1 frees the wireguard_device struct while the 400ms timer callback may still be
in-flight, causing a use-after-free crash on deinit.

---

## How v2 Solves Everything

### Core Principle: Dedicated Tasks + Queue-Based Communication

Every subsystem gets its own FreeRTOS task. Communication happens through queues,
never through shared state with mutexes. This eliminates mutex contention, blocking
cascades, and race conditions by design.

### Task Architecture

```
Core 0 (Network I/O)                    Core 1 (Protocol Logic)
+-------------------------+             +-------------------------+
|  TASK: net_io           |             |  TASK: coord            |
|  Priority: 6            |             |  Priority: 5            |
|  Stack: 6KB             |             |  Stack: 12KB            |
|                         |             |                         |
|  - Single select() on   | --queues--> |  - Noise handshake      |
|    ALL sockets (DERP,   |             |  - Registration         |
|    DISCO, STUN)         | <--queues-- |  - Long-poll recv       |
|  - Classify packets:    |             |  - MapResponse parse    |
|    DISCO -> disco_queue |             |  - Endpoint updates     |
|    WG   -> wg_queue     |             +-------------------------+
|    STUN -> stun_queue   |
|  - DERP TLS read (sole  |             +-------------------------+
|    reader)              |             |  TASK: wg_mgr           |
|                         |             |  Priority: 7            |
|  TASK: derp_tx          |             |  Stack: 8KB             |
|  Priority: 7            |             |                         |
|  Stack: 10KB            |             |  - Peer add/remove      |
|                         |             |  - Handshake mgmt       |
|  - Drains derp_tx_queue | <--queue--- |  - Timer scheduling     |
|  - TLS write (sole      |             |  - Inject DERP packets  |
|    writer)              |             +-------------------------+
|  - Backpressure: drop   |
|    oldest if full       |             Unpinned
|                         |             +-------------------------+
+-------------------------+             |  TASK: app              |
                                        |  Priority: 3            |
                                        |  Stack: 4KB             |
                                        |                         |
                                        |  - User callbacks       |
                                        |  - UDP send/recv API    |
                                        |  - Status/health        |
                                        +-------------------------+
```

### Queue Inventory

| Queue | Depth | Item Size | Producer(s) | Consumer |
|-------|-------|-----------|-------------|----------|
| `derp_tx_queue` | 16 | ptr+len+key | coord, disco, wg_mgr | derp_tx task |
| `disco_rx_queue` | 8 | ptr+len+src | net_io task | wg_mgr task |
| `wg_rx_queue` | 8 | ptr+len+src | net_io task | wg_mgr task |
| `stun_rx_queue` | 4 | ptr+len | net_io task | coord task |
| `coord_cmd_queue` | 4 | command enum | app, wg_mgr | coord task |
| `peer_update_queue` | 4 | peer_info ptr | coord task | wg_mgr task |

### Event Group: System State

```c
#define EVT_WIFI_CONNECTED    BIT0
#define EVT_COORD_REGISTERED  BIT1
#define EVT_DERP_CONNECTED    BIT2
#define EVT_WG_READY          BIT3
#define EVT_PEERS_AVAILABLE   BIT4
#define EVT_STUN_COMPLETE     BIT5
```

Tasks wait on event bits instead of polling flags. State transitions are atomic
and visible across cores immediately (event groups use critical sections internally).

### Module Redesign Details

#### DERP Client (derp_tx + net_io read side)

**Tailscale reference:** `derp/derphttp/derphttp_client.go`, `wgengine/magicsock/derp.go`

v1 problem: Single mutex for read+write, blocking reads starve sends.

v2 solution:
- `derp_tx` task: Sole owner of `mbedtls_ssl_write()`. Blocks on `derp_tx_queue`.
  When a packet arrives, it writes the DERP frame (5-byte header + payload) via TLS.
  Backpressure: if queue is full, dequeue+drop oldest, try 3 times, then drop new.
- `net_io` task: Sole owner of `mbedtls_ssl_read()`. Reads in the unified select()
  loop. When DERP socket is readable, reads frame header, then payload. Classifies
  and routes to appropriate queue.
- **No mutex between read and write.** Each has exclusive ownership of their TLS
  direction.

Connection management:
- Connect/reconnect is initiated by `coord` task via `coord_cmd_queue`.
- `derp_tx` detects write failures and signals reconnect via event group.
- `net_io` detects read failures (EOF, timeout > 120s) and signals reconnect.
- Reconnect: both tasks pause, `coord` task performs the TLS handshake on a
  temporary context, then atomically swaps the SSL context pointer.

#### Coordination (coord task)

**Tailscale reference:** `control/controlclient/auto.go`, `control/controlclient/direct.go`

v1 problem: Registration blocks 30s, heartbeat kills long-poll, nonce race.

v2 solution:
- Runs entirely on Core 1 in its own task with 12KB stack (enough for Noise + TLS).
- Owns ALL Noise protocol state (tx_key, rx_key, tx_nonce, rx_nonce). No other task
  touches these. Eliminates nonce race condition.
- Registration is a state machine with non-blocking steps:
  1. DNS resolve (async via netconn API or getaddrinfo with timeout)
  2. TCP connect (non-blocking with select)
  3. Noise handshake (chunked into message exchanges)
  4. HTTP/2 preface
  5. RegisterRequest send
  6. Response receive (with timeout, not blocking other tasks)
- Long-poll: After registration, sends Stream=true MapRequest on one HTTP/2 stream.
  Endpoint updates use a SEPARATE stream (never Stream=false on the long-poll stream).
- Watchdog: 120-second timer. If no data received, cancel and reconnect.
- Backoff: Exponential, 30s max, per-attempt. State change, not vTaskDelay.

#### DISCO (integrated into wg_mgr + net_io)

**Tailscale reference:** `wgengine/magicsock/endpoint.go`

v1 problem: No rate limiting, thundering herd, peer array race.

v2 solution:
- DISCO receive: `net_io` task reads from UDP socket in select() loop, routes to
  `disco_rx_queue`. `wg_mgr` task processes them.
- DISCO send: `wg_mgr` task sends DISCO packets via `derp_tx_queue` (for DERP relay)
  or directly via UDP `sendto()` (for direct probes). No contention since `wg_mgr`
  is the only task sending direct UDP.
- Rate limiter per peer:
  ```c
  typedef struct {
      uint64_t last_ping_ms;        // Last DISCO ping sent
      uint64_t last_pong_ms;        // Last DISCO pong received
      uint64_t trust_until_ms;      // Path trusted until this time
      uint64_t last_upgrade_ms;     // Last path upgrade attempt
      uint8_t  active_probes;       // Currently outstanding pings
  } disco_peer_state_t;
  ```
- Timing constants (from tailscaled):
  - `DISCO_PING_INTERVAL_MS = 5000` (min between pings to same endpoint)
  - `DISCO_HEARTBEAT_MS = 3000` (heartbeat to active peers)
  - `DISCO_TRUST_DURATION_MS = 6500` (trust confirmed path)
  - `DISCO_UPGRADE_INTERVAL_MS = 60000` (try path upgrade)
  - `DISCO_PING_TIMEOUT_MS = 5000` (pong wait timeout)
- Peer state is owned exclusively by `wg_mgr` task. No shared access, no lock needed.
- IPv6: Check socket validity at init time. If no IPv6 socket, set a flag and never
  attempt IPv6 sends. Zero log spam.

#### STUN (async, non-blocking)

**Tailscale reference:** `net/netcheck/netcheck.go`

v1 problem: Blocking recvfrom with 3s timeout, 3 retries, 2 servers = 18s freeze.

v2 solution:
- STUN socket is included in `net_io`'s select() fd_set.
- `coord` task sends STUN binding request via UDP (non-blocking sendto).
- `net_io` task receives STUN response, routes to `stun_rx_queue`.
- `coord` task matches TxID and processes result.
- Timeout: 500ms retransmit, 3s total, single server at a time.
- Total worst case: 3s (not 18s). And non-blocking (other tasks continue).

#### WireGuard (wg_mgr task)

v1 problem: wireguardif_tmr use-after-free, peer limit mismatch, DERP output from
lwIP thread with insufficient stack.

v2 solution:
- `wg_mgr` task owns all peer management (add, remove, update endpoint).
- `wireguardif_shutdown()` called before freeing device (proper timer cancellation).
- `WIREGUARD_MAX_PEERS` configured to match `MICROLINK_MAX_PEERS` at compile time.
- DERP output callback queues to `derp_tx_queue` instead of attempting TLS write
  from lwIP thread. Always queue, never direct send.
- Peer map: Use first 4 bytes of public key as hash for O(1) lookup instead of
  O(n) linear scan with 32-byte memcmp.

#### Connection State Machine

v1 problem: GOAWAY blocks with vTaskDelay(30s), static variables persist across
reconnect cycles.

v2 solution:
- State machine runs in `coord` task (not in the main loop).
- States: INIT -> WIFI_WAIT -> STUN_PROBE -> REGISTERING -> FETCHING_PEERS ->
  CONNECTED -> LONG_POLL -> RECONNECTING
- All state is in the task's local struct, reset on reconnect. No static variables.
- Backoff: Counter incremented on failure, delay achieved by waiting on a queue
  with timeout (allowing other commands to interrupt the wait).
- GOAWAY: Sets state to RECONNECTING, resets backoff counter. No delay.

### Logging Strategy

**Production:**
- Compile-time max: `CONFIG_LOG_MAXIMUM_LEVEL_WARN`
- Zero `log_hex()` calls
- Zero per-packet logging
- Private keys NEVER logged at any level
- UART output via CP2102 (not USB Serial/JTAG)

**Debug builds:**
- `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`
- Async log buffer task (lowest priority)
- Per-module runtime level control

### Memory Layout

| Buffer | Size | Location | Owner |
|--------|------|----------|-------|
| DERP RX frame | 1600B | PSRAM | net_io task |
| DERP TX queue items | 16 x 1600B | PSRAM | derp_tx task |
| Coordination buffer | 64KB | PSRAM | coord task |
| H2/TCP/JSON buffers | 1MB each | PSRAM | coord task (during fetch_peers only) |
| DISCO RX buffer | 256B | stack | net_io task |
| STUN RX buffer | 128B | stack | net_io task |
| Peer array | 16 x ~320B | PSRAM | wg_mgr task |
| cJSON allocations | variable | PSRAM (via hooks) | coord task |

Total PSRAM usage: ~3.1MB peak during MapResponse parsing, ~200KB steady state.
ESP32-S3 with 8MB PSRAM has plenty of headroom.

### Thermal Management

- WiFi TX power: Reduce from 19.5dBm to 13dBm (`esp_wifi_set_max_tx_power(52)`)
- Idle delays: All tasks block on queues when idle (no busy-wait loops)
- STUN probe: Max 3s total (not 18s of CPU-bound polling)
- CPU frequency: Enable DFS (240MHz active, 80MHz idle)

### sdkconfig Recommendations

```
# lwIP
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_SO_LINGER=y

# mbedTLS
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_HARDWARE_SHA=y
CONFIG_MBEDTLS_HARDWARE_AES=y
CONFIG_MBEDTLS_HARDWARE_MPI=y

# Logging
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# Power
CONFIG_PM_ENABLE=y

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
```

---

## File Structure

```
microlink-v2/
  components/
    microlink/
      include/
        microlink.h              -- Public API (same as v1)
        microlink_internal.h     -- Internal types, queue handles, event bits
      src/
        microlink.c              -- Init/deinit, public API wrappers
        ml_coord.c               -- Coordination task (Noise, HTTP/2, control plane)
        ml_derp.c                -- DERP TX task + connection management
        ml_net_io.c              -- Unified network I/O task (select loop)
        ml_wg_mgr.c              -- WireGuard + DISCO + peer management task
        ml_stun.c                -- Async STUN (send from coord, recv in net_io)
        ml_noise.c               -- Noise IK protocol (extracted, clean)
        ml_h2.c                  -- Minimal HTTP/2 framing (extracted, clean)
        nacl_box.c               -- NaCl crypto (carried from v1)
        nacl_box.h
        x25519.c                 -- Curve25519 (carried from v1)
        x25519.h
      components/
        wireguard_lwip/          -- Symlink or copy from v1
      CMakeLists.txt
  docs/
    DESIGN.md                    -- This document
    V1_POSTMORTEM.md             -- Detailed v1 failure analysis
    TAILSCALE_REFERENCE.md       -- Key patterns from tailscaled source
  examples/
    basic_connect/
      main/
        main.c
        Kconfig.projbuild
      CMakeLists.txt
      sdkconfig.defaults
  CMakeLists.txt
```

---

## Implementation Order

1. **Core framework:** Task creation, queues, event groups, public API shell
2. **Noise protocol:** Extract and clean up from v1 (ml_noise.c)
3. **HTTP/2 framing:** Extract and clean up from v1 (ml_h2.c)
4. **Coordination task:** Registration state machine, MapResponse parsing
5. **DERP client:** TX task + read integration in net_io
6. **Net I/O task:** Unified select() loop
7. **WG manager task:** Peer management, DISCO with rate limiting
8. **STUN:** Async probe integrated with net_io
9. **Connection state machine:** In coord task
10. **Example app:** basic_connect with Kconfig credentials

Each step builds on the previous. The system becomes functional at step 6 (can
register and receive peer data). Full DISCO/WireGuard comes online at step 7.
