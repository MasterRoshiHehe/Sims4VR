#pragma once

// First person: flying the game's Streaming Drone yourself ([drone] enabled, user 2026-10-28).
// Python (scriptmod drone.py) sees you start a drone interaction, stops the drone's own routing and calls start().
// From then on the camera hangs off a free flight point instead of your Sim: the left stick flies where you look,
// the right stick turns (as always) and lifts (up/down), A / X lands. This module only integrates the point and
// reads the sticks; Python moves the drone object to the point several times a second.
// Nothing here runs outside first person: the camera asks active() only with a body followed.
namespace s4vr::drone {

// Python (any thread): pilot from here (world units); the view keeps its heading.
void start(const float position[3]);
// Python, or first person ending: back to your Sim.
void stop();
bool active();

// Python, ten times a second: the lowest and highest the point may fly (world y, units).
void setLimits(float lowest, float highest);
// Controls thread, every frame: the left stick as a world direction (x, z) and strength 0..1 (what walkInput gives),
// the right stick's up/down (-1..1; positive climbs), and whether A / X was pressed this frame.
void input(float x, float z, float strength, float lift, bool land);

// Game thread, once per drawn pair: advance by dt seconds (dt <= 0.1) and give the flight point (world units).
// scale = game units per metre. False when not flying.
bool update(float dt, float scale, float out[3]);

// Python: x y z of the flight point, speed (m/s), and whether you asked to land (A / X). False when not flying.
bool pose(float out[5]);

// [drone] speed / lift_speed (m/s), smoothing (seconds to reach the stick's speed), boost (factor, left stick held fully).
void configure(float speed, float liftSpeed, float smoothing, float boost);

}  // namespace s4vr::drone
