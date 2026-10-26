from pathlib import Path
from collections import defaultdict
import subprocess, json, os

root = Path(r'D:\Development\LIMINAL-IX')
src = root / 'source/integration'
play = root / 'play-integration'

def disk(p):
    total, n = 0, 0
    for d, _, files in os.walk(p):
        for f in files:
            try:
                total += (Path(d)/f).stat().st_size
                n += 1
            except (OSError, ValueError): pass
    return {'files': n, 'mib': round(total / 2**20,1)}

raw = subprocess.run(['git','-C',str(src),'ls-files','-z'],capture_output=True,check=True).stdout
groups = defaultdict(lambda:{'files':0,'bytes':0})
large = []
for b in raw.split(b'\0'):
    if not b: continue
    rel = b.decode('utf-8', 'surrogateescape')
    p = src / rel
    if not p.is_file(): continue
    sz = p.stat().st_size
    k = '/'.join(Path(rel).parts[:2])
    groups[k]['files'] += 1
    groups[k]['bytes'] += sz
    if sz >= 90_000_000: large.append((rel,sz))
add = {d.name:disk(d) for d in (play/'ixr_addons').iterdir() if d.is_dir()}
d = {'source_tracked_mib':round(sum(x['bytes'] for x in groups.values())/2**20,1),
     'source_by_group':sorted(({ 'path':k,'files':v['files'],
       'mib':round(v['bytes']/2**20,1)} for k,v in groups.items()),key=lambda x:-x['mib'])[:25],
     'big_tracked_files':large,
     'addons':dict(sorted(add.items(),key=lambda pair:-pair[1]['mib'])),
     'game': {x:disk(play/x) for x in ['bin','gamedata','userdata']}}
print(json.dumps(d,ensure_ascii=False,indent=2))