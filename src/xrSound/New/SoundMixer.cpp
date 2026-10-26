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
#include <atomic>
#include "SoundMixer.h"
#include "SoundMixerInternal.h"
#include "SoundSource.h"
#include "SoundBackend.h"
#include "SoundDSP.h"
#include "SoundBus.h"
#include "SoundConfig.h"
#include "SoundEffect.h"
#include "SoundGeometryReflections.h"
#include "SoundOpeningGeometry.h"
#include "SoundTransmission.h"
#include "SoundAcousticCapture.h"

#include "../Sound.h"
#include "../SoundRender.h"
#include "../ai_sounds.h"
#include "../Plugins/ResonanceAudio.h"

#define ENGINE_API
#include "../xrEngine/xr_object.h"

#define DEFAULT_SLOT_COUNT (1024)
#define SND_MAX_PITCH (4)
#define SND_MAX_VELOCITY (100.0f)

using namespace XRay::Sound;
enum class ESoundMixerCommands : u16
{
	invalid,
	play,
	pause,
	stop,
	destroy,
	stop_all,
	pause_all,
	resume_all,
	update_parameter,
	set_volume,
	set_panning
};

struct SoundCommand
{
	u32 slot;
	ESoundMixerCommands id;
	u16 param0;
	u64 param1;
	u64 param2;
	u64 param3;
	shared_str string_storage;
	u32 bus = 0;
    u32 shot_group = 0;
    u32 shot_layer = 0;
    float shot_layer_gain = 1.0f;
};

struct AcousticRoomGeometry
{
    float indoor = 0.0f;
    float ceiling = 0.0f;
    float floor = 0.0f;
    float width = 0.0f;
    float length = 0.0f;
    float height = 0.0f;
    float volume = 0.0f;
    float openness = 1.0f;
    float window_proximity = 0.0f;
    float doorway_proximity = 0.0f;
    u32 room_id = 0;
    u32 sector = 0;
    u32 roof_hits = 0;
    u32 wall_hits = 0;
    u32 window_hits = 0;
    u32 door_openings = 0;
    u32 opening_count = 0;
    Fvector opening_position[8] = {};
    u32 opening_kind[8] = {}; // 1 = door; 2 = window
    u32 kind = 0;
    bool overhead = false;
};

struct AcousticShotMeter
{
	u64 Event = 0;
	u32 Group = 0;
	u32 SourceZone = 0;
	u32 ListenerZone = 0;
	string_path Sample = {};
	double InputEnergy = 0.0;
	double DryEnergy = 0.0;
	double FarSendEnergy = 0.0;
	double RoomSendEnergy = 0.0;
	u64 Samples = 0;
	float DryGain = 1.0f;
	float RoomReference = 0.0f;
};

static constexpr u32 PhysicalDiagnosticCapacity = 16;

struct AcousticSourceDiagnostic
{
	u64 TimestampNS = 0;
	u64 Event = 0;
	u32 Slot = 0;
	u32 Group = 0;
	u32 Layer = 0;
	u32 Kind = 0; // 1 creature, 2 NPC voice, 3 spatial primary shot (entity role unknown).
	u32 Frame = 0;
	u32 Bus = 0;
	u32 SourceRoom = 0;
	u32 ListenerRoom = 0;
	u32 Flags = 0;
	string_path Sample = {};
	Fvector Source = {};
	Fvector Listener = {};
	AcousticPathObservation Path;
	float Distance = 0.0f;
	float MinRange = 0.0f;
	float MaxRange = 0.0f;
	float SourceGain = 0.0f;
	float DistanceOnlyGain = 1.0f;
	float DistanceWithVoiceGain = 1.0f;
	u32 SpatialStages = 0;
	u32 OtherVoiceStages = 0;
	float LowPassCutoff = -1.0f;
	float LowPassBlend = 0.0f;
	float OcclusionGain = 1.0f;
	float ResidualVoiceGain = 1.0f;
	float DryBegin = 1.0f;
	float DryEnd = 1.0f;
	float EnvelopeBegin = 0.0f;
	float EnvelopeEnd = 0.0f;
	float BusBegin = 1.0f;
	float BusEnd = 1.0f;
	float Left = 1.0f;
	float Right = 1.0f;
	double DryInputEnergy = 0.0;
	double FaderEquivalentEnergy = 0.0;
	float DryInputPeak = 0.0f;
	float FaderEquivalentPeak = 0.0f;
	u32 Samples = 0;
};

struct sound_mixer_state
{
	xrSRWLock render_lock;
	xrSRWLock update_lock;
	xrSRWLock manage_lock;
	xrCriticalSection play_lock;

	float dt;
	float time_factor = 1.0f;
	float mute_volume = 1.0f;
	float compression = -1.0f;
	float shooting_reverb = -1.0f;
	u32 default_bus = 0;
	SoundEffectListener listener = {};
	Fvector occ[3]; // occluder triangle cache for get_occlusion()

	xr_vector<u32> free_slots;
	xr_vector<SoundCommand> cmd;
	xr_vector<sound_slot_state> slots;
	xr_hash_set<ref_sound*> sounds;

    float IndoorFactor = 0.0f;
    u32 ListenerZone = 0;
    AcousticRoomGeometry RoomGeometry = {};
	bool WetBusReady[SND_BUS_COUNT] = {};
	float WetBusReference[SND_BUS_COUNT] = {};
	float ListenerRoomWeights[SND_BUS_COUNT] = {};
	float DryMin = 1.0f;
	float WetMax = 0.0f;
	u32 Physical2DVoices = 0;
	u32 RoomTransitions = 0;
	u32 TransitionOldBus = 0;
	u32 TransitionNewBus = 0;
	u32 CallbackOverBudget = 0;
	u32 EarlyQueryPeak = 0;
	u32 CallbackDeadlineMisses = 0;
	u32 CallbackTotalPeakUS = 0;
	u32 SimulatedVoices = 0;
	AcousticShotMeter ShotMeters[2]; // One actor and one world primary per QA window.
	AcousticShotMeter VoiceMeter;
	// Fixed snapshots survive a source stopping or its slot being reused.
	AcousticSourceDiagnostic PhysicalDiagnostics[PhysicalDiagnosticCapacity];
	u32 PhysicalDiagnosticCount = 0;
	u32 PhysicalDiagnosticDropped = 0;
	u32 PhysicalDiagnosticKinds[3] = {};
	u32 PhysicalDiagnosticDroppedKinds[3] = {};

	float read_buffer[SND_CHANNEL_COUNT][(SND_BLOCKSIZE + 1) * 10];
};

static sound_mixer_state GMixer = {};
static XRay::Sound::GeometryReflections::Renderer g_NativeReflections;
static u64 g_AcousticVoiceSerial = 0;

// One shot is one logical acoustic emitter, even though all STCoP tracks
// remain independently streamable. These game-thread TLS values are copied
// into the queued commands; the audio thread never reads game TLS.
struct LiminalShotContext
{
    u32 group = 0;
    u32 layer = 0;
    float gain = 1.0f;
};
static thread_local LiminalShotContext g_ShotContext;
static std::atomic<u32> g_ShotSerial{1};

void Mixer::BeginShotGroup()
{
    g_ShotContext.group = g_ShotSerial.fetch_add(1, std::memory_order_relaxed);
    g_ShotContext.layer = 0;
    g_ShotContext.gain = 1.0f;
}
void Mixer::SetShotLayer(u32 index, float gain)
{
    if (!g_ShotContext.group)
        return;
    g_ShotContext.layer = index;
    g_ShotContext.gain = std::clamp(gain, 0.0f, 2.0f);
}
void Mixer::EndShotGroup()
{
    g_ShotContext = {};
}

// Measurements are owned by the render thread.
static u32 g_LiminalSyncDecodePeakUS = 0;
static u32 g_LiminalSyncDecodeCalls = 0;
static std::atomic<u32> g_LiminalMetricsVoices{0};
static std::atomic<u32> g_LiminalMetricsShots{0};
static std::atomic<u32> g_LiminalGroupedVoices{0};
static std::atomic<u32> g_LiminalGroupedPrimary{0};
static std::atomic<u32> g_LiminalGroupedSecondary{0};
static std::atomic<u32> g_LiminalActorShotVoices{0};
static std::atomic<u32> g_LiminalDistantShotVoices{0};
static std::atomic<u32> g_LiminalPortalRouted{0};
static std::atomic<u32> g_LiminalGlassRouted{0};
static std::atomic<u32> g_LiminalDynamicRouted{0};
static std::atomic<u32> g_LiminalRoutedFootsteps{0};
static std::atomic<u32> g_LiminalRoutedNpc{0};
static std::atomic<u32> g_LiminalRoutedCreatures{0};
static std::atomic<u32> g_LiminalRoutedGear{0};
static std::atomic<u32> g_LiminalRoutedImpacts{0};
static std::atomic<u32> g_LiminalRoutedActor{0};
static std::atomic<u32> g_LiminalRoutedAnomalies{0};
static std::atomic<u32> g_LiminalRoutedEnvironment{0};
static std::atomic<u32> g_LiminalRoutedUi{0};
static std::atomic<u32> g_LiminalRoutedShotDetails{0};
static std::atomic<u32> g_LiminalRoutedGeneric{0};
static std::atomic<u32> g_LiminalWorldStereoVoices{0};
static std::atomic<u32> g_LiminalOutdoorToRoomVoices{0};
static std::atomic<u32> g_LiminalBlockedOutdoorVoices{0};
static std::atomic<u32> g_LiminalLowestTransmission{1000};
static std::atomic<u32> g_LiminalMetricsTails{0};
static std::atomic<u32> g_LiminalMetricsSyncCalls{0};
static std::atomic<u32> g_LiminalMetricsSyncPeak{0};
static std::atomic<u32> g_LiminalMetricsCacheMiss{0};
static std::atomic<u32> g_LiminalMetricsMixerUS{0};
static std::atomic<u32> g_LiminalRenderPeakUS{0};
static std::atomic<u32> g_LiminalRenderLockPeakUS{0};
static std::atomic<u32> g_LiminalVoicePeakUS{0};
static std::atomic<u32> g_LiminalBusPeakUS{0};
static std::atomic<u32> g_LiminalMaxOcclusionUS{0};
static std::atomic<u32> g_LiminalMaxSourceReadUS{0};
static std::atomic<u32> g_LiminalMaxSpatializeUS{0};
// Render callback only, no atomic increment on every sound.
static u32 g_LiminalStageOccMax = 0;
static u32 g_LiminalStageReadMax = 0;
static u32 g_LiminalStageSpatialMax = 0;
static std::atomic<bool> g_LiminalMetricsReady{false};

static void Snd_GrowSlots(bool IsLockUpdate)
{
	xrSRWLockGuard Guard0(GMixer.render_lock, false);
	xrSRWLockGuard Guard1(GMixer.manage_lock, false);
	bool Locked = !g_SoundSourceLock.TryAcquireExclusive();

	if (IsLockUpdate)
	{
		GMixer.update_lock.AcquireExclusive();
	}

	size_t OldSize = GMixer.slots.size();
	size_t NewSize = std::max((size_t)DEFAULT_SLOT_COUNT, GMixer.slots.size() * 2);

	GMixer.slots.resize(NewSize);
	GMixer.free_slots.reserve(NewSize);

	g_SoundStats.possible_free_count += (NewSize - OldSize);
	for (size_t Iter = OldSize; Iter < NewSize; Iter++)
	{
		GMixer.free_slots.push_back(Iter + 1);
	}

	if (IsLockUpdate)
	{
		GMixer.update_lock.ReleaseExclusive();
	}

	if (!Locked)
	{
		g_SoundSourceLock.ReleaseExclusive();
	}
}

static void Snd_ReleaseVoiceEffects(sound_slot_state& Slot, SoundEffectOp Op)
{
	for (u32 EffectIdx = 0; EffectIdx < SND_VOICE_EFFECT_COUNT; EffectIdx++)
	{
		Snd_CallEffect(Slot.VoiceEffects[EffectIdx], Slot.VoiceEffectState[EffectIdx], Op, nullptr);
		if (Op == SoundEffectOp::Destroy)
		{
			Slot.VoiceEffects[EffectIdx] = 0;
		}
	}
}

static void Snd_ProcessVoiceEffects(sound_slot_state& Slot, const SoundBus* Bus, SoundEffectProcess* Process,
	AcousticSourceDiagnostic* Diagnostic = nullptr)
{
	for (u32 EffectIdx = 0; EffectIdx < SND_VOICE_EFFECT_COUNT; EffectIdx++)
	{
		const SoundBusEffect* Effect = EffectIdx < Bus->VoiceEffectCount ? &Bus->VoiceEffects[EffectIdx] : nullptr;
		u8 Id = Effect != nullptr ? Effect->Effect : 0;
		void* State = Slot.VoiceEffectState[EffectIdx];
		if (Slot.VoiceEffects[EffectIdx] != Id)
		{
			Snd_CallEffect(Slot.VoiceEffects[EffectIdx], State, SoundEffectOp::Destroy, nullptr);
			memset(State, 0, SND_VOICE_EFFECT_STATE_SIZE);

			SoundEffectCreate Create = {Effect != nullptr ? Effect->Params : nullptr, nullptr};
			Slot.VoiceEffects[EffectIdx] = Snd_CallEffect(Id, State, SoundEffectOp::Create, &Create) ? Id : 0;
		}

		if (Slot.VoiceEffects[EffectIdx] != 0)
		{
			Process->Params = Effect->Params;
			if (Diagnostic != nullptr)
			{
				const SoundEffectEntry* Entry = Snd_GetEffect(Id);
				if (Entry != nullptr && (xr_strcmp(Entry->Name.c_str(), "spatial") == 0 ||
					xr_strcmp(Entry->Name.c_str(), "spatialize") == 0))
				{
					float Rolloff = 1.0f;
					for (u32 Param = 0; Param < Entry->Desc.ParamCount; ++Param)
					{
						if (xr_strcmp(Entry->Desc.Params[Param].Name, "rolloff") == 0)
						{
							Rolloff = Effect->Params[Param];
						}
					}
					SoundSpatialPosition Position = {};
					Position.MinDistance = std::max(Process->Voice->Distances.x, EPS_S);
					Position.MaxDistance = std::max(Process->Voice->Distances.y, Position.MinDistance + EPS_S);
					Position.Distance = std::clamp(Process->Listener->Position.distance_to(Process->Voice->Position),
						Position.MinDistance, Position.MaxDistance);
					Position.Gain = Process->Voice->Gain;
					Diagnostic->DistanceWithVoiceGain *= Snd_DistanceAttenuation(&Position, Rolloff);
					Position.Gain = 1.0f;
					Diagnostic->DistanceOnlyGain *= Snd_DistanceAttenuation(&Position, Rolloff);
					++Diagnostic->SpatialStages;
				}
				else
				{
					++Diagnostic->OtherVoiceStages;
				}
			}
			Snd_CallEffect(Id, State, SoundEffectOp::Process, Process);
		}
	}
}

void MixerNewState(u32 Slot, Mixer::State State)
{
	if (Slot == 0)
	{
		return;
	}

    auto& VoiceSlot = GMixer.slots[Slot - 1];
    // Reclaim expensive high-quality binaural instances as soon as a voice
    // finishes or pauses. A stopped slot can remain referenced by a weapon;
    // without this release, all 32 shooting HRTFs remain occupied forever.
    if (VoiceSlot.state != State &&
        (State == Mixer::State::Stopped || State == Mixer::State::Paused))
    {
        Snd_ReleaseVoiceEffects(VoiceSlot, SoundEffectOp::Reset);
    }
    VoiceSlot.prev_state = VoiceSlot.state;
    VoiceSlot.state = State;
    VoiceSlot.fake_state = State;
}

enum class ESlotOcclusionResult
{
	False,
	True,
	SOM
};

ICF ESlotOcclusionResult Snd_SlotOcclusion(u32 slot_idx, const SoundSourceState& source, float dt, float* occ_volume)
{
	PROF_EVENT("Sound: SlotOcclusion");
	auto& slot = GMixer.slots[slot_idx - 1];
	if (slot.state != Mixer::State::Playing)
	{
		return ESlotOcclusionResult::False;
	}

	ESlotOcclusionResult Result = ESlotOcclusionResult::True;

	if (slot.flags & (u32)Mixer::Flags::Spatial)
	{
		// Check range
		Fvector& pos = slot.parameters[(u32)Mixer::ParameterId::Position];
		Fvector& distances = slot.parameters[(u32)Mixer::ParameterId::DistanceRange];
		float dist = GMixer.listener.Position.distance_to(pos);
		if (dist > distances.y)
		{
			if (occ_volume)
			{
				*occ_volume = 0.0f;
			}
			return ESlotOcclusionResult::False;
		}

        // The per-source update determines occlusion. Never ray-cast SOM
        // again for every DSP audio block (and then multiply its gain).
        if (occ_volume != nullptr)
            *occ_volume = 1.0f;
	}

	return Result;
}

ICF u32 Snd_ReadSlotData(u32 SlotIdx, SoundSourceState& Source, float** Data, u32 FramesCount)
{
	auto& Slot = GMixer.slots[SlotIdx - 1];

	u32 ReadPostion = Slot.position;
	u32 Frames2Read = FramesCount;
	u32 DecodeAttempts = 0;

	while (Frames2Read && ReadPostion < Source.Desc.frames_total)
	{
		float* Dest[SND_CHANNEL_COUNT];
		for (size_t Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
		{
			Dest[Channel] = &Data[Channel][FramesCount - Frames2Read];
		}

		u32 CacheFrames = Snd_CopyCached(&Source, ReadPostion, Dest, Frames2Read);
		if (CacheFrames != 0)
		{
			Frames2Read -= CacheFrames;
			ReadPostion += CacheFrames;
			DecodeAttempts = 0;
			continue;
		}

		// Cache miss: the decode thread didn't make it in time (or never got the request).
		// Don't wait for it, decode the line right here on the render thread.
		SND_STAT_ADD(g_SoundStats.render_cache_miss, 1u);
        if (++DecodeAttempts > 2)
            break;
        const u64 decode_start = Snd_GetTimestamp();
        const bool decoded = Snd_DecodeNow(&Source, ReadPostion);
        const u32 decode_us = (u32)((Snd_GetTimestamp() - decode_start) / 1000);
        g_LiminalSyncDecodePeakUS = std::max(g_LiminalSyncDecodePeakUS, decode_us);
        ++g_LiminalSyncDecodeCalls;
        if (!decoded)
            break;
	}

	return FramesCount - Frames2Read;
}


ICF void Snd_ReadSlot(u32 slot_idx, SoundSourceState& source, float** data, u32 frames_count)
{
	auto& slot = GMixer.slots[slot_idx - 1];
	if (source.Desc.frames_total == 0)
	{
		MixerNewState(slot_idx, Mixer::State::Stopped);
		return;
	}

	u32 last_frames = frames_count;
	while (last_frames)
	{
		float* offfseted_data[SND_CHANNEL_COUNT];
		u32 buf_offset = (frames_count - last_frames);
		for (size_t i = 0; i < SND_CHANNEL_COUNT; i++)
		{
			offfseted_data[i] = &data[i][buf_offset];
		}

		u32 read_frames = Snd_ReadSlotData(slot_idx, source, offfseted_data, last_frames);

		last_frames -= read_frames;
		slot.position = std::min(slot.position + read_frames, source.Desc.frames_total);

		if (slot.position < source.Desc.frames_total)
		{
			if (read_frames != 0)
			{
				continue;
			}

			// Nothing decodable from here on: treat it as the end of the stream (loop or stop)
			if (slot.position == 0)
			{
				MixerNewState(slot_idx, Mixer::State::Stopped);
				break;
			}
		}

		slot.position = 0;
		if ((slot.flags & (u32)Mixer::Flags::Looped) == 0)
		{
			MixerNewState(slot_idx, Mixer::State::Stopped);
			break;
		}
	}
}

ICF void Snd_ProcessSlot(u32 slot_idx, SoundSourceState& source, float** data)
{
	auto& slot = GMixer.slots[slot_idx - 1];
	float pitch = slot.parameters[(u32)Mixer::ParameterId::Pitch].x;

	u32 output_frames = SND_BLOCKSIZE;
	float ratio = std::clamp(pitch * slot.doppler * GMixer.time_factor, 0.0f, (float)SND_MAX_PITCH);
	u32 input_frames = std::max((u32)((float)output_frames * ratio), 1u);

	bool is_music = (slot.flags & (u16)Mixer::Flags::Intro);

	for (size_t Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
	{
		memset(GMixer.read_buffer[Channel], 0, (input_frames + 1) * sizeof(float));
	}

	if (is_music || fis_zero(1.0f - ratio))
	{
		Snd_ReadSlot(slot_idx, source, data, SND_BLOCKSIZE);
	}
	else
	{
		float* offfseted_data[SND_CHANNEL_COUNT];
		for (size_t Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
		{
			offfseted_data[Channel] = GMixer.read_buffer[Channel];
		}

		Snd_ReadSlot(slot_idx, source, offfseted_data, input_frames + 1);
		if (slot.position > 0)
		{
			slot.position -= 1; // account 1 sample of tail for interpolation
		}

		DSP_ResampleBuffer(offfseted_data, data, slot.history, input_frames, output_frames);
	}
}

ICF void Snd_PrecacheRenderCallback()
{
	PROF_EVENT("Sound: Precache Stage");

	static u64 counter = 0;
	static u64 timestamp = Snd_GetTimestamp();

	{
		xrSRWLockGuard g1(GMixer.manage_lock);
		float dt = (float)((double)(Snd_GetTimestamp() - timestamp) / 1000000000.0);

		g_SoundStats.frame_time_micros = (Snd_GetTimestamp() - timestamp) / 1000;
		timestamp = Snd_GetTimestamp();

		if (counter % 100 == 0)
		{
			g_SoundStats.cache_hit_count = 0;
			g_SoundStats.cache_miss_count = 0;
		}

		for (size_t i = 0; i < GMixer.slots.size(); i++)
		{
			PROF_EVENT("Sound: GMixer.slot");
			auto& slot = GMixer.slots[i];

			if (slot.state == Mixer::State::Delay)
			{
				slot.delay -= dt;
				if (slot.delay <= 0.f)
				{
					slot.AcousticStartPending = true;
					MixerNewState(i + 1, Mixer::State::Playing);
				}
			}
			if (slot.state != Mixer::State::Playing)
			{
				continue;
			}

			SoundSourceState* source = Snd_FindSource(&slot.sound_name);
			if (source != nullptr && Snd_SlotOcclusion(i + 1, *source, dt, nullptr) != ESlotOcclusionResult::False)
			{
				// Async decode of the current position and the read-ahead window.
				// Whatever doesn't make it in time is decoded on the fly by the render thread.
				PROF_EVENT("Sound: Prefetch");
				Snd_PrefetchSource(source, &slot.sound_name, slot.position, (slot.flags & (u8)Mixer::Flags::Looped) != 0);
			}

			if (source != nullptr)
			{
				PROF_EVENT("Sound: ReleaseSound");
				Snd_ReleaseSource(&slot.sound_name);
			}


		}

		g_SoundStats.precache_time_micros = (Snd_GetTimestamp() - timestamp) / 1000;
		counter++;
	}
}

ICF Fvector Snd_Velocity(const Fvector& From, const Fvector& To)
{
	Fvector Out;
	Out.set(0.0f, 0.0f, 0.0f);

	if (GMixer.dt > EPS_S)
	{
		Out.sub(To, From).mul(1.0f / GMixer.dt);
	}

	if (Out.square_magnitude() > SND_MAX_VELOCITY * SND_MAX_VELOCITY)
	{
		Out.set(0.0f, 0.0f, 0.0f);
	}

	return Out;
}

// Local acoustic geometry samples. The world-space rays are performed ONLY on
// the game/update thread. Each sample is shared across all voices within a
// quantized 0.75m cell and reused for ~350ms. The DSP thread uses floats only.
static constexpr u32 kRoomRaySectors = 16;
struct AcousticRayHit
{
    bool hit = false;
    float distance = 0.0f;
    u32 sector = 0;
    SoundAcousticMaterial material = {};
};

static AcousticRayHit Snd_TraceAcousticGeometry(
    CDB::MODEL* world, CDB::COLLIDER* collider, const Fvector& from,
    const Fvector& direction, float range)
{
    AcousticRayHit hit;
    hit.distance = range;
    if (!world || !collider)
        return hit;
    // One query still counts as one probe. A bush in front of a wall must
    // not hide the actual structural hit, and decor/liquid cannot be a roof.
    collider->ray_options(0);
    collider->ray_query(world, from, direction, range);
    for (u32 Index = 0; Index < collider->r_count(); ++Index)
    {
        const auto& Face = collider->r_begin()[Index];
        SoundAcousticMaterial Material;
        if (!::Sound->AcousticMaterialCallback.empty())
        {
            Material = ::Sound->AcousticMaterialCallback((u16)Face.material);
        }
        if ((Material.flags & 5u) != 0u || (hit.hit && Face.range >= hit.distance))
        {
            continue;
        }
        hit.hit = true;
        hit.distance = std::clamp(Face.range, 0.0f, range);
        hit.sector = Face.sector;
        hit.material = Material;
    }
    return hit;
}

struct AcousticProbeCacheEntry
{
    CDB::MODEL* world = nullptr;
    s32 x = 0, y = 0, z = 0;
    u64 expiry = 0;
    bool detailed = false;
    AcousticRoomGeometry sample = {};
};

static AcousticProbeCacheEntry g_AcousticProbeCache[512] = {};

static float Snd_HemiIndoorFactor(const Fvector& Pos, u32* RoofHitsOut = nullptr,
    u32* WallHitsOut = nullptr, bool* ZenithRoofOut = nullptr,
    u32* RoomKindOut = nullptr, AcousticRoomGeometry* RoomOut = nullptr)
{
    PROF_EVENT("Sound: Directional Room Geometry");
    if (RoofHitsOut) *RoofHitsOut = 0;
    if (WallHitsOut) *WallHitsOut = 0;
    if (ZenithRoofOut) *ZenithRoofOut = false;
    if (RoomKindOut) *RoomKindOut = 0;
    if (RoomOut) *RoomOut = {};

    CDB::MODEL* model = ::Sound->get_geometry_occ();
    CDB::COLLIDER* collider = ::Sound->get_geometry_db();
    if (!model || !collider)
        return 0.0f;

    const bool detailed = RoomKindOut != nullptr || RoomOut != nullptr;
    const s32 cx = (s32)std::floor(Pos.x * 1.3333333f);
    const s32 cy = (s32)std::floor(Pos.y * 1.3333333f);
    const s32 cz = (s32)std::floor(Pos.z * 1.3333333f);
    const u32 hash = ((u32)cx * 73856093u) ^
        ((u32)cy * 19349663u) ^ ((u32)cz * 83492791u);
    AcousticProbeCacheEntry& cache = g_AcousticProbeCache[hash & 511u];
    const u64 now = Snd_GetTimestamp();
    if (cache.world == model && cache.x == cx && cache.y == cy &&
        cache.z == cz && now < cache.expiry &&
        (cache.detailed || !detailed))
    {
        const auto& room = cache.sample;
        if (RoofHitsOut) *RoofHitsOut = room.roof_hits;
        if (WallHitsOut) *WallHitsOut = room.wall_hits;
        if (ZenithRoofOut) *ZenithRoofOut = room.overhead;
        if (RoomKindOut) *RoomKindOut = room.kind;
        if (RoomOut) *RoomOut = room;
        return room.indoor;
    }

    AcousticRoomGeometry room;
    auto addOpening = [&](const Fvector& v, u32 kind)
    {
        for (u32 i = 0; i < room.opening_count; ++i)
            if (room.opening_position[i].distance_to_sqr(v) < 1.5f)
                return;
        if (room.opening_count < 8)
        {
            room.opening_position[room.opening_count] = v;
            room.opening_kind[room.opening_count] = kind;
            ++room.opening_count;
        }
    };
    constexpr float roofRange = 24.0f;
    constexpr float wallRange = 23.0f;
    // Unlike the previous five tilted rays, samples directly above a
    // two-dimensional footprint don't wander outside a narrow room.
    const Fvector roofOffset[] = {
        {0,0,0}, {0.45f,0,0}, {-0.45f,0,0},
        {0,0,0.45f}, {0,0,-0.45f},
        {0.9f,0,0.9f}, {-0.9f,0,0.9f},
        {0.9f,0,-0.9f}, {-0.9f,0,-0.9f}
    };
    const Fvector up = {0,1,0}, down = {0,-1,0};
    const u32 roofCount = detailed ? 9u : 5u;
    float ceilingSum = 0.0f;
    for (u32 i = 0; i < roofCount; ++i)
    {
        Fvector from; from.add(Pos, roofOffset[i]);
        auto roof = Snd_TraceAcousticGeometry(model, collider, from, up, roofRange);
        if (!roof.hit || (roof.material.flags & 5u))
            continue;
        ++room.roof_hits;
        if (i == 0)
        {
            room.overhead = true;
            room.ceiling = roof.distance;
            room.sector = roof.sector;
        }
        ceilingSum += roof.distance;
    }
    // A high roof with skylights is still a covered space: a first-person
    // listener may not be directly beneath a solid center ceiling.
    if (room.roof_hits > 0 && !room.overhead)
        room.ceiling = ceilingSum / room.roof_hits;

    const auto floorHit = Snd_TraceAcousticGeometry(model, collider, Pos, down, 3.8f);
    room.floor = floorHit.hit ? floorHit.distance : 1.6f;
    if (floorHit.hit)
    {
        room.sector = floorHit.sector;
    }

    float midDistance[kRoomRaySectors] = {};
    float lowDistance[kRoomRaySectors] = {};
    bool midSolid[kRoomRaySectors] = {};
    bool lowSolid[kRoomRaySectors] = {};
    bool midGlass[kRoomRaySectors] = {};
    const u32 directions = detailed ? kRoomRaySectors : 8u;
    const u32 stride = kRoomRaySectors / directions;
    float openWeighted = 0.0f;
    u32 enclosureHits = 0, nearWalls = 0;
    float openingRangeSum = 0.0f;
    const float horizontalLimit = detailed ? wallRange : 14.0f;
    for (u32 step = 0; step < directions; ++step)
    {
        const u32 index = step * stride;
        const float a = 2.0f * PI * (float)index / (float)kRoomRaySectors;
        const Fvector dir = {std::cos(a), 0.0f, std::sin(a)};
        const auto mid = Snd_TraceAcousticGeometry(model, collider, Pos, dir, horizontalLimit);
        const bool solid = mid.hit && !(mid.material.flags & 5u);
        midSolid[index] = solid;
        midDistance[index] = mid.distance;
        midGlass[index] = solid && (mid.material.flags & 2u) != 0u;
        if (solid)
        {
            ++room.wall_hits;
            ++enclosureHits;
            if (mid.distance < 8.0f) ++nearWalls;
            openWeighted += mid.material.transmission;
            if (midGlass[index] && mid.distance < 8.0f)
            {
                ++room.window_hits;
                room.window_proximity = std::max(room.window_proximity,
                    std::clamp(1.0f - mid.distance / 8.0f, 0.0f, 1.0f));
                if (detailed)
                {
                    Fvector v = Pos; v.mad(dir, mid.distance);
                    addOpening(v, 2u);
                }
            }

        }
        else openWeighted += 1.0f;

        if (!detailed)
            continue;
        // At door height and below a typical window sill, openings differ.
        // A window can be open at ear level yet have a solid lower wall;
        // an open doorway remains clear at both heights.
        Fvector lowFrom = Pos;
        lowFrom.y -= 0.82f;
        const auto low = Snd_TraceAcousticGeometry(
            model, collider, lowFrom, dir, horizontalLimit);
        lowSolid[index] = low.hit && !(low.material.flags & 5u);
        lowDistance[index] = low.distance;
        if (lowSolid[index] && !solid && low.distance < 5.0f)
        {
            ++room.window_hits;
            room.window_proximity = std::max(room.window_proximity,
                std::clamp(1.0f - low.distance / 6.0f, 0.0f, 1.0f));
            Fvector v = Pos; v.mad(dir, low.distance);
            addOpening(v, 2u);
        }
    }

    // Portal apertures: look for angular gaps bracketed by actual nearby
    // walls at the listener's two body heights. No "door" is conjured where
    // an entire fa?ade is open, and transparent glass remains a window.
    if (detailed)
    {
        for (u32 i = 0; i < kRoomRaySectors; ++i)
        {
            const u32 left = (i + kRoomRaySectors - 1) % kRoomRaySectors;
            const u32 right = (i + 1) % kRoomRaySectors;
            const bool midClear = !midSolid[i] || midDistance[i] > 9.0f;
            const bool lowClear = !lowSolid[i] || lowDistance[i] > 9.0f;
            const bool sides = (midSolid[left] && midDistance[left] < 7.0f) &&
                               (midSolid[right] && midDistance[right] < 7.0f);
            if (midClear && lowClear && sides && room.roof_hits >= 3)
            {
                ++room.door_openings;
                const float nearness = std::min(midDistance[left], midDistance[right]);
                openingRangeSum += nearness;
                room.doorway_proximity = std::max(room.doorway_proximity,
                    std::clamp(1.0f - nearness / 8.0f, 0.0f, 1.0f));
                const float angle = 2.0f * PI * (float)i / (float)kRoomRaySectors;
                const Fvector dir = {std::cos(angle), 0.0f, std::sin(angle)};
                Fvector v = Pos; v.mad(dir, nearness);
                addOpening(v, 1u);
            }
        }
    }

    const float roofFraction = (float)room.roof_hits / (float)roofCount;
    const float wallFraction = (float)enclosureHits / (float)directions;
    room.openness = std::clamp(openWeighted / (float)directions, 0.0f, 1.0f);
    if (room.roof_hits < (detailed ? 2u : 1u) ||
        (room.roof_hits < roofCount / 2u && room.wall_hits < directions / 2u))
    {
        room.indoor = 0.0f;
        room.kind = 0;
    }
    else
    {
        // Roof cover + broad directional enclosure; partial skylights,
        // windows and doorways reduce reverberation but don't erase a room.
        const float cover = std::min(1.0f, roofFraction * 1.35f);
        room.indoor = std::clamp(cover * (0.64f + 0.36f *
            (1.0f - room.openness * 0.70f)), 0.0f, 1.0f);
        if (roofFraction > 0.68f && wallFraction > 0.4f)
            room.indoor = std::max(room.indoor, 0.85f);
    }

    // Local probe spans describe apertures only. Logical room identity and
    // late decay use validated catalog membership, volume and union surfaces.
    if (detailed)
    {
        auto side = [&](u32 i)->float {
            return midSolid[i] ? midDistance[i] : wallRange;
        };
        const float xPlus = side(0), xMinus = side(8);
        const float zPlus = side(4), zMinus = side(12);
        room.width = std::clamp(xPlus + xMinus, 1.0f, 46.0f);
        room.length = std::clamp(zPlus + zMinus, 1.0f, 46.0f);
        room.height = std::clamp(room.ceiling + room.floor, 1.7f, 25.0f);
        room.volume = room.width * room.length * room.height;
        const bool corridor =
            (room.width > room.length * 2.0f || room.length > room.width * 2.0f) &&
            room.roof_hits >= 3;
        room.kind = room.indoor < 0.22f ? 0u :
            corridor ? 3u : room.volume > 500.0f ? 2u : 1u;
        room.room_id = Snd_GetZoneRoom(Snd_FindZone(Pos));
    }
    else
    {
        room.kind = room.indoor > 0.55f ? 1u : 0u;
    }

    cache.world = model;
    cache.x = cx; cache.y = cy; cache.z = cz;
    cache.detailed = detailed;
    cache.expiry = now + (detailed ? 320000000ULL : 420000000ULL);
    cache.sample = room;
    if (RoofHitsOut) *RoofHitsOut = room.roof_hits;
    if (WallHitsOut) *WallHitsOut = room.wall_hits;
    if (ZenithRoofOut) *ZenithRoofOut = room.overhead;
    if (RoomKindOut) *RoomKindOut = room.kind;
    if (RoomOut) *RoomOut = room;
    return room.indoor;
}

// Transmission between two actual acoustic locations, not between abstract
// "indoor"/"outdoor" labels. Every point belongs to its own cached local
// geometry snapshot; this path test detects solid surfaces, glass, open
// apertures and dynamic obstacles such as movable doors.
struct AcousticWallPathEntry
{
    CDB::MODEL* world = nullptr;
    s32 lx = 0, ly = 0, lz = 0, sx = 0, sy = 0, sz = 0;
    u64 expiry = 0;
    u64 frame = 0;
    float obstruction = 0.0f;
    float portal = 0.0f;
    float glass = 0.0f;
    float dynamic = 0.0f;
    u32 portal_type = 0; // 1 = lower opening, 2 = upper/window, 3 = side
	AcousticPathObservation Diagnostic;
};
static AcousticWallPathEntry g_AcousticWallPaths[512] = {};
static u64 g_AcousticPathFrame = 0;
// Per-update ray budgets prevent a burst of 100+ multi-layer NPC sounds
// from making the GAME update thread wait for thousands of collision rays.
static u32 g_AcousticPortalRayBudget = 0;
static u32 g_AcousticDirectRayBudget = 0;
static u32 g_AcousticDynamicRayBudget = 0;
static u32 g_AcousticDetailProbeBudget = 0;

static float Snd_TransmissionRay(CDB::MODEL* model, CDB::COLLIDER* collider,
    const Fvector& start, const Fvector& end, float* FirstSolidHit = nullptr,
    float* GlassContribution = nullptr, AcousticPathObservation* Diagnostic = nullptr)
{
    if (FirstSolidHit) *FirstSolidHit = -1.0f;
    if (GlassContribution) *GlassContribution = 0.0f;
    Fvector ray;
    ray.sub(end, start);
    const float length = ray.magnitude();
    if (length <= EPS_S)
    {
        return 1.0f;
    }
    ray.div(length);
    collider->ray_options(0);
    collider->ray_query(model, start, ray, length);
	if (Diagnostic != nullptr)
	{
		Diagnostic->CenterRayQueried = true;
	}
    auto* Begin = collider->r_begin();
    if (collider->r_count() > 1)
    {
        std::sort(Begin, Begin + collider->r_count(), [](const CDB::RESULT& A, const CDB::RESULT& B) { return A.range < B.range; });
    }
    SoundTransmissionAccumulator Accumulator;
    for (u32 Index = 0; Index < collider->r_count(); ++Index)
    {
        const auto& Face = Begin[Index];
        SoundAcousticMaterial Material;
        if (!::Sound->AcousticMaterialCallback.empty())
            Material = ::Sound->AcousticMaterialCallback((u16)Face.material);
        if (Diagnostic != nullptr && !Diagnostic->MaterialHit)
        {
            Diagnostic->MaterialHit = true;
            Diagnostic->MaterialIndex = u32(Face.material);
            Diagnostic->MaterialDistance = Face.range;
            Diagnostic->MaterialFromCallback = !::Sound->AcousticMaterialCallback.empty();
            if (Diagnostic->MaterialFromCallback)
            {
                Diagnostic->MaterialFlags = Material.flags;
                Diagnostic->MaterialTransmission = Material.transmission;
            }
        }
        if (Diagnostic != nullptr)
        {
            Diagnostic->GlassEncountered |= (Material.flags & 2u) != 0u;
            Diagnostic->MaterialFallbackUsed |= ::Sound->AcousticMaterialCallback.empty();
            Diagnostic->BoundaryEncountered |= (Material.flags & 5u) == 0u;
            if (Material.Class == SoundAcousticClass::Foliage) ++Diagnostic->FoliageCrossings;
        }
        if (GlassContribution && (Material.flags & 2u) != 0u)
            *GlassContribution = std::max(*GlassContribution, 1.0f - Material.transmission);
        if ((Material.flags & 7u) == 0u && FirstSolidHit && *FirstSolidHit < 0.0f)
            *FirstSolidHit = Face.range;
        Accumulator.Push(Face, Material);
    }
    const float Peak = Accumulator.Peak();
    if (Diagnostic != nullptr)
    {
        Diagnostic->LowTransmissionRatio = Peak > 0.0f ? Accumulator.Total[0] / Peak : 0.0f;
        Diagnostic->MidTransmissionRatio = Peak > 0.0f ? Accumulator.Total[1] / Peak : 0.0f;
        Diagnostic->HighTransmissionRatio = Peak > 0.0f ? Accumulator.Total[2] / Peak : 0.0f;
        Diagnostic->MaterialCrossings = Accumulator.Crossings;
    }
    return Peak;
}

static float Snd_WallObstructionBetween(const Fvector& listener,
    const Fvector& emitter, float* PortalOut = nullptr,
    float* GlassOut = nullptr, float* DynamicOut = nullptr,
	AcousticPathObservation* DiagnosticOut = nullptr)
{
	if (DiagnosticOut != nullptr) *DiagnosticOut = {};
    if (PortalOut) *PortalOut = 0.0f;
    if (GlassOut) *GlassOut = 0.0f;
    if (DynamicOut) *DynamicOut = 0.0f;
    CDB::MODEL* model = ::Sound->get_geometry_occ();
    CDB::COLLIDER* collider = ::Sound->get_geometry_db();
    if (!model || !collider)
        return 0.0f;

    Fvector direction; direction.sub(emitter, listener);
    const float distance = direction.magnitude();
    if (distance <= EPS_S)
        return 0.0f;
    direction.div(distance);
    const s32 lx = (s32)std::floor(listener.x * 1.5f);
    const s32 ly = (s32)std::floor(listener.y * 1.5f);
    const s32 lz = (s32)std::floor(listener.z * 1.5f);
    const s32 sx = (s32)std::floor(emitter.x * 1.5f);
    const s32 sy = (s32)std::floor(emitter.y * 1.5f);
    const s32 sz = (s32)std::floor(emitter.z * 1.5f);
    const u32 hash = ((u32)lx * 73856093u) ^ ((u32)ly * 19349663u) ^
        ((u32)lz * 83492791u) ^ ((u32)sx * 2654435761u) ^
        ((u32)sy * 3221225473u) ^ ((u32)sz * 3266489917u);
    auto& cached = g_AcousticWallPaths[hash & 511u];
    const u64 now = Snd_GetTimestamp();
    if (cached.world == model && cached.lx == lx && cached.ly == ly &&
        cached.lz == lz && cached.sx == sx && cached.sy == sy &&
        cached.sz == sz && (now < cached.expiry || cached.frame == g_AcousticPathFrame))
    {
        if (PortalOut) *PortalOut = cached.portal;
        if (GlassOut) *GlassOut = cached.glass;
        if (DynamicOut) *DynamicOut = cached.dynamic;
		if (DiagnosticOut != nullptr)
		{
			*DiagnosticOut = cached.Diagnostic;
			DiagnosticOut->FromCache = true;
		}
        return cached.obstruction;
    }

    if (g_AcousticDirectRayBudget < 7u ||
        (!::Sound->AcousticDynamicRayCallback.empty() && g_AcousticDynamicRayBudget == 0))
    {
        // Defer an untested path; never invent an open portal on budget exhaustion.
        // A previous matching path remains valid until its scheduled refresh.
        if (cached.world == model && now < cached.expiry && cached.lx == lx && cached.ly == ly &&
            cached.lz == lz && cached.sx == sx && cached.sy == sy && cached.sz == sz)
        {
            if (PortalOut) *PortalOut = cached.portal;
            if (GlassOut) *GlassOut = cached.glass;
            if (DynamicOut) *DynamicOut = cached.dynamic;
			if (DiagnosticOut != nullptr)
			{
				*DiagnosticOut = cached.Diagnostic;
				DiagnosticOut->FromCache = true;
				DiagnosticOut->Deferred = true;
			}
            return cached.obstruction;
        }
		if (DiagnosticOut != nullptr) DiagnosticOut->Deferred = true;
        return 0.975f;
    }
	AcousticPathObservation PathDiagnostic;
	PathDiagnostic.TimestampNS = now;
	PathDiagnostic.Listener = listener;
	PathDiagnostic.Source = emitter;
    g_AcousticDirectRayBudget -= 7u;
    Fvector side;
    side.set(-direction.z, 0, direction.x);
    side.normalize_safe();
    if (side.square_magnitude() < 0.01f)
        side.set(1,0,0);
    const Fvector up = {0,1,0};
    static const float horizontalOffsets[] = {0, 0.40f, -0.40f, 0, 0, 0.90f, -0.90f};
    static const float verticalOffsets[] = {0, 0, 0, 0.60f, -0.60f, 0.28f, 0.28f};
    constexpr float weights[] = {0.34f,0.12f,0.12f,0.14f,0.14f,0.07f,0.07f};
    float weightedDirect = 0.0f, weightedLow = 0.0f, weightedMid = 0.0f, weightedHigh = 0.0f, largestGlass = 0.0f;
    float firstSolid = -1.0f;
    float centerTransmission = 1.0f;
    for (u32 ray = 0; ray < 7; ++ray)
    {
        Fvector from = listener, to = emitter;
        Fvector offset = side;
        offset.mul(horizontalOffsets[ray]);
        from.add(offset);to.add(offset);
        from.y += verticalOffsets[ray];
        to.y += verticalOffsets[ray];
        float glass = 0.0f, hit = -1.0f;
        AcousticPathObservation Sample;
        const float trans = Snd_TransmissionRay(model, collider, from, to,
            ray == 0 ? &hit : nullptr, &glass, &Sample);
        if (ray == 0)
        {
            const auto Stamp = PathDiagnostic.TimestampNS;
            PathDiagnostic = Sample;
            PathDiagnostic.TimestampNS = Stamp;
            PathDiagnostic.Listener = listener;
            PathDiagnostic.Source = emitter;
            firstSolid = hit;
            centerTransmission = trans;
        }
        largestGlass = std::max(largestGlass, glass);
        weightedDirect += weights[ray] * trans;
        weightedLow += weights[ray] * trans * Sample.LowTransmissionRatio;
        weightedMid += weights[ray] * trans * Sample.MidTransmissionRatio;
        weightedHigh += weights[ray] * trans * Sample.HighTransmissionRatio;
    }
    const float DirectLowRatio = weightedLow / std::max(weightedDirect, 1e-20f);
    const float DirectMidRatio = weightedMid / std::max(weightedDirect, 1e-20f);
    const float DirectHighRatio = weightedHigh / std::max(weightedDirect, 1e-20f);

    // SOM contributes to world sounds ONLY when CFORM found no solid
    // obstruction on the central ray. An already detected CFORM wall is
    // never multiplied a second time by its duplicate SOM polygon.
    // Divide by the existing central CFORM transmission so glass/vegetation
    // are not penalized twice. The fused result is still applied once
    // via BarrierGain, after the native aperture and dynamic-door checks.
    if (firstSolid < 0.0f && weightedDirect > 0.80f &&
        ::Sound->get_geometry_som() != nullptr)
    {
        const float combined = ::Sound->get_occlusion_to(listener, emitter, 0.0f);
        const float somSupplement = std::clamp(
            combined / std::max(centerTransmission, 0.025f), 0.025f, 1.0f);
        weightedDirect *= somSupplement;
    }

    // The dynamic CFORM check is separate: an OPEN door moves out of the
    // path, a CLOSED door is an actual colliding object. Query it on the
    // game thread and never in the audio callback. The per-frame cap keeps
    // dense automatic fire from monopolizing physics collision work.
    bool PathComplete = true;
    float dynamicBlocked = 0.0f;
    // A fresh path must establish its direct door state. The detail budget
    // limits additional portal legs, never turns an untested door into air.
    if (!::Sound->AcousticDynamicRayCallback.empty())
    {
        if (g_AcousticDynamicRayBudget != 0)
        {
            --g_AcousticDynamicRayBudget;
        }
        dynamicBlocked = ::Sound->AcousticDynamicRayCallback(listener, emitter);
		PathDiagnostic.DynamicQueried = true;
    }
    const float direct = weightedDirect * (1.0f - 0.78f * dynamicBlocked);
    const bool NeedsPortalSearch = weightedDirect < 1.0f - EPS_S || dynamicBlocked > EPS_S;

    // A pair of clear line segments proves visibility, not a finite opening.
    // Only catalog supports with known area/frame and opposite endpoint sides
    // may replace wall transmission. Unknown local probe points remain probes.
    float portal = 0.0f, portalDistance = distance;
    u32 portalType = 0;
	u32 PortalId = 0;
	bool PortalGlass = false, PortalMaterialsKnown = false;
	float PortalLegAGain = -1.0f, PortalLegBGain = -1.0f;
	float PortalLowRatio = 1.0f, PortalMidRatio = 1.0f, PortalHighRatio = 1.0f;
    if (NeedsPortalSearch && distance < 85.0f)
    {
        SoundRoomPortal Candidates[8] = {};
        const u32 Count = Snd_FindRoomPortals(Snd_GetZoneRoom(Snd_FindZone(emitter)),
            Snd_GetZoneRoom(Snd_FindZone(listener)), listener, Candidates, 8u);
        for (u32 Index = 0; Index < Count; ++Index)
        {
            const auto& Opening = Candidates[Index];
            if (!SoundOpeningGeometry::MatchesEndpointSides(Opening, emitter, listener,
                Snd_GetZoneRoom(Snd_FindZone(emitter)), Snd_GetZoneRoom(Snd_FindZone(listener))))
            {
                ++PathDiagnostic.PortalCandidatesRejected;
                continue;
            }
            // Center plus the exact two native return-source points share the
            // same static/dynamic path contract. The catalog supplies finite
            // support; these visibility samples do not invent its dimensions.
            if (g_AcousticPortalRayBudget < 6u || g_AcousticDynamicRayBudget < 6u)
            {
                PathComplete = false;
                break;
            }
            if (::Sound->AcousticDynamicRayCallback.empty())
            {
                ++PathDiagnostic.PortalCandidatesRejected;
                continue;
            }
            bool IsVisible = true;
            AcousticPathObservation CenterLegA, CenterLegB;
            float CenterA = 0.0f, CenterB = 0.0f;
            for (u32 SampleIndex = 0; SampleIndex < 3u; ++SampleIndex)
            {
                const Fvector Point = SoundOpeningGeometry::SupportPoint(Opening, SampleIndex);
                float SolidA = -1.0f, SolidB = -1.0f;
                AcousticPathObservation LegA, LegB;
                g_AcousticPortalRayBudget -= 2u;
                const float A = Snd_TransmissionRay(model, collider, listener, Point, &SolidA, nullptr, &LegA);
                const float B = Snd_TransmissionRay(model, collider, Point, emitter, &SolidB, nullptr, &LegB);
                if (SolidA >= 0.0f || SolidB >= 0.0f)
                {
                    IsVisible = false;
                    break;
                }
                g_AcousticDynamicRayBudget -= 2u;
                if (::Sound->AcousticDynamicRayCallback(listener, Point) > EPS_S ||
                    ::Sound->AcousticDynamicRayCallback(Point, emitter) > EPS_S)
                {
                    IsVisible = false;
                    break;
                }
                if (SampleIndex == 0)
                {
                    CenterA = A; CenterB = B;
                    CenterLegA = LegA; CenterLegB = LegB;
                }
            }
            if (!IsVisible)
            {
                ++PathDiagnostic.PortalCandidatesRejected;
                continue;
            }
            const float ApertureGain = SoundOpeningGeometry::DirectCoupling(Opening, emitter, listener);
            const float Gain = CenterA * CenterB * ApertureGain;
            if (Gain > portal)
            {
                portal = Gain;
                portalDistance = listener.distance_to(Opening.Center) + Opening.Center.distance_to(emitter);
                portalType = (Opening.Flags & SoundRoomPortalRoof) != 0 ? 2u : 1u;
                PortalId = Opening.Id;
                PortalGlass = CenterLegA.GlassEncountered || CenterLegB.GlassEncountered;
                PortalMaterialsKnown = CenterLegA.CenterRayQueried && CenterLegB.CenterRayQueried &&
                    !CenterLegA.MaterialFallbackUsed && !CenterLegB.MaterialFallbackUsed;
                PortalLegAGain = CenterA; PortalLegBGain = CenterB;
                PortalLowRatio = CenterLegA.LowTransmissionRatio * CenterLegB.LowTransmissionRatio;
                PortalMidRatio = CenterLegA.MidTransmissionRatio * CenterLegB.MidTransmissionRatio;
                PortalHighRatio = CenterLegA.HighTransmissionRatio * CenterLegB.HighTransmissionRatio;
                PathDiagnostic.PortalVerified = true;
                PathDiagnostic.PortalPoint = Opening.Center;
                PathDiagnostic.PortalNormal = Opening.Normal;
                PathDiagnostic.PortalTangentU = Opening.TangentU;
                PathDiagnostic.PortalArea = Opening.Area;
                PathDiagnostic.PortalHalfExtentU = Opening.HalfExtentU;
                PathDiagnostic.PortalHalfExtentV = Opening.HalfExtentV;
                PathDiagnostic.PortalPathLength = portalDistance;
                PathDiagnostic.PortalApertureGain = ApertureGain;
            }
        }
    }
    // Compare paths in the same linear transmission units. Each detour
    // already includes both CFORM legs, its extra length and its own dynamic
    // door checks. A door on the direct ray cannot obstruct a separate route.
    const float transmitted = std::clamp(std::max(direct, portal), 0.0f, 1.0f);
	PathDiagnostic.Queried = PathDiagnostic.CenterRayQueried;
	PathDiagnostic.Valid = PathDiagnostic.CenterRayQueried && PathComplete;
	PathDiagnostic.Deferred = !PathComplete;
	PathDiagnostic.CenterTransmission = centerTransmission;
	PathDiagnostic.DirectGain = direct;
	PathDiagnostic.DirectLowTransmissionRatio = DirectLowRatio;
	PathDiagnostic.DirectMidTransmissionRatio = DirectMidRatio;
	PathDiagnostic.DirectHighTransmissionRatio = DirectHighRatio;
	PathDiagnostic.PortalGain = portal;
	PathDiagnostic.DynamicGain = PathDiagnostic.DynamicQueried ? 1.0f - 0.78f * dynamicBlocked : -1.0f;
	PathDiagnostic.SelectedGain = transmitted;
	PathDiagnostic.SelectedLowTransmissionRatio = portal > direct ? PortalLowRatio : DirectLowRatio;
	PathDiagnostic.SelectedMidTransmissionRatio = portal > direct ? PortalMidRatio : DirectMidRatio;
	PathDiagnostic.SelectedHighTransmissionRatio = portal > direct ? PortalHighRatio : DirectHighRatio;
	PathDiagnostic.ChosenPath = portal > direct ? 2u : dynamicBlocked > EPS_S ? 3u : 1u;
	PathDiagnostic.PortalKind = portalType;
	PathDiagnostic.PortalId = PortalId;
	PathDiagnostic.PortalGlass = PortalGlass;
	PathDiagnostic.PortalMaterialsKnown = PortalMaterialsKnown;
	PathDiagnostic.PortalLegAGain = PortalLegAGain;
	PathDiagnostic.PortalLegBGain = PortalLegBGain;
	cached.Diagnostic = PathDiagnostic;
	if (DiagnosticOut != nullptr) *DiagnosticOut = PathDiagnostic;
    cached.world = model;
    cached.lx = lx; cached.ly = ly; cached.lz = lz;
    cached.sx = sx; cached.sy = sy; cached.sz = sz;
    // Incomplete work is shared by this update's layers only, then retried.
    cached.frame = g_AcousticPathFrame;
    cached.expiry = PathComplete ? now + 230000000ULL : 0;
    cached.obstruction = 1.0f - transmitted;
    cached.portal = portal;
    cached.glass = largestGlass;
    cached.dynamic = dynamicBlocked;
    cached.portal_type = portalType;
    if (PortalOut) *PortalOut = portal;
    if (GlassOut) *GlassOut = largestGlass;
    if (DynamicOut) *DynamicOut = dynamicBlocked;
    return cached.obstruction;
}

// A source can be behind glass or a door even while both sound emitter and
// listener are individually classified as "indoors". Smooth all parameters
// over time so crossing an opening does not click or abruptly switch DSP.
static void Snd_UpdateShotBarrier(sound_slot_state& slot, const Fvector& pos)
{
    const float mismatch = std::clamp(
        std::abs(slot.IndoorFactor - GMixer.IndoorFactor), 0.0f, 1.0f);
    float portal = 0.0f, glass = 0.0f, dynamic = 0.0f;
    const float blocked = Snd_WallObstructionBetween(
        GMixer.listener.Position, pos, &portal, &glass, &dynamic, &slot.PathDiagnostic);
    // Material and aperture propagation already provide pressure gain.
    // Room classification must not add an unrelated gain penalty or floor.
    const float gainTarget = std::clamp(1.0f - blocked, 0.0f, 1.0f);
    if (!slot.BarrierInitialized)
    {
        slot.BarrierBlockage = blocked;
        slot.BarrierGain = gainTarget;
        slot.BarrierMismatch = mismatch;
        slot.BarrierPortal = portal;
        slot.BarrierWindow = glass;
        slot.BarrierDynamic = dynamic;
        slot.BarrierLowRatio = slot.PathDiagnostic.SelectedLowTransmissionRatio;
        slot.BarrierMidRatio = slot.PathDiagnostic.SelectedMidTransmissionRatio;
        slot.BarrierHighRatio = slot.PathDiagnostic.SelectedHighTransmissionRatio;
        slot.BarrierInitialized = true;
        return;
    }
    const float smoothing = std::clamp(GMixer.dt * 6.0f, 0.0f, 1.0f);
    slot.BarrierBlockage += (blocked - slot.BarrierBlockage) * smoothing;
    slot.BarrierGain += (gainTarget - slot.BarrierGain) * smoothing;
    slot.BarrierMismatch += (mismatch - slot.BarrierMismatch) * smoothing;
    slot.BarrierPortal += (portal - slot.BarrierPortal) * smoothing;
    slot.BarrierWindow += (glass - slot.BarrierWindow) * smoothing;
    slot.BarrierDynamic += (dynamic - slot.BarrierDynamic) * smoothing;
    slot.BarrierLowRatio += (slot.PathDiagnostic.SelectedLowTransmissionRatio - slot.BarrierLowRatio) * smoothing;
    slot.BarrierMidRatio += (slot.PathDiagnostic.SelectedMidTransmissionRatio - slot.BarrierMidRatio) * smoothing;
    slot.BarrierHighRatio += (slot.PathDiagnostic.SelectedHighTransmissionRatio - slot.BarrierHighRatio) * smoothing;
}

struct AcousticRoomBlend
{
    u32 Rooms[2] = {};
    float Weights[2] = {};
};

struct AcousticOpeningValidation
{
    CDB::MODEL* World = nullptr;
    u32 PortalId = 0;
    u32 Frame = 0;
    Fvector From = {};
    Fvector To = {};
    u64 Expiry = 0;
    float Transmission = 0.0f;
    bool Open = false;
};

// Fixed game-thread cache shared with read-only callback weight lookup. All
// publication is under render_lock; no allocation or rays occur in rendering.
static AcousticOpeningValidation g_AcousticOpeningValidation[512];

static bool Snd_ValidateOpeningLeg(u32 PortalId, const Fvector& From, const Fvector& To,
    bool AllowQuery, float& Transmission)
{
    Transmission = 0.0f;
    // A cached static opening cannot certify a door when dynamic state is unavailable.
    if (::Sound->AcousticDynamicRayCallback.empty()) return false;
    auto* World = ::Sound->get_geometry_occ();
    auto* Collider = ::Sound->get_geometry_db();
    if (World == nullptr || Collider == nullptr) return false;
    if (From.distance_to_sqr(To) <= EPS_S * EPS_S)
    {
        Transmission = 1.0f;
        return true;
    }
    const auto Quantize = [](float Value) { return u32(s32(std::floor(Value * 10.0f))); };
    const u32 Hash = PortalId * 2654435761u ^ Quantize(From.x) * 73856093u ^
        Quantize(From.y) * 19349663u ^ Quantize(From.z) * 83492791u ^
        Quantize(To.x) * 3221225473u ^ Quantize(To.y) * 1640531513u ^ Quantize(To.z);
    auto& Cache = g_AcousticOpeningValidation[Hash & 511u];
    const bool Matching = Cache.World == World && Cache.PortalId == PortalId &&
        Cache.From.distance_to_sqr(From) < 0.0001f && Cache.To.distance_to_sqr(To) < 0.0001f;
    if (Matching && (Cache.Frame == g_AcousticPathFrame ||
        (AllowQuery && Snd_GetTimestamp() < Cache.Expiry)))
    {
        if (AllowQuery) Cache.Frame = g_AcousticPathFrame;
        Transmission = Cache.Transmission;
        return Cache.Open;
    }
    if (!AllowQuery || g_AcousticPortalRayBudget == 0 ||
        (!::Sound->AcousticDynamicRayCallback.empty() && g_AcousticDynamicRayBudget == 0))
    {
        return false;
    }
    --g_AcousticPortalRayBudget;
    float Solid = -1.0f;
    AcousticPathObservation Observation;
    const float Gain = Snd_TransmissionRay(World, Collider, From, To, &Solid, nullptr, &Observation);
    bool Open = Solid < 0.0f && !Observation.GlassEncountered && Observation.CenterRayQueried;
    if (!::Sound->AcousticDynamicRayCallback.empty())
    {
        --g_AcousticDynamicRayBudget;
        Open = ::Sound->AcousticDynamicRayCallback(From, To) <= EPS_S && Open;
    }
    Cache.World = World;
    Cache.PortalId = PortalId;
    Cache.Frame = g_AcousticPathFrame;
    Cache.From = From;
    Cache.To = To;
    // Same finite geometry-cache lifetime as the existing direct path. A
    // deferred query never refreshes a validation or invents an open link.
    Cache.Expiry = Snd_GetTimestamp() + 230000000ULL;
    Cache.Transmission = Gain;
    Cache.Open = Open;
    Transmission = Gain;
    return Open;
}

static AcousticRoomBlend Snd_ResolveRoomBlend(const Fvector& Position, u32 BaseRoom, bool AllowQuery)
{
    AcousticRoomBlend Blend;
    Blend.Rooms[0] = BaseRoom;
    Blend.Weights[0] = 1.0f;
    SoundRoomPortal Portals[4] = {};
    const u32 Count = Snd_FindRoomTransitionPortals(BaseRoom, Position, Portals, std::size(Portals));
    for (u32 Index = 0; Index < Count; ++Index)
    {
        const SoundRoomPortal& Portal = Portals[Index];
        Fvector Delta;
        Delta.sub(Position, Portal.Center);
        const float SignedDistance = Delta.dotproduct(Portal.Normal);
        const float Depth = Snd_RoomPortalTransitionDepth(Portal);
        Fvector PlanePoint = Position;
        PlanePoint.mad(Portal.Normal, -SignedDistance);
        Fvector From = PlanePoint, To = PlanePoint;
        From.mad(Portal.Normal, -Depth - 0.1f);
        To.mad(Portal.Normal, Depth + 0.1f);
        float Transmission = 0.0f;
        if (!Snd_ValidateOpeningLeg(Portal.Id, From, To, AllowQuery, Transmission)) continue;
        float Across = std::clamp((SignedDistance + Depth) / (2.0f * Depth), 0.0f, 1.0f);
        Across = Across * Across * (3.0f - 2.0f * Across);
        Blend.Rooms[0] = Portal.RoomA;
        Blend.Rooms[1] = Portal.RoomB;
        Blend.Weights[0] = 1.0f - Across;
        Blend.Weights[1] = Across;
        break;
    }
    return Blend;
}

static float Snd_RoomBlendIndoor(const AcousticRoomBlend& Blend)
{
    float Indoor = 0.0f;
    for (u32 Index = 0; Index < 2; ++Index)
        if (Blend.Rooms[Index] != 0) Indoor += Blend.Weights[Index];
    return std::clamp(Indoor, 0.0f, 1.0f);
}

static float Snd_OpeningFieldCoupling(const SoundRoomPortal& Opening, const Fvector& Listener, u32 SourceRoom)
{
    if (!SoundOpeningGeometry::IsValidOpening(Opening)) return 0.0f;
    Fvector Delta;
    Delta.sub(Listener, Opening.Center);
    if (Delta.dotproduct(Opening.Normal) * (Opening.RoomA == SourceRoom ? 1.0f : -1.0f) < -EPS_S) return 0.0f;
    return SoundOpeningGeometry::FieldCoupling(Opening, Listener);
}

// A room return remains alive independently of the voices that excited it.
// The formed diffuse field uses aperture coupling independently of the direct
// source ray; static/dynamic walls remain a separate transmission fallback.
static void Snd_UpdateRoomReturns()
{
    SoundRoomReturnState Rooms[12] = {};
    const u32 Count = Snd_GetRoomReturnStates(Rooms, std::size(Rooms));
    if (Count == 0) return;
    const u32 ListenerRoom = Snd_GetZoneRoom(GMixer.ListenerZone);
    // Only audible tails or imminent room excitations need transport. Retired
    // pool assignments must not consume refresh capacity. Share the existing
    // total ray budgets; a separate one-room cap starves tails below 48 Hz.
    bool Requested[SND_BUS_COUNT] = {};
    float OriginPriority[SND_BUS_COUNT] = {};
    Fvector Origins[SND_BUS_COUNT] = {};
    u32 OriginRooms[SND_BUS_COUNT] = {};
    for (const auto& Voice : GMixer.slots)
    {
        if (Voice.state != Mixer::State::Playing &&
            !(Voice.state == Mixer::State::Delay && Voice.delay <= 0.1f)) continue;
        if (Voice.flags & ((u16)Mixer::Flags::Intro | (u16)Mixer::Flags::NoOCC)) continue;
        if (!(Voice.flags & (u16)Mixer::Flags::Spatial) && Voice.shot_group == 0) continue;
        const SoundBus* VoiceBus = Snd_GetBus(Voice.bus);
        if (!VoiceBus || VoiceBus->ZoneSend <= 0.0f) continue;
        const Fvector& Position = Voice.parameters[(u32)Mixer::ParameterId::Position];
        const float Distance = GMixer.listener.Position.distance_to(Position);
        if (Distance >= 85.0f || Distance >= Voice.parameters[(u32)Mixer::ParameterId::DistanceRange].y) continue;
        const auto Blend = Snd_ResolveRoomBlend(Position, Snd_GetZoneRoom(Snd_FindZone(Position)), false);
        for (u32 Side = 0; Side < 2; ++Side)
        {
            const u32 Bus = Snd_GetRoomBus(Blend.Rooms[Side]);
            if (Bus != 0 && Blend.Weights[Side] > 0.0f)
            {
                Requested[Bus - 1] = true;
                // A source fading out of this room cannot move its tail origin through a wall.
                if (Blend.Rooms[Side] != Snd_GetZoneRoom(Snd_FindZone(Position))) continue;
                const Fvector& Volume = Voice.parameters[(u32)Mixer::ParameterId::VolumePerChannel];
                const float Priority = Volume.x * Volume.y * VoiceBus->Volume * VoiceBus->UserVolume *
                    Blend.Weights[Side] / std::max(1.0f, Distance * Distance);
                if (Priority > OriginPriority[Bus - 1])
                {
                    OriginPriority[Bus - 1] = Priority;
                    Origins[Bus - 1] = Position;
                    OriginRooms[Bus - 1] = Blend.Rooms[Side];
                }
            }
        }
    }
    for (u32 Index = 0; Index < SND_BUS_COUNT; ++Index)
        if (OriginPriority[Index] > 0.0f)
            Snd_SetRoomReturnOrigin(Index + 1, OriginRooms[Index], Origins[Index]);
    Snd_GetRoomReturnStates(Rooms, std::size(Rooms));
    static u32 Cursor = 0;
    for (u32 Index = 0; Index < Count; ++Index)
    {
        const auto& Room = Rooms[(Cursor + Index) % Count];
        const auto ListenerBlend = Snd_ResolveRoomBlend(GMixer.listener.Position, ListenerRoom, false);
        float LocalWeight = 0.0f;
        for (u32 Side = 0; Side < 2; ++Side)
            if (ListenerBlend.Rooms[Side] == Room.RoomId) LocalWeight += ListenerBlend.Weights[Side];
        const SoundRoomInfo* Info = Snd_GetRoomInfo(Room.RoomId);
        if (Room.RoomId == ListenerRoom || LocalWeight >= 1.0f - EPS_S)
        {
            Snd_SetRoomReturnPath(Room.Bus, Room.RoomId, 1.0f, 20000.0f, 0.0f, true);
            continue;
        }
        if (!Room.Active && !Requested[Room.Bus - 1]) continue;
        AcousticPathObservation Path;
        float Portal = 0.0f, Glass = 0.0f, Dynamic = 0.0f;
        const float Blocked = Snd_WallObstructionBetween(GMixer.listener.Position,
            Room.Anchor, &Portal, &Glass, &Dynamic, &Path);
        bool Valid = Path.Valid && !Path.Deferred;
        const float Mismatch = 1.0f - GMixer.IndoorFactor;
        float Amount = std::clamp(Blocked * (0.58f + 0.42f * Mismatch), 0.0f, 1.0f);
        if (Path.ChosenPath == 2)
        {
            Glass = Path.PortalGlass ? 1.0f - std::min(Path.PortalLegAGain, Path.PortalLegBGain) : 0.0f;
        }
        float Cutoff = std::clamp(11000.0f - 9800.0f * Amount +
            4500.0f * Glass + 2400.0f * Portal, 1000.0f, 13000.0f);
        float Gain = std::clamp((1.0f - Blocked) * (1.0f - 0.10f * Mismatch), 0.0f, 1.0f);
        float ReturnBandRatios[3] = {1.0f, 1.0f, 1.0f};
        SoundRoomReturnSpatial Spatial;
        SoundRoomPortal DominantOpening;
        float DominantOpeningGain = 0.0f;
        const bool MaterialReturn = Info != nullptr && Info->HasMaterialGeometry;
        if (Info != nullptr && Info->HasOpenGeometry)
        {
            // A clear central ray through a window is not an unrestricted
            // return of the whole room field. Only an actual boundary hit
            // supplies the independent wall/glass transmission branch.
            const bool DirectValid = Path.CenterRayQueried &&
                (::Sound->AcousticDynamicRayCallback.empty() || Path.DynamicQueried);
            const float WallGain = DirectValid && Path.BoundaryEncountered
                ? std::clamp(Path.DirectGain, 0.0f, 1.0f) : 0.0f;
            float OpeningEnergy = 0.0f;
            SoundRoomPortal Candidates[4] = {};
            const u32 PortalCount = Snd_FindRoomPortals(Room.RoomId, ListenerRoom,
                GMixer.listener.Position, Candidates, std::size(Candidates));
            for (u32 PortalIndex = 0; PortalIndex < PortalCount; ++PortalIndex)
            {
                const SoundRoomPortal& Opening = Candidates[PortalIndex];
                if (!SoundOpeningGeometry::MatchesEndpointSides(Opening, Room.Anchor,
                    GMixer.listener.Position, Room.RoomId, ListenerRoom)) continue;
                float FromRoom = 1.0f, ToListener = 1.0f;
                bool SupportsClear = true;
                for (u32 Support = 0; Support < 3; ++Support)
                {
                    const Fvector Point = SoundOpeningGeometry::SupportPoint(Opening, Support);
                    float LegA = 0.0f, LegB = 0.0f;
                    if (!Snd_ValidateOpeningLeg(Opening.Id, Room.Anchor, Point, true, LegA) ||
                        !Snd_ValidateOpeningLeg(Opening.Id, Point, GMixer.listener.Position, true, LegB))
                    {
                        SupportsClear = false;
                        break;
                    }
                    FromRoom = std::min(FromRoom, LegA);
                    ToListener = std::min(ToListener, LegB);
                }
                if (!SupportsClear) continue;
                const float Coupling = Snd_OpeningFieldCoupling(Opening, GMixer.listener.Position, Room.RoomId);
                // Exported open supports for one endpoint pair are disjoint.
                // Diffuse fields add energy across independent windows; max
                // incorrectly made a second open window acoustically silent.
                const float Contribution = FromRoom * ToListener * Coupling;
                OpeningEnergy += Contribution * Contribution;
                if (Contribution > DominantOpeningGain)
                {
                    DominantOpeningGain = Contribution;
                    DominantOpening = Opening;
                }
            }
            // Fixed candidate/ray limits remain; total diffuse coupling is
            // bounded by unity even where projected solid angles overlap.
            const float OpeningGain = std::sqrt(std::clamp(OpeningEnergy, 0.0f, 1.0f));
            Gain = LocalWeight + (1.0f - LocalWeight) * std::max(WallGain, OpeningGain);
            Valid = LocalWeight > 0.0f || DirectValid || OpeningGain > 0.0f;
            if (OpeningGain > 0.0f && OpeningGain >= WallGain && Gain > 0.0f && DominantOpening.Id != 0)
            {
                Spatial.PortalId = DominantOpening.Id;
                Spatial.Fraction = (1.0f - LocalWeight) * OpeningGain / Gain;
                Fmatrix Camera;
                Camera.build_camera_dir(GMixer.listener.Position, GMixer.listener.Direction, GMixer.listener.Normal);
                for (u32 Side = 0; Side < 2; ++Side)
                {
                    Fvector Direction;
                    Direction.sub(SoundOpeningGeometry::SupportPoint(DominantOpening, Side + 1), GMixer.listener.Position);
                    Direction.normalize_safe();
                    Camera.transform_dir(Spatial.LocalDirections[Side], Direction);
                }
                const float Radius = std::sqrt(DominantOpening.Area / PI);
                Spatial.SpreadDegrees = std::clamp(2.0f * std::atan2(Radius,
                    GMixer.listener.Position.distance_to(DominantOpening.Center)) * 180.0f / PI, 0.0f, 180.0f);
            }
            if (MaterialReturn)
            {
                // The direct field's wall spectrum also transports the room's
                // existing late tail. Local/aperture coupling remains uncolored.
                // Mix the local and wall pressure contributions before dividing
                // by the same scalar gain; do not filter the local part twice.
                if (WallGain > OpeningGain && Gain > 0.0f)
                {
                    const float DirectRatios[3] = {Path.DirectLowTransmissionRatio,
                        Path.DirectMidTransmissionRatio, Path.DirectHighTransmissionRatio};
                    for (u32 Band = 0; Band < 3; ++Band)
                    {
                        ReturnBandRatios[Band] = (LocalWeight + (1.0f - LocalWeight) *
                            WallGain * DirectRatios[Band]) / Gain;
                    }
                }
                // The three material bands replace the unrelated scalar LPF.
                Amount = 0.0f;
                Cutoff = 20000.0f;
            }
            else if (OpeningGain >= WallGain || LocalWeight > 0.0f)
            {
                Amount = 0.0f;
                Cutoff = 20000.0f;
            }
        }
        Snd_SetRoomReturnPath(Room.Bus, Room.RoomId, Gain, Cutoff, Amount, Valid,
            MaterialReturn ? ReturnBandRatios : nullptr, Spatial.PortalId != 0 ? &Spatial : nullptr);
    }
    Cursor = (Cursor + 1) % Count;
}

// Authored environments determine source membership; collision probes only
// describe transmission and apertures, never select a reverb preset.
static void Snd_UpdateSlotIndoorFactor(sound_slot_state& Slot, const Fvector& Pos)
{
	Slot.zone_idx = Snd_FindZone(Pos);
	Slot.IndoorFactor = Snd_HasRoomMetadata()
        ? Snd_RoomBlendIndoor(Snd_ResolveRoomBlend(Pos, Snd_GetZoneRoom(Slot.zone_idx), false))
        : Slot.zone_idx != 0 ? 1.0f : 0.0f;
	Slot.IndoorFactorValid = true;
}

static bool Snd_IsPremixedShotName(const char* n, bool RoomPath = false)
{
    if (!n || !strstr(n, "weapons\\"))
        return false;
    // Distance-band recordings must remain audible and can excite the room.
    // Recorded reflections, bass and cracks do not generate another room tail.
    if (RoomPath)
    {
        return strstr(n, "\\reflectors\\") || strstr(n, "\\_cracks\\") ||
            strstr(n, "\\_bass\\") || strstr(n, "\\tail_") || strstr(n, "\\echo_");
    }
    // Preserve the existing outdoor mix of pre-baked distance recordings.
    return strstr(n, "\\_shared\\") || strstr(n, "\\_cracks\\") ||
        strstr(n, "\\_bass\\") || strstr(n, "\\_boom\\") ||
        strstr(n, "\\tail_") || strstr(n, "\\echo_") ||
        strstr(n, "\\far_") || strstr(n, "\\medium_distance") ||
        strstr(n, "\\close_distance");
}

static bool Snd_IsPremixedShotTail(const SoundSourceState& Source, bool RoomPath = false)
{
    return Snd_IsPremixedShotName(Source.Desc.name.c_str(), RoomPath);
}

static bool Snd_IsGeometryExciter(const sound_slot_state& Slot)
{
    if (Snd_IsPremixedShotName(Slot.sound_name.c_str(), true)) return false;
    if (Slot.shot_group == 0 || Slot.shot_layer == 0) return true;
    // STCoP layer order differs between mono and HUD sets. Select authored
    // distance recordings by their semantic name, never a numeric layer cap.
    const char* Name = Slot.sound_name.c_str();
    return strstr(Name, "\\close_distance") || strstr(Name, "\\medium_distance") ||
        strstr(Name, "\\far_");
}

static void Snd_PrepareActiveRooms()
{
    u32 Rooms[32] = {};
    u32 Count = 0;
    bool Pins[SND_BUS_COUNT] = {};
    auto AddRoom = [&](u32 Id)
    {
        if (Id == 0 || Count == std::size(Rooms)) return;
        for (u32 Index = 0; Index < Count; ++Index) if (Rooms[Index] == Id) return;
        Rooms[Count++] = Id;
    };
    const auto AddBlend = [&](const AcousticRoomBlend& Blend)
    {
        for (u32 Side = 0; Side < 2; ++Side)
            if (Blend.Weights[Side] > 0.0f) AddRoom(Blend.Rooms[Side]);
    };
    AddBlend(Snd_ResolveRoomBlend(GMixer.listener.Position, Snd_GetZoneRoom(GMixer.ListenerZone), true));
    for (u32 Index = 0; Index < SND_BUS_COUNT; ++Index)
        Pins[Index] = GMixer.ListenerRoomWeights[Index] > 0.00001f;
    struct RoomRequest { u32 Slot; float Contribution; };
    xr_vector<RoomRequest> Requests;
    Requests.reserve(GMixer.slots.size());
    for (u32 SlotIndex = 0; SlotIndex < GMixer.slots.size(); ++SlotIndex)
    {
        const auto& Slot = GMixer.slots[SlotIndex];
        if (Slot.state == Mixer::State::Stopped) continue;
        for (u32 Index = 0; Index < SND_BUS_COUNT; ++Index)
            Pins[Index] = Pins[Index] || Slot.RoomWeights[Index] > 0.00001f || Slot.RoomBeginWeights[Index] > 0.00001f;
        const bool Physical = (Slot.flags & (u16)Mixer::Flags::Spatial) != 0 || Slot.shot_group != 0;
        const SoundBus* Bus = Snd_GetBus(Slot.bus);
        if (!Physical || !Bus || Bus->ZoneSend <= 0.0f ||
            (Slot.flags & ((u16)Mixer::Flags::Intro | (u16)Mixer::Flags::NoOCC)) ||
            !(Slot.state == Mixer::State::Playing || (Slot.state == Mixer::State::Delay && Slot.delay <= 0.1f))) continue;
        const float Distance = GMixer.listener.Position.distance_to(Slot.parameters[(u32)Mixer::ParameterId::Position]);
        const auto& Range = Slot.parameters[(u32)Mixer::ParameterId::DistanceRange];
        if (Distance >= 85.0f || Distance >= Range.y) continue;
        const auto& Volume = Slot.parameters[(u32)Mixer::ParameterId::VolumePerChannel];
        const float Contribution = Volume.x * Volume.y * Bus->Volume * Bus->UserVolume *
            std::min(1.0f, std::max(Range.x, 1.0f) / std::max(Distance, 1.0f)) * (Slot.shot_group != 0 ? 4.0f : 1.0f);
        if (Contribution > 0.0001f) Requests.push_back({SlotIndex, Contribution});
    }
    std::stable_sort(Requests.begin(), Requests.end(), [](const RoomRequest& A, const RoomRequest& B)
    { return A.Contribution > B.Contribution; });
    for (const auto& Request : Requests)
    {
        const auto& Position = GMixer.slots[Request.Slot].parameters[(u32)Mixer::ParameterId::Position];
        AddBlend(Snd_ResolveRoomBlend(Position, Snd_GetZoneRoom(Snd_FindZone(Position)), true));
    }
    Snd_PrepareRoomBuses(Rooms, Count, Pins);
}

static float Snd_RoomReference(const sound_slot_state& Slot, u32 Index)
{
    if (Snd_IsLogicalRoomBus(Index + 1))
    {
        const SoundBus* Bus = Snd_GetBus(Index + 1);
        for (u32 EffectIndex = 0; Bus && EffectIndex < Bus->EffectCount; ++EffectIndex)
        {
            const auto& Effect = Bus->Effects[EffectIndex];
            const auto* Entry = Snd_GetEffect(Effect.Effect);
            if (Entry && xr_strcmp(Entry->Name.c_str(), "reverb") == 0)
                return std::max(Resonance_GetWetReference(Effect.State), 0.0001f);
        }
    }
    return 1.0f;
}

static void Snd_ResetRoomWeights(sound_slot_state& Slot)
{
    Slot.RoomWeightsValid = false;
    memset(Slot.RoomWeights, 0, sizeof(Slot.RoomWeights));
    memset(Slot.RoomBeginWeights, 0, sizeof(Slot.RoomBeginWeights));
    memset(Slot.RoomLocalWeights, 0, sizeof(Slot.RoomLocalWeights));
    memset(Slot.RoomLocalBeginWeights, 0, sizeof(Slot.RoomLocalBeginWeights));
    memset(Slot.RoomIncomingWeights, 0, sizeof(Slot.RoomIncomingWeights));
    memset(Slot.RoomIncomingBeginWeights, 0, sizeof(Slot.RoomIncomingBeginWeights));
}

static float Snd_UpdateRoomWeights(sound_slot_state& Slot, u32 RoomBus, u32 SourceRoomBus, bool ReceivesEnvironment)
{
    const bool LocalSource = ((Slot.flags & (u16)Mixer::Flags::Spatial) != 0 || Slot.shot_group != 0) &&
        !(Slot.flags & (u16)Mixer::Flags::NoOCC) && Snd_HasRoomMetadata();
    AcousticRoomBlend SourceBlend;
    if (LocalSource)
    {
        SourceBlend = Snd_ResolveRoomBlend(Slot.parameters[(u32)Mixer::ParameterId::Position],
            Snd_GetZoneRoom(Slot.zone_idx), false);
    }
    const u32 SourceBuses[2] = {Snd_GetRoomBus(SourceBlend.Rooms[0]), Snd_GetRoomBus(SourceBlend.Rooms[1])};
    const float OutsideWeight = LocalSource ? 1.0f - Snd_RoomBlendIndoor(SourceBlend) : 1.0f;
    // Audio time smooths the geometric target; it never expands ownership
    // through walls. A first block starts with this same verified target.
    const float Blend = 1.0f - std::exp(-4.5f * float(SND_BLOCKSIZE) / float(SND_SAMPLERATE));
    float Indoor = 0.0f, LargestTarget = 0.0f;
    u32 DominantBus = 0;
    for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
    {
        float LocalTarget = 0.0f;
        for (u32 Side = 0; Side < 2; ++Side)
            if (SourceBuses[Side] == Idx + 1) LocalTarget += SourceBlend.Weights[Side];
        float IncomingTarget = OutsideWeight * GMixer.ListenerRoomWeights[Idx];
        if (!Snd_HasRoomMetadata())
        {
            LocalTarget = 0.0f;
            IncomingTarget = Idx + 1 == RoomBus ? 1.0f : 0.0f;
        }
        if (!ReceivesEnvironment || !GMixer.WetBusReady[Idx])
        {
            LocalTarget = 0.0f;
            IncomingTarget = 0.0f;
        }
        const float Target = LocalTarget + IncomingTarget;
        if (Target > LargestTarget)
        {
            LargestTarget = Target;
            DominantBus = Idx + 1;
        }
        float& LocalWeight = Slot.RoomLocalWeights[Idx];
        float& IncomingWeight = Slot.RoomIncomingWeights[Idx];
        if (!Slot.RoomWeightsValid)
        {
            LocalWeight = LocalTarget;
            IncomingWeight = IncomingTarget;
        }
        Slot.RoomLocalBeginWeights[Idx] = LocalWeight;
        Slot.RoomIncomingBeginWeights[Idx] = IncomingWeight;
        if (!GMixer.WetBusReady[Idx])
        {
            LocalWeight = IncomingWeight = 0.0f;
            Slot.RoomLocalBeginWeights[Idx] = Slot.RoomIncomingBeginWeights[Idx] = 0.0f;
        }
        else
        {
            LocalWeight += (LocalTarget - LocalWeight) * Blend;
            IncomingWeight += (IncomingTarget - IncomingWeight) * Blend;
            if (LocalTarget == 0.0f && LocalWeight < 0.00001f) LocalWeight = 0.0f;
            if (IncomingTarget == 0.0f && IncomingWeight < 0.00001f) IncomingWeight = 0.0f;
        }
        Slot.RoomWeights[Idx] = LocalWeight + IncomingWeight;
        Slot.RoomBeginWeights[Idx] = Slot.RoomLocalBeginWeights[Idx] + Slot.RoomIncomingBeginWeights[Idx];
        Indoor += Slot.RoomWeights[Idx];
    }
    if (Slot.RoomWeightsValid && Slot.SendBus != DominantBus)
    {
        Slot.PreviousRoomBus = Slot.SendBus;
        GMixer.TransitionOldBus = Slot.SendBus;
        GMixer.TransitionNewBus = DominantBus;
        ++GMixer.RoomTransitions;
    }
    Slot.SendBus = DominantBus;
    Slot.RoomWeightsValid = true;
    return std::clamp(Indoor, 0.0f, 1.0f);
}

static u32 Snd_AcousticSourceKind(const sound_slot_state& Slot)
{
	const SoundBus* Bus = Snd_GetBus(Slot.bus);
	if (Bus && xr_strcmp(Bus->Name.c_str(), "npc_voice") == 0) return 2;
	if (Bus && xr_strcmp(Bus->Name.c_str(), "creatures") == 0) return 1;
	return Slot.shot_group != 0 ? 3u : 0u;
}

static void Snd_SendRoomWeights(sound_slot_state& Slot, float** Data,
	float BeginFactor, float EndFactor, float Gain, float Left, float Right,
    float** LocalData, float LocalBegin, float LocalEnd)
{
	if (!psSoundFlags.is(ss_EFX))
	{
		Slot.RoomSendScale = 0.0f;
		return;
	}
	const float BeginGain = Slot.RoomSendScale < 0.0f ? Gain : Slot.RoomSendScale;
	const u32 Kind = Snd_AcousticSourceKind(Slot);
	if (Kind == 2)
		SoundAcousticCapture::SelectVoice(Slot.AcousticEvent, Slot.sound_name.c_str(), Snd_GetZoneRoom(Slot.zone_idx),
			Snd_GetRoomBus(Snd_GetZoneRoom(Slot.zone_idx)), Slot.parameters[(u32)Mixer::ParameterId::Position], GMixer.listener.Position);
	for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
	{
		if (Slot.RoomWeights[Idx] > 0.0f || Slot.RoomBeginWeights[Idx] > 0.0f)
		{
            const float Reference = Snd_RoomReference(Slot, Idx);
            // These contributions may coexist in one bus. Interpolate their
            // own PCM/gain endpoints; never switch the entire feed at zero.
            if (Slot.RoomLocalBeginWeights[Idx] > 0.0f || Slot.RoomLocalWeights[Idx] > 0.0f)
            {
                const float Begin = LocalBegin * BeginGain * Slot.RoomLocalBeginWeights[Idx] / Reference;
                const float End = LocalEnd * Gain * Slot.RoomLocalWeights[Idx] / Reference;
                Snd_SendToBus(Idx + 1, LocalData, Begin, End, 1.0f, 1.0f, Slot.AcousticEvent, Kind);
                SoundAcousticCapture::TapVoice(Slot.AcousticEvent, SoundAcousticCapture::Stage::VoiceSend,
                    Idx + 1, LocalData, Begin, End, 1.0f, 1.0f);
            }
            if (Slot.RoomIncomingBeginWeights[Idx] > 0.0f || Slot.RoomIncomingWeights[Idx] > 0.0f)
            {
                const float Begin = BeginFactor * BeginGain * Slot.RoomIncomingBeginWeights[Idx] / Reference;
                const float End = EndFactor * Gain * Slot.RoomIncomingWeights[Idx] / Reference;
                Snd_SendToBus(Idx + 1, Data, Begin, End, Left, Right, Slot.AcousticEvent, Kind);
                SoundAcousticCapture::TapVoice(Slot.AcousticEvent, SoundAcousticCapture::Stage::VoiceSend,
                    Idx + 1, Data, Begin, End, Left, Right);
            }
		}
	}
	Slot.RoomSendScale = Gain;
}

static void Snd_MeterShot(sound_slot_state& Slot, const SoundSourceState& Source, float** Data,
	float BeginFactor, float EndFactor, float BusGain, float Left, float Right,
	float BeginDry, float EndDry, float BeginFar, float EndFar,
	float BeginRoom, float EndRoom, float RoomReference,
    float** LocalData, float LocalBegin, float LocalEnd)
{
	const bool IsVoice = Snd_AcousticSourceKind(Slot) == 2;
	if (!IsVoice && (Slot.shot_group == 0 || Slot.shot_layer != 0)) return;
	const u32 Kind = (Slot.flags & (u16)Mixer::Flags::Spatial) != 0 ? 1u : 0u;
	auto& Meter = IsVoice ? GMixer.VoiceMeter : GMixer.ShotMeters[Kind];
	if (Meter.Event == 0)
	{
		Meter.Event = Slot.AcousticEvent;
		Meter.Group = Slot.shot_group;
		xr_strcpy(Meter.Sample, Source.Desc.name.c_str());
	}
	if ((IsVoice && Meter.Event != Slot.AcousticEvent) || (!IsVoice && Meter.Group != Slot.shot_group)) return;
	Meter.SourceZone = Slot.zone_idx;
	Meter.ListenerZone = GMixer.ListenerZone;
	Meter.DryGain = EndDry;
	Meter.RoomReference = RoomReference;
	float RoomStart2 = 0.0f, RoomCross = 0.0f, RoomEnd2 = 0.0f;
    float LocalStart2 = 0.0f, LocalCross = 0.0f, LocalEnd2 = 0.0f;
    float MixedStart = 0.0f, MixedCross = 0.0f, MixedEnd = 0.0f;
	for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
	{
		const float Reference = Snd_RoomReference(Slot, Idx);
        const float Start = BeginFactor * BeginRoom * Slot.RoomIncomingBeginWeights[Idx] / Reference;
        const float End = EndFactor * EndRoom * Slot.RoomIncomingWeights[Idx] / Reference;
        const float LocalStart = LocalBegin * BeginRoom * Slot.RoomLocalBeginWeights[Idx] / Reference;
        const float LocalEndGain = LocalEnd * EndRoom * Slot.RoomLocalWeights[Idx] / Reference;
        RoomStart2 += Start * Start;
        RoomCross += Start * End;
        RoomEnd2 += End * End;
        LocalStart2 += LocalStart * LocalStart;
        LocalCross += LocalStart * LocalEndGain;
        LocalEnd2 += LocalEndGain * LocalEndGain;
        // Both streams are summed before this bus's reverb. Include their
        // coherent cross term once; independent bus energies remain separate.
        MixedStart += Start * LocalStart;
        MixedCross += Start * LocalEndGain + End * LocalStart;
        MixedEnd += End * LocalEndGain;
	}
	for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
	{
		const float T = float(Frame) / float(SND_BLOCKSIZE - 1);
		const float OneMinusT = 1.0f - T;
		const float RoomGain2 = OneMinusT * OneMinusT * RoomStart2 +
			2.0f * T * OneMinusT * RoomCross + T * T * RoomEnd2;
		for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
		{
			const double Raw = double(Data[Channel][Frame]) * (Channel == 0 ? Left : Right);
			const double Direct = Raw * lerp(BeginFactor, EndFactor, T) * BusGain;
			// Match DSP_MixBufferPanning: interpolate endpoint products, not products
			// of independently interpolated fades and route gains.
			const double Dry = Raw * BusGain * lerp(BeginFactor * BeginDry, EndFactor * EndDry, T);
			const double Far = Raw * lerp(BeginFactor * BeginFar, EndFactor * EndFar, T);
			Meter.InputEnergy += Direct * Direct;
			Meter.DryEnergy += Dry * Dry;
			Meter.FarSendEnergy += Far * Far;
            const float LocalGain2 = OneMinusT * OneMinusT * LocalStart2 +
                2.0f * T * OneMinusT * LocalCross + T * T * LocalEnd2;
            const float MixedGain = OneMinusT * OneMinusT * MixedStart +
                T * OneMinusT * MixedCross + T * T * MixedEnd;
            const double LocalRaw = double(LocalData[Channel][Frame]);
            Meter.RoomSendEnergy += std::max(0.0, Raw * Raw * RoomGain2 +
                LocalRaw * LocalRaw * LocalGain2 + 2.0 * Raw * LocalRaw * MixedGain);
		}
	}
	Meter.Samples += u64(SND_BLOCKSIZE) * SND_CHANNEL_COUNT;
}

static void Snd_DirectSend(sound_slot_state& Slot, const SoundBus* Bus, const SoundSourceState& Source,
	float** Data, float BusGain, float& BeginFactor, float& EndFactor,
	float Left, float Right, float Indoor, float** EarlyData,
    float EarlyBegin, float EarlyEnd, float LocalBegin, float LocalEnd, bool IsShooting, bool ReceivesEnvironment)
{
	constexpr float OutdoorCriticalDistance = 25.0f;
	constexpr float IndoorCriticalDistance = 4.0f;
	constexpr float MinReverbDistance = 5.0f;

	bool IsSpatial = (Slot.flags & (u16)Mixer::Flags::Spatial) != 0;
	// Measured past min_distance: distance-band layers already carry the distance they were recorded at
	float Distance = IsSpatial ? GMixer.listener.Position.distance_to(Slot.parameters[(u32)Mixer::ParameterId::Position]) - Slot.parameters[(u32)Mixer::ParameterId::DistanceRange].x : 0.0f;
	Indoor = std::clamp(Indoor, 0.0f, 1.0f);
	float CriticalDistance = lerp(OutdoorCriticalDistance, IndoorCriticalDistance, Indoor);
	const bool EfxEnabled = psSoundFlags.is(ss_EFX);
	const bool FarAvailable = ReceivesEnvironment && IsShooting && EfxEnabled && Bus->FarSend != 0 &&
		GMixer.WetBusReady[Bus->FarSend - 1] && !Snd_IsPremixedShotTail(Source);
	const bool RoomAvailable = ReceivesEnvironment && EfxEnabled && Indoor > 0.0f && Bus->ZoneSend > 0.0f &&
		!Snd_IsPremixedShotTail(Source, true);
	// Diffuse outdoor IR is excited only by validated building reflections.
    // The finite ground reflection remains in the native early path in a field.
    const float Structure = Snd_HasRoomMetadata() ? g_NativeReflections.GetOutdoorStructure(Slot.AcousticEvent) : 1.0f;
    const float FarWeight = FarAvailable ? (1.0f - Indoor) * Structure : 0.0f;
	const float RoomWeight = RoomAvailable ? Indoor : 0.0f;
	const float Ratio = IsShooting ? std::max(Bus->DirectRatio, 0.0f) *
		std::max(Distance, MinReverbDistance) / CriticalDistance : 1.0f;
	const float FarReference = FarAvailable ? GMixer.WetBusReference[Bus->FarSend - 1] : 0.0f;
	float RoomReferenceEnergy = 0.0f;
	if (RoomAvailable)
	{
		for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
		{
			const float Reference = Slot.RoomWeights[Idx] * Bus->ZoneSend * GMixer.WetBusReference[Idx] / Snd_RoomReference(Slot, Idx);
			RoomReferenceEnergy += Reference * Reference / Indoor;
		}
	}
	// A missing/disabled wet path cannot take energy away from the direct sound.
	// Compensate using the configured processor response, not a unit-gain room.
	const float WetReferenceEnergy = FarWeight * FarReference * FarReference + RoomReferenceEnergy;
	const bool EarlyAvailable = ReceivesEnvironment && EfxEnabled && Snd_IsGeometryExciter(Slot);
    const float EarlyEnergy = EarlyAvailable ? g_NativeReflections.GetReferenceEnergy(Slot.AcousticEvent) : 0.0f;
    const float Dry = 1.0f / std::sqrt(1.0f + EarlyEnergy + Ratio * Ratio * WetReferenceEnergy);
    if (EarlyAvailable)
    {
        const float Previous = Slot.EarlySendGain < 0.0f ? Dry : Slot.EarlySendGain;
        g_NativeReflections.AddExcitation(Slot.AcousticEvent, EarlyData,
            EarlyBegin * Previous, EarlyEnd * Dry);
    }
    Slot.EarlySendGain = EarlyAvailable ? Dry : 0.0f;
	const float Wet = Ratio * Dry * BusGain;
	const float FarGain = Wet * std::sqrt(FarWeight);
	const float IndoorGain = RoomAvailable ? Wet * Bus->ZoneSend / std::sqrt(Indoor) : 0.0f;
	const float BeginFarGain = FarAvailable ? (Slot.FarSendGain < 0.0f ? FarGain : Slot.FarSendGain) : 0.0f;
	const float BeginRoomGain = EfxEnabled
		? (Slot.RoomSendScale < 0.0f ? IndoorGain : Slot.RoomSendScale) : 0.0f;
	if (FarAvailable)
	{
		Snd_SendToBus(Bus->FarSend, Data, BeginFactor * BeginFarGain,
			EndFactor * FarGain, Left, Right);
	}
	Slot.FarSendGain = FarGain;
	// Linear normalized room weights sum to <= 1: a shared preset is sent once,
	// different presets cannot double the room feed. Bus tails finish naturally.
	Snd_SendRoomWeights(Slot, Data, BeginFactor, EndFactor, IndoorGain, Left, Right,
        EarlyData, LocalBegin, LocalEnd);
	Slot.SendGain = Wet * std::sqrt(RoomWeight) * Bus->ZoneSend;
	GMixer.DryMin = std::min(GMixer.DryMin, Dry);
	GMixer.WetMax = std::max(GMixer.WetMax, std::max(FarGain, Slot.SendGain));

	// Restore a missing/disabled wet path to full dry through the existing
	// block interpolation, including a zero-gain processor during a transition.
	float PrevDry = Slot.ReverbDryGain < 0.0f ? Dry : Slot.ReverbDryGain;
	Snd_MeterShot(Slot, Source, Data, BeginFactor, EndFactor, BusGain, Left, Right,
		PrevDry, Dry, BeginFarGain, FarGain, BeginRoomGain, IndoorGain, std::sqrt(RoomReferenceEnergy),
        EarlyData, LocalBegin, LocalEnd);
	Slot.ReverbDryGain = Dry;
	BeginFactor *= PrevDry;
	EndFactor *= Dry;
}

static AcousticSourceDiagnostic* Snd_BeginPhysicalDiagnostic(u32 SlotIdx, sound_slot_state& Slot,
	const SoundSourceState& Source, const SoundBus& Bus, u32 BlockFrame)
{
	if (!Slot.PhysicalDiagnosticPending)
	{
		return nullptr;
	}
	Slot.PhysicalDiagnosticPending = false;
	if ((Slot.flags & (u16)Mixer::Flags::Spatial) == 0 || (Slot.flags & (u16)Mixer::Flags::Intro) != 0)
	{
		return nullptr;
	}
	const char* Name = Source.Desc.name.c_str();
	const u32 Kind = xr_strcmp(Bus.Name.c_str(), "creatures") == 0 || strstr(Name, "monsters\\") != nullptr ? 1u :
		xr_strcmp(Bus.Name.c_str(), "npc_voice") == 0 || strstr(Name, "characters_voice\\") != nullptr ? 2u :
		((Slot.shot_group != 0 && Slot.shot_layer == 0) ||
		 (Slot.shot_group == 0 && strstr(Name, "weapons\\") != nullptr)) ? 3u : 0u;
	if (Kind == 0)
	{
		return nullptr;
	}
	// Reserve eight creature records; NPC voices and primary world weapons get four each.
	if (GMixer.PhysicalDiagnosticCount == PhysicalDiagnosticCapacity ||
		GMixer.PhysicalDiagnosticKinds[Kind - 1] >= (Kind == 1 ? 8u : 4u))
	{
		++GMixer.PhysicalDiagnosticDropped;
		++GMixer.PhysicalDiagnosticDroppedKinds[Kind - 1];
		return nullptr;
	}
	++GMixer.PhysicalDiagnosticKinds[Kind - 1];
	auto& Record = GMixer.PhysicalDiagnostics[GMixer.PhysicalDiagnosticCount++];
	Record = {};
	Record.TimestampNS = Snd_GetTimestamp();
	Record.Event = Slot.AcousticEvent;
	Record.Slot = SlotIdx;
	Record.Group = Slot.shot_group;
	Record.Layer = Slot.shot_layer;
	Record.Kind = Kind;
	Record.Frame = BlockFrame;
	Record.Bus = Slot.bus;
	Record.Flags = Slot.flags;
	xr_strcpy(Record.Sample, Name);
	Record.Source = Slot.parameters[(u32)Mixer::ParameterId::Position];
	Record.Listener = GMixer.listener.Position;
	Record.SourceRoom = Snd_GetZoneRoom(Slot.zone_idx);
	Record.ListenerRoom = Snd_GetZoneRoom(GMixer.ListenerZone);
	Record.Distance = Record.Listener.distance_to(Record.Source);
	Record.MinRange = Slot.parameters[(u32)Mixer::ParameterId::DistanceRange].x;
	Record.MaxRange = Slot.parameters[(u32)Mixer::ParameterId::DistanceRange].y;
	Record.SourceGain = Slot.parameters[(u32)Mixer::ParameterId::VolumePerChannel].x *
		Slot.parameters[(u32)Mixer::ParameterId::VolumePerChannel].y;
	Record.Path = Slot.PathDiagnostic;
	Record.BusEnd = Bus.Volume * Bus.UserVolume;
	Record.BusBegin = Bus.Gain < 0.0f ? Record.BusEnd : Bus.Gain;
	return &Record;
}

static void Snd_FinishPhysicalDiagnostic(AcousticSourceDiagnostic* Record, float** Data,
	float BeginFactor, float EndFactor, float Left, float Right)
{
	if (Record == nullptr)
	{
		return;
	}
	Record->Left = Left;
	Record->Right = Right;
	Record->EnvelopeBegin = Record->DistanceWithVoiceGain * BeginFactor * Record->BusBegin;
	Record->EnvelopeEnd = Record->DistanceWithVoiceGain * EndFactor * Record->BusEnd;
	// Same PCM and interpolation as DSP_MixBufferPanning, before sum/bus effects.
	// Applying the bus fader alone is an estimate, not isolated bus DSP output.
	for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
	{
		const float T = float(Frame) / float(SND_BLOCKSIZE - 1);
		const float Factor = lerp(BeginFactor, EndFactor, T);
		const float Fader = lerp(Record->BusBegin, Record->BusEnd, T);
		for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
		{
			const float Sample = Data[Channel][Frame] * (Factor * (Channel == 0 ? Left : Right));
			const float FaderSample = Sample * Fader;
			Record->DryInputEnergy += double(Sample) * Sample;
			Record->FaderEquivalentEnergy += double(FaderSample) * FaderSample;
			Record->DryInputPeak = std::max(Record->DryInputPeak, std::abs(Sample));
			Record->FaderEquivalentPeak = std::max(Record->FaderEquivalentPeak, std::abs(FaderSample));
		}
	}
	Record->Samples = SND_BLOCKSIZE * SND_CHANNEL_COUNT;
}

ICF void Snd_RenderSlot(u32 SlotIdx, SoundSourceState& Source, float** process_buffer, float dt)
{
	auto& Slot = GMixer.slots[SlotIdx - 1];

	float OCCVolume = 1.0f;
    const u64 occlusionStart = Snd_GetTimestamp();
	ESlotOcclusionResult OCCResult = Snd_SlotOcclusion(SlotIdx, Source, dt, &OCCVolume);
    g_LiminalStageOccMax = std::max(g_LiminalStageOccMax,
        (u32)((Snd_GetTimestamp() - occlusionStart) / 1000));
	if (OCCResult == ESlotOcclusionResult::False)
	{
		++GMixer.SimulatedVoices;
		// TODO: hack for simulated sounds
		Slot.position = std::min(Slot.position + SND_BLOCKSIZE, Source.Desc.frames_total);
		if (Slot.position == Source.Desc.frames_total && (Slot.flags & (u16)Mixer::Flags::Looped) == 0)
		{
			MixerNewState(SlotIdx, Mixer::State::Stopped);
		}

		Snd_ReleaseVoiceEffects(Slot, SoundEffectOp::Reset);
		Slot.SendGain = 0.0f;
		Snd_ResetRoomWeights(Slot);
		Slot.FarSendGain = -1.0f;
		Slot.RoomSendScale = -1.0f;
		return;
	}

	// Clear process buffer and read data from source
	for (size_t Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
	{
		memset(process_buffer[Channel], 0, SND_BLOCKSIZE * sizeof(float));
	}

	const u32 DiagnosticBlockFrame = Slot.position;
	const u64 readStart = Snd_GetTimestamp();
    Snd_ProcessSlot(SlotIdx, Source, process_buffer);
    g_LiminalStageReadMax = std::max(g_LiminalStageReadMax,
        (u32)((Snd_GetTimestamp() - readStart) / 1000));

	const Fvector& Pos = Slot.parameters[(u32)Mixer::ParameterId::Position];
	const Fvector& Volume = Slot.parameters[(u32)Mixer::ParameterId::VolumePerChannel];
	float BeginFactor = 1.0f, EndFactor = 1.0f;

	// Deferred stopping
	bool IsIntro = (Slot.flags & (u16)Mixer::Flags::Intro);
    // Occlusion is applied once using the single source gain below.
    // SlotOcclusion now checks only playback eligibility and distance.
    OCCVolume = 1.0f;

	Slot.fade_volume = 1.f;

	if (Slot.stopping_position != (u32)-1)
	{
		u32 stopping_total = Source.Desc.frames_total - Slot.stopping_position;
		if (stopping_total > 1 && Slot.position >= Slot.stopping_position)
		{
			u32 begin_offset = Slot.position - Slot.stopping_position;
			u32 write_count = IsIntro ? SND_BLOCKSIZE : (u32)((float)SND_BLOCKSIZE * GMixer.time_factor);
			u32 end_offset = std::min(begin_offset + write_count, Source.Desc.frames_total - 1);
			BeginFactor = 1.0f - ((float)begin_offset / (float)(stopping_total - 1));
			EndFactor = 1.0f - ((float)end_offset / (float)(stopping_total - 1));
			BeginFactor = std::clamp(BeginFactor, 0.0f, 1.0f);
			EndFactor = std::clamp(EndFactor, 0.0f, 1.0f);
		}
	}

	float LeftPanning = Slot.parameters[(u32)Mixer::ParameterId::Panning].x;
	float RightPanning = Slot.parameters[(u32)Mixer::ParameterId::Panning].y;

	SoundBus* Bus = Snd_GetBus(Slot.bus);
	if (Bus == nullptr)
	{
		Slot.bus = Snd_GetMasterBus();
		Bus = Snd_GetBus(Slot.bus);
	}

	AcousticSourceDiagnostic* Diagnostic = Snd_BeginPhysicalDiagnostic(SlotIdx, Slot, Source, *Bus, DiagnosticBlockFrame);
	bool IsSpatial = (Slot.flags & (u32)Mixer::Flags::Spatial) != 0;
	bool IsMono = !IsIntro && Source.Desc.channels_count == 1;
    // The legacy engine silently bypassed positional effects for stereo world
    // sounds. In the installed content this includes creature attacks and NPC
    // speech. A real mono point source + binaural output localizes their
    // existing stereo recording without changing any OGG file. Explicit 2D
    // music, UI, and pre-recorded HUD stereo remain untouched.
    const bool IsWorldStereo = !IsIntro && IsSpatial &&
        Source.Desc.channels_count == 2 && Bus->VoiceEffectCount > 0;
    if (IsWorldStereo)
    {
        for (u32 frame = 0; frame < SND_BLOCKSIZE; ++frame)
        {
            const float point = 0.5f * (process_buffer[0][frame] + process_buffer[1][frame]);
            process_buffer[0][frame] = point;
            process_buffer[1][frame] = point;
        }
    }
    float EarlyPCM[SND_CHANNEL_COUNT][SND_BLOCKSIZE];
    float* EarlyData[SND_CHANNEL_COUNT] = {EarlyPCM[0], EarlyPCM[1]};
    for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
        memcpy(EarlyPCM[Channel], process_buffer[Channel], sizeof(EarlyPCM[Channel]));
    float EarlyAttenuation = std::min(Volume.x * Volume.y, 1.0f);
    if (IsSpatial)
    {
        SoundSpatialPosition Position = {};
        Position.MinDistance = std::max(Slot.parameters[(u32)Mixer::ParameterId::DistanceRange].x, EPS_S);
        Position.MaxDistance = std::max(Slot.parameters[(u32)Mixer::ParameterId::DistanceRange].y, Position.MinDistance + EPS_S);
        Position.Distance = std::clamp(GMixer.listener.Position.distance_to(Pos), Position.MinDistance, Position.MaxDistance);
        Position.Gain = Volume.x * Volume.y;
        float Rolloff = 1.0f;
        for (u32 EffectIndex = 0; EffectIndex < Bus->VoiceEffectCount; ++EffectIndex)
        {
            const auto& Effect = Bus->VoiceEffects[EffectIndex];
            const auto* Entry = Snd_GetEffect(Effect.Effect);
            if (!Entry) continue;
            for (u32 Param = 0; Param < Entry->Desc.ParamCount; ++Param)
                if (xr_strcmp(Entry->Desc.Params[Param].Name, "rolloff") == 0) Rolloff = Effect.Params[Param];
        }
        EarlyAttenuation = Snd_DistanceAttenuation(&Position, Rolloff);
    }
    const float LocalRoomBegin = BeginFactor * EarlyAttenuation;
    const float LocalRoomEnd = EndFactor * EarlyAttenuation;
    const float EarlyBegin = LocalRoomBegin * Bus->Volume * Bus->UserVolume;
    const float EarlyEnd = LocalRoomEnd * Bus->Volume * Bus->UserVolume;
	float Gain = Volume.x * Volume.y;
	if ((IsMono || IsWorldStereo) && IsSpatial)
	{
		PROF_EVENT("Slot Spatial");
		SoundEffectVoice Voice = {Pos, Slot.velocity, Slot.parameters[(u32)Mixer::ParameterId::DistanceRange], &Slot.doppler, Gain};
		Voice.AllowHrtf = Slot.shot_group == 0 || Slot.shot_layer == 0;
		SoundEffectProcess Process = {process_buffer, nullptr, &GMixer.listener, &Voice, true};
		const u64 spatialStart = Snd_GetTimestamp();
        Snd_ProcessVoiceEffects(Slot, Bus, &Process, Diagnostic);
        g_LiminalStageSpatialMax = std::max(g_LiminalStageSpatialMax,
            (u32)((Snd_GetTimestamp() - spatialStart) / 1000));
		Gain = Voice.Gain;
	}

    // The same bounded three-band filter is used by wall-fed room returns.
    // Published material energy is converted to pressure in GameMtlLib once.
    if (Slot.BarrierInitialized && IsSpatial && (Slot.flags & (u16)Mixer::Flags::NoOCC) == 0)
    {
        Slot.MaterialFilter.SetRatios(Slot.BarrierLowRatio, Slot.BarrierMidRatio, Slot.BarrierHighRatio, SND_SAMPLERATE);
        for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; ++Channel)
        {
            for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
            {
                process_buffer[Channel][Frame] = Slot.MaterialFilter.ProcessSample(Channel,
                    process_buffer[Channel][Frame], Frame, SND_BLOCKSIZE);
            }
        }
        Slot.MaterialFilter.EndBlock();
    }
    const bool noOcclusion = (Slot.flags & (u16)Mixer::Flags::NoOCC) != 0;
    // World emitters use the existing multi-ray CFORM room/portal path.
    // Non-world emitters use the unified CFORM/SOM Volume.z path. Never both.
    const float singleOcclusionGain = (!IsSpatial || noOcclusion) ? 1.0f
        : Slot.BarrierInitialized ? Slot.BarrierGain : Volume.z;
	float VolumeFinal = OCCVolume * std::min(Gain, 1.0f) * singleOcclusionGain * Slot.fade_volume;
	BeginFactor *= VolumeFinal;
	EndFactor *= VolumeFinal;

	float BusGain = Bus->Volume * Bus->UserVolume;
    const bool IsShooting = xr_strcmp(Bus->Name.c_str(), "shooting") == 0;
    const bool IsPhysical2D = (Slot.flags & (u16)Mixer::Flags::Physical2D) != 0;
    const bool ReceivesEnvironment = !IsIntro &&
        (IsSpatial || Slot.shot_group != 0 || IsPhysical2D) && psSoundFlags.is(ss_EFX);
    // The late field belongs to the source room. An outdoor source can
    // excite the listener room through its validated incoming path.
    const u32 SourceRoom = Snd_GetZoneRoom(Slot.zone_idx);
    const u32 RoomZone = SourceRoom != 0 && (IsSpatial || Slot.shot_group != 0) && !noOcclusion
        ? Slot.zone_idx : GMixer.ListenerZone;
    const u32 RoomBus = ReceivesEnvironment ? Snd_GetZoneBus(RoomZone) : 0u;
    const u32 SourceRoomBus = ReceivesEnvironment && (IsSpatial || Slot.shot_group != 0) && !noOcclusion
        ? Snd_GetZoneBus(Slot.zone_idx) : 0u;
    const float RoomFactor = Snd_UpdateRoomWeights(Slot, RoomBus, SourceRoomBus, ReceivesEnvironment);
    if (IsPhysical2D && RoomFactor > 0.0f)
    {
        ++GMixer.Physical2DVoices;
    }
	const float DiagnosticPreviousDry = Slot.ReverbDryGain;
    Snd_DirectSend(Slot, Bus, Source, process_buffer, BusGain, BeginFactor, EndFactor,
        LeftPanning, RightPanning, RoomFactor, EarlyData, EarlyBegin, EarlyEnd,
        LocalRoomBegin, LocalRoomEnd, IsShooting, ReceivesEnvironment);

	if (Diagnostic != nullptr)
	{
		Diagnostic->OcclusionGain = singleOcclusionGain;
		Diagnostic->ResidualVoiceGain = std::min(Gain, 1.0f);
		Diagnostic->DryBegin = DiagnosticPreviousDry < 0.0f ? Slot.ReverbDryGain : DiagnosticPreviousDry;
		Diagnostic->DryEnd = Slot.ReverbDryGain;
		Snd_FinishPhysicalDiagnostic(Diagnostic, process_buffer, BeginFactor, EndFactor, LeftPanning, RightPanning);
	}
	SoundAcousticCapture::TapVoice(Slot.AcousticEvent, SoundAcousticCapture::Stage::VoiceDry,
		Slot.bus, process_buffer, BeginFactor, EndFactor, LeftPanning, RightPanning);
	Snd_SendToBus(Slot.bus, process_buffer, BeginFactor, EndFactor, LeftPanning, RightPanning);

    // Snd_ProcessSlot may mark this slot stopped after decoding the final
    // PCM frames. The spatialization above may reacquire an HRTF to process
    // that final block. Release it here, *after* the last block reaches the
    // buses. Otherwise it remains pinned until the slot is reused/destroyed.
    if (Slot.state != Mixer::State::Playing)
        Snd_ReleaseVoiceEffects(Slot, SoundEffectOp::Reset);
}

void Snd_MixerRenderCallback(float* buffer)
{
	PROF_EVENT("Sound: Render Stage");

    const u64 callbackStart = Snd_GetTimestamp();
	xrSRWLockGuard Guard(GMixer.render_lock, true);
    const u32 renderLockWaitUS = (u32)((Snd_GetTimestamp() - callbackStart) / 1000);

	g_SoundStats.render_cache_miss = 0;

	static u64 TimeStamp = Snd_GetTimestamp();
	float dt = (float)((double)(Snd_GetTimestamp() - TimeStamp) / 1000000000.0);
	TimeStamp = Snd_GetTimestamp();

	static float _process_buffer[SND_CHANNEL_COUNT][SND_BLOCKSIZE] = {};

	float* process_buffer[SND_CHANNEL_COUNT] = {};
	for (size_t i = 0; i < SND_CHANNEL_COUNT; i++)
	{
		process_buffer[i] = _process_buffer[i];
	}

    Snd_BeginBuses();
    g_NativeReflections.BeginBlock();
	GMixer.Physical2DVoices = 0;
	GMixer.SimulatedVoices = 0;
	for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
	{
		GMixer.WetBusReady[Idx] = Snd_HasReverbBus(Idx + 1);
		GMixer.WetBusReference[Idx] = Snd_GetBusWetReference(Idx + 1);
	}
    u32 activeVoices = 0, shootingVoices = 0, premixedTails = 0;
    u32 groupedVoices = 0, groupedPrimary = 0, groupedSecondary = 0;
    u32 routedFootsteps = 0, routedNpc = 0, routedCreatures = 0;
    u32 routedGear = 0, routedImpacts = 0, routedActor = 0;
    u32 routedAnomalies = 0, routedEnvironment = 0, routedUi = 0;
    u32 routedShotDetails = 0, routedGeneric = 0;
    u32 worldStereoVoices = 0;
    u32 shotOutdoorToRoom = 0, shotBlockedOutside = 0;
    u32 shotLowestTransmissionPermille = 1000;
    u32 actorShotVoices = 0, distantShotVoices = 0;
    u32 portalRouted = 0, glassRouted = 0, dynamicRouted = 0;
    const u32 shootingBus = Snd_FindBus("shooting");
    const u32 footstepsBus = Snd_FindBus("footsteps");
    const u32 npcBus = Snd_FindBus("npc_voice");
    const u32 creaturesBus = Snd_FindBus("creatures");
    const u32 gearBus = Snd_FindBus("equipment");
    const u32 impactBus = Snd_FindBus("world_impacts");
    const u32 actorBus = Snd_FindBus("player_foley");
    const u32 anomaliesBus = Snd_FindBus("anomalies");
    const u32 environmentBus = Snd_FindBus("environment");
    const u32 uiBus = Snd_FindBus("ui");
    const u32 genericBus = Snd_FindBus("effects");
    const u64 voicesStart = Snd_GetTimestamp();

	for (size_t i = 0; i < GMixer.slots.size(); i++)
	{
		PROF_EVENT("Slot Render");
		// A new/delayed voice becomes audible only after its first complete
		// acoustic publication. Do not consume its first impulse while another
		// voice is being prepared between update lock sections.
		if (GMixer.slots[i].state != Mixer::State::Playing || !GMixer.slots[i].AcousticReady)
		{
			continue;
		}

		SoundSourceState* Source = Snd_FindSource(&GMixer.slots[i].sound_name);
		if (Source == nullptr)
		{
			MixerNewState(i + 1, Mixer::State::Stopped);
			continue;
		}

        ++activeVoices;
        if (GMixer.slots[i].state == Mixer::State::Playing &&
            GMixer.slots[i].BarrierInitialized)
        {
            const auto& path = GMixer.slots[i];
            if (path.BarrierPortal > 0.35f) ++portalRouted;
            if (path.BarrierWindow > 0.18f) ++glassRouted;
            if (path.BarrierDynamic > 0.15f) ++dynamicRouted;
        }
        if ((GMixer.slots[i].flags & (u16)Mixer::Flags::Spatial) &&
            Source->Desc.channels_count == 2)
            ++worldStereoVoices;
        const u32 sourceBus = GMixer.slots[i].bus;
        if (sourceBus == footstepsBus && footstepsBus) ++routedFootsteps;
        else if (sourceBus == npcBus && npcBus) ++routedNpc;
        else if (sourceBus == creaturesBus && creaturesBus) ++routedCreatures;
        else if (sourceBus == gearBus && gearBus) ++routedGear;
        else if (sourceBus == impactBus && impactBus) ++routedImpacts;
        else if (sourceBus == actorBus && actorBus) ++routedActor;
        else if (sourceBus == anomaliesBus && anomaliesBus) ++routedAnomalies;
        else if (sourceBus == environmentBus && environmentBus) ++routedEnvironment;
        else if (sourceBus == uiBus && uiBus) ++routedUi;
        else if (sourceBus == genericBus && genericBus) ++routedGeneric;
        if (GMixer.slots[i].shot_group)
        {
            ++groupedVoices;
            if (GMixer.slots[i].shot_layer == 0)
                ++groupedPrimary;
            else
                ++groupedSecondary;
            const auto& shot = GMixer.slots[i];
            if (!(shot.flags & (u16)Mixer::Flags::Spatial))
                ++actorShotVoices;
            else if (GMixer.listener.Position.distance_to(
                shot.parameters[(u32)Mixer::ParameterId::Position]) > 30.0f)
                ++distantShotVoices;
            if ((shot.flags & (u16)Mixer::Flags::Spatial) != 0 &&
                GMixer.IndoorFactor > 0.7f && shot.IndoorFactor < 0.35f)
            {
                ++shotOutdoorToRoom;
                if (shot.BarrierBlockage > 0.5f)
                {
                    ++shotBlockedOutside;
                    const u32 trans = (u32)(std::clamp(shot.BarrierGain, 0.0f, 1.0f) * 1000.0f);
                    shotLowestTransmissionPermille = std::min(shotLowestTransmissionPermille, trans);
                }
            }
        }
        if (GMixer.slots[i].bus == shootingBus)
        {
            ++shootingVoices;
            if (Snd_IsPremixedShotTail(*Source))
                ++premixedTails;
        }
        Snd_RenderSlot(i + 1, *Source, process_buffer, dt);
        Snd_ReleaseSource(&GMixer.slots[i].sound_name);
	}

    float Reflections[SND_CHANNEL_COUNT][SND_BLOCKSIZE] = {};
    float* ReflectedData[SND_CHANNEL_COUNT] = {Reflections[0], Reflections[1]};
    g_NativeReflections.Render(ReflectedData);
    if (psSoundFlags.is(ss_EFX))
        Snd_SendToBus(Snd_GetMasterBus(), ReflectedData, 1.0f, 1.0f, 1.0f, 1.0f);
	const u32 voiceWorkUS = (u32)((Snd_GetTimestamp() - voicesStart) / 1000);
    const u64 busStart = Snd_GetTimestamp();
	{
		PROF_EVENT("Sound Mixing");
		Snd_RenderBuses(buffer, GMixer.mute_volume);
		SoundAcousticCapture::EndBlock();
	}
    const u32 busWorkUS = (u32)((Snd_GetTimestamp() - busStart) / 1000);

#ifdef DEBUG_DRAW
	float Volumes[SND_CHANNEL_COUNT] = {};
	for (size_t Block = 0; Block < SND_BLOCKSIZE; Block++)
	{
		for (size_t Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
		{
			Volumes[Channel] = (Volumes[Channel] + fabs(buffer[Block * SND_CHANNEL_COUNT + Channel])) * 0.5f;
		}
	}

	for (size_t i = 0; i < SND_CHANNEL_COUNT; i++)
	{
		Volumes[i] = lin2dB(Volumes[i]);
	}

	memcpy(g_SoundStats.channel_volumes, Volumes, sizeof(Volumes));
#endif

    g_SoundStats.render_time_micros = (Snd_GetTimestamp() - TimeStamp) / 1000;
    static u32 peakRenderUS = 0;
    static u32 peakRenderLockUS = 0;
    static u32 peakVoiceUS = 0;
    static u32 peakBusUS = 0;
    peakRenderUS = std::max(peakRenderUS, g_SoundStats.render_time_micros);
    peakRenderLockUS = std::max(peakRenderLockUS, renderLockWaitUS);
    peakVoiceUS = std::max(peakVoiceUS, voiceWorkUS);
    peakBusUS = std::max(peakBusUS, busWorkUS);
	if (g_SoundStats.render_time_micros > u32(1000000ull * SND_BLOCKSIZE / SND_SAMPLERATE))
	{
		++GMixer.CallbackOverBudget;
	}
    static u64 lastAudioTelemetry = 0;
    const u64 stamp = Snd_GetTimestamp();
    if (stamp - lastAudioTelemetry > 5000000000ULL)
    {
        lastAudioTelemetry = stamp;
        // Never log or allocate on the real-time mixer callback.
        g_LiminalMetricsVoices.store(activeVoices, std::memory_order_relaxed);
        g_LiminalMetricsShots.store(shootingVoices, std::memory_order_relaxed);
        g_LiminalGroupedVoices.store(groupedVoices, std::memory_order_relaxed);
        g_LiminalGroupedPrimary.store(groupedPrimary, std::memory_order_relaxed);
        g_LiminalGroupedSecondary.store(groupedSecondary, std::memory_order_relaxed);
        g_LiminalActorShotVoices.store(actorShotVoices, std::memory_order_relaxed);
        g_LiminalDistantShotVoices.store(distantShotVoices, std::memory_order_relaxed);
        g_LiminalPortalRouted.store(portalRouted, std::memory_order_relaxed);
        g_LiminalGlassRouted.store(glassRouted, std::memory_order_relaxed);
        g_LiminalDynamicRouted.store(dynamicRouted, std::memory_order_relaxed);
        g_LiminalRoutedFootsteps.store(routedFootsteps, std::memory_order_relaxed);
        g_LiminalRoutedNpc.store(routedNpc, std::memory_order_relaxed);
        g_LiminalRoutedCreatures.store(routedCreatures, std::memory_order_relaxed);
        g_LiminalRoutedGear.store(routedGear, std::memory_order_relaxed);
        g_LiminalRoutedImpacts.store(routedImpacts, std::memory_order_relaxed);
        g_LiminalRoutedActor.store(routedActor, std::memory_order_relaxed);
        g_LiminalRoutedAnomalies.store(routedAnomalies, std::memory_order_relaxed);
        g_LiminalRoutedEnvironment.store(routedEnvironment, std::memory_order_relaxed);
        g_LiminalRoutedUi.store(routedUi, std::memory_order_relaxed);
        g_LiminalRoutedShotDetails.store(routedShotDetails, std::memory_order_relaxed);
        g_LiminalRoutedGeneric.store(routedGeneric, std::memory_order_relaxed);
        g_LiminalWorldStereoVoices.store(worldStereoVoices, std::memory_order_relaxed);
        g_LiminalOutdoorToRoomVoices.store(shotOutdoorToRoom, std::memory_order_relaxed);
        g_LiminalBlockedOutdoorVoices.store(shotBlockedOutside, std::memory_order_relaxed);
        g_LiminalLowestTransmission.store(shotLowestTransmissionPermille, std::memory_order_relaxed);
        g_LiminalMetricsTails.store(premixedTails, std::memory_order_relaxed);
        g_LiminalMetricsSyncCalls.store(g_LiminalSyncDecodeCalls, std::memory_order_relaxed);
        g_LiminalMetricsSyncPeak.store(g_LiminalSyncDecodePeakUS, std::memory_order_relaxed);
        g_LiminalMetricsCacheMiss.store(g_SoundStats.render_cache_miss, std::memory_order_relaxed);
        g_LiminalMetricsMixerUS.store(g_SoundStats.render_time_micros, std::memory_order_relaxed);
        g_LiminalRenderPeakUS.store(peakRenderUS, std::memory_order_relaxed);
        g_LiminalRenderLockPeakUS.store(peakRenderLockUS, std::memory_order_relaxed);
        g_LiminalVoicePeakUS.store(peakVoiceUS, std::memory_order_relaxed);
        g_LiminalBusPeakUS.store(peakBusUS, std::memory_order_relaxed);
        g_LiminalMaxOcclusionUS.store(g_LiminalStageOccMax, std::memory_order_relaxed);
        g_LiminalMaxSourceReadUS.store(g_LiminalStageReadMax, std::memory_order_relaxed);
        g_LiminalMaxSpatializeUS.store(g_LiminalStageSpatialMax, std::memory_order_relaxed);
        g_LiminalStageOccMax = g_LiminalStageReadMax = g_LiminalStageSpatialMax = 0;
        peakRenderUS = peakRenderLockUS = peakVoiceUS = peakBusUS = 0;
        g_LiminalMetricsReady.store(true, std::memory_order_release);
        g_LiminalSyncDecodeCalls = g_LiminalSyncDecodePeakUS = 0;
    }
	// Include lock wait, PCM, bus processing and telemetry publication up to callback exit.
	const u64 CallbackTotalNS = Snd_GetTimestamp() - callbackStart;
	GMixer.CallbackTotalPeakUS = std::max(GMixer.CallbackTotalPeakUS, u32(CallbackTotalNS / 1000));
	if (CallbackTotalNS > (u64(SND_BLOCKSIZE) * 1000000000ull / SND_SAMPLERATE))
	{
		++GMixer.CallbackDeadlineMisses;
	}
}

void Mixer::Initialize()
{
	GMixer.slots.clear();
	GMixer.free_slots.clear();
	GMixer.sounds.clear();
	GMixer.cmd.clear();
	GMixer.cmd.reserve(256);
	GMixer.PhysicalDiagnosticCount = 0;
	GMixer.PhysicalDiagnosticDropped = 0;
	GMixer.VoiceMeter = {};
	for (u32& Count : GMixer.PhysicalDiagnosticKinds) Count = 0;
	for (u32& Count : GMixer.PhysicalDiagnosticDroppedKinds) Count = 0;
	Snd_GrowSlots(true);

	Snd_InitEffects();
	Snd_LoadConfig();
	Snd_InitBuses();
	g_NativeReflections.Reset();
	GMixer.ListenerZone = 0;
	GMixer.IndoorFactor = 0.0f;
	GMixer.RoomGeometry = {};
	memset(GMixer.ListenerRoomWeights, 0, sizeof(GMixer.ListenerRoomWeights));
	GMixer.DryMin = 1.0f;
	GMixer.WetMax = 0.0f;
	GMixer.RoomTransitions = 0;
	GMixer.CallbackOverBudget = 0;
	for (auto& Meter : GMixer.ShotMeters)
	{
		Meter = {};
	}
	GMixer.compression = -1.0f;
	GMixer.shooting_reverb = -1.0f;
	GMixer.default_bus = Snd_FindBus("effects");
	GMixer.default_bus = GMixer.default_bus != 0 ? GMixer.default_bus : Snd_GetMasterBus();

	Snd_InitSources();

	Backend::Initialize(Snd_MixerRenderCallback, Snd_PrecacheRenderCallback);
}

void Mixer::Shutdown()
{
	Backend::Shutdown();
	SoundAcousticCapture::Shutdown();
	g_NativeReflections.Reset();

	for (sound_slot_state& Slot : GMixer.slots)
	{
		Snd_ReleaseVoiceEffects(Slot, SoundEffectOp::Destroy);
	}

	Snd_ShutdownBuses();
	Resonance_Shutdown();
	Snd_ShutdownSources();
	Snd_UnloadConfig();

	GMixer.slots.clear();
	GMixer.free_slots.clear();
	GMixer.sounds.clear();
	GMixer.cmd.clear();
}

ICF void DestroyInternal(int slot)
{
	if (slot == 0)
	{
		return;
	}

	if (!GMixer.slots[slot - 1].sound_name.empty())
	{
		Snd_ReleaseSource(&GMixer.slots[slot - 1].sound_name);
		GMixer.slots[slot - 1].sound_name.clear();
	}

	memset(GMixer.slots[slot - 1].parameters, 0, sizeof(GMixer.slots[slot - 1].parameters));
	memset(GMixer.slots[slot - 1].history, 0, sizeof(GMixer.slots[slot - 1].history));
	GMixer.slots[slot - 1].position = 0;
	GMixer.slots[slot - 1].stopping_position = (u32)-1;
	GMixer.slots[slot - 1].flags = 0;
	Snd_ReleaseVoiceEffects(GMixer.slots[slot - 1], SoundEffectOp::Destroy);
	GMixer.slots[slot - 1].bus = 0;
	GMixer.slots[slot - 1].shot_group = 0;
    GMixer.slots[slot - 1].shot_layer = 0;
    GMixer.slots[slot - 1].SendBus = 0;
	GMixer.slots[slot - 1].SendGain = 0.0f;
	GMixer.slots[slot - 1].state = Mixer::State::Stopped;
	GMixer.slots[slot - 1].prev_state = Mixer::State::Stopped;
	GMixer.slots[slot - 1].fake_state = Mixer::State::Stopped;
	GMixer.slots[slot - 1].fade_volume = 1.0f;
	GMixer.free_slots.push_back(slot);
}

void Mixer::Update(void* event_handler, float time_factor, float mute_volume, float compression, Fvector P, Fvector D, Fvector N)
{
	PROF_EVENT("Sound: Update Stage");
	sound_event* Handler = (sound_event*)event_handler;

	// Producers may take play_lock before allocating a slot. Drain a complete
	// command batch before taking the render/manage/update locks; never hold
	// the queue mutex while yielding to the audio callback.
	xr_vector<SoundCommand> Commands;
	{
		xrCriticalSectionGuard QueueGuard(GMixer.play_lock);
		Commands.swap(GMixer.cmd);
	}

	// Cold OGG/metadata work holds only a shared render guard: callbacks may
	// continue, while editor config/bus mutation remains excluded. One owned
	// reference per play command survives the later exclusive-lock transition.
	struct PreparedSource
	{
		xr_string Name;
		SoundSourceState* Source = nullptr;
		PreparedSource() = default;
		PreparedSource(const PreparedSource&) = delete;
		PreparedSource& operator=(const PreparedSource&) = delete;
		PreparedSource(PreparedSource&& Other) noexcept : Name(std::move(Other.Name)), Source(Other.Source)
		{
			Other.Source = nullptr;
		}
		~PreparedSource()
		{
			if (Source != nullptr) Snd_ReleaseSource(&Name);
		}
	};
	static u32 PeakSourcePreloadUS = 0, PeakAIPropagationUS = 0;
	xr_vector<PreparedSource> PreparedSources(Commands.size());
	const u64 SourcePreloadStart = Snd_GetTimestamp();
	{
		xrSRWLockGuard SourceReadGuard(GMixer.render_lock, true);
		for (size_t Index = 0; Index < Commands.size(); ++Index)
		{
			const auto& Command = Commands[Index];
			if (Command.id != ESoundMixerCommands::play) continue;
			auto& Prepared = PreparedSources[Index];
			Prepared.Name = Command.string_storage.size() ? Command.string_storage.c_str() : "";
			Prepared.Source = Snd_AcquireSource(&Prepared.Name);
			if (Prepared.Source != nullptr) Snd_QueueDecode(&Prepared.Name, 0);
		}
	}
	PeakSourcePreloadUS = std::max(PeakSourcePreloadUS, (u32)((Snd_GetTimestamp() - SourcePreloadStart) / 1000));

	static u64 TimeStamp = Snd_GetTimestamp();
	GMixer.dt = (float)((Snd_GetTimestamp() - TimeStamp) / 1000000) * 0.001f;
	TimeStamp = Snd_GetTimestamp();

	GMixer.time_factor = std::clamp(time_factor, 0.1f, 10.0f);
	GMixer.mute_volume = mute_volume;

    const u64 lockStart = Snd_GetTimestamp();
    static u32 peakUpdateLockWaitUS = 0;
    static u32 peakUpdateWorkUS = 0;
    static u32 peakListenerUS = 0, peakRefSoundsUS = 0;
    static u32 peakSlotsUS = 0, peakCommandsUS = 0, peakEventsUS = 0;
    static u32 peakCommandCount = 0;
	static u32 PeakLockHoldUS = 0, PeakCommandBatchUS = 0, PeakRoomReturnsUS = 0, PeakSourceAcousticsUS = 0;
	GMixer.render_lock.AcquireExclusive();
	u64 LockSectionStart = Snd_GetTimestamp();
	GMixer.manage_lock.AcquireExclusive();
	GMixer.update_lock.AcquireExclusive();
    const u32 waitUS = (u32)((Snd_GetTimestamp() - lockStart) / 1000);
    peakUpdateLockWaitUS = std::max(peakUpdateLockWaitUS, waitUS);
    const u64 updateWorkStart = Snd_GetTimestamp();
	const auto ReleaseForAudio = [&]()
	{
		PeakLockHoldUS = std::max(PeakLockHoldUS, (u32)((Snd_GetTimestamp() - LockSectionStart) / 1000));
		GMixer.update_lock.ReleaseExclusive();
		GMixer.manage_lock.ReleaseExclusive();
		GMixer.render_lock.ReleaseExclusive();
		// No slot/bus references cross this boundary. Preserve the existing
		// lock order, and serialize every complete geometry/DSP publication.
		const u64 WaitStart = Snd_GetTimestamp();
		GMixer.render_lock.AcquireExclusive();
		LockSectionStart = Snd_GetTimestamp();
		GMixer.manage_lock.AcquireExclusive();
		GMixer.update_lock.AcquireExclusive();
		peakUpdateLockWaitUS = std::max(peakUpdateLockWaitUS, (u32)((Snd_GetTimestamp() - WaitStart) / 1000));
	};
	struct PendingSoundEvent
	{
		ref_sound_data_ptr Sound;
		float Range;
	};
	const auto PropagateWithAudio = [&](const xr_vector<PendingSoundEvent>& Events)
	{
		if (Events.empty() || Handler == nullptr) return;
		PeakLockHoldUS = std::max(PeakLockHoldUS, (u32)((Snd_GetTimestamp() - LockSectionStart) / 1000));
		GMixer.update_lock.ReleaseExclusive();
		GMixer.manage_lock.ReleaseExclusive();
		GMixer.render_lock.ReleaseExclusive();
		const u64 PropagationStart = Snd_GetTimestamp();
		{
			// The native handler reads slot parameters/flags, then performs game
			// spatial and occlusion queries. Keep that data stable and owned while
			// allowing the audio callback's shared render access. Game objects are
			// registered on this same game update thread; no object work is deferred.
			xrSRWLockGuard RenderReadGuard(GMixer.render_lock, true);
			xrSRWLockGuard ManageGuard(GMixer.manage_lock);
			xrSRWLockGuard UpdateGuard(GMixer.update_lock);
			for (const auto& Event : Events)
			{
				Handler(Event.Sound, Event.Range);
			}
		}
		PeakAIPropagationUS = std::max(PeakAIPropagationUS, (u32)((Snd_GetTimestamp() - PropagationStart) / 1000));
		const u64 WaitStart = Snd_GetTimestamp();
		GMixer.render_lock.AcquireExclusive();
		LockSectionStart = Snd_GetTimestamp();
		GMixer.manage_lock.AcquireExclusive();
		GMixer.update_lock.AcquireExclusive();
		peakUpdateLockWaitUS = std::max(peakUpdateLockWaitUS, (u32)((Snd_GetTimestamp() - WaitStart) / 1000));
	};
    ++g_AcousticPathFrame;
    g_AcousticPortalRayBudget = 96;
    g_AcousticDirectRayBudget = 112;
    g_AcousticDynamicRayBudget = 48;
    g_NativeReflections.BeginUpdate(TimeStamp);
    Snd_BeginRoomUpdate();
    g_AcousticDetailProbeBudget = 2;

	GMixer.listener.Velocity = Snd_Velocity(GMixer.listener.Position, P);
	GMixer.listener.Position = P;
	GMixer.listener.Direction = D;
	GMixer.listener.Normal = N;

	if (GMixer.compression != compression)
	{
		GMixer.compression = compression;
		Snd_SetBusParam(Snd_GetMasterBus(), "compressor", "mix", compression);
	}

	SoundBus* ShootingBus = Snd_GetBus(Snd_FindBus("shooting"));
	if (ShootingBus != nullptr && GMixer.shooting_reverb != psSoundShootingReverb)
	{
		GMixer.shooting_reverb = psSoundShootingReverb;
		ShootingBus->DirectRatio = psSoundShootingReverb;
	}

	GMixer.ListenerZone = Snd_FindZone(GMixer.listener.Position);
    Snd_PrepareActiveRooms();
    const auto ListenerRoomBlend = Snd_ResolveRoomBlend(GMixer.listener.Position,
        Snd_GetZoneRoom(GMixer.ListenerZone), false);
	const float listenerIndoorTarget = Snd_HasRoomMetadata() ? Snd_RoomBlendIndoor(ListenerRoomBlend)
        : GMixer.ListenerZone != 0 ? 1.0f : 0.0f;
	GMixer.IndoorFactor += (listenerIndoorTarget - GMixer.IndoorFactor) *
		(1.0f - std::exp(-std::max(GMixer.dt, 0.0f) * 4.5f));
	const float ListenerBlend = 1.0f - std::exp(-std::max(GMixer.dt, 0.0f) * 4.5f);
	const u32 ListenerRoomBus = psSoundFlags.is(ss_EFX) ? Snd_GetZoneBus(GMixer.ListenerZone) : 0u;
    const u32 ListenerRoomBuses[2] = {Snd_GetRoomBus(ListenerRoomBlend.Rooms[0]),
        Snd_GetRoomBus(ListenerRoomBlend.Rooms[1])};
	for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
	{
		float& Weight = GMixer.ListenerRoomWeights[Idx];
		const bool Ready = Snd_HasReverbBus(Idx + 1);
        float Target = 0.0f;
        if (Ready && psSoundFlags.is(ss_EFX))
        {
            if (Snd_HasRoomMetadata())
            {
                for (u32 Side = 0; Side < 2; ++Side)
                    if (ListenerRoomBuses[Side] == Idx + 1) Target += ListenerRoomBlend.Weights[Side];
            }
            else Target = Idx + 1 == ListenerRoomBus ? 1.0f : 0.0f;
        }
		Weight = Ready ? Weight + (Target - Weight) * ListenerBlend : 0.0f;
		if (Target == 0.0f && Weight < 0.00001f)
		{
			Weight = 0.0f;
		}
	}
	// Retain measured apertures for the existing transmission path only.
	// The environment bus and all reverb parameters come from level.snd_env.
	Snd_HemiIndoorFactor(GMixer.listener.Position, nullptr, nullptr, nullptr, nullptr,
		&GMixer.RoomGeometry);
    const u64 listenerPhaseEnd = Snd_GetTimestamp();
    peakListenerUS = std::max(peakListenerUS, (u32)((listenerPhaseEnd - updateWorkStart) / 1000));

	xr_vector<PendingSoundEvent> PeriodicEvents;
	for (auto& RefSound : GMixer.sounds)
	{
		if (RefSound == nullptr || !RefSound->slot() || RefSound->_g_object() == nullptr || !RefSound->unique_id())
		{
			continue;
		}

		auto& Slot = GMixer.slots[RefSound->slot() - 1];

		if (Slot.fake_state != State::Playing || Slot.state != State::Playing)
		{
			continue;
		}

		CObject* Object = RefSound->_g_object();
		if (Slot.flags & (u16)Flags::Spatial && (Slot.flags & (u16)Flags::NoPosUpdate) == 0)
		{
			if (Object != nullptr)
			{
				auto& Pos = Slot.parameters[(u32)Mixer::ParameterId::Position];
				Pos = ((IRenderable*)Object)->renderable.xform.c;
			}
		}

		// Periodic AI sound-event propagation:
		// re-emit the sound event every s_f_def_event_pulse seconds while the sound
		// is playing so NPCs keep tracking ongoing / moving / looped fire.
		if (RefSound->_p != nullptr && RefSound->_p->g_type != 0)
		{
			RefSound->TimeToPropagade -= GMixer.dt;
			if (RefSound->TimeToPropagade <= 0.0f)
			{
				RefSound->TimeToPropagade = s_f_def_event_pulse;
				if (Handler != nullptr)
				{
					const Fvector& Dist = Slot.parameters[(u32)Mixer::ParameterId::DistanceRange];
					const float SndVolume = Slot.parameters[(u32)Mixer::ParameterId::VolumePerChannel].y;
					float Clip = Dist.z * SndVolume;
					float Range = std::min(Dist.z, Clip);
					if (Range >= 0.1f)
					{
						PeriodicEvents.push_back({RefSound->_p, Range});
					}
				}
			}
		}
	}
	// Preserve periodic notification order: these still precede new commands.
	PropagateWithAudio(PeriodicEvents);
    const u64 refSoundsPhaseEnd = Snd_GetTimestamp();
    peakRefSoundsUS = std::max(peakRefSoundsUS, (u32)((refSoundsPhaseEnd - listenerPhaseEnd) / 1000));

    const u64 slotSweepPhaseEnd = Snd_GetTimestamp();
    peakSlotsUS = std::max(peakSlotsUS, (u32)((slotSweepPhaseEnd - refSoundsPhaseEnd) / 1000));

	xr_vector<ref_sound_data_ptr> PendingEvents;
    peakCommandCount = std::max(peakCommandCount, (u32)Commands.size());
	const u64 CommandBatchStart = Snd_GetTimestamp();
	for (size_t CommandIndex = 0; CommandIndex < Commands.size(); ++CommandIndex)
	{
		const auto& Command = Commands[CommandIndex];
		switch (Command.id)
		{
			case ESoundMixerCommands::play:
			{
				bool IsSoundExists = (Command.param1 && GMixer.sounds.contains((ref_sound*)Command.param1));
				ref_sound* RefSound = IsSoundExists ? (ref_sound*)Command.param1 : nullptr;
				u16 Flags = Command.param0;
				// Overlapping playback passes its emitter in the command, without changing the reusable sound reference.
				CObject* PlaybackObject = (Flags & (u16)Flags::NoFeedback) ?
					reinterpret_cast<CObject*>(Command.param3) : (RefSound ? RefSound->_g_object() : nullptr);

				auto& ActualSlot = GMixer.slots[Command.slot - 1];
				Snd_ReleaseVoiceEffects(ActualSlot, SoundEffectOp::Destroy);
				const xr_string NewName = Command.string_storage.size() ? Command.string_storage.c_str() : "";
				bool IsSameFile = ActualSlot.sound_name == NewName;
				if (!IsSameFile && !ActualSlot.sound_name.empty())
				{
					Snd_ReleaseSource(&ActualSlot.sound_name);
					ActualSlot.sound_name.clear();
				}

				// Transfer the prepared reference to the new slot. A same-file
				// replay already owns its slot reference and releases the extra pin.
				SoundSourceState* SourcePtr = PreparedSources[CommandIndex].Source;
				PreparedSources[CommandIndex].Source = nullptr;

				if (SourcePtr == nullptr)
				{
					MixerNewState(Command.slot, State::Stopped);
					break;
				}

				if (IsSameFile)
				{
					// The slot already holds a reference on it.
					Snd_ReleaseSource(&NewName);
				}

				auto& Source = *SourcePtr;
				memset(ActualSlot.parameters, 0, sizeof(ActualSlot.parameters));
				memset(ActualSlot.history, 0, sizeof(ActualSlot.history));
				ActualSlot.parameters[(u32)Mixer::ParameterId::VolumePerChannel] = Fvector(Source.Desc.volume * Command.shot_layer_gain, 1.0f, 1.0f);
				ActualSlot.parameters[(u32)Mixer::ParameterId::DistanceRange] = Fvector(Source.Desc.min_distance, Source.Desc.max_distance, Source.Desc.max_ai_distance);
				ActualSlot.parameters[(u32)Mixer::ParameterId::Pitch] = Fvector{1.0f, 1.0f, 1.0f};
				ActualSlot.parameters[(u32)Mixer::ParameterId::Panning] = Fvector{1.0f, 1.0f, 1.0f};
				ActualSlot.position = 0;
				ActualSlot.stopping_position = (u32)-1;
				ActualSlot.sound_name = NewName;
				ActualSlot.AcousticStartPending = true;
				ActualSlot.AcousticReady = false;
				ActualSlot.flags = Flags;
				ActualSlot.bus = Snd_GetBus(Command.bus) != nullptr ? Command.bus : Snd_GetBus(Source.Desc.bus) != nullptr ? Source.Desc.bus : GMixer.default_bus;
                ActualSlot.shot_group = Command.shot_group;
                ActualSlot.shot_layer = Command.shot_layer;
                ActualSlot.BarrierBlockage = 0.0f;
                ActualSlot.BarrierGain = 1.0f;
                ActualSlot.BarrierMismatch = 0.0f;
                ActualSlot.BarrierPortal = 0.0f;
                ActualSlot.BarrierWindow = 0.0f;
                ActualSlot.BarrierDynamic = 0.0f;
                ActualSlot.BarrierLowRatio = ActualSlot.BarrierMidRatio = ActualSlot.BarrierHighRatio = 1.0f;
                ActualSlot.MaterialFilter.Reset();
                memset(ActualSlot.BarrierFoliageLowPass, 0, sizeof(ActualSlot.BarrierFoliageLowPass));
                ActualSlot.BarrierInitialized = false;
				ActualSlot.PathDiagnostic = {};
				ActualSlot.PhysicalDiagnosticPending = true;
                ActualSlot.AcousticEvent = ActualSlot.shot_group != 0 ? u64(ActualSlot.shot_group)
                    : (1ull << 63) | ++g_AcousticVoiceSerial;
                ActualSlot.EarlySendGain = -1.0f;
                for (float& previous : ActualSlot.BarrierLowPass)
                    previous = 0.0f;
				ActualSlot.SendBus = 0;
				ActualSlot.SendGain = 0.0f;
				ActualSlot.PreviousRoomBus = 0;
				Snd_ResetRoomWeights(ActualSlot);
				ActualSlot.FarSendGain = -1.0f;
				ActualSlot.RoomSendScale = -1.0f;
				ActualSlot.fade_volume = 0.0f;
				ActualSlot.ReverbDryGain = -1.0f;
				ActualSlot.IndoorFactor = GMixer.IndoorFactor;
                ActualSlot.IndoorFactorValid = false;
                ActualSlot.IndoorNextProbeNS = 0;
                ActualSlot.IndoorLastProbeNS = 0;
				ActualSlot.zone_idx = 0;

				// First-line decoding was queued with the owned preload reference.

				if (ActualSlot.flags & (u16)Flags::Spatial && RefSound != nullptr && PlaybackObject != nullptr)
				{
					ActualSlot.parameters[(u32)Mixer::ParameterId::Position] = ((IRenderable*)PlaybackObject)->renderable.xform.c;
				}

				ActualSlot.doppler = 1.0f;
				ActualSlot.velocity.set(0.0f, 0.0f, 0.0f);
				ActualSlot.prev_position = ActualSlot.parameters[(u32)Mixer::ParameterId::Position];

				if (Handler && RefSound && RefSound->_p && PlaybackObject)
				{
					int GameType = RefSound->_p->g_type;
					if (GameType == int(sg_SourceType))
					{
						GameType = int(Source.Desc.game_type);
						RefSound->_p->g_type = GameType;
					}
					// Preserve each playback's emitter and type until all queued parameters are applied.
					ref_sound_data_ptr Data = new ref_sound_data();
					Data->slot = Command.slot;
					Data->g_type = GameType;
					Data->g_object = PlaybackObject;
					Data->g_userdata = RefSound->_p->g_userdata;
					Data->dont_destroy_slot = true;
					Data->fn_attached[0] = Source.Desc.path;
					PendingEvents.push_back(Data);
				}

				if (!fis_zero(Command.param2))
				{
					ActualSlot.delay = (float)*(double*)&Command.param2;
					MixerNewState(Command.slot, State::Delay);
				}
				else
				{
					MixerNewState(Command.slot, State::Playing);
				}
			}
			break;
			case ESoundMixerCommands::pause:
			{
				MixerNewState(Command.slot, State::Paused);
			}
			break;
			case ESoundMixerCommands::stop:
			{
				if (Command.param0 && GMixer.slots[Command.slot - 1].state != State::Delay)
				{
					GMixer.slots[Command.slot - 1].flags &= ~((u8)Flags::Looped);
					GMixer.slots[Command.slot - 1].stopping_position = GMixer.slots[Command.slot - 1].position;
				}
				else
				{
					MixerNewState(Command.slot, State::Stopped);
					GMixer.slots[Command.slot - 1].position = 0;
					GMixer.slots[Command.slot - 1].stopping_position = (u32)-1;
				}
			}
			break;
			case ESoundMixerCommands::destroy:
			{
				DestroyInternal(Command.slot);
			}
			break;
			case ESoundMixerCommands::stop_all:
			{
				g_NativeReflections.Reset();
				for (size_t Iter = 0; Iter < GMixer.slots.size(); Iter++)
				{
					if (GMixer.slots[Iter].sound_name.size() && GMixer.slots[Iter].state != State::Stopped)
					{
						GMixer.slots[Iter].position = 0;
						GMixer.slots[Iter].stopping_position = (u32)-1;
						Snd_ReleaseVoiceEffects(GMixer.slots[Iter], SoundEffectOp::Reset);
						GMixer.slots[Iter].prev_state = GMixer.slots[Iter].state;
						GMixer.slots[Iter].state = State::Stopped;
						GMixer.slots[Iter].fake_state = State::Stopped;
					}
				}
			}
			break;
			case ESoundMixerCommands::pause_all:
			{
				for (size_t Iter = 0; Iter < GMixer.slots.size(); Iter++)
				{
					if (GMixer.slots[Iter].sound_name.size() && GMixer.slots[Iter].state != State::Stopped && GMixer.slots[Iter].state != State::Paused)
					{
						Snd_ReleaseVoiceEffects(GMixer.slots[Iter], SoundEffectOp::Reset);
						GMixer.slots[Iter].prev_state = GMixer.slots[Iter].state;
						GMixer.slots[Iter].state = State::Paused;
					}
				}
			}
			break;
			case ESoundMixerCommands::resume_all:
			{
				for (size_t Iter = 0; Iter < GMixer.slots.size(); Iter++)
				{
					if (GMixer.slots[Iter].state == State::Paused && GMixer.slots[Iter].prev_state != State::Paused)
					{
						GMixer.slots[Iter].state = GMixer.slots[Iter].prev_state;
						GMixer.slots[Iter].prev_state = State::Paused;
					}
				}
			}
			break;
			case ESoundMixerCommands::update_parameter:
			{
				GMixer.slots[Command.slot - 1].parameters[(u32)Command.param0] = Fvector{(float)*(double*)&Command.param1, (float)*(double*)&Command.param2, (float)*(double*)&Command.param3};

			}
			break;
			case ESoundMixerCommands::set_volume:
			{
				GMixer.slots[Command.slot - 1].parameters[(u32)ParameterId::VolumePerChannel].y = *(double*)&Command.param1;
			}
			break;
			case ESoundMixerCommands::set_panning:
			{
				GMixer.slots[Command.slot - 1].parameters[(u32)ParameterId::Panning].x = *(double*)&Command.param1;
				GMixer.slots[Command.slot - 1].parameters[(u32)ParameterId::Panning].y = *(double*)&Command.param2;
			}
			break;
		}
	}
	// Reclaim stopped one-shot owners once, after the immutable command batch.
	// Queuing Destroy here would enqueue the same owner again next update.
	for (size_t Iter = 0; Iter < GMixer.slots.size(); ++Iter)
	{
		const auto& Slot = GMixer.slots[Iter];
		if ((Slot.flags & (u16)Flags::NoFeedback) && Slot.state == State::Stopped)
		{
			DestroyInternal(Iter + 1);
			++g_SoundStats.possible_free_count;
		}
	}
	// PlayNoFeedback queues position/range/volume after play. AI must see their final values.
	xr_vector<PendingSoundEvent> InitialEvents;
	InitialEvents.reserve(PendingEvents.size());
	for (const auto& Event : PendingEvents)
	{
		const auto& Slot = GMixer.slots[Event->slot - 1];
		if (Slot.state != State::Playing && Slot.state != State::Delay) continue;
		const CSound_params Params = Event->get_params();
		const float Range = std::min(Params.max_ai_distance, Params.max_ai_distance * Params.volume);
		if (Range >= .1f) InitialEvents.push_back({Event, Range});
	}
	PeakCommandBatchUS = std::max(PeakCommandBatchUS, (u32)((Snd_GetTimestamp() - CommandBatchStart) / 1000));
	PropagateWithAudio(InitialEvents);
	ReleaseForAudio();
    // One game-thread publication per logical event, with shots first and
    // rotating non-shot priority so fixed query budgets cannot starve a loop.
    Snd_PrepareActiveRooms();
	const u64 RoomReturnsStart = Snd_GetTimestamp();
    Snd_UpdateRoomReturns();
	PeakRoomReturnsUS = std::max(PeakRoomReturnsUS, (u32)((Snd_GetTimestamp() - RoomReturnsStart) / 1000));
	bool AcousticSectionComplete = true;
	u64 SourceAcousticsStart = 0;
    static u32 AcousticCursor = 0;
    const u32 SlotCount = (u32)GMixer.slots.size();
	xr_vector<u32> ReadySlots;
	ReadySlots.reserve(Commands.size());
    const u32 StartSlot = SlotCount ? AcousticCursor % SlotCount : 0;
    xr_vector<u32> SlotOrder;
    SlotOrder.reserve(SlotCount);
    for (u32 Offset = 0; Offset < SlotCount; ++Offset)
        SlotOrder.push_back((StartSlot + Offset) % SlotCount);
    const auto AcousticPriority = [](u32 Index)
    {
        const auto& Slot = GMixer.slots[Index];
        if (Slot.state != State::Playing && Slot.state != State::Delay) return 0.0f;
        const bool Spatial = (Slot.flags & (u16)Flags::Spatial) != 0;
        const float Distance = Spatial || Slot.shot_group != 0
            ? GMixer.listener.Position.distance_to(Slot.parameters[(u32)ParameterId::Position]) : 0.0f;
        const auto& Range = Slot.parameters[(u32)ParameterId::DistanceRange];
        if (Spatial && Distance >= Range.y) return 0.0f;
        const auto& Volume = Slot.parameters[(u32)ParameterId::VolumePerChannel];
        const auto* Bus = Snd_GetBus(Slot.bus);
        const float Attenuation = std::min(1.0f, std::max(Range.x, 1.0f) / std::max(Distance, 1.0f));
        const float Importance = Slot.shot_group != 0 ? 4.0f : (Slot.flags & (u16)Flags::Physical2D) ? 2.0f : 1.0f;
        return Volume.x * Volume.y * Attenuation * Importance * (Bus ? Bus->Volume * Bus->UserVolume : 0.0f);
    };
    // Stable rotation breaks equal-priority ties; close audible events claim
    // bounded ray work before quieter distant events. DSP tails are never stolen.
    std::stable_sort(SlotOrder.begin(), SlotOrder.end(), [&](u32 A, u32 B)
    { return AcousticPriority(A) > AcousticPriority(B); });
    xr_vector<u64> RankedEvents;
    const auto GeometryEligible = [](const sound_slot_state& Slot)
    {
        const bool Spatial = (Slot.flags & (u16)Flags::Spatial) != 0;
        const bool Physical = Spatial || Slot.shot_group != 0 || (Slot.flags & (u16)Flags::Physical2D) != 0;
        const float Distance = Spatial || Slot.shot_group != 0
            ? GMixer.listener.Position.distance_to(Slot.parameters[(u32)ParameterId::Position]) : 0.0f;
        const auto* Bus = Snd_GetBus(Slot.bus);
        const auto& Volume = Slot.parameters[(u32)ParameterId::VolumePerChannel];
        return Physical && Snd_IsGeometryExciter(Slot) && Snd_HasRoomMetadata() && Bus && Bus->ZoneSend > 0.0f && psSoundFlags.is(ss_EFX) &&
            (Slot.flags & ((u16)Flags::NoOCC | (u16)Flags::Intro)) == 0 &&
            (Slot.state == State::Playing || (Slot.state == State::Delay && Slot.delay <= 0.1f)) &&
            Distance < 85.0f && (!Spatial || Distance < Slot.parameters[(u32)ParameterId::DistanceRange].y) && Volume.x * Volume.y > 0.004f;
    };
    for (u32 Index : SlotOrder)
    {
        const auto& Slot = GMixer.slots[Index];
        if (GeometryEligible(Slot) && Slot.AcousticEvent != 0 &&
            std::find(RankedEvents.begin(), RankedEvents.end(), Slot.AcousticEvent) == RankedEvents.end())
            RankedEvents.push_back(Slot.AcousticEvent);
    }
    if (RankedEvents.size() > GeometryReflections::EventCapacity)
        RankedEvents.resize(GeometryReflections::EventCapacity);
    // Admission publishes atomically before any per-source lock release.
    // Lower-ranked sources retain dry/late audio; old early tails drain.
    g_NativeReflections.SetRankedEvents(RankedEvents.data(), (u32)RankedEvents.size());
    xr_vector<u64> UpdatedEvents;
    UpdatedEvents.reserve(RankedEvents.size());
    AcousticCursor = SlotCount ? (StartSlot + 1) % SlotCount : 0;
    Fvector ListenerRight;
    ListenerRight.crossproduct(GMixer.listener.Normal, GMixer.listener.Direction).normalize_safe();
    for (const u32 SlotIndex : SlotOrder)
    {
		if (AcousticSectionComplete)
		{
			if (SourceAcousticsStart != 0)
				PeakSourceAcousticsUS = std::max(PeakSourceAcousticsUS, (u32)((Snd_GetTimestamp() - SourceAcousticsStart) / 1000));
			ReleaseForAudio();
			AcousticSectionComplete = false;
			SourceAcousticsStart = 0;
		}
        auto& Slot = GMixer.slots[SlotIndex];
		if (Slot.state != State::Playing && Slot.state != State::Delay)
		{
			continue;
		}
		const bool IsStart = Slot.AcousticStartPending;
        // Prepare an imminent delayed source before the precache thread can
        // promote it. This does not change playback time or emit any PCM.
		if (Slot.state == State::Delay && Slot.delay > 0.1f)
		{
			continue;
		}
		Slot.AcousticStartPending = false;
		if (!Slot.AcousticReady)
			ReadySlots.push_back(SlotIndex);
		const bool IsSpatial = (Slot.flags & (u16)Flags::Spatial) != 0;
		Fvector& Pos = Slot.parameters[(u32)ParameterId::Position];
		if (IsStart)
		{
			Slot.velocity.set(0.0f, 0.0f, 0.0f);
			Slot.doppler = 1.0f;
		}
		else if (IsSpatial)
		{
			Slot.velocity = Snd_Velocity(Slot.prev_position, Pos);
		}
		Slot.prev_position = Pos;
		if (Slot.flags & ((u16)Flags::NoOCC | (u16)Flags::Intro))
		{
			continue;
		}
		// Complete all fields for this source under one section. The next
		// iteration releases every mixer lock before selecting another slot.
		SourceAcousticsStart = Snd_GetTimestamp();
		AcousticSectionComplete = true;
		Snd_UpdateSlotIndoorFactor(Slot, IsSpatial || Slot.shot_group != 0
			? Pos : GMixer.listener.Position);
        const Fvector AcousticPosition = IsSpatial || Slot.shot_group != 0 ? Pos : GMixer.listener.Position;
        if (GeometryEligible(Slot))
        {
            if (std::find(UpdatedEvents.begin(), UpdatedEvents.end(), Slot.AcousticEvent) == UpdatedEvents.end())
            {
                const u32 Remaining = 192u - std::min(192u, g_NativeReflections.GetDiagnostics().UpdateQueries);
                const u32 Pending = std::max(1u, std::min(8u, (u32)RankedEvents.size() - std::min((u32)RankedEvents.size(), (u32)UpdatedEvents.size())));
                const u32 Allowance = std::min(Remaining, std::clamp(Remaining / Pending, 12u, 64u));
                g_NativeReflections.UpdateEvent(Slot.AcousticEvent, AcousticPosition,
                    GMixer.listener.Position, ListenerRight, Allowance);
                UpdatedEvents.push_back(Slot.AcousticEvent);
            }
        }
		if (!IsSpatial)
		{
			Slot.parameters[(u32)ParameterId::VolumePerChannel].z = 1.0f;
			continue;
		}
		if (GMixer.listener.Position.distance_to(Pos) >
			Slot.parameters[(u32)ParameterId::DistanceRange].y)
		{
			continue;
		}
		Snd_UpdateShotBarrier(Slot, Pos);
		float& Occlusion = Slot.parameters[(u32)ParameterId::VolumePerChannel].z;
		if (Slot.BarrierInitialized)
		{
			Occlusion = 1.0f;
		}
		else
		{
			const float Transmission = ::Sound->get_occlusion(Pos, 0.2f, GMixer.occ);
			if (IsStart)
			{
				Occlusion = Transmission;
			}
			else
			{
				volume_lerp(Occlusion, Transmission, 1.0f, GMixer.dt);
			}
		}
	}
	// Publish all starts together, preserving STCoP layer onset coherence.
	// Existing voices and tails continued rendering between completed units.
	for (const u32 Index : ReadySlots)
		GMixer.slots[Index].AcousticReady = true;
	if (SourceAcousticsStart != 0)
		PeakSourceAcousticsUS = std::max(PeakSourceAcousticsUS, (u32)((Snd_GetTimestamp() - SourceAcousticsStart) / 1000));
    Snd_PrepareActiveRooms();
    GMixer.EarlyQueryPeak = std::max(GMixer.EarlyQueryPeak, g_NativeReflections.GetDiagnostics().UpdateQueries);
    const u64 commandPhaseEnd = Snd_GetTimestamp();
    peakCommandsUS = std::max(peakCommandsUS, (u32)((commandPhaseEnd - slotSweepPhaseEnd) / 1000));

    const u64 eventPhaseEnd = Snd_GetTimestamp();
    peakEventsUS = std::max(peakEventsUS, (u32)((eventPhaseEnd - commandPhaseEnd) / 1000));
	g_SoundStats.update_time_micros = (Snd_GetTimestamp() - TimeStamp) / 1000;
    peakUpdateWorkUS = std::max(peakUpdateWorkUS, (u32)((Snd_GetTimestamp() - updateWorkStart) / 1000));

	// Snapshot meters under the render lock, then format/log on the game thread.
	const bool PublishMetrics = g_LiminalMetricsReady.exchange(false, std::memory_order_acquire);
	SoundBusMeter BusMeters[SND_BUS_COUNT] = {};
	shared_str BusNames[SND_BUS_COUNT];
	shared_str BusEffectNames[SND_BUS_COUNT][SND_BUS_EFFECT_COUNT];
	u32 PlayingSlots = 0, DelayedSlots = 0, PausedSlots = 0, StoppedSlots = 0;
	u32 LoopedSlots = 0;
	float LongestPosition = 0.0f, LongestDelay = 0.0f;
	const float DryMin = GMixer.DryMin, WetMax = GMixer.WetMax;
	const u32 Physical2DVoices = GMixer.Physical2DVoices;
	const u32 SimulatedVoices = GMixer.SimulatedVoices;
	const u32 RoomTransitions = GMixer.RoomTransitions;
	const u32 TransitionOldBus = GMixer.TransitionOldBus, TransitionNewBus = GMixer.TransitionNewBus;
	const u32 CallbackOverBudget = GMixer.CallbackOverBudget;
    const auto EarlyStats = g_NativeReflections.GetDiagnostics();
    const u32 EarlyQueryPeak = GMixer.EarlyQueryPeak;
    const u32 LogicalRoom = Snd_GetZoneRoom(GMixer.ListenerZone);
    const SoundRoomInfo* RoomInfo = Snd_GetRoomInfo(LogicalRoom);
    const u32 CallbackDeadlineMisses = GMixer.CallbackDeadlineMisses;
	const u32 CallbackTotalPeakUS = GMixer.CallbackTotalPeakUS;
	AcousticShotMeter ShotMeters[2] = {};
	AcousticShotMeter VoiceMeter;
    SoundRoomReturnState RoomReturns[12] = {};
    const u32 ReturnCount = PublishMetrics ? Snd_GetRoomReturnStates(RoomReturns, std::size(RoomReturns)) : 0;
	AcousticSourceDiagnostic PhysicalDiagnostics[PhysicalDiagnosticCapacity];
	const u32 PhysicalDiagnosticCount = PublishMetrics ? GMixer.PhysicalDiagnosticCount : 0;
	const u32 PhysicalDiagnosticDropped = GMixer.PhysicalDiagnosticDropped;
	const u32 CreatureDropped = GMixer.PhysicalDiagnosticDroppedKinds[0];
	const u32 NpcVoiceDropped = GMixer.PhysicalDiagnosticDroppedKinds[1];
	const u32 WorldPrimaryDropped = GMixer.PhysicalDiagnosticDroppedKinds[2];
	if (PublishMetrics)
	{
		for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
		{
			BusMeters[Idx] = Snd_TakeBusMeter(Idx + 1);
			const SoundBus* MeterBus = Snd_GetBus(Idx + 1);
			if (MeterBus != nullptr)
			{
				BusNames[Idx] = MeterBus->Name;
				for (u32 EffectIdx = 0; EffectIdx < BusMeters[Idx].EffectCount; ++EffectIdx)
				{
					const SoundEffectEntry* Entry = Snd_GetEffect(MeterBus->Effects[EffectIdx].Effect);
					if (Entry != nullptr)
					{
						BusEffectNames[Idx][EffectIdx] = Entry->Name;
					}
				}
			}
		}
		for (const auto& Slot : GMixer.slots)
		{
			if (Slot.sound_name.empty())
			{
				continue;
			}
			switch (Slot.state)
			{
			case State::Playing:
				++PlayingSlots;
				LoopedSlots += (Slot.flags & (u16)Flags::Looped) != 0 ? 1u : 0u;
				LongestPosition = std::max(LongestPosition, float(Slot.position) / float(SND_SAMPLERATE));
				break;
			case State::Delay:
				++DelayedSlots;
				LongestDelay = std::max(LongestDelay, Slot.delay);
				break;
			case State::Paused: ++PausedSlots; break;
			case State::Stopped: ++StoppedSlots; break;
			}
		}
		for (u32 Idx = 0; Idx < PhysicalDiagnosticCount; ++Idx)
		{
			PhysicalDiagnostics[Idx] = GMixer.PhysicalDiagnostics[Idx];
		}
		VoiceMeter = GMixer.VoiceMeter;
		GMixer.VoiceMeter = {};
		GMixer.PhysicalDiagnosticCount = 0;
		GMixer.PhysicalDiagnosticDropped = 0;
		for (u32& Count : GMixer.PhysicalDiagnosticKinds) Count = 0;
		for (u32& Count : GMixer.PhysicalDiagnosticDroppedKinds) Count = 0;
		GMixer.DryMin = 1.0f;
		GMixer.WetMax = 0.0f;
		GMixer.RoomTransitions = 0;
		GMixer.CallbackOverBudget = 0;
        GMixer.EarlyQueryPeak = 0;
        GMixer.CallbackDeadlineMisses = 0;
		GMixer.CallbackTotalPeakUS = 0;
		for (u32 Idx = 0; Idx < std::size(ShotMeters); ++Idx)
		{
			ShotMeters[Idx] = GMixer.ShotMeters[Idx];
			GMixer.ShotMeters[Idx] = {};
		}
	}

	PeakLockHoldUS = std::max(PeakLockHoldUS, (u32)((Snd_GetTimestamp() - LockSectionStart) / 1000));
	GMixer.update_lock.ReleaseExclusive();
	GMixer.manage_lock.ReleaseExclusive();
	GMixer.render_lock.ReleaseExclusive();
	SoundAcousticCapture::DumpReady();

    if (PublishMetrics)
    {
        Msg("* [LIMINAL_ACOUSTICS_V6] room=%u cover=%.5f openings_m2=%.5f roof_openings_m2=%.5f open_geometry=%u selected_events=%u draining_events=%u approximate_material_paths=%llu measured_material_paths=%llu transition_model=verified_aperture query_limit=192 event_pool=32 room_pool=12",
            LogicalRoom, RoomInfo ? RoomInfo->CoverFraction : 0.0f,
            RoomInfo ? RoomInfo->OpeningArea : 0.0f, RoomInfo ? RoomInfo->RoofOpeningArea : 0.0f,
            RoomInfo ? u32(RoomInfo->HasOpenGeometry) : 0u, EarlyStats.SelectedEvents, EarlyStats.DrainingEvents,
            (unsigned long long)EarlyStats.ApproximateMaterialPaths, (unsigned long long)EarlyStats.MeasuredMaterialPaths);
        Msg("* [LIMINAL_ACOUSTICS_V5] room=%u cuboid=%u room_bus=%u volume=%.3f surface=%.3f dimensions=(%.3f %.3f %.3f) wet_reference=%.6f material_model=%s measured_materials=0",
            LogicalRoom, GMixer.ListenerZone, Snd_GetZoneBus(GMixer.ListenerZone),
            RoomInfo ? RoomInfo->Volume : 0.0f, RoomInfo ? RoomInfo->SurfaceArea : 0.0f,
            RoomInfo ? RoomInfo->Dimensions.x : 0.0f, RoomInfo ? RoomInfo->Dimensions.y : 0.0f,
            RoomInfo ? RoomInfo->Dimensions.z : 0.0f, RoomInfo ? RoomInfo->WetReference : 0.0f,
            RoomInfo && RoomInfo->HasMaterialGeometry ? "area_weighted_generic_boundary_loss" : "preset_equivalent");
        if (RoomInfo && RoomInfo->HasMaterialGeometry)
        {
            float Decay[9] = {};
            for (u32 Band = 0; Band < 9; ++Band)
            {
                Decay[Band] = Resonance_ComputeMaterialRoomDecay(RoomInfo->Volume, RoomInfo->SurfaceArea,
                    RoomInfo->OpeningArea, RoomInfo->MeanBoundaryLoss, Band);
            }
            Msg("* [LIMINAL_ACOUSTICS_V7] room=%u sampled_closed_area=%.4f classified_fraction=%.5f boundary_loss=(%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) rt60=(%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) measured_materials=0",
                RoomInfo->Id, RoomInfo->SampledClosedArea, RoomInfo->ClassifiedAreaFraction,
                RoomInfo->MeanBoundaryLoss[0], RoomInfo->MeanBoundaryLoss[1], RoomInfo->MeanBoundaryLoss[2],
                RoomInfo->MeanBoundaryLoss[3], RoomInfo->MeanBoundaryLoss[4], RoomInfo->MeanBoundaryLoss[5],
                RoomInfo->MeanBoundaryLoss[6], RoomInfo->MeanBoundaryLoss[7], RoomInfo->MeanBoundaryLoss[8],
                Decay[0], Decay[1], Decay[2], Decay[3], Decay[4], Decay[5], Decay[6], Decay[7], Decay[8]);
        }
        Msg("* [LIMINAL_ACOUSTICS_V5] early_events=%u paths=%u tail_events=%u query_peak=%u query_limit=192 pool_rejected=%llu budget_deferred=%llu solid_rejected=%llu door_rejected=%llu unknown_material_paths=%llu callback_total_over_budget=%u",
            EarlyStats.ActiveEvents, EarlyStats.ActivePaths, EarlyStats.TailEvents, EarlyQueryPeak,
            (unsigned long long)EarlyStats.PoolRejected, (unsigned long long)EarlyStats.BudgetDeferred,
            (unsigned long long)EarlyStats.SolidRejected, (unsigned long long)EarlyStats.DoorRejected,
            (unsigned long long)EarlyStats.UnknownMaterialPaths, CallbackDeadlineMisses);
		Msg("* [LIMINAL_AUDIO_TIMING_V5] callback_total_peak_us=%u render_lock_wait_peak_us=%u render_work_peak_us=%u voice_peak_us=%u bus_peak_us=%u update_lock_wait_peak_us=%u update_work_peak_us=%u listener_phase_peak_us=%u refs_phase_peak_us=%u slots_phase_peak_us=%u commands_acoustics_phase_peak_us=%u events_phase_peak_us=%u command_count_peak=%u",
			CallbackTotalPeakUS, g_LiminalRenderLockPeakUS.load(std::memory_order_relaxed),
			g_LiminalRenderPeakUS.load(std::memory_order_relaxed), g_LiminalVoicePeakUS.load(std::memory_order_relaxed),
			g_LiminalBusPeakUS.load(std::memory_order_relaxed), peakUpdateLockWaitUS, peakUpdateWorkUS,
			peakListenerUS, peakRefSoundsUS, peakSlotsUS, peakCommandsUS, peakEventsUS, peakCommandCount);
		Msg("* [LIMINAL_AUDIO_LOCK_V5] lock_section_peak_us=%u command_batch_peak_us=%u room_returns_peak_us=%u source_acoustics_peak_us=%u source_preload_shared_peak_us=%u ai_propagation_shared_peak_us=%u command_queue=detached readiness=before_first_pcm",
			PeakLockHoldUS, PeakCommandBatchUS, PeakRoomReturnsUS, PeakSourceAcousticsUS, PeakSourcePreloadUS, PeakAIPropagationUS);
		Msg("* [LIMINAL_SOURCE_V5] records=%u cap=%u dropped_first_blocks=%u dropped_creature=%u dropped_npc_voice=%u dropped_world_primary=%u quotas=8/4/4 window=audio_telemetry_5s pcm=source_contribution_before_bus_effects envelope=approximate_without_hrtf_pan_bus_dsp",
			PhysicalDiagnosticCount, PhysicalDiagnosticCapacity, PhysicalDiagnosticDropped, CreatureDropped, NpcVoiceDropped, WorldPrimaryDropped);
		for (u32 Idx = 0; Idx < PhysicalDiagnosticCount; ++Idx)
		{
			const auto& Record = PhysicalDiagnostics[Idx];
			const auto& Path = Record.Path;
			const char* Kind = Record.Kind == 1 ? "creature" : Record.Kind == 2 ? "npc_voice" : Record.Group != 0 ? "world_primary" : "world_weapon_sample";
			const char* Chosen = Path.ChosenPath == 1 ? "direct" : Path.ChosenPath == 2 ? "portal" :
				Path.ChosenPath == 3 ? "direct_dynamic" : "unavailable";
			const double Samples = double(std::max(Record.Samples, 1u));
			const double PathAgeMs = Path.TimestampNS != 0 && Record.TimestampNS >= Path.TimestampNS
				? double(Record.TimestampNS - Path.TimestampNS) / 1000000.0 : -1.0;
			Msg("* [LIMINAL_SOURCE_V5] record=%u event=%llu slot=%u group=%u layer=%u kind=%s sample=%s frame=%u bus=%u flags=%u source=(%.3f %.3f %.3f) listener=(%.3f %.3f %.3f) distance=%.4f min_range=%.4f max_range=%.4f source_room=%u listener_room=%u",
				Idx, (unsigned long long)Record.Event, Record.Slot, Record.Group, Record.Layer, Kind, Record.Sample,
				Record.Frame, Record.Bus, Record.Flags, Record.Source.x, Record.Source.y, Record.Source.z,
				Record.Listener.x, Record.Listener.y, Record.Listener.z, Record.Distance, Record.MinRange, Record.MaxRange,
				Record.SourceRoom, Record.ListenerRoom);
			Msg("* [LIMINAL_SOURCE_V5] record=%u event=%llu slot=%u queried=%u query_valid=%u budget_deferred=%u cache=%u path_age_ms=%.3f center_queried=%u first_hit=%u material_index=%u material_flags=%u material_from_callback=%u material_transmission=%.5f material_distance=%.4f center_gain=%.5f direct_gain=%.5f portal_gain=%.5f dynamic_queried=%u dynamic_gain=%.5f selected_path=%s selected_gain=%.5f portal_kind=%u portal_id=%u portal_glass=%u portal_materials_known=%u portal_leg_gain=(%.5f %.5f) ray_source=(%.3f %.3f %.3f) ray_listener=(%.3f %.3f %.3f)",
				Idx, (unsigned long long)Record.Event, Record.Slot, u32(Path.Queried), u32(Path.Valid), u32(Path.Deferred), u32(Path.FromCache),
				PathAgeMs, u32(Path.CenterRayQueried), u32(Path.MaterialHit), Path.MaterialIndex, Path.MaterialFlags,
				u32(Path.MaterialFromCallback), Path.MaterialTransmission, Path.MaterialDistance, Path.CenterTransmission,
				Path.DirectGain, Path.PortalGain, u32(Path.DynamicQueried), Path.DynamicGain, Chosen, Path.SelectedGain, Path.PortalKind,
				Path.PortalId, u32(Path.PortalGlass), u32(Path.PortalMaterialsKnown), Path.PortalLegAGain, Path.PortalLegBGain,
				Path.Source.x, Path.Source.y, Path.Source.z, Path.Listener.x, Path.Listener.y, Path.Listener.z);
			Msg("* [LIMINAL_SOURCE_V5] record=%u event=%llu slot=%u source_gain=%.6f distance_only_gain=%.6f distance_with_voice_gain=%.6f native_spatial_stages=%u other_voice_stages=%u occlusion_gain=%.6f residual_voice_gain=%.6f lp_cutoff_hz=%.2f lp_blend=%.5f dry_begin=%.6f dry_end=%.6f bus_begin=%.6f bus_end=%.6f envelope_begin_approx=%.6f envelope_end_approx=%.6f pan=(%.5f %.5f) dry_bus_input_rms=%.7f dry_bus_input_peak=%.7f fader_equiv_rms_approx=%.7f fader_equiv_peak_approx=%.7f samples=%u",
				Idx, (unsigned long long)Record.Event, Record.Slot, Record.SourceGain, Record.DistanceOnlyGain, Record.DistanceWithVoiceGain,
				Record.SpatialStages, Record.OtherVoiceStages, Record.OcclusionGain, Record.ResidualVoiceGain,
				Record.LowPassCutoff, Record.LowPassBlend, Record.DryBegin, Record.DryEnd, Record.BusBegin, Record.BusEnd,
				Record.EnvelopeBegin, Record.EnvelopeEnd, Record.Left, Record.Right,
				std::sqrt(Record.DryInputEnergy / Samples), Record.DryInputPeak,
				std::sqrt(Record.FaderEquivalentEnergy / Samples), Record.FaderEquivalentPeak, Record.Samples);
		}
        for (u32 Index = 0; Index < ReturnCount; ++Index)
        {
            const auto& Room = RoomReturns[Index];
            if (!Room.Active) continue;
            Msg("* [LIMINAL_ROOM_RETURN] room=%u bus=%u listener_room=%u active_tail_or_input=%u validated=%u gain=%.5f target=%.5f cutoff=%.1f blend=%.4f anchor=(%.3f %.3f %.3f) band_ratios=(%.5f %.5f %.5f) target_band_ratios=(%.5f %.5f %.5f) stage=after_room_effects",
                Room.RoomId, Room.Bus, LogicalRoom, Room.Active, Room.Validated, Room.Gain,
                Room.TargetGain, Room.Cutoff, Room.FilterBlend, Room.Anchor.x, Room.Anchor.y, Room.Anchor.z,
                Room.BandRatios[0], Room.BandRatios[1], Room.BandRatios[2],
                Room.TargetBandRatios[0], Room.TargetBandRatios[1], Room.TargetBandRatios[2]);
            Msg("* [LIMINAL_ROOM_V8] room=%u bus=%u source_event=%llu source_kind=%u mixed=%u portal=%u spatial_fraction=%.5f spatial_target=%.5f spatial_ready=%u direction_model=dominant_verified_opening origin_model=dominant_live_source_retained_for_tail",
                Room.RoomId, Room.Bus, (unsigned long long)Room.SourceEvent, Room.SourceKind, u32(Room.SourceMixed),
                Room.PortalId, Room.SpatialFraction, Room.TargetSpatialFraction, u32(Room.SpatialReady));
        }
        if (VoiceMeter.Event != 0 && VoiceMeter.Samples != 0)
        {
            const double Samples = double(VoiceMeter.Samples);
            Msg("* [LIMINAL_VOICE_PCM_V8] event=%llu sample=%s source_zone=%u listener_zone=%u dry_rms=%.7f room_send_rms=%.7f samples=%llu room_send=exact_source_contribution_all_destination_rooms return_attribution=see_room_meter",
                (unsigned long long)VoiceMeter.Event, VoiceMeter.Sample, VoiceMeter.SourceZone, VoiceMeter.ListenerZone,
                std::sqrt(VoiceMeter.DryEnergy / Samples), std::sqrt(VoiceMeter.RoomSendEnergy / Samples), (unsigned long long)VoiceMeter.Samples);
        }
        const u32 asyncCount = xr_atomic_ref<u32>(g_SoundStats.liminal_async_decode_count).exchange(0, std::memory_order_relaxed);
        const u32 asyncPeak = xr_atomic_ref<u32>(g_SoundStats.liminal_async_decode_peak_us).exchange(0, std::memory_order_relaxed);
		Msg("* [LIMINAL_AUDIO_QA] voices=%u shooting=%u listener_zone=%u listener_room=%.3f room_target=%.3f environment_bus=%u outdoor_bus=%u indoor_bus=%u hrtf_active=%u hrtf_shooting=%u hrtf_high_active=%u shot_group_primary=%u shot_group_secondary=%u render_peak_us=%u update_work_peak_us=%u listener_pos=(%.1f %.1f %.1f)",
			g_LiminalMetricsVoices.load(std::memory_order_relaxed),
			g_LiminalMetricsShots.load(std::memory_order_relaxed),
			GMixer.ListenerZone, GMixer.IndoorFactor, listenerIndoorTarget,
			Snd_GetZoneBus(GMixer.ListenerZone),
			ShootingBus != nullptr ? ShootingBus->FarSend : 0u,
			ShootingBus != nullptr ? ShootingBus->IndoorSend : 0u,
			Resonance_GetActiveHrtfs(), Resonance_GetActiveShootingHrtfs(), Resonance_GetActiveHighHrtfs(),
			g_LiminalGroupedPrimary.load(std::memory_order_relaxed),
			g_LiminalGroupedSecondary.load(std::memory_order_relaxed),
			g_LiminalRenderPeakUS.load(std::memory_order_relaxed), peakUpdateWorkUS,
			GMixer.listener.Position.x, GMixer.listener.Position.y, GMixer.listener.Position.z);
		Msg("* [LIMINAL_AUDIO_QA] route dry_min=%.4f wet_send_max=%.4f physical_2d=%u simulated=%u send_transitions=%u old_room_bus=%u new_room_bus=%u callback_work_over_budget=%u underruns=unavailable playing=%u delay=%u paused=%u stopped_owned=%u looped=%u longest_position_s=%.3f longest_delay_s=%.3f",
			DryMin, WetMax, Physical2DVoices, SimulatedVoices, RoomTransitions,
			TransitionOldBus, TransitionNewBus, CallbackOverBudget,
			PlayingSlots, DelayedSlots, PausedSlots, StoppedSlots, LoopedSlots,
			LongestPosition, LongestDelay);
		for (u32 Idx = 0; Idx < std::size(ShotMeters); ++Idx)
		{
			const auto& Meter = ShotMeters[Idx];
			if (Meter.Samples == 0)
			{
				continue;
			}
			const double Samples = double(Meter.Samples);
			Msg("* [LIMINAL_AUDIO_QA] shot_pcm kind=%s group=%u primary=%s source_zone=%u listener_zone=%u dry_gain=%.5f room_reference_nominal=%.5f input_rms=%.6f dry_rms=%.6f far_send_rms=%.6f room_send_rms=%.6f samples=%llu wet_measurement=processor_input",
				Idx == 0 ? "actor" : "world", Meter.Group, Meter.Sample,
				Meter.SourceZone, Meter.ListenerZone, Meter.DryGain, Meter.RoomReference,
				std::sqrt(Meter.InputEnergy / Samples), std::sqrt(Meter.DryEnergy / Samples),
				std::sqrt(Meter.FarSendEnergy / Samples), std::sqrt(Meter.RoomSendEnergy / Samples),
				(unsigned long long)Meter.Samples);
		}
		for (u32 Idx = 0; Idx < SND_BUS_COUNT; ++Idx)
		{
			const auto& Meter = BusMeters[Idx];
			if (Meter.InputPeak > 0.0f || Meter.OutputPeak > 0.0f || Meter.ClippedSamples != 0)
			{
				const double Samples = double(std::max(Meter.Samples, 1ull));
				Msg("* [LIMINAL_AUDIO_QA] bus=%u name=%s input_peak=%.6f output_peak=%.6f output_clipped_samples=%llu input_rms=%.6f output_rms=%.6f window_samples=%llu",
					Idx + 1, BusNames[Idx].size() ? BusNames[Idx].c_str() : "unavailable",
					Meter.InputPeak, Meter.OutputPeak, (unsigned long long)Meter.ClippedSamples,
					std::sqrt(Meter.InputEnergy / Samples), std::sqrt(Meter.OutputEnergy / Samples),
					(unsigned long long)Meter.Samples);
				for (u32 EffectIdx = 0; EffectIdx < Meter.EffectCount; ++EffectIdx)
				{
					Msg("* [LIMINAL_AUDIO_QA] bus_stage=%u name=%s effect_index=%u effect=%s peak=%.6f rms=%.6f window_samples=%llu",
						Idx + 1, BusNames[Idx].c_str(), EffectIdx,
						BusEffectNames[Idx][EffectIdx].size() ? BusEffectNames[Idx][EffectIdx].c_str() : "unavailable",
						Meter.EffectPeak[EffectIdx], std::sqrt(Meter.EffectEnergy[EffectIdx] / Samples),
						(unsigned long long)Meter.Samples);
				}
                if (Meter.ReturnSamples != 0)
                {
                    const double ReturnSamples = double(Meter.ReturnSamples);
                    const double Reduction = Meter.EffectCount >= 2 && Meter.EffectEnergy[0] > 0.0 && Meter.EffectEnergy[1] > 0.0
                        ? 10.0 * std::log10(Meter.EffectEnergy[1] / Meter.EffectEnergy[0]) : 0.0;
                    Msg("* [LIMINAL_ROOM_PCM_V8] bus=%u event=%llu kind=%u mixed=%u source_observed=%u compressor_delta_db=%.4f return_input_rms=%.7f return_output_rms=%.7f return_input_peak=%.7f return_output_peak=%.7f samples=%llu spatial_blocks=%u attribution=exact_only_if_single_owner_since_tail_start",
                        Idx + 1, (unsigned long long)Meter.SourceEvent, Meter.SourceKind, u32(Meter.SourceMixed), u32(Meter.SourceObserved),
                        Reduction, std::sqrt(Meter.ReturnInputEnergy / ReturnSamples), std::sqrt(Meter.ReturnOutputEnergy / ReturnSamples),
                        Meter.ReturnInputPeak, Meter.ReturnOutputPeak, (unsigned long long)Meter.ReturnSamples, Meter.SpatialBlocks);
                }
			}
		}
        peakUpdateLockWaitUS = 0;
        peakUpdateWorkUS = 0;
        peakListenerUS = peakRefSoundsUS = peakSlotsUS = peakCommandsUS = peakEventsUS = 0;
        peakCommandCount = 0;
		PeakLockHoldUS = PeakCommandBatchUS = PeakRoomReturnsUS = PeakSourceAcousticsUS = 0;
		PeakSourcePreloadUS = PeakAIPropagationUS = 0;
    }
}

void Mixer::ArmAcousticCapture()
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Msg("* [LIMINAL_PCM_V8] armed=%u next=NPC_voice_in_logical_room frames=524288 tracks=6 allocation=game_thread_only",
		u32(SoundAcousticCapture::Arm()));
}

void Mixer::StopAll()
{
	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.id = ESoundMixerCommands::stop_all});
}

void Mixer::PauseAll()
{
	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.id = ESoundMixerCommands::pause_all});
}

void Mixer::ResumeAll()
{
	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.id = ESoundMixerCommands::resume_all});
}

void Mixer::DereferenceObjects(CObject** object, int count)
{
	xrSRWLockGuard Guard0(GMixer.update_lock);
	xrSRWLockGuard Guard1(GMixer.manage_lock);

	for (auto& SoundRef : GMixer.sounds)
	{
		if (SoundRef == nullptr || SoundRef->_p == nullptr)
		{
			continue;
		}

		for (size_t Iter = 0; Iter < count; Iter++)
		{
			if (object[Iter] == SoundRef->_g_object())
			{
				SoundRef->_p->g_object = nullptr;
			}
		}
	}
}

u32 Mixer::Create()
{
	if (GMixer.free_slots.empty())
	{
		Snd_GrowSlots(true);
	}

	xrSRWLockGuard Guard0(GMixer.update_lock);

	u32 SlotIdx = GMixer.free_slots[GMixer.free_slots.size() - 1];
	GMixer.free_slots.pop_back();
	g_SoundStats.possible_free_count--;

	return SlotIdx;
}

void Mixer::Destroy(u32 SlotID)
{
	if (SlotID == 0)
	{
		return;
	}

	GMixer.slots[SlotID - 1].fake_state = State::Stopped;

	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.slot = SlotID, .id = ESoundMixerCommands::destroy});
	g_SoundStats.possible_free_count++;
}

void Mixer::Play(u32 SlotID, u16 flags, ref_sound* SoundRef, double Delay, u32 Bus)
{
	xrCriticalSectionGuard Guard(GMixer.play_lock);

	if (SlotID == 0 || SoundRef == nullptr || SoundRef->_p == nullptr || SoundRef->_p->fn_attached[0] == nullptr)
	{
		return;
	}

	GMixer.slots[SlotID - 1].fake_state = State::Playing;

	GMixer.cmd.emplace_back(SoundCommand{
        .slot = SlotID, .id = ESoundMixerCommands::play, .param0 = flags,
        .param1 = (u64)SoundRef, .param2 = *(u64*)&Delay,
        .param3 = (u64)SoundRef->_g_object(), .string_storage = SoundRef->_p->fn_attached[0],
        .bus = Bus, .shot_group = g_ShotContext.group,
        .shot_layer = g_ShotContext.layer, .shot_layer_gain = g_ShotContext.gain
    });
}

void Mixer::PlayNoFeedback(u16 Flags, ref_sound* SoundRef, CObject* Obj, double Delay, float* Pitch, float* Volume, Fvector* Distance, Fvector* Pos, u32 Bus)
{
	xrCriticalSectionGuard Guard(GMixer.play_lock);

	u32 SlotIdx = Create();
	if (SlotIdx == 0)
	{
		return;
	}

	auto& Slot = GMixer.slots[SlotIdx - 1];
	Slot.state = State::Paused;

	Slot.fake_state = State::Playing;

	GMixer.cmd.emplace_back(SoundCommand{
        .slot = SlotIdx, .id = ESoundMixerCommands::play, .param0 = Flags,
        .param1 = (u64)SoundRef, .param2 = *(u64*)&Delay,
        .param3 = (u64)Obj, .string_storage = SoundRef->_p->fn_attached[0],
        .bus = Bus, .shot_group = g_ShotContext.group,
        .shot_layer = g_ShotContext.layer, .shot_layer_gain = g_ShotContext.gain
    });

	auto Params = SoundRef->_p->get_params();
	Fvector Distances = {Params.min_distance, Params.max_distance, Params.max_ai_distance};

	if (SoundRef->slot())
	{
		Pitch = (Pitch ? Pitch : &Params.freq);
		Distance = (Distance ? Distance : &Distances);
		Pos = (Pos ? Pos : &Params.position);
		Volume = (Volume ? Volume : &Params.volume);
	}

	if (Pitch)
	{
		Mixer::UpdateParameter(SlotIdx, ParameterId::Pitch, Fvector{*Pitch, 1.f, 1.f});
	}
	if (Distance)
	{
		Mixer::UpdateParameter(SlotIdx, ParameterId::DistanceRange, *Distance);
	}
	if (Pos)
	{
		Mixer::UpdateParameter(SlotIdx, ParameterId::Position, *Pos);
	}
	if (Volume)
	{
		Mixer::SetVolume(SlotIdx, *Volume);
	}
}

void Mixer::Pause(u32 slot)
{
	if (slot == 0)
	{
		return;
	}

	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.slots[slot - 1].fake_state = State::Paused;
	GMixer.cmd.emplace_back(SoundCommand{.slot = slot, .id = ESoundMixerCommands::pause});
}

void Mixer::Stop(u32 SlotID, bool IsDeferred)
{
	if (SlotID == 0)
	{
		return;
	}

	auto& Slot = GMixer.slots[SlotID - 1];
	if (Slot.state == State::Delay)
	{
		IsDeferred = false;
	}

	if (!IsDeferred)
	{
		Slot.fake_state = State::Stopped;
	}

	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.slot = SlotID, .id = ESoundMixerCommands::stop, .param0 = IsDeferred});
}

void Mixer::UpdateParameter(u32 SlotID, ParameterId Parameter, Fvector Value)
{
	if (SlotID == 0)
	{
		return;
	}

	double P0 = Value.x, P1 = Value.y, P2 = Value.z;
	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.slot = SlotID, .id = ESoundMixerCommands::update_parameter, .param0 = (u16)Parameter, .param1 = *(u64*)&P0, .param2 = *(u64*)&P1, .param3 = *(u64*)&P2});
}

void Mixer::SetVolume(u32 Slot, double Volume)
{
	if (Slot == 0)
	{
		return;
	}

	Volume = std::clamp(Volume, 0.0, 1.0);
	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.slot = Slot, .id = ESoundMixerCommands::set_volume, .param1 = *(u64*)&Volume});
}

void Mixer::SetPanning(u32 slot, double Left, double Right)
{
	if (slot == 0)
	{
		return;
	}

	Left = std::clamp(Left, 0.0, 1.0);
	Right = std::clamp(Right, 0.0, 1.0);

	xrCriticalSectionGuard Guard(GMixer.play_lock);
	GMixer.cmd.emplace_back(SoundCommand{.slot = slot, .id = ESoundMixerCommands::set_panning, .param1 = *(u64*)&Left, .param2 = *(u64*)&Right});
}

xr_vector<sound_slot_state>& Mixer::GetSlots()
{
	return GMixer.slots;
}

xrSRWLock& Mixer::GetUpdateMutex()
{
	return GMixer.update_lock;
}

xrSRWLock& Mixer::GetManageMutex()
{
	return GMixer.manage_lock;
}

float* Mixer::GetMasterVolume()
{
	return GetBusVolume("master");
}

float* Mixer::GetBusVolume(const char* Name)
{
	u32 BusIdx = Snd_ReserveBus(Name);
	R_ASSERT(BusIdx != 0);
	return &Snd_GetBuses()[BusIdx - 1].UserVolume;
}

u32 Mixer::FindBus(const char* Name)
{
	return Snd_FindBus(Name);
}

u32 Mixer::RegisterEffect(const char* Name, SoundEffectProc Proc)
{
	return Snd_RegisterEffect(Name, Proc);
}

SoundBus* Mixer::GetBuses()
{
	return Snd_GetBuses();
}

const SoundEffectEntry* Mixer::GetEffect(u8 Id)
{
	return Snd_GetEffect(Id);
}

u32 Mixer::GetEffectCount()
{
	return Snd_GetEffectCount();
}

u32 Mixer::CreateBus(const char* Name)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	return Snd_CreateBus(Name);
}

void Mixer::DeleteBus(u32 Bus)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_DeleteBus(Bus);
}

void Mixer::SetBusValue(u32 Bus, const char* Key, const char* Value)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_SetBusValue(Bus, Key, Value);
	GMixer.shooting_reverb = -1.0f;
	GMixer.compression = -1.0f;
}

void Mixer::SetBusEffectParam(u32 Bus, bool IsVoice, u32 Effect, u32 Param, float Value)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_SetBusEffectParam(Bus, IsVoice, Effect, Param, Value);
}

const char* Mixer::GetBusValue(u32 Bus, const char* Key)
{
	return Snd_GetBusValue(Bus, Key);
}

bool Mixer::SaveBus(u32 Bus)
{
	return Snd_SaveBus(Bus);
}

void Mixer::AddZone(sound_zone_params& Params)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_AddZone(&Params, false);
}

void Mixer::AddEditorZone(sound_zone_params& Params)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_AddZone(&Params, true);
}

bool Mixer::LoadRoomMetadata(IReader* Reader, u32 EnvCrc32)
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	return Snd_LoadRoomMetadata(Reader, EnvCrc32);
}

void Mixer::ResetZones()
{
	xrSRWLockGuard Guard(GMixer.render_lock);
	Snd_ResetZones();
	g_NativeReflections.Reset();
	GMixer.ListenerZone = 0;
	memset(GMixer.ListenerRoomWeights, 0, sizeof(GMixer.ListenerRoomWeights));
	memset(g_AcousticProbeCache, 0, sizeof(g_AcousticProbeCache));
	memset(g_AcousticWallPaths, 0, sizeof(g_AcousticWallPaths));
	for (auto& Slot : GMixer.slots)
	{
		Snd_ResetRoomWeights(Slot);
		Slot.zone_idx = 0;
	}
}

void Mixer::ExportConfig()
{
	Snd_ExportConfig();
}

bool Mixer::GetSoundConfig(const char* Name, sound_config* Config)
{
	return Snd_GetSoundConfig(Name, Config);
}

void Mixer::SetSoundConfig(const char* Name, sound_config* Config)
{
	xrSRWLockGuard RenderGuard(GMixer.render_lock);
	xrSRWLockGuard UpdateGuard(GMixer.update_lock);
	xrSRWLockGuard SourceGuard(g_SoundSourceLock, true);
	SoundSourceState* Source = Snd_SetSoundConfig(Name, Config);
	if (Source == nullptr)
	{
		return;
	}

	for (sound_slot_state& Slot : GMixer.slots)
	{
		if (Slot.state == State::Stopped || Snd_LookupSource(&Slot.sound_name) != Source)
		{
			continue;
		}

		Slot.parameters[(u32)ParameterId::VolumePerChannel].x = Source->Desc.volume;
		Slot.parameters[(u32)ParameterId::DistanceRange] = Fvector(Source->Desc.min_distance, Source->Desc.max_distance, Source->Desc.max_ai_distance);
	}
}

bool Mixer::SaveSoundConfig(const char* Name)
{
	return Snd_SaveConfig(Name);
}

void Mixer::GetLoadedSources(xr_vector<shared_str>& Names)
{
	Snd_GetLoadedSources(Names);
}

const xr_vector<sound_zone_params>& Mixer::GetZones()
{
	return Snd_GetZones();
}

sound_stats* Mixer::GetStats()
{
	return &g_SoundStats;
}

float Mixer::GetPlaytime(u32 SlotID)
{
	if (SlotID == 0)
	{
		return 0.0f;
	}

	return (((float)GMixer.slots[SlotID - 1].position) / (float)SND_SAMPLERATE);
}

float Mixer::GetDuration(u32 SlotID)
{
	xrSRWLockGuard Guard(g_SoundSourceLock, true);

	if (SlotID == 0)
	{
		return 0.0f;
	}

	const SoundSourceState* Source = Snd_LookupSource(&GMixer.slots[SlotID - 1].sound_name);
	if (Source == nullptr)
	{
		return 0.0f;
	}

	return (float)Source->Desc.frames_total / (float)SND_SAMPLERATE;
}

bool Mixer::SlotIsRelated(u32 slot)
{
	if (slot == 0)
	{
		return false;
	}

	const xr_string& Name = GMixer.slots[slot - 1].sound_name;
	SoundSourceState* Source = Snd_FindSource(&Name);
	if (Source == nullptr)
	{
		return false;
	}

	bool Result = Snd_SlotOcclusion(slot, *Source, 0.0f, nullptr) != ESlotOcclusionResult::False;
	Snd_ReleaseSource(&Name);
	return Result;
}

u32 Mixer::GetGameType(u32 Slot)
{
	xrSRWLockGuard Guard(g_SoundSourceLock, true);

	if (Slot == 0)
	{
		return 0.0f;
	}

	const SoundSourceState* Source = Snd_LookupSource(&GMixer.slots[Slot - 1].sound_name);
	if (Source == nullptr)
	{
		return 0;
	}

	return Source->Desc.game_type;
}

u32 Mixer::GetFlags(u32 Slot)
{
	if (Slot == 0)
	{
		return 0;
	}

	return GMixer.slots[Slot - 1].flags;
}

Mixer::State Mixer::GetState(u32 Slot)
{
	if (Slot == 0)
	{
		return State::Stopped;
	}

	return GMixer.slots[Slot - 1].fake_state;
}

Fvector* Mixer::GetParameters(u32 SlotID)
{
	if (SlotID == 0)
	{
		return nullptr;
	}

	return GMixer.slots[SlotID - 1].parameters;
}

ref_sound::ref_sound() : _unique_id(0)
{
	xrSRWLockGuard Guard1(GMixer.manage_lock);

	if (!GMixer.sounds.contains(this))
	{
		GMixer.sounds.emplace(this);
	}
}

// Sound pools copy references during insertion and reallocation; every live address must be registered.
ref_sound::ref_sound(const ref_sound& Other) : ref_sound()
{
	_unique_id = Other._unique_id;
	_p = Other._p;
	TimeToPropagade = Other.TimeToPropagade;
}

ref_sound::~ref_sound()
{
	xrSRWLockGuard Guard1(GMixer.manage_lock);

	if (GMixer.sounds.contains(this))
	{
		GMixer.sounds.erase(this);
	}
}
