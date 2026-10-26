#pragma once
#include "HumanoidPose.h"

namespace Humanoid
{
struct PoseLayer
{
	const PoseSnapshot* Pose = nullptr;
	Fmatrix ToBody = Fidentity;
	u32 Mask = BothArms;
	float Weight = 1.f;
};

struct LimbSolution
{
	Fvector Joint = {0.f, 0.f, 0.f};
	Fvector End = {0.f, 0.f, 0.f};
	float ReachError = 0.f;
	bool Valid = false;
};

struct MotorState
{
	float LeftArmStrength = 1.f;
	float RightArmStrength = 1.f;
	float LeftLegStrength = 1.f;
	float RightLegStrength = 1.f;
	bool FeetPlanted = false;
};

// The camera rig and pose adapter share this pure two-segment geometry solver.
LimbSolution SolveLimb(const Fvector& Root, const Fvector& Target, float UpperLength,
	float LowerLength, const Fvector& AuthoredElbow, const Fvector& FallbackElbow);

class PoseAdapter
{
public:
	bool Evaluate(const RigProfile& Target, const PoseSnapshot& Body, const PoseLayer* Layers,
		u32 LayerCount, PoseResult& Result, xr_string& Error, const MotorState* Motor = nullptr) const;
};
}
