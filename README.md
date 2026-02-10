# openclaw-channel-beagle

**Beagle Chat (Elastos Carrier) channel plugin for OpenClaw**

This plugin enables OpenClaw to send and receive messages via Beagle Chat using a local sidecar service that interfaces with the Elastos Carrier network.

## Features

- ✅ Outbound text messages
- ✅ Outbound media (images, files) via URL or local path
- ✅ Inbound message polling via sidecar events endpoint
- ✅ Configurable sidecar URL and authentication
- ✅ TypeScript support with full type definitions

## Installation

### For macOS Development

```bash
npm install openclaw-channel-beagle
```

### For Ubuntu Production Deployment

1. Install Node.js 18+ and OpenClaw:
```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
npm install -g openclaw
```

2. Install the Beagle channel plugin:
```bash
npm install -g openclaw-channel-beagle
```

## Configuration

Add to your OpenClaw configuration file:

```yaml
channels:
  beagle:
    accounts:
      default:
        sidecarBaseUrl: "http://127.0.0.1:39091"  # Default sidecar URL
        authToken: "your-secret-token"            # Optional auth token
        pollInterval: 5000                        # Poll interval in ms (default: 5000)
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `sidecarBaseUrl` | string | `http://127.0.0.1:39091` | Base URL of the Beagle sidecar service |
| `authToken` | string | - | Authentication token for sidecar API calls |
| `pollInterval` | number | `5000` | Interval in milliseconds for polling inbound messages |

## Usage

### Sending Text Messages

```bash
openclaw message send --channel beagle --to "user@beagle" --text "Hello from OpenClaw!"
```

### Sending Media

```bash
# Send image via URL
openclaw message send --channel beagle --to "user@beagle" --media "https://example.com/image.jpg"

# Send image via local path
openclaw message send --channel beagle --to "user@beagle" --media "/path/to/image.jpg"
```

## Beagle Sidecar Service

This plugin requires a **Beagle Carrier sidecar** service running locally. The sidecar handles the actual communication with the Elastos Carrier network.

### Sidecar API Endpoints

The sidecar must implement the following HTTP endpoints:

#### POST `/sendText`
Send a text message.

**Request:**
```json
{
  "to": "recipient-address",
  "text": "Message content",
  "from": "sender-address",
  "authToken": "optional-auth-token"
}
```

**Response:** `200 OK` on success

#### POST `/sendMedia`
Send media (image, file, etc.).

**Request:**
```json
{
  "to": "recipient-address",
  "mediaUrl": "https://example.com/file.jpg",
  "mediaPath": "/local/path/to/file.jpg",
  "caption": "Optional caption",
  "from": "sender-address",
  "authToken": "optional-auth-token"
}
```

**Response:** `200 OK` on success

#### GET `/events`
Poll for inbound messages.

**Query Parameters:**
- `authToken` (optional): Authentication token

**Response:**
```json
{
  "events": [
    {
      "from": "sender-address",
      "to": "recipient-address",
      "text": "Message content",
      "mediaUrl": "https://example.com/file.jpg",
      "timestamp": 1234567890,
      "messageId": "unique-message-id"
    }
  ]
}
```

## Ubuntu Deployment with systemd

### 1. Create Sidecar Service

Create `/etc/systemd/system/beagle-carrier-sidecar.service`:

```ini
[Unit]
Description=Beagle Carrier Sidecar Service
After=network.target

[Service]
Type=simple
User=openclaw
WorkingDirectory=/opt/beagle-sidecar
ExecStart=/opt/beagle-sidecar/beagle-carrier-sidecar
Restart=always
RestartSec=10
Environment="PORT=39091"
Environment="AUTH_TOKEN=your-secret-token"

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/beagle-sidecar

[Install]
WantedBy=multi-user.target
```

### 2. Enable and Start Services

```bash
# Create service user
sudo useradd -r -s /bin/false openclaw

# Create data directory
sudo mkdir -p /var/lib/beagle-sidecar
sudo chown openclaw:openclaw /var/lib/beagle-sidecar

# Enable and start sidecar
sudo systemctl daemon-reload
sudo systemctl enable beagle-carrier-sidecar
sudo systemctl start beagle-carrier-sidecar

# Check status
sudo systemctl status beagle-carrier-sidecar
```

### 3. Configure OpenClaw

Edit `/etc/openclaw/config.yaml` or create config in your home directory:

```yaml
channels:
  beagle:
    accounts:
      default:
        sidecarBaseUrl: "http://127.0.0.1:39091"
        authToken: "your-secret-token"
```

### 4. Start OpenClaw

```bash
openclaw start
```

## Development

### Building from Source

```bash
git clone https://github.com/0xli/openclaw-beagle-channel.git
cd openclaw-beagle-channel
npm install
npm run build
```

### Project Structure

```
src/
├── index.ts       # Main plugin entry point and setup
├── channel.ts     # Outbound message handling (sendText, sendMedia)
├── service.ts     # Inbound message polling service
├── config.ts      # Configuration schema and defaults
└── types.ts       # TypeScript type definitions
```

### Testing with Mock Sidecar

For development, you can create a simple mock sidecar:

```javascript
// mock-sidecar.js
const express = require('express');
const app = express();
app.use(express.json());

let events = [];

app.post('/sendText', (req, res) => {
  console.log('Received text:', req.body);
  res.json({ success: true });
});

app.post('/sendMedia', (req, res) => {
  console.log('Received media:', req.body);
  res.json({ success: true });
});

app.get('/events', (req, res) => {
  const eventsToSend = [...events];
  events = [];
  res.json({ events: eventsToSend });
});

app.listen(39091, () => {
  console.log('Mock sidecar listening on port 39091');
});
```

Run it with:
```bash
node mock-sidecar.js
```

## Architecture

```
┌─────────────┐         ┌──────────────────┐         ┌─────────────────┐
│  OpenClaw   │◄───────►│  Beagle Plugin   │◄───────►│ Beagle Sidecar  │
│             │         │                  │  HTTP   │                 │
│ (TypeScript)│         │   (TypeScript)   │         │ (Native/Any)    │
└─────────────┘         └──────────────────┘         └─────────────────┘
                                                              │
                                                              ▼
                                                      ┌─────────────────┐
                                                      │ Elastos Carrier │
                                                      │    Network      │
                                                      └─────────────────┘
```

- **OpenClaw**: Main application managing multiple chat channels
- **Beagle Plugin**: This TypeScript plugin implementing OpenClaw's channel API
- **Beagle Sidecar**: Separate service handling Elastos Carrier protocol
- **Elastos Carrier**: Decentralized peer-to-peer network

## Troubleshooting

### Sidecar Connection Failed

If you see connection errors:

1. Check sidecar is running:
   ```bash
   curl http://127.0.0.1:39091/events
   ```

2. Check firewall settings:
   ```bash
   sudo ufw allow 39091/tcp
   ```

3. Verify sidecar logs:
   ```bash
   sudo journalctl -u beagle-carrier-sidecar -f
   ```

### Messages Not Sending

1. Verify authentication token matches in both plugin config and sidecar
2. Check sidecar logs for errors
3. Test sidecar API directly with curl:
   ```bash
   curl -X POST http://127.0.0.1:39091/sendText \
     -H "Content-Type: application/json" \
     -d '{"to":"user@beagle","text":"test"}'
   ```

### Messages Not Receiving

1. Check polling is active in OpenClaw logs
2. Verify `/events` endpoint returns data:
   ```bash
   curl http://127.0.0.1:39091/events
   ```
3. Increase log verbosity in OpenClaw config

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests if applicable
5. Submit a pull request

## License

MIT

## Links

- [OpenClaw Documentation](https://docs.openclaw.ai/)
- [OpenClaw Plugins](https://docs.openclaw.ai/tools/plugin)
- [Elastos Carrier](https://www.elastos.org/carrier)

## Support

For issues and questions:
- GitHub Issues: https://github.com/0xli/openclaw-beagle-channel/issues
- OpenClaw Community: https://openclaw.ai/community
