/**
 * Mock Beagle Carrier sidecar for development and testing
 * 
 * This is a simple HTTP server that implements the sidecar API endpoints
 * for testing the Beagle channel plugin without a real sidecar.
 * 
 * Usage:
 *   node examples/mock-sidecar.js
 * 
 * The mock sidecar will:
 * - Accept sendText and sendMedia requests
 * - Store messages in memory
 * - Serve stored messages via /events endpoint
 * - Simulate message delivery
 */

const http = require('http');
const url = require('url');

const PORT = process.env.PORT || 39091;
const AUTH_TOKEN = process.env.AUTH_TOKEN || 'test-token';

// In-memory message store
let messageQueue = [];
let messageId = 0;

/**
 * Parse JSON body from request
 */
function parseBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (e) {
        reject(e);
      }
    });
  });
}

/**
 * Send JSON response
 */
function sendJSON(res, statusCode, data) {
  res.writeHead(statusCode, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(data));
}

/**
 * Handle POST /sendText
 */
async function handleSendText(req, res) {
  try {
    const body = await parseBody(req);
    console.log('[sendText] Received:', body);
    
    // Validate auth token
    if (AUTH_TOKEN && body.authToken !== AUTH_TOKEN) {
      return sendJSON(res, 401, { error: 'Invalid auth token' });
    }
    
    // Simulate sending and receiving echo
    setTimeout(() => {
      messageQueue.push({
        messageId: `msg-${++messageId}`,
        from: body.to,
        to: body.from || 'system',
        text: `Echo: ${body.text}`,
        timestamp: Date.now(),
      });
    }, 1000);
    
    sendJSON(res, 200, { success: true, messageId: `sent-${messageId}` });
  } catch (error) {
    console.error('[sendText] Error:', error);
    sendJSON(res, 500, { error: 'Internal server error' });
  }
}

/**
 * Handle POST /sendMedia
 */
async function handleSendMedia(req, res) {
  try {
    const body = await parseBody(req);
    console.log('[sendMedia] Received:', body);
    
    // Validate auth token
    if (AUTH_TOKEN && body.authToken !== AUTH_TOKEN) {
      return sendJSON(res, 401, { error: 'Invalid auth token' });
    }
    
    // Simulate media delivery confirmation
    setTimeout(() => {
      messageQueue.push({
        messageId: `msg-${++messageId}`,
        from: body.to,
        to: body.from || 'system',
        text: `Received media: ${body.caption || 'No caption'}`,
        mediaUrl: body.mediaUrl,
        timestamp: Date.now(),
      });
    }, 1500);
    
    sendJSON(res, 200, { success: true, messageId: `sent-${messageId}` });
  } catch (error) {
    console.error('[sendMedia] Error:', error);
    sendJSON(res, 500, { error: 'Internal server error' });
  }
}

/**
 * Handle GET /events
 */
function handleEvents(req, res) {
  const query = url.parse(req.url, true).query;
  
  // Validate auth token
  if (AUTH_TOKEN && query.authToken !== AUTH_TOKEN) {
    return sendJSON(res, 401, { error: 'Invalid auth token' });
  }
  
  // Return all queued messages and clear the queue
  const events = [...messageQueue];
  messageQueue = [];
  
  console.log(`[events] Returning ${events.length} messages`);
  sendJSON(res, 200, { events });
}

/**
 * Handle GET /health
 */
function handleHealth(req, res) {
  sendJSON(res, 200, { 
    status: 'ok', 
    uptime: process.uptime(),
    queuedMessages: messageQueue.length,
  });
}

/**
 * Main request handler
 */
const server = http.createServer(async (req, res) => {
  const { pathname } = url.parse(req.url);
  
  console.log(`${req.method} ${pathname}`);
  
  // Route requests
  if (req.method === 'POST' && pathname === '/sendText') {
    await handleSendText(req, res);
  } else if (req.method === 'POST' && pathname === '/sendMedia') {
    await handleSendMedia(req, res);
  } else if (req.method === 'GET' && pathname === '/events') {
    handleEvents(req, res);
  } else if (req.method === 'GET' && pathname === '/health') {
    handleHealth(req, res);
  } else {
    sendJSON(res, 404, { error: 'Not found' });
  }
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`Mock Beagle sidecar listening on http://127.0.0.1:${PORT}`);
  console.log(`Auth token: ${AUTH_TOKEN || 'none'}`);
  console.log('\nEndpoints:');
  console.log(`  POST http://127.0.0.1:${PORT}/sendText`);
  console.log(`  POST http://127.0.0.1:${PORT}/sendMedia`);
  console.log(`  GET  http://127.0.0.1:${PORT}/events`);
  console.log(`  GET  http://127.0.0.1:${PORT}/health`);
  console.log('\nReady to accept requests!');
});
