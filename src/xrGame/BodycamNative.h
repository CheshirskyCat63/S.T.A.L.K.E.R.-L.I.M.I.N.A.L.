#pragma once

class CActor;
class CHudItem;
class CWeapon;
struct attachable_hud_item;
struct lua_State;

namespace BodycamNative
{
	void ScriptRegister(lua_State* State);
	bool SecondaryInputState(int Scan, bool Physical);
	void UpdateFrame(CActor* Actor);
	float AdjustCameraHeight(CActor* Actor, float NativeHeight);
	bool OwnsObstaclePose(CHudItem* Item);
	void AdjustFireOrigin(CActor* Actor, CWeapon* Weapon, Fvector& Origin);
	bool HandleMouse(CActor* Actor, float Yaw, float Pitch);
	bool UiBlocked();
	bool MovementReactionAllowed();
	bool BeginSettingsPreview();
	void EndSettingsPreview();
	bool SettingsPreviewActive();
	bool RefreshSettingsPreview(const Fvector& Position, const Fvector& Rotation, float Ratio, bool Enabled);
	void AlignSettingsWeapon(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud);
	bool UsesAim(CWeapon* Weapon);
	bool ApplyFireAim(CActor* Actor, CWeapon* Weapon, const Fvector& Origin, Fvector& Direction);
	void RecordShot(CWeapon* Weapon, const Fvector& Origin, const Fvector& Direction);
	void InvalidateAimPose();
	void RefreshAimPose(CWeapon* Weapon);
	void AlignWeapon(Fmatrix& RightHands, Fmatrix* LeftHands, attachable_hud_item* Hud, const Fmatrix& Camera);
	float ZoomFactor();
	void AdjustHands(Fvector& Position, Fvector& Rotation);
	float HudFov(CHudItem* Item, float Original);
	bool HandsReloadPending();
	void CommitHandsReload();
	shared_str ResolveHandsVisual(const shared_str& Section, const shared_str& Original);
	float MovementPower(u8 Layer);
	float MovementSpeed(u8 Layer);
	bool IsCameraMountEnabled();
	bool OwnsCameraMovement();
}
