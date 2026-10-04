// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayInputToolset.h"

#include "GameplayInputJobs.h"
#include "GameplayMCPHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "InputAction.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using FStep = FGameplayInputJobs::FStep;
	using EStepKind = FGameplayInputJobs::EStepKind;

	FString ValueTypeName(EInputActionValueType Type)
	{
		switch (Type)
		{
		case EInputActionValueType::Boolean: return TEXT("boolean");
		case EInputActionValueType::Axis1D:  return TEXT("axis1d");
		case EInputActionValueType::Axis2D:  return TEXT("axis2d");
		default:                             return TEXT("axis3d");
		}
	}

	int32 ValueDimensions(EInputActionValueType Type)
	{
		switch (Type)
		{
		case EInputActionValueType::Axis2D: return 2;
		case EInputActionValueType::Axis3D: return 3;
		default:                            return 1;
		}
	}

	TArray<FAssetData> AllInputActions()
	{
		TArray<FAssetData> Assets;
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);
		return Assets;
	}

	const UInputAction* ResolveAction(const FString& InName, FString& OutPath, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("No input action given. Pass an asset path such as '/Game/Input/IA_Jump' or a unique asset name such as 'IA_Jump' (input_list_actions lists them).");
			return nullptr;
		}

		if (Name.StartsWith(TEXT("/")))
		{
			FString ObjectPath = Name;
			if (!ObjectPath.Contains(TEXT(".")))
			{
				ObjectPath += TEXT(".") + FPackageName::GetShortName(Name);
			}
			const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
			if (!Action)
			{
				OutError = FString::Printf(TEXT("No Input Action at '%s'."), *Name);
				return nullptr;
			}
			OutPath = Action->GetPathName();
			return Action;
		}

		TArray<FAssetData> Matches;
		TArray<FString> Known;
		for (const FAssetData& Asset : AllInputActions())
		{
			if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				Matches.Add(Asset);
			}
			if (Known.Num() < 30)
			{
				Known.Add(Asset.AssetName.ToString());
			}
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Paths;
			for (const FAssetData& Asset : Matches)
			{
				Paths.Add(Asset.GetObjectPathString());
			}
			OutError = FString::Printf(TEXT("'%s' matches several Input Actions: %s. Pass the full path."), *Name, *FString::Join(Paths, TEXT(", ")));
			return nullptr;
		}
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No Input Action named '%s'. Known actions: %s."), *Name, Known.Num() ? *FString::Join(Known, TEXT(", ")) : TEXT("none"));
			return nullptr;
		}
		const UInputAction* Action = Cast<UInputAction>(Matches[0].GetAsset());
		if (!Action)
		{
			OutError = FString::Printf(TEXT("Could not load '%s'."), *Matches[0].GetObjectPathString());
			return nullptr;
		}
		OutPath = Action->GetPathName();
		return Action;
	}

	/** Parses '1', '0.5', '0,1', '0 1', '(X=0,Y=1)', '[0,1]', 'true'/'false' into a vector sized for the action. */
	bool ParseActionValue(const FString& Text, const UInputAction* Action, FVector& OutValue, FString& OutError)
	{
		FString Clean = Text.TrimStartAndEnd();
		if (Clean.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			Clean = TEXT("1");
		}
		else if (Clean.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			Clean = TEXT("0");
		}
		for (const TCHAR* Strip : { TEXT("("), TEXT(")"), TEXT("["), TEXT("]"), TEXT("X="), TEXT("Y="), TEXT("Z="), TEXT("x="), TEXT("y="), TEXT("z=") })
		{
			Clean.ReplaceInline(Strip, TEXT(" "));
		}
		Clean.ReplaceInline(TEXT(","), TEXT(" "));
		TArray<FString> Parts;
		Clean.ParseIntoArrayWS(Parts);

		const int32 Dimensions = ValueDimensions(Action->ValueType);
		if (Parts.Num() == 0 || Parts.Num() > Dimensions)
		{
			OutError = FString::Printf(TEXT("'%s' does not fit %s (%s, %d component%s). Examples: %s."), *Text, *Action->GetName(), *ValueTypeName(Action->ValueType), Dimensions,
				Dimensions == 1 ? TEXT("") : TEXT("s"), Dimensions == 1 ? TEXT("'1', '0.5'") : Dimensions == 2 ? TEXT("'0,1', '1,0'") : TEXT("'0,0,1'"));
			return false;
		}
		OutValue = FVector::ZeroVector;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			double Number = 0.0;
			if (!FCString::IsNumeric(*Parts[Index]) || !LexTryParseString(Number, *Parts[Index]))
			{
				OutError = FString::Printf(TEXT("'%s' in value '%s' is not a number."), *Parts[Index], *Text);
				return false;
			}
			OutValue[Index] = Number;
		}
		return true;
	}

	bool ParseJobId(const FString& Text, int32& OutId, FString& OutError)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (GameplayMCP::IsUnset(Trimmed) || Trimmed.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			OutId = INDEX_NONE;
			return true;
		}
		if (Trimmed.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
		{
			OutId = FGameplayInputJobs::Get().GetLatestJobId();
			return true;
		}
		if (!Trimmed.IsNumeric() || Trimmed.Contains(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a job id. Use a number, 'latest' or '*'."), *Text);
			return false;
		}
		OutId = FCString::Atoi(*Trimmed);
		return true;
	}

	FGameplayMCPResult StartSteps(TArray<FStep>&& Steps, int32 PlayerIndex, const FString& ActionPath)
	{
		const int32 StepCount = Steps.Num();
		const double Expected = FGameplayInputJobs::EstimateDuration(Steps);
		FString Error;
		const int32 JobId = FGameplayInputJobs::Get().StartJob(PlayerIndex, MoveTemp(Steps), Error);
		if (JobId == INDEX_NONE)
		{
			return GameplayMCP::Fail(Error);
		}
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("job_id"), JobId);
		Payload->SetStringField(TEXT("state"), TEXT("queued"));
		if (!ActionPath.IsEmpty())
		{
			Payload->SetStringField(TEXT("action"), ActionPath);
		}
		Payload->SetNumberField(TEXT("step_count"), StepCount);
		Payload->SetNumberField(TEXT("expected_seconds"), Expected);
		Payload->SetStringField(TEXT("next"), TEXT("Poll input_status with this job_id until state is 'completed', then check the result in game."));
		return GameplayMCP::Ok(Payload);
	}

	bool ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Min, double Max, double& InOut, FString& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid())
		{
			return true;
		}
		double Number = 0.0;
		if (!Value->TryGetNumber(Number))
		{
			FString Text;
			if (!Value->TryGetString(Text) || !FCString::IsNumeric(*Text) || !LexTryParseString(Number, *Text))
			{
				OutError = FString::Printf(TEXT("\"%s\" must be a number."), Field);
				return false;
			}
		}
		if (Number < Min || Number > Max)
		{
			OutError = FString::Printf(TEXT("\"%s\" must be between %g and %g, got %g."), Field, Min, Max, Number);
			return false;
		}
		InOut = Number;
		return true;
	}

	bool ParseStepJson(const TSharedPtr<FJsonObject>& Object, int32 Index, FStep& OutStep, FString& OutError)
	{
		static const TSet<FString> KnownFields = { TEXT("type"), TEXT("action"), TEXT("value"), TEXT("end_value"), TEXT("duration"), TEXT("count"), TEXT("interval"), TEXT("hold_time"), TEXT("wait") };
		for (const auto& Pair : Object->Values)
		{
			const FString Key(*Pair.Key);
			if (!KnownFields.Contains(Key))
			{
				OutError = FString::Printf(TEXT("Step %d: unknown field \"%s\". Allowed: type, action, value, end_value, duration, count, interval, hold_time, wait."), Index, *Key);
				return false;
			}
		}

		FString Type;
		Object->TryGetStringField(TEXT("type"), Type);
		Type = Type.ToLower();
		if (Type == TEXT("hold")) { OutStep.Kind = EStepKind::Hold; }
		else if (Type == TEXT("tap")) { OutStep.Kind = EStepKind::Tap; }
		else if (Type == TEXT("axis")) { OutStep.Kind = EStepKind::Axis; }
		else if (Type == TEXT("wait")) { OutStep.Kind = EStepKind::Wait; }
		else
		{
			OutError = FString::Printf(TEXT("Step %d: \"type\" must be hold, tap, axis or wait."), Index);
			return false;
		}

		bool bWait = true;
		if (Object->TryGetBoolField(TEXT("wait"), bWait))
		{
			OutStep.bWait = bWait;
		}

		FString Error;
		double Duration = OutStep.Kind == EStepKind::Hold || OutStep.Kind == EStepKind::Axis ? -1.0 : 0.0;
		if (!ReadNumber(Object, TEXT("duration"), 0.0, FGameplayInputJobs::MaxJobSeconds, Duration, Error))
		{
			OutError = FString::Printf(TEXT("Step %d: %s"), Index, *Error);
			return false;
		}
		if (Duration < 0.0 || (OutStep.Kind == EStepKind::Wait && !Object->HasField(TEXT("duration"))))
		{
			OutError = FString::Printf(TEXT("Step %d (%s) needs \"duration\" in seconds."), Index, *Type);
			return false;
		}
		OutStep.Duration = Duration;

		if (OutStep.Kind == EStepKind::Wait)
		{
			if (Object->HasField(TEXT("action")))
			{
				OutError = FString::Printf(TEXT("Step %d: a wait step takes no action."), Index);
				return false;
			}
			return true;
		}

		FString ActionName;
		if (!Object->TryGetStringField(TEXT("action"), ActionName))
		{
			OutError = FString::Printf(TEXT("Step %d (%s) needs \"action\"."), Index, *Type);
			return false;
		}
		const UInputAction* Action = ResolveAction(ActionName, OutStep.ActionPath, Error);
		if (!Action)
		{
			OutError = FString::Printf(TEXT("Step %d: %s"), Index, *Error);
			return false;
		}
		OutStep.Action = Action;

		const TSharedPtr<FJsonValue> ValueField = Object->TryGetField(TEXT("value"));
		const FString ValueText = ValueField.IsValid() ? GameplayMCP::JsonValueToText(ValueField) : FString(TEXT("1"));
		if (OutStep.Kind == EStepKind::Axis && !ValueField.IsValid())
		{
			OutError = FString::Printf(TEXT("Step %d (axis) needs \"value\"."), Index);
			return false;
		}
		if (!ParseActionValue(ValueText, Action, OutStep.Value, Error))
		{
			OutError = FString::Printf(TEXT("Step %d: %s"), Index, *Error);
			return false;
		}

		if (OutStep.Kind == EStepKind::Axis)
		{
			if (Action->ValueType == EInputActionValueType::Boolean)
			{
				OutError = FString::Printf(TEXT("Step %d: %s is a Boolean action; use a hold or tap step."), Index, *Action->GetName());
				return false;
			}
			const TSharedPtr<FJsonValue> EndField = Object->TryGetField(TEXT("end_value"));
			if (EndField.IsValid() && !GameplayMCP::IsUnset(GameplayMCP::JsonValueToText(EndField)))
			{
				if (!ParseActionValue(GameplayMCP::JsonValueToText(EndField), Action, OutStep.EndValue, Error))
				{
					OutError = FString::Printf(TEXT("Step %d: end_value: %s"), Index, *Error);
					return false;
				}
				OutStep.bRamp = true;
			}
		}
		else if (Object->HasField(TEXT("end_value")))
		{
			OutError = FString::Printf(TEXT("Step %d: end_value only applies to axis steps."), Index);
			return false;
		}

		if (OutStep.Kind == EStepKind::Tap)
		{
			double Count = 1.0;
			if (!ReadNumber(Object, TEXT("count"), 1.0, 100.0, Count, Error)
				|| !ReadNumber(Object, TEXT("interval"), 0.0, 60.0, OutStep.Interval, Error)
				|| !ReadNumber(Object, TEXT("hold_time"), 0.0, 60.0, OutStep.HoldTime, Error))
			{
				OutError = FString::Printf(TEXT("Step %d: %s"), Index, *Error);
				return false;
			}
			if (FMath::Frac(Count) != 0.0)
			{
				OutError = FString::Printf(TEXT("Step %d: \"count\" must be a whole number."), Index);
				return false;
			}
			OutStep.Count = static_cast<int32>(Count);
		}
		else if (Object->HasField(TEXT("count")) || Object->HasField(TEXT("interval")) || Object->HasField(TEXT("hold_time")))
		{
			OutError = FString::Printf(TEXT("Step %d: count, interval and hold_time only apply to tap steps."), Index);
			return false;
		}
		return true;
	}
}

FGameplayMCPResult UGameplayInputToolset::input_list_actions(const FString& name_filter)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	const FString Filter = name_filter.TrimStartAndEnd();
	TArray<TSharedRef<FJsonObject>> Actions;
	for (const FAssetData& Asset : AllInputActions())
	{
		if (!GameplayMCP::IsUnset(Filter) && !Asset.AssetName.ToString().MatchesWildcard(Filter))
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		if (const UInputAction* Action = Cast<UInputAction>(Asset.GetAsset()))
		{
			Entry->SetStringField(TEXT("value_type"), ValueTypeName(Action->ValueType));
		}
		Actions.Add(Entry);
	}
	Actions.Sort([](const TSharedRef<FJsonObject>& A, const TSharedRef<FJsonObject>& B) { return A->GetStringField(TEXT("path")) < B->GetStringField(TEXT("path")); });

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("count"), Actions.Num());
	Payload->SetArrayField(TEXT("actions"), GameplayMCP::ToJsonArray(Actions));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayInputToolset::input_hold_action(const FString& action, float duration, const FString& value, int32 player_index)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FStep Step;
	Step.Kind = EStepKind::Hold;
	FString Error;
	const UInputAction* Action = ResolveAction(action, Step.ActionPath, Error);
	if (!Action)
	{
		return GameplayMCP::Fail(Error);
	}
	if (duration < 0.f || duration > FGameplayInputJobs::MaxJobSeconds)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("duration must be between 0 and %.0f seconds."), FGameplayInputJobs::MaxJobSeconds));
	}
	if (!ParseActionValue(GameplayMCP::IsUnset(value) ? FString(TEXT("1")) : value, Action, Step.Value, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	Step.Action = Action;
	Step.Duration = duration;
	const FString Path = Step.ActionPath;
	return StartSteps({ Step }, player_index, Path);
}

FGameplayMCPResult UGameplayInputToolset::input_tap_action(const FString& action, int32 count, float interval, float hold_time, int32 player_index)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FStep Step;
	Step.Kind = EStepKind::Tap;
	FString Error;
	const UInputAction* Action = ResolveAction(action, Step.ActionPath, Error);
	if (!Action)
	{
		return GameplayMCP::Fail(Error);
	}
	if (count < 1 || count > 100)
	{
		return GameplayMCP::Fail(TEXT("count must be between 1 and 100."));
	}
	if (interval < 0.f || interval > 60.f || hold_time < 0.f || hold_time > 60.f)
	{
		return GameplayMCP::Fail(TEXT("interval and hold_time must be between 0 and 60 seconds."));
	}
	Step.Action = Action;
	Step.Count = count;
	Step.Interval = interval;
	Step.HoldTime = hold_time;
	const FString Path = Step.ActionPath;
	return StartSteps({ Step }, player_index, Path);
}

FGameplayMCPResult UGameplayInputToolset::input_set_axis(const FString& action, const FString& value, const FString& end_value, float duration, int32 player_index)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FStep Step;
	Step.Kind = EStepKind::Axis;
	FString Error;
	const UInputAction* Action = ResolveAction(action, Step.ActionPath, Error);
	if (!Action)
	{
		return GameplayMCP::Fail(Error);
	}
	if (Action->ValueType == EInputActionValueType::Boolean)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("%s is a Boolean action; use input_hold_action or input_tap_action."), *Action->GetName()));
	}
	if (duration < 0.f || duration > FGameplayInputJobs::MaxJobSeconds)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("duration must be between 0 and %.0f seconds."), FGameplayInputJobs::MaxJobSeconds));
	}
	if (!ParseActionValue(value, Action, Step.Value, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	if (!GameplayMCP::IsUnset(end_value))
	{
		if (!ParseActionValue(end_value, Action, Step.EndValue, Error))
		{
			return GameplayMCP::Fail(FString::Printf(TEXT("end_value: %s"), *Error));
		}
		Step.bRamp = true;
	}
	Step.Action = Action;
	Step.Duration = duration;
	const FString Path = Step.ActionPath;
	return StartSteps({ Step }, player_index, Path);
}

FGameplayMCPResult UGameplayInputToolset::input_sequence(const FString& steps, int32 player_index)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	TArray<TSharedPtr<FJsonValue>> Items;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(steps.TrimStartAndEnd());
	if (!FJsonSerializer::Deserialize(Reader, Items))
	{
		return GameplayMCP::Fail(TEXT("steps must be a JSON array, e.g. [{\"type\":\"hold\",\"action\":\"IA_Move\",\"value\":\"0,1\",\"duration\":1},{\"type\":\"tap\",\"action\":\"IA_Jump\"}]."));
	}
	if (Items.Num() == 0 || Items.Num() > 200)
	{
		return GameplayMCP::Fail(TEXT("steps must contain between 1 and 200 steps."));
	}

	TArray<FStep> Parsed;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Items[Index].IsValid() || !Items[Index]->TryGetObject(Object))
		{
			return GameplayMCP::Fail(FString::Printf(TEXT("Step %d is not a JSON object."), Index));
		}
		FStep Step;
		FString Error;
		if (!ParseStepJson(*Object, Index, Step, Error))
		{
			return GameplayMCP::Fail(Error);
		}
		Parsed.Add(MoveTemp(Step));
	}
	return StartSteps(MoveTemp(Parsed), player_index, FString());
}

FGameplayMCPResult UGameplayInputToolset::input_status(const FString& job_id)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	int32 JobId = INDEX_NONE;
	FString Error;
	if (!ParseJobId(job_id, JobId, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	TArray<TSharedRef<FJsonObject>> Jobs = FGameplayInputJobs::Get().GetStatus(JobId);
	if (JobId != INDEX_NONE && Jobs.Num() == 0)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("No input job %s. Only the most recent jobs are kept; use '*' to list them."), *job_id));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("jobs"), GameplayMCP::ToJsonArray(Jobs));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayInputToolset::input_cancel(const FString& job_id)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	int32 JobId = INDEX_NONE;
	FString Error;
	if (!ParseJobId(job_id, JobId, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	if (JobId != INDEX_NONE && FGameplayInputJobs::Get().GetStatus(JobId).Num() == 0)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("No input job %s."), *job_id));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("cancelled"), FGameplayInputJobs::Get().Cancel(JobId, TEXT("cancelled by input_cancel")));
	return GameplayMCP::Ok(Payload);
}
