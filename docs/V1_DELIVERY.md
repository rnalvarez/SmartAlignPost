# V1 public delivery checklist

## What this delivery proves

- The DSP engine compiles independently of the VST3 SDK.
- The deterministic DSP test passes.
- The repository is structured for a public GitHub project.
- The Steinberg SDK remains an external Git submodule.
- The VST3 wrapper is deliberately kept separate from the alignment engine.

## What this delivery does not claim

It is not yet a finished Auto-Align Post replacement and it does not yet implement item-aware offline APPLY in REAPER.

## Next implementation milestone

Build the shared offline analysis API around complete clips/items, then add the REAPER host bridge and VST3/ARA-facing layer without changing the DSP core.
