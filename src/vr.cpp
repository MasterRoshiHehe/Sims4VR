#include "vr.h"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "armik.h"
#include "camera.h"
#include "player.h"
#include "crashlog.h"
#include "log.h"
#include "gamepad.h"
#include "gpuload.h"
#include "nearplane.h"
#include "pointer.h"
#include "renderscale.h"
#include "screenshot.h"
#include "settings.h"
#include "sharpen.h"
#include "bodylayer.h"
#include "eyelids.h"
#include "hud.h"
#include "nodim.h"
#include "eyealign.h"
#include "grab.h"
#include "handshape.h"
#include "touch.h"
#include "bubbles.h"
#include "uihook.h"
#include "phone3d.h"
#include "simsforever.h"
#include "claudescreen.h"
#include "voice.h"
#include "singleview.h"
#include "deptheye.h"
#include "benchmark.h"
#include "texbias.h"

namespace s4vr::vr {

#include "vr_parts/01_xr_state.inc"
#include "vr_parts/02_actions_instance.inc"
#include "vr_parts/03_session.inc"
#include "vr_parts/04_swapchains.inc"
#include "vr_parts/05_laser.inc"
#include "vr_parts/06_uipanel.inc"
#include "vr_parts/07_pie.inc"
#include "vr_parts/08_wrist.inc"
#include "vr_parts/09_hands_input.inc"
#include "vr_parts/10_phone_screen.inc"
#include "vr_parts/11_object_menus.inc"
#include "vr_parts/12_hud.inc"
#include "vr_parts/13_sims1_screen.inc"
#include "vr_parts/14_phone.inc"
#include "vr_parts/15_settings_menu.inc"
#include "vr_parts/16_controls.inc"
#include "vr_parts/17_render.inc"
#include "vr_parts/18_stereo.inc"
#include "vr_parts/18b_frame_clock.inc"
#include "vr_parts/19_present_control.inc"
}  // namespace s4vr::vr
