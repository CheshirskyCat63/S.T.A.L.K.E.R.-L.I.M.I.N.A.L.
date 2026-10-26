#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace BodycamMount
{
	inline float CameraHeight(float NativeHeight, bool Enabled, float Drop)
	{
		if (!Enabled || !std::isfinite(Drop)) { return NativeHeight; }
		return std::max(0.25f, NativeHeight - std::clamp(Drop, 0.f, 0.35f));
	}

	inline float HipRise(float SightHeight, float Strength, float Phase, float Ready)
	{
		return std::clamp(-SightHeight, -0.20f, 0.20f) * std::clamp(Strength, 0.f, 1.f) *
			(1.f - std::clamp(Phase, 0.f, 1.f)) * std::clamp(Ready, 0.f, 1.f);
	}

	inline float HipHudFov(float Ratio)
	{
		// Existing ratios were tuned against the RC4 75-degree world lens.
		// Keep that reference so changing the world lens cannot magnify the gun.
		return std::clamp(Ratio * 75.f, 5.f, 150.f);
	}

	inline float Obstruction(float HitDistance, float MuzzleReach, float Clearance)
	{
		return std::clamp((MuzzleReach + Clearance - HitDistance) / std::max(Clearance, 0.01f), 0.f, 1.f);
	}

	inline float Approach(float Current, float Target, float Delta, float Seconds)
	{
		return Current + (Target - Current) * (1.f - std::exp(-std::max(Delta, 0.f) / std::max(Seconds, 0.001f)));
	}

	struct FrameBlend
	{
		float Value = 0.f;
		std::uint32_t Frame = std::uint32_t(-1);
		void Reset() { Value = 0.f; Frame = std::uint32_t(-1); }
		float Update(std::uint32_t CurrentFrame, float Target, float Delta, float Seconds)
		{
			if (Frame != CurrentFrame)
			{
				Frame = CurrentFrame;
				Value = Approach(Value, Target, Delta, Seconds);
			}
			return Value;
		}
	};
}
