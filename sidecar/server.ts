import express from 'express';
import { WebSocketServer } from 'ws';
import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join } from 'path';

/**
 * Reference implementation of Beagle Sidecar API
 * 
 * This is a minimal implementation that demonstrates the API contract.
 * In production, this would integrate with the actual Carrier SDK.
 */

const app = express();
const server = createServer(app);
const wss = new WebSocketServer({ server, path: '/events' });

app.use(express.json());

// Track connected WebSocket clients
const clients = new Set<any>();

wss.on('connection', (ws) => {
  console.log('WebSocket client connected');
  clients.add(ws);

  ws.on('close', () => {
    console.log('WebSocket client disconnected');
    clients.delete(ws);
  });
});

/**
 * Broadcast an inbound message to all connected clients
 */
function broadcastMessage(message: any) {
  const data = JSON.stringify(message);
  clients.forEach((client) => {
    if (client.readyState === 1) { // OPEN
      client.send(data);
    }
  });
}

/**
 * POST /sendText - Send a text message
 */
app.post('/sendText', async (req, res) => {
  const { peer, text } = req.body;

  if (!peer || !text) {
    return res.status(400).json({ error: 'peer and text are required' });
  }

  console.log(`Sending text message to ${peer}: ${text}`);

  // In production: Send via Carrier SDK
  // For now, just log it
  
  res.json({ success: true });
});

/**
 * POST /sendMedia - Send a media message
 * 
 * MVP: Upload file and send as URL
 * V2: Use native file transfer if available
 */
app.post('/sendMedia', async (req, res) => {
  const { peer, path, url, mime, filename, caption } = req.body;

  if (!peer) {
    return res.status(400).json({ error: 'peer is required' });
  }

  if (!path && !url) {
    return res.status(400).json({ error: 'Either path or url is required' });
  }

  console.log(`Sending media to ${peer}:`, { path, url, mime, filename, caption });

  // MVP implementation:
  // 1. If path is provided, upload to storage and get URL
  // 2. Send the URL (and caption) via Carrier
  
  // In production:
  // - Upload file to storage endpoint
  // - Or use native file transfer if available
  // - Send message with media reference

  res.json({ success: true });
});

/**
 * Simulate receiving an inbound message
 * In production, this would be triggered by Carrier SDK events
 */
app.post('/simulate-inbound', (req, res) => {
  const message = {
    peer: req.body.peer || 'test-peer',
    text: req.body.text,
    mediaPath: req.body.mediaPath,
    mediaUrl: req.body.mediaUrl,
    mediaType: req.body.mediaType,
    filename: req.body.filename,
    size: req.body.size,
    timestamp: Date.now(),
    messageId: `msg-${Date.now()}`
  };

  broadcastMessage(message);
  res.json({ success: true, message });
});

const PORT = process.env.PORT || 8080;

server.listen(PORT, () => {
  console.log(`Beagle Sidecar API listening on port ${PORT}`);
  console.log(`WebSocket endpoint: ws://localhost:${PORT}/events`);
});
