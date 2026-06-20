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

State ONLY conclusions that are backed by the Validation section below. Every
finding here must be something we actually observed or verified - not inferred,
predicted, or assumed. If it isn't validated, it does not go here.

## Validation

REQUIRED. For EACH finding above, state exactly how it was validated, so a reader
can trust (and reproduce) it. Use one line per finding, naming the evidence:

- Runtime: the specific `SPIKE NN ...` log line(s) on host AND/OR join that prove it
  (quote them or cite `NN/raw/host.log` line), what was compared, and the result.
- Code: the exact `path:line` (or symbol) that establishes it, and why it follows.
- Header/SDK: the exact `header:line` field/method that establishes it.

A finding with no validation entry is not allowed - delete it or move it to
"Open questions / hypotheses".

## Open questions / hypotheses (UNVALIDATED)

Anything we suspect but did NOT validate goes here, clearly labelled as unverified,
with the test that WOULD validate it. Never promote these to Findings without
evidence. (If this section carries the spike's core question, Status is PARTIAL.)

## Implications for co-op

How the VALIDATED findings change the design / what they unblock or rule out.

## Recommended follow-ups

Concrete next steps, including the runtime test that would validate any open
hypothesis above. If the spike produced a keeper primitive, note it here (it is NOT
left in the tree - the code was reverted).
