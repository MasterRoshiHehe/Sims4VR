# Research: The Sims 4 VR mod (kit research pass, 2026-11-26)

Retrospective pass: the mod already exists (v0.13.x). This records what is known, with sources, and what the kit says is missing. Detailed notes live on the project drive (docs/, re/) and Linear team GLA; they are not in this repo.

## 1. Existing mods and frameworks
- convexvr/sims4-vr (Apache-2.0, DX9 hooks, AOB camera patterns): prior art, broke on current game; we do not build on it. Source: https://github.com/krystal-large/sims4-vr (fork).
- Direct Controls / RTBP: never copy code (project rule).
- Our own: C++ DLL (OpenXR, D3D11 Present hook) plus Python 3.7 script mod loader. Reference cores considered: REFramework/vrframework, xivr.

## 2. Loading, graphics API, publisher guidance
- Game: TS4_x64.exe, DX11 only, Python 3.7 embedded (no _ctypes; native module loaded as extension).
- Mods load from the user's Mods folder (MO2 profile "VR test"). DLL injection is a grey area in the EA user agreement; the mod is free, single player, ships no game files.
- Publisher policy to read and record (not yet read line by line, user decides scope): https://help.ea.com/en/articles/the-sims/the-sims-4/mods-policy/ (found 2026-11-26). Unresolved: whether DLL-based mods are covered.
- Anti-cheat: none known for single player.

## 3. Camera, projection, frame boundary
- Camera copy function at TS4_x64 rva 0x310410 (build 1.129.39.1030 via the per-build address table, src/addrs_table.inc). Camera position float3 at +0x60.
- Frame boundary: D3D11 Present hook (d3d11_hook.cpp). Render passes per eye: second pass costs 3.5-5 ms (project STATE notes).
- Open: cameras that own their own frustum (shadow renderer, mirrors) are the suspected cause of shadow tick (GLA-54) and broken mirrors/pool reflection.

## 4. Runtime and stereo
- OpenXR via VDXR, Quest 3 through Virtual Desktop. Stereo: both real eyes every frame is the default; alternate-eye and flat-world modes were tried and dropped (project STEREO-FEASIBILITY).
- Kit says same-frame stereo is the target. Already so.

## 5. Gameplay contexts and hooks
- First-person free-controller movement, hand IK, phone, pie menus, build mode, CAS. Camera and input chosen separately. See project docs MENU-PATHS and INTERACTION-REVIEW. CONTROLS.md does not exist yet: gap.

## 6. First flat-screen experiment
- Done long ago (camera shift test, v0.1.x). Not repeated.

## 7. Rendering baseline and gates (gaps against the kit)
- Hardware tested: RTX 4080 Super, Quest 3, VDXR only. NVIDIA-only, one runtime: everything else is untested.
- Missing versus kit: per-eye flicker measurement, both-eye image checks (frame_check), per-eye call-count audit of hooks, workaround register, pose-age and delivery-gap metrics, crash ledger.
- Proposed next steps, each its own Linear issue: (a) count hook calls per eye, mono vs stereo, to find per-frame work run twice (shadow tick candidate); (b) shadow-fit/light-selection disagreement log between the two passes; (c) log pose age at submit and frames over 50 ms; (d) register broad workarounds in STATUS.md.
