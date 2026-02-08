# Contributing to Super Timecode Converter

Thanks for your interest in contributing! Here's how you can help.

## Reporting Bugs

- Open an [issue](https://github.com/fiverecords/SuperTimecodeConverter/issues) with a clear title
- Include your OS version, audio driver type (WASAPI/ASIO), and device details
- Describe the steps to reproduce the problem
- Include the contents of `%APPDATA%/SuperTimecodeConverter/settings.json` if relevant

## Suggesting Features

- Open an issue with the `enhancement` label
- Describe the use case and why it would be useful

## Pull Requests

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Make your changes
4. Test thoroughly with real timecode sources if possible
5. Commit with clear messages (`git commit -m "Add XYZ feature"`)
6. Push and open a Pull Request

### Code Style

- Follow the existing code style (JUCE conventions)
- Use `camelCase` for variables and methods
- Use `PascalCase` for classes and structs
- Keep header files self-contained (include what you use)
- Prefer lock-free patterns for anything in the audio callback path

### Audio Thread Safety

Any code that runs in `audioDeviceIOCallbackWithContext` must be **real-time safe**:
- No memory allocation
- No locks (use atomics or lock-free structures)
- No system calls

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
