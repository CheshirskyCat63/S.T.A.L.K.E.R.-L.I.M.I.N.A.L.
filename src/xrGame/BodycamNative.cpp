#include "StdAfx.h"
#include "pch_script.h"
#include "BodycamNative.h"
#include "BodycamMotionMath.h"
#include "BodycamAimMath.h"
#include "BodycamMountMath.h"
#include "../xrEngine/GameMtlLib.h"
#include "EffectorShot.h"
#include "Level.h"
#include "../xrCore/Platform/AtomicFile.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "CameraFirstEye.h"
#include "HudItem.h"
#include "Weapon.h"
#include "Inventory.h"
#include "player_hud.h"
#include "HudAnimatorManager.h"
#include "ai_space.h"
#include "ui/UIGameCustom.h"
#include "../xrScripts/script_engine.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/ObjectAnimator.h"
#include "../xrEngine/IGame_Persistent.h"
#include "MainMenu.h"
#include "../xrEngine/xr_level_controller.h"
#include <cmath>

extern ENGINE_API float psHUD_FOV;

namespace BodycamNative
{
namespace
{
	xrCriticalSection StateLock;
	Fvector HandPosition = {0.f, 0.f, 0.f};
	Fvector HandRotation = {0.f, 0.f, 0.f};
	float FovRatio = 0.f;
	Fvector AimAngles = {0.f, 0.f, 0.f};
	Fvector LastShotOrigin = {0.f, 0.f, 0.f};
	Fvector LastShotDirection = {0.f, 0.f, 1.f};
	Fvector ResolvedAimTarget = {0.f, 0.f, 0.f};
	Fvector LastShotTarget = {0.f, 0.f, 0.f};
	u32 LastShotFrame = 0;
	u32 ShotSerial = 0;
	// Last fully assembled animation/IK pose, before Bodycam aim correction.
	// Never feed the corrected pose back into a subsequent same-frame solve.
	struct AimPoseSource
	{
		player_hud* Owner = nullptr;
		attachable_hud_item* Hud = nullptr;
		CWeapon* Weapon = nullptr;
		Fmatrix* Right = nullptr;
		Fmatrix* Left = nullptr;
		Fmatrix RightBase;
		Fmatrix LeftBase;
		Fmatrix ItemBase;
		Fmatrix Camera;
	};
	AimPoseSource AimPose;

	bool CameraMountEnabled = false;
	float CameraDropMetres = 0.18f;
	float HipSightStrength = 1.f;
	float ObstacleStrength = 1.f;
	float AppliedCameraDrop = 0.f;
	struct MountPoseState
	{
		CWeapon* Weapon = nullptr;
		attachable_hud_item* Hud = nullptr;
		bool SightReferenceValid = false;
		float SightHeight = 0.f;
		BodycamMount::FrameBlend HipRise;
		BodycamMount::FrameBlend Obstruction;
	};
	MountPoseState MountPose;

    struct SettingsPreviewState
    {
        CActor* Actor = nullptr;
        player_hud* Hud = nullptr;
        float SavedFov = 0.f;
        bool Enabled = false;
    };
    SettingsPreviewState SettingsPreview;
	bool InputEnabled = false;
	bool HandsEnabled = true;
	bool ReloadHands = false;
	shared_str CurrentHandVisual;
	bool InertiaEnabled = false;
	float LayerPower[10] = {1,1,1,1,1,1,1,1,1,1};
	float LayerSpeed[10] = {1,1,1,1,1,1,1,1,1,1};
	u32 MouseConsumed = 0;
	u32 MotionEpoch = 0;
	class CBodycamCameraEffector final : public CAnimatorCamEffector
	{
		float AuthoredPower = 1.f;
		float ElapsedSeconds = 0.f;
		float BlendSeconds = 0.05f;
		u32 Epoch = 0;
		Fmatrix TransitionFrom;
		Fmatrix LastPose;
	public:
		CBodycamCameraEffector() { TransitionFrom.identity(); LastPose.identity(); }
		void StartMotion(const char* Name, float Power)
		{
			// Reuse a live channel instead of removing it before deferred insertion.
			// Carry only its local ANM displacement; never the mouse-driven camera.
			if (Epoch == MotionEpoch) { TransitionFrom = LastPose; }
			else { TransitionFrom.identity(); }
			Epoch = MotionEpoch;
			AuthoredPower = std::clamp(Power, 0.f, 3.f);
			ElapsedSeconds = 0.f;
			Start(Name);
			BlendSeconds = std::min(0.05f, GetAnimatorLength() * 0.25f);
		}
		bool ProcessCam(SCamEffectorInfo& Info) override
		{
			// Clear also invalidates motions waiting in CameraManager's deferred list.
			if (Epoch != MotionEpoch || !MovementReactionAllowed()) { return false; }
			if (!CEffectorCam::ProcessCam(Info)) { return false; }
			m_objectAnimator->Update(Device.fTimeDelta);
			const Fmatrix& Authored = m_objectAnimator->XFORM();
			Fquaternion Rotation, From, Result;
			Rotation.set(Authored);
			Rotation = BodycamMotion::ScaleRotation(Rotation, AuthoredPower);
			From.set(TransitionFrom);
			const float Blend = BodycamMotion::TransitionWeight(ElapsedSeconds, BlendSeconds);
			Result.slerp(From, Rotation, Blend);
			Result.normalize();
			const float Tail = BodycamMotion::TransitionWeight(fLifeTime, BlendSeconds);
			Result = BodycamMotion::ScaleRotation(Result, Tail);
			LastPose.rotation(Result);
			Fvector Position = Authored.c;
			Position.mul(AuthoredPower);
			LastPose.c.lerp(TransitionFrom.c, Position, Blend);
			LastPose.c.mul(Tail);
			ElapsedSeconds += Device.fTimeDelta;

			Fmatrix Base, Applied;
			Base.identity();
			Base.j = Info.n; Base.k = Info.d;
			Base.i.crossproduct(Info.n, Info.d); Base.c = Info.p;
			Applied.mul(Base, LastPose);
			Info.d = Applied.k; Info.n = Applied.j; Info.p = Applied.c;
			if (m_fov > 0.f) { Info.fFov = m_fov; }
			return true;
		}
	};
	bool Finite(const Fvector& V) { return std::isfinite(V.x) && std::isfinite(V.y) && std::isfinite(V.z); }
	int MotionId(int Id) { return Id >= 7800 ? 18100 + Id - 7800 : 18000 + Id - 1900; }
}

bool SettingsPreviewActive()
{
    return SettingsPreview.Actor && SettingsPreview.Actor == g_actor && SettingsPreview.Actor->g_Alive() &&
        SettingsPreview.Hud && SettingsPreview.Hud == g_player_hud && SettingsPreview.Hud->SettingsPoseValid() &&
        SettingsPreview.Actor->cam_Active() == SettingsPreview.Actor->cam_FirstEye();
}

bool BeginSettingsPreview()
{
    if (SettingsPreviewActive()) { return true; }
    if (!g_actor || !g_actor->g_Alive() || !g_player_hud ||
        g_actor->cam_Active() != g_actor->cam_FirstEye() || !g_player_hud->BeginSettingsPose()) { return false; }
    SettingsPreview.Actor = g_actor;
    SettingsPreview.Hud = g_player_hud;
    SettingsPreview.SavedFov = psHUD_FOV;
    return true;
}

void EndSettingsPreview()
{
    if (SettingsPreview.Hud && SettingsPreview.Hud == g_player_hud) { SettingsPreview.Hud->EndSettingsPose(); }
    if (SettingsPreview.Actor && SettingsPreview.Actor == g_actor) { psHUD_FOV = SettingsPreview.SavedFov; }
    SettingsPreview.Actor = nullptr;
    SettingsPreview.Hud = nullptr;
}

bool RefreshSettingsPreview(const Fvector& Position, const Fvector& Rotation, float Ratio, bool Enabled)
{
    if (!SettingsPreviewActive() || !Finite(Position) || !Finite(Rotation) || !std::isfinite(Ratio)) { return false; }
    SettingsPreview.Enabled = Enabled;
    CHudItem* Item = SettingsPreview.Hud->attached_item(0)->m_parent_hud_item;
    psHUD_FOV = Ratio >= 0.1f && Ratio <= 1.f ? BodycamMount::HipHudFov(Ratio) : Item->GetHudFov();
    return SettingsPreview.Hud->RefreshSettingsPose(Position, Rotation);
}


bool UiBlocked()
{
	if (g_actor == nullptr || !g_actor->g_Alive() || Device.Paused()) { return true; }
	if (g_actor->cam_Active() != g_actor->cam_FirstEye()) { return true; }
	if (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive()) { return true; }
	CUIGameCustom* Ui = CurrentGameUI();
	return Ui != nullptr && Ui->TopInputReceiver() != nullptr;
}

bool MovementReactionAllowed()
{
	if (UiBlocked()) { return false; }
	CHudAnimatorManager* Manager = g_actor->HudAnimator();
	if (!Manager) { return true; }
	CHudAnimatorBase* Animators[] = {Manager->CurrentAnimator(), Manager->TargetAnimator(),
		Manager->ItemAnimator(), Manager->HudStateAnimator(), Manager->BackpackAnimator(),
		Manager->PdaAnimator(), Manager->BurnAnimator()};
	for (CHudAnimatorBase* Animator : Animators)
	{
		if (Animator && Animator->IsActive() && !Animator->NeedMovementBlend()) { return false; }
	}
	return true;
}

void UpdateFrame(CActor* Actor)
{
    if (Actor != g_actor) { return; }
    static u32 LastFrame = u32(-1);
    if (LastFrame == Device.dwFrame) { return; }
    LastFrame = Device.dwFrame;
    luabind::functor<void> Update;
    if (ai().script_engine().functor("liminal_bodycam_weapon.actor_on_update", Update)) { Update(); }
}

void ConfigureCamera(bool Enabled, float DropMetres, float SightStrength, float CoverStrength)
{
	xrCriticalSectionGuard Guard(StateLock);
	CameraMountEnabled = Enabled;
	CameraDropMetres = std::isfinite(DropMetres) ? std::clamp(DropMetres, 0.f, 0.35f) : 0.18f;
	HipSightStrength = std::isfinite(SightStrength) ? std::clamp(SightStrength, 0.f, 1.f) : 1.f;
	ObstacleStrength = std::isfinite(CoverStrength) ? std::clamp(CoverStrength, 0.f, 1.5f) : 1.f;
	if (!Enabled) { MountPose = MountPoseState(); }
	if (ObstacleStrength == 0.f) { MountPose.Obstruction.Reset(); }
}

float AdjustCameraHeight(CActor* Actor, float NativeHeight)
{
	xrCriticalSectionGuard Guard(StateLock);
	const bool Enabled = CameraMountEnabled && Actor == g_actor && Actor->g_Alive() &&
		Actor->cam_Active() == Actor->cam_FirstEye();
	// Inventory and ADS do not move a physical body-mounted camera back to the eyes.
	const float Height = BodycamMount::CameraHeight(NativeHeight, Enabled, CameraDropMetres);
	if (Actor == g_actor) { AppliedCameraDrop = NativeHeight - Height; }
	return Height;
}

float GetCameraDrop() { xrCriticalSectionGuard Guard(StateLock); return AppliedCameraDrop; }
float GetObstructionFraction() { xrCriticalSectionGuard Guard(StateLock); return MountPose.Obstruction.Value; }

void SetInputEnabled(bool Enabled)
{
	xrCriticalSectionGuard Guard(StateLock);
	InputEnabled = Enabled;
	if (!Enabled) { HandPosition.set(0,0,0); HandRotation.set(0,0,0); AimAngles.set(0,0,0); FovRatio = 0.f; AimPose.Hud = nullptr; MountPose = MountPoseState(); }
}

void ClearHud()
{
	xrCriticalSectionGuard Guard(StateLock);
	HandPosition.set(0,0,0); HandRotation.set(0,0,0); FovRatio = 0.f; AimPose.Hud = nullptr; MountPose = MountPoseState();
}

void SetHudOffset(const Fvector& Position, const Fvector& Rotation)
{
	if (!Finite(Position) || !Finite(Rotation)) { ClearHud(); return; }
	xrCriticalSectionGuard Guard(StateLock);
	HandPosition = Position; HandRotation = Rotation;
	HandPosition.clamp(Fvector().set(-0.5f,-0.5f,-0.5f), Fvector().set(0.5f,0.5f,0.5f));
	HandRotation.clamp(Fvector().set(-90.f,-90.f,-90.f), Fvector().set(90.f,90.f,90.f));
}

// The logical angles are published by Lua immediately after every input event.
// Do not cache aim by Device.dwFrame: another mouse event can arrive in that frame.
Fvector SetAimAngles(float Yaw, float Pitch)
{
	xrCriticalSectionGuard Guard(StateLock);
	if (!std::isfinite(Yaw) || !std::isfinite(Pitch)) { return AimAngles; }
	Yaw = std::clamp(Yaw, -PI_DIV_2, PI_DIV_2);
	Pitch = std::clamp(Pitch, -PI_DIV_2, PI_DIV_2);
	if (g_actor)
	{
		CCameraBase* Camera = g_actor->cam_FirstEye();
		// Keep the composed target inside native camera limits before ADS hands
		// residual to the camera. Return exactly what was accepted to Lua.
		if (Camera->bClampYaw)
		{
			Yaw = BodycamAim::ConstrainResidual(Camera->yaw, Yaw, Camera->lim_yaw[0], Camera->lim_yaw[1]);
		}
		if (Camera->bClampPitch)
		{
			Pitch = BodycamAim::ConstrainResidual(Camera->pitch, Pitch, Camera->lim_pitch[0], Camera->lim_pitch[1]);
		}
	}
	AimAngles.set(Yaw, Pitch, 0.f);
	return AimAngles;
}

Fvector GetAimAngles()
{
	xrCriticalSectionGuard Guard(StateLock);
	return AimAngles;
}

float ZoomFactor()
{
	if (!g_actor || !g_actor->inventory().ActiveItem()) { return 0.f; }
	CWeapon* Weapon = g_actor->inventory().ActiveItem()->cast_weapon();
	return Weapon ? std::clamp(Weapon->GetAimFactor(), 0.f, 1.f) : 0.f;
}

bool UsesAim(CWeapon* Weapon)
{
	if (!Weapon || UiBlocked() || !g_actor->HUDview() || Weapon->H_Parent() != g_actor ||
		Weapon->cast_weapon_knife() || Weapon->cast_weapon_binoculars() || Weapon->IsGrenadeMode() ||
		!g_actor->inventory().ActiveItem() || g_actor->inventory().ActiveItem()->cast_weapon() != Weapon)
	{
		return false;
	}
	xrCriticalSectionGuard Guard(StateLock);
	return InputEnabled;
}

Fvector AimDirection(const Fvector& Angles)
{
	if (!g_actor) { return Fvector().set(0.f, 0.f, 1.f); }
	CCameraBase* Camera = g_actor->cam_FirstEye();
	float Yaw, Pitch;
	BodycamAim::ComposeAngles(Camera->yaw, Camera->pitch, Angles.x, Angles.y, Yaw, Pitch);
	// Same rotation convention as CCameraBase::Update, without camera noise or
	// presentation effectors. ChangeHP has already applied gameplay recoil here.
	Fquaternion Rotation;
	Rotation.rotationYawPitchRoll(Camera->roll, Yaw, Pitch);
	Fmatrix Basis;
	Basis.rotation(Rotation).transpose();
	Fvector Direction = Basis.k;
	if (auto* Shot = smart_cast<CCameraShotEffector*>(g_actor->Cameras().GetCamEffector(eCEShot)))
	{
		// Legacy mode additionally offsets ProcessCam. Reproduce only that exact
		// last applied offset; never call ProcessCam or advance recoil a second time.
		const Fvector Offset = Shot->GetAppliedAimOffset();
		float Heading, Elevation;
		Direction.getHP(Heading, Elevation);
		Direction.setHP(Heading + Offset.x, Elevation + Offset.y);
	}
	return Direction.normalize_safe();
}

Fvector GetAimDirection() { return AimDirection(GetAimAngles()); }

Fvector GetAimTarget()
{
	if (!g_actor) { return Fvector().set(0.f, 0.f, 0.f); }
	const Fvector Direction = GetAimDirection();
	// Camera ANM translation is presentation-only; retain the physical eye for
	// the logical target, then converge from the native chosen firing origin.
	const Fvector Origin = g_actor->cam_FirstEye()->vPosition;
	float Range = 1000.f;
	collide::rq_result Query;
	if (Level().ObjectSpace.RayPick(Origin, Direction, Range, collide::rqtBoth, Query, g_actor))
	{
		Range = std::max(Query.range, 0.01f);
	}
	return Fvector().mad(Origin, Direction, Range);
}

// World and HUD have the same view but different lenses. A HUD muzzle must
// pass through this inverse of WorldTargetToHud before querying world geometry.
Fvector HudPointToWorld(const Fvector& Point)
{
	Fvector View;
	Device.mView.transform_tiny(View, Point);
	View.x = BodycamAim::HudSlope(View.x, psHUD_FOV, Device.fFOV);
	View.y = BodycamAim::HudSlope(View.y, psHUD_FOV, Device.fFOV);
	Fmatrix InverseView;
	InverseView.invert(Device.mView);
	InverseView.transform_tiny(View);
	return View;
}

bool WorldBarrel(CWeapon* Weapon, Fvector& Muzzle, Fvector& Direction)
{
	attachable_hud_item* Hud = Weapon ? Weapon->HudItemData() : nullptr;
	if (!Hud || !Hud->m_model || Hud->m_measures.m_fire_bone == BI_NONE) { return false; }
	Fvector Local;
	Hud->m_model->LL_GetTransform(Hud->m_measures.m_fire_bone).transform_tiny(Local, Hud->m_measures.m_fire_point_offset);
	Hud->m_item_transform.transform_tiny(Muzzle, Local);
	const Fvector End = HudPointToWorld(Fvector().add(Muzzle, Hud->m_item_transform.k));
	Muzzle = HudPointToWorld(Muzzle);
	Direction.sub(End, Muzzle).normalize_safe();
	return Finite(Muzzle) && Finite(Direction) && Direction.square_magnitude() > 0.5f;
}

bool PickSolid(const Fvector& Start, const Fvector& Direction, float Range, float& HitDistance)
{
	collide::rq_results Results;
	const collide::ray_defs Ray(Start, Direction, Range, 0, collide::rqtBoth);
	Level().ObjectSpace.RayQuery(Results, Ray, nullptr, nullptr, nullptr, g_actor);
	bool Found = false;
	float Nearest = Range;
	const auto& Triangles = Level().ObjectSpace.GetStaticTris();
	// Inspect every hit: a stack of passable foliage must never conceal a wall.
	// Transparency alone is not passability, so solid glass still blocks a gun.
	for (const collide::rq_result& Query : Results.results)
	{
		if (Query.range < 0.f || Query.range > Nearest) { continue; }
		bool Passable = false;
		if (!Query.O && Query.element >= 0 && size_t(Query.element) < Triangles.size())
		{
			const SGameMtl* Material = GMLib.GetMaterialByIdx(Triangles[Query.element].material);
			Passable = Material && (Material->Flags.is(SGameMtl::flPassable) || Material->Flags.is(SGameMtl::flActorObstacle));
		}
		if (!Passable) { Nearest = Query.range; Found = true; }
	}
	if (Found) { HitDistance = Nearest; }
	return Found;
}

bool OwnsObstaclePose(CHudItem* Item)
{
	CWeapon* Weapon = Item ? Item->cast_weapon() : nullptr;
	if (!UsesAim(Weapon)) { return false; }
	xrCriticalSectionGuard Guard(StateLock);
	return CameraMountEnabled;
}

void AdjustFireOrigin(CActor* Actor, CWeapon* Weapon, Fvector& Origin)
{
	if (Actor != g_actor || !UsesAim(Weapon)) { return; }
	Fvector Muzzle, Direction;
	if (!WorldBarrel(Weapon, Muzzle, Direction)) { return; }
	Origin = Muzzle;
	const Fvector Eye = Actor->cam_FirstEye()->vPosition;
	Fvector Segment = Fvector().sub(Origin, Eye);
	const float Length = Segment.magnitude();
	if (Length <= EPS) { Origin = Eye; return; }
	Segment.div(Length);
	float Hit;
	if (PickSolid(Eye, Segment, Length, Hit))
	{
		Origin.mad(Eye, Segment, std::max(0.f, Hit - 0.015f));
	}
}

Fvector GetWorldMuzzle()
{
	CWeapon* Weapon = g_actor && g_actor->inventory().ActiveItem() ? g_actor->inventory().ActiveItem()->cast_weapon() : nullptr;
	Fvector Muzzle = {0.f, 0.f, 0.f}, Direction;
	WorldBarrel(Weapon, Muzzle, Direction);
	return Muzzle;
}

bool ApplyFireAim(CActor* Actor, CWeapon* Weapon, const Fvector& Origin, Fvector& Direction)
{
	if (Actor != g_actor || !UsesAim(Weapon)) { return false; }
	Fvector Target = GetAimTarget();
	Direction.sub(Target, Origin).normalize_safe();
	bool InCover;
	{
		xrCriticalSectionGuard Guard(StateLock);
		InCover = CameraMountEnabled && ObstacleStrength > 0.f && MountPose.Obstruction.Value > 0.f;
	}
	if (InCover)
	{
		// A tucked gun fires along its visible barrel, not sideways towards the
		// unobstructed mouse target. The logical cursor itself is never steered.
		Fvector Muzzle, Barrel;
		if (WorldBarrel(Weapon, Muzzle, Barrel))
		{
			Direction = Barrel;
			float Range = 1000.f, Hit;
			if (PickSolid(Origin, Direction, Range, Hit)) { Range = std::max(Hit, 0.01f); }
			Target.mad(Origin, Direction, Range);
		}
	}
	xrCriticalSectionGuard Guard(StateLock);
	ResolvedAimTarget = Target;
	return true;
}

void RecordShot(CWeapon* Weapon, const Fvector& Origin, const Fvector& Direction)
{
	if (!UsesAim(Weapon)) { return; }
	xrCriticalSectionGuard Guard(StateLock);
	LastShotOrigin = Origin;
	LastShotDirection = Direction;
	LastShotTarget = ResolvedAimTarget;
	LastShotFrame = Device.dwFrame;
	++ShotSerial;
}

Fvector GetShotOrigin() { xrCriticalSectionGuard Guard(StateLock); return LastShotOrigin; }
Fvector GetShotDirection() { xrCriticalSectionGuard Guard(StateLock); return LastShotDirection; }
Fvector GetShotTarget() { xrCriticalSectionGuard Guard(StateLock); return LastShotTarget; }
u32 GetShotFrame() { xrCriticalSectionGuard Guard(StateLock); return LastShotFrame; }
u32 GetShotSerial() { xrCriticalSectionGuard Guard(StateLock); return ShotSerial; }

Fmatrix RotationBetween(const Fvector& From, const Fvector& To, float Weight)
{
	Fmatrix Rotation;
	Rotation.identity();
	Fvector Axis = Fvector().crossproduct(From, To);
	const float Sine = Axis.magnitude();
	if (Sine <= EPS || Weight <= 0.f) { return Rotation; }
	Axis.div(Sine);
	const float Angle = std::atan2(Sine, std::clamp(From.dotproduct(To), -1.f, 1.f)) * Weight;
	return Rotation.rotation(Axis, Angle);
}

Fvector WorldTargetToHud(const Fvector& World)
{
	Fvector Target;
	Device.mView.transform_tiny(Target, World);
	// World and HUD share a view and aspect ratio, but have independent FOVs.
	Target.x = BodycamAim::HudSlope(Target.x, Device.fFOV, psHUD_FOV);
	Target.y = BodycamAim::HudSlope(Target.y, Device.fFOV, psHUD_FOV);
	Fmatrix InverseView;
	InverseView.invert(Device.mView);
	InverseView.transform_tiny(Target);
	return Target;
}

Fmatrix BarrelRotation(const Fvector& Pivot, const Fvector& Muzzle, const Fvector& Forward, const Fvector& Target)
{
	const Fvector MuzzleOffset = Fvector().sub(Muzzle, Pivot);
	const Fvector TargetOffset = Fvector().sub(Target, Pivot);
	const float Distance = BodycamAim::BarrelDistance(MuzzleOffset.dotproduct(Forward),
		MuzzleOffset.square_magnitude(), TargetOffset.square_magnitude());
	// A target inside the weapon's reach cannot be solved by hand rotation.
	// Retain native collision pose; native fire-origin obstruction still applies.
	if (Distance < 0.f) { return Fmatrix().identity(); }
	const Fvector BarrelPoint = Fvector().mad(MuzzleOffset, Forward, Distance).normalize_safe();
	const Fvector TargetDirection = Fvector(TargetOffset).normalize_safe();
	return RotationBetween(BarrelPoint, TargetDirection, 1.f);
}

bool WeaponReadyForSight(CWeapon* Weapon)
{
	const u32 State = Weapon->GetState();
	return (State == CHUDState::eIdle || State == CWeapon::eFire || State == CWeapon::eFire2) &&
		!g_actor->IsSafemode() && !(g_actor->GetMovementState(ACTOR_DEFS::EMovementStates::eReal) & ACTOR_DEFS::EMoveCommand::mcSprint);
}

void ApplyHipSightHeight(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud, CWeapon* Weapon, bool Presentation = false)
{
	xrCriticalSectionGuard Guard(StateLock);
	if (!CameraMountEnabled) { return; }
    if (Presentation)
    {
        Fmatrix Camera;
        Camera.invert(Device.mView);
        const Fvector Shift = Fvector(Camera.j).mul(MountPose.HipRise.Value * (1.f - ZoomFactor()));
        RightHands.c.add(Shift);
        if (LeftHands) { LeftHands->c.add(Shift); }
        Hud->m_item_transform.c.add(Shift);
        return;
    }
	if (MountPose.Weapon != Weapon || MountPose.Hud != Hud)
	{
		MountPose = MountPoseState();
		MountPose.Weapon = Weapon;
		MountPose.Hud = Hud;
	}
	Fmatrix Camera;
	Camera.invert(Device.mView);
	const bool Ready = WeaponReadyForSight(Weapon);
	const float Phase = ZoomFactor();
	if (!MountPose.SightReferenceValid)
	{
		// Authored aim placement is a stable initial fallback, including a weapon
		// first equipped already in ADS. Never chase its animated muzzle every frame.
		MountPose.SightHeight = -Hud->m_measures.m_hands_positions.hands_offsets[EHudOffsetAxis::eAxisPos][EHudOffsetType::eAim].y;
		if (Ready && Phase < 0.01f)
		{
			Fvector Sight;
			if (Hud->m_aim_bone_id != BI_NONE && Hud->m_aim_bone_id < Hud->m_model->LL_BoneCount())
			{
				Hud->m_item_transform.transform_tiny(Sight, Hud->m_model->LL_GetTransform(u16(Hud->m_aim_bone_id)).c);
			}
			else
			{
				Hud->m_model->LL_GetTransform(Hud->m_measures.m_fire_bone).transform_tiny(Sight, Hud->m_measures.m_fire_point_offset);
				Hud->m_item_transform.transform_tiny(Sight);
				// Models without a sight bone use a modest bore-to-sight separation.
				Sight.mad(Camera.j, 0.025f);
			}
			MountPose.SightHeight = Fvector().sub(Sight, Camera.c).dotproduct(Camera.j);
			MountPose.SightReferenceValid = std::isfinite(MountPose.SightHeight);
			if (!MountPose.SightReferenceValid) { MountPose.SightHeight = 0.f; }
		}
	}
	const float Target = BodycamMount::HipRise(MountPose.SightHeight, HipSightStrength, 0.f, Ready ? 1.f : 0.f);
	const float Rise = MountPose.HipRise.Update(Device.dwFrame, Target, Device.fTimeDelta, 0.08f) * (1.f - Phase);
	const Fvector Shift = Fvector(Camera.j).mul(Rise);
	RightHands.c.add(Shift);
	if (LeftHands) { LeftHands->c.add(Shift); }
	Hud->m_item_transform.c.add(Shift);
}

void ApplyWeaponObstruction(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud, CWeapon* Weapon, bool Presentation = false)
{
	xrCriticalSectionGuard Guard(StateLock);
	if (!CameraMountEnabled || ObstacleStrength <= 0.f) { return; }
	Fmatrix Camera;
	Camera.invert(Device.mView);
	if (!Presentation && MountPose.Obstruction.Frame != Device.dwFrame)
	{
		Fvector Muzzle, Forward;
		float Target = 0.f;
		if (WorldBarrel(Weapon, Muzzle, Forward))
		{
			constexpr float Clearance = 0.18f;
			constexpr float Radius = 0.045f;
			const Fvector Eye = g_actor->cam_FirstEye()->vPosition;
			const Fvector Tip = Fvector().mad(Muzzle, Forward, Clearance);
			Fvector Direction = Fvector().sub(Tip, Eye);
			const float Range = Direction.magnitude();
			if (Range > EPS && Range < 3.f)
			{
				Direction.div(Range);
				const float Reach = Fvector().sub(Muzzle, Eye).dotproduct(Direction);
				const Fvector Offsets[] = {{0.f, 0.f, 0.f}, Fvector(Camera.i).mul(Radius), Fvector(Camera.i).mul(-Radius),
					Fvector(Camera.j).mul(Radius), Fvector(Camera.j).mul(-Radius)};
				for (const Fvector& Offset : Offsets)
				{
					float Hit;
					if (PickSolid(Fvector().add(Eye, Offset), Direction, Range, Hit))
					{
						Target = std::max(Target, BodycamMount::Obstruction(Hit, Reach, Range - Reach));
					}
				}
			}
		}
		const float Seconds = Target > MountPose.Obstruction.Value ? 0.035f : 0.12f;
		MountPose.Obstruction.Update(Device.dwFrame, Target, Device.fTimeDelta, Seconds);
		if (Target == 0.f && MountPose.Obstruction.Value < 0.001f) { MountPose.Obstruction.Value = 0.f; }
	}
	const float Amount = MountPose.Obstruction.Value * ObstacleStrength;
	if (Amount <= 0.f) { return; }
	const float Angle = deg2rad(28.f) * Amount;
	const Fvector Down = Fvector().mul(Camera.k, std::cos(Angle)).mad(Camera.j, -std::sin(Angle));
	Fmatrix Correction = RotationBetween(Camera.k, Down, 1.f);
	Fvector RotatedPivot;
	Correction.transform_dir(RotatedPivot, RightHands.c);
	Correction.c.sub(RightHands.c, RotatedPivot).mad(Camera.k, -0.24f * Amount).mad(Camera.j, -0.04f * Amount);
	RightHands.mulA_43(Correction);
	if (LeftHands) { LeftHands->mulA_43(Correction); }
	Hud->m_item_transform.mulA_43(Correction);
}

void SolveWeaponAlignment(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud, bool Presentation = false)
{
	CWeapon* Weapon = Hud && Hud->m_parent_hud_item ? Hud->m_parent_hud_item->cast_weapon() : nullptr;
    const bool Eligible = Presentation ? SettingsPreviewActive() && SettingsPreview.Enabled && Weapon &&
        Hud == SettingsPreview.Hud->attached_item(0) && Weapon->H_Parent() == g_actor : UsesAim(Weapon);
	if (!Eligible || !Hud->m_model || Hud->m_measures.m_fire_bone == BI_NONE ||
		!g_player_hud || g_player_hud->GetAnimator()) { return; }
	ApplyHipSightHeight(RightHands, LeftHands, Hud, Weapon, Presentation);
	const float HipWeight = 1.f - ZoomFactor();
	const Fvector WorldTarget = GetAimTarget();
	const Fvector Eye = g_actor->cam_FirstEye()->vPosition;
	const float Range = WorldTarget.distance_to(Eye);
	const Fvector ZeroTarget = Fvector().mad(Eye, AimDirection(Fvector().set(0.f, 0.f, 0.f)), Range);
	const Fvector Target = WorldTargetToHud(WorldTarget);
	const Fvector Center = WorldTargetToHud(ZeroTarget);

	Fvector Muzzle;
	Hud->m_model->LL_GetTransform(Hud->m_measures.m_fire_bone).transform_tiny(Muzzle, Hud->m_measures.m_fire_point_offset);
	Hud->m_item_transform.transform_tiny(Muzzle);
	const Fvector Forward = Fvector(Hud->m_item_transform.k).normalize_safe();
	const Fvector Pivot = RightHands.c;
	const Fmatrix AimRotation = BarrelRotation(Pivot, Muzzle, Forward, Target);
	const Fmatrix CenterRotation = BarrelRotation(Pivot, Muzzle, Forward, Center);
	// qAim * inverse(qCenter) is the residual cursor rotation. Keep it even in
	// full ADS, while releasing only the zero-cursor calibration to authored sights.
	Fmatrix InverseCenter;
	InverseCenter.invert(CenterRotation);
	Fmatrix Residual;
	Residual.mul_43(AimRotation, InverseCenter);
	Fquaternion CenterQuaternion, Identity, Weighted;
	CenterQuaternion.set(CenterRotation);
	Identity.identity();
	Weighted.slerp(Identity, CenterQuaternion, HipWeight);
	Weighted.normalize();
	Fmatrix Calibration;
	Calibration.rotation(Weighted);
	Fmatrix Correction;
	Correction.mul_43(Residual, Calibration);
	Fvector RotatedPivot;
	Correction.transform_dir(RotatedPivot, Pivot);
	Correction.c.sub(Pivot, RotatedPivot);
	RightHands.mulA_43(Correction);
	if (LeftHands) { LeftHands->mulA_43(Correction); }
	Hud->m_item_transform.mulA_43(Correction);
	ApplyWeaponObstruction(RightHands, LeftHands, Hud, Weapon, Presentation);
}

void AlignSettingsWeapon(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud)
{
    SolveWeaponAlignment(RightHands, LeftHands, Hud, true);
}

void InvalidateAimPose()
{
	xrCriticalSectionGuard Guard(StateLock);
	AimPose.Hud = nullptr;
}

void AlignWeapon(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud, const Fmatrix& Camera)
{
	CWeapon* Weapon = Hud && Hud->m_parent_hud_item ? Hud->m_parent_hud_item->cast_weapon() : nullptr;
	if (!UsesAim(Weapon) || !g_player_hud || g_player_hud->GetAnimator()) { return; }
	{
		xrCriticalSectionGuard Guard(StateLock);
		AimPose.Owner = g_player_hud;
		AimPose.Hud = Hud;
		AimPose.Weapon = Weapon;
		AimPose.Right = &RightHands;
		AimPose.Left = LeftHands;
		AimPose.RightBase = RightHands;
		if (LeftHands) { AimPose.LeftBase = *LeftHands; }
		AimPose.ItemBase = Hud->m_item_transform;
		AimPose.Camera = Camera;
	}
	SolveWeaponAlignment(RightHands, LeftHands, Hud);
}

void RefreshAimPose(CWeapon* Weapon)
{
	if (!UsesAim(Weapon) || !g_player_hud || g_player_hud->GetAnimator()) { return; }
	attachable_hud_item* Hud = Weapon->HudItemData();
	AimPoseSource Source;
	{
		xrCriticalSectionGuard Guard(StateLock);
		if (!Hud || AimPose.Hud != Hud || AimPose.Weapon != Weapon || AimPose.Owner != g_player_hud || !AimPose.Right) { return; }
		Source = AimPose;
	}
	// Rebase the last completed native pose to the current native HUD camera.
	// This reads state only: no second animation, IK, inertia or recoil update.
	Fmatrix Camera, InverseCamera, Delta;
	g_actor->Cameras().hud_camera_Matrix(Camera);
	InverseCamera.invert(Source.Camera);
	Delta.mul_43(Camera, InverseCamera);
	Source.Right->mul_43(Delta, Source.RightBase);
	if (Source.Left) { Source.Left->mul_43(Delta, Source.LeftBase); }
	Hud->m_item_transform.mul_43(Delta, Source.ItemBase);
	SolveWeaponAlignment(*Source.Right, Source.Left, Hud);
}

Fvector ProjectAimPoint(const Fvector& World)
{
	Fvector View, Ndc;
	Device.mView.transform_tiny(View, World);
	if (View.z <= EPS) { return Fvector().set(-10.f, -10.f, -10.f); }
	Device.mFullTransform.transform(Ndc, World);
	return Fvector().set((Ndc.x + 1.f) * 0.5f, (1.f - Ndc.y) * 0.5f, Ndc.z);
}

Fvector GetAimScreenPoint()
{
	return g_actor ? ProjectAimPoint(GetAimTarget()) : Fvector().set(-10.f, -10.f, -10.f);
}

Fvector GetShotScreenPoint()
{
	if (!GetShotSerial()) { return Fvector().set(-10.f, -10.f, -10.f); }
	return ProjectAimPoint(GetShotTarget());
}

void AdjustHands(Fvector& Position, Fvector& Rotation)
{
	if (UiBlocked() || !g_player_hud || g_player_hud->GetAnimator()) { return; }
	xrCriticalSectionGuard Guard(StateLock);
	if (!InputEnabled) { return; }
	Position.add(HandPosition); Rotation.add(HandRotation);
}

Fvector GetHudPosition() { xrCriticalSectionGuard Guard(StateLock); return HandPosition; }
Fvector GetHudRotation() { xrCriticalSectionGuard Guard(StateLock); return HandRotation; }
u32 GetMouseConsumed() { return MouseConsumed; }
Fvector GetCameraAngles()
{
	if (!g_actor) { return Fvector().set(0,0,0); }
	return Fvector().set(g_actor->cam_Active()->yaw, g_actor->cam_Active()->pitch, g_actor->cam_Active()->roll);
}

void MoveCamera(float Yaw, float Pitch)
{
	if (UiBlocked() || !std::isfinite(Yaw) || !std::isfinite(Pitch)) { return; }
	if (Yaw != 0.f) { g_actor->cam_Active()->Move(Yaw < 0.f ? kLEFT : kRIGHT, std::abs(Yaw)); }
	if (Pitch != 0.f) { g_actor->cam_Active()->Move(Pitch > 0.f ? kUP : kDOWN, std::abs(Pitch)); }
}

bool HandleMouse(CActor* Actor, float Yaw, float Pitch)
{
	if (Actor != g_actor || UiBlocked()) { ClearHud(); return false; }
	{ xrCriticalSectionGuard Guard(StateLock); if (!InputEnabled) { return false; } }
	luabind::functor<bool> Handler;
	if (ai().script_engine().functor("liminal_bodycam_weapon.on_mouse_radians", Handler) && Handler(Yaw, Pitch))
	{
		++MouseConsumed;
		return true;
	}
	return false;
}

void SetFovRatio(float Ratio)
{
	xrCriticalSectionGuard Guard(StateLock);
	// Donor hud_fov is a ratio in [0.1,1]. Out-of-range commands must not
    // silently turn into a much wider lens (RC1 imported 1.5 as 112.5 degrees).
    FovRatio = std::isfinite(Ratio) && Ratio >= 0.1f && Ratio <= 1.f ? Ratio : 0.f;
}
float HudFov(CHudItem* Item, float Original)
{
	if (UiBlocked() || !g_actor->inventory().ActiveItem() || g_actor->inventory().ActiveItem()->cast_hud_item() != Item) { return Original; }
	CWeapon* Weapon = Item->cast_weapon();
	if (!Weapon) { return Original; }
	xrCriticalSectionGuard Guard(StateLock);
	return InputEnabled && FovRatio > 0.f ? BodycamMount::HipHudFov(FovRatio) : Original;
}

// Read-only evidence from the SAME final weapon matrix and HUD projection
// used by rendering, not from logical yaw/offset state.
Fvector GetRenderedWeaponPoint(int Point)
{
    if (!g_actor || !g_actor->inventory().ActiveItem()) { return Fvector().set(-10,-10,-10); }
    CHudItem* Item = g_actor->inventory().ActiveItem()->cast_hud_item();
    attachable_hud_item* Hud = Item ? Item->HudItemData() : nullptr;
    if (!Hud || !Hud->m_model) { return Fvector().set(-10,-10,-10); }
    Fvector Local = {0.f,0.f,0.f};
    if ((Point == 0 || Point == 3) && Hud->m_measures.m_fire_bone != BI_NONE)
    {
        Hud->m_model->LL_GetTransform(Hud->m_measures.m_fire_bone).transform_tiny(Local, Hud->m_measures.m_fire_point_offset);
		if (Point == 3) { Local.z += 1.f; }
    }
    else if (Point == 2) { Local.set(0.f,0.05f,0.f); }
    Fvector World, Ndc;
    Hud->m_item_transform.transform_tiny(World, Local);
    Device.mFullTransform_hud.transform(Ndc, World);
    return Fvector().set((Ndc.x+1.f)*0.5f, (1.f-Ndc.y)*0.5f, Ndc.z);
}
float GetEffectiveHudFov() { return psHUD_FOV; }

void SetHandsEnabled(bool Enabled)
{
	xrCriticalSectionGuard Guard(StateLock);
	if (HandsEnabled != Enabled) { HandsEnabled = Enabled; ReloadHands = true; }
}
bool HandsReloadPending() { xrCriticalSectionGuard Guard(StateLock); return ReloadHands; }
void CommitHandsReload() { xrCriticalSectionGuard Guard(StateLock); ReloadHands = false; }
shared_str ResolveHandsVisual(const shared_str& Section, const shared_str& Original)
{
	xrCriticalSectionGuard Guard(StateLock);
	if (HandsEnabled && pSettings->section_exist("liminal_bodycam_hands") && pSettings->line_exist("liminal_bodycam_hands", Section))
	{
		CurrentHandVisual = pSettings->r_string("liminal_bodycam_hands", Section.c_str());
		return CurrentHandVisual;
	}
	CurrentHandVisual = Original;
	return Original;
}
const char* GetHandsVisual() { return CurrentHandVisual.c_str(); }

void SetMovementLayer(int Layer, float Power, float Speed)
{
	if (Layer < 0 || Layer >= 10 || !std::isfinite(Power) || !std::isfinite(Speed)) { return; }
	xrCriticalSectionGuard Guard(StateLock);
	LayerPower[Layer] = std::clamp(Power, 0.f, 4.f);
	LayerSpeed[Layer] = std::clamp(Speed, 0.1f, 3.f);
}
float MovementPower(u8 Layer)
{
	if (!MovementReactionAllowed()) { return 0.f; }
	xrCriticalSectionGuard Guard(StateLock); return Layer < 10 ? LayerPower[Layer] : 1.f;
}
float MovementSpeed(u8 Layer) { xrCriticalSectionGuard Guard(StateLock); return Layer < 10 ? LayerSpeed[Layer] : 1.f; }

void ClearMotionEffects()
{
	++MotionEpoch;
	if (g_actor)
	{
		for (int Id = 18000; Id < 18032; ++Id) { g_actor->Cameras().RemoveCamEffector((ECamEffectorType)Id); }
		for (int Id = 18100; Id < 18116; ++Id) { g_actor->Cameras().RemoveCamEffector((ECamEffectorType)Id); }
	}
	if (g_player_hud)
	{
		g_player_hud->StopBlendAnm("liminal_bodycam\\camera\\lean_left_crp.anm", true);
		g_player_hud->StopBlendAnm("liminal_bodycam\\camera\\lean_right_crp.anm", true);
		g_player_hud->StopBlendAnm("liminal_bodycam\\camera\\fmode_crp.anm", true);
	}
}
void SetInertiaEnabled(bool Enabled)
{
	{ xrCriticalSectionGuard Guard(StateLock); InertiaEnabled = Enabled; }
	if (!Enabled) { ClearMotionEffects(); }
}
bool IsCameraMountEnabled()
{
	xrCriticalSectionGuard Guard(StateLock);
	return CameraMountEnabled;
}

bool OwnsCameraMovement() { xrCriticalSectionGuard Guard(StateLock); return InertiaEnabled; }
float PlayCameraMotion(const char* Name, int Id, float Power, bool AffectHud)
{
	if (!MovementReactionAllowed() || !std::isfinite(Power) || Power <= 0.f) { return 0.f; }
	int Type = MotionId(Id);
	if (Type < 18000 || Type >= 18116) { return 0.f; }
	string_path Path;
	if (!FS.exist(Path, "$game_anims$", Name)) { Msg("! [BODYCAM] Missing camera motion %s", Name); return 0.f; }
	auto* Effector = smart_cast<CBodycamCameraEffector*>(g_actor->Cameras().GetCamEffector((ECamEffectorType)Type));
	if (Effector)
	{
		Effector->SetHudAffect(AffectHud);
		Effector->StartMotion(Name, Power);
		return Effector->GetAnimatorLength();
	}
	Effector = new CBodycamCameraEffector();
	Effector->SetType((ECamEffectorType)Type);
	Effector->SetCyclic(false); Effector->SetHudAffect(AffectHud); Effector->StartMotion(Name, Power);
	g_actor->Cameras().AddCamEffector(Effector);
	return Effector->GetAnimatorLength();
}
void PlayHudMotion(const char* Name, float Speed, float Power)
{
	if (!MovementReactionAllowed() || !g_player_hud || !std::isfinite(Power) || !std::isfinite(Speed)) { return; }
	string_path Path;
	if (!FS.exist(Path, "$game_anims$", Name)) { Msg("! [BODYCAM] Missing HUD motion %s", Name); return; }
	g_player_hud->PlayBlendAnm(Name, std::clamp(Speed,0.1f,3.f), std::clamp(Power,0.f,3.f), Fvector2().set(0.05f,0.1f), false, true, true, 2, 0, 0);
}

void StopHudMotion(const char* Name)
{
	if (g_player_hud) { g_player_hud->StopBlendAnm(Name, true); }
}


bool UiFocused()
{
	if (Device.Paused()) { return true; }
	if (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive()) { return true; }
	CUIGameCustom* Ui = CurrentGameUI();
	return Ui != nullptr && Ui->TopInputReceiver() != nullptr;
}
bool HasActiveWeapon()
{
	return g_actor && g_actor->inventory().ActiveItem() && g_actor->inventory().ActiveItem()->cast_weapon() != nullptr;
}
bool WeaponZoomed()
{
	if (!g_actor || !g_actor->inventory().ActiveItem()) { return false; }
	CWeapon* Weapon = g_actor->inventory().ActiveItem()->cast_weapon();
	return Weapon && Weapon->IsZoomed();
}
bool MoveState(const char* Name)
{
	if (!g_actor) { return false; }
	const u32 Flags = g_actor->GetMovementState(ACTOR_DEFS::EMovementStates::eReal);
	u32 Mask = 0;
	if (!xr_strcmp(Name,"mcCrouch")) { Mask = ACTOR_DEFS::mcCrouch; }
	else if (!xr_strcmp(Name,"mcAccel")) { Mask = ACTOR_DEFS::mcAccel; }
	else if (!xr_strcmp(Name,"mcSprint")) { Mask = ACTOR_DEFS::mcSprint; }
	else if (!xr_strcmp(Name,"mcLLookout")) { Mask = ACTOR_DEFS::mcLLookout; }
	else if (!xr_strcmp(Name,"mcRLookout")) { Mask = ACTOR_DEFS::mcRLookout; }
	return (Flags & Mask) != 0;
}

// Command-line smoke input uses the normal secondary-bind handlers. Physical
// key state is overridden only when explicitly launched with -autotest.
static xr_map<int, bool> TestSecondaryState;
bool SecondaryInputState(int Scan, bool Physical)
{
	if (!strstr(Core.Params, "-autotest")) { return Physical; }
	const auto Found = TestSecondaryState.find(Scan);
	return Found != TestSecondaryState.end() ? Found->second : Physical;
}
void TestSecondary(int Action, bool Press)
{
	if (!g_actor || !strstr(Core.Params, "-autotest")) { return; }
	const int Scan = get_action_dik(static_cast<EGameActions>(Action), 1);
	if (Scan < 0) { return; }
	TestSecondaryState[Scan] = Press;
	if (Press) { g_actor->IR_OnKeyboardPress(Scan); } else { g_actor->IR_OnKeyboardRelease(Scan); }
}
const char* HumanoidState() { return g_player_hud ? g_player_hud->HumanoidDebugState() : "valid=0"; }

void TestMouse(int X, int Y) { if (g_actor) { g_actor->IR_OnMouseMove(X,Y); } }
void TestKey(int Key, bool Press)
{
    if (!g_actor) { return; }
    int Scan = get_action_dik((EGameActions)Key);
    if (Scan < 0) { return; }
    if (Press) { g_actor->IR_OnKeyboardPress(Scan); } else { g_actor->IR_OnKeyboardRelease(Scan); }
}
void TestHold(int Key)
{
    if (!g_actor) { return; }
    int Scan = get_action_dik((EGameActions)Key);
    if (Scan >= 0) { g_actor->IR_OnKeyboardHold(Scan); }
}

bool SaveOptionsAtomic(const char* Text)
{
    string_path Path;
    FS.update_path(Path, "$app_data_root$", "ixr_options.ixr");
    unsigned long Error = 0;
    const bool Saved = Platform::WriteTextFileAtomically(Path, Text, Error);
    if (!Saved) { Msg("! [BODYCAM] Native options save failed (%lu); previous file retained", Error); }
    return Saved;
}

void ScriptRegister(lua_State* State)
{
	using namespace luabind;
	module(State, "bodycam")
	[
		def("save_options_atomic", &SaveOptionsAtomic),
		def("settings_preview_active", &SettingsPreviewActive),
		def("refresh_settings_preview", &RefreshSettingsPreview),
		def("configure_camera", &ConfigureCamera), def("camera_drop", &GetCameraDrop),
		def("obstruction_fraction", &GetObstructionFraction), def("world_muzzle", &GetWorldMuzzle),
		def("set_aim_angles", &SetAimAngles), def("aim_angles", &GetAimAngles), def("zoom_factor", &ZoomFactor),
		def("aim_direction", &GetAimDirection), def("aim_target", &GetAimTarget),
		def("aim_screen_point", &GetAimScreenPoint), def("shot_screen_point", &GetShotScreenPoint),
		def("shot_origin", &GetShotOrigin), def("shot_direction", &GetShotDirection), def("shot_target", &GetShotTarget),
		def("shot_frame", &GetShotFrame), def("shot_serial", &GetShotSerial),
		def("ui_focused", &UiFocused), def("weapon_zoomed", &WeaponZoomed), def("has_active_weapon", &HasActiveWeapon), def("move_state", &MoveState),
		def("movement_reaction_allowed", &MovementReactionAllowed),
		def("stop_hud_motion", &StopHudMotion),
		def("ui_blocked", &UiBlocked), def("set_input_enabled", &SetInputEnabled),
		def("set_hud_offset", &SetHudOffset), def("clear_hud", &ClearHud), def("move_camera", &MoveCamera),
		def("get_hands_visual", &GetHandsVisual), def("hands_pending", &HandsReloadPending),
		def("movement_power", &MovementPower), def("movement_speed", &MovementSpeed),
		def("get_hud_position", &GetHudPosition), def("get_hud_rotation", &GetHudRotation),
		def("get_camera_angles", &GetCameraAngles), def("mouse_consumed", &GetMouseConsumed),
		def("rendered_weapon_point", &GetRenderedWeaponPoint), def("effective_hud_fov", &GetEffectiveHudFov),
		def("set_fov_ratio", &SetFovRatio), def("set_hands_enabled", &SetHandsEnabled),
		def("set_movement_layer", &SetMovementLayer), def("set_inertia_enabled", &SetInertiaEnabled),
		def("play_camera_motion", &PlayCameraMotion), def("play_hud_motion", &PlayHudMotion),
		def("clear_motion_effects", &ClearMotionEffects),
		def("humanoid_state", &HumanoidState), def("test_secondary", &TestSecondary), def("test_mouse", &TestMouse), def("test_key", &TestKey), def("test_hold", &TestHold)
	];
}
}
