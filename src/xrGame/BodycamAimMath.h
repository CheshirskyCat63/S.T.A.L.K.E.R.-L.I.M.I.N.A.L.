#pragma once
#include <cmath>
#include <algorithm>

namespace BodycamAim
{
	inline float ConstrainResidual(float Camera, float Residual, float Minimum, float Maximum)
	{
		return std::clamp(Camera + Residual, Minimum, Maximum) - Camera;
	}

	inline void ComposeAngles(float CameraYaw, float CameraPitch, float AimYaw, float AimPitch, float& Yaw, float& Pitch)
	{
		Yaw = CameraYaw + AimYaw;
		Pitch = CameraPitch + AimPitch;
	}

	inline float BarrelDistance(float MuzzleProjection, float MuzzleLengthSquared, float TargetLengthSquared)
	{
		const float Discriminant = MuzzleProjection * MuzzleProjection + TargetLengthSquared - MuzzleLengthSquared;
		if (Discriminant < 0.f) { return -1.f; }
		const float Distance = -MuzzleProjection + std::sqrt(Discriminant);
		return Distance > 0.f ? Distance : -1.f;
	}

	inline float HudSlope(float WorldSlope, float WorldFov, float HudFov)
	{
		constexpr float DegreesToHalfRadians = 0.008726646259971648f;
		return WorldSlope * std::tan(HudFov * DegreesToHalfRadians) / std::tan(WorldFov * DegreesToHalfRadians);
	}

	inline float HudFov(float Hip, float AuthoredHip, float AuthoredZoom, float WorldFov, float DefaultWorldFov, float HipFactor, float ZoomFactor, float Phase, bool Lensed)
	{
		const float Zoom = AuthoredZoom > 0.f ? AuthoredZoom : Lensed ? AuthoredHip : AuthoredHip * WorldFov / DefaultWorldFov;
		return (Hip + (Zoom - Hip) * Phase) * (HipFactor + (ZoomFactor - HipFactor) * Phase);
	}
}
