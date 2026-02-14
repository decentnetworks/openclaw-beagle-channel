# Beagle Channel Installation Guide

Complete guide for compiling and installing OpenClaw's Beagle Chat channel, including the native Elastos Carrier SDK.

## Source Repositories

Clone these two repositories to get started:

```bash
# 1. Elastos Carrier Native SDK (the P2P networking layer)
git clone https://github.com/0xli/Elastos.NET.Carrier.Native.SDK.git

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
git clone https://github.com/0xli/Elastos.NET.Carrier.Native.SDK.git
cd Elastos.NET.Carrier.Native.SDK
```

> **Note:** We use the fork at `github.com/0xli/` which includes necessary patches for OpenClaw integration.

### Configure and Build

```bash
mkdir -p build/linux
cd build/linux

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

## Step 4: Install to OpenClaw

### Method A: Automated Install

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
          "authToken": "devtoken"
        }
      }
    }
  },
  "plugins": {
    "entries": {
      "beagle": { "enabled": true }
    }
  }
}
```

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
- **Elastos Carrier SDK** (fork with patches): https://github.com/0xli/Elastos.NET.Carrier.Native.SDK
- **Elastos Carrier SDK**: https://github.com/0xli/Elastos.NET.Carrier.Native.SDK
- **OpenClaw Beagle Channel**: https://github.com/decentnetworks/openclaw-beagle-channel
- **Beagle Chat**: https://beagle.chat
- **Decent Network**: https://decent.network

---

*Document generated for OpenClaw Beagle Channel integration.*