#pragma once
#include "SoundTransmission.h"

#include "SoundMeta.h"
#include "../Sound.h"
#include "../../xrCore/Collision/xrCDB.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

// Private mixer component. Update and render calls are serialized by the
// mixer's render lock. Only BeginUpdate/UpdateEvent may access world geometry.
namespace XRay::Sound::GeometryReflections
{

constexpr u32 EventCapacity = 32;
constexpr u32 PathCapacity = 8;
constexpr u32 RingFrames = 16384;
constexpr u32 QueryLimit = 192; // Static AND dynamic queries, shared by all events.
constexpr float SpeedOfSound = 343.0f;
constexpr float MaxDelayFrames = float(RingFrames - SND_BLOCKSIZE - 2);
constexpr float SurfaceEpsilon = 0.01f;
constexpr u64 CacheLifetimeNs = 100000000ULL;
constexpr float Pi = 3.14159265358979323846f;

struct Diagnostics
{
	u32 UpdateQueries = 0;
	u32 StaticQueries = 0;
	u32 DynamicQueries = 0;
	u32 ActiveEvents = 0;
	u32 ActivePaths = 0;
	u32 TailEvents = 0;
	u32 SelectedEvents = 0;
	u32 DrainingEvents = 0;
	u64 PoolRejected = 0;
	u64 BudgetDeferred = 0;
	u64 SolidRejected = 0;
	u64 DoorRejected = 0;
	u64 OutsideTriangleRejected = 0;
	u64 ResultOverflowRejected = 0;
	u64 UnknownMaterialPaths = 0;
	u64 ApproximateMaterialPaths = 0;
	u64 MeasuredMaterialPaths = 0;
	u64 MissingDynamicRejected = 0;
	u64 ExcessDelayRejected = 0;
};

struct ImagePoint
{
	Fvector Position = {};
	Fvector Normal = {};
	float SourceLength = 0.0f;
	float ListenerLength = 0.0f;
};

// Kept independent of CDB queries so the actual image-source math can be
// exercised by a small deterministic harness, including finite-triangle edges.
inline bool PointInsideTriangle(const Fvector& Point, const Fvector* Vertices)
{
	Fvector A, B, P;
	A.sub(Vertices[1], Vertices[0]);
	B.sub(Vertices[2], Vertices[0]);
	P.sub(Point, Vertices[0]);
	const double AA = A.dotproduct(A);
	const double BB = B.dotproduct(B);
	const double AB = A.dotproduct(B);
	const double PA = P.dotproduct(A);
	const double PB = P.dotproduct(B);
	const double Denominator = AA * BB - AB * AB;
	if (Denominator <= 1.0e-12)
	{
		return false;
	}
	const double U = (BB * PA - AB * PB) / Denominator;
	const double V = (AA * PB - AB * PA) / Denominator;
	return U >= -0.00001 && V >= -0.00001 && U + V <= 1.00001;
}

inline bool FindImagePoint(const Fvector& Source, const Fvector& Listener,
	const Fvector* Vertices, ImagePoint& Out, bool RequireInside = true)
{
	Fvector A, B, FromPlane, ListenerFromPlane;
	A.sub(Vertices[1], Vertices[0]);
	B.sub(Vertices[2], Vertices[0]);
	Out.Normal.crossproduct(A, B);
	const float NormalLength = Out.Normal.magnitude();
	if (NormalLength < 0.000001f)
	{
		return false;
	}
	Out.Normal.div(NormalLength);
	FromPlane.sub(Source, Vertices[0]);
	ListenerFromPlane.sub(Listener, Vertices[0]);
	float SourceSide = FromPlane.dotproduct(Out.Normal);
	float ListenerSide = ListenerFromPlane.dotproduct(Out.Normal);
	// A first-order specular reflection requires both endpoints on one side.
	if (SourceSide * ListenerSide <= 0.0f ||
		std::abs(SourceSide) < SurfaceEpsilon || std::abs(ListenerSide) < SurfaceEpsilon)
	{
		return false;
	}
	if (SourceSide < 0.0f)
	{
		Out.Normal.mul(-1.0f);
		SourceSide = -SourceSide;
		ListenerSide = -ListenerSide;
	}
	Fvector Image, ToImage;
	Image.mad(Source, Out.Normal, -2.0f * SourceSide);
	ToImage.sub(Image, Listener);
	const float T = ListenerSide / (SourceSide + ListenerSide);
	Out.Position.mad(Listener, ToImage, T);
	if (RequireInside && !PointInsideTriangle(Out.Position, Vertices))
	{
		return false;
	}
	Out.SourceLength = Source.distance_to(Out.Position);
	Out.ListenerLength = Listener.distance_to(Out.Position);
	return std::isfinite(Out.SourceLength) && std::isfinite(Out.ListenerLength) &&
		Out.SourceLength > SurfaceEpsilon && Out.ListenerLength > SurfaceEpsilon;
}

class Renderer
{
public:
	Renderer() = default;
	Renderer(const Renderer&) = delete;
	Renderer& operator=(const Renderer&) = delete;

	// Game/update thread only, after taking the existing mixer render lock.
	void BeginUpdate(u64 NowNs)
	{
		CDB::MODEL* NewWorld = ::Sound ? ::Sound->get_geometry_occ() : nullptr;
		if (NewWorld != World)
		{
			Reset();
			World = NewWorld;
		}
		Now = NowNs;
		++UpdateSerial;
		Stats.UpdateQueries = Stats.StaticQueries = Stats.DynamicQueries = 0;
		Collider = ::Sound ? ::Sound->get_geometry_db() : nullptr;
	}

	// Publish the whole ranked admission set under one exclusive render section.
	// Deselection stops only NEW excitation. Existing rings, paths, gains and
	// filters remain untouched until their already excited samples drain.
	void SetRankedEvents(const u64* Keys, u32 Count)
	{
		SelectedCount = 0;
		for (u32 Index = 0; Keys != nullptr && Index < Count && SelectedCount < EventCapacity; ++Index)
		{
			if (Keys[Index] != 0 && !IsSelected(Keys[Index]))
			{
				SelectedKeys[SelectedCount++] = Keys[Index];
			}
		}
		for (auto& Entry : Events)
		{
			Entry.AcceptsExcitation = IsSelected(Entry.Key);
		}
		Stats.SelectedEvents = SelectedCount;
	}

	// Caller may share the unchanged global budget among ranked logical events.
	bool UpdateEvent(u64 Key, const Fvector& Source, const Fvector& Listener,
		const Fvector& ListenerRight, u32 QueryAllowance = 64)
	{
		if (Key == 0 || !World || !Collider || !IsSelected(Key))
		{
			return false;
		}
		QueryAllowance = std::min(QueryAllowance, 64u);
		EventQueryEnd = std::min(QueryLimit, Stats.UpdateQueries + QueryAllowance);
		Event* Entry = FindEvent(Key);
		if (!Entry)
		{
			const auto& Empty = EmptyCache[Key & 127u];
			if (Empty.Key == Key && Now - Empty.QueryTime < CacheLifetimeNs &&
				Empty.Source.distance_to_sqr(Source) < 0.0625f &&
				Empty.Listener.distance_to_sqr(Listener) < 0.0625f)
			{
				return false;
			}
			if (Stats.UpdateQueries + 6 > EventQueryEnd)
			{
				++Stats.BudgetDeferred;
				return false;
			}
			for (auto& Candidate : Events)
			{
				if (Candidate.Key == 0)
				{
					Entry = &Candidate;
					// Ring storage was cleared on release, never stolen from a tail.
					Entry->Key = Key;
					Entry->AcceptsExcitation = true;
					break;
				}
			}
			if (!Entry)
			{
				++Stats.PoolRejected;
				return false;
			}
		}
		if (Entry->UpdateFrame == UpdateSerial)
		{
			return true; // All layers of a shot use this single event publication.
		}
		Entry->UpdateFrame = UpdateSerial;
		UpdatePanning(*Entry, Listener, ListenerRight);
		const bool Moved = Entry->Source.distance_to_sqr(Source) > 0.0625f ||
			Entry->Listener.distance_to_sqr(Listener) > 0.0625f;
		if (Entry->GeometryValid && !Moved && Now - Entry->QueryTime < CacheLifetimeNs)
		{
			return true;
		}
		if (Stats.UpdateQueries + 6 > EventQueryEnd)
		{
			++Stats.BudgetDeferred;
			// Stale routes cannot remain open across an unchecked moving door.
			MuteTargets(*Entry);
			Entry->GeometryValid = false;
			return true;
		}
		Entry->Source = Source;
		Entry->Listener = Listener;
		Entry->QueryTime = Now;
		Entry->GeometryValid = true;
		BuildPaths(*Entry, ListenerRight);
		if (!Entry->HasRendered && GetPathCount(Key) == 0)
		{
			// Open-field/fully blocked events do not reserve a DSP tail slot.
			EmptyCache[Key & 127u] = {Key, Now, Source, Listener};
			*Entry = {};
			return false;
		}
		return true;
	}

	float GetReferenceEnergy(u64 Key) const
	{
		const Event* Entry = FindEvent(Key);
		float Energy = 0.0f;
		// Normalize the current source only against reflections it can excite.
		// A retiring event's residual tail belongs to earlier PCM contributions.
		if (Entry && Entry->AcceptsExcitation)
		{
			for (u32 Index = 0; Index < PathCapacity; ++Index)
			{
				const auto& Path = Entry->Paths[Index];
				const float Amplitude = std::max(std::max(Path.TargetLow, Path.TargetHigh),
					std::max(Path.Low, Path.High));
				Energy += Amplitude * Amplitude;
				for (u32 Other = 0; Other < Index; ++Other)
				{
					const auto& B = Entry->Paths[Other];
					const float Overlap = std::max(1.0f - std::abs(Path.TargetDelay - B.TargetDelay), 0.0f);
					const float OtherAmplitude = std::max(std::max(B.TargetLow, B.TargetHigh), std::max(B.Low, B.High));
					Energy += 2.0f * Amplitude * OtherAmplitude * Overlap *
						(Path.TargetLeft * B.TargetLeft + Path.TargetRight * B.TargetRight);
				}
			}
		}
		// Separate arrivals add energy; near-coincident taps include their stereo
		// cross-term. This conservative band estimate cannot predict correlations
		// in colored weapon PCM and is not a claim of measured +/- 1 dB loudness.
		return Energy;
	}

	float GetOutdoorStructure(u64 Key) const
	{
		const Event* Entry = FindEvent(Key);
		float Strength = 0.0f;
		if (Entry)
		{
			for (const auto& Path : Entry->Paths)
			{
				if (Path.Structure)
				{
					Strength += std::max(Path.TargetLow, Path.TargetHigh);
				}
			}
		}
		return std::min(Strength, 1.0f);
	}

	u32 GetPathCount(u64 Key) const
	{
		const Event* Entry = FindEvent(Key);
		u32 Count = 0;
		if (Entry)
		{
			for (const auto& Path : Entry->Paths)
			{
				Count += Path.TargetLow > 0.0f || Path.TargetHigh > 0.0f;
			}
		}
		return Count;
	}

	const Diagnostics& GetDiagnostics() const { return Stats; }

	// Audio thread: fixed storage, no geometry, allocations, logs or callbacks.
	void BeginBlock()
	{
		for (auto& Entry : Events)
		{
			if (Entry.Key == 0)
			{
				continue;
			}
			Entry.HasInput = false;
			for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
			{
				Entry.Ring[(Entry.WriteFrame + Frame) & (RingFrames - 1)] = 0.0f;
			}
		}
	}

	// Data must be the pre-HRTF/pre-obstruction PCM. The caller supplies source
	// bus gain, unoccluded distance gain and the same normalization as dry/late.
	// Eligible primary/distance layers sum once into their logical event ring.
	bool AddExcitation(u64 Key, float** Data, float BeginGain, float EndGain)
	{
		Event* Entry = FindEvent(Key);
		if (!Entry || !Entry->AcceptsExcitation || !Data)
		{
			return false;
		}
		float Peak = 0.0f;
		for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
		{
			const float T = float(Frame) / float(SND_BLOCKSIZE - 1);
			const float Gain = BeginGain + (EndGain - BeginGain) * T;
			const float Sample = 0.5f * (Data[0][Frame] + Data[1][Frame]) * Gain;
			Entry->Ring[(Entry->WriteFrame + Frame) & (RingFrames - 1)] += Sample;
			Peak = std::max(Peak, std::abs(Sample));
		}
		Entry->HasInput |= Peak > 0.000000001f;
		return true;
	}

	void Render(float** Output)
	{
		Stats.ActiveEvents = Stats.ActivePaths = Stats.TailEvents = Stats.DrainingEvents = 0;
		// A 900 Hz split is a declared transport approximation, not a measured
		// surface response. Both paths retain unity DC when LowGain is unity.
		constexpr float SplitAlpha = 0.12035005f; // 1-exp(-2*pi*900/44100).
		for (auto& Entry : Events)
		{
			if (Entry.Key == 0)
			{
				continue;
			}
			++Stats.ActiveEvents;
			Stats.TailEvents += !Entry.HasInput;
			Stats.DrainingEvents += !Entry.AcceptsExcitation;
			for (auto& Path : Entry.Paths)
			{
				if (Path.Triangle < 0)
				{
					continue;
				}
				if (Path.TargetLow == 0.0f && Path.TargetHigh == 0.0f &&
					Path.Low == 0.0f && Path.High == 0.0f)
				{
					Path = {};
					continue;
				}
				++Stats.ActivePaths;
				for (u32 Frame = 0; Frame < SND_BLOCKSIZE; ++Frame)
				{
					const float T = float(Frame) / float(SND_BLOCKSIZE - 1);
					// Crossfade old/new fractional delay taps without pitch jumps.
					const float OldSample = ReadDelay(Entry, Frame, Path.Delay);
					const float NewSample = ReadDelay(Entry, Frame, Path.TargetDelay);
					const float Input = OldSample + (NewSample - OldSample) * T;
					Path.LowPass += SplitAlpha * (Input - Path.LowPass);
					const float Low = Path.Low + (Path.TargetLow - Path.Low) * T;
					const float High = Path.High + (Path.TargetHigh - Path.High) * T;
					const float Sample = Path.LowPass * Low + (Input - Path.LowPass) * High;
					const float Left = Path.Left + (Path.TargetLeft - Path.Left) * T;
					const float Right = Path.Right + (Path.TargetRight - Path.Right) * T;
					Output[0][Frame] += Sample * Left;
					Output[1][Frame] += Sample * Right;
				}
				Path.Delay = Path.TargetDelay;
				Path.Low = Path.TargetLow;
				Path.High = Path.TargetHigh;
				Path.Left = Path.TargetLeft;
				Path.Right = Path.TargetRight;
				if (std::abs(Path.LowPass) < 1.0e-20f)
				{
					Path.LowPass = 0.0f;
				}
			}
			Entry.WriteFrame = (Entry.WriteFrame + SND_BLOCKSIZE) & (RingFrames - 1);
			Entry.HasRendered = true;
			Entry.SilentFrames = Entry.HasInput ? 0 : Entry.SilentFrames + SND_BLOCKSIZE;
			// A selected live loop keeps ownership during a silent gap. A
			// deselected owner retires only after the existing conservative ring
			// drain (about 0.49 s of rendered audio), without stealing its tail.
			if (Entry.SilentFrames > RingFrames + 4096 &&
				(!Entry.AcceptsExcitation || UpdateSerial - Entry.UpdateFrame > 2 || !Entry.GeometryValid))
			{
				Entry = {};
			}
		}
	}

	// Only world/session teardown or an explicit global effect disable. Never
	// call for an individual voice Stop/Destroy: rings own already excited tails.
	void Reset()
	{
		for (auto& Entry : Events)
		{
			Entry = {};
		}
		for (auto& Entry : EmptyCache)
		{
			Entry = {};
		}
		World = nullptr;
		Collider = nullptr;
		Now = UpdateSerial = 0;
		SelectedCount = 0;
		memset(SelectedKeys, 0, sizeof(SelectedKeys));
		Stats = {};
	}

private:
	struct PathState
	{
		s32 Triangle = -1;
		Fvector Point = {};
		Fvector Normal = {};
		float Delay = 0.0f;
		float TargetDelay = 0.0f;
		float Low = 0.0f;
		float High = 0.0f;
		float TargetLow = 0.0f;
		float TargetHigh = 0.0f;
		float Left = 0.70710678f;
		float Right = 0.70710678f;
		float TargetLeft = 0.70710678f;
		float TargetRight = 0.70710678f;
		float LowPass = 0.0f;
		bool Structure = false;
	};

	struct Event
	{
		u64 Key = 0;
		u64 UpdateFrame = 0;
		u64 QueryTime = 0;
		Fvector Source = {};
		Fvector Listener = {};
		PathState Paths[PathCapacity] = {};
		float Ring[RingFrames] = {};
		u32 WriteFrame = 0;
		u32 SilentFrames = 0;
		bool GeometryValid = false;
		bool HasInput = false;
		bool HasRendered = false;
		bool AcceptsExcitation = false;
	};

	struct EmptyEvent
	{
		u64 Key = 0;
		u64 QueryTime = 0;
		Fvector Source = {};
		Fvector Listener = {};
	};

	u64 SelectedKeys[EventCapacity] = {};
	u32 SelectedCount = 0;
	Event Events[EventCapacity] = {};
	EmptyEvent EmptyCache[128] = {};
	CDB::MODEL* World = nullptr;
	CDB::COLLIDER* Collider = nullptr;
	u64 Now = 0;
	u64 UpdateSerial = 0;
	u32 EventQueryEnd = QueryLimit;
	Diagnostics Stats = {};

	bool IsSelected(u64 Key) const
	{
		for (u32 Index = 0; Key != 0 && Index < SelectedCount; ++Index)
		{
			if (SelectedKeys[Index] == Key) return true;
		}
		return false;
	}

	Event* FindEvent(u64 Key)
	{
		if (Key != 0)
		{
			for (auto& Entry : Events)
			{
				if (Entry.Key == Key)
				{
					return &Entry;
				}
			}
		}
		return nullptr;
	}

	const Event* FindEvent(u64 Key) const
	{
		return const_cast<Renderer*>(this)->FindEvent(Key);
	}

	bool StaticQuery(const Fvector& From, const Fvector& Direction, float Range)
	{
		if (Stats.UpdateQueries >= EventQueryEnd)
		{
			return false;
		}
		++Stats.UpdateQueries;
		++Stats.StaticQueries;
		Collider->ray_options(0);
		Collider->ray_query(World, From, Direction, Range);
		if (Collider->r_count() > 64)
		{
			// Bounded result processing; an overflow cannot certify visibility.
			++Stats.ResultOverflowRejected;
			return false;
		}
		if (Collider->r_count() > 1)
		{
			std::sort(Collider->r_begin(), Collider->r_end(),
				[](const CDB::RESULT& A, const CDB::RESULT& B) { return A.range < B.range; });
		}
		return true;
	}

	SoundAcousticMaterial Material(u16 Id) const
	{
		return ::Sound->AcousticMaterialCallback.empty() ? SoundAcousticMaterial{} :
			::Sound->AcousticMaterialCallback(Id);
	}

	bool SegmentClear(const Fvector& From, const Fvector& To, float& Transmission, float& HighTransmission)
	{
		Fvector Direction;
		Direction.sub(To, From);
		const float Length = Direction.magnitude();
		if (Length <= 2.0f * SurfaceEpsilon)
		{
			return false;
		}
		Direction.div(Length);
		Fvector Start, End;
		Start.mad(From, Direction, SurfaceEpsilon);
		End.mad(To, Direction, -SurfaceEpsilon);
		if (!StaticQuery(Start, Direction, Length - 2.0f * SurfaceEpsilon))
		{
			return false;
		}
        SoundTransmissionAccumulator Accumulator;
        for (const CDB::RESULT* Hit = Collider->r_begin(); Hit != Collider->r_end(); ++Hit)
        {
            const auto Surface = Material((u16)Hit->material);
            if ((Surface.flags & 7u) == 0)
            {
                ++Stats.SolidRejected;
                return false;
            }
            Accumulator.Push(*Hit, Surface);
        }
        Accumulator.Peak();
        // The existing early renderer has two bands. Use LF and the RMS
        // pressure of the published middle/high bands without modifying rays.
        const float LowGain = Accumulator.Total[0];
        const float HighGain = std::sqrt(0.5f * (Accumulator.Total[1] * Accumulator.Total[1] +
            Accumulator.Total[2] * Accumulator.Total[2]));
        Transmission *= LowGain;
        HighTransmission *= LowGain > 0.0f ? HighGain / LowGain : 0.0f;
		if (::Sound->AcousticDynamicRayCallback.empty())
		{
			++Stats.MissingDynamicRejected;
			return false;
		}
		if (Stats.UpdateQueries >= EventQueryEnd)
		{
			return false;
		}
		++Stats.UpdateQueries;
		++Stats.DynamicQueries;
		if (::Sound->AcousticDynamicRayCallback(Start, End) > 0.01f)
		{
			++Stats.DoorRejected;
			return false;
		}
		return Transmission > 0.0001f;
	}

	static void MuteTargets(Event& Entry)
	{
		for (auto& Path : Entry.Paths)
		{
			Path.TargetLow = Path.TargetHigh = 0.0f;
		}
	}

	static void SetPan(PathState& Path, const Fvector& Listener, const Fvector& Right)
	{
		Fvector ToPoint;
		ToPoint.sub(Path.Point, Listener);
		const float Length = ToPoint.magnitude();
		const float Pan = Length > SurfaceEpsilon ?
			std::clamp(ToPoint.dotproduct(Right) / Length, -1.0f, 1.0f) : 0.0f;
		const float Angle = (Pan + 1.0f) * Pi * 0.25f;
		Path.TargetLeft = std::cos(Angle);
		Path.TargetRight = std::sin(Angle);
	}

	static void UpdatePanning(Event& Entry, const Fvector& Listener, const Fvector& Right)
	{
		for (auto& Path : Entry.Paths)
		{
			if (Path.Triangle >= 0)
			{
				SetPan(Path, Listener, Right);
			}
		}
	}

	bool ResolveSurface(CDB::RESULT& Face, ImagePoint& Point)
	{
		if (PointInsideTriangle(Point.Position, Face.verts))
		{
			return true;
		}
		// A probe can hit a different triangle of the same wall. Locate the
		// triangle at the exact image point; a hole in that wall remains a hole.
		Fvector Start, Direction;
		Start.mad(Point.Position, Point.Normal, 0.05f);
		Direction.set(Point.Normal).mul(-1.0f);
		if (!StaticQuery(Start, Direction, 0.1f))
		{
			return false;
		}
		for (const CDB::RESULT* Hit = Collider->r_begin(); Hit != Collider->r_end(); ++Hit)
		{
			if (std::abs(Hit->range - 0.05f) <= 0.002f &&
				PointInsideTriangle(Point.Position, Hit->verts))
			{
				Fvector A, B, Normal;
				A.sub(Hit->verts[1], Hit->verts[0]);
				B.sub(Hit->verts[2], Hit->verts[0]);
				Normal.crossproduct(A, B);
				Normal.normalize_safe();
				if (std::abs(Normal.dotproduct(Point.Normal)) > 0.999f)
				{
					Face = *Hit;
					return true;
				}
			}
		}
		++Stats.OutsideTriangleRejected;
		return false;
	}

	bool AddPath(Event& Entry, CDB::RESULT Face, const Fvector& Right)
	{
		ImagePoint Point;
		if (!FindImagePoint(Entry.Source, Entry.Listener, Face.verts, Point, false) ||
			!ResolveSurface(Face, Point))
		{
			return false;
		}
		const auto Surface = Material((u16)Face.material);
		if ((Surface.flags & 4u) != 0 || ((Surface.flags & 1u) != 0 && (Surface.flags & 2u) == 0))
		{
			return false;
		}
		for (const auto& Existing : Entry.Paths)
		{
			if ((Existing.TargetLow > 0.0f || Existing.TargetHigh > 0.0f) &&
				Existing.Point.distance_to_sqr(Point.Position) < 0.0025f)
			{
				return false;
			}
		}
		const float DirectLength = Entry.Source.distance_to(Entry.Listener);
		const float TotalLength = Point.SourceLength + Point.ListenerLength;
		const float Delay = std::max(TotalLength - DirectLength, 0.0f) * SND_SAMPLERATE / SpeedOfSound;
		if (Delay > MaxDelayFrames)
		{
			++Stats.ExcessDelayRejected;
			return false;
		}
		float Transmission = 1.0f;
		float HighTransmission = 1.0f;
		if (!SegmentClear(Entry.Source, Point.Position, Transmission, HighTransmission) ||
			!SegmentClear(Point.Position, Entry.Listener, Transmission, HighTransmission))
		{
			return false;
		}
		PathState* Destination = nullptr;
		for (auto& Existing : Entry.Paths)
		{
			Fvector PointDifference;
			PointDifference.sub(Point.Position, Existing.Point);
			const bool SameLocalPlane = Existing.Triangle >= 0 &&
				Existing.Point.distance_to_sqr(Point.Position) < 1.0f &&
				std::abs(Existing.Normal.dotproduct(Point.Normal)) > 0.999f &&
				std::abs(PointDifference.dotproduct(Point.Normal)) < 0.002f;
			if (Existing.Triangle == Face.id || SameLocalPlane)
			{
				Destination = &Existing;
				break;
			}
		}
		if (!Destination)
		{
			for (auto& Existing : Entry.Paths)
			{
				if (Existing.Triangle < 0 || (Existing.Low == 0.0f && Existing.High == 0.0f &&
					Existing.TargetLow == 0.0f && Existing.TargetHigh == 0.0f))
				{
					Destination = &Existing;
					*Destination = {};
					Destination->Delay = Delay;
					break;
				}
			}
		}
		if (!Destination)
		{
			return false; // Old paths fade out before their slots can be reused.
		}
		const bool CoefficientsValid = std::isfinite(Surface.reflection_low) && std::isfinite(Surface.reflection_high) &&
			Surface.reflection_low >= 0.0f && Surface.reflection_low <= 1.0f &&
			Surface.reflection_high >= 0.0f && Surface.reflection_high <= 1.0f;
		const bool Measured = Surface.reflection_measured && CoefficientsValid;
		const bool Approximate = !Measured && CoefficientsValid &&
			(Surface.Provenance == SoundAcousticProvenance::ResonanceApproximation ||
			 Surface.Provenance == SoundAcousticProvenance::DeclaredApproximation);
		// Unclassified or missing callback data uses Resonance's neutral uniform
		// 0.5 energy fallback, not an ideal rigid boundary and not a measurement.
		const float LowEnergy = Measured || Approximate ? Surface.reflection_low : 0.5f;
		const float HighEnergy = Measured || Approximate ? Surface.reflection_high : 0.5f;
		Stats.UnknownMaterialPaths += Surface.Class == SoundAcousticClass::UnknownSolid || (!Measured && !Approximate);
		Stats.ApproximateMaterialPaths += !Measured;
		Stats.MeasuredMaterialPaths += Measured;
		const float Spreading = std::min(std::max(DirectLength, 1.0f) / std::max(TotalLength, 1.0f), 1.0f);
		// Declared air-loss approximation (unknown humidity/temperature), applied
		// only to the reflected high band. It leaves reflected LF/DC untouched.
		const float AirHighGain = std::exp(-0.0025f * TotalLength);
		Destination->Triangle = Face.id;
		Destination->Point = Point.Position;
		Destination->Normal = Point.Normal;
		Destination->TargetDelay = Delay;
		Destination->TargetLow = Spreading * Transmission * std::sqrt(LowEnergy);
		Destination->TargetHigh = Spreading * Transmission * HighTransmission * std::sqrt(HighEnergy) * AirHighGain;
		// A conservative geometric structure indicator excludes all near-level
		// surfaces. It cannot semantically distinguish a wall from a steep rock.
		Destination->Structure = std::abs(Point.Normal.y) < 0.65f;
		SetPan(*Destination, Entry.Listener, Right);
		if (!Entry.HasRendered)
		{
			// The first impulse must arrive at its computed time at full gain;
			// fading from zero here would erase short ground/near-wall returns.
			Destination->Low = Destination->TargetLow;
			Destination->High = Destination->TargetHigh;
			Destination->Left = Destination->TargetLeft;
			Destination->Right = Destination->TargetRight;
		}
		return true;
	}

	void BuildPaths(Event& Entry, const Fvector& Right)
	{
		// Revalidate the strongest previously audible paths first. Sort indices,
		// never the DSP taps/rings, so existing delay/filter/tail identity survives.
		u32 Previous[PathCapacity];
		for (u32 Index = 0; Index < PathCapacity; ++Index)
		{
			Previous[Index] = Index;
		}
		const auto Contribution = [&Entry](u32 Index)
		{
			const auto& Path = Entry.Paths[Index];
			return std::max(Path.Low * Path.Low + Path.High * Path.High,
				Path.TargetLow * Path.TargetLow + Path.TargetHigh * Path.TargetHigh);
		};
		std::sort(std::begin(Previous), std::end(Previous), [&Contribution](u32 A, u32 B)
		{
			const float Left = Contribution(A), Right = Contribution(B);
			return Left != Right ? Left > Right : A < B;
		});
		MuteTargets(Entry);
		u32 Found = 0;
		for (u32 Index = 0; Index < PathCapacity && Stats.UpdateQueries + 5 <= EventQueryEnd; ++Index)
		{
			const s32 Id = Entry.Paths[Previous[Index]].Triangle;
			if (Id < 0 || (size_t)Id >= World->get_tris().size())
			{
				continue;
			}
			const auto& Triangle = World->get_tris()[Id];
			CDB::RESULT Face = {};
			Face.id = Id;
			Face.dummy = Triangle.dummy;
			for (u32 Vertex = 0; Vertex < 3; ++Vertex)
			{
				Face.verts[Vertex] = World->get_verts()[Triangle.verts[Vertex]];
			}
			Found += AddPath(Entry, Face, Right);
		}
		// Sampling finds candidate planes only; image points and both legs must
		// still validate. No spherical/listener-only delays are ever rendered.
		static constexpr Fvector Directions[] = {
			{0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
			{1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
			{1, -1, 0}, {-1, -1, 0}, {0, -1, 1}, {0, -1, -1},
			{1, 1, 0}, {-1, 1, 0}, {0, 1, 1}, {0, 1, -1}};
		Fvector Midpoint;
		Midpoint.add(Entry.Source, Entry.Listener).mul(0.5f);
		for (u32 Index = 0; Index < (u32)std::size(Directions) && Found < PathCapacity &&
			Stats.UpdateQueries + 6 <= EventQueryEnd; ++Index)
		{
			Fvector Direction = Directions[Index];
			Direction.normalize_safe();
			const Fvector& Origin = Index < 6 ? Midpoint : (Index & 1 ? Entry.Source : Entry.Listener);
			if (!StaticQuery(Origin, Direction, 64.0f))
			{
				continue;
			}
			CDB::RESULT Face = {};
			bool HasSurface = false;
			for (const CDB::RESULT* Hit = Collider->r_begin(); Hit != Collider->r_end(); ++Hit)
			{
				const auto Surface = Material((u16)Hit->material);
				if ((Surface.flags & 4u) == 0 && ((Surface.flags & 1u) == 0 || (Surface.flags & 2u) != 0))
				{
					Face = *Hit; // Copy before another query replaces collider results.
					HasSurface = true;
					break;
				}
			}
			if (HasSurface)
			{
				Found += AddPath(Entry, Face, Right);
			}
		}
	}

	static float ReadDelay(const Event& Entry, u32 Frame, float Delay)
	{
		const u32 Whole = (u32)Delay;
		const float Fraction = Delay - float(Whole);
		const u32 First = (Entry.WriteFrame + Frame + RingFrames - Whole) & (RingFrames - 1);
		const u32 Second = (First + RingFrames - 1) & (RingFrames - 1);
		return Entry.Ring[First] + (Entry.Ring[Second] - Entry.Ring[First]) * Fraction;
	}
};

static_assert((RingFrames & (RingFrames - 1)) == 0, "Reflection ring must be a power of two.");
static_assert(SND_CHANNEL_COUNT == 2, "Native reflection panning currently targets stereo output.");

} // namespace XRay::Sound::GeometryReflections
