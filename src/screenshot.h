#pragma once
#include <d3d11.h>
#include <string>

// Screenshots of what the headset shows (right stick click): both eyes side
// by side in one timestamped JPEG under <mod folder>\screenshots, plus the menu
// layer in its own file when one is up. Half resolution per eye, so the folder
// stays small; good enough to see what looks wrong. Names listed in
// screenshots\resolved.txt are deleted at the next start.
namespace s4vr::screenshot {

enum Slot { kLeftEye = 0, kRightEye = 1, kFlatScreen = 2, kMenu = 3, kSlots = 4 };

void init(const std::wstring& moduleDir);
// Any thread: take a screenshot of the next frames.
void request();
// Whether a screenshot is being taken now (render thread).
bool taking();
// Render thread: an image just drawn for the headset (the swapchain image,
// still acquired). Copied only while a screenshot is being taken.
void offer(ID3D11DeviceContext* context, ID3D11Texture2D* image, int slot);
// Render thread, once a frame: reads the copies back and writes the file on
// a worker thread.
void pump(ID3D11DeviceContext* context);

}  // namespace s4vr::screenshot
