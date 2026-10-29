#!/usr/bin/env python3
"""Close, open or restart a title on the PS5 without the controller.

status  the title's processes, from Payload Manager (port 8084)
kill    ends every running title process (eboot.bin over 50 MiB): Payload
        Manager's process_kill, the UI's own request
launch  opens the title through RecompStore's launcher payload, as the store
        does: the job goes to /data/recompstore/launch/job.txt over ftpsrv
        (port 2121), the console's own recompstore-launcher.elf to the payload
        loader (port 9021), and the launcher's status.txt says how it went
restart kill, then launch

Needs Payload Manager, ftpsrv, a payload loader on 9021 and RecompStore
installed on the console. A title launched this way gets its title ID as
argv[1]; reSOT ignores its arguments.
"""
import argparse
import ftplib
import io
import json
import socket
import sys
import time
import urllib.request

TITLE = 'PPSA99023'
LAUNCHER = '/data/recompstore/payloads/recompstore-launcher.elf'
JOB_DIR = '/data/recompstore/launch'
# The smallest title seen was 160 MiB; a 0.2 MiB eboot.bin runs from boot
# with no title open (observed 2026-10-29).
TITLE_MIB = 50


def manager(addr, path, post=False):
    base = f'http://{addr}:8084'
    request = urllib.request.Request(base + path, method='POST' if post else 'GET')
    if post:
        request.add_header('X-PLDMGR-Request', '1')
        request.add_header('Origin', base)
    with urllib.request.urlopen(request, timeout=10) as answer:
        return json.load(answer)


def titles(addr):
    return [p for p in manager(addr, '/processes_list')['processes']
            if p['name'] == 'eboot.bin' and p['memory'] > TITLE_MIB]


def kill(addr):
    for p in titles(addr):
        print(f"kill pid {p['pid']} ({p['memory']:.0f} MiB):", manager(addr, f"/process_kill?pid={p['pid']}", post=True))
    for _ in range(20):
        if not titles(addr):
            return True
        time.sleep(0.5)
    print('still running:', titles(addr))
    return False


def launch(addr, title):
    if titles(addr):
        print('a title is running; kill it first:', titles(addr))
        return False
    ftp = ftplib.FTP()
    ftp.connect(addr, 2121, timeout=15)
    ftp.login()
    listing = []
    ftp.retrlines(f'LIST {JOB_DIR}', listing.append)  # ftpsrv has no NLST
    if any(line.split()[-1] == 'cancel' for line in listing if line.split()):
        ftp.delete(JOB_DIR + '/cancel')
    token = f'ps5-app-{int(time.time())}'
    ftp.storbinary(f'STOR {JOB_DIR}/job.txt', io.BytesIO(f'token={token}\ntitle={title}\n'.encode()))
    elf = io.BytesIO()
    ftp.retrbinary(f'RETR {LAUNCHER}', elf.write)
    with socket.create_connection((addr, 9021), timeout=10) as loader:
        loader.sendall(elf.getvalue())
    status = ''
    for _ in range(60):
        time.sleep(1)
        text = io.BytesIO()
        try:
            ftp.retrbinary(f'RETR {JOB_DIR}/status.txt', text.write)
        except ftplib.error_perm:
            continue
        status = text.getvalue().decode(errors='replace')
        if f'token={token}' in status and ('phase=done' in status or 'phase=failed' in status):
            break
    ftp.quit()
    print(status.strip() or 'no status from the launcher')
    return 'phase=done' in status and f'token={token}' in status


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['status', 'kill', 'launch', 'restart'])
    p.add_argument('--addr', required=True, help='console address')
    p.add_argument('--title', default=TITLE)
    a = p.parse_args()
    if a.action == 'status':
        print(json.dumps(titles(a.addr)))
        return 0
    ok = True
    if a.action in ('kill', 'restart'):
        ok = kill(a.addr)
    if ok and a.action in ('launch', 'restart'):
        ok = launch(a.addr, a.title)
        if ok:
            time.sleep(2)
            print('running:', json.dumps(titles(a.addr)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
