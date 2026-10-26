#pragma once
namespace BodycamRender
{
	bool Enabled();
	bool MaskOverride();
	Fvector4 Overlay();
	Fvector4 Image();
	Fvector4 Grade();
	void RegisterCommands();
}
