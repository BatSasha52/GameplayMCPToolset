// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "GameplayInputJobs.h"
#include "GameplayInputToolset.h"
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
		FGameplayInputJobs::Get().Startup();
		UToolsetRegistry::RegisterToolsetClass(UGameplayPIEToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UGameplayInputToolset::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		if (!UObjectInitialized())
		{
			return;
		}
		FGameplayInputJobs::Get().Shutdown();
		UToolsetRegistry::UnregisterToolsetClass(UGameplayInputToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayPIEToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FGameplayMCPToolsetModule, GameplayMCPToolset);
