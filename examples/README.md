# Examples

This directory contains example code demonstrating how to use the Beagle Channel.

## Available Examples

### 1. basic-usage.js

Basic usage of the BeagleCarrier class directly.

**Features:**
- Creating a Carrier instance
- Initializing with configuration
- Setting up event handlers
- Starting and stopping the Carrier
- Sending messages and friend requests

**Run:**
```bash
cd /path/to/openclaw-beagle-channel
npm run build
node examples/basic-usage.js
```

### 2. runtime-usage.js

Using the BeagleChannelRuntime for higher-level functionality.

**Features:**
- Runtime initialization and lifecycle
- Message handling
- Friend request management
- Connection monitoring

**Run:**
```bash
cd /path/to/openclaw-beagle-channel
npm run build
node examples/runtime-usage.js
```

## Notes

- These examples use the **mock implementation** and will work without the Elastos Carrier SDK
- To use with the actual SDK, complete the integration steps in `IMPLEMENTATION.md`
- The mock implementation logs actions but doesn't perform real network communication
- All examples are designed to be educational and show best practices

## Creating Your Own Examples

Feel free to create your own examples based on these templates:

1. Import the necessary classes
2. Initialize the Carrier or Runtime
3. Set up event handlers
4. Perform operations (send messages, add friends, etc.)
5. Clean up properly

## Troubleshooting

### "Cannot find module" errors

Make sure to build the TypeScript code first:
```bash
npm run build
```

### Examples don't do anything

The mock implementation logs to console but doesn't perform real operations.
Check that you see log output in the console.

### Want to test with real Carrier network

Follow the instructions in `IMPLEMENTATION.md` to integrate the actual Elastos Carrier SDK.
