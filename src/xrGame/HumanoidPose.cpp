#include "StdAfx.h"
#include "HumanoidPose.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrEngine/Bone.h"

namespace Humanoid
{
bool PoseSnapshot::Valid() const
{
	return SourceProfile && SourceProfile->Valid() && SourceProfile->Revision() == ProfileRevision &&
		ModelTransforms.size() == SourceProfile->Bones().size();
}

bool PoseSnapshot::Capture(IKinematics* Model, const RigProfile& Profile, u32 Frame, const char* Source,
	const char* Clip, float Time, xr_string& Error)
{
	if (!Profile.Matches(Model) || !std::isfinite(Time))
	{
		Error = "humanoid snapshot: invalid source instance, profile revision or time";
		return false;
	}
	// One immutable sample per source and frame. A read cannot advance or resample it.
	if (Valid() && SourceProfile == &Profile && SampleFrame == Frame)
	{
		return true;
	}
	xr_vector<Fmatrix> Captured;
	Captured.reserve(Profile.Bones().size());
	for (u16 Index = 0; Index < Profile.Bones().size(); ++Index)
	{
		const Fmatrix& Transform = Model->LL_GetTransform(Index);
		Fvector Cross;
		Cross.crossproduct(Transform.i, Transform.j);
		if (!_valid(Transform) || std::abs(Cross.dotproduct(Transform.k)) < 0.00001f)
		{
			Error = "humanoid snapshot: non-finite or singular transform: ";
			Error += Profile.Bones()[Index].Name.c_str();
			return false;
		}
		Captured.push_back(Transform);
	}
	ModelTransforms.swap(Captured);
	SourceProfile = &Profile;
	ProfileRevision = Profile.Revision();
	SampleFrame = Frame;
	SourceName = Source ? Source : "";
	ClipName = Clip ? Clip : "";
	SampleTime = Time;
	Error.clear();
	return true;
}

bool PoseResult::Valid() const
{
	return TargetProfile && TargetProfile->Valid() && TargetProfile->Revision() == ProfileRevision &&
		ModelTransforms.size() == TargetProfile->Bones().size();
}

bool PoseResult::Apply(IKinematics* Model, const RigProfile& Profile, xr_string& Error) const
{
	if (!Valid() || TargetProfile != &Profile || !Profile.Matches(Model))
	{
		Error = "humanoid apply: result belongs to another model instance or profile revision";
		return false;
	}
	for (u16 Index = 0; Index < ModelTransforms.size(); ++Index)
	{
		CBoneInstance& Bone = Model->LL_GetBoneInstance(Index);
		Bone.mTransform = ModelTransforms[Index];
		Bone.mRenderTransform.mul_43(Bone.mTransform, Profile.Bones()[Index].InverseBind);
	}
	Model->CalculateBBox(true);
	Error.clear();
	return true;
}
}
