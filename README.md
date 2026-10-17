[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/masterroshihehe)

# Sims4VR

A full 6DoF virtual reality mod for The Sims 4 (PC, DirectX 11). You play in first person
inside your Sim's body: walk with the stick, use your own hands to reach, touch and pick things,
and use the game's menus on your wrist, your phone or the objects themselves. You can also
switch back to the normal game camera in VR.

> **This mod is 100% vibe coded.** Every line of code was written by Claude (Anthropic's AI).
> I ([MasterRoshiHehe](https://ko-fi.com/masterroshihehe)) act as the director: I decide what
> the mod should do, test every build in the headset and report what works and what doesn't.
> I don't write the code myself. Expect bugs, and read the code before you trust it.

**Status: early development.** Things change and break often.

## Features

- **First person in your Sim's body:** your own free movement (stick walking, snap/smooth turn),
  your Sim's body and animations follow you, seated and lying views stay in the Sim's head.
- **Hands:** arm IK and finger curls follow your controllers (or your real hands with hand
  tracking), reach-to-interact, grab and hold objects, open doors by hand.
- **Menus in VR:** pie menu at your hand, your own pie menu (poke your stomach), pickers drawn on
  their object (fridge, TV), popups as cards in the world, wrist HUD with time, needs and the
  action queue drawn in the game's own style, a phone menu hub and a build/buy tablet.
- **Living in the world:** Sims turn to face you and follow you in conversations, gaze-based TV
  watching, far voices placed correctly, outdoor sound on porches, speech bubbles in 3D.
- **Debug menu (B button):** almost every setting changes live in the headset and is saved to
  `sims4vr_user.ini`. Pages for other mods ([Integrated Body Physics](https://github.com/MasterRoshiHehe/Sims-4-Integrated-Body-Physics),
  [Extended Sound Control](https://github.com/MasterRoshiHehe/Sims-4-Extended-Sound-Control)) appear only when those mods are installed.
- **Game camera mode:** the regular god view, in VR.

## Requirements

- The Sims 4 on PC, **DirectX 11** (the main menu shows "DX11" next to the version).
  Supported game version: 1.128.90.1030. Other versions turn off the features that need game addresses.
- An OpenXR headset runtime. Tested on Quest 3 with Virtual Desktop (VDXR). SteamVR and Quest Link
  should work but are not tested right now.
- Script mods enabled in the game (Options > Game Options > Other).

## Install

1. There is no release yet: the mod is in early development. When there is one, download the zip from the [Releases](../../releases) page. Until then, build it from source (below).
2. Unzip it into `Documents/Electronic Arts/The Sims 4/Mods/` so you get `Mods/sims4vr/`.
3. Make your VR runtime the active OpenXR runtime (in Virtual Desktop: pick VDXR).
4. Start the game with the headset connected.

Settings live in `sims4vr.ini`. The in-headset debug menu (B) writes your changes to
`sims4vr_user.ini`, which wins over `sims4vr.ini`. Delete it to go back to the defaults.

## Building from source

The mod is cross-compiled on Linux with MinGW-w64 (posix threads) and CMake. The script mod
needs Python 3.7 to compile its `.pyc` files (the game runs Python 3.7).

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=tools/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The output folder `build/package/sims4vr/` is the ready-to-run mod. OpenXR SDK, MinHook and
Dear ImGui are downloaded by CMake.

## Layout

- `src/` the DLL: OpenXR session on the game's D3D11 device, camera, body, hands, menus, HUD.
- `scriptmod/sims4vr/` the Python script mod: gameplay side (movement rules, interactions,
  conversations, menus) and the bridge to the DLL.
- `config/sims4vr.ini` default settings.
- `tools/` build helpers.

## License

MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licenses: OpenXR SDK
(Apache 2.0), MinHook (BSD 2-Clause), Dear ImGui (MIT).

The Sims 4 is a trademark of Electronic Arts Inc. This is a fan-made mod and is not affiliated
with or endorsed by Electronic Arts.
