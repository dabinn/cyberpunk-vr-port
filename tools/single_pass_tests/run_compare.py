"""Run the real captured VS/geometry through two draws and one view-instanced draw.

Requires the private exported capture fixture; does not launch or modify the game.
"""
import argparse
import itertools
import json
import subprocess
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('exe',type=Path)
parser.add_argument('fixture',type=Path)
args=parser.parse_args()
rows=[]
cases=[(1,0,0,1,-1,3)]
cases += [(scale,shift,roll,instances,-1,3) for scale,shift,roll,instances in
          itertools.product((8,16),(0,.08),(-15,15),(1,3))]
cases += [(16,.08,15,3,eye,3) for eye in (0,1)]
cases += [(16,.08,15,3,-1,mask) for mask in (0,1,2)]
for scale,shift,roll,instances,clip,mask in cases:
    proc=subprocess.run([str(args.exe),str(args.fixture),str(scale),str(shift),str(roll),str(instances),str(clip),str(mask)],capture_output=True,text=True,timeout=15)
    if proc.returncode:raise RuntimeError(proc.stdout+'\n'+proc.stderr)
    row=json.loads(proc.stdout);row.update(roll=roll,clip=clip)
    assert row['differentPixels']==0
    rows.append(row)
result={'cases':len(rows),'result':'PASS','rows':rows}
(args.fixture/'comparison-results.json').write_text(json.dumps(result,indent=2))
print(json.dumps({'cases':len(rows),'differentPixels':sum(x['differentPixels'] for x in rows),
                  'maxChannelError':max(x['maxChannelError'] for x in rows),'maxCovered':max(max(x['covered']) for x in rows)}))
