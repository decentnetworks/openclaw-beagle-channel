/**
 * Example usage of the Beagle Chat channel plugin
 */

// Import the plugin
const beaglePlugin = require('openclaw-channel-beagle').default;

// Example 1: Basic setup with default configuration
const config = {
  sidecarBaseUrl: 'http://127.0.0.1:39091',
  authToken: 'your-secret-token',
};

// The plugin would be registered by OpenClaw like this:
// beaglePlugin.setup(api, config);

// Example 2: Using the plugin programmatically
const { BeagleChannel } = require('openclaw-channel-beagle');

async function sendMessage() {
  const channel = new BeagleChannel({
    sidecarBaseUrl: 'http://127.0.0.1:39091',
    authToken: 'your-secret-token',
  });

  // Send a text message
  await channel.sendText({
    to: 'user@beagle',
    text: 'Hello from OpenClaw!',
    from: 'me@beagle',
  });

  // Send media
  await channel.sendMedia({
    to: 'user@beagle',
    mediaUrl: 'https://example.com/image.jpg',
    caption: 'Check out this image',
    from: 'me@beagle',
  });

  // Send local file
  await channel.sendMedia({
    to: 'user@beagle',
    mediaPath: '/path/to/local/file.jpg',
    caption: 'Local file',
    from: 'me@beagle',
  });
}

// Example 3: Setting up inbound message handling
const { startInboundService } = require('openclaw-channel-beagle');

async function setupInbound() {
  // Mock OpenClaw API
  const api = {
    receiveMessage: async (message) => {
      console.log('Received message:', message);
      // OpenClaw would handle this message
      // and route it to the appropriate handler
    },
  };

  const config = {
    sidecarBaseUrl: 'http://127.0.0.1:39091',
    authToken: 'your-secret-token',
    pollInterval: 5000, // Poll every 5 seconds
  };

  // Start the inbound service
  await startInboundService(api, config);
}

module.exports = {
  sendMessage,
  setupInbound,
};
