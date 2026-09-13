# Contributing

Smart Align Post is an open-source research/development project for microphone alignment in production sound post-production.

## Development priorities

1. Deterministic offline analysis.
2. Conservative dynamic correction.
3. Clear confidence reporting.
4. No destructive modification of original media.
5. DAW-independent DSP core.

## Before opening a pull request

- Build the project.
- Run `ctest --test-dir build --output-on-failure`.
- Document any algorithmic change.
- Do not add proprietary SDKs, media, or test recordings to the repository.

## Scope

The DSP engine is intentionally independent from the VST3 wrapper. Host-specific integrations may live under `reaper/` or future host-specific directories.
