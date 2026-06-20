# Spike NN - <Title>

- Type: STATIC | DUMP | RUN
- Status: DONE | PARTIAL | BLOCKED
- Save: c
- Branch commit: <filled at commit>

## Goal

What question this spike answers and why it matters for co-op.

## Method

What the probe does. For RUN/DUMP, the exact reproduce command, e.g.:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id NN -Save c
```

For STATIC, the headers/files examined.

## Raw evidence

Key `SPIKE NN ...` log lines or header excerpts (full logs in `NN/raw/`).

## Findings

The answer, stated plainly.

## Implications for co-op

How this changes the design / what it unblocks or rules out.

## Recommended follow-ups

Concrete next steps. If the spike produced a keeper primitive, note it here (it is
NOT left in the tree - the code was reverted).
