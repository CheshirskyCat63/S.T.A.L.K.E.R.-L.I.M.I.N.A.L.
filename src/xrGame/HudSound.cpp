#include "StdAfx.h"
#include "HudSound.h"
#include "Actor.h"
#include "../xrSound/ai_sounds.h"
#include "../xrSound/New/SoundMixer.h"

float HUD_SOUND_ITEM::g_fHudSndFrequency = 1.0f;
float HUD_SOUND_ITEM::g_fHudSndVolumeFactor = 1.0f;
float psHUDSoundVolume = 1.0f;
float psHUDStepSoundVolume = 1.0f;

void InitHudSoundSettings()
{
	psHUDSoundVolume = READ_IF_EXISTS(pSettings, r_float, "hud_sound", "hud_sound_vol_k", 1.0f);
	psHUDStepSoundVolume = READ_IF_EXISTS(pSettings, r_float, "hud_sound", "hud_step_sound_vol_k", 1.0f);
}

void HUD_SOUND_ITEM::LoadSound(const char* section, const char* line, HUD_SOUND_ITEM& hud_snd, int type, esound_type sound_type)
{
	string256 sound_line;

	string64 type_str;
	xr_sprintf(type_str, "%d|%u\n", type, (u32)sound_type);
	xr_string source_key = type_str;

	xr_strcpy(sound_line, line);
	for (int i = 0; pSettings->line_exist(section, sound_line);)
	{
		source_key += pSettings->r_string(section, sound_line);
		source_key += '\n';
		xr_sprintf(sound_line, "%s%d", line, ++i);
	}

	if (!hud_snd.sounds.empty() && hud_snd.m_source_key == source_key)
	{
		return;
	}

	hud_snd.m_source_key = source_key;
	hud_snd.m_activeSnd = nullptr;
	hud_snd.sounds.clear();

	xr_strcpy(sound_line, line);
	int k = 0;

	while (pSettings->line_exist(section, sound_line))
	{
		hud_snd.sounds.push_back(SSnd());
		SSnd& s = hud_snd.sounds.back();

		LoadSound(section, sound_line, s.snd, type, &s.volume, &s.delay, sound_type);
		xr_sprintf(sound_line, "%s%d", line, ++k);
	}//while
}

void HUD_SOUND_ITEM::LoadSound(const char* section, const char* line, ref_sound& snd, int type, float* volume, float* delay, esound_type sound_type)
{
	const char* str = pSettings->r_string(section, line);
	string256 buf_str;

	int	count = _GetItemCount(str);
	R_ASSERT2(count, make_string<const char*>(
		"In section [%s], line [%s] does not contain sound", section, line));

	_GetItem(str, 0, buf_str);
	// Preserve gunshot semantics but distinguish suppressed sound propagation.
	if (strstr(line, "silencer") || strstr(line, "silncer")) type |= SOUND_TYPE_SUPPRESSED;
	snd.create(buf_str, sound_type, type);

	if (volume != nullptr)
	{
		*volume = 1.f;
		if (count > 1)
		{
			_GetItem(str, 1, buf_str);
			if (xr_strlen(buf_str) > 0)
			{
				*volume = (float)atof(buf_str);
			}
		}
	}

	if (delay != nullptr)
	{
		*delay = 0;
		if (count > 2)
		{
			_GetItem(str, 2, buf_str);
			if (xr_strlen(buf_str) > 0)
			{
				*delay = (float)atof(buf_str);
			}
		}
	}
}

void HUD_SOUND_ITEM::DestroySound(HUD_SOUND_ITEM& hud_snd)
{
	for (SSnd& it : hud_snd.sounds)
	{
		it.snd.destroy();
	}

	hud_snd.sounds.clear();
	hud_snd.m_activeSnd = nullptr;
}

static bool IsPhysicalHudSound(ref_sound& Sound)
{
	if (Sound._sound_type() != st_Effect)
	{
		return false;
	}

	// HUD collections also carry UI and subjective device signals. Only
	// explicit physical AI types opt in; sg_SourceType is never inferred.
	switch ((u32)Sound._g_type())
	{
	case SOUND_TYPE_WEAPON:
	case SOUND_TYPE_WEAPON_EMPTY_CLICKING:
	case SOUND_TYPE_WEAPON_RECHARGING:
	case SOUND_TYPE_WEAPON_SHOOTING: // Shotgun reload stages retain this legacy AI type.
	case SOUND_TYPE_ITEM_PICKING_UP:
	case SOUND_TYPE_ITEM_DROPPING:
	case SOUND_TYPE_ITEM_TAKING:
	case SOUND_TYPE_ITEM_HIDING:
		return true;
	default:
		return false;
	}
}

void HUD_SOUND_ITEM::PlaySound(HUD_SOUND_ITEM& hud_snd, const Fvector& position, const CObject* parent, bool b_hud_mode, bool looped, bool allowOverlap, u8 index)
{
	if (hud_snd.sounds.empty())
	{
		return;
	}

    if (hud_snd.m_b_exclusive)
	{
		StopSound(hud_snd);
	}

	u32 flags = b_hud_mode ? sm_2D : 0;

	if (looped)
	{
		flags |= sm_Looped;
	}

	u32 sounds_pool_size = (u32)hud_snd.sounds.size();
	if (index != u8(-1) && index < sounds_pool_size)
	{
		hud_snd.m_activeSnd = &hud_snd.sounds[index];
	}
	else
	{
		hud_snd.m_activeSnd = &hud_snd.sounds[Random.randI(sounds_pool_size)];
	}

	if (b_hud_mode && IsPhysicalHudSound(hud_snd.m_activeSnd->snd))
	{
		flags |= sm_Physical2D;
	}

	float ai_vol = hud_snd.m_activeSnd->volume;
	float vol = hud_snd.m_activeSnd->volume * (b_hud_mode ? psHUDSoundVolume : 1.0f) * g_fHudSndVolumeFactor;

    // A HUD shot keeps its muzzle coordinates for environment membership.
    // sm_2D still preserves its authored panning and local playback gain.
    const bool KeepMuzzle = hud_snd.m_activeSnd->snd._sound_type() == st_Shooting;
    Fvector pos = (flags & sm_2D) && !KeepMuzzle ? zero_vel : position;

    if (!allowOverlap)
    {
		hud_snd.m_activeSnd->snd.play_at_pos(const_cast<CObject*>(parent), pos, flags, hud_snd.m_activeSnd->delay);
        hud_snd.m_activeSnd->snd.set_volume(ai_vol);
        hud_snd.m_activeSnd->snd.set_frequency(g_fHudSndFrequency);
    }
	else
    {
		hud_snd.m_activeSnd->snd.play_no_feedback(const_cast<CObject*>(parent), flags, hud_snd.m_activeSnd->delay, &pos, &vol, &g_fHudSndFrequency);
    }
}

void HUD_SOUND_ITEM::StopSound(HUD_SOUND_ITEM& hud_snd)
{
	for (SSnd& it : hud_snd.sounds)
	{
		it.snd.stop();
	}

	hud_snd.m_activeSnd = nullptr;
}

HUD_SOUND_COLLECTION::HUD_SOUND_COLLECTION() : m_alias("")
{
	m_sound_items.clear();

	m_sound_items.resize(m_sound_items.size() + 1);
	HUD_SOUND_ITEM& snd_item = m_sound_items.back();
	snd_item.m_alias = "sndByMotion";
	snd_item.m_b_exclusive = false;
}

//----------------------------------------------------------
HUD_SOUND_COLLECTION::~HUD_SOUND_COLLECTION()
{
	Clear();
}

void HUD_SOUND_COLLECTION::Clear()
{

	for (HUD_SOUND_ITEM& it : m_sound_items)
	{
		HUD_SOUND_ITEM::StopSound(it);
		HUD_SOUND_ITEM::DestroySound(it);
	}

	m_sound_items.clear();
}

HUD_SOUND_ITEM* HUD_SOUND_COLLECTION::FindSoundItem(const char* alias, bool b_assert)
{
	xr_vector<HUD_SOUND_ITEM>::iterator it = std::find(m_sound_items.begin(),m_sound_items.end(),alias);
	
	if (it != m_sound_items.end())
	{
		return &*it;
	}
    else
    {
		R_ASSERT3(!b_assert,"sound item not found in collection", alias);
        return nullptr;
	}
}

void HUD_SOUND_COLLECTION::PlaySound(HUD_SOUND_ITEM* snd_item, const Fvector& position, const CObject* parent, bool hud_mode, bool looped, bool allowOverlap, u8 index)
{
	for (HUD_SOUND_ITEM& Item : m_sound_items)
	{
		if (Item.m_b_exclusive)
		{
			HUD_SOUND_ITEM::StopSound(Item);
		}
	}

	HUD_SOUND_ITEM::PlaySound(*snd_item, position, parent, hud_mode, looped, allowOverlap, index);
}

void HUD_SOUND_COLLECTION::PlaySound(const char* alias, const Fvector& position, const CObject* parent, bool hud_mode, bool looped, bool allowOverlap, u8 index)
{
	HUD_SOUND_ITEM* snd_item = FindSoundItem(alias, false);
	if (snd_item != nullptr)
	{
		HUD_SOUND_ITEM::PlaySound(*snd_item, position, parent, hud_mode, looped, allowOverlap, index);
	}
}

void HUD_SOUND_COLLECTION::StopSound(const char* alias)
{
	HUD_SOUND_ITEM* snd_item = FindSoundItem(alias, true);
	HUD_SOUND_ITEM::StopSound(*snd_item);
}

void HUD_SOUND_COLLECTION::SetPosition(const char* alias, const Fvector& pos)
{
    HUD_SOUND_ITEM* snd_item = FindSoundItem(alias, false);
    if (snd_item != nullptr && snd_item->playing())
	{
		snd_item->set_position(pos);
	}
}

void HUD_SOUND_COLLECTION::StopAllSounds()
{
	for (HUD_SOUND_ITEM& it : m_sound_items)
	{
		HUD_SOUND_ITEM::StopSound(it);
	}
}

void HUD_SOUND_COLLECTION::LoadSound(const char* section, const char* line, const char* alias, bool exclusive, int type, esound_type sound_type)
{
	HUD_SOUND_ITEM* snd_item = FindSoundItem(alias, false);
	if (snd_item)
	{
		HUD_SOUND_ITEM::LoadSound(section, line, *snd_item, type, sound_type);
		snd_item->m_alias = alias;
		snd_item->m_b_exclusive = exclusive;
	}
	else
	{
		m_sound_items.resize(m_sound_items.size() + 1);
		HUD_SOUND_ITEM& snd_item_new = m_sound_items.back();
		HUD_SOUND_ITEM::LoadSound(section, line, snd_item_new, type, sound_type);
		snd_item_new.m_alias = alias;
		snd_item_new.m_b_exclusive = exclusive;
	}
}

//Alundaio:
/*
It's usage is to play a group of sounds HUD_SOUND_ITEMs as if they were a single layered entity. This is a achieved by
wrapping the class around HUD_SOUND_COLLECTION and tagging them with the same alias. This way, when one for example
sndShot is played, it will play all the sound items with the same alias.
*/
//----------------------------------------------------------
HUD_SOUND_COLLECTION_LAYERED::~HUD_SOUND_COLLECTION_LAYERED()
{
	m_sound_items.clear();
}

void HUD_SOUND_COLLECTION_LAYERED::StopAllSounds()
{
	for (HUD_SOUND_COLLECTION& it : m_sound_items)
	{
		it.StopAllSounds();
	}
}

void HUD_SOUND_COLLECTION_LAYERED::StopSound(const char* alias)
{
	for (HUD_SOUND_COLLECTION& it : m_sound_items)
	{
		if (it.m_alias == alias)
		{
			it.StopSound(alias);
		}
	}
}

void HUD_SOUND_COLLECTION_LAYERED::SetPosition(const char* alias, const Fvector& pos)
{
	for (HUD_SOUND_COLLECTION& it : m_sound_items)
	{
		if (it.m_alias == alias)
		{
			it.SetPosition(alias, pos);
		}
	}
}

void HUD_SOUND_COLLECTION_LAYERED::PlaySound(const char* alias, const Fvector& position, const CObject* parent, bool hud_mode, bool looped, bool allowOverlap, u8 index)
{
    const bool weaponShot = alias &&
        (strncmp(alias, "sndShot", 7) == 0 || strncmp(alias, "sndSilencerShot", 15) == 0);

    // Sample actual post-selection layer resources for one NPC event per
    // five seconds, with fixed process-wide record and per-event limits.
    static u32 ReportedNpcShots = 0;
    static u32 LastNpcReport = 0;
    const bool ReportNpcShot = weaponShot && parent && !smart_cast<const CActor*>(parent) &&
        ReportedNpcShots < 64 && (ReportedNpcShots == 0 || Device.dwTimeGlobal - LastNpcReport >= 5000);
    if (ReportNpcShot)
    {
        ++ReportedNpcShots;
        LastNpcReport = Device.dwTimeGlobal;
    }

    // Preserve each authored layer. The native voice effect reserves HRTF
    // processing for the primary world transient; balance stays in LTX.
    if (weaponShot)
        XRay::Sound::Mixer::BeginShotGroup();

    u32 layer = 0;
    for (HUD_SOUND_COLLECTION& it : m_sound_items)
    {
        if (it.m_alias != alias)
            continue;

        if (weaponShot)
        {
            XRay::Sound::Mixer::SetShotLayer(layer, 1.0f);
        }
        // Let automatic-fire events overlap in time, but share their
        // heavyweight positional processing with the close layer.
        it.PlaySound(alias, position, parent, hud_mode, looped, allowOverlap, index);
        if (ReportNpcShot && layer < 8)
        {
            const auto* Item = it.FindSoundItem(alias, false);
            const auto* Active = Item ? Item->m_activeSnd : nullptr;
            const char* Sample = Active && Active->snd._p ? Active->snd._p->fn_attached[0].c_str() : "";
            Msg("* [LIMINAL_NPC_SHOT_LAYER] event=%u time_ms=%u frame=%u owner_id=%u alias=%s layer=%u sample=%s volume=%.3f delay=%.3f",
                ReportedNpcShots, Device.dwTimeGlobal, Device.dwFrame, parent->ID(), alias, layer,
                Sample ? Sample : "", Active ? Active->volume : 0.0f, Active ? Active->delay : 0.0f);
        }
        ++layer;
    }

    if (weaponShot)
    {
        XRay::Sound::Mixer::EndShotGroup();
        if (ReportNpcShot)
        {
            Msg("* [LIMINAL_NPC_SHOT_LAYERS] event=%u time_ms=%u frame=%u owner_id=%u alias=%s layers_total=%u layers_reported=%u",
                ReportedNpcShots, Device.dwTimeGlobal, Device.dwFrame, parent->ID(), alias,
                layer, std::min(layer, 8u));
        }
        static u32 restoredShots = 0;
        static u32 restoredLayers = 0;
        static u32 lastReport = 0;
        ++restoredShots;
        restoredLayers += layer;
        if (Device.dwTimeGlobal - lastReport > 5000)
        {
            Msg("* [LIMINAL_SHOT_GROUP] shots=%u layers_played=%u",
                restoredShots, restoredLayers);
            restoredShots = restoredLayers = 0;
            lastReport = Device.dwTimeGlobal;
        }
    }
}

HUD_SOUND_ITEM* HUD_SOUND_COLLECTION_LAYERED::FindSoundItem(const char* alias, bool b_assert)
{
	for (HUD_SOUND_COLLECTION& it : m_sound_items)
	{
		if (it.m_alias == alias)
		{
			return it.FindSoundItem(alias, b_assert);
		}
	}

    return nullptr;
}

void HUD_SOUND_COLLECTION_LAYERED::LoadSound(const char* section, const char* line, const char* alias, bool exclusive, int type, esound_type sound_type)
{
	if (!pSettings->line_exist(section, line))
	{
		return;
	}
	// Layer keys no longer contain the outer suppressed-shot sound name.
	// Preserve its AI propagation type before expanding the collection.
	if (strstr(line, "silencer") || strstr(line, "silncer"))
	{
		type |= SOUND_TYPE_SUPPRESSED;
	}

	xr_vector<HUD_SOUND_COLLECTION> NewItems;
	const char* str = pSettings->r_string(section, line);
	string256 buf_str;

	int	count = _GetItemCount(str);
	R_ASSERT2(count, make_string<const char*>(
		"In section [%s], line [%s] does not contain sound", section, line));

	_GetItem(str, 0, buf_str);
	
	if (pSettings->section_exist(buf_str))
	{
		string256 sound_line;
		xr_strcpy(sound_line, "snd_1_layer");
		int k = 1;

		while (pSettings->line_exist(buf_str, sound_line))
		{
			NewItems.emplace_back();
			HUD_SOUND_COLLECTION& snd_item = NewItems.back();
			snd_item.LoadSound(buf_str, sound_line, alias, exclusive, type, sound_type);
			snd_item.m_alias = alias;
			xr_sprintf(sound_line, "snd_%d_layer", ++k);
		}
	}
	else //For compatibility with normal HUD_SOUND_COLLECTION sounds
	{
		NewItems.emplace_back();
		HUD_SOUND_COLLECTION& snd_item = NewItems.back();
		snd_item.LoadSound(section, line, alias, exclusive, type, sound_type);
		snd_item.m_alias = alias;
	}
	if (!NewItems.empty())
	{
		for (auto& Item : m_sound_items)
		{
			if (Item.m_alias == alias)
			{
				Item.Clear();
			}
		}
		std::erase_if(m_sound_items, [alias](const HUD_SOUND_COLLECTION& Item) { return Item.m_alias == alias; });
		for (auto& Item : NewItems)
		{
			m_sound_items.emplace_back(std::move(Item));
		}
	}
}

void HUD_SOUND_COLLECTION_LAYERED::LoadSound(CInifile const* ini, const char* section, const char* line, const char* alias, bool exclusive, int type, esound_type sound_type)
{
	if (strstr(line, "silencer") || strstr(line, "silncer"))
	{
		type |= SOUND_TYPE_SUPPRESSED;
	}
	if (!ini->line_exist(section, line))
	{
		return;
	}
	xr_vector<HUD_SOUND_COLLECTION> NewItems;
	const char* str = ini->r_string(section, line);
	string256 buf_str;

	int	count = _GetItemCount(str);
	R_ASSERT2(count, make_string<const char*>(
		"In section [%s], line [%s] does not contain sound", section, line));

	_GetItem(str, 0, buf_str);

	if (ini->section_exist(buf_str))
	{
		string256 sound_line;
		xr_strcpy(sound_line, "snd_1_layer");
		int k = 1;
		while (ini->line_exist(buf_str, sound_line))
		{
			NewItems.emplace_back();
			HUD_SOUND_COLLECTION& snd_item = NewItems.back();
			snd_item.LoadSound(buf_str, sound_line, alias, exclusive, type, sound_type);
			snd_item.m_alias = alias;
			xr_sprintf(sound_line, "snd_%d_layer", ++k);
		}
	}
	else //For compatibility with normal HUD_SOUND_COLLECTION sounds
	{
		NewItems.emplace_back();
		HUD_SOUND_COLLECTION& snd_item = NewItems.back();
		snd_item.LoadSound(section, line, alias, exclusive, type, sound_type);
		snd_item.m_alias = alias;
	}

	if (!NewItems.empty())
	{
		for (auto& Item : m_sound_items)
		{
			if (Item.m_alias == alias)
			{
				Item.Clear();
			}
		}
		std::erase_if(m_sound_items, [alias](const HUD_SOUND_COLLECTION& Item) { return Item.m_alias == alias; });
		for (auto& Item : NewItems)
		{
			m_sound_items.emplace_back(std::move(Item));
		}
	}
}
//-Alundaio
