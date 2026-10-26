#pragma once
#include "HumanoidRigProfile.h"

namespace Humanoid
{
class PoseAdapter;

// Copies already evaluated transforms. Capture never evaluates animation or events.
class PoseSnapshot
{
public:
	bool Capture(IKinematics* Model, const RigProfile& Profile, u32 Frame, const char* Source,
		const char* Clip, float Time, xr_string& Error);
	const xr_vector<Fmatrix>& Transforms() const { return ModelTransforms; }
	const RigProfile* Profile() const { return SourceProfile; }
	u32 Frame() const { return SampleFrame; }
	const char* Source() const { return SourceName.c_str(); }
	const char* Clip() const { return ClipName.c_str(); }
	float Time() const { return SampleTime; }
	bool Valid() const;
private:
	const RigProfile* SourceProfile = nullptr;
	u32 ProfileRevision = 0;
	u32 SampleFrame = u32(-1);
	shared_str SourceName;
	shared_str ClipName;
	float SampleTime = 0.f;
	xr_vector<Fmatrix> ModelTransforms;
};

class PoseResult
{
public:
	const xr_vector<Fmatrix>& Transforms() const { return ModelTransforms; }
	u32 Frame() const { return SampleFrame; }
	const Fmatrix& ContactTransform() const { return ContactCorrection; }
	float LeftReachError() const { return ReachError[0]; }
	float RightReachError() const { return ReachError[1]; }
	float LeftFootError() const { return FootError[0]; }
	float RightFootError() const { return FootError[1]; }
	bool Valid() const;
	bool Apply(IKinematics* Model, const RigProfile& Profile, xr_string& Error) const;
private:
	friend class PoseAdapter;
	const RigProfile* TargetProfile = nullptr;
	u32 ProfileRevision = 0;
	u32 SampleFrame = u32(-1);
	xr_vector<Fmatrix> ModelTransforms;
	Fmatrix ContactCorrection = Fidentity;
	float ReachError[2] = {};
	float FootError[2] = {};
};
}
