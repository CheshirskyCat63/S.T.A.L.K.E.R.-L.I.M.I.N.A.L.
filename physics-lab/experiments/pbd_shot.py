"""Autonomous LIMINAL PhysX 5.12 GPU PBD experiment, projectile vs zero-gravity particle-cloud impact rig.
This is real NVIDIA ovphysx GPU PBD, not particle animation or an IX-Ray game build.
Returns GPU positions, paired control experiment, JSON and frame data.
"""
from pathlib import Path
from time import perf_counter
import gc, json, csv, sys
import numpy as np
import ovstage, ovphysx
from ovphysx import PhysX
from ovphysx.types import SimObjectType, ObjectScope
from ovphysx.samples.python_samples.schema_support import physx_schema_paths

ROOT = Path(__file__).resolve().parent.parent
SCENES = ROOT / "experiments"
REPORTS = ROOT / "reports"
SCENES.mkdir(parents=True, exist_ok=True)
REPORTS.mkdir(parents=True, exist_ok=True)
N_X, N_Y, N_Z = 12, 7, 5
COUNT = N_X * N_Y * N_Z
DT = 1 / 120.0
STEPS = 132
SAMPLE_EVERY = 3
GRAIN_SPACING = 0.115
GRAIN_RADIUS = 0.049
SPHERE_MASS = 5.0
SPHERE_SPEED = 13.5

def write_scene(name, velocity):
    coords = [(0.12*i, 0.19+0.117*j, 0.115*(k-(N_Z-1)/2))
              for j in range(N_Y) for i in range(N_X) for k in range(N_Z)]
    positions = ",\n".join("      (%.4f, %.4f, %.4f)" % v for v in coords)
    widths = ", ".join([str(2*GRAIN_RADIUS)]*len(coords))
    data = '''#usda 1.0
(
 defaultPrim = "World"
 metersPerUnit = 1
 upAxis = "Y"
 kilogramsPerUnit = 1
)
def Xform "World"
{
 def PhysicsScene "physicsScene" (prepend apiSchemas = ["PhysxSceneAPI"])
 {
  vector3f physics:gravityDirection = (0, -1, 0)
  float physics:gravityMagnitude = 0.0
  bool physxScene:enableGPUDynamics = true
  uniform token physxScene:broadphaseType = "GPU"
 }
 def Xform "Ground"
 {
  def Cube "Floor" (prepend apiSchemas = ["PhysicsCollisionAPI"])
  {
   float3[] extent = [(-4, -0.05, -2.5), (4, 0.05, 2.5)]
   double size = 1
   double3 xformOp:scale = (8, 0.1, 5)
   double3 xformOp:translate = (-0.4, -0.05, 0)
   uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
  }
 }
 def PhysxParticleSystem "particleSystem"
 {
  float particleContactOffset = 0.055
  bool particleSystemEnabled = 1
  rel simulationOwner = </World/physicsScene>
 }
 def Points "grains" (prepend apiSchemas = ["PhysxParticleSetAPI"])
 {
  point3f[] points = [@@POSITIONS@@]
  float[] widths = [@@WIDTHS@@]
  bool physxParticle:fluid = 0
  rel physxParticle:particleSystem = </World/particleSystem>
 }
 def Xform "Projectile" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"])
 {
  double3 xformOp:translate = (-1.65, 0.55, 0)
  quatd xformOp:orient = (1, 0, 0, 0)
  double3 xformOp:scale = (1, 1, 1)
  uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:orient", "xformOp:scale"]
  float physics:mass = 5.0
  vector3f physics:velocity = (@@VX@@, 0, 0)
  vector3f physics:angularVelocity = (0, 0, 0)
  def Sphere "Shell" (prepend apiSchemas = ["PhysicsCollisionAPI"])
  {
   double radius = 0.16
   float3[] extent = [(-0.16, -0.16, -0.16), (0.16, 0.16, 0.16)]
  }
 }
}
'''
    scene = SCENES / (name + ".usda")
    scene.write_text(data.replace("@@POSITIONS@@", positions)
                          .replace("@@WIDTHS@@", widths)
                          .replace("@@VX@@", str(float(velocity))), encoding="utf-8")
    return scene

def read_values(px, kind, field):
    chunks = []
    with px.read_sync(kind, [field], scope=ObjectScope.ALL) as result:
        for group in result.groups:
            if group.is_delete:
                continue
            for tensor in group.tensors:
                chunks.append(np.array(tensor.numpy(), copy=True))
    return np.concatenate(chunks) if chunks else np.empty((0, 3), dtype=np.float32)

def simulate(name, velocity, schema):
    scene = write_scene(name, velocity)
    stage = ovstage.Stage("liminal-physx-pbd-"+name)
    px = PhysX()
    snapshots = []
    t0 = perf_counter()
    try:
        ovstage.population.open_usd(stage, str(scene), ordinal=1, domains=ovstage.PopulationDomain.ALL)
        stage.advance_write_floor(ordinal=1).wait()
        px.wait_task(px.attach_ovstage(stage, read_ordinal=1))
        print("ATTACHED", name, "GPU PBD", COUNT, "grains", flush=True)
        for i in range(STEPS + 1):
            if i:
                px.step_sync(DT)
            if i % SAMPLE_EVERY == 0:
                a = read_values(px, SimObjectType.PARTICLE_SET, "points")
                b = read_values(px, SimObjectType.RIGID_BODY, "position")
                if a.shape != (COUNT, 3):
                    raise RuntimeError("PBD output count unexpected: "+str(a.shape))
                if b.shape[0] != 1:
                    raise RuntimeError("Projectile count unexpected: "+str(b.shape))
                if not (np.isfinite(a).all() and np.isfinite(b).all()):
                    raise RuntimeError("Nonfinite physics output")
                snapshots.append((i*DT, a, b[0].copy()))
                if i in (0, 24, 48, 72, 96, 120, 132):
                    print(name, "t=", round(i*DT, 3), "projectile=", b[0].round(3).tolist(),
                          "grains COM=", a.mean(axis=0).round(3).tolist(), flush=True)
        return snapshots, perf_counter()-t0
    finally:
        px.wait_task(px.detach_ovstage())
        stage.destroy()
        gc.collect()
        px.destroy()
        print("CLEANUP_OK", name, flush=True)

def main():
    schemas = [str(x) for x in physx_schema_paths()]
    try: schemas.append(str(ovphysx.newton_schema_root()))
    except FileNotFoundError: pass
    ovstage.population.register_usd_schemas(schemas)
    control, c_seconds = simulate("control_no_shot", 0.0, schemas)
    shot, s_seconds = simulate("pbd_shot", SPHERE_SPEED, schemas)
    deltas = [np.linalg.norm(s[1]-c[1], axis=1) for c,s in zip(control,shot)]
    final = deltas[-1]
    first = deltas[0]
    metrics = {
        "backend":"NVIDIA PhysX 5.12 via ovphysx 0.7.0, CUDA GPU PBD",
        "n_particles":COUNT, "projectile_mass_kg":SPHERE_MASS,
        "projectile_speed_m_per_s":SPHERE_SPEED,
        "physics_dt_s":DT, "steps":STEPS, "gravity_m_s2":0.0,
        "elapsed_control_wall_s":round(c_seconds,3),
        "elapsed_shot_wall_s":round(s_seconds,3),
        "initial_max_difference_m":float(first.max()),
        "final_mean_difference_m":float(final.mean()),
        "final_max_difference_m":float(final.max()),
        "final_particles_different_gt_1cm":int(np.count_nonzero(final>0.01)),
        "projectile_final_position":shot[-1][2].round(6).tolist(),
        "frames":len(shot),
    }
    if metrics["final_particles_different_gt_1cm"] < 10:
        raise RuntimeError("Shot did not produce demonstrable granular impact; "+str(metrics))
    (REPORTS / "pbd_shot_summary.json").write_text(json.dumps(metrics,indent=2,ensure_ascii=False),encoding="utf-8")
    with (REPORTS / "pbd_shot_frames.csv").open("w",newline="",encoding="utf-8") as f:
        w=csv.writer(f); w.writerow(["time_s","particle_id","x","y","z","control_x","control_y","control_z"])
        for (t,a,p),(tc,ac,pc) in zip(shot,control):
            for index,(xyz,cxyz) in enumerate(zip(a,ac)):
                w.writerow([round(t,5),index,*map(float,xyz),*map(float,cxyz)])
    np.savez_compressed(REPORTS / "pbd_shot_trajectories.npz",
        times=np.array([f[0] for f in shot]),
        shot_particles=np.stack([f[1] for f in shot]),
        control_particles=np.stack([f[1] for f in control]),
        projectile=np.stack([f[2] for f in shot]),
        control_projectile=np.stack([f[2] for f in control]))
    print("REAL_GPU_PBD_SHOT_PASS",json.dumps(metrics,ensure_ascii=False),flush=True)

if __name__=="__main__":
    main()

