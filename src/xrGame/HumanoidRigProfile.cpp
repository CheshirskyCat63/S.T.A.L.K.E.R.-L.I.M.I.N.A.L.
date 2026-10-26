#include "StdAfx.h"
#include "HumanoidRigProfile.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrEngine/Bone.h"

namespace Humanoid
{
namespace
{
struct BoneContract { const char* Name; const char* Parent; u16 Role; };
const char* CanonicalNames[RoleCount] =
{
	"root_stalker",
	"bip01",
	"bip01_pelvis",
	"bip01_l_thigh",
	"bip01_l_calf",
	"bip01_l_foot",
	"bip01_l_toe0",
	"bip01_r_thigh",
	"bip01_r_calf",
	"bip01_r_foot",
	"bip01_r_toe0",
	"bip01_spine",
	"bip01_spine1",
	"bip01_spine2",
	"bip01_neck",
	"bip01_head",
	"eye_left",
	"eye_right",
	"eyelid_1",
	"jaw_1",
	"bip01_l_clavicle",
	"bip01_l_upperarm",
	"bip01_l_forearm",
	"bip01_l_hand",
	"bip01_l_finger0",
	"bip01_l_finger01",
	"bip01_l_finger02",
	"bip01_l_finger1",
	"bip01_l_finger11",
	"bip01_l_finger12",
	"bip01_l_finger2",
	"bip01_l_finger21",
	"bip01_l_finger22",
	"bip01_r_clavicle",
	"bip01_r_upperarm",
	"bip01_r_forearm",
	"bip01_r_hand",
	"bip01_r_finger0",
	"bip01_r_finger01",
	"bip01_r_finger02",
	"bip01_r_finger1",
	"bip01_r_finger11",
	"bip01_r_finger12",
	"bip01_r_finger2",
	"bip01_r_finger21",
	"bip01_r_finger22",
	"bip01_tail",
	"bip01_l_finger3",
	"bip01_l_finger31",
	"bip01_l_finger32",
	"bip01_l_finger4",
	"bip01_l_finger41",
	"bip01_l_finger42",
	"bip01_r_finger3",
	"bip01_r_finger31",
	"bip01_r_finger32",
	"bip01_r_finger4",
	"bip01_r_finger41",
	"bip01_r_finger42",
	"bip01_l_forearm_twist",
	"bip01_r_forearm_twist",
	"hud_root",
	"item_mount",
};
const BoneContract Actor47[] =
{
	{"root_stalker", "", 0},
	{"bip01", "root_stalker", 1},
	{"bip01_pelvis", "bip01", 2},
	{"bip01_l_thigh", "bip01_pelvis", 3},
	{"bip01_l_calf", "bip01_l_thigh", 4},
	{"bip01_l_foot", "bip01_l_calf", 5},
	{"bip01_l_toe0", "bip01_l_foot", 6},
	{"bip01_r_thigh", "bip01_pelvis", 7},
	{"bip01_r_calf", "bip01_r_thigh", 8},
	{"bip01_r_foot", "bip01_r_calf", 9},
	{"bip01_r_toe0", "bip01_r_foot", 10},
	{"bip01_spine", "bip01_pelvis", 11},
	{"bip01_spine1", "bip01_spine", 12},
	{"bip01_spine2", "bip01_spine1", 13},
	{"bip01_neck", "bip01_spine2", 14},
	{"bip01_head", "bip01_neck", 15},
	{"eye_left", "bip01_head", 16},
	{"eye_right", "bip01_head", 17},
	{"eyelid_1", "bip01_head", 18},
	{"jaw_1", "bip01_head", 19},
	{"bip01_l_clavicle", "bip01_neck", 20},
	{"bip01_l_upperarm", "bip01_l_clavicle", 21},
	{"bip01_l_forearm", "bip01_l_upperarm", 22},
	{"bip01_l_hand", "bip01_l_forearm", 23},
	{"bip01_l_finger0", "bip01_l_hand", 24},
	{"bip01_l_finger01", "bip01_l_finger0", 25},
	{"bip01_l_finger02", "bip01_l_finger01", 26},
	{"bip01_l_finger1", "bip01_l_hand", 27},
	{"bip01_l_finger11", "bip01_l_finger1", 28},
	{"bip01_l_finger12", "bip01_l_finger11", 29},
	{"bip01_l_finger2", "bip01_l_hand", 30},
	{"bip01_l_finger21", "bip01_l_finger2", 31},
	{"bip01_l_finger22", "bip01_l_finger21", 32},
	{"bip01_r_clavicle", "bip01_neck", 33},
	{"bip01_r_upperarm", "bip01_r_clavicle", 34},
	{"bip01_r_forearm", "bip01_r_upperarm", 35},
	{"bip01_r_hand", "bip01_r_forearm", 36},
	{"bip01_r_finger0", "bip01_r_hand", 37},
	{"bip01_r_finger01", "bip01_r_finger0", 38},
	{"bip01_r_finger02", "bip01_r_finger01", 39},
	{"bip01_r_finger1", "bip01_r_hand", 40},
	{"bip01_r_finger11", "bip01_r_finger1", 41},
	{"bip01_r_finger12", "bip01_r_finger11", 42},
	{"bip01_r_finger2", "bip01_r_hand", 43},
	{"bip01_r_finger21", "bip01_r_finger2", 44},
	{"bip01_r_finger22", "bip01_r_finger21", 45},
	{"bip01_tail", "bip01_pelvis", 46},
};
const BoneContract Hands42[] =
{
	{"bip01", "", 61},
	{"l_clavicle", "bip01", 20},
	{"l_upperarm", "l_clavicle", 21},
	{"l_forearm", "l_upperarm", 22},
	{"l_forearm_twist", "l_forearm", 59},
	{"l_hand", "l_forearm", 23},
	{"bip01_l_finger1", "l_hand", 27},
	{"bip01_l_finger11", "bip01_l_finger1", 28},
	{"bip01_l_finger12", "bip01_l_finger11", 29},
	{"bip01_l_finger2", "l_hand", 30},
	{"bip01_l_finger21", "bip01_l_finger2", 31},
	{"bip01_l_finger22", "bip01_l_finger21", 32},
	{"bip01_l_finger3", "l_hand", 47},
	{"bip01_l_finger31", "bip01_l_finger3", 48},
	{"bip01_l_finger32", "bip01_l_finger31", 49},
	{"bip01_l_finger4", "l_hand", 50},
	{"bip01_l_finger41", "bip01_l_finger4", 51},
	{"bip01_l_finger42", "bip01_l_finger41", 52},
	{"l_finger0", "l_hand", 24},
	{"l_finger01", "l_finger0", 25},
	{"l_finger02", "l_finger01", 26},
	{"lead_gun", "bip01", 62},
	{"r_clavicle", "bip01", 33},
	{"r_upperarm", "r_clavicle", 34},
	{"r_forearm", "r_upperarm", 35},
	{"r_forearm_twist", "r_forearm", 60},
	{"r_hand", "r_forearm", 36},
	{"bip01_r_finger1", "r_hand", 40},
	{"bip01_r_finger11", "bip01_r_finger1", 41},
	{"bip01_r_finger12", "bip01_r_finger11", 42},
	{"bip01_r_finger2", "r_hand", 43},
	{"bip01_r_finger21", "bip01_r_finger2", 44},
	{"bip01_r_finger22", "bip01_r_finger21", 45},
	{"bip01_r_finger3", "r_hand", 53},
	{"bip01_r_finger31", "bip01_r_finger3", 54},
	{"bip01_r_finger32", "bip01_r_finger31", 55},
	{"bip01_r_finger4", "r_hand", 56},
	{"bip01_r_finger41", "bip01_r_finger4", 57},
	{"bip01_r_finger42", "bip01_r_finger41", 58},
	{"r_finger0", "r_hand", 37},
	{"r_finger01", "r_finger0", 38},
	{"r_finger02", "r_finger01", 39},
};
const BoneContract Body61[] =
{
	{"root_stalker", "", 0},
	{"bip01", "root_stalker", 1},
	{"bip01_pelvis", "bip01", 2},
	{"bip01_l_thigh", "bip01_pelvis", 3},
	{"bip01_l_calf", "bip01_l_thigh", 4},
	{"bip01_l_foot", "bip01_l_calf", 5},
	{"bip01_l_toe0", "bip01_l_foot", 6},
	{"bip01_r_thigh", "bip01_pelvis", 7},
	{"bip01_r_calf", "bip01_r_thigh", 8},
	{"bip01_r_foot", "bip01_r_calf", 9},
	{"bip01_r_toe0", "bip01_r_foot", 10},
	{"bip01_spine", "bip01_pelvis", 11},
	{"bip01_spine1", "bip01_spine", 12},
	{"bip01_spine2", "bip01_spine1", 13},
	{"bip01_neck", "bip01_spine2", 14},
	{"bip01_head", "bip01_neck", 15},
	{"eye_left", "bip01_head", 16},
	{"eye_right", "bip01_head", 17},
	{"eyelid_1", "bip01_head", 18},
	{"jaw_1", "bip01_head", 19},
	{"bip01_l_clavicle", "bip01_neck", 20},
	{"bip01_l_upperarm", "bip01_l_clavicle", 21},
	{"bip01_l_forearm", "bip01_l_upperarm", 22},
	{"bip01_l_hand", "bip01_l_forearm", 23},
	{"bip01_l_finger0", "bip01_l_hand", 24},
	{"bip01_l_finger01", "bip01_l_finger0", 25},
	{"bip01_l_finger02", "bip01_l_finger01", 26},
	{"bip01_l_finger1", "bip01_l_hand", 27},
	{"bip01_l_finger11", "bip01_l_finger1", 28},
	{"bip01_l_finger12", "bip01_l_finger11", 29},
	{"bip01_l_finger2", "bip01_l_hand", 30},
	{"bip01_l_finger21", "bip01_l_finger2", 31},
	{"bip01_l_finger22", "bip01_l_finger21", 32},
	{"bip01_r_clavicle", "bip01_neck", 33},
	{"bip01_r_upperarm", "bip01_r_clavicle", 34},
	{"bip01_r_forearm", "bip01_r_upperarm", 35},
	{"bip01_r_hand", "bip01_r_forearm", 36},
	{"bip01_r_finger0", "bip01_r_hand", 37},
	{"bip01_r_finger01", "bip01_r_finger0", 38},
	{"bip01_r_finger02", "bip01_r_finger01", 39},
	{"bip01_r_finger1", "bip01_r_hand", 40},
	{"bip01_r_finger11", "bip01_r_finger1", 41},
	{"bip01_r_finger12", "bip01_r_finger11", 42},
	{"bip01_r_finger2", "bip01_r_hand", 43},
	{"bip01_r_finger21", "bip01_r_finger2", 44},
	{"bip01_r_finger22", "bip01_r_finger21", 45},
	{"bip01_tail", "bip01_pelvis", 46},
	{"bip01_l_finger3", "bip01_l_hand", 47},
	{"bip01_l_finger31", "bip01_l_finger3", 48},
	{"bip01_l_finger32", "bip01_l_finger31", 49},
	{"bip01_l_finger4", "bip01_l_hand", 50},
	{"bip01_l_finger41", "bip01_l_finger4", 51},
	{"bip01_l_finger42", "bip01_l_finger41", 52},
	{"bip01_r_finger3", "bip01_r_hand", 53},
	{"bip01_r_finger31", "bip01_r_finger3", 54},
	{"bip01_r_finger32", "bip01_r_finger31", 55},
	{"bip01_r_finger4", "bip01_r_hand", 56},
	{"bip01_r_finger41", "bip01_r_finger4", 57},
	{"bip01_r_finger42", "bip01_r_finger41", 58},
	{"bip01_l_forearm_twist", "bip01_l_forearm", 59},
	{"bip01_r_forearm_twist", "bip01_r_forearm", 60},
};

bool Failure(xr_string& Error, const char* Reason, const char* Bone)
{
	Error = "humanoid profile: ";
	Error += Reason;
	if (Bone && *Bone)
	{
		Error += ": ";
		Error += Bone;
	}
	return false;
}
}

void RigProfile::Clear()
{
	Owner = nullptr;
	++ProfileRevision;
	BoneData.clear();
	BoneOrder.clear();
	for (u16& Index : RoleToBone) { Index = InvalidBone; }
}

bool RigProfile::Matches(const IKinematics* Model) const
{
	return Owner && Owner == Model && Model->LL_BoneCount() == BoneData.size();
}

u16 RigProfile::BoneIndex(const char* CanonicalName) const
{
	if (!Owner || !CanonicalName) { return InvalidBone; }
	for (u16 Role = 0; Role < RoleCount; ++Role)
	{
		if (xr_strcmp(CanonicalNames[Role], CanonicalName) == 0) { return RoleToBone[Role]; }
	}
	return InvalidBone;
}

bool RigProfile::Initialize(IKinematics* Model, RigKind Kind, xr_string& Error)
{
	Clear();
	const BoneContract* Contract = Kind == RigKind::Actor47 ? Actor47 : Kind == RigKind::Hands42 ? Hands42 : Body61;
	const u16 Count = Kind == RigKind::Actor47 ? 47 : Kind == RigKind::Hands42 ? 42 : 61;
	if (!Model || Model->LL_BoneCount() != Count)
	{
		string128 Message;
		xr_sprintf(Message, "expected %u bones, received %u", Count, Model ? Model->LL_BoneCount() : 0);
		return Failure(Error, Message, "");
	}
	BoneData.resize(Count);
	u16 Roots = 0;
	for (u16 Index = 0; Index < Count; ++Index)
	{
		const CBoneData& Native = Model->LL_GetData(Index);
		RigBone& Bone = BoneData[Index];
		Bone.Name = Native.name;
		Bone.Parent = Native.GetParentID();
		Bone.BindLocal = Native.bind_transform;
		if (!_valid(Bone.BindLocal)) { return Failure(Error, "non-finite bind transform", Bone.Name.c_str()); }
		if (Bone.Parent == InvalidBone) { ++Roots; }
		else if (Bone.Parent >= Count || Bone.Parent == Index) { return Failure(Error, "invalid parent", Bone.Name.c_str()); }
		for (u16 Previous = 0; Previous < Index; ++Previous)
		{
			if (BoneData[Previous].Name == Bone.Name) { return Failure(Error, "duplicate bone name", Bone.Name.c_str()); }
		}
		const BoneContract* Expected = nullptr;
		for (u16 Spec = 0; Spec < Count; ++Spec)
		{
			if (xr_strcmp(Bone.Name.c_str(), Contract[Spec].Name) == 0) { Expected = &Contract[Spec]; break; }
		}
		if (!Expected) { return Failure(Error, "unknown mandatory bone", Bone.Name.c_str()); }
		const char* ParentName = Bone.Parent == InvalidBone ? "" : Model->LL_GetData(Bone.Parent).name.c_str();
		if (xr_strcmp(ParentName, Expected->Parent) != 0)
		{
			Error = "humanoid profile: parent mismatch for "; Error += Bone.Name.c_str();
			Error += "; expected "; Error += Expected->Parent; Error += "; received "; Error += ParentName;
			return false;
		}
		Bone.Role = Expected->Role;
		if (RoleToBone[Bone.Role] != InvalidBone) { return Failure(Error, "duplicate semantic role", Bone.Name.c_str()); }
		RoleToBone[Bone.Role] = Index;
		const char* Canonical = CanonicalNames[Bone.Role];
		Bone.Arm = strstr(Canonical, "bip01_l_") && !strstr(Canonical, "thigh") && !strstr(Canonical, "calf") && !strstr(Canonical, "foot") && !strstr(Canonical, "toe") ? LeftArm :
			strstr(Canonical, "bip01_r_") && !strstr(Canonical, "thigh") && !strstr(Canonical, "calf") && !strstr(Canonical, "foot") && !strstr(Canonical, "toe") ? RightArm : 0;
	}
	if (Roots != 1) { return Failure(Error, "expected exactly one root", ""); }
	xr_vector<bool> Visited(Count, false);
	while (BoneOrder.size() != Count)
	{
		const size_t Before = BoneOrder.size();
		for (u16 Index = 0; Index < Count; ++Index)
		{
			RigBone& Bone = BoneData[Index];
			if (Visited[Index] || (Bone.Parent != InvalidBone && !Visited[Bone.Parent])) { continue; }
			Bone.BindModel = Bone.BindLocal;
			if (Bone.Parent != InvalidBone) { Bone.BindModel.mul_43(BoneData[Bone.Parent].BindModel, Bone.BindLocal); }
			Fvector Cross; Cross.crossproduct(Bone.BindModel.i, Bone.BindModel.j);
			if (std::abs(Cross.dotproduct(Bone.BindModel.k)) < 0.00001f) { return Failure(Error, "singular bind transform", Bone.Name.c_str()); }
			Bone.InverseBind.invert(Bone.BindModel);
			Visited[Index] = true;
			BoneOrder.push_back(Index);
		}
		if (BoneOrder.size() == Before) { return Failure(Error, "cycle in parent graph", ""); }
	}
	Owner = Model;
	ProfileKind = Kind;
	Error.clear();
	return true;
}
}
