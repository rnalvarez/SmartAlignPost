# Smart Align Post V1 design

## Objective

Create a public, open-source VST3 foundation for microphone alignment with production-sound use cases in mind.

### User model

1. Feed one master microphone and one or more source microphones into the plug-in.
2. Choose STATIC or DYNAMIC.
3. Analyze the complete material.
4. Inspect delay and confidence.
5. Apply the result.

## Static

One delay value is estimated from a representative window. No tracking occurs.

## Dynamic

The material is split into overlapping windows. Each window is analyzed independently. Low-confidence estimates are rejected. Accepted estimates are slew-limited and smoothed.

## V1 engineering decision

The alignment engine is independent of VST3. This is deliberate.

A standard VST3 plug-in does not expose a portable API for:
- enumerating selected DAW timeline items,
- reading arbitrary source media files,
- replacing item offsets,
- committing destructive item edits.

Therefore the first public VST3 prototype uses routed audio and establishes the DSP foundation.

## Next milestone

Create a host-specific REAPER bridge or ARA2 layer that:
- reads selected media items,
- identifies the master,
- feeds complete buffers to `AlignEngine`,
- renders aligned copies/non-destructive offsets,
- supports Undo.

This keeps the public DSP core reusable by other DAWs.
