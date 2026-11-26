# Status

The agents' scratch pad for this mod. Every session reads this first and adds a dated entry to the
Log (newest first) before it stops: what changed, the evidence (commit, run folder, test result),
and what's next. Keep the sections above the Log current. Keep it factual; it's how the next
session (or the other agent) picks up the work.

## Current state

- Runtime: OpenXR via VDXR (Virtual Desktop); Quest 3
- Stereo: [alternate-eye / same-frame]
- Camera mode(s): [embodied / cockpit / external / tabletop / other, per context]
- Input mode(s): [existing controls / tracked pointing / tracked interaction / mixed, per context]
- Capabilities and action mappings: [RESEARCH.md / CONTROLS.md; confirmed hooks and unknowns]
- Publisher guidance and chosen scope: [dated sources in RESEARCH.md; user decision and unknowns]
- Launch for testing: [command]
- Who owns the game install right now: [nobody / session, run folder, deadline]

## Rendering compatibility

- Minimum capabilities: [graphics API/features, formats/resource operations, runtime and memory
  requirements; confirmed versus unknown; vendor-neutral AA/stereo baseline]
- Evidence matrix: [GPU model/driver, OS/runtime/headset, game/mod build, AA, actual input/output
  size, scenes, run IDs, image checks, recovery, performance and human comfort verdict]
- Claims: [tested / experimental / untested / unsupported per combination; missing vendor
  middleware test is separate from real hardware validation]
- Unsupported settings: [visible fallback/reason and how supported stereo resumes]

## Rendering workarounds

One entry per broad workaround; link longer evidence rather than copying logs here.

- [ ] [ID / introduced commit: optimisation disabled or work forced; correctness reproduction;
  exact scope and activation log; A/B switch; costs by representative scene (unknown stays
  unknown); narrower replacement or removal criterion; revisit on new scene types and release]

## Performance and release evidence

- Baselines and budgets: [fixed checkpoints/routes, matched settings and builds, repeated A/B
  runs, timing/throughput/tail/pose-age budgets and configurable regression review triggers]
- Results: [small scene plus applicable dense interior, terrain and busiest gameplay; CPU/GPU
  times, fresh delivered stereo, gaps/drops and pose age; missing metrics and profiler overhead]
- Package audit: [package hash, effective defaults/settings, active workaround IDs, diagnostics
  off, normal-launch and settings/load recovery checks; evidence and untested gaps]

## Waiting on the human

- [ ] Fill remaining [brackets] above (stereo mode, rendering matrix) from the project drive docs: docs/STEREO-FEASIBILITY.md, docs/PERFORMANCE.md.

- [ ] First headset look (once stereo and the camera pass): [open / done, date, findings]
- [ ] [Headset checks that need a person: what to launch, what to do, what to look for]

## Open issues

- [ ] 1.129 not recognised (found 2026-11-26 from logs 910 and 916 via tools/run_report.py): exe stamp 6acff709 vs table 6aa08454 (1.128). Features at fixed addresses stay off: mirror capture, sim cache, texture probe, and the mirror/water oblique-clip override (camera.cpp raw globals 0x280cb7c, 0x280cb71, 0x280cb9c; the 916 log shows "Mirror camera ... counter -1"). Likely contributes to GLA-120 and the mirror/pool reflection problems. Fix is a table port (kit skill vr-mod-game-patches), a user decision because of the no-full-1.129-decompile rule.
- [ ] Shadow reuse hook (shadowreuse.cpp) failed with MH_ERROR_ALREADY_CREATED on the first attempt in logs 901, 910, 916. 901 and 910 retried and turned on; 916 never turned on. Cause of the already-created hook not established (another D3D11 hooker?). Relevant to GLA-54 (shadows tick) only as data: 910 had reuse on and still ticked.
- [ ] Per-eye call-count audit (kit "Stereo side effects") not done: no counter of per-frame hooks run in both passes.

- [ ] [Issue: how to reproduce it, current evidence]

## Crash ledger

| Date | Signature (module+offset or error ID) | Run | Build and settings | Doing what | Linked issue |
|---|---|---|---|---|---|

## Log

### 2026-11-26 Claude: kit set up (kit commit 07424c2)

Added AGENTS.md, CLAUDE.md, STATUS.md, checklists/, .agents/skills and .claude/skills; merged .gitignore. Work list stays on Linear (team GLA) and docs/STATE.md on the project drive. Bracketed fields left unfilled are in "Waiting on the human". Research pass: see RESEARCH.md. Tooling added from the kit: tools/run_report.py (log verdicts and known states), tools/check_addrs.py (raw addresses outside the table), tools/test_tools.py.

### [YYYY-MM-DD] [agent]: [what this session did]

[What changed, with commit and run IDs. Tests run and results. Anything left half done.]
