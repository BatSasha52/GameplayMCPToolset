// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "GameplayPIEToolset.h"

/**
 * Toolsets are not discovered automatically: each UToolsetDefinition subclass must be
 * registered explicitly. The Unreal MCP plugin then exposes every registered toolset.
 */
class FGameplayMCPToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UGameplayPIEToolset::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		if (!UObjectInitialized())
		{
			return;
		}
		UToolsetRegistry::UnregisterToolsetClass(UGameplayPIEToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FGameplayMCPToolsetModule, GameplayMCPToolset);
