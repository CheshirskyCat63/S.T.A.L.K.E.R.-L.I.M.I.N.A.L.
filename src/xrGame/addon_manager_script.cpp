#include "StdAfx.h"
#include "pch_script.h"
#include "addon_manager.h"
#include "../xrCore/xrAddons.h"

using namespace luabind;
#pragma optimize("s",on)

const char* get_addon_name_script(CAddonManager::AddonInfo info)
{
	return info.AddonName.c_str();
}

const char* get_addon_script_init(CAddonManager::AddonInfo info)
{
	return info.ScriptInit.c_str();
}

const char* get_addon_entry_script(CAddonManager::AddonInfo info)
{
	return info.EntryDir.c_str();
}

namespace
{
	int LoadedAddons(lua_State* State)
	{
		lua_newtable(State);
		if (GAddonsManager)
		{
			int Index = 1;
			for (const auto& Addon : GAddonsManager->Addons)
			{
				lua_newtable(State);
				lua_pushstring(State, Addon.AddonName.c_str());
				lua_setfield(State, -2, "name");
				lua_rawseti(State, -2, Index++);
			}
		}
		return 1;
	}

	int AddonResourceOwner(lua_State* State)
	{
		const char* Alias = luaL_checkstring(State, 1);
		const char* Name = luaL_checkstring(State, 2);
		const CLocatorAPI::file* File = FS.exist(Alias, Name);
		const CAddonManager::AddonInfo* Addon = File && GAddonsManager ? GAddonsManager->FindAddon(*File) : nullptr;
		if (Addon)
		{
			lua_pushstring(State, Addon->AddonName.c_str());
		}
		else
		{
			lua_pushnil(State);
		}
		return 1;
	}
}

void addon_manager::script_register(lua_State *L)
{
	module(L)
	[
		class_<CAddonManager>("CAddonManager")
			.def_readonly("Addons",					&CAddonManager::Addons)
			.def("AddonName",						&get_addon_name_script)
			.def("ScriptInit",						&get_addon_script_init)
			.def("EntryDir",						&get_addon_entry_script)
	];
	const luaL_Reg Functions[] = {{"loaded", &LoadedAddons}, {"owner", &AddonResourceOwner}, {nullptr, nullptr}};
	luaL_register(L, "addons", Functions);
	lua_pop(L, 1);
}
