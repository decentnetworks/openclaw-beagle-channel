/**
 * Advanced Example: Beagle Channel Runtime
 * 
 * This example shows how to use the BeagleChannelRuntime
 * which provides a higher-level interface.
 */

const { BeagleChannelRuntime } = require('../dist/extension/runtime.js');

async function main() {
    console.log('=== Beagle Channel Runtime Example ===\n');

    // Create runtime instance
    const runtime = new BeagleChannelRuntime();

    // Configuration
    const config = {
        enabled: true,
        dataDir: './runtime-example-data',
        userName: 'OpenClaw Agent',
        userDescription: 'AI Assistant via Beagle',
    };

    console.log('1. Initializing runtime...');
    await runtime.initialize(config);
    console.log('   ✓ Runtime initialized\n');

    // Set up message handler
    console.log('2. Setting up message handler...');
    runtime.onMessage((message) => {
        console.log('   [MESSAGE RECEIVED]');
        console.log(`   From: ${message.from}`);
        console.log(`   To: ${message.to}`);
        console.log(`   Text: ${message.text}`);
        console.log(`   Timestamp: ${new Date(message.timestamp).toISOString()}\n`);
    });
    console.log('   ✓ Message handler configured\n');

    // Set up friend request handler
    console.log('3. Setting up friend request handler...');
    runtime.onFriendRequest((userId, name, hello) => {
        console.log('   [FRIEND REQUEST]');
        console.log(`   User: ${name} (${userId})`);
        console.log(`   Message: ${hello}\n`);
        
        // In a real implementation, you might:
        // - Check if userId is in allowlist
        // - Prompt user for approval
        // - Auto-accept based on policy
    });
    console.log('   ✓ Friend request handler configured\n');

    // Set up connection handler
    console.log('4. Setting up connection handler...');
    runtime.onConnectionChanged((connected) => {
        console.log(`   [CONNECTION] Status: ${connected ? 'ONLINE' : 'OFFLINE'}\n`);
    });
    console.log('   ✓ Connection handler configured\n');

    // Start the runtime
    console.log('5. Starting runtime...');
    await runtime.start();
    console.log('   ✓ Runtime started\n');

    // Display connection info
    console.log('6. Connection Information:');
    console.log(`   Address: ${runtime.getAddress()}`);
    console.log(`   User ID: ${runtime.getUserId()}`);
    console.log(`   Ready: ${runtime.isReady()}\n`);

    // Add a friend
    console.log('7. Adding a friend...');
    const friendAddress = 'example_carrier_address_abc123';
    await runtime.addFriend(friendAddress, 'Hello! Connect with my OpenClaw agent.');
    console.log('   ✓ Friend request sent\n');

    // Send a message
    console.log('8. Sending a message...');
    const friendId = 'example_friend_id_xyz789';
    const sent = await runtime.sendMessage(friendId, 'How can I assist you today?');
    console.log(`   ${sent ? '✓' : '✗'} Message ${sent ? 'sent' : 'failed'}\n`);

    // Simulate some activity
    console.log('9. Waiting for events (5 seconds)...\n');
    await new Promise(resolve => setTimeout(resolve, 5000));

    // Shutdown
    console.log('10. Shutting down...');
    await runtime.stop();
    console.log('    ✓ Runtime stopped\n');

    console.log('=== Example Complete ===');
}

// Run the example
main().catch(console.error);
