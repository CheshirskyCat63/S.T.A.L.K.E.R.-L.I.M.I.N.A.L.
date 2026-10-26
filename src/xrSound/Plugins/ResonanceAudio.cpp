#include "stdafx.h"
#include <atomic>
#include "ResonanceAudio.h"
#include "../New/SoundEffect.h"
#include "../../3rd-party/resonance-audio/resonance_audio/api/resonance_audio_api.h"

// Real-time budget: 32 nearby firearm sources + 16 other nearby sources.
// Authored secondary gunshot layers retain native directional panning.
#define RESONANCE_HRTF_COUNT (48)
#define RESONANCE_HRTF_SHOOTING_COUNT (32)
#define RESONANCE_HRTF_AMBIENT_COUNT (16)
#define RESONANCE_HIGH_HRTF_DISTANCE (6.0f)
#define RESONANCE_HIGH_HRTF_COUNT (8)

enum ResonanceSpatializeParam : u32
{
	SpatializeRolloff,
	SpatializeBackAttenuation,
	SpatializeDoppler,
    SpatializePriority
};

enum ResonanceReverbParam : u32
{
	ReverbGain,
	ReverbSize,
	ReverbCutoff,
	ReverbDiffusion,
	ReverbReflections,
	ReverbDecay,
	ReverbDecayHf,
	ReverbAirHf,
	ReverbLateGain,
	ReverbDimensionsX,
	ReverbDimensionsY,
	ReverbDimensionsZ,
	ReverbRoomVolume,
	ReverbSurfaceArea,
	ReverbGeometryEarly,
	ReverbWetReference
};

struct ResonanceInstance
{
	vraudio::ResonanceAudioApi* Api = nullptr;
	vraudio::ResonanceAudioApi::SourceId Source = vraudio::ResonanceAudioApi::kInvalidSourceId;
    vraudio::RenderingMode Mode = vraudio::RenderingMode::kBinauralMediumQuality;
};

struct ResonanceState
{
	xr_vector<ResonanceInstance> Hrtfs;
	xr_vector<u32> FreeMediumHrtfs;
    xr_vector<u32> FreeHighHrtfs;
    u32 CreatedHighHrtfs = 0;
    std::atomic<u32> ActiveHighHrtfs{0};
    std::atomic<u32> ActiveShooting{0};
    std::atomic<u32> ActiveAmbient{0};
};

struct ResonanceSpatializeState
{
	SoundSpatialState Pan;
	u32 Hrtf;
    bool PriorityHrtf = false;
    bool HighQualityHrtf = false;
};

struct ResonanceReverbState
{
	ResonanceInstance Instance;
	u32 TailFrames;
	float Mono[SND_BLOCKSIZE];
	float Params[SND_EFFECT_PARAM_COUNT];
	float CoverFraction = 0.0f;
	float OpeningArea = 0.0f;
	float RoofOpeningArea = 0.0f;
	bool HasOpenGeometry = false;
	float MeanBoundaryLoss[9] = {};
	bool HasMaterialGeometry = false;
};

// One context per existing logical room, two finite aperture samples. This
// is a spatial decoder only: there is no second reverberator or source bus.
struct ResonanceRoomReturnState
{
	vraudio::ResonanceAudioApi* Api = nullptr;
	vraudio::ResonanceAudioApi::SourceId Sources[2] =
	{
		vraudio::ResonanceAudioApi::kInvalidSourceId,
		vraudio::ResonanceAudioApi::kInvalidSourceId
	};
	float Input[2][SND_BLOCKSIZE] = {};
	float Output[2][SND_BLOCKSIZE] = {};
	float Scratch[2][SND_BLOCKSIZE] = {};
	SoundSpatialState Pan[2] = {};
	bool WasHrtf = true;
};

static const SoundEffectParam SpatializeParams[] =
{
	{"rolloff", 2.0f, 0.1f, 4.0f},
	{"back_attenuation", 0.3f, 0.0f, 1.0f},
	{"doppler", 1.0f, 0.0f, 10.0f},
    {"priority", 0.0f, 0.0f, 1.0f}
};

static const SoundEffectParam ReverbParams[] =
{
	{"gain", 0.005f, 0.0f, 1.0f},
	{"size", 10.0f, 1.0f, 100.0f},
	{"cutoff", 5000.0f, 200.0f, 20000.0f},
	{"diffusion", 1.0f, 0.0f, 1.0f},
	{"reflections", 0.05f, 0.0f, 3.16227766f},
	{"decay", 1.49f, 0.1f, 20.0f},
	{"decay_hf", 0.83f, 0.1f, 2.0f},
	{"air_hf", 0.994f, 0.0f, 1.0f},
	{"late_gain", 1.0f, 0.0f, 10.0f},
	{"dimensions_x", 0.0f, 0.0f, 1000.0f},
	{"dimensions_y", 0.0f, 0.0f, 1000.0f},
	{"dimensions_z", 0.0f, 0.0f, 1000.0f},
	{"room_volume", 0.0f, 0.0f, 1.0e9f},
	{"surface_area", 0.0f, 0.0f, 1.0e7f},
	{"geometry_early", 0.0f, 0.0f, 1.0f},
	{"wet_reference", 0.0f, 0.0f, 100.0f}
};

static_assert(std::size(ReverbParams) <= SND_EFFECT_PARAM_COUNT);

static ResonanceState GResonance = {};

static void Resonance_DestroyInstance(ResonanceInstance* Instance)
{
	if (Instance->Api == nullptr)
	{
		return;
	}

	if (Instance->Source != vraudio::ResonanceAudioApi::kInvalidSourceId)
	{
		Instance->Api->DestroySource(Instance->Source);
	}

	delete Instance->Api;
	Instance->Api = nullptr;
	Instance->Source = vraudio::ResonanceAudioApi::kInvalidSourceId;
}

static bool Resonance_CreateInstance(ResonanceInstance* Instance, vraudio::RenderingMode Mode, bool IsRoom)
{
	Instance->Api = vraudio::CreateResonanceAudioApi(SND_CHANNEL_COUNT, SND_BLOCKSIZE, SND_SAMPLERATE);
	if (Instance->Api == nullptr)
	{
		return false;
	}

	Instance->Source = Instance->Api->CreateSoundObjectSource(Mode);
    Instance->Mode = Mode;
	if (Instance->Source == vraudio::ResonanceAudioApi::kInvalidSourceId)
	{
		Resonance_DestroyInstance(Instance);
		return false;
	}

	Instance->Api->EnableRoomEffects(IsRoom);
	Instance->Api->SetHeadPosition(0.0f, 0.0f, 0.0f);
	Instance->Api->SetHeadRotation(0.0f, 0.0f, 0.0f, 1.0f);
	Instance->Api->SetSourceDistanceModel(Instance->Source, vraudio::DistanceRolloffModel::kNone, 0.0f, 0.0f);
	Instance->Api->SetSourceDistanceAttenuation(Instance->Source, 1.0f);
	Instance->Api->SetSourceRoomEffectsGain(Instance->Source, IsRoom ? 1.0f : 0.0f);
	return true;
}

void Resonance_DestroyRoomReturn(void* StatePtr)
{
	auto* State = static_cast<ResonanceRoomReturnState*>(StatePtr);
	if (State == nullptr)
	{
		return;
	}
	if (State->Api != nullptr)
	{
		for (auto Source : State->Sources)
		{
			if (Source != vraudio::ResonanceAudioApi::kInvalidSourceId)
			{
				State->Api->DestroySource(Source);
			}
		}
		delete State->Api;
	}
	xr_delete(State);
}

void Resonance_SilenceRoomReturn(void* StatePtr)
{
	auto* State = static_cast<ResonanceRoomReturnState*>(StatePtr);
	if (State == nullptr || State->Api == nullptr)
	{
		return;
	}
	memset(State->Input, 0, sizeof(State->Input));
	for (u32 Channel = 0; Channel < 2; ++Channel)
	{
		const float* Input[1] = {State->Input[Channel]};
		State->Api->SetPlanarBuffer(State->Sources[Channel], Input, 1, SND_BLOCKSIZE);
	}
	float* Output[2] = {State->Output[0], State->Output[1]};
	// Native HRTF decoder retains one block on empty input. Drain it while
	// inaudible so a later aperture cannot replay the previous field.
	State->Api->FillPlanarOutputBuffer(2, SND_BLOCKSIZE, Output);
	memset(State->Output, 0, sizeof(State->Output));
}

void Resonance_PrimeRoomReturn(void* StatePtr)
{
	auto* State = static_cast<ResonanceRoomReturnState*>(StatePtr);
	if (State == nullptr || State->Api == nullptr)
	{
		return;
	}
	memset(State->Input, 0, sizeof(State->Input));
	float* Output[2] = {State->Output[0], State->Output[1]};
	// Allocate the native binaural path on the game thread. A finite impulse
	// prevents silence short-circuits; eight zero blocks drain the HRTF FIR.
	State->Api->SetStereoSpeakerMode(false);
	for (u32 Block = 0; Block < 9; ++Block)
	{
		for (u32 Channel = 0; Channel < 2; ++Channel)
		{
			State->Input[Channel][0] = Block == 0 ? 0.0001f : 0.0f;
			const float* Input[1] = {State->Input[Channel]};
			State->Api->SetPlanarBuffer(State->Sources[Channel], Input, 1, SND_BLOCKSIZE);
		}
		State->Api->FillPlanarOutputBuffer(2, SND_BLOCKSIZE, Output);
	}
	memset(State->Input, 0, sizeof(State->Input));
	memset(State->Output, 0, sizeof(State->Output));
	State->Pan[0] = {};
	State->Pan[1] = {};
	State->WasHrtf = true;
}

void* Resonance_CreateRoomReturn()
{
	auto* State = new ResonanceRoomReturnState();
	State->Api = vraudio::CreateResonanceAudioApi(2, SND_BLOCKSIZE, SND_SAMPLERATE);
	if (State->Api == nullptr)
	{
		Resonance_DestroyRoomReturn(State);
		return nullptr;
	}
	State->Api->EnableRoomEffects(false);
	State->Api->SetHeadPosition(0.0f, 0.0f, 0.0f);
	State->Api->SetHeadRotation(0.0f, 0.0f, 0.0f, 1.0f);
	for (u32 Channel = 0; Channel < 2; ++Channel)
	{
		auto& Source = State->Sources[Channel];
		Source = State->Api->CreateSoundObjectSource(vraudio::RenderingMode::kBinauralMediumQuality);
		if (Source == vraudio::ResonanceAudioApi::kInvalidSourceId)
		{
			Resonance_DestroyRoomReturn(State);
			return nullptr;
		}
		State->Api->SetSourceDistanceModel(Source, vraudio::DistanceRolloffModel::kNone, 0.0f, 0.0f);
		State->Api->SetSourceDistanceAttenuation(Source, 1.0f);
		State->Api->SetSourceRoomEffectsGain(Source, 0.0f);
		State->Api->SetSoundObjectNearFieldEffectGain(Source, 0.0f);
		State->Api->SetSourcePosition(Source, Channel == 0 ? -0.5f : 0.5f, 0.0f, -1.0f);
	}
	Resonance_PrimeRoomReturn(State);
	return State;
}

bool Resonance_ProcessRoomReturn(void* StatePtr, float** Data, const float* LocalDirections,
	float SpreadDegrees, float BeginFraction, float EndFraction)
{
	auto* State = static_cast<ResonanceRoomReturnState*>(StatePtr);
	if (State == nullptr || State->Api == nullptr || LocalDirections == nullptr)
	{
		return false;
	}
	const bool Hrtf = psSoundFlags.is(ss_HRTF);
	float* Output[2] = {State->Output[0], State->Output[1]};
	if (!Hrtf && State->WasHrtf)
	{
		// Drain a previously active FIR once when HRTF is disabled. Its old
		// samples must not reappear if headphones are enabled much later.
		memset(State->Input, 0, sizeof(State->Input));
		for (u32 Channel = 0; Channel < 2; ++Channel)
		{
			const float* Input[1] = {State->Input[Channel]};
			State->Api->SetPlanarBuffer(State->Sources[Channel], Input, 1, SND_BLOCKSIZE);
		}
		State->Api->FillPlanarOutputBuffer(2, SND_BLOCKSIZE, Output);
	}
	State->WasHrtf = Hrtf;
	memset(State->Output, 0, sizeof(State->Output));
	// Two stereo components are spatialized separately. 1/sqrt(2) bounds
	// coherent addition before native HRTF coloration. This is not a claim
	// that every phase relationship has identical input/output energy.
	constexpr float ComponentWeight = 0.7071067811865475f;
	for (u32 Channel = 0; Channel < 2; ++Channel)
	{
		const float* Direction = LocalDirections + Channel * 3;
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
		{
			State->Input[Channel][Frame] = Data[Channel][Frame] * ComponentWeight;
		}
		if (Hrtf)
		{
			State->Api->SetSourcePosition(State->Sources[Channel], Direction[0], Direction[1], -Direction[2]);
			State->Api->SetSoundObjectSpread(State->Sources[Channel], SpreadDegrees);
			const float* Input[1] = {State->Input[Channel]};
			State->Api->SetPlanarBuffer(State->Sources[Channel], Input, 1, SND_BLOCKSIZE);
			State->Pan[Channel].IsPanned = false;
		}
		else
		{
			// Match IX-Ray's native no-HRTF panner. Angular extent is encoded
			// by the two actual aperture sample directions. Resonance's stereo
			// soundfield decoder changes DC gain with source spread.
			memcpy(State->Scratch[0], State->Input[Channel], sizeof(State->Scratch[0]));
			memcpy(State->Scratch[1], State->Input[Channel], sizeof(State->Scratch[1]));
			float* Scratch[2] = {State->Scratch[0], State->Scratch[1]};
			SoundEffectProcess Process = {Scratch, nullptr, nullptr, nullptr, true};
			SoundSpatialPosition Position = {};
			Position.Local.set(Direction[0], Direction[1], Direction[2]);
			Position.Distance = Position.MinDistance = 1.0f;
			Position.MaxDistance = 2.0f;
			Position.Gain = 1.0f;
			Snd_SpatialPan(&State->Pan[Channel], &Process, &Position, 0.0f, 0.0f);
			for (u32 OutputChannel = 0; OutputChannel < 2; ++OutputChannel)
			{
				for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
				{
					State->Output[OutputChannel][Frame] += State->Scratch[OutputChannel][Frame];
				}
			}
		}
	}
	if (Hrtf && !State->Api->FillPlanarOutputBuffer(2, SND_BLOCKSIZE, Output))
	{
		return false;
	}
	for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
	{
		const float Fraction = lerp(BeginFraction, EndFraction, float(Frame) / float(SND_BLOCKSIZE - 1));
		for (u32 Channel = 0; Channel < 2; ++Channel)
		{
			// A pressure crossfade retains the unmodified local/wall stereo
			// branch at zero, and never runs the field into a second reverb.
			Data[Channel][Frame] = lerp(Data[Channel][Frame], State->Output[Channel][Frame], Fraction);
		}
	}
	return true;
}

static u32 Resonance_AcquireHrtf(bool priority, bool wantHighQuality, bool* actualHighQuality)
{
    auto& classCount = priority ? GResonance.ActiveShooting : GResonance.ActiveAmbient;
    const u32 classLimit = priority ? RESONANCE_HRTF_SHOOTING_COUNT : RESONANCE_HRTF_AMBIENT_COUNT;
    if (classCount.load(std::memory_order_relaxed) >= classLimit)
        return 0;

    *actualHighQuality = false;
    u32 result = 0;
    // High-quality third-order binaural mode is reserved for very near
    // firearm impulses (at most 8 simultaneous voices).
    if (wantHighQuality && priority &&
        GResonance.ActiveHighHrtfs.load(std::memory_order_relaxed) < RESONANCE_HIGH_HRTF_COUNT)
    {
        if (!GResonance.FreeHighHrtfs.empty())
        {
            result = GResonance.FreeHighHrtfs.back();
            GResonance.FreeHighHrtfs.pop_back();
        }
        else if (GResonance.Hrtfs.size() < RESONANCE_HRTF_COUNT &&
            GResonance.CreatedHighHrtfs < RESONANCE_HIGH_HRTF_COUNT)
        {
            ResonanceInstance Instance;
            if (Resonance_CreateInstance(&Instance, vraudio::RenderingMode::kBinauralHighQuality, false))
            {
                GResonance.Hrtfs.push_back(Instance);
                result = (u32)GResonance.Hrtfs.size();
                ++GResonance.CreatedHighHrtfs;
            }
        }
        if (result != 0)
        {
            *actualHighQuality = true;
            GResonance.ActiveHighHrtfs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Second-order binaural mode uses a smaller spherical-harmonic decoder
    // than high-Q, while retaining head-related directional positioning.
    if (result == 0)
    {
        if (!GResonance.FreeMediumHrtfs.empty())
        {
            result = GResonance.FreeMediumHrtfs.back();
            GResonance.FreeMediumHrtfs.pop_back();
        }
        else if (GResonance.Hrtfs.size() < RESONANCE_HRTF_COUNT)
        {
            ResonanceInstance Instance;
            if (Resonance_CreateInstance(&Instance, vraudio::RenderingMode::kBinauralMediumQuality, false))
            {
                GResonance.Hrtfs.push_back(Instance);
                result = (u32)GResonance.Hrtfs.size();
            }
        }
    }

    if (result != 0)
        classCount.fetch_add(1, std::memory_order_relaxed);
    return result;
}

static void Resonance_ReleaseHrtf(u32* Hrtf, bool* PriorityHrtf, bool* HighQualityHrtf)
{
    if (*Hrtf != 0)
    {
        if (*HighQualityHrtf)
        {
            GResonance.FreeHighHrtfs.push_back(*Hrtf);
            GResonance.ActiveHighHrtfs.fetch_sub(1, std::memory_order_relaxed);
        }
        else
        {
            GResonance.FreeMediumHrtfs.push_back(*Hrtf);
        }
        auto& classCount = *PriorityHrtf ? GResonance.ActiveShooting : GResonance.ActiveAmbient;
        classCount.fetch_sub(1, std::memory_order_relaxed);
        *Hrtf = 0;
        *PriorityHrtf = false;
        *HighQualityHrtf = false;
    }
}

u32 Resonance_GetActiveHrtfs()
{
    return GResonance.ActiveShooting.load(std::memory_order_relaxed) +
           GResonance.ActiveAmbient.load(std::memory_order_relaxed);
}

u32 Resonance_GetActiveShootingHrtfs()
{
    return GResonance.ActiveShooting.load(std::memory_order_relaxed);
}

u32 Resonance_GetActiveHighHrtfs()
{
    return GResonance.ActiveHighHrtfs.load(std::memory_order_relaxed);
}

static void Resonance_Spatialize(ResonanceSpatializeState* State, SoundEffectProcess* Process)
{
	const float* Params = Process->Params;
	SoundSpatialPosition Position;
	Snd_SpatialLocate(Process, Params[SpatializeDoppler], &Position);

    const bool priority = Params[SpatializePriority] >= 0.5f;
    const bool relevant = psSoundFlags.is(ss_HRTF) && Process->Voice->AllowHrtf &&
        Snd_DistanceAttenuation(&Position, Params[SpatializeRolloff]) > 0.004f;
    // Only very near firearm sound objects justify 3rd-order HRTF.
    const bool nearHigh = priority &&
        Position.Distance <= RESONANCE_HIGH_HRTF_DISTANCE &&
        Snd_DistanceAttenuation(&Position, Params[SpatializeRolloff]) > 0.025f;
    if (!relevant || (State->Hrtf != 0 && State->PriorityHrtf != priority) ||
        (State->Hrtf != 0 && State->HighQualityHrtf && !nearHigh))
    {
        Resonance_ReleaseHrtf(&State->Hrtf, &State->PriorityHrtf, &State->HighQualityHrtf);
    }
    if (relevant && State->Hrtf == 0)
    {
        bool gotHighQuality = false;
        State->Hrtf = Resonance_AcquireHrtf(priority, nearHigh, &gotHighQuality);
        if (State->Hrtf != 0)
        {
            State->PriorityHrtf = priority;
            State->HighQualityHrtf = gotHighQuality;
        }
    }

	if (State->Hrtf == 0)
	{
		Snd_SpatialPan(&State->Pan, Process, &Position, Params[SpatializeRolloff], Params[SpatializeBackAttenuation]);
		return;
	}

	State->Pan.IsPanned = false;

	Fvector Direction = Position.Local;
	if (Direction.square_magnitude() < EPS)
	{
		Direction.set(0.0f, 0.0f, 1.0f);
	}

	ResonanceInstance& Instance = GResonance.Hrtfs[State->Hrtf - 1];
	// IX-Ray camera space is +Z forward; Resonance camera space is -Z forward.
	Instance.Api->SetSourcePosition(Instance.Source, Direction.x, Direction.y, -Direction.z);

	const float* MonoInput[1] = {Process->Data[0]};
	Instance.Api->SetPlanarBuffer(Instance.Source, MonoInput, 1, SND_BLOCKSIZE);
	Instance.Api->FillPlanarOutputBuffer(SND_CHANNEL_COUNT, SND_BLOCKSIZE, Process->Data);

	float Attenuation = Snd_DistanceAttenuation(&Position, Params[SpatializeRolloff]);
	for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
	{
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; Frame++)
		{
			Process->Data[Channel][Frame] *= Attenuation;
		}
	}
}

static bool Resonance_SpatializeProc(void* StatePtr, SoundEffectOp Op, void* Arg)
{
	ResonanceSpatializeState* State = (ResonanceSpatializeState*)StatePtr;
	switch (Op)
	{
	case SoundEffectOp::Describe:
		*(SoundEffectDesc*)Arg = {SoundEffectScope::Voice, sizeof(ResonanceSpatializeState), (u32)std::size(SpatializeParams), SpatializeParams};
		return true;
	case SoundEffectOp::Create:
		return true;
	case SoundEffectOp::Reset:
	case SoundEffectOp::Destroy:
		Resonance_ReleaseHrtf(&State->Hrtf, &State->PriorityHrtf, &State->HighQualityHrtf);
		State->Pan.IsPanned = false;
		return true;
	case SoundEffectOp::Process:
		Resonance_Spatialize(State, (SoundEffectProcess*)Arg);
		return true;
	default:
		return false;
	}
}

static void Resonance_ApplyReverb(ResonanceReverbState* State, const float* Params, bool Immediate = false)
{
	const bool HasGeometry = Params[ReverbRoomVolume] > 0.0f && Params[ReverbSurfaceArea] > 0.0f &&
		Params[ReverbDimensionsX] > 0.0f && Params[ReverbDimensionsY] > 0.0f && Params[ReverbDimensionsZ] > 0.0f;
	const bool GeometryEarly = HasGeometry && Params[ReverbGeometryEarly] >= 0.5f;
	vraudio::ReflectionProperties Reflection = {};
	Reflection.room_dimensions[0] = HasGeometry ? Params[ReverbDimensionsX] : Params[ReverbSize];
	Reflection.room_dimensions[1] = HasGeometry ? Params[ReverbDimensionsY] : Params[ReverbSize];
	Reflection.room_dimensions[2] = HasGeometry ? Params[ReverbDimensionsZ] : Params[ReverbSize];
	Reflection.room_rotation[3] = 1.0f;
	Reflection.cutoff_frequency = Params[ReverbCutoff];
	Reflection.gain = GeometryEarly ? 0.0f : Params[ReverbReflections];
	// CoP diffusion describes echo density, not wall reflectivity. Invert the
	// Eyring relation used by Resonance's room utilities for the preset cube:
	// pressure reflection = sqrt(exp(-0.161 * V / (surface area * RT60))).
	// This is the equivalent uniform boundary loss implied by size and decay;
	// it does not identify materials or separate their loss from air absorption.
	const float BoundaryReflection = std::exp(-0.161f * Params[ReverbSize] / (12.0f * Params[ReverbDecay]));
	for (float& Coefficient : Reflection.coefficients)
	{
		Coefficient = std::clamp(BoundaryReflection, 0.0f, 1.0f);
	}

	vraudio::ReverbProperties Reverb = {};
	Reverb.immediate = Immediate;
	// room_effects_utils.cc uses 0.045 as the spectral tail loudness reference.
	// This API applies Reverb.gain at both the input mixer and spectral/onset
	// processors. The square root applies that reference and the authored late
	// amplitude once, independently of the early-reflection and shared room gain.
	constexpr float NativeLateNormalization = 0.045f;
	Reverb.gain = std::sqrt(NativeLateNormalization * Params[ReverbLateGain]);
	// Version 2 separates open boundary area (including roof damage) from
	// the retained preset-equivalent closed-surface absorption. An opening
	// shortens the tail without destroying the connected room volume. Legacy
	// sidecars retain their exact previous V/S model.
	const float RoomDecay = HasGeometry
		? Resonance_ComputeRoomDecay(Params[ReverbRoomVolume], Params[ReverbSurfaceArea],
			Params[ReverbSize], Params[ReverbDecay], State->OpeningArea, State->HasOpenGeometry)
		: Params[ReverbDecay];
	float MaxDecay = 0.0f;
	for (u32 Band = 0; Band < std::size(Reverb.rt60_values); Band++)
	{
		// Material spectra already include band-dependent surface and air loss;
		// multiplying the legacy HF preset again would count that loss twice.
		const float Decay = HasGeometry && State->HasMaterialGeometry
			? Resonance_ComputeMaterialRoomDecay(Params[ReverbRoomVolume], Params[ReverbSurfaceArea],
				State->OpeningArea, State->MeanBoundaryLoss, Band)
			: Band >= 6 ? RoomDecay * Params[ReverbDecayHf] * Params[ReverbAirHf] : RoomDecay;
		Reverb.rt60_values[Band] = std::clamp(Decay, 0.05f, 120.0f);
		MaxDecay = std::max(MaxDecay, Reverb.rt60_values[Band]);
	}

	const float MaxDimension = std::max(Reflection.room_dimensions[0],
		std::max(Reflection.room_dimensions[1], Reflection.room_dimensions[2]));
	const float ReflectionDelay = GeometryEarly ? 0.0f : (0.5f * MaxDimension + 1.0f) / 343.0f;
	// Retire room slots after the nominal -90 dB decay envelope, not at -60 dB.
	// This bounds retained state; it is not an assertion about a measured room.
	State->TailFrames = (u32)std::ceil(std::max(1.5f * MaxDecay, ReflectionDelay) * SND_SAMPLERATE) + SND_BLOCKSIZE;
	State->Instance.Api->SetReflectionProperties(Reflection);
	State->Instance.Api->SetReverbProperties(Reverb);
	if (Params != State->Params)
	{
		memcpy(State->Params, Params, sizeof(State->Params));
	}
}

void Resonance_SetRoomOpenGeometry(void* StatePtr, float CoverFraction,
	float OpeningArea, float RoofOpeningArea, bool HasOpenGeometry)
{
	auto* State = static_cast<ResonanceReverbState*>(StatePtr);
	if (State == nullptr)
	{
		return;
	}
	State->CoverFraction = CoverFraction;
	State->OpeningArea = OpeningArea;
	State->RoofOpeningArea = RoofOpeningArea;
	State->HasOpenGeometry = HasOpenGeometry;
	// An old-format room reusing a material room bus must not inherit spectra.
	State->HasMaterialGeometry = false;
	memset(State->MeanBoundaryLoss, 0, sizeof(State->MeanBoundaryLoss));
}

void Resonance_SetRoomMaterials(void* StatePtr, const float* MeanBoundaryLoss, bool HasMaterials)
{
	auto* State = static_cast<ResonanceReverbState*>(StatePtr);
	if (State == nullptr)
	{
		return;
	}
	State->HasMaterialGeometry = HasMaterials && MeanBoundaryLoss != nullptr;
	memset(State->MeanBoundaryLoss, 0, sizeof(State->MeanBoundaryLoss));
	if (State->HasMaterialGeometry)
	{
		memcpy(State->MeanBoundaryLoss, MeanBoundaryLoss, sizeof(State->MeanBoundaryLoss));
	}
}

float Resonance_GetWetReference(const void* StatePtr)
{
	const auto* State = (const ResonanceReverbState*)StatePtr;
	return State != nullptr ? State->Params[ReverbWetReference] : 0.0f;
}

// Create/Reset are game-thread operations. Prime the graph's lazy processing
// buffers before it can be reached by the audio callback.
static void Resonance_PrimeReverb(ResonanceReverbState* State)
{
	float Output[SND_CHANNEL_COUNT][SND_BLOCKSIZE] = {};
	float* OutputChannels[SND_CHANNEL_COUNT] = {Output[0], Output[1]};
	memset(State->Mono, 0, sizeof(State->Mono));
	const float* InputChannels[1] = {State->Mono};
	// Immediate inactive-room properties avoid interpolation. Eight blocks
	// finish the six native onset partitions and their processing boundary.
	for (u32 Block = 0; Block < 8; ++Block)
	{
		State->Instance.Api->SetPlanarBuffer(State->Instance.Source, InputChannels, 1, SND_BLOCKSIZE);
		State->Instance.Api->FillPlanarOutputBuffer(SND_CHANNEL_COUNT, SND_BLOCKSIZE, OutputChannels);
	}
}

void Resonance_PrimeInactiveReverb(void* StatePtr)
{
	auto* State = (ResonanceReverbState*)StatePtr;
	if (State != nullptr && State->Instance.Api != nullptr)
	{
		Resonance_ApplyReverb(State, State->Params, true);
		Resonance_PrimeReverb(State);
	}
}

static bool Resonance_ReverbProc(void* StatePtr, SoundEffectOp Op, void* Arg)
{
	ResonanceReverbState* State = (ResonanceReverbState*)StatePtr;
	switch (Op)
	{
	case SoundEffectOp::Describe:
		*(SoundEffectDesc*)Arg = {SoundEffectScope::Bus, sizeof(ResonanceReverbState), (u32)std::size(ReverbParams), ReverbParams};
		return true;
	case SoundEffectOp::Create:
		if (Resonance_CreateInstance(&State->Instance, vraudio::RenderingMode::kRoomEffectsOnly, true))
		{
			Resonance_ApplyReverb(State, ((SoundEffectCreate*)Arg)->Params, true);
			Resonance_PrimeReverb(State);
			return true;
		}
		return false;
	case SoundEffectOp::Reset:
		// The public API has no allocation-free tail flush. Room buses are
		// reused only after their finite tail drained; a callback reset must
		// never reconstruct the graph. The allocator primes after Update.
		return true;
	case SoundEffectOp::Update:
		if (State->Instance.Api != nullptr)
		{
			Resonance_ApplyReverb(State, (const float*)Arg);
		}
		return true;
	case SoundEffectOp::Destroy:
		Resonance_DestroyInstance(&State->Instance);
		return true;
	case SoundEffectOp::Tail:
		*(u32*)Arg = State->TailFrames;
		return true;
	case SoundEffectOp::Process:
	{
		SoundEffectProcess* Process = (SoundEffectProcess*)Arg;
		if (State->Instance.Api == nullptr)
		{
			memset(Process->Data[0], 0, SND_BLOCKSIZE * sizeof(float));
			memset(Process->Data[1], 0, SND_BLOCKSIZE * sizeof(float));
			return true;
		}

		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; Frame++)
		{
			State->Mono[Frame] = (Process->Data[0][Frame] + Process->Data[1][Frame]) * 0.5f;
		}

		const float* MonoInput[1] = {State->Mono};
		State->Instance.Api->SetPlanarBuffer(State->Instance.Source, MonoInput, 1, SND_BLOCKSIZE);
		State->Instance.Api->FillPlanarOutputBuffer(SND_CHANNEL_COUNT, SND_BLOCKSIZE, Process->Data);

		float Gain = Process->Params[ReverbGain];
		for (u32 Channel = 0; Channel < SND_CHANNEL_COUNT; Channel++)
		{
			for (u32 Frame = 0; Frame < SND_BLOCKSIZE; Frame++)
			{
				Process->Data[Channel][Frame] *= Gain;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

void Resonance_RegisterEffects()
{
    // All 48 native HRTF graph contexts are prepared during startup, before
    // the audio callback runs. This eliminates XR instance construction
    // and heap allocation during the first heavy NPC gunfight.
    if (GResonance.Hrtfs.empty())
    {
        GResonance.Hrtfs.reserve(RESONANCE_HRTF_COUNT);
        GResonance.FreeMediumHrtfs.reserve(RESONANCE_HRTF_COUNT);
        GResonance.FreeHighHrtfs.reserve(RESONANCE_HIGH_HRTF_COUNT);
        for (u32 i = 0; i < RESONANCE_HIGH_HRTF_COUNT; ++i)
        {
            ResonanceInstance inst;
            if (!Resonance_CreateInstance(&inst, vraudio::RenderingMode::kBinauralHighQuality, false))
                break;
            GResonance.Hrtfs.push_back(inst);
            GResonance.FreeHighHrtfs.push_back((u32)GResonance.Hrtfs.size());
            ++GResonance.CreatedHighHrtfs;
        }
        for (u32 i = 0; i < RESONANCE_HRTF_COUNT - RESONANCE_HIGH_HRTF_COUNT; ++i)
        {
            ResonanceInstance inst;
            if (!Resonance_CreateInstance(&inst, vraudio::RenderingMode::kBinauralMediumQuality, false))
                break;
            GResonance.Hrtfs.push_back(inst);
            GResonance.FreeMediumHrtfs.push_back((u32)GResonance.Hrtfs.size());
        }
        Msg("* [LIMINAL_HRTF] prepared=%zu third_order=%u second_order=%zu (no runtime construction in normal combat)",
            GResonance.Hrtfs.size(), GResonance.CreatedHighHrtfs,
            GResonance.FreeMediumHrtfs.size());
    }
	Snd_RegisterEffect("spatialize", Resonance_SpatializeProc);
	Snd_RegisterEffect("reverb", Resonance_ReverbProc);
}

void Resonance_Shutdown()
{
	for (ResonanceInstance& Instance : GResonance.Hrtfs)
	{
		Resonance_DestroyInstance(&Instance);
	}

	GResonance.Hrtfs.clear();
	GResonance.FreeMediumHrtfs.clear();
    GResonance.FreeHighHrtfs.clear();
    GResonance.CreatedHighHrtfs = 0;
    GResonance.ActiveHighHrtfs.store(0, std::memory_order_relaxed);
    GResonance.ActiveAmbient.store(0, std::memory_order_relaxed);
    GResonance.ActiveShooting.store(0, std::memory_order_relaxed);
}
