#include "StdAfx.h"
#include "HumanoidPoseAdapter.h"

namespace Humanoid
{
namespace
{
constexpr float GeometryEpsilon = 0.00001f;
constexpr float ContactTolerance = 0.001f;

Fmatrix RotationOnly(Fmatrix Value)
{
	Value.c.set(0.f, 0.f, 0.f);
	return Value;
}

Fvector Perpendicular(const Fvector& Direction, Fvector Candidate)
{
	Candidate.mad(Direction, -Candidate.dotproduct(Direction));
	return Candidate;
}

Fvector StablePerpendicular(const Fvector& Direction)
{
	Fvector Axis;
	const float X = std::abs(Direction.x), Y = std::abs(Direction.y), Z = std::abs(Direction.z);
	Axis.set(X <= Y && X <= Z ? 1.f : 0.f, Y < X && Y <= Z ? 1.f : 0.f, Z < X && Z < Y ? 1.f : 0.f);
	return Perpendicular(Direction, Axis).normalize_safe();
}

Fmatrix Local(const PoseSnapshot& Pose, u16 Index)
{
	const u16 Parent = Pose.Profile()->Bones()[Index].Parent;
	if (Parent == InvalidBone) { return Pose.Transforms()[Index]; }
	Fmatrix Inverse, Result;
	Inverse.invert(Pose.Transforms()[Parent]);
	Result.mul_43(Inverse, Pose.Transforms()[Index]);
	return Result;
}

Fmatrix TransferRotation(const RigBone& Destination, const RigBone& Source, const Fmatrix& SourceLocal,
	const Fmatrix& SourceToBody)
{
	Fmatrix SourceBindInverse, Delta, Correction, CorrectionInverse, Temporary, Result;
	SourceBindInverse.invert(RotationOnly(Source.BindLocal));
	Delta.mul_43(SourceBindInverse, RotationOnly(SourceLocal));
	Temporary.mul_43(RotationOnly(SourceToBody), RotationOnly(Source.BindModel));
	Correction.mul_43(RotationOnly(Destination.InverseBind), Temporary);
	CorrectionInverse.invert(Correction);
	Temporary.mul_43(Correction, Delta);
	Delta.mul_43(Temporary, CorrectionInverse);
	Result.mul_43(RotationOnly(Destination.BindLocal), Delta);
	Result.c = Destination.BindLocal.c;
	return Result;
}

Fmatrix BlendLocal(const Fmatrix& From, const Fmatrix& To, float Weight)
{
	Fquaternion A, B, Blended;
	A.set(From); B.set(To);
	Blended.slerp(A, B, Weight);
	Blended.normalize();
	Fmatrix Result;
	Result.rotation(Blended);
	Result.c = From.c;
	return Result;
}

void Compose(const RigProfile& Profile, const xr_vector<Fmatrix>& Locals, xr_vector<Fmatrix>& Model)
{
	Model.resize(Locals.size());
	for (u16 Index : Profile.Order())
	{
		const u16 Parent = Profile.Bones()[Index].Parent;
		if (Parent == InvalidBone) { Model[Index] = Locals[Index]; }
		else { Model[Index].mul_43(Model[Parent], Locals[Index]); }
	}
}

void TransformBranch(const RigProfile& Profile, xr_vector<Fmatrix>& Model, u16 Root, const Fmatrix& Delta)
{
	xr_vector<bool> Affected(Model.size(), false);
	for (u16 Index : Profile.Order())
	{
		const u16 Parent = Profile.Bones()[Index].Parent;
		Affected[Index] = Index == Root || (Parent != InvalidBone && Affected[Parent]);
		if (Affected[Index]) { Model[Index].mulA_43(Delta); }
	}
}

void RotateBranch(const RigProfile& Profile, xr_vector<Fmatrix>& Model, u16 Root,
	const Fvector& Pivot, Fvector From, Fvector To)
{
	From.normalize_safe(); To.normalize_safe();
	Fvector Axis; Axis.crossproduct(From, To);
	const float Sine = Axis.magnitude();
	const float Cosine = std::clamp(From.dotproduct(To), -1.f, 1.f);
	if (Sine <= GeometryEpsilon && Cosine >= 0.f) { return; }
	if (Sine <= GeometryEpsilon) { Axis = StablePerpendicular(From); }
	else { Axis.div(Sine); }
	Fmatrix Delta;
	Delta.rotation(Axis, atan2f(Sine, Cosine));
	Fvector Rotated; Delta.transform_dir(Rotated, Pivot);
	Delta.c.sub(Pivot, Rotated);
	TransformBranch(Profile, Model, Root, Delta);
}

bool SolvePoseLimb(const RigProfile& Profile, xr_vector<Fmatrix>& Model, u16 Upper, u16 Joint, u16 End,
	const Fmatrix& Goal, const Fvector& Pole, const Fvector& Fallback, float& Error)
{
	const Fvector Root = Model[Upper].c;
	const Fvector OldJoint = Model[Joint].c;
	const LimbSolution Solved = SolveLimb(Root, Goal.c, Profile.Bones()[Joint].BindLocal.c.magnitude(),
		Profile.Bones()[End].BindLocal.c.magnitude(), Pole, Fallback);
	if (!Solved.Valid) { return false; }
	RotateBranch(Profile, Model, Upper, Root, Fvector().sub(OldJoint, Root), Fvector().sub(Solved.Joint, Root));
	RotateBranch(Profile, Model, Joint, Solved.Joint, Fvector().sub(Model[End].c, Solved.Joint), Fvector().sub(Solved.End, Solved.Joint));
	Fmatrix Desired = Goal;
	Desired.c = Model[End].c;
	Fmatrix Inverse, Delta;
	Inverse.invert(Model[End]);
	Delta.mul_43(Desired, Inverse);
	TransformBranch(Profile, Model, End, Delta);
	Error = Model[End].c.distance_to(Goal.c);
	return true;
}

bool ApplySupport(const RigProfile& Profile, xr_vector<Fmatrix>& Model, const MotorState& Motor, float* FootError)
{
	if (!Motor.FeetPlanted || std::abs(Motor.LeftLegStrength - Motor.RightLegStrength) <= GeometryEpsilon) { return true; }
	const u16 Upper[] = {Profile.BoneIndex("bip01_l_thigh"), Profile.BoneIndex("bip01_r_thigh")};
	const u16 Joint[] = {Profile.BoneIndex("bip01_l_calf"), Profile.BoneIndex("bip01_r_calf")};
	const u16 End[] = {Profile.BoneIndex("bip01_l_foot"), Profile.BoneIndex("bip01_r_foot")};
	const Fmatrix Feet[] = {Model[End[0]], Model[End[1]]};
	const Fvector Knees[] = {Model[Joint[0]].c, Model[Joint[1]].c};
	const float TotalStrength = Motor.LeftLegStrength + Motor.RightLegStrength;
	if (TotalStrength <= GeometryEpsilon) { return true; }
	Fvector Shift = Fvector().sub(Feet[1].c, Feet[0].c).mul(Motor.RightLegStrength / TotalStrength - 0.5f);
	Shift.y = 0.f;
	// Move support toward the stronger planted foot, bounded by both existing leg lengths.
	for (u32 Iteration = 0; Iteration < 64; ++Iteration)
	{
		float MaximumError = 0.f;
		for (u32 Side = 0; Side < 2; ++Side)
		{
			const float A = Profile.Bones()[Joint[Side]].BindLocal.c.magnitude();
			const float B = Profile.Bones()[End[Side]].BindLocal.c.magnitude();
			if (A <= GeometryEpsilon || B <= GeometryEpsilon) { return false; }
			Fvector Direction = Fvector(Model[Upper[Side]].c).add(Shift).sub(Feet[Side].c);
			const float Distance = Direction.magnitude();
			const float Desired = std::clamp(Distance, std::abs(A - B) + GeometryEpsilon, A + B - GeometryEpsilon);
			MaximumError = std::max(MaximumError, std::abs(Desired - Distance));
			if (Distance > GeometryEpsilon) { Direction.div(Distance); }
			else { Direction.set(0.f, 1.f, 0.f); }
			Shift.mad(Direction, Desired - Distance);
		}
		if (MaximumError <= GeometryEpsilon) { break; }
	}
	Fmatrix Support = Fidentity;
	Support.c = Shift;
	TransformBranch(Profile, Model, Profile.BoneIndex("bip01_pelvis"), Support);
	for (u32 Side = 0; Side < 2; ++Side)
	{
		if (!SolvePoseLimb(Profile, Model, Upper[Side], Joint[Side], End[Side], Feet[Side],
			Fvector(Knees[Side]).add(Shift), Knees[Side], FootError[Side])) { return false; }
	}
	return FootError[0] <= ContactTolerance && FootError[1] <= ContactTolerance;
}

struct ArmTarget
{
	u16 Upper = InvalidBone, Forearm = InvalidBone, Hand = InvalidBone;
	float UpperLength = 0.f, LowerLength = 0.f;
	Fmatrix Goal = Fidentity;
	Fvector Elbow = {0.f, 0.f, 0.f};
	const PoseSnapshot* Source = nullptr;
	bool Active = false;
};

bool PoseFailure(xr_string& Error, const char* Reason, const PoseSnapshot& Source, float Left, float Right)
{
	string1024 Message;
	xr_sprintf(Message, "humanoid pose: %s; source=%s; clip=%s; time=%.6f; frame=%u; left_error_m=%.6f; right_error_m=%.6f",
		Reason, Source.Source(), Source.Clip(), Source.Time(), Source.Frame(), Left, Right);
	Error = Message;
	return false;
}
}

LimbSolution SolveLimb(const Fvector& Root, const Fvector& Target, float UpperLength,
	float LowerLength, const Fvector& AuthoredElbow, const Fvector& FallbackElbow)
{
	LimbSolution Result;
	if (!_valid(Root) || !_valid(Target) || !_valid(AuthoredElbow) || !_valid(FallbackElbow) ||
		!std::isfinite(UpperLength) || !std::isfinite(LowerLength) ||
		UpperLength <= GeometryEpsilon || LowerLength <= GeometryEpsilon)
	{
		return Result;
	}
	Fvector Direction; Direction.sub(Target, Root);
	float Distance = Direction.magnitude();
	if (Distance <= GeometryEpsilon)
	{
		Direction.sub(AuthoredElbow, Root).normalize_safe();
		if (Direction.square_magnitude() <= GeometryEpsilon) { Direction.set(0.f, 0.f, 1.f); }
	}
	else { Direction.div(Distance); }
	const float Minimum = std::abs(UpperLength - LowerLength) + GeometryEpsilon;
	const float Maximum = UpperLength + LowerLength - GeometryEpsilon;
	if (Minimum > Maximum) { return Result; }
	const float Reach = std::clamp(Distance, Minimum, Maximum);
	Fvector Pole = Perpendicular(Direction, Fvector().sub(AuthoredElbow, Root));
	if (Pole.square_magnitude() <= GeometryEpsilon * GeometryEpsilon)
	{
		Pole = Perpendicular(Direction, Fvector().sub(FallbackElbow, Root));
	}
	if (Pole.square_magnitude() <= GeometryEpsilon * GeometryEpsilon) { Pole = StablePerpendicular(Direction); }
	Pole.normalize_safe();
	const float Cosine = std::clamp((UpperLength * UpperLength + Reach * Reach - LowerLength * LowerLength) /
		(2.f * UpperLength * Reach), -1.f, 1.f);
	Result.Joint.mad(Root, Direction, Cosine * UpperLength).mad(Pole, sqrtf(std::max(0.f, 1.f - Cosine * Cosine)) * UpperLength);
	Result.End.mad(Root, Direction, Reach);
	Result.ReachError = Result.End.distance_to(Target);
	Result.Valid = _valid(Result.Joint) && _valid(Result.End);
	return Result;
}

bool PoseAdapter::Evaluate(const RigProfile& Target, const PoseSnapshot& Body, const PoseLayer* Layers,
	u32 LayerCount, PoseResult& Result, xr_string& Error, const MotorState* Motor) const
{
	if (!Target.Valid() || Target.Kind() != RigKind::Body61 || !Body.Valid() ||
		Body.Profile()->Kind() != RigKind::Actor47 || (LayerCount && !Layers))
	{
		Error = "humanoid pose: expected valid Body61 target, Actor47 locomotion and explicit source layers";
		return false;
	}
	if (Motor)
	{
		for (float Strength : {Motor->LeftArmStrength, Motor->RightArmStrength, Motor->LeftLegStrength, Motor->RightLegStrength})
		{
			if (!std::isfinite(Strength) || Strength < 0.f || Strength > 1.f)
			{
				return PoseFailure(Error, "motor strength must be finite and normalized", Body, 0.f, 0.f);
			}
		}
	}
	xr_vector<Fmatrix> Locals;
	Locals.reserve(Target.Bones().size());
	for (const RigBone& Bone : Target.Bones())
	{
		Fmatrix Pose = Bone.BindLocal;
		const u16 Source = Body.Profile()->RoleIndex(Bone.Role);
		if (Source != InvalidBone)
		{
			const Fmatrix SourceLocal = Local(Body, Source);
			Pose = TransferRotation(Bone, Body.Profile()->Bones()[Source], SourceLocal, Fidentity);
			// Root and pelvis carry authored locomotion displacement. Limb lengths stay native.
			if (Bone.Role <= 2)
			{
				Pose.c.add(Fvector().sub(SourceLocal.c, Body.Profile()->Bones()[Source].BindLocal.c));
			}
		}
		Locals.push_back(Pose);
	}
	xr_vector<Fmatrix> BasePose;
	Compose(Target, Locals, BasePose);
	float FootError[2] = {};
	if (Motor)
	{
		if (!ApplySupport(Target, BasePose, *Motor, FootError))
		{
			return PoseFailure(Error, "planted-foot support constraints unreachable (errors are feet)", Body, FootError[0], FootError[1]);
		}
		for (u16 Index : Target.Order())
		{
			const u16 Parent = Target.Bones()[Index].Parent;
			if (Parent == InvalidBone) { Locals[Index] = BasePose[Index]; }
			else
			{
				Fmatrix Inverse; Inverse.invert(BasePose[Parent]);
				Locals[Index].mul_43(Inverse, BasePose[Index]);
			}
		}
	}
	ArmTarget Arms[2];
	const char* UpperNames[] = {"bip01_l_upperarm", "bip01_r_upperarm"};
	const char* ForearmNames[] = {"bip01_l_forearm", "bip01_r_forearm"};
	const char* HandNames[] = {"bip01_l_hand", "bip01_r_hand"};
	for (u32 Side = 0; Side < 2; ++Side)
	{
		Arms[Side].Upper = Target.BoneIndex(UpperNames[Side]);
		Arms[Side].Forearm = Target.BoneIndex(ForearmNames[Side]);
		Arms[Side].Hand = Target.BoneIndex(HandNames[Side]);
		Arms[Side].UpperLength = Target.Bones()[Arms[Side].Forearm].BindLocal.c.magnitude();
		Arms[Side].LowerLength = Target.Bones()[Arms[Side].Hand].BindLocal.c.magnitude();
	}
	for (u32 LayerIndex = 0; LayerIndex < LayerCount; ++LayerIndex)
	{
		const PoseLayer& Layer = Layers[LayerIndex];
		Fvector Cross;
		Cross.crossproduct(Layer.ToBody.i, Layer.ToBody.j);
		if (!Layer.Pose || !Layer.Pose->Valid() || Layer.Pose->Profile()->Kind() != RigKind::Hands42 ||
			Layer.Pose->Frame() != Body.Frame() || !_valid(Layer.ToBody) || !std::isfinite(Layer.Weight) ||
			std::abs(Cross.dotproduct(Layer.ToBody.k)) < GeometryEpsilon ||
			Layer.Weight < 0.f || Layer.Weight > 1.f || (Layer.Mask & ~BothArms))
		{
			return PoseFailure(Error, "invalid source, role mask, weight or mixed-frame snapshot", Body, 0.f, 0.f);
		}
		if (Layer.Weight <= 0.f || !Layer.Mask) { continue; }
		const RigProfile& SourceProfile = *Layer.Pose->Profile();
		for (u16 Index = 0; Index < Target.Bones().size(); ++Index)
		{
			const RigBone& Bone = Target.Bones()[Index];
			if (!(Bone.Arm & Layer.Mask)) { continue; }
			const u16 Source = SourceProfile.RoleIndex(Bone.Role);
			if (Source == InvalidBone) { return PoseFailure(Error, "mandatory arm role missing", *Layer.Pose, 0.f, 0.f); }
			const Fmatrix Transferred = TransferRotation(Bone, SourceProfile.Bones()[Source], Local(*Layer.Pose, Source), Layer.ToBody);
			Locals[Index] = BlendLocal(Locals[Index], Transferred, Layer.Weight);
		}
		for (u32 Side = 0; Side < 2; ++Side)
		{
			if (!(Layer.Mask & (Side == 0 ? LeftArm : RightArm))) { continue; }
			ArmTarget& Arm = Arms[Side];
			const u16 SourceHand = SourceProfile.BoneIndex(HandNames[Side]);
			const u16 SourceElbow = SourceProfile.BoneIndex(ForearmNames[Side]);
			Fmatrix MappedHand, Correction, Desired;
			MappedHand.mul_43(Layer.ToBody, Layer.Pose->Transforms()[SourceHand]);
			Correction.mul_43(RotationOnly(SourceProfile.Bones()[SourceHand].InverseBind), RotationOnly(Target.Bones()[Arm.Hand].BindModel));
			Desired.mul_43(MappedHand, Correction);
			Desired.c = MappedHand.c;
			const Fmatrix& Previous = Arm.Active ? Arm.Goal : BasePose[Arm.Hand];
			Arm.Goal = BlendLocal(Previous, Desired, Layer.Weight);
			Arm.Goal.c.lerp(Previous.c, Desired.c, Layer.Weight);
			Fvector Elbow; Layer.ToBody.transform_tiny(Elbow, Layer.Pose->Transforms()[SourceElbow].c);
			const Fvector PreviousElbow = Arm.Active ? Arm.Elbow : BasePose[Arm.Forearm].c;
			Arm.Elbow.lerp(PreviousElbow, Elbow, Layer.Weight);
			Arm.Source = Layer.Pose;
			Arm.Active = true;
		}
	}

	PoseResult Candidate;
	Candidate.FootError[0] = FootError[0];
	Candidate.FootError[1] = FootError[1];
	Compose(Target, Locals, Candidate.ModelTransforms);
	if (Motor)
	{
		for (u32 Side = 0; Side < 2; ++Side)
		{
			if (!Arms[Side].Active) { continue; }
			const float Strength = Side == 0 ? Motor->LeftArmStrength : Motor->RightArmStrength;
			Arms[Side].Elbow.lerp(BasePose[Arms[Side].Forearm].c, Arms[Side].Elbow, Strength);
		}
	}
	Fvector ContactShift = {0.f, 0.f, 0.f};
	// Project one shared rigid item/hand translation, never clamp the two grips separately.
	for (u32 Iteration = 0; Iteration < 64; ++Iteration)
	{
		float MaximumError = 0.f;
		for (const ArmTarget& Arm : Arms)
		{
			if (!Arm.Active) { continue; }
			Fvector Direction = Fvector(Arm.Goal.c).add(ContactShift).sub(Candidate.ModelTransforms[Arm.Upper].c);
			const float Distance = Direction.magnitude();
			const float Minimum = std::abs(Arm.UpperLength - Arm.LowerLength) + GeometryEpsilon;
			const float Maximum = Arm.UpperLength + Arm.LowerLength - GeometryEpsilon;
			if (Maximum < Minimum) { return PoseFailure(Error, "degenerate target limb lengths", *Arm.Source, 0.f, 0.f); }
			const float Desired = std::clamp(Distance, Minimum, Maximum);
			MaximumError = std::max(MaximumError, std::abs(Desired - Distance));
			if (Distance > GeometryEpsilon) { Direction.div(Distance); }
			else { Direction.set(0.f, 0.f, 1.f); }
			ContactShift.mad(Direction, Desired - Distance);
		}
		if (MaximumError <= GeometryEpsilon) { break; }
	}
	Candidate.ContactCorrection.identity();
	Candidate.ContactCorrection.c = ContactShift;
	for (u32 Side = 0; Side < 2; ++Side)
	{
		ArmTarget& Arm = Arms[Side];
		if (!Arm.Active) { continue; }
		Arm.Goal.c.add(ContactShift);
		Arm.Elbow.add(ContactShift);
		if (!SolvePoseLimb(Target, Candidate.ModelTransforms, Arm.Upper, Arm.Forearm, Arm.Hand,
			Arm.Goal, Arm.Elbow, BasePose[Arm.Forearm].c, Candidate.ReachError[Side]))
		{
			return PoseFailure(Error, "limb solver rejected non-finite or zero-length chain", *Arm.Source, 0.f, 0.f);
		}
	}
	if (Candidate.ReachError[0] > ContactTolerance || Candidate.ReachError[1] > ContactTolerance)
	{
		const PoseSnapshot& Source = Arms[0].Source ? *Arms[0].Source : *Arms[1].Source;
		return PoseFailure(Error, "no shared reachable hand/item placement", Source, Candidate.ReachError[0], Candidate.ReachError[1]);
	}
	for (const Fmatrix& Transform : Candidate.ModelTransforms)
	{
		if (!_valid(Transform)) { return PoseFailure(Error, "non-finite final transform", Body, 0.f, 0.f); }
	}
	Candidate.TargetProfile = &Target;
	Candidate.ProfileRevision = Target.Revision();
	Candidate.SampleFrame = Body.Frame();
	Result = Candidate;
	Error.clear();
	return true;
}
}
