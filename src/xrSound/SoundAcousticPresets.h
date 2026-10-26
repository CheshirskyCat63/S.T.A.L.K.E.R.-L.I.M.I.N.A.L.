#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

// Generic surface estimates, never measurements of a particular assembly.
// Absorption: bundled Resonance Audio platforms/common/room_effects_utils.cc,
// Apache 2.0, 31.25/62.5/125/250/500/1000/2000/4000/8000 Hz.
// Transmission: Valve Steam Audio Resources/Materials/{Metal,Wood,Concrete,
// Brick,Glass,Plaster}.asset, Apache 2.0, documented ENERGY coefficients:
// https://valvesoftware.github.io/steam-audio/doc/capi/scene.html
// https://github.com/ValveSoftware/steam-audio/tree/master/unity/src/project/SteamAudioUnity/Assets/Plugins/SteamAudio/Resources/Materials
// The current Steam direct effect uses these numbers as EQ gains; this model
// follows the documented energy convention and converts to pressure ONCE.
// Missing thickness, density, mounting and measured scattering are not invented.
namespace SoundAcousticPresets
{
enum class Family
{
	UnknownSolid, Masonry, Wood, Metal, Ground, Glass, Soft, Foliage, NonStructural, Liquid
};

struct Preset
{
	const char* Name;
	Family Class;
	float Absorption[9];
	float TransmissionEnergy[3]; // <800 Hz, 800..8000 Hz, >=8000 Hz; negative means legacy fallback.
};

inline constexpr Preset Brick = {"resonance_brick_bare__steam_brick", Family::Masonry, {0.030f,0.030f,0.030f,0.030f,0.030f,0.040f,0.050f,0.070f,0.140f}, {0.015f,0.015f,0.015f}};
inline constexpr Preset Concrete = {"resonance_dense_concrete_proxy__steam_concrete", Family::Masonry, {0.008f,0.008f,0.010f,0.010f,0.015f,0.020f,0.020f,0.020f,0.040f}, {0.015f,0.002f,0.001f}};
inline constexpr Preset ConcreteCoarse = {"resonance_concrete_block_coarse__steam_concrete", Family::Masonry, {0.360f,0.360f,0.360f,0.440f,0.310f,0.290f,0.390f,0.250f,0.500f}, {0.015f,0.002f,0.001f}};
inline constexpr Preset Plaster = {"resonance_plaster_rough__steam_plaster", Family::Masonry, {0.017f,0.018f,0.020f,0.030f,0.040f,0.050f,0.040f,0.030f,0.060f}, {0.056f,0.056f,0.004f}};
inline constexpr Preset HardSurface = {"resonance_polished_concrete_tile__steam_concrete_proxy", Family::Masonry, {0.008f,0.008f,0.010f,0.010f,0.015f,0.020f,0.020f,0.020f,0.040f}, {0.015f,0.002f,0.001f}};
inline constexpr Preset Wood = {"resonance_wood_panel__steam_wood", Family::Wood, {0.280f,0.280f,0.280f,0.220f,0.170f,0.090f,0.100f,0.110f,0.220f}, {0.070f,0.014f,0.005f}};
inline constexpr Preset Metal = {"resonance_metal__steam_generic_metal", Family::Metal, {0.030f,0.035f,0.040f,0.040f,0.050f,0.050f,0.050f,0.070f,0.090f}, {0.200f,0.025f,0.010f}};
inline constexpr Preset Ground = {"resonance_grass_ground_proxy__legacy_transmission", Family::Ground, {0.050f,0.050f,0.150f,0.250f,0.400f,0.550f,0.600f,0.600f,0.600f}, {-1.0f,-1.0f,-1.0f}};
inline constexpr Preset Glass = {"resonance_glass_thin__steam_glass", Family::Glass, {0.180f,0.169f,0.180f,0.060f,0.040f,0.030f,0.020f,0.020f,0.040f}, {0.060f,0.044f,0.011f}};
inline constexpr Preset Soft = {"resonance_heavy_curtain_proxy__legacy_transmission", Family::Soft, {0.073f,0.106f,0.140f,0.350f,0.550f,0.720f,0.700f,0.650f,1.000f}, {-1.0f,-1.0f,-1.0f}};
inline constexpr Preset Unknown = {"resonance_uniform_unknown__legacy_transmission", Family::UnknownSolid, {0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f}, {-1.0f,-1.0f,-1.0f}};

inline const Preset& Select(const char* LowerName)
{
	const auto Has = [LowerName](const char* Token) { return std::strstr(LowerName, Token) != nullptr; };
	if (Has("glass") || Has("window") || Has("vitreous")) return Glass;
	if (Has("wood") || Has("tree_trunk")) return Wood;
	if (Has("metal") || Has("materials\\tin")) return Metal;
	if (Has("cloth") || Has("fabric") || Has("carpet")) return Soft;
	if (Has("earth") || Has("gravel") || Has("sand") || Has("grass") || Has("dirt")) return Ground;
	if (Has("asphalt") || Has("shifer") || Has("tile")) return HardSurface;
	if (Has("brick")) return Brick;
	if (Has("stucco") || Has("plaster")) return Plaster;
	if (Has("concrete")) return Has("block") || Has("coarse") ? ConcreteCoarse : Concrete;
	return Unknown;
}

inline constexpr unsigned TransmissionBand(unsigned Octave)
{
	return Octave < 5 ? 0 : Octave < 8 ? 1 : 2;
}

inline float TransmissionEnergy(const Preset& Material, unsigned Band, float LegacyPressure)
{
	const float Value = Material.TransmissionEnergy[Band];
	return Value >= 0.0f ? Value : LegacyPressure * LegacyPressure;
}

inline float OctaveTransmissionEnergy(const Preset& Material, unsigned Octave, float LegacyPressure)
{
	constexpr float Frequencies[9] = {31.25f,62.5f,125.0f,250.0f,500.0f,1000.0f,2000.0f,4000.0f,8000.0f};
	const float Frequency = Frequencies[Octave];
	const unsigned LowerBand = Frequency < 2500.0f ? 0u : 1u;
	const float LowFrequency = LowerBand == 0 ? 400.0f : 2500.0f;
	const float HighFrequency = LowerBand == 0 ? 2500.0f : 15000.0f;
	const float Weight = std::clamp(std::log(Frequency / LowFrequency) / std::log(HighFrequency / LowFrequency), 0.0f, 1.0f);
	const float Low = TransmissionEnergy(Material, LowerBand, LegacyPressure);
	const float High = TransmissionEnergy(Material, LowerBand + 1, LegacyPressure);
	return Low + Weight * (High - Low);
}

inline float BoundaryLoss(const Preset& Material, unsigned Octave, float LegacyPressure)
{
	return std::clamp(Material.Absorption[Octave] + OctaveTransmissionEnergy(Material, Octave, LegacyPressure), 0.0f, 1.0f);
}
}
