from pathlib import Path
import subprocess, tarfile, shutil, json, hashlib, datetime

root=Path(r'D:\Development\LIMINAL-IX')
source=root/'source/integration'
target=root/'movement'
dest=target
assert (target/'.git').is_dir()
assert not (target/'src').exists(), 'Refusing to overwrite a previously imported tree'
paths=['src','cmake','CMakeLists.txt','CMakePresets.json','CMakeSettings.json',
       'build_cmake.bat','.clang-format','.clang-tidy','.editorconfig','.gitattributes']
revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
proc=subprocess.Popen(['git','-C',str(source),'archive','--format=tar',revision,*paths],stdout=subprocess.PIPE)
with tarfile.open(fileobj=proc.stdout,mode='r|') as tf:
    for entry in tf:
        if not entry.name.startswith(tuple(paths)) or '..' in Path(entry.name).parts: raise RuntimeError(entry.name)
        tf.extract(entry, path=dest, filter='data')
assert proc.wait()==0
selected=[
'cmake/msvc.cmake',
'src/Include/xrRender/Kinematics.h',
'src/Layers/xrRender/RenderInterfaceShared.h',
'src/Layers/xrRender/SkeletonCustom.cpp',
'src/Layers/xrRender/SkeletonCustom.h',
'src/Layers/xrRender/SkeletonX.h',
'src/xrGame/Actor.cpp',
'src/xrGame/Actor.h',
'src/xrGame/ActorAnimation.cpp',
'src/xrGame/ActorBackpack.cpp',
'src/xrGame/ActorBackpack.h',
'src/xrGame/ActorCameras.cpp',
'src/xrGame/ActorCondition.cpp',
'src/xrGame/ActorCondition.h',
'src/xrGame/ActorCondition_script.cpp',
'src/xrGame/ActorInput.cpp',
'src/xrGame/Actor_Movement.cpp',
'src/xrGame/Actor_Network.cpp',
'src/xrGame/Actor_Weapon.cpp',
'src/xrGame/BodycamNative.cpp',
'src/xrGame/BodycamNative.h',
'src/xrGame/CameraRig.cpp',
'src/xrGame/EffectorBobbing.cpp',
'src/xrGame/HudAnimatorManager.cpp',
'src/xrGame/HudAnimatorManager.h',
'src/xrGame/HudItem.cpp',
'src/xrGame/HudItemAnimator.cpp',
'src/xrGame/HudItemAnimator.h',
'src/xrGame/Inventory.cpp',
'src/xrGame/Level_input.cpp',
'src/xrGame/MainMenu.cpp',
'src/xrGame/MainMenu.h',
'src/xrGame/Missile.cpp',
'src/xrGame/Weapon.cpp',
'src/xrGame/Weapon.h',
'src/xrGame/WeaponMagazined.cpp',
'src/xrGame/actor_script.cpp',
'src/xrGame/addon_manager_script.cpp',
'src/xrGame/eatable_item.cpp',
'src/xrGame/eatable_item.h',
'src/xrGame/player_hud.cpp',
'src/xrGame/player_hud.h',
'src/xrGame/ui/UIActorMenuBase_props.cpp',
'src/xrGame/ui/ui_export_script.cpp']
new=[
'src/xrGame/BodyHealth.h',
'src/xrGame/HumanoidPose.cpp',
'src/xrGame/HumanoidPose.h',
'src/xrGame/HumanoidPoseAdapter.cpp',
'src/xrGame/HumanoidPoseAdapter.h',
'src/xrGame/HumanoidRigProfile.cpp',
'src/xrGame/HumanoidRigProfile.h',
'tools/humanoid/README.md',
'tools/humanoid/ogf_binary.py',
'tools/humanoid/prepare_resources.py',
'tools/humanoid/profile.json',
'tools/humanoid/validate_library.py',
'tools/humanoid/validate_resources.py']
for rel in selected+new:
    f=source/rel
    assert f.is_file(), rel
    targetfile=target/rel
    targetfile.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(f,targetfile)
(target/'manifests').mkdir(exist_ok=True)
manifest={'upstream_commit':revision,'source':str(source),'date':datetime.datetime.now().isoformat(),
          'movement_overlays':selected,'new_movement_source':new,
          'shared_injury_contract':['src/xrGame/ActorCondition.cpp','src/xrGame/ActorCondition.h','src/xrGame/ActorCondition_script.cpp','src/xrGame/BodyHealth.h'],
          'excluded_from_movement':['CharacterPhysicsSupport','level_script','Level_bullet_manager_firetrace','PHSkeleton','WallmarksEngine']}
(target/'manifests/engine-import.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
print(json.dumps({'revision':revision,'overlays':len(selected),'new_files':len(new),
 'src_files':sum(1 for p in (target/'src').rglob('*') if p.is_file())},ensure_ascii=False))