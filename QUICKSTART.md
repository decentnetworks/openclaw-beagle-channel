# Quick Start Guide

Get started with the OpenClaw Beagle Channel in 5 minutes.

## What You Need

- Ubuntu 20.04+ (or similar Linux distribution)
- Node.js 22 or higher
- Basic familiarity with terminal/command line

## Installation

### 1. Clone the Repository

```bash
git clone https://github.com/0xli/openclaw-beagle-channel.git
cd openclaw-beagle-channel
```

### 2. Run the Setup Script

```bash
./setup.sh
```

This will:
- Check for required dependencies
- Install Node.js packages
- Build the TypeScript code
- Attempt to build the native addon (may fail without SDK)

### 3. Understanding the Current State

The current implementation provides:
- ✅ Complete project structure
- ✅ TypeScript interfaces and types
- ✅ C++ addon scaffolding
- ✅ Mock implementation for testing
- ⚠️ Not yet connected to actual Elastos Carrier SDK

This means you can:
- Explore the codebase
- Understand the architecture
- See how it integrates with OpenClaw
- Test the TypeScript layer with mock data

## Testing the Mock Implementation

Create a test file `test.js`:

```javascript
const { BeagleCarrier } = require('./dist/src/beagle-carrier.js');

console.log('Creating Beagle Carrier instance...');
const carrier = new BeagleCarrier();

console.log('Initializing...');
carrier.initialize({
    dataDir: './test-data',
    bootstrapNodes: '[]'
});

console.log('Setting up callbacks...');
carrier.onConnectionChanged((connected) => {
    console.log('Connection status:', connected ? 'CONNECTED' : 'DISCONNECTED');
    if (connected) {
        console.log('Carrier Address:', carrier.getAddress());
        console.log('User ID:', carrier.getUserId());
    }
});

carrier.onMessage((from, message) => {
    console.log(`[MESSAGE] From: ${from}`);
    console.log(`[MESSAGE] Text: ${message}`);
});

console.log('Starting carrier...');
carrier.start();

console.log('Sending test message...');
carrier.sendMessage('test_friend_id', 'Hello from OpenClaw!');

// Keep the process running
setTimeout(() => {
    console.log('Test complete');
    carrier.stop();
}, 2000);
```

Run it:

```bash
node test.js
```

You should see output from the mock implementation.

## Next Steps

### To Complete the Integration

1. **Build the Elastos Carrier SDK**
   - Follow the instructions in `IMPLEMENTATION.md`
   - This is the main step to make it functional

2. **Update the C++ Wrapper**
   - Replace stub implementations with real Carrier API calls
   - See detailed instructions in `IMPLEMENTATION.md`

3. **Test with Real Carrier Network**
   - Connect to actual Carrier bootstrap nodes
   - Exchange messages with Beagle clients

### To Integrate with OpenClaw

1. **Install OpenClaw**
   ```bash
   npm install -g openclaw@latest
   ```

2. **Link This Plugin**
   - Configure OpenClaw to load this extension
   - Update OpenClaw's configuration file

3. **Test End-to-End**
   - Start OpenClaw with Beagle channel enabled
   - Connect from Beagle chat client
   - Send messages to your AI agent

## Project Structure

```
openclaw-beagle-channel/
├── extension/           # OpenClaw plugin
│   ├── index.ts        # Plugin entry point
│   └── runtime.ts      # Channel runtime
├── src/
│   ├── addon/          # C++ native addon
│   └── beagle-carrier.ts  # TypeScript wrapper
├── README.md           # Full documentation
├── IMPLEMENTATION.md   # Detailed implementation guide
├── setup.sh           # Setup script
└── package.json       # Project configuration
```

## Key Files to Understand

1. **`extension/index.ts`** - How the plugin integrates with OpenClaw
2. **`extension/runtime.ts`** - Channel runtime and event handling
3. **`src/addon/carrier_wrapper.cc`** - C++ bridge to Carrier SDK
4. **`src/beagle-carrier.ts`** - TypeScript API wrapper

## Common Commands

```bash
# Build TypeScript
npm run build

# Rebuild C++ addon
npm run prebuild && npm run build

# Clean build artifacts
npm run clean

# Run the setup script again
./setup.sh
```

## Getting Help

- **README.md**: Full project documentation
- **IMPLEMENTATION.md**: Detailed integration guide
- **GitHub Issues**: Report bugs or ask questions
- **Elastos Carrier Docs**: [SDK Documentation](https://github.com/0xli/Elastos.NET.Carrier.Native.SDK)

## What's Working vs. What Needs Work

### ✅ Working Now
- Project builds successfully
- TypeScript compiles
- Mock implementation runs
- All interfaces defined
- Documentation complete

### 🚧 Needs Work
- Actual Carrier SDK integration
- Real P2P networking
- Message encryption
- Friend management
- Testing with Beagle clients

## Development Workflow

1. Make changes to TypeScript files in `extension/` or `src/`
2. Run `npm run build` to compile
3. Test changes
4. For C++ changes, rebuild the addon
5. Iterate

## Tips

- Start by understanding the mock implementation
- Read through the code to see the architecture
- Check `IMPLEMENTATION.md` for SDK integration details
- The current code is designed to be educational and extensible

## Troubleshooting

### "Module not found" errors
- Run `npm install` again
- Check that Node.js version is 22+

### Native addon build fails
- This is expected without the Carrier SDK
- The TypeScript code will use mock implementation
- See `IMPLEMENTATION.md` to fix

### Can't connect to Carrier network
- The mock doesn't actually connect
- You need to complete the SDK integration first

## Support

For questions:
1. Check the documentation files
2. Look at example code in the SDK repository
3. Create an issue on GitHub
4. Reference the Elastos Carrier documentation

---

**Ready to dive deeper?** Check out `IMPLEMENTATION.md` for the complete integration guide!
