# OpenClaw Beagle Channel Plugin

OpenClaw channel plugin for Beagle Chat with full media support (images, documents, audio, video, voice).

## Overview

This plugin enables OpenClaw agents to communicate via Beagle Chat with support for:
- Text messages
- Images with captions
- Documents (any file type)
- Audio/voice messages
- Video messages

The implementation follows a two-layer architecture:
1. **Channel Plugin** (TypeScript): Implements OpenClaw's channel interface
2. **Sidecar API** (Node.js): Handles actual Carrier SDK integration and file transport

## Architecture

```
OpenClaw Agent
     ↓
Channel Plugin (this repo)
     ↓ HTTP/WebSocket
Beagle Sidecar API
     ↓ Carrier SDK
Beagle Chat Network
```

## Installation

### Plugin

```bash
npm install openclaw-beagle-channel
```

### Sidecar

```bash
cd sidecar
npm install
npm run build
npm start
```

The sidecar runs on `http://localhost:8080` by default.

## Usage

### Basic Setup

```typescript
import { BeagleChannelPlugin } from 'openclaw-beagle-channel';

const plugin = new BeagleChannelPlugin('http://localhost:8080');

// Initialize connection
await plugin.initialize();

// Handle inbound messages
plugin.onMessage((message) => {
  console.log('Received:', message);
  if (message.mediaType) {
    console.log('Media:', message.mediaType, message.mediaUrl || message.mediaPath);
  }
});
```

### Sending Text Messages

```typescript
await plugin.sendText({
  channelId: 'beagle',
  peer: 'user-id',
  text: 'Hello from OpenClaw!'
});
```

### Sending Media Messages

```typescript
// Send an image with caption
await plugin.sendMedia({
  channelId: 'beagle',
  peer: 'user-id',
  mediaPath: '/path/to/image.jpg',
  mediaType: MediaType.Image,
  text: 'Check out this image!'
});

// Send a document
await plugin.sendMedia({
  channelId: 'beagle',
  peer: 'user-id',
  mediaPath: '/path/to/report.pdf',
  mediaType: MediaType.Document,
  filename: 'report.pdf',
  mimeType: 'application/pdf'
});

// Send from URL
await plugin.sendMedia({
  channelId: 'beagle',
  peer: 'user-id',
  mediaUrl: 'https://example.com/image.png',
  mediaType: MediaType.Image
});
```

## Capabilities

The plugin advertises the following capabilities to OpenClaw:

```typescript
{
  chatTypes: ['direct'],
  media: {
    sendImage: true,
    receiveImage: true,
    sendDocument: true,
    receiveDocument: true,
    sendAudio: true,
    receiveAudio: true,
    sendVideo: true,
    receiveVideo: true,
    sendVoice: true,
    receiveVoice: true
  }
}
```

## OpenClaw CLI Integration

Once configured, you can use OpenClaw's CLI to send media:

```bash
# Send text only
openclaw message send --channel beagle --to user-id --text "Hello!"

# Send image with caption
openclaw message send --channel beagle --to user-id --media /path/to/image.jpg --message "Check this out"

# Send document
openclaw message send --channel beagle --to user-id --media /path/to/document.pdf
```

## Sidecar API

The sidecar implements the following API:

### POST /sendText

Send a text message.

```bash
curl -X POST http://localhost:8080/sendText \
  -H "Content-Type: application/json" \
  -d '{"peer": "user-id", "text": "Hello"}'
```

### POST /sendMedia

Send a media message (MVP: URL-based, V2: native file transfer).

```bash
curl -X POST http://localhost:8080/sendMedia \
  -H "Content-Type: application/json" \
  -d '{
    "peer": "user-id",
    "path": "/path/to/file.jpg",
    "mime": "image/jpeg",
    "caption": "Optional caption"
  }'
```

### WebSocket /events

Subscribe to inbound messages.

```javascript
const ws = new WebSocket('ws://localhost:8080/events');

ws.on('message', (data) => {
  const message = JSON.parse(data);
  console.log('Inbound:', message);
});
```

### Inbound Message Format

```typescript
{
  peer: string;           // Sender ID
  text?: string;          // Message text or caption
  mediaPath?: string;     // Local path to downloaded media
  mediaUrl?: string;      // URL to media
  mediaType?: MediaType;  // Type of media
  filename?: string;      // Original filename
  size?: number;          // File size in bytes
  timestamp: number;      // Unix timestamp
  messageId: string;      // Unique message ID
  transcript?: string;    // For audio/voice messages
}
```

## Media Implementation Strategy

### MVP (Current)

- **Outbound**: Sidecar uploads files to storage and sends URLs
- **Inbound**: Sidecar downloads files and provides local paths
- Compatible with any Carrier client

### V2 (Future)

- **Outbound**: Native file transfer via Carrier SDK
- **Inbound**: Direct file streaming
- Fallback to URL-based transfer for compatibility

## Development

### Build Plugin

```bash
npm run build
```

### Build Sidecar

```bash
cd sidecar
npm run build
```

### Testing

```bash
# Terminal 1: Start sidecar
cd sidecar
npm run dev

# Terminal 2: Test with curl
curl -X POST http://localhost:8080/sendText \
  -H "Content-Type: application/json" \
  -d '{"peer": "test", "text": "Hello"}'
```

## Integration with OpenClaw

When OpenClaw runs tools (shell commands, file operations), it may produce files that need to be sent as attachments:
- Log files
- Screenshots
- Generated PDFs
- Any other artifacts

The plugin's document support enables agents to return these artifacts to users automatically.

## License

MIT
