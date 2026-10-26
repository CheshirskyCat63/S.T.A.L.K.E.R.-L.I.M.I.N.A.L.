"""Renders ONLY measured PhysX particle trajectories from pbd_shot_trajectories.npz."""
from pathlib import Path
import json
import numpy as np
from PIL import Image, ImageDraw, ImageFont

R = Path(__file__).resolve().parent.parent / "reports"
arr = np.load(R / "pbd_shot_trajectories.npz")
m = json.loads((R / "pbd_shot_summary.json").read_text(encoding="utf-8"))
times=arr["times"]; grains=arr["shot_particles"]; refs=arr["control_particles"]; proj=arr["projectile"]
W,H=1120,690
left,top,width,height=65,105,995,490
xmin,xmax=-4.5,3.0
ymin,ymax=-0.5,2.7
def px(x): return int(left+(x-xmin)/(xmax-xmin)*width)
def py(y): return int(top+height-(y-ymin)/(ymax-ymin)*height)
def font(size,bold=False):
    p=r"C:\Windows\Fonts\segoeuib.ttf" if bold else r"C:\Windows\Fonts\segoeui.ttf"
    try: return ImageFont.truetype(p,size)
    except: return ImageFont.load_default()
bold=font(28,True); normal=font(17); small=font(14)
frames=[]
for n,t in enumerate(times):
    im=Image.new("RGB",(W,H),(12,19,31)); g=ImageDraw.Draw(im)
    g.text((46,24),"LIMINAL  /  PHYSX 5.12  /  GPU PBD PARTICLE IMPACT",font=bold,fill=(239,243,247))
    g.text((48,67),f"420 interacting PBD particles   |   5 kg rigid projectile   |   initial speed 13.5 m/s",font=normal,fill=(168,190,213))
    g.rounded_rectangle((left,top,left+width,top+height),radius=8,outline=(80,96,121),width=2)
    for x in (-4,-3,-2,-1,0,1,2,3):
        xp=px(x)
        if left<=xp<=left+width:
            g.line([(xp,top),(xp,top+height)],fill=(29,41,60),width=1)
            g.text((xp-4,top+height+5),str(x),font=small,fill=(142,160,182))
    for y in (0,1,2):
        yp=py(y)
        g.line([(left,yp),(left+width,yp)],fill=(30,44,59),width=1)
        g.text((24,yp-7),str(y)+"m",font=small,fill=(142,160,182))
    # Control geometry in slate; shot particles in brighter blue. Perspective is side (X,Y).
    for xyz in refs[n]:
        x,y=float(xyz[0]),float(xyz[1])
        if xmin<=x<=xmax and ymin<=y<=ymax:
            X,Y=px(x),py(y)
            g.ellipse((X-2,Y-2,X+2,Y+2),fill=(68,84,99))
    for xyz in grains[n]:
        x,y,z=map(float,xyz)
        if xmin<=x<=xmax and ymin<=y<=ymax:
            X,Y=px(x),py(y)
            rad=3 if abs(z)<.2 else 2
            g.ellipse((X-rad,Y-rad,X+rad,Y+rad),fill=(84,186,246) if abs(z)<.2 else (51,131,200))
    x,y,z=map(float,proj[n])
    X,Y=px(x),py(y)
    r=int(.16/(xmax-xmin)*width)
    g.ellipse((X-r,Y-r,X+r,Y+r),fill=(249,121,74),outline=(255,195,157),width=2)
    g.line([(X-10,Y),(X+10,Y)],fill=(255,216,194),width=1)
    g.text((left+20,top+18),f"t = {t:.3f} s",font=font(25,True),fill=(239,243,247))
    displacement=np.linalg.norm(grains[n]-refs[n],axis=1)
    g.text((left+20,top+53),f"Particles moved > 1 cm vs control: {(displacement>0.01).sum()} / 420",font=normal,fill=(174,216,241))
    g.text((48,H-51),"GREY: no-shot control  |  BLUE: simulated PBD  |  ORANGE: rigid projectile  |  no gravity laboratory rig",font=small,fill=(170,190,212))
    g.text((48,H-28),"REAL SIMULATION DATA; not footage inside S.T.A.L.K.E.R.; prototype collision benchmark",font=small,fill=(131,149,171))
    frames.append(im)
frames[8].save(R/"pbd_shot_impact_preview.png")
frames[-1].save(R/"pbd_shot_final_preview.png")
frames[0].save(R/"pbd_shot_first_preview.png")
frames[0].save(R/"pbd_shot_real_gpu.gif",save_all=True,append_images=frames[1:],duration=100,loop=0,optimize=False)
print("RENDER_PASS",len(frames),"frames",flush=True)
for f in ["pbd_shot_real_gpu.gif","pbd_shot_impact_preview.png","pbd_shot_final_preview.png"]:
    p=R/f;print(str(p),p.stat().st_size,"bytes",flush=True)
