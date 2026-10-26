#pragma once

class IKinematics;

namespace Humanoid
{
enum class RigKind : u8 { Actor47, Hands42, Body61 };
enum ArmMask : u32 { LeftArm = 1, RightArm = 2, BothArms = 3 };
constexpr u16 RoleCount = 63;
constexpr u16 InvalidBone = u16(-1);

struct RigBone
{
	shared_str Name;
	u16 Parent = InvalidBone;
	u16 Role = InvalidBone;
	u32 Arm = 0;
	Fmatrix BindLocal = Fidentity;
	Fmatrix BindModel = Fidentity;
	Fmatrix InverseBind = Fidentity;
};

// A profile belongs to one model instance. No actor, item or camera classification.
class RigProfile
{
public:
	bool Initialize(IKinematics* Model, RigKind Kind, xr_string& Error);
	void Clear();
	bool Matches(const IKinematics* Model) const;
	u16 BoneIndex(const char* CanonicalName) const;
	u16 RoleIndex(u16 Role) const { return Owner && Role < RoleCount ? RoleToBone[Role] : InvalidBone; }
	const xr_vector<RigBone>& Bones() const { return BoneData; }
	const xr_vector<u16>& Order() const { return BoneOrder; }
	RigKind Kind() const { return ProfileKind; }
	u32 Revision() const { return ProfileRevision; }
	bool Valid() const { return Owner != nullptr; }
private:
	IKinematics* Owner = nullptr;
	RigKind ProfileKind = RigKind::Actor47;
	u32 ProfileRevision = 0;
	u16 RoleToBone[RoleCount] = {};
	xr_vector<RigBone> BoneData;
	xr_vector<u16> BoneOrder;
};
}
