import { BeagleChannelPlugin, MediaType } from '../src';

/**
 * Example usage of the Beagle Channel Plugin
 */
async function main() {
  // Create plugin instance pointing to sidecar
  const plugin = new BeagleChannelPlugin('http://localhost:8080');

  // Initialize and connect
  await plugin.initialize();
  console.log('Plugin initialized');

  // Set up message handler
  plugin.onMessage((message) => {
    console.log('Received message:', {
      from: message.peer,
      text: message.text,
      mediaType: message.mediaType,
      mediaUrl: message.mediaUrl,
      timestamp: new Date(message.timestamp).toISOString()
    });
  });

  // Example 1: Send text message
  await plugin.sendText({
    channelId: 'beagle',
    peer: 'test-user',
    text: 'Hello from OpenClaw!'
  });
  console.log('Text message sent');

  // Example 2: Send image with caption
  await plugin.sendMedia({
    channelId: 'beagle',
    peer: 'test-user',
    mediaPath: '/path/to/screenshot.png',
    mediaType: MediaType.Image,
    text: 'Here is a screenshot of the results'
  });
  console.log('Image sent');

  // Example 3: Send document
  await plugin.sendMedia({
    channelId: 'beagle',
    peer: 'test-user',
    mediaPath: '/path/to/report.pdf',
    mediaType: MediaType.Document,
    filename: 'analysis-report.pdf',
    mimeType: 'application/pdf',
    text: 'Here is the analysis report'
  });
  console.log('Document sent');

  // Example 4: Send image from URL
  await plugin.sendMedia({
    channelId: 'beagle',
    peer: 'test-user',
    mediaUrl: 'https://example.com/chart.png',
    mediaType: MediaType.Image,
    text: 'Performance chart'
  });
  console.log('Image from URL sent');

  // Keep running to receive messages
  console.log('Listening for messages... (press Ctrl+C to exit)');
}

main().catch(console.error);
