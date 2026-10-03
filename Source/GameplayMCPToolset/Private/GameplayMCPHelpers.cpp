// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayMCPHelpers.h"

DEFINE_LOG_CATEGORY(LogGameplayMCP);

namespace GameplayMCP
{
	FGameplayMCPResult Ok(const TSharedRef<FJsonObject>& Payload)
	{
		FGameplayMCPResult Out;
		Out.Success = true;
		Out.Result.JsonObject = Payload;
		return Out;
	}

	FGameplayMCPResult Fail(const FString& Message)
	{
		FGameplayMCPResult Out;
		Out.Success = false;
		Out.Result.JsonObject = MakeShared<FJsonObject>();
		Out.Error = Message;
		UE_LOG(LogGameplayMCP, Verbose, TEXT("Tool failed: %s"), *Message);
		return Out;
	}

	bool IsUnset(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		return Trimmed.IsEmpty() || Trimmed == TEXT("*") || Trimmed.Equals(TEXT("none"), ESearchCase::IgnoreCase);
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<TSharedRef<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Objects.Num());
		for (const TSharedRef<FJsonObject>& Object : Objects)
		{
			Out.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Out.Add(MakeShared<FJsonValueString>(String));
		}
		return Out;
	}
}
