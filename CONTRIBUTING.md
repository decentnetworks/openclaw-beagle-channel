# Contributing to openclaw-beagle-channel

Thank you for your interest in contributing to the Beagle Chat channel plugin for OpenClaw!

## Getting Started

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:
   ```bash
   git clone https://github.com/YOUR-USERNAME/openclaw-beagle-channel.git
   cd openclaw-beagle-channel
   ```
3. **Install dependencies**:
   ```bash
   npm install
   ```
4. **Build the project**:
   ```bash
   npm run build
   ```

## Development Workflow

### Making Changes

1. **Create a feature branch**:
   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes** in the `src/` directory

3. **Build and test**:
   ```bash
   npm run build
   ```

4. **Test with the mock sidecar**:
   ```bash
   # Terminal 1: Start mock sidecar
   node examples/mock-sidecar.js
   
   # Terminal 2: Test your changes
   # (use curl or write a test script)
   ```

### Code Style

- Use TypeScript for all source code
- Follow existing code style and conventions
- Add JSDoc comments for public APIs
- Use meaningful variable and function names
- Keep functions focused and concise

### Commit Messages

Use clear and descriptive commit messages:
- Start with a verb in present tense (Add, Fix, Update, etc.)
- Keep the first line under 72 characters
- Add detailed description if needed

Examples:
```
Add support for message reactions
Fix polling interval configuration bug
Update README with new deployment steps
```

### Testing

Before submitting a PR:

1. **Ensure the code builds** without errors:
   ```bash
   npm run build
   ```

2. **Test with the mock sidecar**:
   ```bash
   node examples/mock-sidecar.js
   ```

3. **Verify TypeScript types** are correct:
   ```bash
   npx tsc --noEmit
   ```

## Submitting Changes

1. **Push your changes** to your fork:
   ```bash
   git push origin feature/your-feature-name
   ```

2. **Create a Pull Request** on GitHub:
   - Provide a clear title and description
   - Reference any related issues
   - Describe what changes you made and why

3. **Wait for review**:
   - Address any feedback from reviewers
   - Make requested changes in new commits
   - Push updates to your branch

## Areas for Contribution

### Features
- WebSocket support for real-time message delivery
- Message acknowledgment and delivery receipts
- Group chat support
- File upload progress tracking
- Offline message queuing

### Documentation
- Additional usage examples
- Video tutorials
- Integration guides for specific platforms
- Translation to other languages

### Testing
- Unit tests for channel and service modules
- Integration tests with real sidecar
- Performance benchmarks
- Load testing scripts

### DevOps
- Docker containerization
- CI/CD pipeline improvements
- Kubernetes deployment guides
- Monitoring and alerting setup

## Code of Conduct

### Our Standards

- Be respectful and inclusive
- Welcome newcomers and beginners
- Accept constructive criticism gracefully
- Focus on what's best for the community
- Show empathy towards others

### Unacceptable Behavior

- Harassment or discrimination
- Trolling or insulting comments
- Public or private harassment
- Publishing others' private information
- Other unprofessional conduct

## Questions?

If you have questions or need help:

- Open a [GitHub Issue](https://github.com/0xli/openclaw-beagle-channel/issues)
- Check existing issues and discussions
- Read the [README](README.md) and [DEPLOYMENT](DEPLOYMENT.md) guides

## License

By contributing to this project, you agree that your contributions will be licensed under the MIT License.
