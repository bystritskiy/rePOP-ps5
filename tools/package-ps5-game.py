#!/usr/bin/env python3
"""Sign, stage and optionally roundtrip-pack a fully linked native PS5 game."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('ps5_build', ROOT/'tools/build-ps5.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)
TITLE = 'PPSA99023'   # port/ps5/sce_sys/param.json


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-tool', type=Path, required=True)
    p.add_argument('--llvm-bin', type=Path, required=True)
    # Without --game the app carries no game: it finds the user's files on
    # the console (port/ps5/README.md), and --zip makes the release archive.
    p.add_argument('--game', type=Path)
    p.add_argument('--pack-python', type=Path)
    p.add_argument('--zip', action='store_true', help='write bin/ps5/<ID>-<contentVersion>.zip (no game only)')
    # Another title ID makes an app that can be installed beside this one
    # (a private build, for example). The build must use the same ID
    # (cmake -DRESOT_PS5_TITLE_ID), because it names the data folder.
    p.add_argument('--title-id', default=TITLE)
    p.add_argument('--content-id', help='required with another --title-id')
    args = p.parse_args()
    title = args.title_id
    if not re.fullmatch(r'PPSA[0-9]{5}', title):
        raise SystemExit('Invalid PS5 title ID')
    if (title != TITLE) != bool(args.content_id):
        raise SystemExit('--content-id goes with a title ID other than '+TITLE)
    if args.zip and args.game:
        raise SystemExit('--zip is for the app without game files')
    lock = json.loads((ROOT/'port/ps5/dependencies.json').read_text())
    upstream = lock['upstream']
    release = lock['upstream_release']
    content_version = lock['ps5_content_version']
    if not re.fullmatch(r'[0-9a-f]{40}', upstream):
        raise SystemExit('Invalid pinned upstream commit')
    if not re.fullmatch(r'\d{2}\.\d{3}\.\d{3}', content_version):
        raise SystemExit('Invalid PS5 content version')
    if b.run(['git', 'rev-parse', release+'^{commit}']).strip() != upstream:
        raise SystemExit('Release tag does not match the pinned upstream commit')
    b.run(['git', 'merge-base', '--is-ancestor', upstream, 'HEAD'])
    native, llvm = args.native_tool.resolve(), args.llvm_bin.resolve()
    game = args.game.resolve() if args.game else None
    if game:
        if any(x.is_symlink() for x in game.rglob('*')):
            raise SystemExit('Game source must not contain symlinks')
        b.run(['python3', ROOT/'tools/check-ps5-game.py', game])
    linked = ROOT/'build/ps5/linked-xbox-retail'
    sdk = ROOT/'build/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk'
    work = ROOT/'build/ps5'/('game-package' if game else 'app-package')
    work.mkdir(parents=True, exist_ok=True)
    app = work/title
    if app.is_symlink():
        raise SystemExit('Staging directory must not be a symlink')
    app.mkdir(exist_ok=True)
    if any(x.is_symlink() for x in app.rglob('*')):
        raise SystemExit('Staging contains symlinks')
    source = b.source_fingerprint()
    dynamic = b.run([llvm/'llvm-readelf', '--dyn-syms', '--wide', linked/'llvm-pie.elf'])
    exports = [x for x in dynamic.splitlines() if re.search(r'\b(?:GLOBAL|WEAK)\b', x) and ' UND ' not in x]
    if exports:
        raise SystemExit('Application exports are unsupported: '+repr(exports[:5]))
    imports = re.findall(r'\bUND[ \t]+(\S+)', dynamic)
    if b'/data/reSOT/'+title.encode()+b'\0' not in (linked/'llvm-pie.elf').read_bytes():
        raise SystemExit('The linked build has another data folder: rebuild with -DRESOT_PS5_TITLE_ID='+title)
    b.run([native, 'link', '--in', linked/'llvm-pie.elf', '--out', linked/'eboot.elf',
           '--stub-dir', sdk/'target/lib', '--stub', linked/'stubs/libSceAgc.so',
           '--stub', linked/'stubs/libSceAgcDriver.so', '--module-sdk', '0x02000009',
           '--companion-sdk', '0x08050001', '--file-name', 'eboot.elf'], work/'convert.log')
    b.run([native, 'self', '--sign', '--in', linked/'eboot.elf', '--out', app/'eboot.bin',
           '--magic', '0x1D3D154F'], work/'sign.log')
    modules = app/'sce_module'
    modules.mkdir(exist_ok=True)
    libc = ROOT/'build/deps/PS5_Vulkan/runtime/libc.prx'
    if b.digest(libc) != 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036':
        raise SystemExit('Pinned clean-room libc hash mismatch')
    shutil.copyfile(libc, modules/'libc.prx')
    for file in [app/'eboot.bin', modules/'libc.prx']:
        inspection = b.run([native, 'self', '--inspect', '--file', file])
        (work/(file.name+'-inspect.txt')).write_text(inspection)
        if 'integrity: valid' not in inspection:
            raise SystemExit('FSELF integrity check failed: '+str(file))
    sysdir = app/'sce_sys'
    sysdir.mkdir(exist_ok=True)
    # sce_sys is kept in the repository, where the homebrew catalog reads it.
    param = json.loads((ROOT/'port/ps5/sce_sys/param.json').read_text())
    if param['titleId'] != TITLE or param['contentVersion'] != content_version:
        raise SystemExit('port/ps5/sce_sys/param.json must name '+TITLE+' and contentVersion '+content_version)
    if not re.fullmatch(r'[A-Z]{2}[0-9]{4}-PPSA[0-9]{5}_00-[A-Z0-9]{16}', param['contentId']):
        raise SystemExit('Invalid PS5 contentId')
    if args.content_id:
        if not re.fullmatch(r'[A-Z]{2}[0-9]{4}-'+title+r'_00-[A-Z0-9]{16}', args.content_id):
            raise SystemExit('Invalid PS5 contentId for '+title)
        param.update(titleId=title, contentId=args.content_id)
        (sysdir/'param.json').unlink(missing_ok=True)
        (sysdir/'param.json').write_text(json.dumps(param, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    else:
        shutil.copyfile(ROOT/'port/ps5/sce_sys/param.json', sysdir/'param.json')
    shutil.copyfile(ROOT/'port/ps5/sce_sys/icon0.png', sysdir/'icon0.png')
    # Keep the complete extracted disc. Hard links avoid another multi-GB
    # copy; no source file is modified. Button prompts are redrawn at run time
    # (port/ps5/src/buttons_ps5.c), so prince.bf is staged as it is.
    staged = app/'game'
    expected = b.inventory(game) if game else {}
    current = b.inventory(staged) if staged.exists() else {}
    for name in current.keys()-expected.keys():
        raise SystemExit('Unexpected staged game file (remove '+str(staged)+'): '+name)
    for name, facts in expected.items():
        dest = staged/name
        if dest.exists():
            if current[name] != facts:
                raise SystemExit('Staged game differs: '+name)
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.link(game/name, dest)
    notices = app/'licenses'
    notices.mkdir(exist_ok=True)
    sources = {
        'reSOT-MIT.txt': ROOT/'LICENSE',
        'xboxrecomp-MIT.txt': ROOT/'port/third_party/xboxrecomp/LICENSE',
        'xboxrecomp-NOTICE.txt': ROOT/'port/third_party/xboxrecomp/NOTICE',
        'xemu-LGPL-2.1.txt': ROOT/'port/third_party/xboxrecomp/LICENSES/LGPL-2.1.txt',
        'SDL3-zlib.txt': ROOT/'build/deps/sdl3/LICENSE.txt',
        'glslang.txt': ROOT/'build/deps/glslang/LICENSE.txt',
        'SPIRV-Tools-Apache-2.0.txt': ROOT/'build/deps/spirv-tools/LICENSE',
        'SPIRV-Headers.txt': ROOT/'build/deps/spirv-headers/LICENSE',
        'PS5-Vulkan-GPL-3.0.txt': ROOT/'build/deps/PS5_Vulkan/LICENSE',
        'PS5-Vulkan-NOTICE.txt': ROOT/'build/deps/PS5_Vulkan/NOTICE.md',
        'PS5-PayloadSDK.txt': ROOT/'build/deps/PS5_PayloadSDK/LICENSE',
        'PS5-Port-NOTICE.txt': ROOT/'port/ps5/NOTICE.md',
        'dependencies.json': ROOT/'port/ps5/dependencies.json',
        'Mesa-license.txt': ROOT/'build/deps/PS5_Mesa/docs/license.rst',
    }
    for name, src in sources.items():
        shutil.copyfile(src, notices/name)
    shutil.copytree(ROOT/'build/deps/PS5_Mesa/licenses', notices/'Mesa', dirs_exist_ok=True)
    # Preserve the complete SDK platform's third-party notices (dlmalloc etc.).
    platform = ROOT/'build/deps/PS5_PayloadSDK/platform'
    for src in platform.rglob('*'):
        if src.is_file() and any(word in src.name.lower() for word in ['license','copying','notice']):
            dest=notices/'PS5-platform'/src.relative_to(platform)
            dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(src,dest)
    (app/'README.txt').write_text(f'reSOT PS5 native retail build, {title}, upstream {release}.\n'
        + ('' if game else 'No game files are included. Copy all files of your own Xbox retail disc to\n'
           '/data/reSOT/xbox-retail/ (or /mnt/usbN/reSOT/xbox-retail/, /mnt/extN/reSOT/xbox-retail/).\n') +
        f'Upstream commit: {upstream}. PS5 content version: {content_version}.\n'
        'Tested on FW 13.40, kstuff-lite 1.11, ShadowMountPlus 1.7-beta5fix1.\n'
        f'Saves/options/logs: /data/reSOT/{title}/\n'
        'Logs: logs/xbox-retail-err.txt and xbox-retail-out.txt. Previous run: .previous.\n'
        'Cross/Circle/Square/Triangle: A/B/X/Y. R1/L1: Black/White. L2/R2: triggers.\n'
        'Options: Start. Touchpad click: Back. Native game menu includes reSOT options.\n'
        'Output: 1920x1080, 16:9; guest internal resolution configurable in game.\n')
    inventory = b.inventory(app)
    receipt = {'title_id': title, 'source': source, 'console_tested': False,
               'upstream_commit': upstream, 'upstream_release': release, 'content_version': content_version,
               'game_linked': True, 'game_renderer_linked': True, 'fself_integrity': 'valid',
               'runtime_libc_integrity': 'valid', 'imports': imports, 'import_count': len(imports),
               'game_included': bool(game),
               'target_xbe_sha256': expected['default.xbe']['sha256'] if game else
                   re.search(r'RESOT_XBE_SHA256\s+"([0-9a-fA-F]{64})"', (ROOT/'source/xbox-retail/target.h').read_text())[1].lower(),
               'native_tool_sha256': b.digest(native), 'linked_elf_sha256': b.digest(linked/'llvm-pie.elf'),
               'radv_provenance': (ROOT/'build/deps/PS5_Vulkan/.deps/native/radv-release/PROVENANCE.txt').read_text(),
               'files': inventory}
    if args.pack_python:
        destination = ROOT/f'bin/ps5/reSOT-PS5-{title}.ffpfsc'
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='resot-game-', dir=destination.parent) as temp:
            temp = Path(temp)
            packed, unpacked = temp/'game.ffpfsc', temp/'unpacked'
            b.run([args.pack_python, '-m', 'mkpfs', 'pack', 'folder', '--verify', app, packed], work/'pack.log')
            b.run([args.pack_python, '-m', 'mkpfs', 'unpack', '--deep', packed, unpacked], work/'unpack.log')
            actual = b.inventory(unpacked)
            if actual != inventory:
                mismatch = {
                    'missing': sorted(inventory.keys()-actual.keys()),
                    'unexpected': sorted(actual.keys()-inventory.keys()),
                    'changed': {name: {'expected': inventory[name], 'actual': actual[name]}
                                for name in sorted(inventory.keys() & actual.keys())
                                if inventory[name] != actual[name]}}
                (work/'roundtrip-mismatch.json').write_text(json.dumps(mismatch,indent=2)+'\n')
                raise SystemExit('Package roundtrip mismatch: see '+str(work/'roundtrip-mismatch.json'))
            os.replace(packed, destination)
        receipt['package']={'path':str(destination.relative_to(ROOT)), 'size':destination.stat().st_size,
                            'sha256':b.digest(destination), 'roundtrip_files':len(inventory)}
    if args.zip:
        # The homebrew catalog's artifact: the app folder at the top of the
        # archive, entries sorted and dated 1980 so a rebuild is byte-identical.
        archive = ROOT/f'bin/ps5/{title}-{content_version}.zip'
        archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive.with_suffix('.tmp'), 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(p for p in app.rglob('*') if p.is_file() and p.name.lower() != '.ds_store'):
                info = zipfile.ZipInfo(f'{title}/'+path.relative_to(app).as_posix(), (1980, 1, 1, 0, 0, 0))
                info.external_attr = 0o644 << 16
                z.writestr(info, path.read_bytes(), zipfile.ZIP_DEFLATED, 9)
        os.replace(archive.with_suffix('.tmp'), archive)
        receipt['zip'] = {'path': str(archive.relative_to(ROOT)), 'size': archive.stat().st_size,
                          'sha256': b.digest(archive)}
    if b.source_fingerprint()!=source:
        raise SystemExit('Source changed during packaging: rerun before publishing receipt')
    (work/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    (ROOT/'bin/ps5').mkdir(parents=True, exist_ok=True)
    (ROOT/'bin/ps5'/(('game' if game else 'app')+'-verification.json')).write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:receipt[k] for k in ['title_id','game_included','game_linked','console_tested']+(['zip'] if args.zip else [])},indent=2))


if __name__ == '__main__':
    main()
