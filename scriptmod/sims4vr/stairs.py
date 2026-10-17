"""Stairs in first person: you walk up and down them yourself.

The game takes Sims up stairs through a portal: one animation from the bottom
to the top. Here a staircase is just a ramp. Each flight (a straight run of
steps; L- and U-shaped stairs are several, joined by landings) comes from the
staircase's portal lanes:
- the up lane's entry sits 0.309 m before the bottom step and its exit 0.25 m
  past the top one (the game's lane builder, Simulation_x64.dll rva 0x5b1b70),
  so they give the run, its direction and the bottom edge;
- the lines of all lanes give the width (the outer ones sit 0.25 m inside);
- the floor heights at both ends come from each end's own routing surface
  (the lanes' points are not at floor height).
walk.py asks which flight you are on, the ramp height under you, opens the
stair's mouth in the wall check, and switches your level at the ends.
Research notes: /tmp/claude-0/re/stairs_notes.md (container-local).
"""
import math
import time

import services
import terrain

from sims4vr import native

REFRESH = 10.0  # seconds between looking up the lot's stairs again
UP_START_OUT = 0.309  # the up lane starts this far before the bottom step
UP_END_OUT = 0.25  # and ends this far past the top step
SIDE_INSET = 0.25  # the outermost lane lines sit this far inside the sides
ON_HEIGHT = 0.6  # metres between you and the ramp that still count as on it
# Rise over run above which a run of steps is not a straight flight (spiral
# stairs). The game's straight stairs climb at exactly 1.0, so the line sits
# well above that.
STEEP = 1.25

_flights = []
_flights_time = -1e9
_flights_sig = None
_errors = 0


class Flight:
    __slots__ = ('obj_id', 'bx', 'bz', 'dx', 'dz', 'run', 'half_w', 'h0', 'h1', 'low', 'high')

    def coords(self, x, z):
        """(s along the run from the bottom step, l to the side of the centre line)."""
        rx, rz = x - self.bx, z - self.bz
        return rx * self.dx + rz * self.dz, rx * self.dz - rz * self.dx

    def height(self, s):
        return self.h0 + (self.h1 - self.h0) * min(1.0, max(0.0, s / self.run))

    def slope(self):
        """Rise per metre along x and z on the steps."""
        g = (self.h1 - self.h0) / self.run
        return g * self.dx, g * self.dz

    def on(self, x, y, z):
        """On the steps (between the bottom and the top step, within the
        width, at the ramp's height)."""
        s, l = self.coords(x, z)
        return 0.0 < s < self.run and abs(l) < self.half_w and abs(self.height(s) - y) < ON_HEIGHT

    def __repr__(self):
        return 'flight of {:.1f} m, {:.2f} -> {:.2f} high, {:.1f} m wide'.format(
            self.run, self.h0, self.h1, self.half_w * 2)


def surface_key(surface):
    """Comparable and hashable form of a routing surface."""
    if surface is None:
        return None
    return (surface.primary_id, surface.secondary_id, surface.type)


def _height(location):
    p = location.position
    return terrain.get_terrain_height(p.x, p.z, routing_surface=location.routing_surface)


_signature = (-1e9, None)


def portal_signature():
    """The lot's objects with portals (doors, stairs, ladders) and where they
    are, as a comparable value: lists built from them are rebuilt only when
    it changes (building them every few seconds stalled the frame)."""
    global _signature
    now = time.monotonic()
    if now - _signature[0] < 1.0:
        return _signature[1]
    sig = frozenset((obj.id, round(obj.position.x, 1), round(obj.position.y, 1), round(obj.position.z, 1))
                    for obj in services.object_manager().portal_cache_gen())
    _signature = (now, sig)
    return sig


def _stairs_type():
    from routing.portals.portal_data_stairs import _PortalTypeDataStairs
    return _PortalTypeDataStairs


@native.timed('listing stairs', 0.004)
def _build():
    stairs_type = _stairs_type()
    flights = []
    for obj in services.object_manager().portal_cache_gen():
        groups = {}
        try:
            portals = obj.get_portal_instances()
        except Exception:
            continue
        for portal in portals:
            # Plain stairs only; ladders and other "dynamic stairs" subclass it.
            if type(portal.traversal_type) is not stairs_type:
                continue
            a, b = portal.there_entry, portal.there_exit
            if a is None or b is None:
                continue
            dx, dz = b.position.x - a.position.x, b.position.z - a.position.z
            length = math.hypot(dx, dz)
            if length < UP_START_OUT + UP_END_OUT + 0.2:
                continue
            h0, h1 = _height(a), _height(b)
            run = length - UP_START_OUT - UP_END_OUT
            if abs(h1 - h0) > STEEP * run:
                continue  # not one straight flight (spiral stairs): carried.py
            key = (round(math.atan2(dx, dz), 1), round(h0, 1), round(h1, 1),
                   surface_key(a.routing_surface), surface_key(b.routing_surface))
            groups.setdefault(key, []).append((portal, dx / length, dz / length, length, h0, h1))
        for lanes in groups.values():
            first, dx, dz, length, h0, h1 = lanes[0]
            lx, lz = dz, -dx
            a = first.there_entry.position
            offsets = []
            for portal, *_ in lanes:
                for location in (portal.there_entry, portal.back_entry):
                    if location is not None:
                        q = location.position
                        offsets.append((q.x - a.x) * lx + (q.z - a.z) * lz)
            middle = (min(offsets) + max(offsets)) / 2
            f = Flight()
            f.obj_id = obj.id
            f.dx, f.dz = dx, dz
            f.bx = a.x + dx * UP_START_OUT + lx * middle
            f.bz = a.z + dz * UP_START_OUT + lz * middle
            f.run = length - UP_START_OUT - UP_END_OUT
            f.half_w = (max(offsets) - min(offsets)) / 2 + SIDE_INSET
            f.h0, f.h1 = h0, h1
            f.low, f.high = first.there_entry.routing_surface, first.there_exit.routing_surface
            flights.append(f)
    return flights


def flights():
    """Every flight of plain stairs on the lot (looked up every few seconds)."""
    global _flights, _flights_time, _flights_sig, _errors
    now = time.monotonic()
    if now - _flights_time > REFRESH:
        _flights_time = now
        try:
            sig = portal_signature()
            if sig == _flights_sig:
                return _flights
            _flights_sig = sig
            found = _build()
            if len(found) != len(_flights):
                native._log('stairs: {} flights on the lot{}'.format(
                    len(found), ': ' + '; '.join(repr(f) for f in found[:6]) if found else ''))
            _flights = found
        except Exception as e:
            _errors += 1
            if _errors <= 5:
                native._log('stairs: listing stairs failed: {!r}'.format(e))
    return _flights


def forget():
    global _flights, _flights_time, _flights_sig
    _flights = []
    _flights_time = -1e9
    _flights_sig = None


def flight_under(x, y, z):
    """The flight you are on, or None."""
    best, best_dy = None, ON_HEIGHT
    for f in flights():
        if f.on(x, y, z):
            s, _ = f.coords(x, z)
            dy = abs(f.height(s) - y)
            if dy < best_dy:
                best, best_dy = f, dy
    return best


def ramp(x, y, z, reach=0.6):
    """The flight whose ramp carries you here: on its steps, or at one of its
    ends within reach and level with that end. None elsewhere."""
    best, best_dy = None, 0.3
    for f in flights():
        s, l = f.coords(x, z)
        if not (-reach < s < f.run + reach and abs(l) < f.half_w):
            continue
        dy = abs(f.height(s) - y)
        limit = ON_HEIGHT if 0.0 < s < f.run else 0.3
        if dy < limit and (best is None or dy < best_dy):
            best, best_dy = f, dy
    return best


def near(x, y, z, reach):
    """Flights whose run comes within reach of you at your height."""
    found = []
    for f in flights():
        s, l = f.coords(x, z)
        if -reach < s < f.run + reach and abs(l) < f.half_w + reach and \
                min(abs(f.h0 - y), abs(f.h1 - y), abs(f.height(s) - y)) < 1.0:
            found.append(f)
    return found
