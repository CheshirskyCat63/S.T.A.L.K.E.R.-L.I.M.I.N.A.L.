#include "stdafx.h"
#include "BodycamRender.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include <cmath>

namespace BodycamRender
{
namespace
{
	xrCriticalSection ParamLock;
	bool EffectEnabled = false;
	bool MaskOverrideValue = true;
	Fvector4 OverlayValue = {0.f,0.f,0.f,0.5f};
	Fvector4 ImageValue = {0.12f,0.01f,0.f,0.7f};
	Fvector4 GradeValue = {0.f,0.f,0.f,0.f};
	enum class ParameterKind { Overlay, Image, Grade };
	class CCC_BodycamEnable final : public IConsole_Command
	{
	public:
		explicit CCC_BodycamEnable(const char* Name) : IConsole_Command(Name) {}
		void Execute(const char* Args) override
		{
			const bool On = !xr_strcmp(Args,"1") || !xr_strcmp(Args,"on");
			const bool Off = !xr_strcmp(Args,"0") || !xr_strcmp(Args,"off");
			if (!On && !Off) { InvalidSyntax(); return; }
			xrCriticalSectionGuard Guard(ParamLock);
			if (!xr_strcmp(cName,"r_bodycam_mask_override"))
			{
				MaskOverrideValue = On;
			}
			else
			{
				EffectEnabled = On;
				if (!On)
				{
					GradeValue = {0.f,0.f,0.f,0.f};
				}
			}
		}
		void Status(TStatus& S) override { xrCriticalSectionGuard Guard(ParamLock); xr_strcpy(S, (!xr_strcmp(cName,"r_bodycam_mask_override") ? MaskOverrideValue : EffectEnabled) ? "on" : "off"); }
		void Save(IWriter*) override {}
	};
	class CCC_BodycamVector final : public IConsole_Command
	{
		ParameterKind Kind;
	public:
		CCC_BodycamVector(const char* Name, ParameterKind ValueKind) : IConsole_Command(Name), Kind(ValueKind) {}
		void Execute(const char* Args) override
		{
			Fvector4 Value;
			if (sscanf(Args,"%f,%f,%f,%f",&Value.x,&Value.y,&Value.z,&Value.w)!=4 || !std::isfinite(Value.x) || !std::isfinite(Value.y) || !std::isfinite(Value.z) || !std::isfinite(Value.w)) { InvalidSyntax(); return; }
			if (Kind == ParameterKind::Grade)
			{
				Value.x = std::clamp(Value.x, 0.f, 1.f);
				Value.y = Value.z = Value.w = 0.f;
			}
			else
			{
				const bool IsImage = Kind == ParameterKind::Image;
				Value.x=std::clamp(Value.x,IsImage ? 0.f : -0.5f,0.5f);
				Value.y=std::clamp(Value.y,IsImage ? 0.f : -0.5f,IsImage ? 1.f : 0.5f);
				Value.z=std::clamp(Value.z,0.f,1.f); Value.w=std::clamp(Value.w,0.01f,1.2f);
			}
			xrCriticalSectionGuard Guard(ParamLock);
			(Kind == ParameterKind::Grade ? GradeValue : Kind == ParameterKind::Image ? ImageValue : OverlayValue) = Value;
		}
		void Status(TStatus& S) override
		{
			xrCriticalSectionGuard Guard(ParamLock);
			const Fvector4& V = Kind == ParameterKind::Grade ? GradeValue : Kind == ParameterKind::Image ? ImageValue : OverlayValue;
			xr_sprintf(S,"%.6f,%.6f,%.6f,%.6f",V.x,V.y,V.z,V.w);
		}
		void Save(IWriter*) override {}
	};
}
bool Enabled() { xrCriticalSectionGuard Guard(ParamLock); return EffectEnabled; }
bool MaskOverride() { xrCriticalSectionGuard Guard(ParamLock); return EffectEnabled && MaskOverrideValue; }
Fvector4 Overlay() { xrCriticalSectionGuard Guard(ParamLock); return OverlayValue; }
Fvector4 Image() { xrCriticalSectionGuard Guard(ParamLock); return ImageValue; }
Fvector4 Grade() { xrCriticalSectionGuard Guard(ParamLock); return GradeValue; }
void RegisterCommands()
{
	Console->AddCommand(new CCC_BodycamEnable("r_bodycam_enable"));
	Console->AddCommand(new CCC_BodycamEnable("r_bodycam_mask_override"));
	Console->AddCommand(new CCC_BodycamVector("r_bodycam_overlay",ParameterKind::Overlay));
	Console->AddCommand(new CCC_BodycamVector("r_bodycam_image",ParameterKind::Image));
	Console->AddCommand(new CCC_BodycamVector("r_bodycam_grade",ParameterKind::Grade));
}
}
