#pragma once

#include "../Sound.h"
#include "../../xrCore/Collision/xrCDB.h"
#include <algorithm>
#include <cmath>

// Each distinct physical CFORM crossing is counted. Coincident coplanar
// triangles describe one boundary, even when duplicate triangles disagree on
// material. No wall thickness or solid-mesh pairing is inferred from distance.
struct SoundTransmissionAccumulator
{
	float Total[3] = {1.0f, 1.0f, 1.0f};
	float Group[3] = {1.0f, 1.0f, 1.0f};
	Fvector Normal = {};
	Fvector PlanePoint = {};
	float Range = -1.0f;
	u32 Crossings = 0;

	void Flush()
	{
		if (Range < 0.0f) return;
		for (u32 Band = 0; Band < 3; ++Band) Total[Band] *= Group[Band];
		++Crossings;
		Range = -1.0f;
	}

	void Push(const CDB::RESULT& Face, const SoundAcousticMaterial& Material)
	{
		Fvector EdgeA, EdgeB, FaceNormal, Delta;
		EdgeA.sub(Face.verts[1], Face.verts[0]);
		EdgeB.sub(Face.verts[2], Face.verts[0]);
		FaceNormal.crossproduct(EdgeA, EdgeB);
		FaceNormal.normalize_safe();
		Delta.sub(Face.verts[0], PlanePoint);
		const bool Coincident = Range >= 0.0f && std::abs(Face.range - Range) <= 0.005f &&
			std::abs(FaceNormal.dotproduct(Normal)) >= 0.99999f &&
			std::abs(Delta.dotproduct(Normal)) <= 0.0001f;
		if (!Coincident)
		{
			Flush();
			Range = Face.range;
			Normal = FaceNormal;
			PlanePoint = Face.verts[0];
			for (float& Value : Group) Value = 1.0f;
		}
		const float Ratios[3] = {Material.LowTransmissionRatio, Material.MidTransmissionRatio, Material.HighTransmissionRatio};
		for (u32 Band = 0; Band < 3; ++Band)
		{
			// Pick the strongest loss within the same boundary; triangle ordering
			// at a shared edge cannot increase transmission through that wall.
			Group[Band] = std::min(Group[Band], std::clamp(Material.transmission * Ratios[Band], 0.0f, 1.0f));
		}
	}

	float Peak()
	{
		Flush();
		return std::max({Total[0], Total[1], Total[2]});
	}
};
