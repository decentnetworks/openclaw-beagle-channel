# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2024-02-10

### Added
- Initial release of OpenClaw Beagle Chat channel plugin
- Outbound text message support via `sendText` method
- Outbound media support via `sendMedia` method (URL and local path)
- Inbound message polling service via `/events` endpoint
- Configuration schema for `sidecarBaseUrl`, `authToken`, and `pollInterval`
- Complete TypeScript type definitions
- Comprehensive README with installation and usage instructions
- Detailed DEPLOYMENT.md for Ubuntu systemd setup
- Production-ready systemd service template
- Example configuration file (config.example.yaml)
- Mock sidecar for development and testing
- Usage examples demonstrating plugin features

### Features
- HTTP-based communication with Beagle Carrier sidecar
- Configurable polling interval for inbound messages
- Authentication token support for API security
- Error handling and logging for debugging
- Support for both development (macOS) and production (Ubuntu) environments

### Documentation
- Complete README with:
  - Installation instructions for macOS and Ubuntu
  - Configuration options
  - Usage examples
  - API endpoint specifications
  - Troubleshooting guide
- DEPLOYMENT.md with:
  - Step-by-step Ubuntu deployment guide
  - systemd configuration
  - Security hardening recommendations
  - Monitoring and maintenance procedures
- Inline code documentation with JSDoc comments

### Developer Tools
- Mock sidecar HTTP server for local testing
- Example usage code in JavaScript
- TypeScript declarations for IDE support
- Build scripts for compilation

[1.0.0]: https://github.com/0xli/openclaw-beagle-channel/releases/tag/v1.0.0
