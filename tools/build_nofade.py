"""Builds sims4vr_nofade.package, the mod's renderer tuning override for VR:
- Renderer.GlobalDofSettings: no depth-of-field blur (the game blurs whatever
  is past 7.5 m, fully at 60 m, which in a headset looks like bad eyesight).
Up to v0.13.44 it also zeroed Renderer.TunableFadeValues (Sims, objects and
walls fading out near the camera) for every mode; since v0.13.45 gamefix.cpp
does that at runtime, in first person only, so god view fades like the game.
patch() stays for reference.

Input: the game's SimData resources (extracted from SimulationDeltaBuild0.package):
the two fade ones and the DoF one. Output: a DBPF 2.1 package with patched copies."""
import struct, sys

SIMDATA = 0x545AC67A
GROUP = 0x001F95DE
INSTANCES = {'default': 0xDEAD4915C9C6C0D8, 'minspec': 0x89768266CE86855E}
# Row offsets inside the 808-byte resource (see TunableFadeValues schema).
PATCH = {
    0x80: 0.001,  # DefaultObjectFadeStart   (game: 0.5)
    0x84: 0.001,  # DefaultBuildingFadeStart (game: 2.0)
    0x88: 0.0,    # DefaultObjectFadeFinish
    0x8c: 0.0,    # DefaultBuildingFadeFinish
    0x90: 0.001,  # SimFadeStart             (game: 0.6)
    0x94: 0.0,    # SimFadeFinish            (game: 0.4)
}

DOF_GROUP = 0x0033A053
DOF_INSTANCE = 0xDEAD4915C9C6C0D8
# Row offsets inside the 938-byte resource (see GlobalDofSettings schema).
DOF_PATCH = {
    0xa0: 0.0,  # FarBlurStrength_0  (game: 1.0)
    0x9c: 0.0,  # NearBlurStrength_0 (game: 0.01)
    0xbc: 0.0,  # NearBlurStrength_1 (game: 1.0)
    0xc0: 0.0,  # FarBlurStrength_1  (game: 0.2)
}
DOF_ENABLE = 0x80  # EnableDof (bool; game: 1)

def patch_dof(data):
    d = bytearray(data)
    assert d[:4] == b'DATA' and b'Renderer.GlobalDofSettings' in d, 'unexpected resource'
    d[DOF_ENABLE] = 0
    for off, v in DOF_PATCH.items():
        struct.pack_into('<f', d, off, v)
    return bytes(d)

def patch(data):
    d = bytearray(data)
    assert d[:4] == b'DATA' and b'Renderer.TunableFadeValues' in d, 'unexpected resource'
    for off, v in PATCH.items():
        struct.pack_into('<f', d, off, v)
    return bytes(d)

def write_package(path, resources):
    body = bytearray(96)
    entries = []
    for (t, g, i), data in resources:
        entries.append((t, g, i, len(body), len(data)))
        body += data
    index = bytearray(struct.pack('<I', 0))
    for t, g, i, off, size in entries:
        index += struct.pack('<IIIIIIIHH', t, g, i >> 32, i & 0xffffffff, off,
                             size | 0x80000000, size, 0, 1)
    pos = len(body)
    body += index
    struct.pack_into('<4sII', body, 0, b'DBPF', 2, 1)
    struct.pack_into('<III', body, 36, len(entries), 0, len(index))
    struct.pack_into('<IQ', body, 60, 3, pos)
    open(path, 'wb').write(body)

if __name__ == '__main__':
    src_default, src_minspec, src_dof, out = sys.argv[1:5]
    write_package(out, [
        ((SIMDATA, DOF_GROUP, DOF_INSTANCE), patch_dof(open(src_dof, 'rb').read())),
    ])
