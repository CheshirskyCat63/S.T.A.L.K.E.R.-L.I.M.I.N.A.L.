#pragma once

#include <algorithm>
#include <cmath>

// Preset-equivalent absorption, not measured material RT60. Kept in one
// scalar helper so offline native impulse calibration uses the runtime model.
inline float Resonance_ComputeRoomDecay(float Volume, float SurfaceArea, float PresetSize,
	float PresetDecay, float OpeningArea, bool HasOpenGeometry)
{
	if (!HasOpenGeometry)
	{
		return std::clamp(PresetDecay * 6.0f * Volume / (SurfaceArea * PresetSize), 0.1f, 20.0f);
	}
	const float OpenArea = std::clamp(OpeningArea, 0.0f, SurfaceArea);
	const float Absorption = std::clamp(0.161f * PresetSize / (6.0f * PresetDecay), 0.0f, 1.0f);
	const float LossArea = (SurfaceArea - OpenArea) * Absorption + OpenArea;
	return std::clamp(0.161f * Volume / std::max(LossArea, 0.0001f), 0.05f, 20.0f);
}

// Generic material absorption plus transmitted energy in Resonance's octave bands, not measured room
// response. The offline native calibration uses this exact runtime relation.
inline float Resonance_ComputeMaterialRoomDecay(float Volume, float SurfaceArea,
	float OpeningArea, const float* MeanBoundaryLoss, u32 Band)
{
	// Same air attenuation as bundled platforms/common/room_effects_utils.cc.
	constexpr float AirAbsorption[9] = {0.0006f, 0.0006f, 0.0007f,
		0.0008f, 0.0010f, 0.0015f, 0.0026f, 0.0060f, 0.0207f};
	if (MeanBoundaryLoss == nullptr || Band >= 9 || Volume <= 0.0f || SurfaceArea <= 0.0f)
	{
		return 0.05f;
	}
	const float OpenArea = std::clamp(OpeningArea, 0.0f, SurfaceArea);
	const float Mean = std::clamp(((SurfaceArea - OpenArea) * MeanBoundaryLoss[Band] + OpenArea) /
		SurfaceArea, 0.0f, 0.999999f);
	const float LossArea = -SurfaceArea * std::log1p(-Mean) + 4.0f * AirAbsorption[Band] * Volume;
	return std::clamp(0.161f * Volume / std::max(LossArea, 0.0001f), 0.05f, 20.0f);
}

#ifdef DISABLE_RESONANCE_AUDIO
inline void* Resonance_CreateRoomReturn() { return nullptr; }
inline void Resonance_DestroyRoomReturn(void*) {}
inline void Resonance_PrimeRoomReturn(void*) {}
inline void Resonance_SilenceRoomReturn(void*) {}
inline bool Resonance_ProcessRoomReturn(void*, float**, const float*, float, float, float) { return false; }
inline void Resonance_RegisterEffects() {}
inline void Resonance_Shutdown() {}
inline u32 Resonance_GetActiveHrtfs() { return 0; }
inline u32 Resonance_GetActiveShootingHrtfs() { return 0; }
inline u32 Resonance_GetActiveHighHrtfs() { return 0; }
inline float Resonance_GetWetReference(const void*) { return 0.0f; }
inline void Resonance_PrimeInactiveReverb(void*) {}
inline void Resonance_SetRoomOpenGeometry(void*, float, float, float, bool) {}
inline void Resonance_SetRoomMaterials(void*, const float*, bool) {}
#else
// Bounded spatial transport of an already formed stereo room field. Create,
// prime and destroy run at level initialization/reassignment under mixer lock.
// The processor performs no engine allocation, reverb, rays or distance gain.
void* Resonance_CreateRoomReturn();
void Resonance_DestroyRoomReturn(void* State);
void Resonance_PrimeRoomReturn(void* State);
// Allocation-free one-block FIR drain when aperture output becomes inactive.
void Resonance_SilenceRoomReturn(void* State);
bool Resonance_ProcessRoomReturn(void* State, float** Data, const float* LocalDirections,
	float SpreadDegrees, float BeginFraction, float EndFraction);
void Resonance_RegisterEffects();
void Resonance_Shutdown();
u32 Resonance_GetActiveHrtfs();
u32 Resonance_GetActiveShootingHrtfs();
u32 Resonance_GetActiveHighHrtfs();
// Offline unit-impulse stereo-energy reference, including the effect gain.
// Zero means no matching calibration is present. Never measures in callback.
float Resonance_GetWetReference(const void* State);
// Game thread only, for an unpinned room bus whose previous tail is drained.
void Resonance_PrimeInactiveReverb(void* State);
// Game thread only before Update of a drained logical-room bus. Private state
// avoids changing the native public effect parameter count and shared ABI.
void Resonance_SetRoomOpenGeometry(void* State, float CoverFraction,
	float OpeningArea, float RoofOpeningArea, bool HasOpenGeometry);
void Resonance_SetRoomMaterials(void* State, const float* MeanBoundaryLoss, bool HasMaterials);
#endif
