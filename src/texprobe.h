#pragma once

// Texture/Sim compositing probe ([perf] texture_probe, docs/PERF-PLAN.md section 7): v0.13.281's hitch capture put the
// main thread's ~240 ms stalls inside game+0x19f3210 -> 0x1a03f10 -> 0x1a01840 -> loop at 0x1a3a3c5 (Sim texture /
// deformer code, exe stamp 0x6aa08454). This times those two functions and logs the calls over 20 ms with the object's
// vtable (resolve with re/exe/<stamp>/classes.tsv) so the next step knows what is being rebuilt and when.
// Log only; passes every argument through; 200 lines per session.
namespace s4vr::texprobe {

void install(bool on);

}  // namespace s4vr::texprobe
