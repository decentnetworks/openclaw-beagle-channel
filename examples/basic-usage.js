/**
 * Basic Example: Using the Beagle Carrier
 * 
 * This example demonstrates the basic usage of the Beagle Carrier
 * with the mock implementation (no actual SDK required).
 */

const { BeagleCarrier } = require('../dist/src/beagle-carrier.js');

async function main() {
    console.log('=== Beagle Carrier Basic Example ===\n');

    // Create a new Carrier instance
    const carrier = new BeagleCarrier();

    // Initialize with configuration
    console.log('1. Initializing Carrier...');
    const initialized = carrier.initialize({
        dataDir: './example-data',
        bootstrapNodes: '[]' // Empty for mock, use actual nodes in production
    });

    if (!initialized) {
        console.error('Failed to initialize Carrier');
        return;
    }
    console.log('   ✓ Initialized\n');

    // Set up event handlers
    console.log('2. Setting up event handlers...');

    carrier.onConnectionChanged((connected) => {
        console.log(`   [EVENT] Connection: ${connected ? 'CONNECTED ✓' : 'DISCONNECTED ✗'}`);
        
        if (connected) {
            console.log(`   [INFO] Carrier Address: ${carrier.getAddress()}`);
            console.log(`   [INFO] User ID: ${carrier.getUserId()}\n`);
        }
    });

    carrier.onFriendRequest((userId, name, hello) => {
        console.log(`   [EVENT] Friend Request Received`);
        console.log(`   [INFO] From: ${name} (${userId})`);
        console.log(`   [INFO] Message: "${hello}"\n`);
    });

    carrier.onFriendAdded((userId) => {
        console.log(`   [EVENT] Friend Added: ${userId}\n`);
    });

    carrier.onMessage((friendId, message) => {
        console.log(`   [EVENT] Message Received`);
        console.log(`   [INFO] From: ${friendId}`);
        console.log(`   [INFO] Message: "${message}"\n`);
    });

    console.log('   ✓ Event handlers configured\n');

    // Start the Carrier
    console.log('3. Starting Carrier...');
    const started = carrier.start();
    
    if (!started) {
        console.error('Failed to start Carrier');
        return;
    }
    console.log('   ✓ Started\n');

    // Wait for connection (in mock, this happens immediately)
    await new Promise(resolve => setTimeout(resolve, 1000));

    // Check if ready
    console.log('4. Checking status...');
    console.log(`   Ready: ${carrier.isReady() ? 'YES ✓' : 'NO ✗'}\n`);

    // Set user info
    console.log('5. Setting user information...');
    carrier.setUserInfo('Example Bot', 'Testing the Beagle Carrier');
    console.log('   ✓ User info updated\n');

    // Simulate adding a friend
    console.log('6. Adding a friend...');
    const friendAddress = 'mock_friend_address_12345';
    carrier.addFriend(friendAddress, 'Hello! I am an OpenClaw agent.');
    console.log('   ✓ Friend request sent\n');

    // Simulate sending a message
    console.log('7. Sending a message...');
    const friendId = 'mock_friend_id_67890';
    carrier.sendMessage(friendId, 'Hello from the Beagle Carrier example!');
    console.log('   ✓ Message sent\n');

    // Keep running for a bit to see events
    console.log('8. Running for 3 seconds to observe events...\n');
    await new Promise(resolve => setTimeout(resolve, 3000));

    // Clean shutdown
    console.log('9. Stopping Carrier...');
    carrier.stop();
    console.log('   ✓ Stopped\n');

    console.log('=== Example Complete ===');
}

// Run the example
main().catch(console.error);
