#!/usr/bin/env python3
"""Copy the staged PS5 app folder to the console as a ShadowMountPlus folder game.

ShadowMountPlus mounts a folder under its scan path the same way as an image
(nullfs onto /system_ex/app/<ID>), so during development the console keeps
/data/homebrew/PPSA99023/ and only changed files travel: eboot.bin is 51 MB,
an image 2.5 GB. Files under game/ and licenses/ are sent when missing, of
another size, or not what the last copy sent (build/ps5/deployed-<ID>.json
records size, mtime and inode, since two builds of a file can share a
size); the rest every time. sce_sys/param.json goes last, so a first
copy is not picked up half-written. Every file is written as NAME.part, its
size checked, then renamed. Remote files absent from the staging are listed,
not deleted. Needs ftpsrv on port 2121; package-ps5-game.py without
--pack-python stages the folder."""
import argparse
import ftplib
import json
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parent.parent
TITLE = 'PPSA99023'
# eboot.bin (signed with --magic 0x1D3D154F) and the clean-room libc.prx.
SELF_MAGICS = (bytes.fromhex('4f153d1d'), bytes.fromhex('5414f5ee'))


def listing(ftp, path, base=''):
    files = {}
    lines = []
    ftp.retrlines('LIST '+path, lines.append)
    for line in lines:
        fields = line.split(None, 8)
        if len(fields) < 9 or fields[8] in ('.', '..'):
            continue
        name = base+fields[8]
        if line.startswith('d'):
            files.update(listing(ftp, path+'/'+fields[8], name+'/'))
        else:
            files[name] = int(fields[4])
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--addr', required=True, help='console address (ftpsrv)')
    p.add_argument('--dest', default='/data/homebrew/'+TITLE)
    # Any staged app folder; by default the one tools/package-ps5-game.py builds.
    p.add_argument('--app', type=Path, default=ROOT/'build/ps5/app-package'/TITLE)
    args = p.parse_args()
    app = args.app
    if not (app/'eboot.bin').is_file() or not (app/'sce_sys/param.json').is_file():
        raise SystemExit('No staged app folder: '+str(app))
    stats = {x.relative_to(app).as_posix(): x.stat() for x in app.rglob('*')
             if x.is_file() and x.name.lower() != '.ds_store'}
    local = {name: st.st_size for name, st in stats.items()}
    stamp = {name: [st.st_size, st.st_mtime_ns, st.st_ino] for name, st in stats.items()}
    record = ROOT/'build/ps5'/('deployed-'+args.dest.strip('/').replace('/', '_')+'.json')
    sent_before = json.loads(record.read_text()) if record.exists() else {}
    ftp = ftplib.FTP()
    ftp.connect(args.addr, 2121, timeout=60)
    ftp.login()
    ftp.voidcmd('TYPE I')
    try:
        remote = listing(ftp, args.dest)
    except ftplib.error_perm:
        remote = {}
    for name in sorted(remote.keys()-local.keys()):
        print('only on console:', name)
    bulky = ('game/', 'licenses/')
    send = [n for n in local if n not in remote or remote[n] != local[n] or not n.startswith(bulky)
            or sent_before.get(n) != stamp[n]]
    send.sort(key=lambda n: (n == 'sce_sys/param.json', not n.startswith('game/'), n))
    made = set()
    total, started = sum(local[n] for n in send), time.time()
    for name in send:
        parent = ''
        for part in [args.dest]+name.split('/')[:-1]:
            parent = parent+'/'+part if parent else part
            if parent not in made:
                try:
                    ftp.mkd(parent)
                except ftplib.error_perm:
                    pass
                made.add(parent)
        target = args.dest+'/'+name
        with open(app/name, 'rb') as fh:
            signed = fh.read(4) in SELF_MAGICS
            fh.seek(0)
            ftp.storbinary('STOR '+target+'.part', fh, blocksize=4 << 20)
        # ftpsrv decrypts a SELF on LIST/SIZE/RETR: eboot.bin reads back as
        # the 53,566,360-byte ELF of its 52,623,517 bytes (observed), so only
        # the 226 reply to STOR vouches for those.
        if not signed and ftp.size(target+'.part') != local[name]:
            raise SystemExit('Size mismatch, left as '+target+'.part')
        if name in remote:
            # ftpsrv answers DELE with 226, which ftplib reports as an error.
            try:
                ftp.delete(target)
            except ftplib.error_reply:
                pass
        ftp.rename(target+'.part', target)
        sent_before[name] = stamp[name]
        record.write_text(json.dumps(sent_before, indent=1)+'\n')
        print('sent', name, local[name], flush=True)
    elapsed = time.time()-started
    print('%d of %d files, %.1f MB in %.0f s' % (len(send), len(local), total/1e6, elapsed))
    ftp.quit()


if __name__ == '__main__':
    main()
