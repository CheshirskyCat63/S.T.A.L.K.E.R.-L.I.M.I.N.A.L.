#include "StdAfx.h"
#include "pch_script.h"
#include "ActorCondition.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "Inventory.h"
#include "Level.h"
#include "game_base_space.h"
#include "autosave_manager.h"
#include "xrServer.h"
#include "ai_space.h"
#include "script_game_object.h"
#include "game_object_space.h"
#include "object_broker.h"
#include "Weapon.h"
#include "ActorHelmet.h"
#include "InventoryVolumeSystem.h"

#include "PDA.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "UIGameCustom.h"
#include "ui/UIMainIngameWnd.h"
#include "../../xrUI/Widgets/UIStatic.h"
#include "../xrEngine/GameMtlLib.h"
#include "CharacterPhysicsSupport.h"
#include "RadioactiveZone.h"
#include "../Include/xrRender/Kinematics.h"
#include "SaveObjectHelpers.h"

#define MAX_SATIETY					1.0f
#define START_SATIETY				0.5f

ICF bool GodMode()
{
	return IsGameTypeSingle() && psActorFlags.test(AF_GOD_MODE | AF_DISABLE_CONDITION_TEST);
}

CActorCondition::CActorCondition(CActor *object) :
	inherited	(object)
{
	m_fJumpPower				= 0.f;
	m_fStandPower				= 0.f;
	m_fWalkPower				= 0.f;
	m_fJumpWeightPower			= 0.f;
	m_fWalkWeightPower			= 0.f;
	m_fOverweightWalkK			= 0.f;
	m_fOverweightJumpK			= 0.f;
	m_fAccelK					= 0.f;
	m_fSprintK					= 0.f;
	Satiety.Current				= 1.0f;
	Alcohol.Current				= 0.0f;
	Thirst.Current				= 1.0f;
	Sleepiness.Current			= 1.0f;
	Intoxication.Current		= 0.0f;

//	m_vecBoosts.clear();

	VERIFY						(object);
	m_object					= object;
	m_condition_flags.zero		();
	m_death_effector			= nullptr;

	m_zone_max_power[ALife::infl_rad]	= 1.0f;
	m_zone_max_power[ALife::infl_fire]	= 1.0f;
	m_zone_max_power[ALife::infl_acid]	= 1.0f;
	m_zone_max_power[ALife::infl_psi]	= 1.0f;
	m_zone_max_power[ALife::infl_electra]= 1.0f;

	m_zone_danger[ALife::infl_rad]	= 0.0f;
	m_zone_danger[ALife::infl_fire]	= 0.0f;
	m_zone_danger[ALife::infl_acid]	= 0.0f;
	m_zone_danger[ALife::infl_psi]	= 0.0f;
	m_zone_danger[ALife::infl_electra]= 0.0f;
	m_f_time_affected = Device.fTimeGlobal;

	m_max_power_restore_speed	= 0.0f;
	m_max_wound_protection		= 0.0f;
	m_max_fire_wound_protection = 0.0f;
	m_fIntoxicationThirstK		= 0.0f;

	for (u8 i = 0; i < eBoostMaxCount; i++)
	{
		SBooster booster = SBooster();
		booster.m_type = EBoostParams(i);
		m_booster_influences.emplace(EBoostParams(i), booster);
	}
}

CActorCondition::~CActorCondition()
{
	xr_delete( m_death_effector );
	m_booster_influences.clear();
}

void CActorCondition::LoadCondition(const char* entity_section)
{
	inherited::LoadCondition(entity_section);
	LoadBodyHealth();

	const char*						section = READ_IF_EXISTS(pSettings,r_string,entity_section,"condition_sect",entity_section);

	m_fJumpPower				= pSettings->r_float(section,"jump_power");
	m_fStandPower				= pSettings->r_float(section,"stand_power");
	m_fWalkPower				= pSettings->r_float(section,"walk_power");
	m_fJumpWeightPower			= pSettings->r_float(section,"jump_weight_power");
	m_fWalkWeightPower			= pSettings->r_float(section,"walk_weight_power");
	m_fOverweightWalkK			= pSettings->r_float(section,"overweight_walk_k");
	m_fOverweightJumpK			= pSettings->r_float(section,"overweight_jump_k");
	m_fAccelK					= pSettings->r_float(section,"accel_k");
	m_fSprintK					= pSettings->r_float(section,"sprint_k");

	//порог силы и здоровья меньше которого актер начинает хромать
	m_fLimpingHealthBegin		= pSettings->r_float(section,	"limping_health_begin");
	m_fLimpingHealthEnd			= pSettings->r_float(section,	"limping_health_end");
	R_ASSERT					(m_fLimpingHealthBegin<=m_fLimpingHealthEnd);

	m_fLimpingPowerBegin		= pSettings->r_float(section,	"limping_power_begin");
	m_fLimpingPowerEnd			= pSettings->r_float(section,	"limping_power_end");
	R_ASSERT					(m_fLimpingPowerBegin<=m_fLimpingPowerEnd);

	m_fCantWalkPowerBegin		= pSettings->r_float(section,	"cant_walk_power_begin");
	m_fCantWalkPowerEnd			= pSettings->r_float(section,	"cant_walk_power_end");
	R_ASSERT					(m_fCantWalkPowerBegin<=m_fCantWalkPowerEnd);

	m_fCantSprintPowerBegin		= pSettings->r_float(section,	"cant_sprint_power_begin");
	m_fCantSprintPowerEnd		= pSettings->r_float(section,	"cant_sprint_power_end");
	R_ASSERT					(m_fCantSprintPowerBegin<=m_fCantSprintPowerEnd);

	m_fPowerLeakSpeed			= pSettings->r_float(section,"max_power_leak_speed");
	m_MaxWalkWeight				= pSettings->r_float(section,"max_walk_weight");
	
	//////////////////////////////////////////////////////////////////////////////////////////////
	// Alcohol
	Alcohol.Variability			= pSettings->r_float(section,"alcohol_v");

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Satiety
	Satiety.Critical = pSettings->r_float(section,"satiety_critical");
	clamp(Satiety.Critical, 0.0f, 1.0f);

	Satiety.Variability			= pSettings->r_float(section,"satiety_v");
	Satiety.PowerBoost			= pSettings->r_float(section,"satiety_power_v");
	Satiety.HealthBoost			= pSettings->r_float(section,"satiety_health_v");
	
	//////////////////////////////////////////////////////////////////////////////////////////////
	// Thirst
	const static bool enableThirst = EngineExternal()[EEngineExternalGame::EnableThirst];
	if (enableThirst)
	{
		Thirst.Critical = pSettings->r_float(section,"thirst_critical");
		clamp(Thirst.Critical, 0.0f, 1.0f);

		Thirst.Variability			= pSettings->r_float(section,"thirst_v");
		Thirst.PowerBoost			= pSettings->r_float(section,"thirst_power_v");
		Thirst.HealthBoost			= pSettings->r_float(section,"thirst_health_v");
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Sleepiness
	const static bool enableSleepiness = EngineExternal()[EEngineExternalGame::EnableSleepiness];
	if (enableSleepiness)
	{
		Sleepiness.Critical = pSettings->r_float(section,"sleepiness_critical");
		clamp(Sleepiness.Critical, 0.0f, 1.0f);

		Sleepiness.Variability		= pSettings->r_float(section,"sleepiness_v");
		Sleepiness.PowerBoost		= pSettings->r_float(section,"sleepiness_power_v");
		Sleepiness.HealthBoost		= pSettings->r_float(section,"sleepiness_health_v");
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Intoxication
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (enableMedIntoxication)
	{
		Intoxication.Critical = READ_IF_EXISTS(pSettings, r_float, section, "intoxication_critical", 0.35f);
		clamp(Intoxication.Critical, 0.0f, 1.0f);

		Intoxication.Variability = READ_IF_EXISTS(pSettings, r_float, section, "intoxication_v", -0.00025f);
		Intoxication.PowerBoost = READ_IF_EXISTS(pSettings, r_float, section, "intoxication_power_v", -0.005f);
		Intoxication.HealthBoost = READ_IF_EXISTS(pSettings, r_float, section, "intoxication_health_v", -0.0015f);
		m_fIntoxicationThirstK = READ_IF_EXISTS(pSettings, r_float, section, "intoxication_thirst_k", 0.3f);
	}
	else
	{
		m_fIntoxicationThirstK = 0.0f;
	}

	m_zone_max_power[ALife::infl_rad]	= READ_IF_EXISTS(pSettings, r_float, section, "radio_zone_max_power", 1.0f);
	m_zone_max_power[ALife::infl_fire]	= READ_IF_EXISTS(pSettings, r_float, section, "fire_zone_max_power", 1.0f);
	m_zone_max_power[ALife::infl_acid]	= READ_IF_EXISTS(pSettings, r_float, section, "acid_zone_max_power", 1.0f);
	m_zone_max_power[ALife::infl_psi]	= READ_IF_EXISTS(pSettings, r_float, section, "psi_zone_max_power", 1.0f);
	m_zone_max_power[ALife::infl_electra]= READ_IF_EXISTS(pSettings, r_float, section, "electra_zone_max_power", 1.0f);

	m_max_power_restore_speed = READ_IF_EXISTS(pSettings, r_float, section, "max_power_restore_speed", 1.0f);
	m_max_wound_protection = READ_IF_EXISTS(pSettings,r_float,section,"max_wound_protection",1.0f);
	m_max_fire_wound_protection = READ_IF_EXISTS(pSettings,r_float,section,"max_fire_wound_protection",1.0f);

	VERIFY( !fis_zero(m_zone_max_power[ALife::infl_rad]) );
	VERIFY( !fis_zero(m_zone_max_power[ALife::infl_fire]) );
	VERIFY( !fis_zero(m_zone_max_power[ALife::infl_acid]) );
	VERIFY( !fis_zero(m_zone_max_power[ALife::infl_psi]) );
	VERIFY( !fis_zero(m_zone_max_power[ALife::infl_electra]) );
	VERIFY( !fis_zero(m_max_power_restore_speed) );
}

float CActorCondition::GetZoneMaxPower( ALife::EInfluenceType type) const
{
	if ( type < ALife::infl_rad || ALife::infl_electra < type )
	{
		return 1.0f;
	}
	return m_zone_max_power[type];
}

float CActorCondition::GetZoneMaxPower( ALife::EHitType hit_type ) const
{
	ALife::EInfluenceType iz_type = ALife::infl_max_count;
	switch( hit_type )
	{
	case ALife::eHitTypeRadiation:		iz_type = ALife::infl_rad;		break;
	case ALife::eHitTypeLightBurn:		iz_type = ALife::infl_fire;		break;
	case ALife::eHitTypeBurn:			iz_type = ALife::infl_fire;		break;
	case ALife::eHitTypeChemicalBurn:	iz_type = ALife::infl_acid;		break;
	case ALife::eHitTypeTelepatic:		iz_type = ALife::infl_psi;		break;
	case ALife::eHitTypeShock:			iz_type = ALife::infl_electra;	break;

	case ALife::eHitTypeStrike:
	case ALife::eHitTypeExplosion:
	case ALife::eHitTypeFireWound:
	case ALife::eHitTypeWound_2:
	case ALife::eHitTypePhysicStrike:
		return 1.0f;
	case ALife::eHitTypeWound:
		return m_max_wound_protection;
	default:
		NODEFAULT;
	}
	
	return GetZoneMaxPower( iz_type );
}

void CActorCondition::UpdateCondition()
{
	// FX: Хак для кат-сцен (GODMODE_RT)
	if (!psActorFlags.test(AF_GOD_MODE))
	{
		Alcohol.Current += Alcohol.Variability * m_fDeltaTime;
		clamp(Alcohol.Current, 0.0f, 1.0f);

		UpdateSatiety();
		UpdateThirst();
		UpdateIntoxication();
		UpdateBoosters();
	}

	if (GodMode())
	{
		HasAcceptedHit = false;
		AcceptedHitEligible = false;
		SetPower(1.f);
		return;
	}

	if (!object().g_Alive())
	{
		HasAcceptedHit = false;
		AcceptedHitEligible = false;
		ClearBodyHealthEffects();
		return;
	}
	if (!object().Local() && m_object != Level().CurrentViewEntity())
	{
		HasAcceptedHit = false;
		AcceptedHitEligible = false;
		return;
	}

	float base_weight			= object().MaxCarryWeight();
	float cur_weight			= object().inventory().TotalWeight();
	const SInventoryVolumePenalty volumePenalty = CInventoryVolumeSystem::Get().GetPenalty(object());
	const float weightFactor = cur_weight / base_weight + volumePenalty.staminaPowerPenalty;

	if (m_object->Holder() == nullptr)
	{
		if ((object().mstate_real & mcAnyMove))
		{
			ConditionWalk(weightFactor,
				isActorAccelerated(object().mstate_real, object().IsZoomAimingMode()),
				(object().mstate_real & mcSprint) != 0);
		}
		else
		{
			ConditionStand(weightFactor);
		}

		if (IsGameTypeSingleCompatible())
		{
			float k_max_power = 1.0f + std::min(cur_weight, base_weight) / base_weight
				+ std::max(0.0f, (cur_weight - base_weight) / 10.0f);

			SetMaxPower(GetMaxPower() - m_fPowerLeakSpeed * m_fDeltaTime * k_max_power * (1.0f + volumePenalty.staminaPowerPenalty));
		}
	}
	else
	{
		SetMaxPower(1.f);
	}

	if (IsGameTypeSingleCompatible() && !g_dedicated_server)
	{
		CEffectorCam* pAlcoholEffector = Actor()->Cameras().GetCamEffector((ECamEffectorType)effAlcohol);
		if ((Alcohol.Current > 0.0001f))
		{
			if (pAlcoholEffector == nullptr)
			{
				AddEffector(m_object, effAlcohol, "effector_alcohol", GET_KOEFF_FUNC(this, &CActorCondition::GetAlcohol));
			}
		}
		else if (pAlcoholEffector)
		{
			RemoveEffector(m_object, effAlcohol);
		}

		shared_str ln = Level().name();
		if (ln.size())
		{
			CEffectorPP* ppe = object().Cameras().GetPPEffector((EEffectorPPType)effPsyHealth);

			string512 pp_sect_name;
			xr_strconcat(pp_sect_name, "effector_psy_health", "_", *ln);

			if (!pSettings->section_exist(pp_sect_name))
				xr_strcpy(pp_sect_name, "effector_psy_health");

			if (!fsimilar(GetPsyHealth(), 1.0f, 0.05f))
			{
				if (!ppe)
				{
					AddEffector(m_object, effPsyHealth, pp_sect_name, GET_KOEFF_FUNC(this, &CActorCondition::GetPsy));
				}
			}
			else if (ppe)
			{
				RemoveEffector(m_object, effPsyHealth);
			}
		}
		if (EngineExternal().ShadowOfChernobylMode() && fis_zero(GetPsyHealth()))
		{
			SetHealth(0.0f);
		}
	}

	UpdateSleepiness();

	inherited::UpdateCondition();
	UpdateBodyHealth();

	if( IsGameTypeSingle() )
		UpdateTutorialThresholds();

	if(GetHealth()<0.05f && m_death_effector==nullptr && IsGameTypeSingle())
	{
		if(pSettings->section_exist("actor_death_effector"))
			m_death_effector = new CActorDeathEffector(this, "actor_death_effector");
	}
	if(m_death_effector && m_death_effector->IsActual())
	{
		m_death_effector->UpdateCL	();

		if(!m_death_effector->IsActual())
			m_death_effector->Stop();
	}

	AffectDamage_InjuriousMaterialAndMonstersInfluence();
}

void CActorCondition::UpdateBoosters()
{
	for (auto& booster : m_booster_influences)
	{
		booster.second.fBoostTime -= m_fDeltaTime / (IsGameTypeSingle() ? Level().GetGameTimeFactor() : 1.0f);
		if (booster.second.fBoostTime <= 0.0f)
		{
			booster.second.fBoostTime = 0.0f;
			booster.second.fBoostValue = 0.0f;
			DisableBoostParameters(booster.second);
		}
	}

	if (m_object == Level().CurrentViewEntity() && !g_dedicated_server)
	{
		CurrentGameUI()->UIMainIngameWnd->UpdateBoosterIndicators(m_booster_influences);
	}
}

void CActorCondition::AffectDamage_InjuriousMaterialAndMonstersInfluence()
{
	float one = 0.1f;
	float tg  = Device.fTimeGlobal;
	if ( m_f_time_affected + one > tg )
	{
		return;
	}

	clamp( m_f_time_affected, tg - (one * 3), tg );

	float psy_influence					=	0;
	float fire_influence				=	0;
	float radiation_influence			=	GetInjuriousMaterialDamage(); // Get Radiation from Material

	// Add Radiation and Psy Level from Monsters
	if (m_object && m_object->g_Alive())
	{
		using monsters = xr_vector<CObject*>;

		for (const CObject* Object : m_object->feel_touch)
		{
			CObject* cast_object = const_cast<CObject*>(Object);
			const CBaseMonster* monster = cast_object != nullptr ? cast_object->cast_base_monster() : nullptr;
			if (!monster || !monster->g_Alive()) continue;

			psy_influence += monster->get_psy_influence();
			radiation_influence += monster->get_radiation_influence();
			fire_influence += monster->get_fire_influence();
		}
	}

	struct 
	{
		ALife::EHitType	type;
		float			value;

	} hits[]		=	{	{ ALife::eHitTypeRadiation, radiation_influence	*	one },
							{ ALife::eHitTypeTelepatic, psy_influence		*	one }, 
							{ ALife::eHitTypeBurn,		fire_influence		*	one }	};

 	NET_Packet	np;

	while ( m_f_time_affected + one < tg )
	{
		m_f_time_affected			+=	one;

		for ( int i=0; i<sizeof(hits)/sizeof(hits[0]); ++i )
		{
			float			damage	=	hits[i].value;
			ALife::EHitType	type	=	hits[i].type;

			if ( damage > EPS )
			{
				SHit HDS = SHit(damage, 
//.								0.0f, 
								Fvector().set(0,1,0), 
								nullptr, 
								BI_NONE, 
								Fvector().set(0,0,0), 
								0.0f, 
								type, 
								0.0f, 
								false);

				HDS.GenHeader(GE_HIT, m_object->ID());
				HDS.Write_Packet( np );
				CGameObject::u_EventSend( np );
			}

		} // for

	}//while
}

float CActorCondition::GetInjuriousMaterialDamage()
{
	u16 mat_injurios = m_object->character_physics_support()->movement()->injurious_material_idx();

	if(mat_injurios!=GAMEMTL_NONE_IDX)
	{
		const SGameMtl* mtl		= GMLib.GetMaterialByIdx(mat_injurios);
		return					mtl->fInjuriousSpeed;
	}else
		return 0.0f;
}

void CActorCondition::SetZoneDanger( float danger, ALife::EInfluenceType type )
{
	VERIFY( type != ALife::infl_max_count );
	m_zone_danger[type] = danger;
	clamp( m_zone_danger[type], 0.0f, 1.0f );
}

float CActorCondition::GetZoneDanger() const
{
	float sum = 0.0f;
	for ( u8 i = 1; i < ALife::infl_max_count; ++i )
	{
		sum += m_zone_danger[i];
	}

	clamp( sum, 0.0f, 1.5f );
	return sum;
}

void CActorCondition::UpdateRadiation()
{
	if (m_object)
	{
		m_fRadiationZonePower = 0;
		for (ISpatialShared& SS : m_object->q_nearest)
		{
			ISpatial* S = SS.get();

			if (!S)
				continue;

			CObject* pFeelObject = S->dcast_CObject();

			if (!pFeelObject || pFeelObject->getDestroy())
				continue;

			if (pFeelObject == nullptr || pFeelObject->getDestroy() || pFeelObject->CLS_ID == 0)
				continue;

			CGameObject* p_go = pFeelObject->cast_game_object();

			if (p_go == nullptr)
				continue;

			CRadioactiveZone* p_rz = p_go->cast_radioactive_zone();

			if (p_rz == nullptr)
				continue;

			m_fRadiationZonePower = std::max(m_fRadiationZonePower, p_rz->fHitPower * 10.f);
		}
	}

	inherited::UpdateRadiation();
}

void CActorCondition::UpdateSatiety()
{
	if (!IsGameTypeSingleCompatible())
	{
		m_fDeltaPower += (Satiety.PowerBoost + m_fBoostPowerRestore) * m_fDeltaTime;
		return;
	}

	if (Satiety.Current > 0)
	{
		Satiety.Current -= Satiety.Variability * m_fDeltaTime;
		clamp(Satiety.Current, 0.0f, 1.0f);
	}

	float satiety_health_koef = (Satiety.Current - Satiety.Critical) / (Satiety.Current >= Satiety.Critical ? 1 - Satiety.Critical : Satiety.Critical);
	if (CanBeHarmed() && !psActorFlags.test(AF_DISABLE_CONDITION_TEST))
	{
		m_fDeltaHealth += Satiety.HealthBoost * satiety_health_koef * m_fDeltaTime;
		m_fDeltaPower += (Satiety.PowerBoost + m_fBoostPowerRestore) * Satiety.Current * m_fDeltaTime;
	}
}

void CActorCondition::UpdateThirst()
{
	const static bool enableThirst = EngineExternal()[EEngineExternalGame::EnableThirst];
	if (!enableThirst)
		return;

	if (Thirst.Current > 0.0f)
	{
		Thirst.Current -= Thirst.Variability * m_fDeltaTime;
		clamp(Thirst.Current, 0.0f, 1.0f);
	}

	float thirst_health_koef = (Thirst.Current - Thirst.Critical) / (Thirst.Current >= Thirst.Critical ? 1 - Thirst.Critical : Thirst.Critical);
	if (CanBeHarmed() && !psActorFlags.test(AF_DISABLE_CONDITION_TEST))
	{
		m_fDeltaHealth += Thirst.HealthBoost * thirst_health_koef * m_fDeltaTime;
		m_fDeltaPower += Thirst.PowerBoost * Thirst.Current * m_fDeltaTime;
	}
}

void CActorCondition::UpdateSleepiness()
{
	const static bool enableSleepiness = EngineExternal()[EEngineExternalGame::EnableSleepiness];
	if (!enableSleepiness)
		return;

	if (Sleepiness.Current > 0)
	{
		Sleepiness.Current -= Sleepiness.Variability * m_fDeltaTime;
		clamp(Sleepiness.Current, 0.0f, 1.0f);
	}

	float SleepinessHealthKoef = ((1.f - Sleepiness.Current) - Sleepiness.Critical) / (Sleepiness.Current < Sleepiness.Critical ? 1 - Sleepiness.Critical : Sleepiness.Critical);
	if (CanBeHarmed() && !psActorFlags.test(AF_DISABLE_CONDITION_TEST))
	{
		m_fDeltaHealth += Sleepiness.HealthBoost * SleepinessHealthKoef * m_fDeltaTime;
		m_fDeltaPower += Sleepiness.PowerBoost * (1.f - Sleepiness.Current) * m_fDeltaTime;
	}
}

void CActorCondition::UpdateIntoxication()
{
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (!enableMedIntoxication)
	{
		return;
	}

	Intoxication.Current += Intoxication.Variability * m_fDeltaTime;
	clamp(Intoxication.Current, 0.0f, 1.0f);

	const float I = Intoxication.Current;
	if (CanBeHarmed() && !psActorFlags.test(AF_DISABLE_CONDITION_TEST) && I > 0.0f)
	{
		// Stamina drains with intoxication level from the first dose.
		m_fDeltaPower += Intoxication.PowerBoost * I * m_fDeltaTime;

		if (I > Intoxication.Critical)
		{
			const float denom = std::max(EPS, 1.0f - Intoxication.Critical);
			const float excess = (I - Intoxication.Critical) / denom;

			// HP drains past critical; stronger past heavy / critical overdose tiers
			float healthMul = 1.0f;
			if (I >= 0.7f)
			{
				healthMul = 1.5f;
			}
			if (I > 0.9f)
			{
				healthMul = 2.5f;
			}
			m_fDeltaHealth += Intoxication.HealthBoost * excess * healthMul * m_fDeltaTime;
		}
	}

	if (!IsGameTypeSingleCompatible() || g_dedicated_server)
	{
		return;
	}

	CEffectorCam* pCamEffector = Actor()->Cameras().GetCamEffector((ECamEffectorType)effIntoxication);
	if (I >= 0.7f)
	{
		if (pCamEffector == nullptr)
		{
			AddEffector(m_object, effIntoxication, "effector_intoxication", GET_KOEFF_FUNC(this, &CActorCondition::GetIntoxication));
		}
	}
	else if (pCamEffector)
	{
		RemoveEffector(m_object, effIntoxication);
	}

	CEffectorPP* pPPEffector = object().Cameras().GetPPEffector((EEffectorPPType)effIntoxicationPP);
	if (I > 0.9f)
	{
		if (pPPEffector == nullptr)
		{
			AddEffector(m_object, effIntoxicationPP, "effector_intoxication_pp", GET_KOEFF_FUNC(this, &CActorCondition::GetIntoxication));
		}
	}
	else if (pPPEffector)
	{
		RemoveEffector(m_object, effIntoxicationPP);
	}
}

float CActorCondition::GetMedicineEfficiencyFactor(const shared_str& sect) const
{
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (!enableMedIntoxication || Intoxication.Current <= Intoxication.Critical)
	{
		return 1.0f;
	}

	const float denom = std::max(EPS, 1.0f - Intoxication.Critical);
	float t = (Intoxication.Current - Intoxication.Critical) / denom;
	clamp(t, 0.0f, 1.0f);

	float kMin = READ_IF_EXISTS(pSettings, r_float, sect, "intoxication_heal_min", 0.25f);
	clamp(kMin, 0.0f, 1.0f);
	return 1.0f + (kMin - 1.0f) * t;
}

CWound* CActorCondition::ConditionHit(SHit* pHDS)
{
	if (GodMode()) return nullptr;
	return inherited::ConditionHit(pHDS);
}

void CActorCondition::PowerHit(float power, bool apply_outfit)
{
	m_fPower			-=	apply_outfit ? HitPowerEffect(power) : power;
	clamp					(m_fPower, 0.f, 1.f);
}
//weight - "удельный" вес от 0..1
void CActorCondition::ConditionJump(float weight)
{
	float power			=	m_fJumpPower;
	power				+=	m_fJumpWeightPower*weight*(weight>1.f?m_fOverweightJumpK:1.f);
	m_fPower			-=	HitPowerEffect(power);
}

void CActorCondition::ConditionWalk(float weight, bool accel, bool sprint)
{	
	float power			=	m_fWalkPower;
	power				+=	m_fWalkWeightPower*weight*(weight>1.f?m_fOverweightWalkK:1.f);
	power				*=	m_fDeltaTime*(accel?(sprint?m_fSprintK:m_fAccelK):1.f);
	m_fPower			-=	HitPowerEffect(power);
}

void CActorCondition::ConditionStand(float weight)
{	
	float power			= m_fStandPower;
	power				*= m_fDeltaTime;
	m_fPower			-= power;
}


bool CActorCondition::IsCantWalk() const
{
	if(m_fPower< m_fCantWalkPowerBegin)
		m_bCantWalk		= true;
	else if(m_fPower > m_fCantWalkPowerEnd)
		m_bCantWalk		= false;
	return				m_bCantWalk;
}

bool CActorCondition::IsCantWalkWeight()
{
	if(IsGameTypeSingleCompatible() && !GodMode())
	{
		float max_w	= m_object->MaxWalkWeight();
		const SInventoryVolumePenalty volumePenalty = CInventoryVolumeSystem::Get().GetPenalty(object());
		max_w = std::max(0.0f, max_w - volumePenalty.maxWalkWeightPenalty);

		if( object().inventory().TotalWeight() > max_w )
		{
			m_condition_flags.set			(eCantWalkWeight, true);
			return true;
		}
	}
	m_condition_flags.set					(eCantWalkWeight, false);
	return false;
}

bool CActorCondition::IsCantSprint() const
{
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (enableMedIntoxication && Intoxication.Current >= 0.7f)
	{
		return true;
	}

	if(m_fPower< m_fCantSprintPowerBegin)
		m_bCantSprint	= true;
	else if(m_fPower > m_fCantSprintPowerEnd)
		m_bCantSprint	= false;
	return				m_bCantSprint;
}

bool CActorCondition::IsLimping() const
{
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (enableMedIntoxication && Intoxication.Current > 0.9f)
	{
		return true;
	}

	if(m_fPower< m_fLimpingPowerBegin || GetHealth() < m_fLimpingHealthBegin)
		m_bLimping = true;
	else if(m_fPower > m_fLimpingPowerEnd && GetHealth() > m_fLimpingHealthEnd)
		m_bLimping = false;
	return m_bLimping;
}
extern bool g_bShowHudInfo;

void CActorCondition::save(NET_Packet &output_packet)
{
	inherited::save		(output_packet);
	save_data			(Alcohol.Current, output_packet);
	save_data			(m_condition_flags, output_packet);
	save_data			(Satiety.Current, output_packet);

	save_data(Thirst.Current, output_packet);
	save_data(Sleepiness.Current, output_packet);
	save_data(Intoxication.Current, output_packet);

	save_data			(m_curr_medicine_influence.fHealth, output_packet);
	save_data			(m_curr_medicine_influence.fPower, output_packet);
	save_data			(m_curr_medicine_influence.fSatiety, output_packet);
	save_data			(m_curr_medicine_influence.fThirst, output_packet);
	save_data			(m_curr_medicine_influence.fRadiation, output_packet);
	save_data			(m_curr_medicine_influence.fWoundsHeal, output_packet);
	save_data			(m_curr_medicine_influence.fMaxPowerUp, output_packet);
	save_data			(m_curr_medicine_influence.fAlcohol, output_packet);
	save_data			(m_curr_medicine_influence.fTimeTotal, output_packet);
	save_data			(m_curr_medicine_influence.fTimeCurrent, output_packet);

	output_packet.w_u8((u8)m_booster_influences.size());
	BOOSTER_MAP::iterator b = m_booster_influences.begin(), e = m_booster_influences.end();
	for (; b != e; b++)
	{
		output_packet.w_u8((u8)b->second.m_type);
		output_packet.w_float(b->second.fBoostValue);
		output_packet.w_float(b->second.fBoostTime);
	}
}

void CActorCondition::load(IReader &input_packet)
{
	BodyHealth.Reset();
	BodyTreatmentTarget = EBodyRegion::All;
	BodyTreatmentItem = ALife::INVALID_OBJECT_ID;
	inherited::load		(input_packet);
	load_data			(Alcohol.Current, input_packet);
	load_data			(m_condition_flags, input_packet);
	load_data			(Satiety.Current, input_packet);

	load_data(Thirst.Current, input_packet);
	load_data(Sleepiness.Current, input_packet);
	load_data(Intoxication.Current, input_packet);

	load_data			(m_curr_medicine_influence.fHealth, input_packet);
	load_data			(m_curr_medicine_influence.fPower, input_packet);
	load_data			(m_curr_medicine_influence.fSatiety, input_packet);
	load_data			(m_curr_medicine_influence.fThirst, input_packet);
	load_data			(m_curr_medicine_influence.fRadiation, input_packet);
	load_data			(m_curr_medicine_influence.fWoundsHeal, input_packet);
	load_data			(m_curr_medicine_influence.fMaxPowerUp, input_packet);
	load_data			(m_curr_medicine_influence.fAlcohol, input_packet);
	load_data			(m_curr_medicine_influence.fTimeTotal, input_packet);
	load_data			(m_curr_medicine_influence.fTimeCurrent, input_packet);

	u8 cntr = input_packet.r_u8();
	for (; cntr > 0; cntr--)
	{
		SBooster B;
		B.m_type = (EBoostParams)input_packet.r_u8();
		B.fBoostValue = input_packet.r_float();
		B.fBoostTime = input_packet.r_float();
		B.fBoostMaxTime = B.fBoostTime;
		m_booster_influences[B.m_type] = B;
		BoostParameters(B);
	}
}

void CActorCondition::reinit()
{
	BodyHealth.Reset();
	BodyTreatmentTarget = EBodyRegion::All;
	BodyTreatmentItem = ALife::INVALID_OBJECT_ID;
	BodyConcussionRepeat = 0.f;
	inherited::reinit();
	m_bLimping = false;
	Satiety.Current = 1.0f;
	Thirst.Current = 1.0f;
	Intoxication.Current = 0.0f;
}

void CActorCondition::ChangeAlcohol(float value)
{
	Alcohol.Current += value;
}

void CActorCondition::ChangeSatiety(float value)
{
	Satiety.Current += value;
	clamp(Satiety.Current, 0.0f, 1.0f);
}

void CActorCondition::ChangeThirst(float value)
{
	Thirst.Current += value;
	clamp(Thirst.Current, 0.0f, 1.0f);
}

void CActorCondition::ChangeSleepiness(float value)
{
	Sleepiness.Current += value;
	clamp(Sleepiness.Current, 0.0f, 1.0f);
}

void CActorCondition::ChangeIntoxication(float value)
{
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (!enableMedIntoxication)
	{
		return;
	}

	Intoxication.Current += value;
	clamp(Intoxication.Current, 0.0f, 1.0f);
}

float CActorCondition::GetBoosterValueByType(EBoostParams type) const
{
	auto BoostInfluenceIter = m_booster_influences.find(type);
		return BoostInfluenceIter->second.fBoostValue;

	return 0.0f;
}

void CActorCondition::BoostParameters(const SBooster& B)
{
	switch (B.m_type)
	{
	case eBoostHpRestore:
	{
		m_fBoostHpRestore = B.fBoostValue;
		break;
	}
	case eBoostPowerRestore:
	{
		m_fBoostPowerRestore = B.fBoostValue;
		break;
	}
	case eBoostRadiationRestore:
	{
		m_fBoostRadiationRestore = B.fBoostValue;
		break;
	}
	case eBoostBleedingRestore:
	{
		m_fBoostBleedingRestore = B.fBoostValue;
		break;
	}
	case eBoostMaxWeight:
	{
		m_fBoostWeightAdd = B.fBoostValue;
		m_object->inventory().SetMaxWeight(object().inventory().GetMaxWeight() + m_fBoostWeightAdd);
		m_MaxWalkWeight += m_fBoostWeightAdd;
		break;
	}
	case eBoostBurnImmunity:
	{
		m_fBoostBurnImmunity = B.fBoostValue;
		break;
	}
	case eBoostShockImmunity:
	{
		m_fBoostShockImmunity = B.fBoostValue;
		break;
	}
	case eBoostRadiationImmunity:
	{
		m_fBoostRadiationImmunity = B.fBoostValue;
		break;
	}
	case eBoostTelepaticImmunity:
	{
		m_fBoostTelepaticImmunity = B.fBoostValue;
		break;
	}
	case eBoostChemicalBurnImmunity:
	{
		m_fBoostChemicalBurnImmunity = B.fBoostValue;
		break;
	}
	case eBoostExplImmunity:
	{
		m_fBoostExplImmunity = B.fBoostValue;
		break;
	}
	case eBoostStrikeImmunity:
	{
		m_fBoostStrikeImmunity = B.fBoostValue;
		break;
	}
	case eBoostFireWoundImmunity:
	{
		m_fBoostFireWoundImmunity = B.fBoostValue;
		break;
	}
	case eBoostWoundImmunity:
	{
		m_fBoostWoundImmunity = B.fBoostValue;
		break;
	}
	case eBoostRadiationProtection:
	{
		m_fBoostRadiationProtection = B.fBoostValue;
		break;
	}
	case eBoostTelepaticProtection:
	{
		m_fBoostTelepaticProtection = B.fBoostValue;
		break;
	}
	case eBoostChemicalBurnProtection:
	{
		m_fBoostChemicalBurnProtection = B.fBoostValue;
		break;
	}
	default: NODEFAULT;
	}
}

void CActorCondition::DisableBoostParameters(const SBooster& B)
{
	switch (B.m_type)
	{
	case eBoostHpRestore:
	{
		m_fBoostHpRestore = 0.0f;
		break;
	}
	case eBoostPowerRestore:
	{
		m_fBoostPowerRestore = 0.0f;
		break;
	}
	case eBoostRadiationRestore:
	{
		m_fBoostRadiationRestore = 0.0f;
		break;
	}
	case eBoostBleedingRestore:
	{
		m_fBoostBleedingRestore = 0.0f;
		break;
	}
	case eBoostMaxWeight:
	{
		m_object->inventory().SetMaxWeight(object().inventory().GetMaxWeight() - m_fBoostWeightAdd);
		m_MaxWalkWeight -= m_fBoostWeightAdd;
		m_fBoostWeightAdd = 0.0f;
		break;
	}
	case eBoostBurnImmunity:
	{
		m_fBoostBurnImmunity = 0.0f;
		break;
	}
	case eBoostShockImmunity:
	{
		m_fBoostShockImmunity = 0.0f;
		break;
	}
	case eBoostRadiationImmunity:
	{
		m_fBoostRadiationImmunity = 0.0f;
		break;
	}
	case eBoostTelepaticImmunity:
	{
		m_fBoostTelepaticImmunity = 0.0f;
		break;
	}
	case eBoostChemicalBurnImmunity:
	{
		m_fBoostChemicalBurnImmunity = 0.0f;
		break;
	}
	case eBoostExplImmunity:
	{
		m_fBoostExplImmunity = 0.0f;
		break;
	}
	case eBoostStrikeImmunity:
	{
		m_fBoostStrikeImmunity = 0.0f;
		break;
	}
	case eBoostFireWoundImmunity:
	{
		m_fBoostFireWoundImmunity = 0.0f;
		break;
	}
	case eBoostWoundImmunity:
	{
		m_fBoostWoundImmunity = 0.0f;
		break;
	}
	case eBoostRadiationProtection:
	{
		m_fBoostRadiationProtection = 0.0f;
		break;
	}
	case eBoostTelepaticProtection:
	{
		m_fBoostTelepaticProtection = 0.0f;
		break;
	}
	case eBoostChemicalBurnProtection:
	{
		m_fBoostChemicalBurnProtection = 0.0f;
		break;
	}
	default: NODEFAULT;
	}
}

void CActorCondition::UpdateTutorialThresholds()
{
	string256						cb_name;
	static float _cPowerThr			= pSettings->r_float("tutorial_conditions_thresholds","power");
	static float _cPowerMaxThr		= pSettings->r_float("tutorial_conditions_thresholds","max_power");
	static float _cBleeding			= pSettings->r_float("tutorial_conditions_thresholds","bleeding");
	static float _cSatiety			= pSettings->r_float("tutorial_conditions_thresholds","satiety");
	static float _cRadiation		= pSettings->r_float("tutorial_conditions_thresholds","radiation");
	static float _cWpnCondition		= pSettings->r_float("tutorial_conditions_thresholds","weapon_jammed");
	static float _cPsyHealthThr		= pSettings->r_float("tutorial_conditions_thresholds","psy_health");



	bool b = true;
	if(b && !m_condition_flags.test(eCriticalPowerReached) && GetPower()<_cPowerThr){
		m_condition_flags.set			(eCriticalPowerReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_critical_power");
	}

	if(b && !m_condition_flags.test(eCriticalMaxPowerReached) && GetMaxPower()<_cPowerMaxThr){
		m_condition_flags.set			(eCriticalMaxPowerReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_critical_max_power");
	}

	if(b && !m_condition_flags.test(eCriticalBleedingSpeed) && BleedingSpeed()>_cBleeding){
		m_condition_flags.set			(eCriticalBleedingSpeed, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_bleeding");
	}

	if(b && !m_condition_flags.test(eCriticalSatietyReached) && GetSatiety()<_cSatiety){
		m_condition_flags.set			(eCriticalSatietyReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_satiety");
	}

	if(b && !m_condition_flags.test(eCriticalRadiationReached) && GetRadiation()>_cRadiation){
		m_condition_flags.set			(eCriticalRadiationReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_radiation");
	}

	if(b && !m_condition_flags.test(ePhyHealthMinReached) && GetPsyHealth()<_cPsyHealthThr){
		m_condition_flags.set			(ePhyHealthMinReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_psy");
	}

	if(b && m_condition_flags.test(eCantWalkWeight) && !m_condition_flags.test(eCantWalkWeightReached)){
		m_condition_flags.set			(eCantWalkWeightReached, true);
		b=false;
		xr_strcpy(cb_name,"_G.on_actor_cant_walk_weight");
	}

	if (b && !m_condition_flags.test(eWeaponJammedReached)&&m_object->inventory().GetActiveSlot()!=NO_ACTIVE_SLOT){
		PIItem item = m_object->inventory().ActiveItem();
		CWeapon* pWeapon = item ? item->cast_weapon() : nullptr;
		if (pWeapon && pWeapon->GetCondition() < _cWpnCondition)
		{
			m_condition_flags.set(eWeaponJammedReached, true);
			b = false;
			xr_strcpy(cb_name,"_G.on_actor_weapon_jammed");
		}
	}
	
	if(!b){
		luabind::functor<const char*>			fl;
		R_ASSERT							(ai().script_engine().functor<const char*>(cb_name,fl));
		fl									();
	}
}

bool CActorCondition::DisableSprint(SHit* pHDS)
{
	return	(pHDS->hit_type != ALife::eHitTypeTelepatic)	&& 
			(pHDS->hit_type != ALife::eHitTypeChemicalBurn)	&&
			(pHDS->hit_type != ALife::eHitTypeBurn)			&&
			(pHDS->hit_type != ALife::eHitTypeLightBurn)	&&
			(pHDS->hit_type != ALife::eHitTypeRadiation)	;
}

bool CActorCondition::PlayHitSound(SHit* pHDS)
{
	switch (pHDS->hit_type)
	{
		case ALife::eHitTypeTelepatic:
			return false;
			break;
		case ALife::eHitTypeShock:
		case ALife::eHitTypeStrike:
		case ALife::eHitTypeWound:
		case ALife::eHitTypeExplosion:
		case ALife::eHitTypeFireWound:
		case ALife::eHitTypeWound_2:
		case ALife::eHitTypePhysicStrike:
			return true;
			break;

		case ALife::eHitTypeRadiation:
		case ALife::eHitTypeBurn:
		case ALife::eHitTypeLightBurn:
		case ALife::eHitTypeChemicalBurn:
			return (pHDS->damage()>0.017f); //field zone threshold
			break;
		default:
			return true;
	}
}

float CActorCondition::HitSlowmo(SHit* pHDS)
{
	float ret = 0.0f;
	if (!psActorFlags.test(AF_HIT_SLOWMO) || pHDS->hit_type != ALife::eHitTypeWound && pHDS->hit_type != ALife::eHitTypeStrike)
	{
		return ret;
	}

	ret = pHDS->damage();
	clamp(ret, 0.0f, 1.f);
	return ret;
}

bool CActorCondition::ApplyInfluence(const SMedicineInfluenceValues& V, const shared_str& sect, bool use_sound)
{
	if(m_curr_medicine_influence.InProcess())
		return false;

	if (m_object->Local() && m_object == Level().CurrentViewEntity())
	{
		if (use_sound && pSettings->line_exist(sect, "use_sound"))
		{
			if(m_use_sound.is_playing())
				m_use_sound.stop		();

			shared_str snd_name			= pSettings->r_string(sect, "use_sound");
			m_use_sound.create			(snd_name.c_str(), st_Effect, sg_SourceType);
			m_use_sound.play			(nullptr, sm_2D);
		}
	}

	SMedicineInfluenceValues Scaled = V;
	if (Scaled.fHealth > 0.0f)
	{
		Scaled.fHealth *= GetMedicineEfficiencyFactor(sect);
	}

	// Hydrating drinks flush toxins when thirst system is active.
	const static bool enableThirst = EngineExternal()[EEngineExternalGame::EnableThirst];
	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (enableThirst && enableMedIntoxication && Scaled.fThirst > 0.0f && m_fIntoxicationThirstK > 0.0f)
	{
		Scaled.fIntoxication -= Scaled.fThirst * m_fIntoxicationThirstK;
	}

	if(Scaled.fTimeTotal<0.0f)
		return inherited::ApplyInfluence	(Scaled, sect, use_sound);

	// Timed influence path does not drain medicine values yet; apply intoxication on use.
	ChangeIntoxication(Scaled.fIntoxication);
	Scaled.fIntoxication = 0.0f;

	m_curr_medicine_influence				= Scaled;
	m_curr_medicine_influence.fTimeCurrent  = m_curr_medicine_influence.fTimeTotal;
	return true;
}

bool CActorCondition::ApplyBooster(const SBooster& B, const shared_str& sect, bool use_sound)
{
	SBooster Scaled = B;
	if (Scaled.m_type == eBoostHpRestore && Scaled.fBoostValue > 0.0f)
	{
		Scaled.fBoostValue *= GetMedicineEfficiencyFactor(sect);
	}

	if (Scaled.fBoostValue > 0.0f)
	{
		if (m_object->Local() && m_object == Level().CurrentViewEntity())
		{
			if (use_sound && pSettings->line_exist(sect, "use_sound"))
			{
				if (m_use_sound.is_playing())
				{
					m_use_sound.stop();
				}

				shared_str snd_name = pSettings->r_string(sect, "use_sound");
				m_use_sound.create(snd_name.c_str(), st_Effect, sg_SourceType);
				m_use_sound.play(nullptr, sm_2D);
			}
		}

		SBooster& this_booster = m_booster_influences[Scaled.m_type];

		if (this_booster.fBoostValue * this_booster.fBoostTime > Scaled.fBoostValue * Scaled.fBoostTime)
		{
			return true;
		}

		this_booster.fBoostTime = 0.0f;
		this_booster.fBoostValue = 0.0f;
		DisableBoostParameters(this_booster);

		this_booster = Scaled;
		BoostParameters(Scaled);
	}

	return true;
}

void disable_input();
void enable_input();
void hide_indicators();
void show_indicators();

CActorDeathEffector::CActorDeathEffector	(CActorCondition* parent, const char* sect)	// -((
:m_pParent(parent)
{
	Actor()->SetWeaponHideState(INV_STATE_BLOCK_ALL,true);
	hide_indicators			();

	AddEffector				(Actor(), effActorDeath, sect);
	disable_input			();
	const char* snd				= pSettings->r_string(sect, "snd");
	m_death_sound.create	(snd,st_Effect,0);
	m_death_sound.play_at_pos(0,Fvector().set(0,0,0),sm_2D);


	SBaseEffector* pe		= Actor()->Cameras().GetPPEffector((EEffectorPPType)effActorDeath);
	pe->m_on_b_remove_callback = SBaseEffector::CB_ON_B_REMOVE(this, &CActorDeathEffector::OnPPEffectorReleased);
	m_b_actual				= true;	
	m_start_health			= m_pParent->health();
}

CActorDeathEffector::~CActorDeathEffector() = default;

void CActorDeathEffector::UpdateCL()
{
	m_pParent->SetHealth( m_start_health );
}

void CActorDeathEffector::OnPPEffectorReleased()
{
	m_b_actual				= false;	
	//m_pParent->health()		= -1.0f;
	m_pParent->SetHealth		(-1.0f);
}

void CActorDeathEffector::Stop()
{
	RemoveEffector			(Actor(),effActorDeath);
	m_death_sound.destroy	();
	enable_input			();
	show_indicators			();
}

float CActorCondition::GetHealthBoost()
{
	float total = 0.0f;

	float satiety_health_koef = (Satiety.Current - Satiety.Critical) / (Satiety.Current >= Satiety.Critical ? 1 - Satiety.Critical : Satiety.Critical);
	total += Satiety.HealthBoost * satiety_health_koef;

	const static bool enableThirst = EngineExternal()[EEngineExternalGame::EnableThirst];
	if (enableThirst)
	{
		float thirst_health_koef = (Thirst.Current - Thirst.Critical) / (Thirst.Current >= Thirst.Critical ? 1 - Thirst.Critical : Thirst.Critical);
		total += Thirst.HealthBoost * thirst_health_koef;
	}

	const static bool enableSleepiness = EngineExternal()[EEngineExternalGame::EnableSleepiness];
	if (enableSleepiness)
	{
		float SleepinessHealthKoef = ((1.f - Sleepiness.Current) - Sleepiness.Critical) / (Sleepiness.Current < Sleepiness.Critical ? 1 - Sleepiness.Critical : Sleepiness.Critical);
		total += Sleepiness.HealthBoost * SleepinessHealthKoef;
	}

	const static bool enableMedIntoxication = EngineExternal()[EEngineExternalGame::EnableMedIntoxication];
	if (enableMedIntoxication)
	{
		const float denom = std::max(EPS, 1.0f - Intoxication.Critical);
		const float excess = (Intoxication.Current - Intoxication.Critical) / denom;

		// HP drains past critical; stronger past heavy / critical overdose tiers
		float healthMul = 1.0f;
		if (Intoxication.Current >= 0.7f)
		{
			healthMul = 1.5f;
		}
		if (Intoxication.Current > 0.9f)
		{
			healthMul = 2.5f;
		}

		total += Intoxication.HealthBoost * excess * healthMul;
	}

	total += (m_change_v.m_fV_HealthRestore + m_fBoostHpRestore);

	for (const PIItem item : object().inventory().m_belt)
	{
		if (CArtefact* artefact = item->cast_artefact())
		{
			float art_cond = artefact->GetCondition();
			total += (artefact->m_fHealthRestoreSpeed * art_cond);
		}
	}

	if (CCustomOutfit* outfit = object().GetOutfit())
	{
		total += outfit->m_fHealthRestoreSpeed;
	}
	if (CHelmet* helmet = object().GetHelmet())
	{
		total += helmet->m_fHealthRestoreSpeed;
	}

	return total;
}

void CActorCondition::SetActorSleepiness(const float value)
{
	Sleepiness.Current = value;
	clamp(Sleepiness.Current, 0.0f, 1.0f);
}

void CActorCondition::SetActorSatiety(const float value)
{
	Satiety.Current = value;
	clamp(Satiety.Current, 0.0f, 1.0f);
}

void CActorCondition::SetActorThirst(const float value)
{
	Thirst.Current = value;
	clamp(Thirst.Current, 0.0f, 1.0f);
}

void CActorCondition::SetActorRadiation(const float value)
{
	m_fRadiation = value;
	clamp(m_fRadiation, 0.0f, m_fRadiationMax);
}

void CActorCondition::SetActorPsyHealth(const float value)
{
	m_fPsyHealth = value;
	clamp(m_fPsyHealth, 0.0f, m_fPsyHealthMax);
}

void CActorCondition::SetActorMorale(const float value)
{
	m_fEntityMorale = value;
	clamp(m_fEntityMorale, 0.0f, m_fEntityMoraleMax);
}

namespace
{
	constexpr int BodyArmEffector = 9921;
	constexpr int BodyConcussionEffector = 99123;
	constexpr int BodyAfterConcussionEffector = 99133;
	constexpr const char* BodyRegionKeys[6] = {"head", "torso", "leftarm", "rightarm", "leftleg", "rightleg"};
}

void CActorCondition::LoadBodyHealth()
{
	constexpr const char* Section = "body_health_system";
	BodyHealthActive = pSettings->section_exist(Section) && READ_IF_EXISTS(pSettings, r_bool, Section, "enabled", true);
	if (!pSettings->section_exist(Section)) { return; }
	BodyPenalties = READ_IF_EXISTS(pSettings, r_bool, Section, "penalties", true);
	BodyHud = READ_IF_EXISTS(pSettings, r_bool, Section, "show_hud", true);
	BodyArmPower = READ_IF_EXISTS(pSettings, r_float, Section, "arm_animation_power", 1.f);
	BodyHeadPower = READ_IF_EXISTS(pSettings, r_float, Section, "head_animation_power", 1.f);
	BodyArmThreshold = READ_IF_EXISTS(pSettings, r_float, Section, "arm_penalty_minimum_hp", 10.f);
	BodyLegPower = READ_IF_EXISTS(pSettings, r_float, Section, "leg_animation_power", 1.f);
	BodyLegThreshold = READ_IF_EXISTS(pSettings, r_float, Section, "leg_penalty_minimum_hp", 5.f);
	BodyLegDamageThreshold = READ_IF_EXISTS(pSettings, r_float, Section, "leg_limping_damage_minimum_hp", 3.f);
	BodyStepDamageChance = READ_IF_EXISTS(pSettings, r_float, Section, "chance_damage_on_footstep", 13.f);
	BodyHealth.Settings.DamageScale = READ_IF_EXISTS(pSettings, r_float, Section, "damage_scale", 0.85f);
	BodyHealth.Settings.MinimumDamage = READ_IF_EXISTS(pSettings, r_float, Section, "minimum_damage", 0.0293f);
	BodyHealth.Settings.HeadDamageCap = READ_IF_EXISTS(pSettings, r_float, Section, "head_damage_cap", 7.f);
	BodyHealth.Settings.HeadPenaltyThreshold = READ_IF_EXISTS(pSettings, r_float, Section, "head_penalty_minimum_hp", 2.f);
	BodyHealth.Settings.ConcussionRecovery = READ_IF_EXISTS(pSettings, r_float, Section, "concussion_recovery", 0.02f);
	for (u32 I = 0; I < 6; ++I)
	{
		string64 Key;
		xr_sprintf(Key, "max_%s", BodyRegionKeys[I]);
		const float Maximum = READ_IF_EXISTS(pSettings, r_float, Section, Key, BodyHealth.Settings.Maximum[I]);
		if (!std::isfinite(Maximum) || Maximum <= 0.f)
		{
			CHECK_OR_EXIT(false, "Body Health System: invalid regional maximum health");
		}
		BodyHealth.Settings.Maximum[I] = Maximum;
	}
	BodyHealth.Reset();
	if (!g_dedicated_server) { BodyTinnitus.create("bhs\\tinnitusx", st_Effect, 0); }
	for (const auto& Entry : pSettings->r_section("body_health_treatments").Data)
	{
		if (!pSettings->section_exist(Entry.first))
		{
			Msg("! [Body Health System] medicine resource gate OPEN: actual item section [%s] absent; specification is dormant", Entry.first.c_str());
		}
	}
	Msg("[Body Health System] native regional owner enabled; donor=479; damage_scale=%.3f", BodyHealth.Settings.DamageScale);
}

EBodyRegion CActorCondition::ResolveBodyRegion(const SHit& Hit) const
{
	EBodyRegion Region = EBodyRegion::Invalid;
	if (Hit.boneID == BI_NONE)
	{
		const int Choice = ::Random.randI(5);
		Region = Choice < 3 ? EBodyRegion::Torso : (Choice == 3 ? EBodyRegion::LeftArm : EBodyRegion::RightArm);
	}
	else
	{
		IKinematics* K = object().Visual() ? object().Visual()->dcast_PKinematics() : nullptr;
		if (K && Hit.boneID < K->LL_BoneCount())
		{
			// Additional fingers and forearm twist inherit the nearest mapped anatomical parent.
			u16 Bone = Hit.boneID;
			for (u16 Depth = 0; Depth < K->LL_BoneCount() && Bone < K->LL_BoneCount(); ++Depth)
			{
				for (u32 I = 0; I < 6; ++I)
				{
					const char* Bones = pSettings->r_string("body_health_bones", BodyRegionKeys[I]);
					const u32 Count = _GetItemCount(Bones);
					for (u32 J = 0; J < Count; ++J)
					{
						string128 Name;
						_GetItem(Bones, J, Name);
						if (K->LL_BoneID(Name) == Bone) { return static_cast<EBodyRegion>(I); }
					}
				}
				const u16 Parent = K->LL_GetData(Bone).GetParentID();
				if (Parent == Bone) { break; }
				Bone = Parent;
			}
			// A known structural bone without a mapped ancestor is deterministic, never a random limb.
			Msg("! [Body Health System] unmapped anatomical chain bone=%u visual=%s; using torso", Hit.boneID, object().cNameVisual().c_str());
			return EBodyRegion::Torso;
		}
	}
	if (Region == EBodyRegion::Invalid)
	{
		Msg("! [Body Health System] unmapped bone=%u visual=%s; applying donor unknown-bone rule", Hit.boneID, object().cNameVisual().c_str());
		const int Choice = ::Random.randI(3);
		Region = Choice == 0 ? EBodyRegion::RightArm : (Choice == 1 ? EBodyRegion::LeftArm : EBodyRegion::Torso);
		return Region;
	}
	if ((Region == EBodyRegion::LeftArm || Region == EBodyRegion::RightArm) && ::Random.randI(1, 101) >= 92)
	{
		Region = EBodyRegion::Torso;
	}
	else if ((Region == EBodyRegion::LeftLeg || Region == EBodyRegion::RightLeg) && ::Random.randI(1, 101) <= 25)
	{
		const int Choice = ::Random.randI(3);
		Region = Choice == 0 ? EBodyRegion::Torso : (Choice == 1 ? EBodyRegion::LeftArm : EBodyRegion::RightArm);
	}
	return Region;
}

void CActorCondition::OnAcceptedHit(const SHit& Hit, float HealthLost)
{
	inherited::OnAcceptedHit(Hit, HealthLost);
	if (!BodyHealthEnabled() || GodMode() || HealthLost <= 0.f) { return; }
	const EBodyRegion Region = ResolveBodyRegion(Hit);
	const float Lost = BodyHealth.ApplyDamage(Region, HealthLost);
	if (Region == EBodyRegion::Head && Lost >= BodyHealth.Settings.HeadPenaltyThreshold) { BodyConcussionSoundPlayed = false; }
	if (BodyHealth.Fatal())
	{
		// Native scalar damage was already applied once; regional fatality adds no second hit.
		SetHealth(0.f);
	}
}

void CActorCondition::UpdateBodyHealth()
{
	if (!BodyHealthEnabled() || !object().g_Alive())
	{
		ClearBodyHealthEffects();
		return;
	}
	const float Factor = IsGameTypeSingle() ? Level().GetGameTimeFactor() : 1.f;
	const float Seconds = Factor > 0.f ? m_fDeltaTime / Factor : 0.f;
	BodyHealth.Advance(Seconds);
	BodyStepPainRemaining = std::max(0.f, BodyStepPainRemaining - Seconds);
	if (BodyHealth.Fatal()) { SetHealth(0.f); }
	UpdateBodyHealthEffects(Seconds);
}

float CActorCondition::GetBodyHealth(int Region) const
{
	return Region >= 0 && Region < 6 ? BodyHealth.Health[Region] : 0.f;
}

float CActorCondition::GetBodyHealthMax(int Region) const
{
	return Region >= 0 && Region < 6 ? BodyHealth.Settings.Maximum[Region] : 0.f;
}

float CActorCondition::GetBodyHealthTemporary(int Region) const
{
	return Region >= 0 && Region < 6 ? BodyHealth.Temporary[Region] : 0.f;
}

float CActorCondition::GetBodyMovementFactor(bool Sprint) const
{
	return BodyHealthEnabled() && BodyPenalties ? BodyHealth.MovementFactor(Sprint) : 1.f;
}

float CActorCondition::GetBodyArmFactor()
{
	if (!BodyHealthEnabled() || !BodyPenalties || !object().g_Alive() || !object().IsZoomAimingMode()) { return 0.f; }
	PIItem Item = object().inventory().ActiveItem();
	CWeapon* Weapon = Item ? Item->cast_weapon() : nullptr;
	if (!Weapon) { return 0.f; }
	const float Weight = pSettings->r_float(Item->object().cNameSect(), "inv_weight");
	const float Sum = (BodyHealth.Health[2] >= 1.f ? BodyHealth.Health[2] : 0.75f) +
		(BodyHealth.Health[3] >= 1.f ? BodyHealth.Health[3] : 0.75f);
	const float Maximum = BodyHealth.Settings.Maximum[2] + BodyHealth.Settings.Maximum[3];
	const bool Wounded = Sum < Maximum * BodyArmThreshold * 0.1f;
	const bool Crouched = (object().mstate_real & mcCrouch) != 0;
	const bool LowCrouched = Crouched && (object().mstate_real & mcAccel) != 0;
	const float Coefficient = Wounded ? (LowCrouched ? 0.55f : (Crouched ? 0.60f : 0.65f)) :
		(LowCrouched ? 0.17f : (Crouched ? 0.18f : 0.19f));
	float Power = std::clamp(Coefficient * Weight / (Wounded ? Sum : Maximum), 0.f, 3.f) * BodyArmPower;
	if (Weapon->IsScopeAttached()) { Power *= 0.33f; }
	return Power;
}

float CActorCondition::GetBodyConcussionFactor()
{
	return BodyHealthEnabled() && BodyPenalties ? BodyHealth.Concussion / BodyHealth.Settings.Maximum[0] * BodyHeadPower : 0.f;
}

void CActorCondition::ClearBodyHealthEffects()
{
	if (g_dedicated_server || !m_object) { return; }
	RemoveEffector(m_object, BodyArmEffector);
	RemoveEffector(m_object, BodyConcussionEffector);
	RemoveEffector(m_object, BodyAfterConcussionEffector);
	RemoveEffector(m_object, 99251);
	RemoveEffector(m_object, 99261);
	RemoveEffector(m_object, 99271);
	RemoveEffector(m_object, 655808);
	BodyTinnitus.stop();
	BodyStepPain.stop();
	BodyConcussionRepeat = 0.f;
}

void CActorCondition::UpdateBodyHealthEffects(float Seconds)
{
	if (g_dedicated_server || !BodyPenalties || !object().g_Alive()) { ClearBodyHealthEffects(); return; }
	const float ArmFactor = GetBodyArmFactor();
	if (ArmFactor > 0.f && !object().Cameras().GetCamEffector((ECamEffectorType)BodyArmEffector))
	{
		AddEffector(m_object, BodyArmEffector, "body_health_arm_effect", GET_KOEFF_FUNC(this, &CActorCondition::GetBodyArmFactor));
	}
	else if (ArmFactor <= 0.f) { RemoveEffector(m_object, BodyArmEffector); }
	const float ConcussionFactor = GetBodyConcussionFactor();
	if (ConcussionFactor <= 0.f)
	{
		RemoveEffector(m_object, BodyConcussionEffector);
		RemoveEffector(m_object, BodyAfterConcussionEffector);
		BodyConcussionRepeat = 0.f;
		return;
	}
	if (BodyHealth.ConcussionAge < 1.f)
	{
		if (!object().Cameras().GetPPEffector((EEffectorPPType)BodyConcussionEffector))
		{
			AddEffector(m_object, BodyConcussionEffector, "body_health_concussion_effect", GET_KOEFF_FUNC(this, &CActorCondition::GetBodyConcussionFactor));
		}
		BodyConcussionRepeat = 0.f;
	}
	else
	{
		if (!BodyConcussionSoundPlayed)
		{
			BodyTinnitus.play(nullptr, sm_2D);
			BodyTinnitus.set_volume(ConcussionFactor * 3.f);
			BodyConcussionSoundPlayed = true;
		}
		BodyConcussionRepeat -= Seconds;
		if (BodyConcussionRepeat <= 0.f)
		{
			AddEffector(m_object, BodyAfterConcussionEffector, "body_health_after_concussion_effect", GET_KOEFF_FUNC(this, &CActorCondition::GetBodyConcussionFactor));
			BodyConcussionRepeat = 15.f;
		}
	}
}

bool CActorCondition::GetBodyTreatment(const shared_str& Section, SBodyTreatment& Treatment) const
{
	if (!BodyHealthEnabled()) { return false; }
	string256 TreatmentSection;
	xr_sprintf(TreatmentSection, "body_health_treatment_%s", Section.c_str());
	if (!pSettings->section_exist(TreatmentSection)) { return false; }
	for (u32 I = 0; I < 6; ++I)
	{
		string64 Key;
		xr_sprintf(Key, "temporary_%s", BodyRegionKeys[I]);
		Treatment.Temporary[I] = READ_IF_EXISTS(pSettings, r_float, TreatmentSection, Key, 0.f);
		xr_sprintf(Key, "stabilize_%s", BodyRegionKeys[I]);
		Treatment.Stabilize[I] = READ_IF_EXISTS(pSettings, r_float, TreatmentSection, Key, 0.f);
	}
	Treatment.Interval = READ_IF_EXISTS(pSettings, r_float, TreatmentSection, "interval", 60.f);
	return true;
}

bool CActorCondition::CanTreatBodyRegion(const char* Section, int Region) const
{
	SBodyTreatment Treatment;
	if (!GetBodyTreatment(Section, Treatment) || Region < 0 || Region > 6 || !object().g_Alive()) { return false; }
	CBodyHealthState Preview = BodyHealth;
	return Preview.Treat(static_cast<EBodyRegion>(Region), Treatment);
}

bool CActorCondition::SetBodyTreatmentTarget(ALife::_OBJECT_ID ItemId, int Region)
{
	if (!BodyHealthEnabled() || Region < 0 || Region > 6 || !object().g_Alive() || object().HudAnimator()->IsAnyAnimatorActive()) { return false; }
	CGameObject* Item = smart_cast<CGameObject*>(Level().Objects.net_Find(ItemId));
	if (!Item || Item->H_Parent() != m_object || !CanTreatBodyRegion(Item->cNameSect().c_str(), Region)) { return false; }
	BodyTreatmentItem = ItemId;
	BodyTreatmentTarget = static_cast<EBodyRegion>(Region);
	return true;
}

EBodyRegion CActorCondition::TakeBodyTreatmentTarget(ALife::_OBJECT_ID ItemId)
{
	const EBodyRegion Target = ItemId == BodyTreatmentItem ? BodyTreatmentTarget : EBodyRegion::All;
	BodyTreatmentItem = ALife::INVALID_OBJECT_ID;
	BodyTreatmentTarget = EBodyRegion::All;
	return Target;
}

void CActorCondition::CommitBodyTreatment(const shared_str& Section, EBodyRegion Region)
{
	SBodyTreatment Treatment;
	if (object().g_Alive() && GetBodyTreatment(Section, Treatment)) { BodyHealth.Treat(Region, Treatment); }
}

void CActorCondition::SaveBodyHealthExtension(NET_Packet& Packet) const
{
	Packet.w_u32(0x31534842); // BHS1, native actor client-data extension.
	Packet.w_u16(1);
	Packet.w_u16(16 * sizeof(float));
	for (u32 I = 0; I < 6; ++I)
	{
		Packet.w_float(BodyHealth.Health[I]);
		Packet.w_float(BodyHealth.Temporary[I]);
	}
	Packet.w_float(BodyHealth.DecayRemaining);
	Packet.w_float(BodyHealth.DecayInterval);
	Packet.w_float(BodyHealth.Concussion);
	Packet.w_float(BodyHealth.ConcussionAge);
}

void CActorCondition::LoadBodyHealthExtension(IReader& Reader)
{
	const auto Position = Reader.tell();
	const auto Remaining = Reader.length() - Position;
	if (Remaining < sizeof(u32)) { return; }
	if (Reader.r_u32() != 0x31534842)
	{
		Reader.seek(Position);
		return;
	}
	CHECK_OR_EXIT(Remaining >= 8, "Body Health System: truncated actor extension header");
	const u16 Version = Reader.r_u16();
	const u16 PayloadSize = Reader.r_u16();
	CHECK_OR_EXIT(Version == 1, "Body Health System: unsupported actor extension version");
	CHECK_OR_EXIT(PayloadSize == 16 * sizeof(float), "Body Health System: invalid actor extension size");
	CHECK_OR_EXIT(Remaining - 8 >= PayloadSize, "Body Health System: truncated actor extension payload");
	for (u32 I = 0; I < 6; ++I)
	{
		BodyHealth.Health[I] = Reader.r_float();
		BodyHealth.Temporary[I] = Reader.r_float();
	}
	BodyHealth.DecayRemaining = Reader.r_float();
	BodyHealth.DecayInterval = Reader.r_float();
	BodyHealth.Concussion = Reader.r_float();
	BodyHealth.ConcussionAge = Reader.r_float();
	BodyHealth.Validate();
}

void CActorCondition::Serialize(ISaveObject& Object)
{
	inherited::Serialize(Object);
	if (!Object.IsSave())
	{
		BodyHealth.Reset();
		BodyTreatmentItem = ALife::INVALID_OBJECT_ID;
		BodyTreatmentTarget = EBodyRegion::All;
		ClearBodyHealthEffects();
	}
	BEGIN_CHUNK(Object, "BodyHealthSystem_v1")
	{
		u32 Version = 1;
		Object << Version;
		CHECK_OR_EXIT(Version == 1, "Body Health System: unsupported regional save version");
		for (u32 I = 0; I < 6; ++I) { Object << BodyHealth.Health[I] << BodyHealth.Temporary[I]; }
		Object << BodyHealth.DecayRemaining << BodyHealth.DecayInterval << BodyHealth.Concussion << BodyHealth.ConcussionAge;
	}
	if (!Object.IsSave()) { BodyHealth.Validate(); }
}

void CActorCondition::BodyFootstep()
{
	if (!BodyHealthEnabled() || !BodyPenalties || !object().g_Alive() || GodMode()) { return; }
	const float Left = BodyHealth.Health[4] >= 1.f ? BodyHealth.Health[4] : 0.8f;
	const float Right = BodyHealth.Health[5] >= 1.f ? BodyHealth.Health[5] : 0.8f;
	const bool Sprint = (object().mstate_real & mcSprint) != 0;
	const bool Both = Left == Right && Left + Right < BodyLegThreshold * 2.f;
	const bool InjuredLeft = Left < BodyLegThreshold && Left < Right;
	const bool InjuredRight = Right < BodyLegThreshold && Right < Left;
	const bool AnimationActive = object().Cameras().GetCamEffector((ECamEffectorType)99251) ||
		object().Cameras().GetCamEffector((ECamEffectorType)99261) || object().Cameras().GetCamEffector((ECamEffectorType)99271);
	if (!g_dedicated_server && !AnimationActive && (Both || !Sprint && (InjuredLeft || InjuredRight)))
	{
		const float LeftPower = std::exp(1.5f / Left) - 1.f;
		const float RightPower = std::exp(1.5f / Right) - 1.f;
		BodyLimpPower = (Both ? (LeftPower + RightPower) * 0.5f : (InjuredLeft ? LeftPower : RightPower)) * BodyLegPower;
		string64 Effect;
		xr_sprintf(Effect, "body_health_limp_%s_%d", Both ? "both" : (InjuredLeft ? "left" : "right"), ::Random.randI(Both ? 3 : 2));
		AddEffector(m_object, Both ? 99271 : (InjuredLeft ? 99251 : 99261), Effect, GET_KOEFF_FUNC(this, &CActorCondition::GetBodyLimpFactor));
	}
	const float Damage = ((Left <= BodyLegDamageThreshold ? 0.029f : 0.f) + (Right <= BodyLegDamageThreshold ? 0.029f : 0.f)) * 0.5f;
	if (Sprint && Damage > 0.f && BodyStepPainRemaining <= 0.f && ::Random.randI(1, 101) <= BodyStepDamageChance)
	{
		ChangeHealth(-Damage);
		BodyStepPainRemaining = 2.5f;
		if (!g_dedicated_server)
		{
			string64 SoundName;
			xr_sprintf(SoundName, "bhs\\%spain_%d", object().GetHelmet() || object().GetOutfit() ? "m_" : "", ::Random.randI(1, 14));
			BodyStepPain.stop();
			BodyStepPain.create(SoundName, st_Effect, 0);
			BodyStepPain.play(nullptr, sm_2D);
			AddEffector(m_object, 655808, "body_health_step_pain_effect");
		}
	}
}

void CActorCondition::BodySleep(float Hours)
{
	if (!BodyHealthEnabled() || !object().g_Alive()) { return; }
	BodyHealth.Sleep(Hours);
	BodyHealth.Concussion = 0.f;
	ClearBodyHealthEffects();
}

bool CActorCondition::SetBodyHealthOption(const char* Key, float Value)
{
	if (!Key || !std::isfinite(Value)) { return false; }
	if (xr_strcmp(Key, "enabled") == 0)
	{
		if (Value != 0.f && !pSettings->section_exist("body_health_bones"))
		{
			Msg("! [Body Health System] cannot enable: owner configuration not loaded");
			return false;
		}
		BodyHealthActive = Value != 0.f;
	}
	else if (xr_strcmp(Key, "penalties") == 0) { BodyPenalties = Value != 0.f; }
	else if (xr_strcmp(Key, "show_hud") == 0) { BodyHud = Value != 0.f; }
	else if (xr_strcmp(Key, "damage_scale") == 0 && Value >= 0.1f && Value <= 2.f) { BodyHealth.Settings.DamageScale = Value; }
	else if (xr_strcmp(Key, "arm_animation_power") == 0 && Value >= 0.1f && Value <= 2.f) { BodyArmPower = Value; }
	else if (xr_strcmp(Key, "head_animation_power") == 0 && Value >= 0.1f && Value <= 2.f) { BodyHeadPower = Value; }
	else if (xr_strcmp(Key, "leg_animation_power") == 0 && Value >= 0.1f && Value <= 2.f) { BodyLegPower = Value; }
	else if (xr_strcmp(Key, "leg_penalty_minimum_hp") == 0 && Value >= 1.f && Value <= 5.f) { BodyLegThreshold = Value; }
	else if (xr_strcmp(Key, "leg_limping_damage_minimum_hp") == 0 && Value >= -1.f && Value <= 4.f) { BodyLegDamageThreshold = Value; }
	else if (xr_strcmp(Key, "chance_damage_on_footstep") == 0 && Value >= 1.f && Value <= 100.f) { BodyStepDamageChance = Value; }
	else if (xr_strcmp(Key, "arm_penalty_minimum_hp") == 0 && Value >= 1.f && Value <= 10.f) { BodyArmThreshold = Value; }
	else if (xr_strcmp(Key, "head_penalty_minimum_hp") == 0 && Value >= 1.f && Value <= 10.f) { BodyHealth.Settings.HeadPenaltyThreshold = Value; }
	else { return false; }
	if (!BodyHealthActive || !BodyPenalties) { ClearBodyHealthEffects(); }
	return true;
}

float CActorCondition::GetBodyHealthOption(const char* Key) const
{
	if (!Key) { return -1.f; }
	if (xr_strcmp(Key, "enabled") == 0) { return BodyHealthActive ? 1.f : 0.f; }
	if (xr_strcmp(Key, "penalties") == 0) { return BodyPenalties ? 1.f : 0.f; }
	if (xr_strcmp(Key, "show_hud") == 0) { return BodyHud ? 1.f : 0.f; }
	if (xr_strcmp(Key, "damage_scale") == 0) { return BodyHealth.Settings.DamageScale; }
	if (xr_strcmp(Key, "arm_animation_power") == 0) { return BodyArmPower; }
	if (xr_strcmp(Key, "head_animation_power") == 0) { return BodyHeadPower; }
	if (xr_strcmp(Key, "leg_animation_power") == 0) { return BodyLegPower; }
	if (xr_strcmp(Key, "leg_penalty_minimum_hp") == 0) { return BodyLegThreshold; }
	if (xr_strcmp(Key, "leg_limping_damage_minimum_hp") == 0) { return BodyLegDamageThreshold; }
	if (xr_strcmp(Key, "chance_damage_on_footstep") == 0) { return BodyStepDamageChance; }
	if (xr_strcmp(Key, "arm_penalty_minimum_hp") == 0) { return BodyArmThreshold; }
	if (xr_strcmp(Key, "head_penalty_minimum_hp") == 0) { return BodyHealth.Settings.HeadPenaltyThreshold; }
	return -1.f;
}
