#pragma once

#include "SoundBus.h"
#include <algorithm>
#include <cmath>

// Finite catalog support shared by source paths and existing room-field returns.
// This geometric angular-coverage approximation is not a wave diffraction solver.
namespace SoundOpeningGeometry
{
constexpr float Pi = 3.14159265358979323846f;

inline bool IsFinite(const Fvector& Value)
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

inline bool IsValidOpening(const SoundRoomPortal& Opening)
{
	const float RectangleArea = 4.0f * Opening.HalfExtentU * Opening.HalfExtentV;
	return Opening.Id != 0 && (Opening.Flags & SoundRoomPortalOpen) != 0 &&
		IsFinite(Opening.Center) && IsFinite(Opening.Normal) && IsFinite(Opening.TangentU) &&
		std::isfinite(Opening.Area) && std::isfinite(RectangleArea) && Opening.Area > 0.0f &&
		Opening.HalfExtentU > 0.0f && Opening.HalfExtentV > 0.0f &&
		Opening.Area <= RectangleArea * 1.0001f &&
		std::abs(Opening.Normal.square_magnitude() - 1.0f) <= 0.001f &&
		std::abs(Opening.TangentU.square_magnitude() - 1.0f) <= 0.001f &&
		std::abs(Opening.Normal.dotproduct(Opening.TangentU)) <= 0.001f;
}

inline bool HasOppositeSides(const SoundRoomPortal& Opening, const Fvector& Source, const Fvector& Listener)
{
	if (!IsValidOpening(Opening) || !IsFinite(Source) || !IsFinite(Listener))
	{
		return false;
	}
	Fvector SourceDelta, ListenerDelta;
	SourceDelta.sub(Source, Opening.Center);
	ListenerDelta.sub(Listener, Opening.Center);
	const float SourceSide = SourceDelta.dotproduct(Opening.Normal);
	const float ListenerSide = ListenerDelta.dotproduct(Opening.Normal);
	return SourceSide * ListenerSide <= 0.0f && std::abs(SourceSide) + std::abs(ListenerSide) > 0.00001f;
}

inline bool MatchesEndpointSides(const SoundRoomPortal& Opening, const Fvector& Source, const Fvector& Listener,
	u32 SourceRoom, u32 ListenerRoom)
{
	if (!HasOppositeSides(Opening, Source, Listener))
	{
		return false;
	}
	if (SourceRoom == ListenerRoom)
	{
		// Coarse room membership may contain an internal partition. The finite
		// incident support and the traced legs still determine actual visibility.
		return true;
	}
	Fvector SourceDelta, ListenerDelta;
	SourceDelta.sub(Source, Opening.Center);
	ListenerDelta.sub(Listener, Opening.Center);
	const float SourceSide = SourceDelta.dotproduct(Opening.Normal);
	const float ListenerSide = ListenerDelta.dotproduct(Opening.Normal);
	const auto MatchesLambda = [&Opening](u32 Room, float Side)
	{
		return (Room == Opening.RoomA && Side <= 0.00001f) ||
			(Room == Opening.RoomB && Side >= -0.00001f) ||
			(Opening.RoomA == u32(-1) && Side <= 0.00001f) ||
			(Opening.RoomB == u32(-1) && Side >= -0.00001f);
	};
	return MatchesLambda(SourceRoom, SourceSide) && MatchesLambda(ListenerRoom, ListenerSide);
}

// The two spatial return sources must use exactly these checked support points.
inline Fvector SupportPoint(const SoundRoomPortal& Opening, u32 Index)
{
	Fvector Point = Opening.Center;
	if (Index != 0)
	{
		Point.mad(Opening.TangentU, Opening.HalfExtentU * (Index == 1 ? -0.5f : 0.5f));
	}
	return Point;
}

inline float FieldCoupling(const SoundRoomPortal& Opening, const Fvector& Position)
{
	if (!IsValidOpening(Opening) || !IsFinite(Position))
	{
		return 0.0f;
	}
	Fvector Delta, TangentV;
	Delta.sub(Position, Opening.Center);
	TangentV.crossproduct(Opening.Normal, Opening.TangentU);
	const double Distance = std::abs(Delta.dotproduct(Opening.Normal));
	const double U = Delta.dotproduct(Opening.TangentU), V = Delta.dotproduct(TangentV);
	const double U0 = -Opening.HalfExtentU - U, U1 = Opening.HalfExtentU - U;
	const double V0 = -Opening.HalfExtentV - V, V1 = Opening.HalfExtentV - V;
	const auto CornerAngleLambda = [Distance](double X, double Y)
	{
		return std::atan2(X * Y, Distance * std::sqrt(X * X + Y * Y + Distance * Distance));
	};
	const double SolidAngle = std::abs(CornerAngleLambda(U1, V1) - CornerAngleLambda(U0, V1) -
		CornerAngleLambda(U1, V0) + CornerAngleLambda(U0, V0));
	const double AreaFraction = Opening.Area / (4.0 * Opening.HalfExtentU * Opening.HalfExtentV);
	// The clamp only bounds roundoff at the physical hemispherical maximum.
	return float(std::sqrt(std::clamp(SolidAngle * AreaFraction / (2.0 * Pi), 0.0, 1.0)));
}

inline float DirectCoupling(const SoundRoomPortal& Opening, const Fvector& Source, const Fvector& Listener)
{
	if (!HasOppositeSides(Opening, Source, Listener))
	{
		return 0.0f;
	}
	const float DirectLength = Source.distance_to(Listener);
	const float PathLength = Source.distance_to(Opening.Center) + Listener.distance_to(Opening.Center);
	if (DirectLength <= 0.00001f || PathLength <= 0.00001f)
	{
		return 0.0f;
	}
	// Symmetric endpoint angular support keeps this pressure approximation
	// reciprocal. The length ratio accounts only for the extra bent path.
	return std::sqrt(FieldCoupling(Opening, Source) * FieldCoupling(Opening, Listener)) *
		DirectLength / std::max(PathLength, DirectLength);
}
}
