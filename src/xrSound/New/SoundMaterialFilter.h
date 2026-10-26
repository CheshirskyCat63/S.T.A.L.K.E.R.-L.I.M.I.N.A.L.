#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Two monotonic high shelves, S=1, from the W3C Audio EQ Cookbook (RBJ):
// https://www.w3.org/TR/audio-eq-cookbook/
// Crossover frequencies follow Steam Audio core/src/core/bands.cpp. This is
// three-band material filtering inside the native mixer, not another effect bus.
// Coefficients change only at block boundaries; two bounded filter histories
// crossfade for that block. No allocation, logging or geometry occurs here.
template <std::size_t Channels>
struct SoundMaterialFilter
{
	struct Coefficients
	{
		float B0 = 1.0f, B1 = 0.0f, B2 = 0.0f, A1 = 0.0f, A2 = 0.0f;
	};
	struct History
	{
		float X1 = 0.0f, X2 = 0.0f, Y1 = 0.0f, Y2 = 0.0f;
		float Process(float Input, const Coefficients& C)
		{
			const float Output = C.B0 * Input + C.B1 * X1 + C.B2 * X2 - C.A1 * Y1 - C.A2 * Y2;
			X2 = X1; X1 = Input; Y2 = Y1; Y1 = Output;
			return Output;
		}
	};
	Coefficients Current[2], Previous[2];
	History State[Channels][2], PreviousState[Channels][2];
	float Ratios[3] = {1.0f, 1.0f, 1.0f};
	float Gain = 1.0f, PreviousGain = 1.0f;
	bool Initialized = false, Transition = false;

	static Coefficients HighShelf(float Frequency, float PressureGain, float SampleRate)
	{
		if (PressureGain == 1.0f) return {};
		const double A = std::sqrt(double(PressureGain));
		const double W = 6.2831853071795864769 * Frequency / SampleRate;
		const double C = std::cos(W);
		const double V = std::sin(W) * std::sqrt(2.0 * A); // S=1, monotonic shelf.
		const double A0 = (A + 1.0) - (A - 1.0) * C + V;
		Coefficients Result;
		Result.B0 = float(A * ((A + 1.0) + (A - 1.0) * C + V) / A0);
		Result.B1 = float(-2.0 * A * ((A - 1.0) + (A + 1.0) * C) / A0);
		Result.B2 = float(A * ((A + 1.0) + (A - 1.0) * C - V) / A0);
		Result.A1 = float(2.0 * ((A - 1.0) - (A + 1.0) * C) / A0);
		Result.A2 = float(((A + 1.0) - (A - 1.0) * C - V) / A0);
		return Result;
	}

	void SetRatios(float Low, float Mid, float High, float SampleRate = 44100.0f)
	{
		Low = std::clamp(Low, 0.0f, 1.0f);
		Mid = std::clamp(Mid, 0.0f, 1.0f);
		High = std::clamp(High, 0.0f, 1.0f);
		if (Initialized && Low == Ratios[0] && Mid == Ratios[1] && High == Ratios[2]) return;
		for (std::size_t Band = 0; Band < 2; ++Band)
		{
			Previous[Band] = Current[Band];
			for (std::size_t Channel = 0; Channel < Channels; ++Channel) PreviousState[Channel][Band] = State[Channel][Band];
		}
		PreviousGain = Gain;
		Ratios[0] = Low; Ratios[1] = Mid; Ratios[2] = High;
		// Finite shelf ratios need a numerical floor; wholly blocked paths still
		// have zero outer pressure gain. This is -160 dB, not an audible wall floor.
		const float SafeLow = std::max(Low, 1.0e-8f), SafeMid = std::max(Mid, 1.0e-8f), SafeHigh = std::max(High, 1.0e-8f);
		Gain = std::max({Low, Mid, High}) > 0.0f ? SafeLow : 0.0f;
		Current[0] = HighShelf(800.0f, SafeMid / SafeLow, SampleRate);
		Current[1] = HighShelf(8000.0f, SafeHigh / SafeMid, SampleRate);
		Transition = Initialized;
		Initialized = true;
	}

	float ProcessSample(std::size_t Channel, float Input, std::size_t Frame, std::size_t Frames)
	{
		float Output = State[Channel][0].Process(Input, Current[0]);
		Output = Gain * State[Channel][1].Process(Output, Current[1]);
		if (Transition)
		{
			float Old = PreviousState[Channel][0].Process(Input, Previous[0]);
			Old = PreviousGain * PreviousState[Channel][1].Process(Old, Previous[1]);
			const float Weight = float(Frame + 1) / float(std::max<std::size_t>(Frames, 1));
			Output = Old + Weight * (Output - Old);
		}
		return Output;
	}

	void EndBlock() { Transition = false; }
	void Reset() { *this = {}; }
};
