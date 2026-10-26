/**************************************************************************************
 * Copyright (C) 2026 Anton Kovalev (vertver)
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

void Snd_InitBuses();
void Snd_ShutdownBuses();

u32 Snd_ReserveBus(const char* Name);
u32 Snd_FindBus(const char* Name);
u32 Snd_GetMasterBus();
SoundBus* Snd_GetBus(u32 BusIdx);
SoundBus* Snd_GetBuses();
bool Snd_HasReverbBus(u32 BusIdx);
float Snd_GetBusWetReference(u32 BusIdx);

struct SoundBusMeter
{
	float InputPeak = 0.0f;
	float OutputPeak = 0.0f;
	u64 ClippedSamples = 0;
	double InputEnergy = 0.0;
	double OutputEnergy = 0.0;
	u64 Samples = 0;
	u32 EffectCount = 0;
	// A single event remains attributable through its drained tail. Unknown
	// or overlapping contributors explicitly invalidate voice attribution.
	u64 SourceEvent = 0;
	u32 SourceKind = 0;
	bool SourceMixed = false;
	bool SourceObserved = false;
	double ReturnInputEnergy = 0.0;
	double ReturnOutputEnergy = 0.0;
	float ReturnInputPeak = 0.0f;
	float ReturnOutputPeak = 0.0f;
	u64 ReturnSamples = 0;
	u32 SpatialBlocks = 0;
	float EffectPeak[SND_BUS_EFFECT_COUNT] = {};
	double EffectEnergy[SND_BUS_EFFECT_COUNT] = {};
};

SoundBusMeter Snd_TakeBusMeter(u32 BusIdx);
void Snd_SetBusParam(u32 BusIdx, const char* Effect, const char* Param, float Value);

u32 Snd_CreateBus(const char* Name);
void Snd_DeleteBus(u32 BusIdx);
void Snd_SetBusValue(u32 BusIdx, const char* Key, const char* Value);
void Snd_SetBusEffectParam(u32 BusIdx, bool IsVoice, u32 EffectIdx, u32 ParamIdx, float Value);
const char* Snd_GetBusValue(u32 BusIdx, const char* Key);
bool Snd_SaveBus(u32 BusIdx);

void Snd_BeginBuses();
bool Snd_SendToBus(u32 BusIdx, float** Data, float BeginFactor, float EndFactor, float Left, float Right,
	u64 AcousticEvent = 0, u32 AcousticKind = 0);
void Snd_RenderBuses(float* Output, float MuteVolume);

void Snd_AddZone(sound_zone_params* Params, bool IsEditor);
void Snd_ResetZones();
u32 Snd_FindZone(const Fvector& Position);
u32 Snd_GetZoneBus(u32 ZoneIdx);
const xr_vector<sound_zone_params>& Snd_GetZones();

// Sidecar metadata is tied to the exact level.snd_env CRC. IDs are CRC32 of
// persistent catalog labels, never the cuboid index or environment preset.
struct SoundRoomInfo
{
	u32 Id = 0;
	u32 ReferenceZone = 0;
	Fvector Min = {};
	Fvector Max = {};
	Fvector Dimensions = {};
	float Volume = 0.0f;
	float SurfaceArea = 0.0f;
	float WetReference = 0.0f;
	float CoverFraction = 0.0f;
	float OpeningArea = 0.0f;
	float RoofOpeningArea = 0.0f;
	bool HasOpenGeometry = false;
	// Version 3: area-weighted generic absorption plus transmitted energy.
	// Unknown sampled area remains explicitly represented by its proxy.
	float MeanBoundaryLoss[9] = {};
	float SampledClosedArea = 0.0f;
	float ClassifiedAreaFraction = 0.0f;
	bool HasMaterialGeometry = false;
};

struct SoundRoomPortal
{
	u32 Id = 0;
	u32 RoomA = 0;
	u32 RoomB = 0;
	Fvector Center = {};
	Fvector Normal = {};
	float Area = 0.0f;
	// Version 2 support rectangle. Normal points from RoomA to RoomB;
	// TangentV = Normal cross TangentU. This is candidate geometry only.
	Fvector TangentU = {};
	float HalfExtentU = 0.0f;
	float HalfExtentV = 0.0f;
	u32 Flags = 0;
};

inline constexpr u32 SoundRoomPortalOpen = 1u;
inline constexpr u32 SoundRoomPortalRoof = 2u;

// Mutations run on the game thread with the mixer render lock held. The fixed
// pool is created at level load; audio rendering never acquires room resources.
bool Snd_LoadRoomMetadata(IReader* Reader, u32 EnvCrc32);
bool Snd_HasRoomMetadata();
bool Snd_IsLogicalRoomBus(u32 BusIdx);
u32 Snd_GetZoneRoom(u32 ZoneIdx);
const SoundRoomInfo* Snd_GetRoomInfo(u32 RoomId);
u32 Snd_GetRoomBus(u32 RoomId);
// Assigned rooms are returned even before their first input block. Active
// includes decaying tails with no source voice. The game update validates the
// path and publishes targets while holding the mixer render lock; no rays or
// resource allocation occur in bus rendering. Same room uses (1, 20000, 0, true).
// Directions are listener-local vectors to the two currently validated
// aperture samples at Center +/- 0.5 * HalfExtentU * TangentU. Spatialization
// transports the existing field only; local/wall pressure stays diffuse.
struct SoundRoomReturnSpatial
{
	u32 PortalId = 0;
	Fvector LocalDirections[2] = {};
	float SpreadDegrees = 0.0f;
	float Fraction = 0.0f;
};

struct SoundRoomReturnState
{
	u32 RoomId = 0;
	u32 Bus = 0;
	Fvector Anchor = {};
	Fvector CatalogAnchor = {};
	bool HasTransportOrigin = false;
	bool Active = false;
	bool Validated = false;
	u64 SourceEvent = 0;
	u32 SourceKind = 0;
	bool SourceMixed = false;
	u32 PortalId = 0;
	float SpatialFraction = 0.0f;
	float TargetSpatialFraction = 0.0f;
	bool SpatialReady = false;
	float Gain = 0.0f;
	float TargetGain = 0.0f;
	float Cutoff = 20000.0f;
	float FilterBlend = 0.0f;
	float BandRatios[3] = {1.0f, 1.0f, 1.0f};
	float TargetBandRatios[3] = {1.0f, 1.0f, 1.0f};
};

// Persist the selected active source's validated membership point through
// its tail. This changes only transport origin, never logical room ownership.
bool Snd_SetRoomReturnOrigin(u32 BusIdx, u32 RoomId, const Fvector& Position);
u32 Snd_GetRoomReturnStates(SoundRoomReturnState* Output, u32 Capacity);
bool Snd_SetRoomReturnPath(u32 BusIdx, u32 RoomId, float Gain, float Cutoff,
	float FilterBlend, bool Validated, const float* BandRatios = nullptr,
	const SoundRoomReturnSpatial* Spatial = nullptr);
void Snd_BeginRoomUpdate();
void Snd_PrepareRoomBuses(const u32* RoomIds, u32 Count, const bool* PinnedBuses = nullptr);
// Only candidate geometry: callers must validate both static/dynamic path legs.
// Room 0 means outdoors, UINT32_MAX is an unclassified graph endpoint.
u32 Snd_FindRoomPortals(u32 SourceRoom, u32 ListenerRoom, const Fvector& Position,
	SoundRoomPortal* Output, u32 Capacity);

// No allocation or ray queries. Only incident, resolved, structurally open v2
// rectangles near Position are returned; current static/dynamic validation is
// mandatory before using a transition. The zone index remains exact.
u32 Snd_FindRoomTransitionPortals(u32 RoomId, const Fvector& Position,
	SoundRoomPortal* Output, u32 Capacity);
float Snd_RoomPortalTransitionDepth(const SoundRoomPortal& Portal);
const SoundRoomPortal* Snd_GetRoomPortal(u32 PortalId);
