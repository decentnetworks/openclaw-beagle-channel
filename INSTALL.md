# Beagle Channel Installation Guide

Complete guide for compiling and installing OpenClaw's Beagle Chat channel, including the native Elastos Carrier SDK.

## Source Repositories

Clone these two repositories to get started:

```bash
# 1. Elastos Carrier Native SDK (the P2P networking layer, with offline replay fix)
git clone https://github.com/decentnetworks/Elastos.NET.Carrier.Native.SDK.git

# 2. OpenClaw Beagle Channel (the OpenClaw integration)
git clone https://github.com/decentnetworks/openclaw-beagle-channel.git
```

## Overview

The Beagle Chat integration consists of two parts:

1. **beagle-sidecar** — C++ daemon that bridges OpenClaw to the Elastos Carrier P2P network
2. **beagle-channel** — TypeScript OpenClaw plugin that communicates with the sidecar

The hardest part is building the Elastos Carrier SDK, which beagle-sidecar depends on.

---

## Prerequisites

### System Packages (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libssl-dev \
    zlib1g-dev \
    libcurl4-openssl-dev \
    libsodium-dev \
    libconfig-dev \
    libsqlite3-dev \
    autoconf \
    automake \
    libtool \
    libncurses5-dev \
    git
```

### Node.js + npm

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

Verify:
```bash
cmake --version    # >= 3.16
node --version     # >= 18
npm --version
```

---

## Step 1: Build Elastos Carrier SDK

This is the foundational dependency. The SDK provides P2P mesh networking over the Elastos Carrier protocol.

### Clone the SDK

```bash
cd ~/devs
git clone https://github.com/decentnetworks/Elastos.NET.Carrier.Native.SDK.git
cd Elastos.NET.Carrier.Native.SDK
git checkout fix/express-offline-watermark-replay
```

> **Note:** We currently use `decentnetworks/fix/express-offline-watermark-replay` to include the offline replay fix in Express. Upstream PR: `https://github.com/0xli/Elastos.NET.Carrier.Native.SDK/pull/3`.

### Configure and Build

```bash
mkdir -p build/linux
cd build/linux
sudo apt-get install libncurses5-dev

cmake ../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SHARED=ON \
    -DENABLE_STATIC=ON \
    -DENABLE_APPS=ON \
    -DENABLE_TESTS=OFF

make -j$(nproc)
```

Build time: ~10-15 minutes on a modern machine.

### Verify the Build

The build produces:

- `build/linux/src/carrier/libcarrier.so` — Main Carrier library
- `build/linux/intermediates/lib/` — Dependencies (libcrystal, libconfig, libcurl, etc.)
- `build/linux/apps/` — Demo applications (elashell, elatests)

---

## Step 2: Build beagle-sidecar

Now build the sidecar that wraps the Carrier SDK with an HTTP API for OpenClaw.

### Clone the Channel Repo

```bash
cd ~/devs
git clone https://github.com/decentnetworks/openclaw-beagle-channel.git
cd openclaw-beagle-channel
```

### Build in "Real SDK" Mode

This links against the actual Carrier SDK (not stub mode):

```bash
cd packages/beagle-sidecar

export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK

cmake -S . -B build \
    -DBEAGLE_SDK_STUB=OFF \
    -DBEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT

cmake --build build
```

### Verify the Binary

```bash
./build/beagle-sidecar --help
```

Should show usage options for `--config`, `--data-dir`, `--port`, etc.

---

## Step 3: Build beagle-channel (TypeScript Plugin)

```bash
cd ~/devs/openclaw-beagle-channel/packages/beagle-channel

npm install
npm run build
```

This produces:
- `dist/` — Compiled JavaScript
- `dist/index.js` — Main entry point
- `dist/sidecarClient.js` — HTTP client for sidecar

---

## Step 4: Install Plugin to OpenClaw

The beagle-channel is an OpenClaw plugin. You must install it to the extensions directory for OpenClaw to recognize the "beagle" channel.

### Method A: Manual Install (Plugin Only)

```bash
cd ~/devs/openclaw-beagle-channel/packages/beagle-channel

# 1. Build (if not already built)
npm install
npm run build

# 2. Copy plugin files to OpenClaw extensions
mkdir -p ~/.openclaw/extensions/beagle
cp package.json index.js openclaw.plugin.json ~/.openclaw/extensions/beagle/
cp -r dist ~/.openclaw/extensions/beagle/
```

**Verify installation:**
```bash
ls ~/.openclaw/extensions/beagle/
# Should show: index.js, openclaw.plugin.json, package.json, dist/
```

### Method B: Fully Automated (Sidecar + Plugin)

```bash
cd ~/devs/openclaw-beagle-channel
./install.sh
```

This:
1. Builds the sidecar
2. Builds the channel plugin
3. Copies files to `~/.openclaw/extensions/beagle/`

### Method B: Manual Install

```bash
# Install channel plugin
mkdir -p ~/.openclaw/extensions/beagle
cp packages/beagle-channel/package.json ~/.openclaw/extensions/beagle/
cp packages/beagle-channel/index.js ~/.openclaw/extensions/beagle/
cp packages/beagle-channel/openclaw.plugin.json ~/.openclaw/extensions/beagle/
cp -r packages/beagle-channel/dist ~/.openclaw/extensions/beagle/

# Sidecar stays in place (or copy binary to PATH)
```

---

## Step 5: Configure OpenClaw

Edit `~/.openclaw/openclaw.json`:

```json
{
  "channels": {
    "beagle": {
      "dmPolicy": "open",
      "accounts": {
        "default": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken",
          "allowFrom": ["*"]
        }
      }
    }
  },
  "plugins": {
    "allow": ["beagle"],
    "entries": {
      "beagle": { "enabled": true }
    }
  }
}
```

OpenClaw 2026.x `doctor` may suggest **`allowFrom`** for open DMs and **`plugins.allow`** so locally installed extensions (under `~/.openclaw/extensions/beagle/`) are explicitly trusted. Run `openclaw doctor --fix` if you prefer it to patch `allowFrom` for you.

---

## Step 6: Run the Sidecar

### Quick Run (Manual)

```bash
cd ~/devs/openclaw-beagle-channel/packages/beagle-sidecar

export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK

./build/beagle-sidecar \
    --config $BEAGLE_SDK_ROOT/config/carrier.conf \
    --data-dir ~/.carrier \
    --port 39091
```

### Auto-add OpenClaw Directory

The sidecar automatically adds the OpenClaw Directory as a Carrier friend on startup.

- Default directory addresses (both are added for redundancy):
  - `bKaapxLjDGwuCZ7oohVFMVbcHWFYy7yNVGXkVSVL8AkAxbokCGi2`
  - `C5WWd6BpDvZmqVysfKZWFitK2B7XJJy9dwXVH12KGK62dk2RdKUt`
- To use a custom directory instead, set `--directory-address <addr>` or `BEAGLE_DIRECTORY_ADDRESS=<addr>`.
- The sidecar skips adding itself when its own Carrier address matches a directory address.

### Directory profile (agent name, versions, IPs)

The sidecar discovers OpenClaw agents from:

1. `agents` / `agents.list` in `~/.openclaw/openclaw.json` (same keys OpenClaw uses for multi-agent config).
2. **Subdirectories of `~/.openclaw/agents/<id>/`** — so a single install with only `agents.defaults` in JSON still gets a runtime for `main` (or whatever folders exist), and **`agentName` in the directory profile** comes from that id or from `agent/openclaw.json` inside the folder when present.
3. If nothing is found, it falls back to one synthetic **`main`** account (OpenClaw’s default agent id from `openclaw agents list`). Set **`BEAGLE_DEFAULT_AGENT_NAME`** to override the **display** name; otherwise it stays **`main`**.

**Versions:** `openclawVersion` is resolved from **`OPENCLAW_VERSION`**, else **`openclaw --version`**, else **`meta.lastTouchedVersion`** in `openclaw.json`. **`beagleChannelVersion`** is read from **`~/.openclaw/extensions/beagle/package.json`**, or **`BEAGLE_CHANNEL_VERSION`**.

**IPs:** `hostIp` is the primary local IPv4 from the sidecar. **`hostIpExternal`** (WAN) is **only filled by the beagle-sidecar** when it builds the directory profile JSON — the **beagle-channel** Node plugin does not send host IPs. The sidecar resolves WAN via **`BEAGLE_EXTERNAL_IP`** if set; otherwise it tries **`curl`** to (in order) api.ipify.org, icanhazip.com, and ifconfig.me/ip, with results **cached ~1 hour** (including empty failures, so a transient block does not spam requests). If all lookups fail or outbound HTTPS is blocked, set **`BEAGLE_EXTERNAL_IP=<your public IP>`** in the environment that launches the sidecar.

### Run with Helper Script

```bash
export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK
./start.sh
```

### Systemd User Service (Recommended)

```bash
export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK
./start.sh --install-systemd-user

# Enable after logout
loginctl enable-linger "$USER"
```

Check status:
```bash
./start.sh --status-systemd-user
journalctl --user -u beagle-sidecar -f
```

---

## Step 7: Test the Setup

### 1. Health Check

```bash
curl http://127.0.0.1:39091/health
```

Expected: `{"ok":true}`

### 2. Get Carrier Identity

```bash
curl http://127.0.0.1:39091/health
```

Also returns:
- `userId` — Your Carrier user ID (Beagle address)
- `address` — Your Carrier address

### 3. Start OpenClaw

```bash
openclaw gateway restart
```

### 4. Verify Channel Load

Check OpenClaw logs — should show:
- `[beagle] inbound service start`
- Polling the sidecar

---

## Troubleshooting

### CMake Can't Find Carrier SDK

Error: `BEAGLE_SDK_ROOT is required when BEAGLE_SDK_STUB=OFF`

**Fix**: Export the environment variable:
```bash
export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK
cmake --build build --target clean
cmake -S . -B build -DBEAGLE_SDK_STUB=OFF
cmake --build build
```

### Missing Libraries at Runtime

Error: `libcarrier.so: cannot open shared object file`

**Fix**: Add to `~/.bashrc`:
```bash
export LD_LIBRARY_PATH=$HOME/devs/Elastos.NET.Carrier.Native.SDK/build/linux/src/carrier:$LD_LIBRARY_PATH
```

### Sidecar Won't Start

Check the Carrier config exists:
```bash
ls $BEAGLE_SDK_ROOT/config/carrier.conf
```

### OpenClaw Plugin Not Loading

1. Check plugin is installed:
   ```bash
   ls ~/.openclaw/extensions/beagle/
   ```

2. Verify OpenClaw config validates:
   ```bash
   openclaw gateway config validate
   ```

### Beagle: sidecar returns `unknown_account` on `/events`

Gateway logs show `sidecar /events failed: 404 {"ok":false,"error":"unknown_account"}` when **`X-Beagle-Account`** (from `channels.beagle.accounts.<id>`) does not match any Carrier runtime the sidecar started.

The sidecar builds one runtime per **OpenClaw agent** from `~/.openclaw/openclaw.json` (`agents` / `agents.list`), using each agent’s id as the account name (e.g. `main`, `dirs`). If `openclaw doctor` moved Beagle settings under **`accounts.default`** but your agents are named **`main`** / **`dirs`**, the plugin may send `default` while the sidecar only has `main` — hence `unknown_account`.

**Fix (pick one):**

- **Upgrade sidecar** (recommended): recent builds map **`default`** to the same runtime as the sidecar’s default agent (`main` when present), so doctor-style `accounts.default` works without renaming.
- **Align config**: Rename `channels.beagle.accounts.default` to **`channels.beagle.accounts.main`** (or whatever agent id you use), matching routing in OpenClaw.

After changing config, restart the gateway and sidecar.

### Sidecar: `Bind failed` right after accounts start

The HTTP API binds **once** after all Carrier accounts are up. **`Bind failed`** means the listen port (default **39091**) is already taken — usually another **beagle-sidecar** still running (e.g. systemd user unit) or a stale process.

```bash
ss -tlnp | grep 39091
systemctl --user status beagle-sidecar.service   # if you use systemd
```

Stop the other instance, or run this one with a different **`--port`** (and point OpenClaw `sidecarBaseUrl` at it). Do not run two sidecars on the same port.

**systemd user service stuck in `failed` / “Start request repeated too quickly”**

That happens when **`beagle-sidecar.service`** restarts in a loop (each run hits `Bind failed` until the start limit). The port is still owned by **another** process — often a **foreground** `./start.sh run` you left running in another SSH session, `screen`, or `tmux`, while systemd tries to bind the same port.

```bash
systemctl --user stop beagle-sidecar
systemctl --user reset-failed beagle-sidecar
ss -tlnp | grep 39091    # or: ss -tlnp | grep "$BEAGLE_SIDECAR_PORT"
# Stop or kill the process that listens (not systemd’s), then:
systemctl --user start beagle-sidecar
```

Use **either** the systemd unit **or** a manual foreground run — not both on the same port.

### OpenClaw CLI: `pairing required` / gateway not reachable

After `openclaw gateway restart`, commands such as `openclaw logs --follow` connect to `ws://127.0.0.1:<port>`. If you see **`GatewayClientRequestError: pairing required`**, the CLI session is not approved yet (this is an OpenClaw gateway/CLI concern, not Beagle-specific).

1. **Confirm the gateway is running**
   ```bash
   systemctl --user status openclaw-gateway.service
   journalctl --user -u openclaw-gateway.service -n 80 --no-pager
   ```
2. **Approve this machine’s CLI as a gateway device** (typical fix on headless servers). List pending requests, then approve by id (exact flags depend on your OpenClaw version — use `openclaw devices --help`):
   ```bash
   openclaw devices list
   openclaw devices approve <RequestId>
   ```
   If `devices list` fails with the same error, use the Control UI from a browser (see `gateway.controlUi` in `openclaw.json`) to approve pending devices, or check OpenClaw’s docs for the pairing flow for your version.
3. **Token auth**: If `gateway.auth.mode` is `token`, ensure the CLI uses the same token (e.g. `OPENCLAW_GATEWAY_TOKEN` in `~/.openclaw/.env`, or `openclaw env list` / `openclaw env set`). The user systemd unit must load that environment if the gateway reads token from env.
4. **Updates**: Newer OpenClaw releases fix edge cases where the CLI incorrectly hits pairing despite token auth; upgrading may help if approval still fails.

---

## Project Structure Summary

```
~/devs/
├── Elastos.NET.Carrier.Native.SDK/
│   ├── build/linux/
│   │   ├── src/carrier/libcarrier.so
│   │   └── intermediates/lib/
│   └── config/carrier.conf
│
└── openclaw-beagle-channel/
    ├── packages/
    │   ├── beagle-sidecar/
    │   │   ├── build/beagle-sidecar
    │   │   ├── src/beagle_sdk.cpp
    │   │   └── start.sh
    │   └── beagle-channel/
    │       ├── dist/
    │       └── src/index.ts
    └── install.sh
```

---

## References

- **Elastos Carrier SDK** (Elastos official): https://github.com/elastos/Elastos.NET.Carrier.Native.SDK
- **Elastos Carrier SDK** (fork with replay fix branch): https://github.com/decentnetworks/Elastos.NET.Carrier.Native.SDK
- **Elastos Carrier SDK** (upstream target): https://github.com/0xli/Elastos.NET.Carrier.Native.SDK
- **OpenClaw Beagle Channel**: https://github.com/decentnetworks/openclaw-beagle-channel
- **Beagle Chat**: https://beagle.chat
- **Decent Network**: https://decent.network

---

*Document generated for OpenClaw Beagle Channel integration.*
