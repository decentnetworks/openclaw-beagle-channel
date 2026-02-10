# OpenClaw Beagle Channel

OpenClaw channel plugin for Beagle Chat - enables users to send direct messages to OpenClaw agents through the Beagle Chat platform.

## Overview

This plugin integrates [OpenClaw](https://www.npmjs.com/package/openclaw) with Beagle Chat, allowing users to interact with AI agents via direct messages. The plugin handles:

- **Direct messaging**: Send and receive DMs between users and agents
- **Webhook support**: Process incoming messages from Beagle Chat
- **User management**: Fetch user information from Beagle API
- **Secure authentication**: Token-based API authentication and webhook signature verification

## Installation

```bash
npm install openclaw-beagle-channel
```

Or with pnpm:

```bash
pnpm add openclaw-beagle-channel
```

## Configuration

The plugin requires the following configuration:

### Required Settings

- **`apiUrl`**: Beagle Chat API endpoint URL
- **`authToken`**: Authentication token for Beagle API

### Optional Settings

- **`webhookSecret`**: Secret key for webhook signature verification
- **`webhookPort`**: Port for webhook server (if applicable)
- **`debug`**: Enable debug logging (default: `false`)

### Example Configuration

```typescript
import { BeagleChannel } from 'openclaw-beagle-channel';

const channel = new BeagleChannel();

await channel.initialize({
  apiUrl: 'https://api.beagle.example.com/v1',
  authToken: 'your-beagle-api-token',
  webhookSecret: 'your-webhook-secret',
  debug: true,
});
```

### Environment Variables

You can also configure the plugin using environment variables:

```bash
BEAGLE_API_URL=https://api.beagle.example.com/v1
BEAGLE_AUTH_TOKEN=your-beagle-api-token
BEAGLE_WEBHOOK_SECRET=your-webhook-secret
BEAGLE_DEBUG=true
```

## Usage

### Sending Messages

```typescript
// Send a direct message to a user
await channel.sendMessage('user-id-123', 'Hello from OpenClaw!');
```

### Receiving Messages

```typescript
// Register a message handler
channel.onMessage(async (message) => {
  console.log(`Received from ${message.userName}: ${message.text}`);
  
  // Respond to the user
  await channel.sendMessage(message.userId, 'Thanks for your message!');
});
```

### Handling Webhooks

```typescript
// Handle incoming webhook from Beagle
import express from 'express';

const app = express();
app.use(express.json());

app.post('/webhook/beagle', async (req, res) => {
  try {
    await channel.handleWebhook(req.body);
    res.status(200).json({ success: true });
  } catch (error) {
    console.error('Webhook error:', error);
    res.status(500).json({ error: 'Failed to process webhook' });
  }
});

app.listen(3000, () => {
  console.log('Webhook server listening on port 3000');
});
```

### Getting User Information

```typescript
// Fetch user details
const user = await channel.getUser('user-id-123');
if (user) {
  console.log(`User: ${user.name} (${user.email})`);
}
```

## API Reference

### BeagleChannel

#### Methods

- **`initialize(config: BeagleChannelConfig): Promise<void>`**
  
  Initialize the channel with configuration settings.

- **`onMessage(handler: MessageHandler): void`**
  
  Register a callback to handle incoming messages.

- **`sendMessage(userId: string, text: string): Promise<void>`**
  
  Send a direct message to a user.

- **`handleWebhook(payload: BeagleWebhookPayload): Promise<void>`**
  
  Process incoming webhook events from Beagle.

- **`getUser(userId: string): Promise<BeagleUser | null>`**
  
  Fetch user information from Beagle API.

- **`disconnect(): Promise<void>`**
  
  Cleanup and disconnect the channel.

### Types

See [types.ts](./types.ts) for full type definitions:

- `BeagleChannelConfig` - Configuration options
- `BeagleMessage` - Message structure
- `BeagleUser` - User information
- `BeagleWebhookPayload` - Webhook event payload
- `BeagleAttachment` - Message attachment

## Development

### Building

```bash
npm run build
```

### Development Mode

```bash
npm run dev
```

## OpenClaw Integration

This package is designed to work with OpenClaw's plugin system. When installed in an OpenClaw project, it will be automatically registered as a channel option.

### OpenClaw Metadata

```json
{
  "openclaw": {
    "channel": {
      "id": "beagle",
      "label": "Beagle Chat",
      "selectionLabel": "Beagle Chat (DM)",
      "blurb": "Beagle Chat direct messaging integration."
    }
  }
}
```

## Security

- **API Authentication**: All API requests use Bearer token authentication
- **Webhook Verification**: Webhooks are verified using HMAC-SHA256 signatures with the configured secret
- **Secure Configuration**: Store sensitive credentials in environment variables
- **Cryptographic Security**: Uses Node.js crypto module for secure signature generation and verification

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

MIT

## Author

0xli

## Repository

[https://github.com/0xli/openclaw-beagle-channel](https://github.com/0xli/openclaw-beagle-channel)
