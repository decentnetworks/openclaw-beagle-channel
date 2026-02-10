# Beagle Sidecar API

Reference implementation of the sidecar API for the OpenClaw Beagle Channel plugin.

## Purpose

The sidecar acts as a bridge between the OpenClaw channel plugin and the Beagle Chat network (Carrier). It:
1. Receives requests from the plugin to send messages/media
2. Integrates with the Carrier SDK to actually transmit data
3. Listens for inbound messages from Carrier and forwards them to the plugin

## Architecture

```
OpenClaw Plugin → HTTP API → Sidecar → Carrier SDK → Beagle Chat Network
                ← WebSocket ←
```

## API Endpoints

### POST /sendText

Send a text message to a peer.

**Request:**
```json
{
  "peer": "user-id",
  "text": "Hello, world!"
}
```

**Response:**
```json
{
  "success": true
}
```

### POST /sendMedia

Send a media message (image, document, audio, video, voice).

**Request:**
```json
{
  "peer": "user-id",
  "path": "/local/path/to/file.jpg",
  "mime": "image/jpeg",
  "filename": "photo.jpg",
  "caption": "Check this out!"
}
```

Or with URL:
```json
{
  "peer": "user-id",
  "url": "https://example.com/file.jpg",
  "caption": "Check this out!"
}
```

**Response:**
```json
{
  "success": true
}
```

### WebSocket /events

Subscribe to inbound messages from Beagle Chat.

**Connection:**
```
ws://localhost:8080/events
```

**Message format:**
```json
{
  "peer": "sender-id",
  "text": "Optional message text or caption",
  "mediaPath": "/local/path/to/downloaded/file.jpg",
  "mediaUrl": "https://example.com/file.jpg",
  "mediaType": "image",
  "filename": "photo.jpg",
  "size": 123456,
  "timestamp": 1234567890000,
  "messageId": "unique-msg-id",
  "transcript": "Optional transcript for audio/voice"
}
```

## Implementation Strategy

### MVP (Current State)

This reference implementation logs actions but doesn't integrate with Carrier yet.

For a production MVP:

1. **Outbound Media**: Upload files to a storage service and send URLs
   - Read file from `path`
   - Upload to your storage endpoint (S3, etc.)
   - Get public URL
   - Send URL + caption via Carrier as a text message

2. **Inbound Media**: Download files and provide local paths
   - Receive message with media URL from Carrier
   - Download to local temp directory
   - Emit WebSocket event with `mediaPath`

### V2 (Native File Transfer)

If Carrier SDK supports native file transfer:

1. **Outbound**: Stream file directly via Carrier
2. **Inbound**: Receive file stream and write to disk

Keep URL fallback for compatibility with clients that don't support native transfer.

## Carrier SDK Integration Points

When integrating with the actual Carrier SDK, you'll need to:

1. **Initialize Carrier** on sidecar startup
2. **Listen for connections** and message events
3. **Map Carrier events** to the WebSocket event format
4. **Send messages** via Carrier SDK in the `/sendText` and `/sendMedia` handlers

Example pseudocode:

```typescript
// Initialize Carrier
const carrier = await Carrier.initialize({
  // ... config
});

// Listen for messages
carrier.on('message', (event) => {
  const message = {
    peer: event.from,
    text: event.text,
    mediaUrl: event.mediaUrl,
    mediaType: detectMediaType(event.mediaUrl),
    timestamp: Date.now(),
    messageId: event.id
  };
  
  // Broadcast to all WebSocket clients
  broadcastMessage(message);
});

// Send via Carrier
app.post('/sendText', async (req, res) => {
  await carrier.sendMessage({
    to: req.body.peer,
    text: req.body.text
  });
  res.json({ success: true });
});
```

## Running

### Development

```bash
npm install
npm run dev
```

### Production

```bash
npm install
npm run build
npm start
```

### Environment Variables

- `PORT`: Server port (default: 8080)
- `CARRIER_CONFIG`: Path to Carrier configuration (if needed)

## Testing

### Start Server

```bash
npm run dev
```

### Test Text Message

```bash
curl -X POST http://localhost:8080/sendText \
  -H "Content-Type: application/json" \
  -d '{"peer": "test-user", "text": "Hello"}'
```

### Test Media Message

```bash
curl -X POST http://localhost:8080/sendMedia \
  -H "Content-Type: application/json" \
  -d '{
    "peer": "test-user",
    "path": "/tmp/test.jpg",
    "mime": "image/jpeg",
    "caption": "Test image"
  }'
```

### Simulate Inbound Message

```bash
curl -X POST http://localhost:8080/simulate-inbound \
  -H "Content-Type: application/json" \
  -d '{
    "peer": "test-sender",
    "text": "Inbound test message",
    "mediaUrl": "https://example.com/image.jpg",
    "mediaType": "image"
  }'
```

### Test WebSocket

```javascript
const WebSocket = require('ws');
const ws = new WebSocket('ws://localhost:8080/events');

ws.on('open', () => {
  console.log('Connected to sidecar');
});

ws.on('message', (data) => {
  console.log('Received:', JSON.parse(data));
});
```

## File Storage Strategy

For the MVP URL-based approach, you'll need a file storage solution:

### Option 1: Local Static Server
```typescript
app.use('/media', express.static('/path/to/media/storage'));
```

Pros: Simple, no external dependencies
Cons: Not suitable for production, no auth, single server

### Option 2: Cloud Storage (S3, GCS, etc.)
```typescript
const uploadToS3 = async (filePath) => {
  // Upload file to S3
  // Return public URL
};
```

Pros: Scalable, reliable, can add auth
Cons: Requires cloud service setup

### Option 3: Self-hosted Object Storage (MinIO, etc.)
Similar to Option 2 but self-hosted.

## Security Considerations

1. **Authentication**: Add auth to all endpoints in production
2. **File Validation**: Validate file types and sizes before processing
3. **Rate Limiting**: Prevent abuse of upload endpoints
4. **URL Expiry**: Use signed URLs with expiration for media
5. **WebSocket Auth**: Require authentication for WebSocket connections

## Next Steps

1. Integrate with actual Carrier SDK
2. Implement file upload/download logic
3. Add proper error handling and logging
4. Add authentication and authorization
5. Set up file storage (local or cloud)
6. Add rate limiting and validation
7. Deploy to production environment (Ubuntu server as mentioned)
