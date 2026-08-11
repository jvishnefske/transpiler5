export const meta = {
  name: 'superdev',
  description: 'One design.md increment through the house protocol: verify anchors, spike GO/NO-GO, tests-first implementation, oracle gate triage',
  whenToUse: 'Invoke with args = a one-paragraph increment description (FR/wave reference + intent, e.g. "61f descending loops -> .rev()"). The workflow stops on NO-GO. Committing and design.md stay with the caller.',
  phases: [
    { title: 'Anchor', detail: 'verify file:line anchors and current behavior' },
    { title: 'Spike', detail: 'code experiments, GO/NO-GO with evidence' },
    { title: 'Implement', detail: 'tests first, iterate to green' },
    { title: 'Gate', detail: 'fast tier + full suite, triage' },
  ],
}

// The build dir is shared and the tree is iCloud-slow: every stage is
// sequential by design. Do not add parallel agents that build or run lit.

const task = typeof args === 'string' ? args : JSON.stringify(args ?? '')
if (!task)
  throw new Error(
    'superdev needs args: the increment description (FR/wave ref + intent)')

const VERDICT = {
  type: 'object',
  required: ['verdict', 'evidence'],
  properties: {
    verdict: { type: 'string', enum: ['GO', 'NO-GO', 'GO-WITH-CONSTRAINTS'] },
    evidence: { type: 'string', description: 'what was probed and measured' },
    constraints: { type: 'string' },
    spec_additions: {
      type: 'string',
      description: 'anchors/shapes the spike adds to the implementation spec',
    },
  },
}

phase('Anchor')
const anchors = await agent(
  `Repo: the emitrust transpiler (CLAUDE.md protocol applies). Task: ${task}\n` +
    'Locate and VERIFY every implementation anchor this task needs: the ' +
    'design.md entry (quote its still-out/frontier lines), the functions and ' +
    'file:line sites to be touched, the sibling tests whose style the new ' +
    'tests must mirror, and the CURRENT behavior of the motivating input ' +
    '(run build/tools/emitrust-import-c or emitrust-cc --emit=mlir on a probe ' +
    'file; quote the exact rejection wording or lowering). Return a compact ' +
    'anchor sheet: verified file:line list, current-behavior quotes, sibling ' +
    'test names, and any discrepancy between design.md and the code as it is.',
  { label: 'anchor', phase: 'Anchor' })

phase('Spike')
const spike = await agent(
  `Task: ${task}\n\nVerified anchor sheet from the scout:\n${anchors}\n\n` +
    'De-risk this increment per your charter (hand-driven emission byte-diff ' +
    'against the native binary, dialect round-trips through ' +
    'emitrust-translate/emitrust-opt, adversarial probes). Resolve the ' +
    'unknowns the anchor sheet exposes.',
  { agentType: 'spike', label: 'spike', phase: 'Spike', schema: VERDICT })

if (spike.verdict === 'NO-GO') {
  log('Spike returned NO-GO — stopping before implementation.')
  return { spike, anchors }
}

phase('Implement')
const impl = await agent(
  `Task: ${task}\n\nVerified anchor sheet:\n${anchors}\n\nSpike verdict: ` +
    `${spike.verdict}\nSpike evidence: ${spike.evidence}\n` +
    `Constraints: ${spike.constraints ?? 'none'}\nSpec additions: ` +
    `${spike.spec_additions ?? 'none'}\n\n` +
    'Implement per your charter: tests FIRST in the house style (intent ' +
    'comment, sibling RUN-line patterns, exact probed rejection wordings), ' +
    'then the code, iterating single lit tests and the fast tier until every ' +
    'test you wrote plus the untouched neighbors pass. Do not commit; do not ' +
    'touch design.md.',
  { agentType: 'tdd-implementer', label: 'implement', phase: 'Implement' })

phase('Gate')
const gate = await agent(
  'A tests-first increment was just implemented (report below). Run the ' +
    'fast tier first; if green, run the FULL meson suite. Triage any failure ' +
    'per your charter (remember the exit-137 OOM signature).\n\n' +
    `Implementation report:\n${impl}`,
  { agentType: 'oracle-gate', label: 'gate', phase: 'Gate' })

return { spike, impl, gate, anchors }
