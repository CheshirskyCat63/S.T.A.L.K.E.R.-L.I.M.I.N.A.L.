#pragma once
#include <cmath>

enum class EBodyRegion : unsigned char
{
	Head, Torso, LeftArm, RightArm, LeftLeg, RightLeg, All, Invalid = 255
};

struct SBodyHealthSettings
{
	float Maximum[6] = {11.f, 11.f, 5.f, 5.f, 5.f, 5.f};
	float DamageScale = 0.85f;
	float MinimumDamage = 0.0293f;
	float HeadDamageCap = 7.f;
	float HeadPenaltyThreshold = 2.f;
	float RunSlope = 0.0625f;
	float RunBase = 0.375f;
	float SprintSlope = 0.025f;
	float SprintBase = 0.75f;
	float ConcussionRecovery = 0.02f;
};

struct SBodyTreatment
{
	float Temporary[6] = {};
	float Stabilize[6] = {};
	float Interval = 60.f;
};

// Body-specific state. CActorCondition owns its lifetime, damage and persistence.
struct CBodyHealthState
{
	SBodyHealthSettings Settings;
	float Health[6] = {};
	float Temporary[6] = {};
	float DecayRemaining = 60.f;
	float DecayInterval = 60.f;
	float Concussion = 0.f;
	float ConcussionAge = 0.f;

	CBodyHealthState() { Reset(); }
	static bool RegionValid(EBodyRegion Region) { return static_cast<unsigned char>(Region) < 6; }
	static float Limit(float Value, float Minimum, float Maximum)
	{
		return Value < Minimum ? Minimum : (Value > Maximum ? Maximum : Value);
	}
	void Reset()
	{
		for (unsigned I = 0; I < 6; ++I)
		{
			Health[I] = Settings.Maximum[I];
			Temporary[I] = 0.f;
		}
		DecayRemaining = DecayInterval = 60.f;
		Concussion = ConcussionAge = 0.f;
	}
	void Validate()
	{
		for (unsigned I = 0; I < 6; ++I)
		{
			Health[I] = std::isfinite(Health[I]) ? Limit(Health[I], 0.f, Settings.Maximum[I]) : Settings.Maximum[I];
			Temporary[I] = std::isfinite(Temporary[I]) ? Limit(Temporary[I], 0.f, Health[I]) : 0.f;
		}
		if (!std::isfinite(DecayInterval) || DecayInterval <= 0.f) { DecayInterval = 60.f; }
		if (!std::isfinite(DecayRemaining) || DecayRemaining <= 0.f) { DecayRemaining = DecayInterval; }
		Concussion = std::isfinite(Concussion) ? Limit(Concussion, 0.f, Settings.Maximum[0]) : 0.f;
		ConcussionAge = std::isfinite(ConcussionAge) && ConcussionAge >= 0.f ? ConcussionAge : 0.f;
	}
	float ApplyDamage(EBodyRegion Region, float Damage)
	{
		if (!RegionValid(Region) || !std::isfinite(Damage) || Damage <= Settings.MinimumDamage) { return 0.f; }
		const unsigned Index = static_cast<unsigned char>(Region);
		float Amount = std::ceil(Damage * (Region == EBodyRegion::Head ? 200.f : 100.f) / 11.f * Settings.DamageScale);
		if (Region == EBodyRegion::Head && Amount > Settings.HeadDamageCap) { Amount = Settings.HeadDamageCap; }
		const float Lost = Amount < Health[Index] ? Amount : Health[Index];
		Health[Index] -= Lost;
		if (Temporary[Index] > Health[Index]) { Temporary[Index] = Health[Index]; }
		if (Region == EBodyRegion::Head && Lost >= Settings.HeadPenaltyThreshold)
		{
			Concussion = Limit(Concussion + Lost, 0.f, Settings.Maximum[0]);
			ConcussionAge = 0.f;
		}
		return Lost;
	}
	bool Treat(EBodyRegion Region, const SBodyTreatment& Treatment)
	{
		if (!RegionValid(Region) && Region != EBodyRegion::All) { return false; }
		bool Changed = false;
		bool FirstAid = false;
		for (unsigned I = 0; I < 6; ++I)
		{
			if (Region != EBodyRegion::All && I != static_cast<unsigned char>(Region)) { continue; }
			const float Capacity = Settings.Maximum[I] - Health[I];
			const float Bonus = Limit(Treatment.Temporary[I], 0.f, Capacity);
			const float Before = Temporary[I];
			Health[I] += Bonus;
			Temporary[I] = Limit(Temporary[I] + Bonus - Treatment.Stabilize[I], 0.f, Health[I]);
			Changed = Changed || Bonus > 0.f || Temporary[I] != Before || Treatment.Temporary[I] > 0.f && Before > 0.f;
			FirstAid = FirstAid || Treatment.Temporary[I] > 0.f;
		}
		if (FirstAid && std::isfinite(Treatment.Interval) && Treatment.Interval > 0.f)
		{
			DecayRemaining = DecayInterval = Treatment.Interval;
		}
		return Changed;
	}
	void Advance(float Seconds)
	{
		if (!std::isfinite(Seconds) || Seconds <= 0.f) { return; }
		Concussion = Limit(Concussion - Seconds * Settings.ConcussionRecovery, 0.f, Settings.Maximum[0]);
		ConcussionAge += Seconds;
		DecayRemaining -= Seconds;
		if (DecayRemaining <= 0.f)
		{
			const float Steps = std::floor(-DecayRemaining / DecayInterval) + 1.f;
			DecayRemaining += Steps * DecayInterval;
			for (unsigned I = 0; I < 6; ++I)
			{
				const float Lost = Temporary[I] < Steps ? Temporary[I] : Steps;
				Temporary[I] -= Lost;
				Health[I] = Limit(Health[I] - Lost, 0.f, Settings.Maximum[I]);
			}
		}
	}
	bool Fatal() const { return Health[0] <= 0.f || Health[1] <= 0.f; }
	void Sleep(float Hours)
	{
		if (!std::isfinite(Hours) || Hours <= 0.f) { return; }
		for (unsigned I = 0; I < 6; ++I)
		{
			Health[I] = Limit(Health[I] - Temporary[I], I < 2 ? 1.f : 0.f, Settings.Maximum[I]);
			Temporary[I] = 0.f;
		}
		constexpr unsigned Order[6] = {0, 1, 3, 2, 5, 4};
		for (unsigned I : Order)
		{
			const float Missing = Settings.Maximum[I] - Health[I];
			const float Restored = Hours < Missing ? Hours : Missing;
			Health[I] += Restored;
			Hours -= Restored;
			if (Hours <= 0.f) { break; }
		}
	}
	float MovementFactor(bool Sprint) const
	{
		const float Sum = Health[4] + Health[5];
		return Limit(Sprint ? Settings.SprintSlope * Sum + Settings.SprintBase : Settings.RunSlope * Sum + Settings.RunBase, 0.f, 1.f);
	}
};

