# MicroLink v2 — ESP32 Tailscale Client

Production-ready Tailscale VPN client for ESP32-S3 with WiFi and 4G cellular support.

## Features

- **Full Tailscale protocol**: Registration, WireGuard encryption, DERP relay, DISCO NAT traversal, STUN
- **WiFi + Cellular**: WiFi primary with automatic 4G cellular failback
- **PPP cellular data**: Real lwIP sockets over PPP — direct UDP, NAT hole-punching, sub-second latency
- **Multi-carrier support**: PAP (IMSI-based auth) and CHAP (credential-based auth), automatic fallback to AT socket bridge
- **Board support**: Waveshare SIM7600X + XIAO ESP32S3, LILYGO T-SIM7670G-S3
- **UDP API**: Simple send/receive over encrypted VPN tunnel
- **Large tailnets**: Tested with 300+ peers (PSRAM-backed 512KB buffers, NVS peer cache, proactive H2 WINDOW_UPDATE)
- **HTTP Config Server**: Web UI for runtime settings, peer allowlist, and device monitoring — no rebuild needed
- **DISCO Peer Filtering**: Allowlist controls which peers receive DISCO probes — reduces ping jitter on large tailnets
- **Credential security**: All secrets in git-ignored sdkconfig, never in source code

## Quick Start

```bash
# 1. Clone and enter example
git clone <repo_url>
cd examples/basic_connect    # or: cellular_connect, failover_connect, lilygo_sim7670g

# 2. Configure credentials
cp sdkconfig.credentials.example sdkconfig.credentials
# Edit sdkconfig.credentials with your WiFi/Tailscale/SIM values

# 3. Build and flash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Examples

| Example | Description | Hardware |
|---|---|---|
| `basic_connect` | WiFi → Tailscale → UDP echo + web config | Any ESP32-S3 |
| `cellular_connect` | 4G cellular → Tailscale → bidirectional UDP | XIAO + Waveshare SIM7600 |
| `failover_connect` | WiFi primary + cellular fallback | XIAO + Waveshare SIM7600 |
| `lilygo_sim7670g` | WiFi/Cellular failover (LILYGO pin config) | LILYGO T-SIM7670G-S3 |

## Cellular Performance

MicroLink uses PPP for cellular data, giving real lwIP sockets instead of routing through AT commands. This enables direct UDP, STUN, and NAT hole-punching — eliminating the DERP relay hop for peers on non-symmetric NATs.

### PPP vs AT Socket Bridge

| Metric | PPP (direct UDP) | AT Socket Bridge (DERP) |
|--------|-------------------|-------------------------|
| UDP round-trip | 300-600ms | 3-15s |
| ICMP ping | 400-700ms | 5-15s |
| Boot → connected | ~35-50s | ~60-90s |
| MapResponse (24KB) | ~8s | ~64s |
| Throughput @ 115200 | 6.5 KB/s | ~0.45 KB/s |
| Transport | Direct peer-to-peer UDP | DERP relay only |

### Carrier Compatibility

PPP authentication is automatic — CHAP with credentials when provided, PAP with empty credentials for IMSI-based carriers. Falls back to AT socket bridge if PPP fails.

| Carrier | APN | Auth | Status |
|---------|-----|------|--------|
| EIOT/BICS | `america.bics` | PAP (empty creds) | Tested |
| Soracom | `soracom.io` | CHAP (`sora`/`sora`) | Tested |
| Google Fi | `h2g2` | PAP (empty creds) | Untested |
| TEAL | `teal` | PAP (empty creds) | Untested |

### Bandwidth Contention

During large downloads (MapResponse parsing, delta updates), DISCO/STUN/WG probes continue with elevated RTTs (800-1800ms vs 500ms normal). Once the download completes, latency returns to normal. All protocol tasks run concurrently without blocking each other.

## HTTP Config Server

Enable `CONFIG_ML_ENABLE_CONFIG_HTTPD=y` in sdkconfig.defaults to get a web UI accessible at `http://<vpn-ip>/` from any device on your tailnet.

**System Monitor** — Real-time ESP32 temperature, WiFi RSSI (or cellular indicator), uptime, heap/PSRAM usage, DERP region, peer count, per-task stack watermarks. Auto-refreshes every 3 seconds.

**Peer Allowlist** — Manage which peers receive DISCO probes. Changes take effect immediately (no restart). On large tailnets (200+ devices), this limits DISCO probing to only the peers that matter.

**All Tailnet Peers** — Paginated peer list (25 per page) with search/filter, direct/DERP path status, and one-click "Allow" buttons. Peers not in the allowlist show a "Not Allowed" badge when filtering is active.

**Device Settings** — WiFi, Tailscale auth key, device name, cellular APN, plus advanced settings (max peers, DISCO heartbeat, priority peer, control plane host for Headscale/Ionscale, debug flags). All persist in NVS and take effect on restart.

**Resource usage** — ~28KB flash, ~7KB RAM (6KB HTTP task stack + 1KB NVS config). Ifdef-gated: zero cost when disabled (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`).

**REST API** — All functionality available via JSON endpoints (`/api/settings`, `/api/peers`, `/api/peers/allowed`, `/api/monitor`, `/api/status`, `/api/restart`).

**Device naming** — Set a prefix (e.g. `sensor`) to auto-generate `sensor-a1b2c3` from MAC, or set a full custom name. Tailscale creates the DNS entry: `your-name.your-tailnet.ts.net`.

## Architecture

MicroLink v2 uses a fully async, task-based architecture:

```
┌─────────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐
│ coord_task  │  │ derp_tx  │  │ net_io   │  │ wg_mgr  │
│ (Tailscale  │  │ (DERP    │  │ (select  │  │ (WG +   │
│  control)   │  │  relay)  │  │  loop)   │  │  DISCO) │
└──────┬──────┘  └────┬─────┘  └────┬─────┘  └────┬────┘
       │              │             │              │
       └──────────────┴─────────────┴──────────────┘
                          │
                    Queue-based IPC
                          │
                ┌─────────┴─────────┐
                │   WiFi / Cellular  │
                │  (ml_net_switch)   │
                └───────────────────┘
```

No polling (`microlink_update()`) needed — all tasks run independently.

## API

```c
// Initialize
microlink_t *ml = microlink_init(&config);
microlink_start(ml);

// UDP communication
microlink_udp_socket_t *sock = microlink_udp_create(ml, 9000);
microlink_udp_send(sock, dest_ip, dest_port, data, len);
microlink_udp_recv(sock, &src_ip, &src_port, buf, &len, timeout_ms);

// Network switching (WiFi + cellular failover)
ml_net_switch_init(&ns_config);
ml_net_switch_start();
microlink_t *ml = ml_net_switch_get_handle();

// Cleanup
microlink_stop(ml);
microlink_destroy(ml);
```

## Configuration

All settings via `idf.py menuconfig` → MicroLink V2:

- **Credentials**: WiFi SSID/password, Tailscale auth key, SIM PIN
- **Cellular Modem**: Board selection (Waveshare/LILYGO/Custom), UART pins, APN, PPP credentials
- **Network Switching**: WiFi timeout, health check interval, failback interval
- **Performance**: Zero-copy WG mode, max peers (1-64), NVS cache size (16-1024)
- **HTTP Config Server**: Enable/disable web UI, max allowed peers for DISCO filter
- **Buffers**: H2 buffer (64-2048 KB), JSON buffer (64-2048 KB) for large tailnets

Settings can also be changed at runtime via the HTTP config server web UI (no rebuild required).

## Documentation

- [TESTING_GUIDE.md](TESTING_GUIDE.md) — Hardware setup, build, flash, test procedures
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — System design and task model
- [docs/LARGE_TAILNET.md](docs/LARGE_TAILNET.md) — Scaling considerations for 300+ peers
- [docs/DESIGN.md](docs/DESIGN.md) — V1→V2 redesign rationale
- [docs/TAILSCALE_REFERENCE.md](docs/TAILSCALE_REFERENCE.md) — Tailscale protocol reference patterns
- [docs/V1_POSTMORTEM.md](docs/V1_POSTMORTEM.md) — V1 bug analysis and lessons learned
