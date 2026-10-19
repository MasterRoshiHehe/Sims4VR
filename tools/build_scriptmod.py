"""Compile scriptmod/ into sims4vr.ts4script.

The game only loads .pyc files built by Python 3.7, so run this with a
Python 3.7 interpreter:  py -3.7 tools/build_scriptmod.py [output_dir]
Default output: build/package/sims4vr/ (next to the DLL from the CMake build).
Env S4VR_BUILD_VERSION / S4VR_BUILD_COMMIT are written into the package as sims4vr/_build.pyc.
"""
import os
import py_compile
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, 'scriptmod')


def main():
    if sys.version_info[:2] != (3, 7):
        sys.exit('Python 3.7 is required (found {}.{}); the game rejects other bytecode versions.'.format(
            *sys.version_info[:2]))
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'package', 'sims4vr')
    os.makedirs(out_dir, exist_ok=True)
    archive = os.path.join(out_dir, 'sims4vr.ts4script')

    with tempfile.TemporaryDirectory() as tmp, zipfile.ZipFile(archive, 'w', zipfile.ZIP_STORED) as zf:
        build_src = os.path.join(tmp, '_build.py')
        with open(build_src, 'w') as f:
            f.write('VERSION = {!r}\nCOMMIT = {!r}\n'.format(
                os.environ.get('S4VR_BUILD_VERSION', 'dev'), os.environ.get('S4VR_BUILD_COMMIT', 'none')))
        py_compile.compile(build_src, cfile=os.path.join(tmp, '_build.pyc'), dfile='_build.py', doraise=True)
        zf.write(os.path.join(tmp, '_build.pyc'), 'sims4vr/_build.pyc')
        for folder, _, files in os.walk(SOURCE):
            for name in files:
                if not name.endswith('.py'):
                    continue
                src = os.path.join(folder, name)
                rel = os.path.relpath(src, SOURCE)
                pyc = os.path.join(tmp, rel + 'c')
                os.makedirs(os.path.dirname(pyc), exist_ok=True)
                py_compile.compile(src, cfile=pyc, dfile=rel, doraise=True)
                zf.write(pyc, (rel + 'c').replace(os.sep, '/'))
                print('  ' + rel)
    print('Wrote ' + archive)


if __name__ == '__main__':
    main()
