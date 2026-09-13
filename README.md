# Smart Align Post

**Open-source microphone alignment engine and VST3 prototype for production-sound post-production.**

The project is inspired by workflows such as Sound Radix Auto-Align Post, but it is an independent implementation with a deliberately DAW-independent DSP core.

## Project goal

Align one or more production microphones against a selected reference microphone, typically:

- BOOM / MASTER
- LAV 1
- LAV 2
- LAV 3...

Two analysis modes are planned from the beginning:

- **STATIC** — calculate one fixed time offset.
- **DYNAMIC** — calculate a time-offset curve offline and make conservative small corrections when the acoustic relationship changes during movement.

The long-term user workflow is:

> Select items → choose MASTER → choose STATIC/DYNAMIC → CALCULATE → inspect → APPLY → listen → Undo.

## What this first public delivery contains

### Implemented

- Independent C++17 alignment engine.
- Normalized cross-correlation delay estimation.
- Search-range limiting.
- Confidence estimation.
- Confidence gating.
- Dynamic temporal smoothing.
- Maximum slew-rate limiting.
- Static and dynamic analysis results.
- VST3 plug-in shell.
- Cross-platform CMake project.
- DSP unit tests.
- GitHub Actions build matrix.
- REAPER selection helper prototype.

### Not yet implemented

This is important: **the VST3 shell in this delivery is not yet the finished offline item-aligning plug-in.**

A normal VST3 plug-in does not have portable access to the host's timeline item-selection API. Therefore the final workflow needs a host integration layer (for example ARA2 where appropriate, or a REAPER-specific bridge) in addition to the VST3 DSP engine.

The current plug-in is intentionally a pass-through shell while the DSP engine is tested independently. This avoids presenting an unverified prototype as a finished Auto-Align replacement.

## Repository structure

```text
SmartAlignPost/
├── src/                 # VST3 wrapper + DSP engine
├── tests/               # deterministic DSP tests
├── reaper/              # REAPER-specific helper prototypes
├── docs/                # design and implementation notes
├── extern/vst3sdk/      # Steinberg VST3 SDK git submodule
├── .github/workflows/   # CI builds
├── CMakeLists.txt
├── LICENSE
├── THIRD_PARTY_LICENSES.md
└── CONTRIBUTING.md
```

## Build

The project uses the official Steinberg VST3 SDK as a Git submodule. Steinberg's current SDK documentation supports CMake-based builds and lists Windows, macOS and Linux targets. See the official SDK repository and developer portal for platform-specific requirements. 

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/YOUR_USER/SmartAlignPost.git
cd SmartAlignPost
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

If the repository was already cloned:

```bash
git submodule update --init --recursive
```

### DSP-only build

The alignment engine tests do not require the VST3 SDK:

```bash
cmake -S . -B build-dsp -DSAP_BUILD_VST3=OFF -DSAP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --config Release
ctest --test-dir build-dsp --output-on-failure
```

## REAPER

The current `reaper/` helper only inspects selected items and demonstrates the intended MASTER/SOURCE selection convention. It does not modify media yet.

For the next milestone, the REAPER integration should become the offline host bridge that:

1. reads the selected items;
2. identifies MASTER and SOURCES;
3. extracts the relevant audio;
4. runs the shared alignment engine offline;
5. creates a non-destructive correction representation;
6. supports preview/apply/undo.

## Algorithm notes

The current engine deliberately starts conservatively. It estimates time offset using normalized correlation and then, in dynamic mode, applies confidence gating, outlier resistance, smoothing and a maximum slew rate.

Spectral phase correction is intentionally **not** part of this first delivery.

## License

Project code: MIT. See `LICENSE`.

The VST3 SDK is a separate third-party dependency and remains under its own license. See `THIRD_PARTY_LICENSES.md` and the SDK repository.
