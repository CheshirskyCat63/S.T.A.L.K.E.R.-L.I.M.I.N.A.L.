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
#include "SoundBus.h"
#include "SoundConfig.h"
#include "SoundEffect.h"
#include "SoundDSP.h"
#include "SoundMaterialFilter.h"
#include "SoundAcousticCapture.h"
#include "SoundRoomChecksum.h"
#include "../Plugins/ResonanceAudio.h"
#include <cmath>
#include <cfloat>
#include <chrono>

#define SND_ZONE_RAY_RANGE (1000.0f)

static constexpr u32 SoundRoomPoolSize = 12;
// File sanity limits, independent of active ray, path and reverb budgets.
static constexpr u32 SoundRoomMetadataPortalLimit = 131072;
static constexpr u64 SoundRoomMetadataByteLimit = 32ull * 1024ull * 1024ull;

struct SoundRoomSlot
{
	u32 RoomId = 0;
	u32 Bus = 0;
	Fvector Anchor = {};
	Fvector TransportOrigin = {};
	bool HasTransportOrigin = false;
	bool ReturnValidated = false;
	void* ReturnSpatializer = nullptr;
	bool ReturnSpatialDirty = false;
	SoundRoomReturnSpatial ReturnSpatial = {};
	float ReturnSpatialFraction = 0.0f;
	float ReturnTargetSpatialFraction = 0.0f;
	u64 SourceEvent = 0;
	u32 SourceKind = 0;
	bool SourceMixed = false;
	bool SourceTagged = false;
	float ReturnGain = 0.0f;
	float ReturnTargetGain = 0.0f;
	float ReturnCutoff = 20000.0f;
	float ReturnTargetCutoff = 20000.0f;
	float ReturnFilterBlend = 0.0f;
	float ReturnTargetFilterBlend = 0.0f;
	float ReturnLowPass[SND_CHANNEL_COUNT] = {};
	float ReturnBandRatios[3] = {1.0f, 1.0f, 1.0f};
	float ReturnTargetBandRatios[3] = {1.0f, 1.0f, 1.0f};
	SoundMaterialFilter<SND_CHANNEL_COUNT> ReturnMaterialFilter;
};

struct SoundBusState
{
	SoundBus Buses[SND_BUS_COUNT];
	u32 Order[SND_BUS_COUNT] = {};
	u32 OrderCount = 0;
	u32 Master = 0;
	xr_vector<sound_zone_params> Zones;
	xr_hash_map<shared_str, shared_str> ZoneBuses;
	xr_hash_map<u64, xr_vector<u32>> ZoneCells;
	xr_vector<u32> LargeZoneIndices;
	bool IsEditorZone = false;
	SoundBusMeter Meters[SND_BUS_COUNT];
	xr_vector<u32> ZoneRooms;
	xr_vector<SoundRoomInfo> Rooms;
	xr_hash_map<u32, u32> RoomIndices;
	xr_vector<SoundRoomPortal> Portals;
	xr_hash_map<u32, xr_vector<u32>> RoomPortalIndices;
	xr_vector<u32> UnresolvedPortalIndices;
	xr_hash_map<u32, u32> PortalIndicesById;
	xr_hash_map<u64, xr_vector<u32>> PortalCells;
	xr_vector<u32> LargePortalIndices;
	SoundRoomSlot RoomSlots[SoundRoomPoolSize];
	u32 RoomAssignmentBudget = 0;
	u32 RoomPrimePeakUs = 0;
};

static SoundBusState GBuses = {};

u32 Snd_ReserveBus(const char* Name)
{
	shared_str BusName = Name;
	u32 FreeIdx = 0;
	for (u32 BusIdx = 0; BusIdx < SND_BUS_COUNT; BusIdx++)
	{
		if (GBuses.Buses[BusIdx].Name == BusName)
		{
			return BusIdx + 1;
		}

		if (FreeIdx == 0 && GBuses.Buses[BusIdx].Name.size() == 0)
		{
			FreeIdx = BusIdx + 1;
		}
	}

	if (FreeIdx == 0)
	{
		Msg("! [Sound] Too many buses, '%s' skipped", Name);
		return 0;
	}

	GBuses.Buses[FreeIdx - 1].Name = BusName;
	return FreeIdx;
}

u32 Snd_FindBus(const char* Name)
{
	shared_str BusName = Name;
	for (u32 BusIdx = 0; BusIdx < SND_BUS_COUNT; BusIdx++)
	{
		if (GBuses.Buses[BusIdx].IsUsed && GBuses.Buses[BusIdx].Name == BusName)
		{
			return BusIdx + 1;
		}
	}

	return 0;
}

u32 Snd_GetMasterBus()
{
	if (GBuses.Master == 0)
	{
		GBuses.Master = Snd_ReserveBus("master");
	}

	return GBuses.Master;
}

SoundBus* Snd_GetBus(u32 BusIdx)
{
	return (BusIdx == 0 || BusIdx > SND_BUS_COUNT || !GBuses.Buses[BusIdx - 1].IsUsed) ? nullptr : &GBuses.Buses[BusIdx - 1];
}

SoundBus* Snd_GetBuses()
{
	return GBuses.Buses;
}

bool Snd_HasReverbBus(u32 BusIdx)
{
	const SoundBus* Bus = Snd_GetBus(BusIdx);
	for (u32 Idx = 0; Bus != nullptr && Idx < Bus->EffectCount; ++Idx)
	{
		const SoundBusEffect& Effect = Bus->Effects[Idx];
		const SoundEffectEntry* Entry = Snd_GetEffect(Effect.Effect);
		if (Effect.State != nullptr && Entry != nullptr &&
			(xr_strcmp(Entry->Name.c_str(), "reverb") == 0 ||
			 xr_strcmp(Entry->Name.c_str(), "convolution") == 0))
		{
			return true;
		}
	}
	return false;
}

SoundBusMeter Snd_TakeBusMeter(u32 BusIdx)
{
	if (BusIdx == 0 || BusIdx > SND_BUS_COUNT)
	{
		return {};
	}
	const SoundBusMeter Meter = GBuses.Meters[BusIdx - 1];
	GBuses.Meters[BusIdx - 1] = {};
	return Meter;
}

static float Snd_EffectParameter(const SoundBusEffect& Effect, const SoundEffectEntry& Entry,
	const char* Name, float Fallback)
{
	for (u32 Idx = 0; Idx < Entry.Desc.ParamCount; ++Idx)
	{
		if (xr_strcmp(Entry.Desc.Params[Idx].Name, Name) == 0)
		{
			return Effect.Params[Idx];
		}
	}
	return Fallback;
}

float Snd_GetBusWetReference(u32 BusIdx)
{
	const SoundBus* Bus = Snd_GetBus(BusIdx);
	for (u32 Idx = 0; Bus != nullptr && Idx < Bus->EffectCount; ++Idx)
	{
		const SoundBusEffect& Effect = Bus->Effects[Idx];
		const SoundEffectEntry* Entry = Snd_GetEffect(Effect.Effect);
		if (Effect.State == nullptr || Entry == nullptr)
		{
			continue;
		}
		const float Fader = Bus->Volume * Bus->UserVolume;
		if (xr_strcmp(Entry->Name.c_str(), "convolution") == 0)
		{
			// Native convolution IRs are normalized to unit energy per channel.
			return Fader * Snd_EffectParameter(Effect, *Entry, "mix", 1.0f);
		}
		if (xr_strcmp(Entry->Name.c_str(), "reverb") == 0)
		{
			const float Measured = Snd_EffectParameter(Effect, *Entry, "wet_reference", 0.0f);
			if (Measured > 0.0f)
			{
				return Fader * Measured;
			}
			const float Size = Snd_EffectParameter(Effect, *Entry, "size", 10.0f);
			const float Decay = Snd_EffectParameter(Effect, *Entry, "decay", 1.49f);
			const float Coefficient = std::exp(-0.161f * Size / (12.0f * Decay));
			const float Early = Snd_EffectParameter(Effect, *Entry, "geometry_early", 0.0f) >= 0.5f ? 0.0f :
				Snd_EffectParameter(Effect, *Entry, "reflections", 0.05f) *
				Coefficient * 3.0f / (0.5f * Size + 1.0f);
			const float Late = Snd_EffectParameter(Effect, *Entry, "late_gain", 1.0f);
			const float Gain = Snd_EffectParameter(Effect, *Entry, "gain", 1.0f);
			// Nominal first-order stereo room response plus independently normalized
			// late field. This is a config reference, not a measured per-shot return.
			return Fader * Gain * std::sqrt(Early * Early + Late * Late);
		}
	}
	return 0.0f;
}

static void Snd_ClearBus(SoundBus* Bus)
{
	for (u32 EffectIdx = 0; EffectIdx < Bus->EffectCount; EffectIdx++)
	{
		Snd_DestroyEffect(&Bus->Effects[EffectIdx]);
	}

	shared_str Name = Bus->Name;
	float UserVolume = Bus->UserVolume;
	*Bus = SoundBus();
	Bus->Name = Name;
	Bus->UserVolume = UserVolume;
}

static u32 Snd_ParseEffects(SoundBusEffect* Effects, u32 Capacity, SoundEffectScope Scope, const char* List)
{
	u32 Count = 0;
	u32 ItemCount = (u32)_GetItemCount(List);
	for (u32 ItemIdx = 0; ItemIdx < ItemCount && Count < Capacity; ItemIdx++)
	{
		string128 Item;
		_GetItem(List, ItemIdx, Item);

		u8 Id = 0;
		u32 AlternativeCount = (u32)_GetItemCount(Item, '|');
		for (u32 AlternativeIdx = 0; AlternativeIdx < AlternativeCount && Id == 0; AlternativeIdx++)
		{
			string128 Alternative;
			Id = Snd_FindEffect(_GetItem(Item, AlternativeIdx, Alternative, '|'));
			Id = (Id != 0 && Snd_GetEffect(Id)->Desc.Scope == Scope) ? Id : 0;
		}

		if (Id == 0)
		{
			Msg("! [Sound] Unknown effect '%s'", Item);
			continue;
		}

		Snd_ResetEffect(&Effects[Count++], Id);
	}

	return Count;
}

static void Snd_SetEffectsParam(SoundBusEffect* Effects, u32 Count, const char* Effect, const char* Param, const char* Value)
{
	for (u32 EffectIdx = 0; EffectIdx < Count; EffectIdx++)
	{
		SoundBusEffect* Target = &Effects[EffectIdx];
		const SoundEffectEntry* Entry = Snd_GetEffect(Target->Effect);
		if (Entry == nullptr || xr_strcmp(Entry->Name.c_str(), Effect) != 0)
		{
			continue;
		}

		if (xr_strcmp(Param, "resource") == 0)
		{
			Target->Resource = Value;
		}
		else if (!Snd_SetEffectParam(Target, Param, (float)atof(Value)))
		{
			Msg("! [Sound] Unknown parameter '%s.%s'", Effect, Param);
		}
	}
}

static void Snd_CreateBusEffects(SoundBus* Bus)
{
	for (u32 EffectIdx = 0; EffectIdx < Bus->EffectCount; EffectIdx++)
	{
		Snd_CreateEffect(&Bus->Effects[EffectIdx]);
	}
}

static void Snd_ApplyBusConfig(u32 BusIdx, const SoundConfigSection* Section)
{
	SoundBus* Bus = &GBuses.Buses[BusIdx - 1];
	Snd_ClearBus(Bus);
	Bus->IsUsed = true;
	Bus->Output = BusIdx == GBuses.Master ? 0 : GBuses.Master;

	for (const CInifile::Item& Item : Section->Items)
	{
		const char* Key = Item.first.c_str();
		const char* Value = Item.second.size() ? Item.second.c_str() : "";
		if (xr_strcmp(Key, "output") == 0)
		{
			Bus->Output = Value[0] != 0 ? Snd_ReserveBus(Value) : Bus->Output;
		} else if (xr_strcmp(Key, "volume") == 0)
		{
			Bus->Volume = (float)atof(Value);
		} else if (xr_strcmp(Key, "zone_send") == 0)
		{
			Bus->ZoneSend = (float)atof(Value);
		} else if (xr_strcmp(Key, "direct_ratio") == 0)
		{
			Bus->DirectRatio = (float)atof(Value);
		} else if (xr_strcmp(Key, "far_send") == 0)
		{
			Bus->FarSend = Value[0] != 0 ? Snd_ReserveBus(Value) : 0;
		} else if (xr_strcmp(Key, "indoor_send") == 0)
		{
			Bus->IndoorSend = Value[0] != 0 ? Snd_ReserveBus(Value) : 0;
		} else if (xr_strcmp(Key, "effects") == 0)
		{
			Bus->EffectCount = Snd_ParseEffects(Bus->Effects, SND_BUS_EFFECT_COUNT, SoundEffectScope::Bus, Value);
		} else if (xr_strcmp(Key, "voice_effects") == 0)
		{
			Bus->VoiceEffectCount = Snd_ParseEffects(Bus->VoiceEffects, SND_VOICE_EFFECT_COUNT, SoundEffectScope::Voice, Value);
		}
	}

	for (const CInifile::Item& Item : Section->Items)
	{
		const char* Dot = strchr(Item.first.c_str(), '.');
		if (Dot == nullptr || Item.second.size() == 0)
		{
			continue;
		}

		string128 Effect;
		xr_strcpy(Effect, Item.first.c_str());
		Effect[Dot - Item.first.c_str()] = 0;
		Snd_SetEffectsParam(Bus->Effects, Bus->EffectCount, Effect, Dot + 1, Item.second.c_str());
		Snd_SetEffectsParam(Bus->VoiceEffects, Bus->VoiceEffectCount, Effect, Dot + 1, Item.second.c_str());
	}

	Snd_CreateBusEffects(Bus);
}

static void Snd_RebuildOrder()
{
	u32 Master = GBuses.Master;
	for (u32 BusIdx = 1; BusIdx <= SND_BUS_COUNT; BusIdx++)
	{
		SoundBus* Bus = Snd_GetBus(BusIdx);
		if (Bus != nullptr && BusIdx != Master && Snd_GetBus(Bus->Output) == nullptr)
		{
			Bus->Output = Master;
		}
	}

	GBuses.OrderCount = 0;
	for (u32 BusIdx = 1; BusIdx <= SND_BUS_COUNT; BusIdx++)
	{
		SoundBus* Bus = Snd_GetBus(BusIdx);
		if (Bus == nullptr)
		{
			continue;
		}

		Bus->Depth = 0;
		for (u32 Cursor = BusIdx; Cursor != Master; Cursor = Snd_GetBus(Cursor)->Output)
		{
			if (++Bus->Depth > SND_BUS_COUNT)
			{
				Msg("! [Sound] Bus '%s' output loop, routed to master", Bus->Name.c_str());
				Bus->Output = Master;
				Bus->Depth = 1;
				break;
			}
		}

		u32 InsertIdx = GBuses.OrderCount++;
		while (InsertIdx > 0 && GBuses.Buses[GBuses.Order[InsertIdx - 1] - 1].Depth < Bus->Depth)
		{
			GBuses.Order[InsertIdx] = GBuses.Order[InsertIdx - 1];
			InsertIdx--;
		}

		GBuses.Order[InsertIdx] = BusIdx;
	}
}

static void Snd_AssignZoneBus(sound_zone_params* Zone)
{
	string256 Name;
	xr_strcpy(Name, Zone->name.c_str());
	xr_strlwr(Name);

	auto Found = GBuses.ZoneBuses.find(shared_str(Name));
	if (Found != GBuses.ZoneBuses.end())
	{
		Zone->bus = Snd_FindBus(Found->second.c_str());
		return;
	}

	u8 Reverb = Snd_FindEffect("reverb");
	u8 Compressor = Snd_FindEffect("compressor");
	xr_strconcat(Name, "env:", Zone->name.c_str());
	Zone->bus = Snd_FindBus(Name);
	if (Zone->bus != 0 || Reverb == 0)
	{
		return;
	}

	u32 BusIdx = Snd_ReserveBus(Name);
	if (BusIdx == 0)
	{
		return;
	}

	SoundBus* Bus = &GBuses.Buses[BusIdx - 1];
	Snd_ClearBus(Bus);
	Bus->IsUsed = true;
	Bus->IsGenerated = true;
	Bus->Output = GBuses.Master;
	// The input compressor suppressed the impulse before reflections existed.
	// Keep output headroom control and the unchanged master compressor.
	Bus->EffectCount = 2;
	Snd_ResetEffect(&Bus->Effects[0], Reverb);
	Snd_ResetEffect(&Bus->Effects[1], Compressor);

	const sound_reverb_settings& Settings = Zone->settings;
	SoundBusEffect* Effect = &Bus->Effects[0];
	Snd_SetEffectParam(Effect, "gain", Settings.room);
	Snd_SetEffectParam(Effect, "size", Settings.environment_size);
	Snd_SetEffectParam(Effect, "cutoff", 5000.0f * Settings.room_hf);
	Snd_SetEffectParam(Effect, "diffusion", Settings.environment_diffusion);
	Snd_SetEffectParam(Effect, "reflections", Settings.reflections);
	Snd_SetEffectParam(Effect, "late_gain", Settings.reverb);
	Snd_SetEffectParam(Effect, "decay", Settings.decay_time);
	Snd_SetEffectParam(Effect, "decay_hf", Settings.decay_hf_ratio);
	Snd_SetEffectParam(Effect, "air_hf", Settings.air_absorption_hf);

	Snd_CreateBusEffects(Bus);
	Zone->bus = BusIdx;
}

static void Snd_SetRoomEffectParameters(SoundBusEffect& Effect, const SoundRoomInfo& Room)
{
	const sound_reverb_settings& Settings = GBuses.Zones[Room.ReferenceZone - 1].settings;
	const char* Names[] = {"gain", "size", "cutoff", "diffusion", "reflections", "late_gain",
		"decay", "decay_hf", "air_hf", "dimensions_x", "dimensions_y", "dimensions_z",
		"room_volume", "surface_area", "geometry_early", "wet_reference"};
	const float Values[] = {Settings.room, Settings.environment_size, 5000.0f * Settings.room_hf,
		Settings.environment_diffusion, Settings.reflections, Settings.reverb,
		Settings.decay_time, Settings.decay_hf_ratio, Settings.air_absorption_hf,
		Room.Dimensions.x, Room.Dimensions.y, Room.Dimensions.z, Room.Volume, Room.SurfaceArea,
		1.0f, Room.WetReference};
	const SoundEffectEntry* Entry = Snd_GetEffect(Effect.Effect);
	for (u32 ParamIdx = 0; Entry != nullptr && ParamIdx < Entry->Desc.ParamCount; ++ParamIdx)
	{
		const SoundEffectParam& Param = Entry->Desc.Params[ParamIdx];
		for (u32 ValueIdx = 0; ValueIdx < std::size(Values); ++ValueIdx)
		{
			if (xr_strcmp(Param.Name, Names[ValueIdx]) == 0)
			{
				Effect.Params[ParamIdx] = std::clamp(Values[ValueIdx], Param.Min, Param.Max);
				break;
			}
		}
	}
}

static void Snd_CreateRoomPool()
{
	for (SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		Resonance_DestroyRoomReturn(Slot.ReturnSpatializer);
		Slot = {};
	}
	if (GBuses.Rooms.empty())
	{
		return;
	}
	const u8 Reverb = Snd_FindEffect("reverb");
	const u8 Compressor = Snd_FindEffect("compressor");
	if (Reverb == 0)
	{
		return;
	}
	u32 Created = 0;
	for (u32 SlotIdx = 0; SlotIdx < SoundRoomPoolSize; ++SlotIdx)
	{
		string64 Name;
		xr_sprintf(Name, "env:logical-room-slot:%u", SlotIdx);
		const u32 BusIdx = Snd_ReserveBus(Name);
		if (BusIdx == 0)
		{
			break;
		}
		SoundBus& Bus = GBuses.Buses[BusIdx - 1];
		Snd_ClearBus(&Bus);
		Bus.IsUsed = true;
		Bus.IsGenerated = true;
		Bus.Output = GBuses.Master;
		Bus.EffectCount = Compressor != 0 ? 2 : 1;
		Snd_ResetEffect(&Bus.Effects[0], Reverb);
		Snd_SetRoomEffectParameters(Bus.Effects[0], GBuses.Rooms.front());
		if (Compressor != 0)
		{
			Snd_ResetEffect(&Bus.Effects[1], Compressor);
		}
		Snd_CreateBusEffects(&Bus);
		if (!Snd_HasReverbBus(BusIdx))
		{
			Snd_ClearBus(&Bus);
			Bus.Name = nullptr;
			continue;
		}
		GBuses.RoomSlots[SlotIdx].Bus = BusIdx;
		GBuses.RoomSlots[SlotIdx].ReturnSpatializer = Resonance_CreateRoomReturn();
		++Created;
	}
	Msg("* [LIMINAL_ROOMS] metadata=%zu zones=%zu portal_candidates=%zu prepared_states=%u max=%u; preset-equivalent boundary loss",
		GBuses.Rooms.size(), GBuses.ZoneRooms.size(), GBuses.Portals.size(), Created, SoundRoomPoolSize);
}

void Snd_InitBuses()
{
	u32 Master = Snd_GetMasterBus();

	GBuses.ZoneBuses.clear();
	for (auto& [Name, Section] : Snd_GetConfig())
	{
		const char* SectionName = Name.c_str();
		if (strstr(SectionName, "bus:") == SectionName)
		{
			u32 BusIdx = Snd_ReserveBus(SectionName + 4);
			if (BusIdx != 0)
			{
				Snd_ApplyBusConfig(BusIdx, &Section);
			}
		}
		else if (strstr(SectionName, "zone:") == SectionName && Snd_ConfigValue(&Section, "bus") != nullptr)
		{
			GBuses.ZoneBuses[shared_str(SectionName + 5)] = Snd_ConfigValue(&Section, "bus");
		}
	}

	if (Snd_GetBus(Master) == nullptr)
	{
		SoundConfigSection Empty;
		Snd_ApplyBusConfig(Master, &Empty);
	}

	if (!GBuses.Rooms.empty())
	{
		Snd_CreateRoomPool();
	}
	else
	{
		for (sound_zone_params& Zone : GBuses.Zones)
		{
			Snd_AssignZoneBus(&Zone);
		}
	}

	Snd_RebuildOrder();
}

void Snd_ShutdownBuses()
{
	for (u32 BusIdx = 0; BusIdx < SND_BUS_COUNT; BusIdx++)
	{
		SoundBus* Bus = &GBuses.Buses[BusIdx];
		Snd_ClearBus(Bus);
	}

	GBuses.OrderCount = 0;
	GBuses.ZoneBuses.clear();
	for (SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		Resonance_DestroyRoomReturn(Slot.ReturnSpatializer);
		Slot = {};
	}
	for (SoundBusMeter& Meter : GBuses.Meters)
	{
		Meter = {};
	}
}

void Snd_SetBusParam(u32 BusIdx, const char* Effect, const char* Param, float Value)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	for (u32 EffectIdx = 0; Bus != nullptr && EffectIdx < Bus->EffectCount; EffectIdx++)
	{
		const SoundEffectEntry* Entry = Snd_GetEffect(Bus->Effects[EffectIdx].Effect);
		if (Entry != nullptr && xr_strcmp(Entry->Name.c_str(), Effect) == 0)
		{
			Snd_SetEffectParam(&Bus->Effects[EffectIdx], Param, Value);
		}
	}
}

static SoundConfigSection* Snd_EditBusConfig(const SoundBus* Bus)
{
	string128 Name;
	xr_strconcat(Name, "bus:", Bus->Name.c_str());
	return Snd_EditConfig(Name);
}

u32 Snd_CreateBus(const char* Name)
{
	u32 BusIdx = Snd_ReserveBus(Name);
	if (BusIdx == 0 || GBuses.Buses[BusIdx - 1].IsUsed)
	{
		return BusIdx;
	}

	SoundConfigSection* Section = Snd_EditBusConfig(&GBuses.Buses[BusIdx - 1]);
	Section->IsDeleted = false;
	Snd_ApplyBusConfig(BusIdx, Section);
	Snd_RebuildOrder();
	return BusIdx;
}

void Snd_DeleteBus(u32 BusIdx)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus == nullptr || BusIdx == GBuses.Master || Bus->IsGenerated)
	{
		return;
	}

	SoundConfigSection* Section = Snd_EditBusConfig(Bus);
	Section->Items.clear();
	Section->IsDeleted = true;
	Snd_ClearBus(Bus);
	Snd_RebuildOrder();
}

void Snd_SetBusValue(u32 BusIdx, const char* Key, const char* Value)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus == nullptr || Bus->IsGenerated)
	{
		return;
	}

	SoundConfigSection* Section = Snd_EditBusConfig(Bus);
	Snd_SetConfigValue(Section, Key, Value);

	float* Scalars[] = {&Bus->Volume, &Bus->ZoneSend, &Bus->DirectRatio};
	const char* ScalarKeys[] = {"volume", "zone_send", "direct_ratio"};
	for (u32 ScalarIdx = 0; ScalarIdx < std::size(Scalars); ScalarIdx++)
	{
		if (xr_strcmp(Key, ScalarKeys[ScalarIdx]) == 0)
		{
			*Scalars[ScalarIdx] = (float)atof(Value);
			return;
		}
	}

	Snd_ApplyBusConfig(BusIdx, Section);
	Snd_RebuildOrder();
}

void Snd_SetBusEffectParam(u32 BusIdx, bool IsVoice, u32 EffectIdx, u32 ParamIdx, float Value)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus == nullptr || EffectIdx >= (IsVoice ? Bus->VoiceEffectCount : Bus->EffectCount))
	{
		return;
	}

	SoundBusEffect* Effect = IsVoice ? &Bus->VoiceEffects[EffectIdx] : &Bus->Effects[EffectIdx];
	const SoundEffectEntry* Entry = Snd_GetEffect(Effect->Effect);
	if (Entry == nullptr || ParamIdx >= Entry->Desc.ParamCount)
	{
		return;
	}

	const char* ParamName = Entry->Desc.Params[ParamIdx].Name;
	Snd_SetEffectParam(Effect, ParamName, Value);
	if (Bus->IsGenerated)
	{
		return;
	}

	string128 Key;
	string32 Text;
	xr_strconcat(Key, Entry->Name.c_str(), ".", ParamName);
	xr_sprintf(Text, "%g", Effect->Params[ParamIdx]);
	Snd_SetConfigValue(Snd_EditBusConfig(Bus), Key, Text);
}

const char* Snd_GetBusValue(u32 BusIdx, const char* Key)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus == nullptr)
	{
		return nullptr;
	}

	string128 Name;
	xr_strconcat(Name, "bus:", Bus->Name.c_str());
	const SoundConfigSection* Section = Snd_FindConfig(Name);
	return Section != nullptr ? Snd_ConfigValue(Section, Key) : nullptr;
}

bool Snd_SaveBus(u32 BusIdx)
{
	if (BusIdx == 0 || BusIdx > SND_BUS_COUNT || GBuses.Buses[BusIdx - 1].IsGenerated)
	{
		return false;
	}

	string128 Name;
	xr_strconcat(Name, "bus:", GBuses.Buses[BusIdx - 1].Name.c_str());
	return Snd_SaveConfig(Name);
}

void Snd_BeginBuses()
{
	for (u32 OrderIdx = 0; OrderIdx < GBuses.OrderCount; OrderIdx++)
	{
		SoundBus* Bus = &GBuses.Buses[GBuses.Order[OrderIdx] - 1];
		if (Bus->TailFrames != 0)
		{
			memset(Bus->Data, 0, sizeof(Bus->Data));
		}

		Bus->HasInput = false;
	}
}

static float** Snd_OpenBus(SoundBus* Bus, float** Data)
{
	if (!Bus->HasInput && Bus->TailFrames == 0)
	{
		memset(Bus->Data, 0, sizeof(Bus->Data));
	}

	Bus->HasInput = true;
	Data[0] = Bus->Data[0];
	Data[1] = Bus->Data[1];
	return Data;
}

static SoundRoomSlot* Snd_FindRoomReturnSlot(u32 BusIdx);

bool Snd_SendToBus(u32 BusIdx, float** Data, float BeginFactor, float EndFactor, float Left, float Right,
	u64 AcousticEvent, u32 AcousticKind)
{
	SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus == nullptr)
	{
		return false;
	}

	if (BeginFactor <= 0.0f && EndFactor <= 0.0f)
	{
		return true;
	}

	if (SoundRoomSlot* Room = Snd_FindRoomReturnSlot(BusIdx))
	{
		if (!Bus->HasInput && Bus->TailFrames == 0)
		{
			Room->SourceEvent = 0;
			Room->SourceKind = 0;
			Room->SourceMixed = false;
			Room->SourceTagged = false;
		}
		if (!Room->SourceTagged)
		{
			Room->SourceEvent = AcousticEvent;
			Room->SourceKind = AcousticKind;
			Room->SourceTagged = true;
		}
		Room->SourceMixed |= AcousticEvent == 0 || Room->SourceEvent != AcousticEvent ||
			Room->SourceKind != AcousticKind;
	}
	float* BusData[SND_CHANNEL_COUNT];
	DSP_MixBufferPanning(Snd_OpenBus(Bus, BusData), Data, BeginFactor, EndFactor, Left, Right, SND_BLOCKSIZE);
	return true;
}

static SoundRoomSlot* Snd_FindRoomReturnSlot(u32 BusIdx)
{
	for (SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		if (Slot.Bus == BusIdx && Slot.RoomId != 0)
		{
			return &Slot;
		}
	}
	return nullptr;
}

static bool Snd_ProcessRoomReturn(SoundRoomSlot& Slot, float** Data)
{
	// The room owns this path after its exciting voices have stopped. Smooth
	// in rendered audio time, using the existing room transition rate.
	const float Blend = 1.0f - std::exp(-4.5f * float(SND_BLOCKSIZE) / float(SND_SAMPLERATE));
	for (u32 Band = 0; Band < 3; ++Band)
	{
		Slot.ReturnBandRatios[Band] += (Slot.ReturnTargetBandRatios[Band] - Slot.ReturnBandRatios[Band]) * Blend;
	}
	// The same three-band material filter as the direct path.
	Slot.ReturnMaterialFilter.SetRatios(Slot.ReturnBandRatios[0], Slot.ReturnBandRatios[1], Slot.ReturnBandRatios[2], float(SND_SAMPLERATE));
	const float BeginGain = Slot.ReturnGain;
	const float BeginFilterBlend = Slot.ReturnFilterBlend;
	const float BeginAlpha = 1.0f - std::exp(-2.0f * PI * Slot.ReturnCutoff / float(SND_SAMPLERATE));
	Slot.ReturnGain += (Slot.ReturnTargetGain - Slot.ReturnGain) * Blend;
	Slot.ReturnCutoff += (Slot.ReturnTargetCutoff - Slot.ReturnCutoff) * Blend;
	Slot.ReturnFilterBlend += (Slot.ReturnTargetFilterBlend - Slot.ReturnFilterBlend) * Blend;
	if (Slot.ReturnTargetGain == 0.0f && Slot.ReturnGain < 0.00001f)
	{
		Slot.ReturnGain = 0.0f;
	}
	const float EndAlpha = 1.0f - std::exp(-2.0f * PI * Slot.ReturnCutoff / float(SND_SAMPLERATE));
	for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
	{
		float Filtered = Slot.ReturnLowPass[Channel];
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
		{
			const float T = float(Frame) / float(SND_BLOCKSIZE - 1);
			const float Sample = Data[Channel][Frame];
			Filtered += lerp(BeginAlpha, EndAlpha, T) * (Sample - Filtered);
			const float Transmitted = Sample + (Filtered - Sample) *
				lerp(BeginFilterBlend, Slot.ReturnFilterBlend, T);
			const float Spectral = Slot.ReturnMaterialFilter.ProcessSample(Channel, Transmitted, Frame, SND_BLOCKSIZE);
			Data[Channel][Frame] = Spectral * lerp(BeginGain, Slot.ReturnGain, T);
		}
		Slot.ReturnLowPass[Channel] = Filtered;
	}
	Slot.ReturnMaterialFilter.EndBlock();
	const float BeginFraction = Slot.ReturnSpatialFraction;
	Slot.ReturnSpatialFraction += (Slot.ReturnTargetSpatialFraction - Slot.ReturnSpatialFraction) * Blend;
	if (Slot.ReturnTargetSpatialFraction == 0.0f && Slot.ReturnSpatialFraction < 0.00001f)
	{
		Slot.ReturnSpatialFraction = 0.0f;
	}
	if (BeginFraction == 0.0f && Slot.ReturnSpatialFraction == 0.0f)
	{
		if (Slot.ReturnSpatialDirty)
		{
			Resonance_SilenceRoomReturn(Slot.ReturnSpatializer);
			Slot.ReturnSpatialDirty = false;
		}
		return false;
	}
	const Fvector* Directions = Slot.ReturnSpatial.LocalDirections;
	const float LocalDirections[6] = {Directions[0].x, Directions[0].y, Directions[0].z,
		Directions[1].x, Directions[1].y, Directions[1].z};
	const bool Spatialized = Resonance_ProcessRoomReturn(Slot.ReturnSpatializer, Data, LocalDirections,
		Slot.ReturnSpatial.SpreadDegrees, BeginFraction, Slot.ReturnSpatialFraction);
	Slot.ReturnSpatialDirty |= Spatialized;
	return Spatialized;
}

void Snd_RenderBuses(float* Output, float MuteVolume)
{
	memset(Output, 0, SND_BLOCKSIZE * SND_CHANNEL_COUNT * sizeof(float));
	// Every bus uses the same audio window, including silence and effect tails.
	for (SoundBusMeter& Meter : GBuses.Meters)
	{
		Meter.Samples += u64(SND_BLOCKSIZE) * SND_CHANNEL_COUNT;
	}

	for (u32 OrderIdx = 0; OrderIdx < GBuses.OrderCount; OrderIdx++)
	{
		u32 BusIdx = GBuses.Order[OrderIdx];
		SoundBus* Bus = &GBuses.Buses[BusIdx - 1];
		if (!Bus->HasInput && Bus->TailFrames == 0)
		{
			Bus->Peak[0] = Bus->Peak[1] = 0.0f;
			continue;
		}

		float* Data[SND_CHANNEL_COUNT] = {Bus->Data[0], Bus->Data[1]};
		SoundBusMeter& Meter = GBuses.Meters[BusIdx - 1];
		for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
		{
			for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
			{
				Meter.InputPeak = std::max(Meter.InputPeak, std::abs(Data[Channel][Frame]));
				Meter.InputEnergy += double(Data[Channel][Frame]) * Data[Channel][Frame];
			}
		}
		SoundRoomSlot* ReturnRoom = Snd_FindRoomReturnSlot(BusIdx);
		if (ReturnRoom != nullptr)
		{
			SoundAcousticCapture::SetProvenance(BusIdx, ReturnRoom->RoomId, ReturnRoom->SourceEvent,
				ReturnRoom->SourceKind, ReturnRoom->SourceMixed || !ReturnRoom->SourceTagged);
			SoundAcousticCapture::TapBus(BusIdx, SoundAcousticCapture::Stage::RoomInput, Data);
		}
		SoundEffectProcess Process = {Data, nullptr, nullptr, nullptr, Bus->HasInput};
		u32 TailFrames = 0;
		for (u32 EffectIdx = 0; EffectIdx < Bus->EffectCount; EffectIdx++)
		{
			SoundBusEffect* Effect = &Bus->Effects[EffectIdx];
			Process.Params = Effect->Params;
			const SoundEffectEntry* Entry = ReturnRoom != nullptr ? Snd_GetEffect(Effect->Effect) : nullptr;
			const bool IsCompressor = Entry != nullptr && xr_strcmp(Entry->Name.c_str(), "compressor") == 0;
			if (IsCompressor)
			{
				SoundAcousticCapture::TapBus(BusIdx, SoundAcousticCapture::Stage::PreCompressor, Data);
			}
			Snd_CallEffect(Effect->Effect, Effect->State, SoundEffectOp::Process, &Process);
			if (IsCompressor)
			{
				SoundAcousticCapture::TapBus(BusIdx, SoundAcousticCapture::Stage::PostCompressor, Data);
			}
			// Only generated room buses need per-effect taps. No sample logging.
			if (Bus->IsGenerated)
			{
				Meter.EffectCount = Bus->EffectCount;
				for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
				{
					for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
					{
						const float Sample = Data[Channel][Frame];
						Meter.EffectPeak[EffectIdx] = std::max(Meter.EffectPeak[EffectIdx], std::abs(Sample));
						Meter.EffectEnergy[EffectIdx] += double(Sample) * Sample;
					}
				}
			}

			u32 EffectTail = 0;
			Snd_CallEffect(Effect->Effect, Effect->State, SoundEffectOp::Tail, &EffectTail);
			TailFrames = std::max(TailFrames, EffectTail);
		}

		Bus->TailFrames = Bus->HasInput ? TailFrames : Bus->TailFrames - std::min(Bus->TailFrames, (u32)SND_BLOCKSIZE);

		// Transport the already formed room field exactly once. It is never
		// fed into another room reverb, and tail lifetime stays independent.
		if (SoundRoomSlot* Room = Snd_FindRoomReturnSlot(BusIdx))
		{
			if (!Meter.SourceObserved)
			{
				Meter.SourceEvent = Room->SourceEvent;
				Meter.SourceKind = Room->SourceKind;
				Meter.SourceObserved = true;
			}
			Meter.SourceMixed |= Room->SourceMixed || !Room->SourceTagged ||
				Meter.SourceEvent != Room->SourceEvent || Meter.SourceKind != Room->SourceKind;
			Meter.ReturnSamples += u64(SND_BLOCKSIZE) * SND_CHANNEL_COUNT;
			for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
			{
				for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
				{
					const float Sample = Data[Channel][Frame];
					Meter.ReturnInputEnergy += double(Sample) * Sample;
					Meter.ReturnInputPeak = std::max(Meter.ReturnInputPeak, std::abs(Sample));
				}
			}
			Meter.SpatialBlocks += Snd_ProcessRoomReturn(*Room, Data) ? 1u : 0u;
			SoundAcousticCapture::TapBus(BusIdx, SoundAcousticCapture::Stage::Return, Data);
			for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
			{
				for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
				{
					const float Sample = Data[Channel][Frame];
					Meter.ReturnOutputEnergy += double(Sample) * Sample;
					Meter.ReturnOutputPeak = std::max(Meter.ReturnOutputPeak, std::abs(Sample));
				}
			}
		}

		float Gain = Bus->Volume * Bus->UserVolume * (BusIdx == GBuses.Master ? MuteVolume : 1.0f);
		float BeginGain = Bus->Gain < 0.0f ? Gain : Bus->Gain;
		Bus->Gain = Gain;
		SoundBus* OutputBus = Snd_GetBus(Bus->Output);

		for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
		{
			float Peak = 0.0f;
			for (u32 Frame = 0; Frame < SND_BLOCKSIZE; Frame++)
			{
				Peak = std::max(Peak, std::abs(Data[Channel][Frame]));
				if (OutputBus != nullptr)
				{
					const float FrameGain = lerp(BeginGain, Gain, float(Frame) / float(SND_BLOCKSIZE - 1));
					const float Sample = Data[Channel][Frame] * FrameGain;
					Meter.OutputEnergy += double(Sample) * Sample;
				}
			}

			Bus->Peak[Channel] = Peak * Gain;
			Meter.OutputPeak = std::max(Meter.OutputPeak, Bus->Peak[Channel]);
		}

		if (OutputBus != nullptr)
		{
			float* OutputData[SND_CHANNEL_COUNT];
			DSP_MixBuffer(Snd_OpenBus(OutputBus, OutputData), Data, BeginGain, Gain, SND_BLOCKSIZE);
			continue;
		}

		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; Frame++)
		{
			float FrameGain = lerp(BeginGain, Gain, (float)Frame / (float)(SND_BLOCKSIZE - 1));
			for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
			{
				float& Sample = Output[Frame * SND_CHANNEL_COUNT + Channel];
				const float Mixed = Sample + Data[Channel][Frame] * FrameGain;
				Meter.ClippedSamples += std::abs(Mixed) > 1.0f ? 1u : 0u;
				Sample = std::clamp(Mixed, -1.0f, 1.0f);
				Meter.OutputEnergy += double(Sample) * Sample;
			}
		}
	}
}

// Offline-generated acoustic cuboids require actual 3D membership.
// The legacy downward ray can select a room when the listener stands ON
// its roof, or pick the room on another floor.
// Fixed-size sparse 3D grid indexes the native Zone AABBs; no per-frame
// level geometry rebuild, no new audio mixer.
static constexpr float kAcousticZoneCellSize = 8.0f;
static int Snd_ZoneCell(float Position)
{
    return static_cast<int>(std::floor(Position / kAcousticZoneCellSize));
}

static u64 Snd_ZoneCellKey(int X, int Y, int Z)
{
    constexpr u64 mask = (1ull << 21) - 1ull;
    return ((u64(X) & mask) << 42) | ((u64(Y) & mask) << 21) | (u64(Z) & mask);
}

float Snd_RoomPortalTransitionDepth(const SoundRoomPortal& Portal)
{
	// Depth scales with the smaller real opening dimension; unrelated wall
	// faces receive no expanded ownership region.
	return std::clamp(std::min(Portal.HalfExtentU, Portal.HalfExtentV) * 0.5f, 0.1f, 0.75f);
}

static void Snd_IndexRoomPortal(u32 Index)
{
	const SoundRoomPortal& Portal = GBuses.Portals[Index];
	if ((Portal.Flags & SoundRoomPortalOpen) == 0)
	{
		return;
	}
	Fvector TangentV;
	TangentV.crossproduct(Portal.Normal, Portal.TangentU);
	const float Depth = Snd_RoomPortalTransitionDepth(Portal);
	Fvector Extent;
	Extent.set(std::abs(Portal.TangentU.x) * Portal.HalfExtentU + std::abs(TangentV.x) * Portal.HalfExtentV + std::abs(Portal.Normal.x) * Depth,
		std::abs(Portal.TangentU.y) * Portal.HalfExtentU + std::abs(TangentV.y) * Portal.HalfExtentV + std::abs(Portal.Normal.y) * Depth,
		std::abs(Portal.TangentU.z) * Portal.HalfExtentU + std::abs(TangentV.z) * Portal.HalfExtentV + std::abs(Portal.Normal.z) * Depth);
	const int X0 = Snd_ZoneCell(Portal.Center.x - Extent.x), X1 = Snd_ZoneCell(Portal.Center.x + Extent.x);
	const int Y0 = Snd_ZoneCell(Portal.Center.y - Extent.y), Y1 = Snd_ZoneCell(Portal.Center.y + Extent.y);
	const int Z0 = Snd_ZoneCell(Portal.Center.z - Extent.z), Z1 = Snd_ZoneCell(Portal.Center.z + Extent.z);
	const u64 Cells = u64(X1 - X0 + 1) * u64(Y1 - Y0 + 1) * u64(Z1 - Z0 + 1);
	if (Cells > 8192)
	{
		GBuses.LargePortalIndices.push_back(Index);
		return;
	}
	for (int X = X0; X <= X1; ++X)
	{
		for (int Y = Y0; Y <= Y1; ++Y)
		{
			for (int Z = Z0; Z <= Z1; ++Z)
			{
				GBuses.PortalCells[Snd_ZoneCellKey(X, Y, Z)].push_back(Index);
			}
		}
	}
}

static void Snd_IndexZone(u32 Idx)
{
    const sound_zone_params& Z = GBuses.Zones[Idx];
    if (Z.min.x > Z.max.x || Z.min.y > Z.max.y || Z.min.z > Z.max.z)
        return;
    const int x0 = Snd_ZoneCell(Z.min.x), x1 = Snd_ZoneCell(Z.max.x);
    const int y0 = Snd_ZoneCell(Z.min.y), y1 = Snd_ZoneCell(Z.max.y);
    const int z0 = Snd_ZoneCell(Z.min.z), z1 = Snd_ZoneCell(Z.max.z);
    const u64 cells = u64(x1 - x0 + 1) * u64(y1 - y0 + 1) * u64(z1 - z0 + 1);
    // Vast outdoor volumes should not fill the sparse index.
    if (cells > 8192)
    {
        GBuses.LargeZoneIndices.push_back(Idx);
        return;
    }
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                GBuses.ZoneCells[Snd_ZoneCellKey(x, y, z)].push_back(Idx);
}
static bool Snd_PointInZone(const sound_zone_params& Z, const Fvector& P)
{
    constexpr float border = 0.004f;
    return P.x >= Z.min.x - border && P.x <= Z.max.x + border &&
        P.y >= Z.min.y - border && P.y <= Z.max.y + border &&
        P.z >= Z.min.z - border && P.z <= Z.max.z + border;
}

void Snd_AddZone(sound_zone_params* Params, bool IsEditor)
{
	if (IsEditor)
	{
		Snd_ResetZones();
	}

	Snd_AssignZoneBus(Params);
	GBuses.Zones.emplace_back(std::move(*Params));
	Snd_IndexZone(u32(GBuses.Zones.size() - 1));
	GBuses.IsEditorZone = IsEditor;
	Snd_RebuildOrder();
}

void Snd_ResetZones()
{
	for (u32 BusIdx = 0; BusIdx < SND_BUS_COUNT; BusIdx++)
	{
		SoundBus* Bus = &GBuses.Buses[BusIdx];
		if (Bus->IsGenerated)
		{
			Snd_ClearBus(Bus);
			Bus->Name = nullptr;
		}
	}

	GBuses.Zones.clear();
	GBuses.ZoneCells.clear();
	GBuses.LargeZoneIndices.clear();
	GBuses.ZoneRooms.clear();
	GBuses.Rooms.clear();
	GBuses.RoomIndices.clear();
	GBuses.Portals.clear();
	GBuses.RoomPortalIndices.clear();
	GBuses.UnresolvedPortalIndices.clear();
	GBuses.PortalIndicesById.clear();
	GBuses.PortalCells.clear();
	GBuses.LargePortalIndices.clear();
	for (SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		Resonance_DestroyRoomReturn(Slot.ReturnSpatializer);
		Slot = {};
	}
	GBuses.IsEditorZone = false;
	Snd_RebuildOrder();
}

u32 Snd_FindZone(const Fvector& Position)
{
    if (GBuses.Zones.empty())
        return 0;
    // Find the correct room on the correct floor. In overlapping cuboids
    // the smaller one wins. This prevents a large hall from masking a closet.
    u32 best = 0;
    float bestVolume = FLT_MAX;
    const auto consider = [&](u32 idx, u32& selected, float& size)
    {
        if (idx >= GBuses.Zones.size())
            return;
        const sound_zone_params& zone = GBuses.Zones[idx];
        if (!Snd_PointInZone(zone, Position))
            return;
        const float volume = std::max(zone.max.x-zone.min.x, 0.0f) *
            std::max(zone.max.y-zone.min.y, 0.0f) *
            std::max(zone.max.z-zone.min.z, 0.0f);
        if (volume < size)
        {
            selected = idx + 1;
            size = volume;
        }
    };

    const u64 key = Snd_ZoneCellKey(Snd_ZoneCell(Position.x),
        Snd_ZoneCell(Position.y), Snd_ZoneCell(Position.z));
    auto it = GBuses.ZoneCells.find(key);
    if (it != GBuses.ZoneCells.end())
        for (u32 index : it->second)
            consider(index, best, bestVolume);
    for (u32 index : GBuses.LargeZoneIndices)
        consider(index, best, bestVolume);
    return best;
}

u32 Snd_GetZoneBus(u32 ZoneIdx)
{
	ZoneIdx = GBuses.IsEditorZone ? 1 : ZoneIdx;
	if (!GBuses.ZoneRooms.empty())
	{
		return Snd_GetRoomBus(Snd_GetZoneRoom(ZoneIdx));
	}
	return (ZoneIdx == 0 || ZoneIdx > GBuses.Zones.size()) ? 0 : GBuses.Zones[ZoneIdx - 1].bus;
}

const xr_vector<sound_zone_params>& Snd_GetZones()
{
	return GBuses.Zones;
}

bool Snd_LoadRoomMetadata(IReader* Reader, u32 EnvCrc32)
{
	// Little-endian: magic[8], version, env CRC, zone/room/portal counts,
	// payload CRC; zone IDs[u32]. V1 room44/portal40; v2 room56/portal64.
	// V2 appends room cover/open/roof areas and bounded aperture geometry.
	// Full validation precedes mutation, so old levels keep legacy preset buses.
	const auto Reject = [](const char* Reason)
	{
		Msg("! [LIMINAL_ROOMS] sidecar rejected (%s); using legacy environment buses", Reason);
		return false;
	};
	if (Reader == nullptr || Reader->length() < 32 || Reader->length() > SoundRoomMetadataByteLimit || GBuses.IsEditorZone)
	{
		return Reject("missing/oversized header payload or editor geometry");
	}
	Reader->rewind();
	const u8* Data = static_cast<const u8*>(Reader->pointer());
	char Magic[8];
	Reader->r(Magic, sizeof(Magic));
	const u32 Version = Reader->r_u32();
	const u32 BoundEnvCrc = Reader->r_u32();
	const u32 ZoneCount = Reader->r_u32();
	const u32 RoomCount = Reader->r_u32();
	const u32 PortalCount = Reader->r_u32();
	const u32 PayloadCrc = Reader->r_u32();
	const u32 ComputedPayloadCrc = XRay::Sound::RoomChecksum::IeeeCrc32(Data + 32, Reader->length() - 32);
	Msg("* [LIMINAL_ROOMS] algorithm=ieee_crc32 env_stored=%08X env_computed=%08X payload_stored=%08X payload_computed=%08X",
		BoundEnvCrc, EnvCrc32, PayloadCrc, ComputedPayloadCrc);
	if (memcmp(Magic, "IXROOMS", 8) != 0 || (Version != 1 && Version != 2 && Version != 3) || BoundEnvCrc != EnvCrc32)
	{
		return Reject("format/version or level.snd_env CRC mismatch");
	}
	const u64 ExpectedBytes = 32ull + 4ull * ZoneCount +
		(Version == 3 ? 100ull : Version == 2 ? 56ull : 44ull) * RoomCount + (Version >= 2 ? 64ull : 40ull) * PortalCount;
	if (ZoneCount != GBuses.Zones.size() || RoomCount == 0 || RoomCount > 65536 ||
		PortalCount > SoundRoomMetadataPortalLimit || ExpectedBytes > SoundRoomMetadataByteLimit ||
		ExpectedBytes != Reader->length() ||
		ComputedPayloadCrc != PayloadCrc)
	{
		return Reject("counts, length or payload CRC");
	}
	xr_vector<u32> ZoneRooms(ZoneCount);
	Reader->r(ZoneRooms.data(), ZoneCount * sizeof(u32));
	xr_vector<SoundRoomInfo> Rooms;
	Rooms.reserve(RoomCount);
	xr_hash_map<u32, u32> RoomIndices;
	const auto FiniteVector = [](const Fvector& Value)
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
	};
	for (u32 Index = 0; Index < RoomCount; ++Index)
	{
		SoundRoomInfo Room;
		Room.Id = Reader->r_u32();
		Room.ReferenceZone = Reader->r_u32();
		Reader->r(&Room.Min, sizeof(Fvector));
		Reader->r(&Room.Max, sizeof(Fvector));
		Room.Volume = Reader->r_float();
		Room.SurfaceArea = Reader->r_float();
		Room.WetReference = Reader->r_float();
		if (Version >= 2)
		{
			Room.CoverFraction = Reader->r_float();
			Room.OpeningArea = Reader->r_float();
			Room.RoofOpeningArea = Reader->r_float();
			Room.HasOpenGeometry = true;
		}
		if (Version == 3)
		{
			Reader->r(Room.MeanBoundaryLoss, sizeof(Room.MeanBoundaryLoss));
			Room.SampledClosedArea = Reader->r_float();
			Room.ClassifiedAreaFraction = Reader->r_float();
			Room.HasMaterialGeometry = true;
		}
		Room.Dimensions.sub(Room.Max, Room.Min);
		if (Room.Id == 0 || Room.Id == u32(-1) || RoomIndices.contains(Room.Id) ||
			Room.ReferenceZone == 0 || Room.ReferenceZone > ZoneCount ||
			!FiniteVector(Room.Min) || !FiniteVector(Room.Max) ||
			Room.Dimensions.x <= 0.0f || Room.Dimensions.y <= 0.0f || Room.Dimensions.z <= 0.0f ||
			!std::isfinite(Room.Volume) || Room.Volume <= 0.0f ||
			!std::isfinite(Room.SurfaceArea) || Room.SurfaceArea <= 0.0f ||
			!std::isfinite(Room.WetReference) || Room.WetReference < 0.0f ||
			ZoneRooms[Room.ReferenceZone - 1] != Room.Id)
		{
			return Reject("room identity, geometry or reference zone");
		}
		if (Room.HasOpenGeometry && (!std::isfinite(Room.CoverFraction) ||
			Room.CoverFraction < 0.0f || Room.CoverFraction > 1.0f ||
			!std::isfinite(Room.OpeningArea) || Room.OpeningArea < 0.0f ||
			!std::isfinite(Room.RoofOpeningArea) || Room.RoofOpeningArea < 0.0f ||
			Room.RoofOpeningArea > Room.OpeningArea + 0.001f ||
			Room.OpeningArea > Room.SurfaceArea + 0.001f))
		{
			return Reject("room opening geometry");
		}
		if (Room.HasMaterialGeometry)
		{
			for (const float Loss : Room.MeanBoundaryLoss)
			{
				if (!std::isfinite(Loss) || Loss < 0.0f || Loss > 1.0f)
				{
					return Reject("room material band loss");
				}
			}
			const float AreaTolerance = std::max(0.001f, Room.SurfaceArea * 0.0001f);
			if (!std::isfinite(Room.SampledClosedArea) || Room.SampledClosedArea < 0.0f ||
				Room.SampledClosedArea > Room.SurfaceArea + AreaTolerance ||
				!std::isfinite(Room.ClassifiedAreaFraction) ||
				Room.ClassifiedAreaFraction < 0.0f || Room.ClassifiedAreaFraction > 1.0f)
			{
				return Reject("room material sampling provenance");
			}
			// A v3 record with no observed classified boundary retains the exact
			// v2 decay. Do not replace an unobserved room with an inferred spectrum.
			// Validate every serialized band above even when this DSP fallback applies.
			Room.HasMaterialGeometry = Room.ClassifiedAreaFraction > 0.0f;
		}
		RoomIndices[Room.Id] = Index;
		Rooms.push_back(Room);
	}
	for (u32 Index = 0; Index < ZoneCount; ++Index)
	{
		const auto Found = RoomIndices.find(ZoneRooms[Index]);
		if (Found == RoomIndices.end())
		{
			return Reject("unmapped cuboid");
		}
		const SoundRoomInfo& Room = Rooms[Found->second];
		const sound_zone_params& Zone = GBuses.Zones[Index];
		constexpr float Tolerance = 0.001f;
		if (Zone.min.x < Room.Min.x - Tolerance || Zone.min.y < Room.Min.y - Tolerance ||
			Zone.min.z < Room.Min.z - Tolerance || Zone.max.x > Room.Max.x + Tolerance ||
			Zone.max.y > Room.Max.y + Tolerance || Zone.max.z > Room.Max.z + Tolerance)
		{
			return Reject("cuboid outside catalog room bounds");
		}
	}
	xr_vector<SoundRoomPortal> Portals;
	Portals.reserve(PortalCount);
	xr_hash_map<u32, xr_vector<u32>> RoomPortalIndices;
	xr_vector<u32> UnresolvedPortalIndices;
	xr_hash_map<u32, u32> PortalIndicesById;
	RoomPortalIndices.reserve(RoomCount + 1);
	PortalIndicesById.reserve(PortalCount);
	const auto ValidEndpoint = [&RoomIndices](u32 Id)
	{
		return Id == 0 || Id == u32(-1) || RoomIndices.contains(Id);
	};
	for (u32 Index = 0; Index < PortalCount; ++Index)
	{
		SoundRoomPortal Portal;
		Portal.Id = Reader->r_u32();
		Portal.RoomA = Reader->r_u32();
		Portal.RoomB = Reader->r_u32();
		Reader->r(&Portal.Center, sizeof(Fvector));
		Reader->r(&Portal.Normal, sizeof(Fvector));
		Portal.Area = Reader->r_float();
		if (Version >= 2)
		{
			Reader->r(&Portal.TangentU, sizeof(Fvector));
			Portal.HalfExtentU = Reader->r_float();
			Portal.HalfExtentV = Reader->r_float();
			Portal.Flags = Reader->r_u32();
		}
		if (!ValidEndpoint(Portal.RoomA) || !ValidEndpoint(Portal.RoomB) ||
			!FiniteVector(Portal.Center) || !FiniteVector(Portal.Normal) ||
			Portal.Normal.square_magnitude() < 0.9f || Portal.Normal.square_magnitude() > 1.1f ||
			!std::isfinite(Portal.Area) || Portal.Area < 0.0f)
		{
			return Reject("invalid portal candidate");
		}
		if (Version >= 2 && (!FiniteVector(Portal.TangentU) ||
			!std::isfinite(Portal.HalfExtentU) || !std::isfinite(Portal.HalfExtentV) ||
			Portal.HalfExtentU < 0.0f || Portal.HalfExtentV < 0.0f ||
			(Portal.Flags & ~(SoundRoomPortalOpen | SoundRoomPortalRoof)) != 0))
		{
			return Reject("invalid portal support");
		}
		if ((Portal.Flags & SoundRoomPortalOpen) != 0 &&
			(Portal.RoomA == Portal.RoomB || Portal.RoomA == u32(-1) || Portal.RoomB == u32(-1) ||
			Portal.Area <= 0.0f || Portal.HalfExtentU <= 0.0f || Portal.HalfExtentV <= 0.0f ||
			Portal.Normal.square_magnitude() < 0.99f || Portal.Normal.square_magnitude() > 1.01f ||
			Portal.TangentU.square_magnitude() < 0.99f || Portal.TangentU.square_magnitude() > 1.01f ||
			std::abs(Portal.Normal.dotproduct(Portal.TangentU)) > 0.01f))
		{
			return Reject("unresolved or nonorthogonal open portal");
		}
		// Preserve file ordering in each incident list. Unresolved v1 endpoints
		// remain wildcard candidates, but never require scanning resolved rooms.
		if (Portal.RoomA != u32(-1)) RoomPortalIndices[Portal.RoomA].push_back(Index);
		if (Portal.RoomB != u32(-1) && Portal.RoomB != Portal.RoomA)
			RoomPortalIndices[Portal.RoomB].push_back(Index);
		if (Portal.RoomA == u32(-1) || Portal.RoomB == u32(-1))
			UnresolvedPortalIndices.push_back(Index);
		// The legacy linear lookup returned the first matching ID.
		PortalIndicesById.try_emplace(Portal.Id, Index);
		Portals.push_back(Portal);
	}
	for (SoundBus& Bus : GBuses.Buses)
	{
		if (Bus.IsGenerated)
		{
			Snd_ClearBus(&Bus);
			Bus.Name = nullptr;
		}
	}
	for (sound_zone_params& Zone : GBuses.Zones)
	{
		Zone.bus = 0;
	}
	GBuses.ZoneRooms = std::move(ZoneRooms);
	GBuses.Rooms = std::move(Rooms);
	GBuses.RoomIndices = std::move(RoomIndices);
	GBuses.Portals = std::move(Portals);
	GBuses.RoomPortalIndices = std::move(RoomPortalIndices);
	GBuses.UnresolvedPortalIndices = std::move(UnresolvedPortalIndices);
	GBuses.PortalIndicesById = std::move(PortalIndicesById);
	GBuses.PortalCells.clear();
	GBuses.LargePortalIndices.clear();
	for (u32 Index = 0; Index < GBuses.Portals.size(); ++Index)
	{
		Snd_IndexRoomPortal(Index);
	}
	Msg("* [LIMINAL_ROOMS_V6] format=%u rooms=%zu portals=%zu transition_cells=%zu incident_rooms=%zu unresolved_candidates=%zu portal_id_index=%zu; open-area Sabine model=%u",
		Version, GBuses.Rooms.size(), GBuses.Portals.size(), GBuses.PortalCells.size(),
		GBuses.RoomPortalIndices.size(), GBuses.UnresolvedPortalIndices.size(), GBuses.PortalIndicesById.size(), Version == 2 ? 1u : 0u);
	if (Version == 3)
	{
		u32 MaterialRooms = 0;
		for (const SoundRoomInfo& Room : GBuses.Rooms)
		{
			MaterialRooms += Room.HasMaterialGeometry ? 1u : 0u;
			if (!Room.HasMaterialGeometry)
			{
				Msg("* [LIMINAL_ROOM_MATERIAL_FALLBACK] room=%u classified_fraction=0 decay_model=exact_v2 reason=no_classified_surface_observation", Room.Id);
			}
		}
		Msg("* [LIMINAL_ROOMS_V7] material_rooms=%u legacy_unobserved_rooms=%zu octave_bands=9 decay_model=eyring_boundary_loss_and_air unobserved_decay_model=exact_v2 measured_materials=0",
			MaterialRooms, GBuses.Rooms.size() - MaterialRooms);
	}
	Snd_CreateRoomPool();
	Snd_RebuildOrder();
	return true;
}

u32 Snd_GetZoneRoom(u32 ZoneIdx)
{
	return ZoneIdx != 0 && ZoneIdx <= GBuses.ZoneRooms.size() ? GBuses.ZoneRooms[ZoneIdx - 1] : 0;
}

bool Snd_HasRoomMetadata()
{
	return !GBuses.Rooms.empty();
}

bool Snd_IsLogicalRoomBus(u32 BusIdx)
{
	for (const SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		if (Slot.Bus == BusIdx && Slot.RoomId != 0)
		{
			return true;
		}
	}
	return false;
}

const SoundRoomInfo* Snd_GetRoomInfo(u32 RoomId)
{
	const auto Found = GBuses.RoomIndices.find(RoomId);
	return Found != GBuses.RoomIndices.end() ? &GBuses.Rooms[Found->second] : nullptr;
}

u32 Snd_GetRoomBus(u32 RoomId)
{
	if (RoomId != 0)
	{
		for (const SoundRoomSlot& Slot : GBuses.RoomSlots)
		{
			if (Slot.RoomId == RoomId && Snd_HasReverbBus(Slot.Bus))
			{
				return Slot.Bus;
			}
		}
	}
	return 0;
}

bool Snd_SetRoomReturnOrigin(u32 BusIdx, u32 RoomId, const Fvector& Position)
{
	SoundRoomSlot* Slot = Snd_FindRoomReturnSlot(BusIdx);
	if (Slot == nullptr || Slot->RoomId != RoomId || !std::isfinite(Position.x) ||
		!std::isfinite(Position.y) || !std::isfinite(Position.z))
	{
		return false;
	}
	Slot->TransportOrigin = Position;
	Slot->HasTransportOrigin = true;
	return true;
}

u32 Snd_GetRoomReturnStates(SoundRoomReturnState* Output, u32 Capacity)
{
	if (Output == nullptr || Capacity == 0)
	{
		return 0;
	}
	u32 Count = 0;
	for (const SoundRoomSlot& Slot : GBuses.RoomSlots)
	{
		const SoundBus* Bus = Snd_GetBus(Slot.Bus);
		if (Slot.RoomId == 0 || Bus == nullptr)
		{
			continue;
		}
		SoundRoomReturnState& State = Output[Count++];
		State.RoomId = Slot.RoomId;
		State.Bus = Slot.Bus;
		State.Anchor = Slot.HasTransportOrigin ? Slot.TransportOrigin : Slot.Anchor;
		State.CatalogAnchor = Slot.Anchor;
		State.HasTransportOrigin = Slot.HasTransportOrigin;
		State.Active = Bus->HasInput || Bus->TailFrames != 0;
		State.Validated = Slot.ReturnValidated;
		State.SourceEvent = Slot.SourceEvent;
		State.SourceKind = Slot.SourceKind;
		State.SourceMixed = Slot.SourceMixed;
		State.PortalId = Slot.ReturnSpatial.PortalId;
		State.SpatialFraction = Slot.ReturnSpatialFraction;
		State.TargetSpatialFraction = Slot.ReturnTargetSpatialFraction;
		State.SpatialReady = Slot.ReturnSpatializer != nullptr;
		State.Gain = Slot.ReturnGain;
		State.TargetGain = Slot.ReturnTargetGain;
		State.Cutoff = Slot.ReturnCutoff;
		State.FilterBlend = Slot.ReturnFilterBlend;
		memcpy(State.BandRatios, Slot.ReturnBandRatios, sizeof(State.BandRatios));
		memcpy(State.TargetBandRatios, Slot.ReturnTargetBandRatios, sizeof(State.TargetBandRatios));
		if (Count == Capacity)
		{
			break;
		}
	}
	return Count;
}

bool Snd_SetRoomReturnPath(u32 BusIdx, u32 RoomId, float Gain, float Cutoff,
	float FilterBlend, bool Validated, const float* BandRatios,
	const SoundRoomReturnSpatial* Spatial)
{
	SoundRoomSlot* Slot = Snd_FindRoomReturnSlot(BusIdx);
	if (Slot == nullptr || Slot->RoomId != RoomId)
	{
		return false;
	}
	bool Finite = std::isfinite(Gain) && std::isfinite(Cutoff) && std::isfinite(FilterBlend);
	for (u32 Band = 0; BandRatios != nullptr && Band < 3; ++Band)
	{
		Finite = Finite && std::isfinite(BandRatios[Band]);
	}
	if (Spatial != nullptr)
	{
		Finite = Finite && Spatial->PortalId != 0 && std::isfinite(Spatial->Fraction) &&
			std::isfinite(Spatial->SpreadDegrees);
		for (const Fvector& Direction : Spatial->LocalDirections)
		{
			Finite = Finite && std::isfinite(Direction.x) && std::isfinite(Direction.y) &&
				std::isfinite(Direction.z) && Direction.square_magnitude() > 0.000001f;
		}
	}
	Slot->ReturnValidated = Validated && Finite;
	Slot->ReturnTargetSpatialFraction = Slot->ReturnValidated && Spatial != nullptr &&
		Slot->ReturnSpatializer != nullptr ? std::clamp(Spatial->Fraction, 0.0f, 1.0f) : 0.0f;
	if (Slot->ReturnTargetSpatialFraction > 0.0f)
	{
		Slot->ReturnSpatial = *Spatial;
		Slot->ReturnSpatial.SpreadDegrees = std::clamp(Spatial->SpreadDegrees, 0.0f, 360.0f);
		for (Fvector& Direction : Slot->ReturnSpatial.LocalDirections)
		{
			Direction.normalize_safe();
		}
	}
	Slot->ReturnTargetGain = Slot->ReturnValidated ? std::clamp(Gain, 0.0f, 1.0f) : 0.0f;
	Slot->ReturnTargetCutoff = Slot->ReturnValidated ? std::clamp(Cutoff, 20.0f, 20000.0f) : 20000.0f;
	Slot->ReturnTargetFilterBlend = Slot->ReturnValidated ? std::clamp(FilterBlend, 0.0f, 1.0f) : 0.0f;
	for (u32 Band = 0; Band < 3; ++Band)
	{
		Slot->ReturnTargetBandRatios[Band] = Slot->ReturnValidated && BandRatios != nullptr
			? std::clamp(BandRatios[Band], 0.0f, 1.0f) : 1.0f;
	}
	const SoundBus* Bus = Snd_GetBus(BusIdx);
	if (Bus != nullptr && !Bus->HasInput && Bus->TailFrames == 0)
	{
		// A prepared silent room has no audible transition to smooth. Its
		// first impulse uses the validated route immediately.
		if (Slot->ReturnSpatialDirty)
		{
			Resonance_SilenceRoomReturn(Slot->ReturnSpatializer);
			Slot->ReturnSpatialDirty = false;
		}
		Slot->ReturnSpatialFraction = Slot->ReturnTargetSpatialFraction;
		Slot->ReturnGain = Slot->ReturnTargetGain;
		Slot->ReturnCutoff = Slot->ReturnTargetCutoff;
		Slot->ReturnFilterBlend = Slot->ReturnTargetFilterBlend;
		memset(Slot->ReturnLowPass, 0, sizeof(Slot->ReturnLowPass));
		memcpy(Slot->ReturnBandRatios, Slot->ReturnTargetBandRatios, sizeof(Slot->ReturnBandRatios));
		Slot->ReturnMaterialFilter.Reset();
		Slot->ReturnMaterialFilter.SetRatios(Slot->ReturnBandRatios[0], Slot->ReturnBandRatios[1], Slot->ReturnBandRatios[2], float(SND_SAMPLERATE));
	}
	return Finite;
}

void Snd_BeginRoomUpdate()
{
	// Preparing all twelve graph transitions in one frame would block audio.
	// Listener is requested first; other new rooms may wait until the next frame.
	GBuses.RoomAssignmentBudget = 1;
}

void Snd_PrepareRoomBuses(const u32* RoomIds, u32 Count, const bool* PinnedBuses)
{
	if (RoomIds == nullptr || GBuses.Rooms.empty() || GBuses.RoomAssignmentBudget == 0)
	{
		return;
	}
	bool Reserved[SoundRoomPoolSize] = {};
	for (u32 SlotIdx = 0; SlotIdx < SoundRoomPoolSize; ++SlotIdx)
	{
		const SoundRoomSlot& Slot = GBuses.RoomSlots[SlotIdx];
		Reserved[SlotIdx] = Slot.Bus != 0 && PinnedBuses != nullptr && PinnedBuses[Slot.Bus - 1];
		for (u32 Idx = 0; Idx < Count; ++Idx)
		{
			Reserved[SlotIdx] |= RoomIds[Idx] != 0 && RoomIds[Idx] == Slot.RoomId;
		}
	}
	for (u32 Idx = 0; Idx < Count; ++Idx)
	{
		if (GBuses.RoomAssignmentBudget == 0)
		{
			break;
		}
		const SoundRoomInfo* Room = Snd_GetRoomInfo(RoomIds[Idx]);
		if (Room == nullptr || Snd_GetRoomBus(Room->Id) != 0)
		{
			continue;
		}
		for (u32 SlotIdx = 0; SlotIdx < SoundRoomPoolSize; ++SlotIdx)
		{
			SoundRoomSlot& Slot = GBuses.RoomSlots[SlotIdx];
			SoundBus* Bus = Snd_GetBus(Slot.Bus);
			if (Reserved[SlotIdx] || Bus == nullptr || Bus->HasInput || Bus->TailFrames != 0 ||
				!Snd_HasReverbBus(Slot.Bus))
			{
				continue;
			}
			// Fully drained and not referenced by listener/source transitions.
			// Reuse the existing graph; never allocate/recreate on the audio thread.
			Snd_SetRoomEffectParameters(Bus->Effects[0], *Room);
			Resonance_SetRoomOpenGeometry(Bus->Effects[0].State, Room->CoverFraction,
				Room->OpeningArea, Room->RoofOpeningArea, Room->HasOpenGeometry);
			Resonance_SetRoomMaterials(Bus->Effects[0].State, Room->MeanBoundaryLoss, Room->HasMaterialGeometry);
			Snd_CallEffect(Bus->Effects[0].Effect, Bus->Effects[0].State,
				SoundEffectOp::Update, Bus->Effects[0].Params);
			const auto PrimeStart = std::chrono::steady_clock::now();
			Resonance_PrimeInactiveReverb(Bus->Effects[0].State);
			const u32 PrimeUs = u32(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - PrimeStart).count());
			if (PrimeUs > GBuses.RoomPrimePeakUs)
			{
				const bool First = GBuses.RoomPrimePeakUs == 0;
				GBuses.RoomPrimePeakUs = PrimeUs;
				if (First || PrimeUs > 5000)
				{
					Msg("* [LIMINAL_ROOMS] inactive_prime_peak_us=%u assignments_per_update=1 room=%u", PrimeUs, Room->Id);
				}
			}
			for (u32 EffectIdx = 1; EffectIdx < Bus->EffectCount; ++EffectIdx)
			{
				Snd_CallEffect(Bus->Effects[EffectIdx].Effect, Bus->Effects[EffectIdx].State,
					SoundEffectOp::Reset, nullptr);
			}
			memset(Bus->Data, 0, sizeof(Bus->Data));
			GBuses.Meters[Slot.Bus - 1] = {};
			// A recycled bus must not inherit the previous room's listener path
			// or filter history. A known cuboid center stays inside the room;
			// the aggregate room AABB center need not do so for nonconvex rooms.
			const u32 BusIdx = Slot.Bus;
			void* Spatializer = Slot.ReturnSpatializer;
			Resonance_PrimeRoomReturn(Spatializer);
			Slot = {};
			Slot.Bus = BusIdx;
			Slot.ReturnSpatializer = Spatializer;
			Slot.RoomId = Room->Id;
			const sound_zone_params& ReferenceZone = GBuses.Zones[Room->ReferenceZone - 1];
			Slot.Anchor.add(ReferenceZone.min, ReferenceZone.max).mul(0.5f);
			Reserved[SlotIdx] = true;
			--GBuses.RoomAssignmentBudget;
			break;
		}
		// If all slots are pinned or decaying, this room has no late send until
		// a slot becomes free. It never borrows another room's reverb state.
	}
}

u32 Snd_FindRoomPortals(u32 SourceRoom, u32 ListenerRoom, const Fvector& Position,
	SoundRoomPortal* Output, u32 Capacity)
{
	if (Output == nullptr || Capacity == 0)
	{
		return 0;
	}
	Capacity = std::min(Capacity, 16u);
	double Scores[16] = {};
	u32 Indices[16] = {};
	u32 Count = 0;
	const auto Matches = [](u32 Candidate, u32 Room)
	{
		return Candidate == Room || Candidate == u32(-1);
	};
	const auto Consider = [&](u32 Index)
	{
		const SoundRoomPortal& Portal = GBuses.Portals[Index];
		const bool SameRoom = SourceRoom == ListenerRoom;
		const bool IncidentOpen = (Portal.RoomA == SourceRoom || Portal.RoomB == SourceRoom) &&
			Portal.RoomA != u32(-1) && Portal.RoomB != u32(-1) &&
			(Portal.Flags & SoundRoomPortalOpen) != 0;
		if ((SameRoom && !IncidentOpen) || (!SameRoom &&
			!((Matches(Portal.RoomA, SourceRoom) && Matches(Portal.RoomB, ListenerRoom)) ||
				(Matches(Portal.RoomB, SourceRoom) && Matches(Portal.RoomA, ListenerRoom)))))
		{
			return;
		}
		const float Distance = Portal.Center.distance_to_sqr(Position);
		// A larger confirmed opening can contribute more than a nearer tiny
		// hole. Legacy candidates retain monotonic nearest-first ordering. No
		// orientation gate certifies a path; callers still trace both real legs.
		const bool OpenGeometry = (Portal.Flags & SoundRoomPortalOpen) != 0 && Portal.Area > 0.0f;
		const double Score = OpenGeometry ? double(Portal.Area) / (double(Distance) + Portal.Area) : 1.0 / (double(Distance) + 1.0);
		u32 Insert = 0;
		while (Insert < Count && (Scores[Insert] > Score || (Scores[Insert] == Score && Indices[Insert] < Index)))
		{
			++Insert;
		}
		if (Insert >= Capacity)
		{
			return;
		}
		Count = std::min(Count + 1, Capacity);
		for (u32 Move = Count - 1; Move > Insert; --Move)
		{
			Scores[Move] = Scores[Move - 1];
			Indices[Move] = Indices[Move - 1];
			Output[Move] = Output[Move - 1];
		}
		Scores[Insert] = Score;
		Indices[Insert] = Index;
		Output[Insert] = Portal;
	};
	// Use the indoor endpoint even when the source is outdoors. The outdoor
	// adjacency would otherwise contain openings from every building in Zaton.
	const u32 IncidentRoom = SourceRoom != 0 && SourceRoom != u32(-1) ? SourceRoom : ListenerRoom;
	const auto Incident = GBuses.RoomPortalIndices.find(IncidentRoom);
	if (Incident != GBuses.RoomPortalIndices.end())
	{
		for (u32 Index : Incident->second) Consider(Index);
	}
	for (u32 Index : GBuses.UnresolvedPortalIndices)
	{
		const SoundRoomPortal& Portal = GBuses.Portals[Index];
		if (Incident != GBuses.RoomPortalIndices.end() && (Portal.RoomA == IncidentRoom || Portal.RoomB == IncidentRoom))
		{
			continue; // Already considered through its resolved incident endpoint.
		}
		Consider(Index);
	}
	return Count;
}

const SoundRoomPortal* Snd_GetRoomPortal(u32 PortalId)
{
	const auto Found = GBuses.PortalIndicesById.find(PortalId);
	return Found != GBuses.PortalIndicesById.end() ? &GBuses.Portals[Found->second] : nullptr;
}

u32 Snd_FindRoomTransitionPortals(u32 RoomId, const Fvector& Position,
	SoundRoomPortal* Output, u32 Capacity)
{
	if (Output == nullptr || Capacity == 0 || RoomId == u32(-1))
	{
		return 0;
	}
	Capacity = std::min(Capacity, 16u);
	float Distances[16] = {};
	u32 Count = 0;
	const auto Consider = [&](u32 Index)
	{
		const SoundRoomPortal& Portal = GBuses.Portals[Index];
		if ((Portal.Flags & SoundRoomPortalOpen) == 0 ||
			(Portal.RoomA != RoomId && Portal.RoomB != RoomId))
		{
			return;
		}
		Fvector Delta, TangentV;
		Delta.sub(Position, Portal.Center);
		TangentV.crossproduct(Portal.Normal, Portal.TangentU);
		const float PlaneDistance = std::abs(Delta.dotproduct(Portal.Normal));
		if (PlaneDistance > Snd_RoomPortalTransitionDepth(Portal) ||
			std::abs(Delta.dotproduct(Portal.TangentU)) > Portal.HalfExtentU ||
			std::abs(Delta.dotproduct(TangentV)) > Portal.HalfExtentV)
		{
			return;
		}
		const float Distance = Delta.square_magnitude();
		u32 Insert = 0;
		while (Insert < Count && Distances[Insert] <= Distance)
		{
			++Insert;
		}
		if (Insert >= Capacity)
		{
			return;
		}
		Count = std::min(Count + 1, Capacity);
		for (u32 Move = Count - 1; Move > Insert; --Move)
		{
			Distances[Move] = Distances[Move - 1];
			Output[Move] = Output[Move - 1];
		}
		Distances[Insert] = Distance;
		Output[Insert] = Portal;
	};
	const u64 Key = Snd_ZoneCellKey(Snd_ZoneCell(Position.x), Snd_ZoneCell(Position.y), Snd_ZoneCell(Position.z));
	const auto Found = GBuses.PortalCells.find(Key);
	if (Found != GBuses.PortalCells.end())
	{
		for (u32 Index : Found->second)
		{
			Consider(Index);
		}
	}
	for (u32 Index : GBuses.LargePortalIndices)
	{
		Consider(Index);
	}
	return Count;
}
