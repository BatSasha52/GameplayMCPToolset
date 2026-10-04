// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "GameplayBlueprintToolset.h"
#include "GameplayConsoleJobs.h"
#include "GameplayEditorToolset.h"
#include "GameplayInputJobs.h"
#include "GameplayInputToolset.h"
#include "GameplayLogCapture.h"
#include "GameplayPIEToolset.h"
#include "GameplayViewportToolset.h"

/**
 * Toolsets are not discovered automatically: each UToolsetDefinition subclass must be
 * registered explicitly. The Unreal MCP plugin then exposes every registered toolset.
 */
class FGameplayMCPToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGameplayLogCapture::Get().Startup();
		FGameplayInputJobs::Get().Startup();
		FGameplayConsoleJobs::Get().Startup();
		UToolsetRegistry::RegisterToolsetClass(UGameplayPIEToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UGameplayInputToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UGameplayEditorToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UGameplayBlueprintToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UGameplayViewportToolset::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		// Tickers, delegates and the log device are released even late in engine shutdown, so nothing
		// of this module is left registered (or holding memory) when static destructors run.
		GameplayScreenshots::Shutdown();
		FGameplayConsoleJobs::Get().Shutdown();
		GameplayLiveCoding::Shutdown();
		FGameplayInputJobs::Get().Shutdown();
		FGameplayLogCapture::Get().Shutdown();
		if (!UObjectInitialized())
		{
			return;
		}
		UToolsetRegistry::UnregisterToolsetClass(UGameplayViewportToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayBlueprintToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayEditorToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayInputToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayPIEToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FGameplayMCPToolsetModule, GameplayMCPToolset);
