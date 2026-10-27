#!/usr/bin/env python3
"""Prepare pinned PS5 SDL3/RADV/shader dependencies on macOS with Docker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parent.parent
DEPS=ROOT/'build/deps'
LOCK=json.loads((ROOT/'port/ps5/dependencies.json').read_text())


def run(args, **kwargs):
    print('+',subprocess.list2cmdline([str(x) for x in args]),flush=True)
    return subprocess.run([str(x) for x in args],cwd=ROOT,check=True,**kwargs)


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(1<<20),b''):h.update(data)
    return h.hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--llvm-bin',type=Path,default=Path('/opt/homebrew/opt/llvm@18/bin'))
    p.add_argument('--jobs',type=int,default=8)
    args=p.parse_args()
    DEPS.mkdir(parents=True,exist_ok=True)
    run(['docker','info'],stdout=subprocess.DEVNULL)
    for name,(url,rev) in LOCK['sources'].items():
        dest=DEPS/name
        if not dest.exists():
            run(['git','clone','--filter=blob:none','--no-checkout',url,dest])
            run(['git','-C',dest,'checkout','--detach',rev])
        actual=subprocess.check_output(['git','-C',str(dest),'rev-parse','HEAD'],text=True).strip()
        if actual!=rev:raise SystemExit(f'{name}: expected {rev}, found {actual}; preserve existing changes before switching')
    # BSD tar on the host avoids GNU tar's directory-rename errors on macOS
    # Docker bind mounts. The upstream RADV builder consumes this pinned export.
    mesa=DEPS/'PS5_Vulkan/.deps/work/radv-src'
    marker=mesa/'.revision'
    revision=LOCK['sources']['PS5_Mesa'][1]
    if not marker.exists() or marker.read_text().strip()!=revision:
        if mesa.exists() and any(mesa.iterdir()):raise SystemExit('Existing Mesa export has a different revision')
        mesa.mkdir(parents=True,exist_ok=True)
        with tempfile_archive(DEPS/'PS5_Mesa',revision) as archive:
            run(['tar','-xf',archive,'-C',mesa])
        marker.write_text(revision+'\n')
    run(['docker','build','-t','resot-ps5-builder:local','-f',ROOT/'port/ps5/build/Dockerfile',ROOT/'port/ps5/build'])
    def container(command):
        run(['docker','run','--rm','-v',str(ROOT)+':/work','-w','/work',
             '-e','BUILD_JOBS='+str(args.jobs),'resot-ps5-builder:local','bash','-lc',command])
    container('set -e; python3 -m venv --system-site-packages build/deps/linux-tools; '
              'build/deps/linux-tools/bin/pip install meson==1.9.0; '
              'export PATH=/work/build/deps/linux-tools/bin:/usr/lib/llvm-18/bin:$PATH; '
              'cd build/deps/PS5_Vulkan; bash tools/setup-native-dependencies.sh; '
              'MESON=/work/build/deps/linux-tools/bin/meson NINJA=/usr/bin/ninja bash tools/build-radv.sh release; '
              'CXX=clang++-18 bash tools/rebuild-libc.sh')
    # compiler-rt must contain ELF x86-64 objects even on an ARM build host.
    crt=DEPS/'compiler-rt';crt.mkdir(exist_ok=True)
    builtin=crt/'libclang_rt.builtins-x86_64.a'
    if not builtin.exists():
        run(['docker','run','--rm','--platform','linux/amd64','-v',str(crt)+':/out','ubuntu:24.04','bash','-lc',
             'set -e; apt-get update -qq; cd /tmp; apt-get download libclang-rt-18-dev=1:18.1.3-1ubuntu1; '
             'dpkg-deb -x libclang-rt-18-dev*.deb extracted; '
             'cp extracted/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a /out/; '
             'sha256sum libclang-rt-18-dev*.deb /out/libclang_rt.builtins-x86_64.a > /out/PROVENANCE.txt'])
    if sha(builtin)!=LOCK['compiler_rt']['archive_sha256']:raise SystemExit('compiler-rt hash mismatch')
    dxc=DEPS/'dxc';dxc.mkdir(exist_ok=True)
    archive=dxc/'dxc.tar.gz'
    if not archive.exists():urllib.request.urlretrieve(LOCK['dxc']['url'],archive)
    if sha(archive)!=LOCK['dxc']['archive_sha256']:raise SystemExit('DXC hash mismatch')
    shutil.copyfile(ROOT/'port/ps5/build/Dockerfile.dxc',dxc/'Dockerfile')
    run(['docker','build','--platform','linux/amd64','-t','resot-dxc:local',dxc])
    # This wrapper is invoked by build-native-shaders.py from the checkout.
    # shlex.quote protects checkout paths passed through the shell.
    import shlex
    wrapper=dxc/'dxc'
    wrapper.write_text('#!/bin/sh\nexec docker run --rm --platform linux/amd64 -v '+
        shlex.quote(str(ROOT)+':'+str(ROOT))+' -w "$PWD" resot-dxc:local "$@"\n')
    wrapper.chmod(0o755)
    run(['python3',ROOT/'tools/build-native-shaders.py','--dxc',wrapper,'--output',ROOT/'build/ps5/native-shaders'])
    run(['python3',ROOT/'tools/prepare-ps5-sdl.py',DEPS/'sdl3'])
    sdk=DEPS/'PS5_Vulkan/.deps/native/ps5-payload-sdk'
    options=['SDL_STATIC=ON','SDL_SHARED=OFF','SDL_UNIX_CONSOLE_BUILD=ON',
             'SDL_X11=OFF','SDL_WAYLAND=OFF','SDL_KMSDRM=OFF','SDL_OPENGL=OFF','SDL_OPENGLES=OFF',
             'SDL_VULKAN=ON','SDL_OFFSCREEN=ON','SDL_HIDAPI=OFF','SDL_JOYSTICK=OFF','SDL_AUDIO=OFF',
             'SDL_HAPTIC=OFF','SDL_SENSOR=OFF','SDL_OSS=OFF','SDL_TEST_LIBRARY=OFF','SDL_TESTS=OFF',
             'SDL_EXAMPLES=OFF','LIBC_HAS_WCSNLEN=OFF','LIBC_HAS_WCSLCPY=OFF','LIBC_HAS_WCSLCAT=OFF']
    run(['cmake','-S',DEPS/'sdl3','-B',ROOT/'build/ps5/sdl3','-G','Ninja',
         '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'port/ps5/cmake/ps5.cmake'),
         '-DCMAKE_C_COMPILER='+str(args.llvm_bin/'clang'),'-DCMAKE_CXX_COMPILER='+str(args.llvm_bin/'clang++'),
         '-DPS5_PAYLOAD_SDK='+str(sdk),'-DCMAKE_BUILD_TYPE=Release',*['-D'+x for x in options]])
    run(['cmake','--build',ROOT/'build/ps5/sdl3','-j',str(args.jobs)])


class tempfile_archive:
    def __init__(self,source,revision):self.source,self.revision=source,revision
    def __enter__(self):
        import tempfile
        self.file=tempfile.NamedTemporaryFile(suffix='.tar',dir=DEPS,delete=False)
        run(['git','-C',self.source,'archive',self.revision],stdout=self.file)
        self.file.close();return self.file.name
    def __exit__(self,*args):os.unlink(self.file.name)


if __name__=='__main__':main()
