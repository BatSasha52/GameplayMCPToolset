// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "GameplayMCPTypes.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGameplayMCP, Log, All);

/** Every tool must run on the game thread; bail out with a clean error otherwise. */
#define GAMEPLAYMCP_REQUIRE_GAME_THREAD() \
	if (!IsInGameThread()) \
	{ \
		return GameplayMCP::Fail(TEXT("GameplayMCPToolset tools must run on the game thread.")); \
	}

namespace GameplayMCP
{
	// ---- Result envelope -------------------------------------------------------------------

	FGameplayMCPResult Ok(const TSharedRef<FJsonObject>& Payload);
	FGameplayMCPResult Fail(const FString& Message);

	/** True for "", "*" and "none": the values optional string params use to mean "not set". */
	bool IsUnset(const FString& Value);

	// ---- Serialization ---------------------------------------------------------------------

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<TSharedRef<FJsonObject>>& Objects);
	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings);
}
