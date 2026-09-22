# Smart Align Post — Recovery Checkpoint

This branch is intentionally based on commit 0cea55fc52c783a2c356d0c312a14bfab9c87f54.

## Scope

Phase 1 restores a controlled PRIMARY alignment path before reintroducing DYNAMIC refinements or the fine-alignment VST.

The recovery sequence is:

1. PHASE BATCH baseline
2. deterministic PRIMARY verification
3. BOOM/LAV acceptance cases
4. DYNAMIC reintroduction
5. residual measurement
6. VST fractional fine correction
7. final verification

## Rules for this branch

- Do not modify `main`.
- Do not merge the unified scene-aware/residual architecture into the recovery baseline.
- Do not use the VST to decide the primary alignment.
- Do not use the residual pass to perform another primary alignment.
- Every APPLY change must have a measurable post-apply verification.
- MASTER must remain untouched.
- `D_POSITION` remains unchanged unless the correction cannot be represented by the take source offset and the dedicated fallback explicitly requires a position move.

## Primary acceptance contract

For a known static-delay fixture:

`MASTER -> SOURCE -> ANALYZE -> APPLY -> VERIFY`

must produce a post-apply residual within the declared tolerance.

The first real-material acceptance case is the BOOM/LAV direct-arrival/onset case. It is treated separately from statistical redundancy: one excellent direct-arrival measurement is valid evidence even when a long take does not provide many independent support windows.

## Out of scope for the first recovery checkpoint

- residual auto-correction
- Take FX insertion
- fractional VST correction
- scene-aware residual architecture
- further DYNAMIC tuning
- confidence-threshold experiments

These are deliberately postponed until PRIMARY is stable again.
