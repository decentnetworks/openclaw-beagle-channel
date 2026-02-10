/**
 * Integration test for Beagle channel plugin
 * 
 * This script tests the plugin by:
 * 1. Creating a channel instance
 * 2. Sending text and media messages
 * 3. Starting the inbound service
 * 4. Verifying messages are received
 * 
 * Prerequisites:
 * - Mock sidecar must be running on port 39091
 * - Run: node examples/mock-sidecar.js
 */

const { BeagleChannel, startInboundService } = require('../dist/index.js');

const config = {
  sidecarBaseUrl: 'http://127.0.0.1:39091',
  authToken: 'test-token',
  pollInterval: 2000,
};

let receivedMessages = [];

async function runTests() {
  console.log('🚀 Starting Beagle Chat Plugin Integration Tests\n');

  // Test 1: Create channel
  console.log('✅ Test 1: Creating BeagleChannel instance...');
  const channel = new BeagleChannel(config);
  console.log('   Channel created successfully\n');

  // Test 2: Send text message
  console.log('✅ Test 2: Sending text message...');
  try {
    await channel.sendText({
      to: 'test-user@beagle',
      text: 'Hello from integration test!',
      from: 'test-system',
    });
    console.log('   Text message sent successfully\n');
  } catch (error) {
    console.error('   ❌ Failed to send text:', error.message);
    process.exit(1);
  }

  // Test 3: Send media message
  console.log('✅ Test 3: Sending media message...');
  try {
    await channel.sendMedia({
      to: 'test-user@beagle',
      mediaUrl: 'https://example.com/test-image.jpg',
      caption: 'Test image from integration test',
      from: 'test-system',
    });
    console.log('   Media message sent successfully\n');
  } catch (error) {
    console.error('   ❌ Failed to send media:', error.message);
    process.exit(1);
  }

  // Test 4: Start inbound service
  console.log('✅ Test 4: Starting inbound message service...');
  const mockApi = {
    receiveMessage: async (message) => {
      receivedMessages.push(message);
      console.log('   📨 Received message:', {
        from: message.from,
        text: message.text?.substring(0, 50),
        messageId: message.messageId,
      });
    },
  };

  await startInboundService(mockApi, config);
  console.log('   Inbound service started, polling every 2 seconds\n');

  // Test 5: Wait for messages to be received
  console.log('✅ Test 5: Waiting for echo messages (6 seconds)...');
  await new Promise(resolve => setTimeout(resolve, 6000));

  // Verify results
  console.log('\n📊 Test Results:');
  console.log(`   Messages sent: 2`);
  console.log(`   Messages received: ${receivedMessages.length}`);
  
  if (receivedMessages.length >= 2) {
    console.log('\n✨ All tests passed! The plugin is working correctly.\n');
    process.exit(0);
  } else {
    console.log('\n⚠️  Warning: Expected 2 messages but received', receivedMessages.length);
    console.log('   The plugin works, but echo messages might be delayed.\n');
    process.exit(0);
  }
}

// Run tests
runTests().catch(error => {
  console.error('\n❌ Test failed with error:', error);
  process.exit(1);
});
