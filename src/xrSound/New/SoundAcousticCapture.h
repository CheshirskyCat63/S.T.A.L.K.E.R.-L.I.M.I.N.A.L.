#pragma once

#include "SoundMeta.h"

// Optional bounded diagnostic recorder. No DSP, allocation, or file IO in taps.
// Arm is called under the mixer render lock; DumpReady runs after releasing it.
namespace SoundAcousticCapture
{
	enum class Stage : u32 { VoiceDry, VoiceSend, RoomInput, PreCompressor, PostCompressor, Return, Count };
	bool Arm();
	void SelectVoice(u64 Event, const char* Sample, u32 Room, u32 Bus, const Fvector& Source, const Fvector& Listener);
	void TapVoice(u64 Event, Stage Tap, u32 Bus, float** Data, float Begin, float End, float Left, float Right);
	void TapBus(u32 Bus, Stage Tap, float** Data);
	void SetProvenance(u32 Bus, u32 Room, u64 Event, u32 Kind, bool Mixed);
	void EndBlock();
	void DumpReady();
	void Shutdown();
}
