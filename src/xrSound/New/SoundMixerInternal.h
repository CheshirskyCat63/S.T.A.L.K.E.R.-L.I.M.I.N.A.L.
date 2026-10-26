/**************************************************************************************
* Copyright (C) 2025 Anton Kovalev (vertver)
* New Sound Engine
***************************************************************************************
* Source code is licensed under the following terms:
*
* 1. IX-Ray Team License
*    Non-exclusive, royalty-free, perpetual license is hereby granted to:
*      - ForserX   (https://github.com/ForserX)
*      - Drombeys  (https://github.com/Drombeys)
*      - v2v3v4    (https://github.com/v2v3v4)
*
*    Permitted rights:
*      - Copy, modify, merge, publish and distribute this Software
*        and its documentation.
*
* 2. Public Access License
*    Non-exclusive, "access-view-study" rights granted to everyone else.
*
*    Permitted rights:
*      - Private copying is allowed, provided that no distribution occurs.
*      - Public cloning (i.e. "forking") is allowed, but any source code
*        modification or binary redistribution is prohibited.
*
* Usage of this Software beyond the rights granted above is strictly prohibited.
*
* The above copyright notice and this license text must be included in all
* copies or substantial portions of the Software.
**************************************************************************************/
#pragma once
#include "SoundMeta.h"
#include "SoundMaterialFilter.h"

#define SND_RESAMPLING_QUALITY 3

IC void	volume_lerp(float& c, float t, float s, float dt)
{
    float diff = t - c;
    float diff_a = std::abs(diff);
    if (diff_a < EPS_S) return;
    float mot = s * dt;
    if (mot > diff_a) mot = diff_a;
    c += (diff / diff_a) * mot;
}

// Diagnostic evidence from the existing central transmission ray, never a new query.
struct AcousticPathObservation
{
	u64 TimestampNS = 0;
	Fvector Listener = {};
	Fvector Source = {};
	bool Queried = false;
	bool Valid = false;
	bool Deferred = false;
	bool FromCache = false;
	bool CenterRayQueried = false;
	bool MaterialHit = false;
	bool BoundaryEncountered = false;
	bool MaterialFromCallback = false;
	bool DynamicQueried = false;
	bool GlassEncountered = false;
	bool MaterialFallbackUsed = false;
	u32 MaterialIndex = u32(-1);
	u32 MaterialFlags = u32(-1);
	float MaterialTransmission = -1.0f;
	float MaterialDistance = -1.0f;
	float CenterTransmission = -1.0f;
	float LowTransmissionRatio = 1.0f;
	float MidTransmissionRatio = 1.0f;
	float HighTransmissionRatio = 1.0f;
	float SelectedLowTransmissionRatio = 1.0f;
	float SelectedMidTransmissionRatio = 1.0f;
	float SelectedHighTransmissionRatio = 1.0f;
	u32 MaterialCrossings = 0;
	u32 FoliageCrossings = 0;
	float DirectGain = -1.0f;
	float DirectLowTransmissionRatio = 1.0f;
	float DirectMidTransmissionRatio = 1.0f;
	float DirectHighTransmissionRatio = 1.0f;
	float PortalGain = -1.0f;
	float DynamicGain = -1.0f;
	float SelectedGain = -1.0f;
	u32 ChosenPath = 0; // 0 unavailable, 1 direct, 2 portal, 3 direct with dynamic obstruction.
	u32 PortalKind = 0;
	u32 PortalId = 0;
	bool PortalGlass = false;
	bool PortalMaterialsKnown = false;
	float PortalLegAGain = -1.0f;
	float PortalLegBGain = -1.0f;
	bool PortalVerified = false;
	Fvector PortalPoint = {};
	Fvector PortalNormal = {};
	Fvector PortalTangentU = {};
	float PortalArea = 0.0f;
	float PortalHalfExtentU = 0.0f;
	float PortalHalfExtentV = 0.0f;
	float PortalPathLength = 0.0f;
	float PortalApertureGain = 0.0f;
	u32 PortalCandidatesRejected = 0;
};

struct sound_slot_state
{
    XRay::Sound::Mixer::State prev_state;
    XRay::Sound::Mixer::State state;
    XRay::Sound::Mixer::State fake_state;
    u8 flags;
    u32 bus;
    u32 shot_group = 0;
    u32 shot_layer = 0;
	u64 AcousticEvent = 0;
	float EarlySendGain = -1.0f;
    // A single geometry transmission estimate shared by all 3D tracks
    // of the same STCoP shot. Reset on each Play, update on the game thread.
    float BarrierBlockage = 0.0f;
    float BarrierGain = 1.0f;
    float BarrierMismatch = 0.0f;
    float BarrierPortal = 0.0f;
    float BarrierWindow = 0.0f;
    float BarrierDynamic = 0.0f;
    float BarrierLowRatio = 1.0f;
    float BarrierMidRatio = 1.0f;
    float BarrierHighRatio = 1.0f;
    SoundMaterialFilter<SND_CHANNEL_COUNT> MaterialFilter;
    float BarrierFoliageLowPass[SND_CHANNEL_COUNT] = {};
    bool BarrierInitialized = false;
	AcousticPathObservation PathDiagnostic;
	bool PhysicalDiagnosticPending = false;
    float BarrierLowPass[SND_CHANNEL_COUNT] = {};
    u32 zone_idx;
    u32 position;
    u32 stopping_position;
    f32 history[SND_CHANNEL_COUNT][SND_RESAMPLING_QUALITY + 1];
    f32 delay = 0.f;
    xr_string sound_name;
    Fvector parameters[(u32)XRay::Sound::Mixer::ParameterId::Count];
    Fvector prev_position = {};
    Fvector velocity = {};
    f32 doppler = 1.0f;
    f32 fade_volume = 1.0f;

	f32 ReverbDryGain = -1.0f;
	f32 IndoorFactor = 0.0f;
    bool IndoorFactorValid = false;
	bool AcousticStartPending = false;
	// Update publishes this under render_lock; precache never writes it.
	bool AcousticReady = false;
    u64 IndoorNextProbeNS = 0;
    u64 IndoorLastProbeNS = 0;
    Fvector IndoorProbePos = {};
	u32 SendBus = 0;
	f32 SendGain = 0.0f;
	u32 PreviousRoomBus = 0;
	// Normalized occupancy weights, independent of source gain and dry/wet ratio.
	// Keep all existing bus weights on a reversal; no transition restarts.
	f32 RoomWeights[SND_BUS_COUNT] = {};
	f32 RoomBeginWeights[SND_BUS_COUNT] = {};
	// Local pre-barrier and incoming post-barrier PCM can feed the same bus
	// at a doorway. Preserve both endpoint weights instead of switching PCM.
	f32 RoomLocalWeights[SND_BUS_COUNT] = {};
	f32 RoomLocalBeginWeights[SND_BUS_COUNT] = {};
	f32 RoomIncomingWeights[SND_BUS_COUNT] = {};
	f32 RoomIncomingBeginWeights[SND_BUS_COUNT] = {};
	bool RoomWeightsValid = false;
	f32 FarSendGain = -1.0f;
	f32 RoomSendScale = -1.0f;
	u8 VoiceEffects[SND_VOICE_EFFECT_COUNT] = {};
	alignas(16) u8 VoiceEffectState[SND_VOICE_EFFECT_COUNT][SND_VOICE_EFFECT_STATE_SIZE] = {};
};

namespace XRay::Sound::Mixer
{
    XRSOUND_API SoundBus* GetBuses();
    XRSOUND_API const SoundEffectEntry* GetEffect(u8 id);
    XRSOUND_API u32 GetEffectCount();
    XRSOUND_API u32 CreateBus(const char* name);
    XRSOUND_API void DeleteBus(u32 bus);
    XRSOUND_API void SetBusValue(u32 bus, const char* key, const char* value);
    XRSOUND_API void SetBusEffectParam(u32 bus, bool voice, u32 effect, u32 param, float value);
    XRSOUND_API const char* GetBusValue(u32 bus, const char* key);
    XRSOUND_API bool SaveBus(u32 bus);
    XRSOUND_API void AddEditorZone(sound_zone_params& params);
    XRSOUND_API void AddZone(sound_zone_params& params);
    XRSOUND_API void ResetZones();
    const XRSOUND_API xr_vector<sound_zone_params>& GetZones();
    XRSOUND_API xr_vector<sound_slot_state>& GetSlots();
    XRSOUND_API xrSRWLock& GetUpdateMutex();
    XRSOUND_API xrSRWLock& GetManageMutex();
}
