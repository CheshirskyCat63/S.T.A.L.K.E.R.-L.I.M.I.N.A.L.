//---------------------------------------------------------------------------
#include "stdafx.h"
#include "GameMtlLib.h"
#include <cctype>
#include <cmath>
#include "../xrSound/SoundAcousticPresets.h"
CGameMtlLibrary GMLib;

namespace SoundAcousticsMaterial
{
SoundAcousticMaterial Build(const SGameMtl& Material, const char*& Model)
{
    SoundAcousticMaterial Acoustic;
    xr_string Name = Material.m_Name.c_str() ? Material.m_Name.c_str() : "";
    std::transform(Name.begin(), Name.end(), Name.begin(),
        [](unsigned char Character) { return (char)std::tolower(Character); });
    const auto Has = [&Name](const char* Token) { return Name.find(Token) != xr_string::npos; };
    const bool Liquid = Material.Flags.is(SGameMtl::flLiquid) || Has("water");
    const bool Open = Material.Flags.is(SGameMtl::flPassable) || Material.Flags.is(SGameMtl::flLiquid);
    const bool NamedGlass = Has("glass") || Has("window") || Has("vitreous");
    const bool Transparent = !Open && (NamedGlass ||
        (Material.Flags.is(SGameMtl::flTransparent) &&
         Material.fVisTransparencyFactor > 0.56f && Material.fSndOcclusionFactor > 0.19f));
    Acoustic.flags = (Open ? 1u : 0u) | (Transparent ? 2u : 0u);
    Acoustic.transmission = std::clamp(Material.fSndOcclusionFactor, 0.0f, 1.0f);
    if (Open) Acoustic.transmission = std::max(Acoustic.transmission, 0.88f);

    const auto& Selected = Transparent ? SoundAcousticPresets::Glass : SoundAcousticPresets::Select(Name.c_str());
    // Both enums identify the same public surface families; coefficients are
    // selected once in the shared table also consumed by the offline exporter.
    Acoustic.Class = static_cast<SoundAcousticClass>(Selected.Class);
    if (Liquid) Acoustic.Class = SoundAcousticClass::Liquid;
    else if (Has("bush") || Has("leaves") || Has("foliage") || (Open && Has("grass"))) Acoustic.Class = SoundAcousticClass::Foliage;
    else if (Open || Has("fake") || Has("setka") || Has("wire_mesh")) Acoustic.Class = SoundAcousticClass::NonStructural;

    if (Acoustic.Class == SoundAcousticClass::Foliage ||
        Acoustic.Class == SoundAcousticClass::NonStructural || Acoustic.Class == SoundAcousticClass::Liquid)
    {
        Acoustic.flags |= 4u;
        Acoustic.Provenance = SoundAcousticProvenance::DeclaredApproximation;
        Acoustic.reflection_low = Acoustic.reflection_high = 0.0f;
        if (Acoustic.Class == SoundAcousticClass::Foliage)
            Acoustic.HighTransmissionRatio = std::sqrt(Acoustic.transmission);
        Model = Acoustic.Class == SoundAcousticClass::Foliage ? "foliage_crossing_proxy" : "nonstructural_no_specular";
        return Acoustic;
    }

    const float LegacyPressure = Acoustic.transmission;
    const float LowEnergy = SoundAcousticPresets::TransmissionEnergy(Selected, 0, LegacyPressure);
    const float MidEnergy = SoundAcousticPresets::TransmissionEnergy(Selected, 1, LegacyPressure);
    const float HighEnergy = SoundAcousticPresets::TransmissionEnergy(Selected, 2, LegacyPressure);
    const float MaximumEnergy = std::max({LowEnergy, MidEnergy, HighEnergy});
    Acoustic.transmission = std::sqrt(MaximumEnergy);
    Acoustic.LowTransmissionRatio = MaximumEnergy > 0.0f ? std::sqrt(LowEnergy / MaximumEnergy) : 0.0f;
    Acoustic.MidTransmissionRatio = MaximumEnergy > 0.0f ? std::sqrt(MidEnergy / MaximumEnergy) : 0.0f;
    Acoustic.HighTransmissionRatio = MaximumEnergy > 0.0f ? std::sqrt(HighEnergy / MaximumEnergy) : 0.0f;
    Acoustic.FrequencyTransmission = Selected.TransmissionEnergy[0] >= 0.0f;
    Acoustic.absorption = 0.0f;
    float LowReflection = 0.0f, HighReflection = 0.0f;
    for (u32 Band = 0; Band < 9; ++Band)
    {
        Acoustic.AbsorptionBands[Band] = Selected.Absorption[Band];
        Acoustic.BoundaryLossBands[Band] = SoundAcousticPresets::BoundaryLoss(Selected, Band, LegacyPressure);
        Acoustic.absorption += Selected.Absorption[Band] / 9.0f;
        if (Band >= 2 && Band <= 4) LowReflection += (1.0f - Acoustic.BoundaryLossBands[Band]) / 3.0f;
        if (Band >= 5 && Band <= 7) HighReflection += (1.0f - Acoustic.BoundaryLossBands[Band]) / 3.0f;
    }
    Acoustic.reflection_low = LowReflection;
    Acoustic.reflection_high = HighReflection;
    Acoustic.Provenance = SoundAcousticProvenance::ResonanceApproximation;
    Model = Selected.Name;
    return Acoustic;
}
}

CGameMtlLibrary::CGameMtlLibrary()
{
    material_index = 0;
    material_pair_index = 0;
    material_count = 0;
    PGMLib = &GMLib;
}

SGameMtl* CGameMtlLibrary::GetMaterialByIdx(u16 idx)
{
    if (materials.empty()) return nullptr;
    if (idx >= materials.size())
    {
        if (idx != 65535)
            Msg("Material [%d] not found in library! ", (int)idx);
        return materials[0];
    }

    return materials[idx];
}

EGameMtlVersion SGameMtl::Load(IReader& fs)
{
    EGameMtlVersion vers{ GAMEMTL_VERSION_SOC };

	R_ASSERT(fs.find_chunk(GAMEMTL_CHUNK_MAIN));
	ID						= fs.r_u32();
    fs.r_stringZ			(m_Name);

    if (fs.find_chunk(GAMEMTL_CHUNK_DESC)){
		fs.r_stringZ		(m_Desc);
    }
    
	R_ASSERT(fs.find_chunk(GAMEMTL_CHUNK_FLAGS));
    Flags.assign			(fs.r_u32());

	R_ASSERT(fs.find_chunk(GAMEMTL_CHUNK_PHYSICS));
    fPHFriction				= fs.r_float();
    fPHDamping				= fs.r_float();
    fPHSpring				= fs.r_float();
    fPHBounceStartVelocity 	= fs.r_float();
    fPHBouncing				= fs.r_float();

	R_ASSERT(fs.find_chunk(GAMEMTL_CHUNK_FACTORS));
    fShootFactor			= fs.r_float();
    fBounceDamageFactor		= fs.r_float();
    fVisTransparencyFactor	= fs.r_float();
    fSndOcclusionFactor		= fs.r_float();

    if (fs.find_chunk(GAMEMTL_CHUNK_FLOTATION))
    {
        fFlotationFactor = fs.r_float();
    }

    if (fs.find_chunk(GAMEMTL_CHUNK_INJURIOUS))
    {
        fInjuriousSpeed = fs.r_float();
    }
    
    if (fs.find_chunk(GAMEMTL_CHUNK_DENSITY))
    {
        fDensityFactor = fs.r_float();
        vers = GAMEMTL_VERSION_CS;
    }
    else // St4lker0k765: for some reason, in SoC shoot factor is reversed
    {
        fShootFactor = 1.0f - fShootFactor;
    }

    if (fs.find_chunk(GAMEMTL_CHUNK_FACTORS_MP))
    {
        fShootFactorMP = fs.r_float();
        vers = GAMEMTL_VERSION_COP;
    }
    else
    {
        fShootFactorMP = fShootFactor;
    }

    return vers;
}

void CGameMtlLibrary::Load()
{
    AcousticMaterials.clear();
	string_path			name;
	if (!FS.exist(name,	_game_data_,GAMEMTL_FILENAME)){
        Msg("! Can't find game material file: %s",name);
    	return;
    }

    R_ASSERT			(material_pairs.empty());
    R_ASSERT			(materials.empty());

	IReader*	F		= FS.r_open(name);
    IReader& fs			= *F;

    R_ASSERT(fs.find_chunk(GAMEMTLS_CHUNK_VERSION));
    const auto file_version = static_cast<EGameMtlVersion>(fs.r_u16());

    if (file_version != GAMEMTL_VERSION_COP)
    {
        Msg("CGameMtlLibrary: unsupported version [%u]. Library can't load.", file_version);
        FS.r_close(F);
        return;
    }

    R_ASSERT(fs.find_chunk(GAMEMTLS_CHUNK_AUTOINC));
    material_index		= fs.r_u32();
    material_pair_index	= fs.r_u32();

    materials.clear		();
    material_pairs.clear();
    auto detected_version{ GAMEMTL_VERSION_SOC };

    IReader* OBJ 		= fs.open_chunk(GAMEMTLS_CHUNK_MTLS);
    if (OBJ) {
        u32				count;
        for (IReader* O = OBJ->open_chunk_iterator(count); O; O = OBJ->open_chunk_iterator(count,O)) {
        	SGameMtl*	M = new SGameMtl ();
            const auto version = M->Load(*O);
            detected_version = std::max(detected_version, version);
            materials.push_back(M);
        }
        OBJ->close		();
    }
    m_version = detected_version;

    OBJ 				= fs.open_chunk(GAMEMTLS_CHUNK_MTLS_PAIR);
    if (OBJ){
        u32				count;
        for (IReader* O = OBJ->open_chunk_iterator(count); O; O = OBJ->open_chunk_iterator(count,O)) {
        	SGameMtlPair* M	= new SGameMtlPair (this);
	        M->Load		(*O);
        	material_pairs.push_back(M);
        }
        OBJ->close		();
    }

	material_count		= (u32)materials.size();
    material_pairs_rt.resize(material_count*material_count,0);
    for (GameMtlPairIt p_it=material_pairs.begin(); material_pairs.end() != p_it; ++p_it){
		SGameMtlPair* S	= *p_it;
    	int idx0		= GetMaterialIdx(S->mtl0)*material_count+GetMaterialIdx(S->mtl1);
    	int idx1		= GetMaterialIdx(S->mtl1)*material_count+GetMaterialIdx(S->mtl0);
	    material_pairs_rt[idx0]=S;
	    material_pairs_rt[idx1]=S;
    }

	FS.r_close		(F);

	::Sound->OcclusionMaterialCallback = xr_delegate<float(u16)>(+[](u16 MtlID)->float
	{
		static bool UseMaterialOCC = EngineExternal()[EEngineExternalSound::MaterialOCC];

		if (UseMaterialOCC)
		{
			const SGameMtl* MtlIter = GMLib.GetMaterialByIdx((int)MtlID);
			return MtlIter != nullptr ? MtlIter->fSndOcclusionFactor : psSoundOcclusionScale;
		}

		return psSoundOcclusionScale;
	});

    // Publish all generic material estimates during OnAppStart, before level
    // collision/audio updates. OnAppEnd Unload clears the cache after gameplay
    // teardown. The hot ray callback performs no allocation, classification,
    // transcendental calculations or logging.
    AcousticMaterials.reserve(materials.size());
    for (u32 Index = 0; Index < materials.size(); ++Index)
    {
        const SGameMtl* Material = materials[Index];
        const char* Model = "unknown";
        const SoundAcousticMaterial Acoustic = SoundAcousticsMaterial::Build(*Material, Model);
        AcousticMaterials.push_back(Acoustic);
        Msg("* [LIMINAL_ACOUSTIC_MATERIAL] id=%u name=%s snd=%.2f vis=%.2f flags=%u glass=%u class=%u provenance=%u model=%s absorption=%.4f reflection_low=%.4f reflection_high=%.4f lf_ratio=%.4f mid_ratio=%.4f hf_ratio=%.4f transmission_peak_pressure=%.5f measured=0 transmission=published_energy_sqrt_or_legacy_pressure cached=1",
            Index, Material->m_Name.c_str(), Material->fSndOcclusionFactor,
            Material->fVisTransparencyFactor, (u32)Material->Flags.get(),
            (Acoustic.flags & 2u) != 0 ? 1u : 0u, (u32)Acoustic.Class,
            (u32)Acoustic.Provenance, Model, Acoustic.absorption,
            Acoustic.reflection_low, Acoustic.reflection_high, Acoustic.LowTransmissionRatio,
            Acoustic.MidTransmissionRatio, Acoustic.HighTransmissionRatio, Acoustic.transmission);
    }
    ::Sound->AcousticMaterialCallback =
        xr_delegate<SoundAcousticMaterial(u16)>(+[](u16 MaterialIndex)->SoundAcousticMaterial
    {
        return GMLib.GetAcousticMaterialByIdx(MaterialIndex);
    });
}

#ifdef DEBUG
const char* SGameMtlPair::dbg_Name()
{
	static string256 nm;
	SGameMtl* M0 = GMLib.GetMaterialByID(GetMtl0());
	SGameMtl* M1 = GMLib.GetMaterialByID(GetMtl1());
	xr_sprintf(nm,sizeof(nm),"Pair: %s - %s",*M0->m_Name,*M1->m_Name);
	return nm;
}
#endif
