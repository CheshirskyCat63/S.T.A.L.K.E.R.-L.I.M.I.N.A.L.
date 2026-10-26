#include "stdafx.h"
#include "SoundAcousticCapture.h"
#include "SoundSource.h"
#include <atomic>

namespace SoundAcousticCapture
{
	static constexpr u32 Blocks = 512;
	static constexpr u32 Frames = Blocks * SND_BLOCKSIZE;
	static constexpr u32 TrackSamples = Frames * SND_CHANNEL_COUNT;
	static constexpr const char* Names[] = {"voice_dry_prebus", "voice_room_send", "room_input_all", "room_pre_compressor", "room_post_compressor", "room_return_pre_master"};
	enum class State { Idle, Armed, Recording, Ready, Writing };

	class Recorder
	{
	public:
		std::atomic<State> Status{State::Idle};
		xr_vector<float> PCM;
		u64 Event = 0;
		u64 Serial = 0;
		u32 Room = 0;
		u32 Bus = 0;
		u32 Block = 0;
		string_path Sample = {};
		Fvector Source = {}, Listener = {};
		u64 BusEvent[Blocks] = {};
		u32 BusKind[Blocks] = {};
		bool Mixed[Blocks] = {};
		bool AttributionObserved[Blocks] = {};
		bool RoomReassigned = false;
	};
	static Recorder Capture;

	bool Arm()
	{
		if (Capture.Status.load(std::memory_order_acquire) != State::Idle) return false;
		Capture.PCM.assign(size_t(Stage::Count) * TrackSamples, 0.0f);
		Capture.Event = 0;
		Capture.Block = 0;
		Capture.RoomReassigned = false;
		memset(Capture.BusEvent, 0, sizeof(Capture.BusEvent));
		memset(Capture.BusKind, 0, sizeof(Capture.BusKind));
		memset(Capture.Mixed, 0, sizeof(Capture.Mixed));
		memset(Capture.AttributionObserved, 0, sizeof(Capture.AttributionObserved));
		Capture.Serial = Snd_GetTimestamp();
		Capture.Status.store(State::Armed, std::memory_order_release);
		return true;
	}

	void SelectVoice(u64 Event, const char* Sample, u32 Room, u32 Bus, const Fvector& Source, const Fvector& Listener)
	{
		if (Capture.Status.load(std::memory_order_relaxed) != State::Armed || Event == 0 || Room == 0 || Bus == 0) return;
		Capture.Event = Event;
		Capture.Room = Room;
		Capture.Bus = Bus;
		xr_strcpy(Capture.Sample, Sample);
		Capture.Source = Source;
		Capture.Listener = Listener;
		Capture.Status.store(State::Recording, std::memory_order_release);
	}

	void TapVoice(u64 Event, Stage Tap, u32 Bus, float** Data, float Begin, float End, float Left, float Right)
	{
		if (Capture.Status.load(std::memory_order_relaxed) != State::Recording || Event != Capture.Event ||
			(Tap != Stage::VoiceDry && (Tap != Stage::VoiceSend || Bus != Capture.Bus))) return;
		float* Dest = Capture.PCM.data() + size_t(Tap) * TrackSamples + Capture.Block * SND_BLOCKSIZE * SND_CHANNEL_COUNT;
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
		{
			const float Gain = lerp(Begin, End, float(Frame) / float(SND_BLOCKSIZE - 1));
			Dest[Frame * 2] += Data[0][Frame] * Gain * Left;
			Dest[Frame * 2 + 1] += Data[1][Frame] * Gain * Right;
		}
	}

	void TapBus(u32 Bus, Stage Tap, float** Data)
	{
		if (Capture.Status.load(std::memory_order_relaxed) != State::Recording || Bus != Capture.Bus ||
			Tap < Stage::RoomInput || Tap >= Stage::Count) return;
		float* Dest = Capture.PCM.data() + size_t(Tap) * TrackSamples + Capture.Block * SND_BLOCKSIZE * SND_CHANNEL_COUNT;
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
		{
			Dest[Frame * 2] = Data[0][Frame];
			Dest[Frame * 2 + 1] = Data[1][Frame];
		}
	}

	void SetProvenance(u32 Bus, u32 Room, u64 Event, u32 Kind, bool Mixed)
	{
		if (Capture.Status.load(std::memory_order_relaxed) != State::Recording || Bus != Capture.Bus) return;
		if (Room != Capture.Room)
		{
			Capture.RoomReassigned = true;
			Capture.Status.store(State::Ready, std::memory_order_release);
			return;
		}
		Capture.BusEvent[Capture.Block] = Event;
		Capture.BusKind[Capture.Block] = Kind;
		Capture.Mixed[Capture.Block] = Mixed || Event != Capture.Event;
		Capture.AttributionObserved[Capture.Block] = true;
	}

	void EndBlock()
	{
		if (Capture.Status.load(std::memory_order_relaxed) == State::Recording && ++Capture.Block == Blocks)
			Capture.Status.store(State::Ready, std::memory_order_release);
	}

	void DumpReady()
	{
		State Expected = State::Ready;
		if (!Capture.Status.compare_exchange_strong(Expected, State::Writing, std::memory_order_acq_rel)) return;
		const u32 Samples = Capture.Block * SND_BLOCKSIZE * SND_CHANNEL_COUNT;
		bool Success = true;
		for (u32 Track = 0; Track < u32(Stage::Count); ++Track)
		{
			string_path Name;
			xr_sprintf(Name, "acoustic-v8-%llu-%s.wav", (unsigned long long)Capture.Serial, Names[Track]);
			IWriter* Writer = FS.w_open("$logs$", Name);
			if (!Writer) { Success = false; continue; }
			Writer->w("RIFF", 4); Writer->w_u32(36 + Samples * sizeof(float)); Writer->w("WAVEfmt ", 8);
			Writer->w_u32(16); Writer->w_u16(3); Writer->w_u16(SND_CHANNEL_COUNT);
			Writer->w_u32(SND_SAMPLERATE); Writer->w_u32(SND_SAMPLERATE * SND_CHANNEL_COUNT * sizeof(float));
			Writer->w_u16(SND_CHANNEL_COUNT * sizeof(float)); Writer->w_u16(32);
			Writer->w("data", 4); Writer->w_u32(Samples * sizeof(float));
			Writer->w(Capture.PCM.data() + size_t(Track) * TrackSamples, Samples * sizeof(float));
			FS.w_close(Writer);
		}
		string_path Name;
		xr_sprintf(Name, "acoustic-v8-%llu.txt", (unsigned long long)Capture.Serial);
		IWriter* Writer = FS.w_open("$logs$", Name);
		if (Writer)
		{
			string2048 Line;
			xr_sprintf(Line, "event=%llu sample=%s room=%u bus=%u source=(%.3f %.3f %.3f) listener_start=(%.3f %.3f %.3f) frames=%u sample_rate=%u room_reassigned=%u\r\nVoice tracks are exact contributions. Room tracks contain ALL contributors and inherited tails; attribution per block below. Return is before bus fader/master. No second reverb was used.\r\nblock,bus_event,bus_kind,mixed,attribution_observed\r\n",
				(unsigned long long)Capture.Event, Capture.Sample, Capture.Room, Capture.Bus,
				Capture.Source.x, Capture.Source.y, Capture.Source.z, Capture.Listener.x, Capture.Listener.y, Capture.Listener.z,
				Capture.Block * SND_BLOCKSIZE, SND_SAMPLERATE, u32(Capture.RoomReassigned));
			Writer->w(Line, u32(xr_strlen(Line)));
			for (u32 Index = 0; Index < Capture.Block; ++Index)
			{
				xr_sprintf(Line, "%u,%llu,%u,%u,%u\r\n", Index, (unsigned long long)Capture.BusEvent[Index], Capture.BusKind[Index], u32(Capture.Mixed[Index]), u32(Capture.AttributionObserved[Index]));
				Writer->w(Line, u32(xr_strlen(Line)));
			}
			FS.w_close(Writer);
		}
		else Success = false;
		Msg("* [LIMINAL_PCM_V8] capture=%llu event=%llu frames=%u files_ok=%u directory=$logs$ mixed_room_output_is_not_isolated_voice=1",
			(unsigned long long)Capture.Serial, (unsigned long long)Capture.Event, Capture.Block * SND_BLOCKSIZE, u32(Success));
		Capture.Status.store(State::Idle, std::memory_order_release);
	}

	void Shutdown()
	{
		if (Capture.Status.load(std::memory_order_relaxed) == State::Recording)
			Capture.Status.store(State::Ready, std::memory_order_release);
		DumpReady();
		xr_vector<float>().swap(Capture.PCM);
		Capture.Status.store(State::Idle, std::memory_order_release);
	}
}
