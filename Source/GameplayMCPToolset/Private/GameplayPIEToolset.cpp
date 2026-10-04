// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayPIEToolset.h"

#include "GameplayMCPHelpers.h"

#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Resolves the PIE world and the target object, failing with a clear error if either is missing. */
	UObject* ResolveTargetOrError(const FString& Actor, const FString& Component, UWorld*& OutWorld, FString& OutError)
	{
		OutWorld = GameplayMCP::GetPIEWorld(OutError);
		if (!OutWorld)
		{
			return nullptr;
		}
		UObject* Target = GameplayMCP::ResolvePIETarget(OutWorld, Actor, Component, OutError);
		if (Target && !GameplayMCP::IsPIEObject(Target))
		{
			OutError = FString::Printf(TEXT("%s is not a PIE-world object."), *Target->GetPathName());
			return nullptr;
		}
		return Target;
	}

	FString DescribeObject(const UObject* Object)
	{
		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			return FString::Printf(TEXT("%s.%s"), Component->GetOwner() ? *Component->GetOwner()->GetName() : TEXT("?"), *Component->GetName());
		}
		if (const UAnimInstance* AnimInstance = Cast<UAnimInstance>(Object))
		{
			const AActor* Owner = AnimInstance->GetOwningActor();
			return FString::Printf(TEXT("%s.%s"), Owner ? *Owner->GetName() : TEXT("?"), *AnimInstance->GetName());
		}
		return Object->GetName();
	}

	bool IsReturnParam(const FProperty* Param)
	{
		return Param->HasAnyPropertyFlags(CPF_ReturnParm);
	}

	/** Non-const reference parameters: values the function writes back. */
	bool IsOutParam(const FProperty* Param)
	{
		return !IsReturnParam(Param) && Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm);
	}

	FString ParamDirection(const FProperty* Param)
	{
		if (IsReturnParam(Param))
		{
			return TEXT("return");
		}
		if (IsOutParam(Param))
		{
			return Param->HasAnyPropertyFlags(CPF_ReferenceParm) ? TEXT("inout") : TEXT("out");
		}
		return TEXT("in");
	}

	TArray<FString> FunctionFlagsToStrings(const UFunction* Function)
	{
		TArray<FString> Flags;
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)) { Flags.Add(TEXT("blueprint_callable")); }
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) { Flags.Add(TEXT("blueprint_pure")); }
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { Flags.Add(TEXT("blueprint_event")); }
		if (Function->HasAnyFunctionFlags(FUNC_Exec)) { Flags.Add(TEXT("exec")); }
		if (Function->HasAnyFunctionFlags(FUNC_Net)) { Flags.Add(TEXT("rpc")); }
		if (Function->HasAnyFunctionFlags(FUNC_Static)) { Flags.Add(TEXT("static")); }
		if (Function->HasAnyFunctionFlags(FUNC_Const)) { Flags.Add(TEXT("const")); }
		if (Function->HasMetaData(TEXT("Latent"))) { Flags.Add(TEXT("latent")); }
		return Flags;
	}

	TArray<TSharedPtr<FJsonValue>> FunctionParamsToJson(const UFunction* Function)
	{
		TArray<TSharedRef<FJsonObject>> Params;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
			Param->SetStringField(TEXT("name"), It->GetName());
			Param->SetStringField(TEXT("type"), GameplayMCP::PropertyTypeToString(*It));
			Param->SetStringField(TEXT("direction"), ParamDirection(*It));
			const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *It->GetName());
			if (Function->HasMetaData(*DefaultKey))
			{
				Param->SetStringField(TEXT("default"), Function->GetMetaData(*DefaultKey));
			}
			Params.Add(Param);
		}
		return GameplayMCP::ToJsonArray(Params);
	}

	bool ClassMatchesFilter(const UClass* Class, const UClass* FilterClass, const FString& Filter)
	{
		if (FilterClass)
		{
			return Class->IsChildOf(FilterClass);
		}
		for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			const FString Name = Current->GetName();
			if (Name.MatchesWildcard(Filter) || Name.MatchesWildcard(Filter + TEXT("_C")))
			{
				return true;
			}
		}
		return false;
	}

	TSharedRef<FJsonObject> SceneComponentToJson(USceneComponent* Component, const AActor* Owner, TSet<const UActorComponent*>& Visited)
	{
		Visited.Add(Component);
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Component->GetName());
		Out->SetStringField(TEXT("class"), Component->GetClass()->GetName());
		Out->SetStringField(TEXT("socket"), Component->GetAttachSocketName().IsNone() ? FString() : Component->GetAttachSocketName().ToString());
		Out->SetBoolField(TEXT("visible"), Component->IsVisible());
		Out->SetObjectField(TEXT("relative"), GameplayMCP::TransformToJson(Component->GetRelativeTransform()));
		Out->SetObjectField(TEXT("world"), GameplayMCP::TransformToJson(Component->GetComponentTransform()));

		TArray<TSharedRef<FJsonObject>> Children;
		for (USceneComponent* Child : Component->GetAttachChildren())
		{
			if (!Child)
			{
				continue;
			}
			if (Child->GetOwner() != Owner)
			{
				// A component of another actor attached here: report it without walking into that actor.
				TSharedRef<FJsonObject> Attached = MakeShared<FJsonObject>();
				Attached->SetStringField(TEXT("name"), Child->GetName());
				Attached->SetStringField(TEXT("class"), Child->GetClass()->GetName());
				Attached->SetStringField(TEXT("attached_actor"), Child->GetOwner() ? Child->GetOwner()->GetName() : FString());
				Children.Add(Attached);
				continue;
			}
			Children.Add(SceneComponentToJson(Child, Owner, Visited));
		}
		Out->SetArrayField(TEXT("children"), GameplayMCP::ToJsonArray(Children));
		return Out;
	}

	bool IsAnimInternalProperty(const FProperty* Property)
	{
		const FString Name = Property->GetName();
		if (Name.StartsWith(TEXT("__")) || Name.StartsWith(TEXT("AnimGraphNode_")) || Name.StartsWith(TEXT("AnimBlueprintExtension")))
		{
			return true;
		}
		const FStructProperty* StructProp = CastField<FStructProperty>(Property);
		return StructProp && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct());
	}
}

FGameplayMCPResult UGameplayPIEToolset::pie_get_status()
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	FString Unused;
	UWorld* World = GameplayMCP::GetPIEWorld(Unused);
	Payload->SetBoolField(TEXT("running"), World != nullptr);
	Payload->SetBoolField(TEXT("requested"), GEditor && GEditor->IsPlaySessionRequestQueued());
	if (!World)
	{
		return GameplayMCP::Ok(Payload);
	}

	Payload->SetBoolField(TEXT("simulating"), GEditor->bIsSimulatingInEditor);
	Payload->SetStringField(TEXT("world"), World->GetName());
	Payload->SetStringField(TEXT("map"), World->GetMapName());
	Payload->SetStringField(TEXT("net_mode"), GameplayMCP::NetModeToString(World));
	Payload->SetBoolField(TEXT("paused"), World->IsPaused());
	Payload->SetNumberField(TEXT("time_seconds"), World->GetTimeSeconds());
	Payload->SetNumberField(TEXT("real_time_seconds"), World->GetRealTimeSeconds());

	TArray<TSharedRef<FJsonObject>> Players;
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->GetLocalPlayers();
		for (int32 Index = 0; Index < LocalPlayers.Num(); ++Index)
		{
			APlayerController* Controller = LocalPlayers[Index] ? LocalPlayers[Index]->GetPlayerController(World) : nullptr;
			TSharedRef<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetNumberField(TEXT("index"), Index);
			Player->SetStringField(TEXT("controller"), Controller ? Controller->GetName() : FString());
			Player->SetStringField(TEXT("pawn"), Controller && Controller->GetPawn() ? Controller->GetPawn()->GetName() : FString());
			Players.Add(Player);
		}
	}
	Payload->SetArrayField(TEXT("local_players"), GameplayMCP::ToJsonArray(Players));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_start()
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	if (!GEditor)
	{
		return GameplayMCP::Fail(TEXT("The editor is not available."));
	}
	if (GEditor->IsPlaySessionInProgress())
	{
		return GameplayMCP::Fail(TEXT("A play session is already running or queued. Use pie_get_status, or pie_stop first."));
	}

	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	FString Destination = TEXT("new_window");
	if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		TSharedPtr<IAssetViewport> Viewport = LevelEditor->GetFirstActiveViewport();
		if (Viewport.IsValid())
		{
			Params.DestinationSlateViewport = Viewport;
			Destination = TEXT("level_viewport");
		}
	}
	GEditor->RequestPlaySession(Params);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("requested"), true);
	Payload->SetStringField(TEXT("destination"), Destination);
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_stop()
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	if (!GameplayMCP::GetPIEWorld(Error))
	{
		return GameplayMCP::Fail(Error);
	}
	GEditor->RequestEndPlayMap();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("requested"), true);
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_list_actors(const FString& class_filter, const FString& label_filter, const FString& tag_filter, int32 limit)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = GameplayMCP::GetPIEWorld(Error);
	if (!World)
	{
		return GameplayMCP::Fail(Error);
	}

	const FString ClassFilter = class_filter.TrimStartAndEnd();
	UClass* FilterClass = nullptr;
	if (!GameplayMCP::IsUnset(ClassFilter) && ClassFilter.StartsWith(TEXT("/")))
	{
		FilterClass = LoadObject<UClass>(nullptr, *ClassFilter, nullptr, LOAD_NoWarn);
		if (!FilterClass)
		{
			FilterClass = LoadObject<UClass>(nullptr, *(ClassFilter + TEXT("_C")), nullptr, LOAD_NoWarn);
		}
		if (!FilterClass)
		{
			return GameplayMCP::Fail(FString::Printf(TEXT("No class found at '%s'. Use a class path such as '/Script/Engine.Pawn' or '/Game/Blueprints/BP_Enemy.BP_Enemy_C', or a plain class name."), *ClassFilter));
		}
	}

	const int32 Limit = FMath::Clamp(limit, 1, 5000);
	TArray<TSharedRef<FJsonObject>> Actors;
	int32 TotalMatches = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		if (!GameplayMCP::IsUnset(ClassFilter) && !ClassMatchesFilter(Actor->GetClass(), FilterClass, ClassFilter))
		{
			continue;
		}
		if (!GameplayMCP::IsUnset(label_filter)
			&& !Actor->GetActorLabel(false).MatchesWildcard(label_filter.TrimStartAndEnd())
			&& !Actor->GetName().MatchesWildcard(label_filter.TrimStartAndEnd()))
		{
			continue;
		}
		if (!GameplayMCP::IsUnset(tag_filter))
		{
			const FString Tag = tag_filter.TrimStartAndEnd();
			const bool bHasTag = Actor->Tags.ContainsByPredicate([&Tag](const FName& ActorTag) { return ActorTag.ToString().MatchesWildcard(Tag); });
			if (!bHasTag)
			{
				continue;
			}
		}
		++TotalMatches;
		if (Actors.Num() < Limit)
		{
			Actors.Add(GameplayMCP::ActorToJson(Actor));
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("count"), Actors.Num());
	Payload->SetNumberField(TEXT("total_matches"), TotalMatches);
	Payload->SetArrayField(TEXT("actors"), GameplayMCP::ToJsonArray(Actors));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_find_actor(const FString& actor)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = GameplayMCP::GetPIEWorld(Error);
	if (!World)
	{
		return GameplayMCP::Fail(Error);
	}
	AActor* Actor = GameplayMCP::FindPIEActor(World, actor, Error);
	if (!Actor)
	{
		return GameplayMCP::Fail(Error);
	}

	TSharedRef<FJsonObject> Payload = GameplayMCP::ActorToJson(Actor);
	TArray<FString> ParentClasses;
	for (const UClass* Class = Actor->GetClass()->GetSuperClass(); Class; Class = Class->GetSuperClass())
	{
		ParentClasses.Add(Class->GetName());
		if (Class == AActor::StaticClass())
		{
			break;
		}
	}
	Payload->SetArrayField(TEXT("parent_classes"), GameplayMCP::ToJsonArray(ParentClasses));
	Payload->SetStringField(TEXT("owner"), Actor->GetOwner() ? Actor->GetOwner()->GetName() : FString());
	Payload->SetStringField(TEXT("instigator"), Actor->GetInstigator() ? Actor->GetInstigator()->GetName() : FString());
	const APawn* Pawn = Cast<APawn>(Actor);
	Payload->SetBoolField(TEXT("is_pawn"), Pawn != nullptr);
	Payload->SetStringField(TEXT("controller"), Pawn && Pawn->GetController() ? Pawn->GetController()->GetName() : FString());
	Payload->SetBoolField(TEXT("hidden"), Actor->IsHidden());
	TInlineComponentArray<UActorComponent*> Components(Actor);
	Payload->SetNumberField(TEXT("component_count"), Components.Num());
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_get_property(const FString& actor, const FString& property_path, const FString& component)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	UObject* Target = ResolveTargetOrError(actor, component, World, Error);
	if (!Target)
	{
		return GameplayMCP::Fail(Error);
	}

	GameplayMCP::FPropertyLocation Location;
	if (!GameplayMCP::ResolvePropertyPath(Target, property_path, /*bForWrite*/ false, Location, Error))
	{
		return GameplayMCP::Fail(Error);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(Target));
	Payload->SetStringField(TEXT("property_path"), property_path.TrimStartAndEnd());
	Payload->SetStringField(TEXT("type"), GameplayMCP::PropertyTypeToString(Location.Property));
	Payload->SetField(TEXT("value"), GameplayMCP::PropertyValueToJson(Location.Property, Location.ValuePtr));
	Payload->SetStringField(TEXT("text"), GameplayMCP::Truncate(GameplayMCP::PropertyValueToText(Location.Property, Location.ValuePtr, Location.Owner), 4000));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_set_property(const FString& actor, const FString& property_path, const FString& value, const FString& component)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	UObject* Target = ResolveTargetOrError(actor, component, World, Error);
	if (!Target)
	{
		return GameplayMCP::Fail(Error);
	}

	GameplayMCP::FPropertyLocation Location;
	if (!GameplayMCP::ResolvePropertyPath(Target, property_path, /*bForWrite*/ true, Location, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	if (!Location.bWritable)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is an element of a fixed-size C array; writing those is not supported."), *property_path));
	}
	if (!GameplayMCP::IsPIEObject(Location.Owner))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Refusing to write: %s is not a PIE-world object."), *Location.Owner->GetPathName()));
	}

	const FString OldText = GameplayMCP::PropertyValueToText(Location.Property, Location.ValuePtr, Location.Owner);
	if (!GameplayMCP::ImportPropertyValue(Location.Property, Location.ValuePtr, Location.Owner, value, World, Error))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Nothing was changed. %s"), *Error));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(Target));
	Payload->SetStringField(TEXT("property_path"), property_path.TrimStartAndEnd());
	Payload->SetStringField(TEXT("type"), GameplayMCP::PropertyTypeToString(Location.Property));
	Payload->SetStringField(TEXT("old_text"), GameplayMCP::Truncate(OldText, 4000));
	Payload->SetField(TEXT("value"), GameplayMCP::PropertyValueToJson(Location.Property, Location.ValuePtr));
	Payload->SetStringField(TEXT("text"), GameplayMCP::Truncate(GameplayMCP::PropertyValueToText(Location.Property, Location.ValuePtr, Location.Owner), 4000));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_list_properties(const FString& actor, const FString& component, const FString& name_filter, bool include_all, int32 max_value_length)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	UObject* Target = ResolveTargetOrError(actor, component, World, Error);
	if (!Target)
	{
		return GameplayMCP::Fail(Error);
	}

	const FString Filter = name_filter.TrimStartAndEnd();
	TArray<TSharedRef<FJsonObject>> Properties;
	for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!include_all && !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}
		if (!GameplayMCP::IsUnset(Filter) && !Property->GetName().MatchesWildcard(Filter) && !Property->GetAuthoredName().MatchesWildcard(Filter))
		{
			continue;
		}

		TArray<FString> Flags;
		if (Property->HasAnyPropertyFlags(CPF_Edit)) { Flags.Add(TEXT("edit")); }
		if (Property->HasAnyPropertyFlags(CPF_EditConst)) { Flags.Add(TEXT("edit_const")); }
		if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible)) { Flags.Add(TEXT("blueprint_visible")); }
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)) { Flags.Add(TEXT("blueprint_read_only")); }
		if (Property->HasAnyPropertyFlags(CPF_Transient)) { Flags.Add(TEXT("transient")); }
		if (Property->HasAnyPropertyFlags(CPF_Net)) { Flags.Add(TEXT("replicated")); }

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Property->GetAuthoredName());
		Entry->SetStringField(TEXT("type"), GameplayMCP::PropertyTypeToString(Property));
		Entry->SetStringField(TEXT("value"), GameplayMCP::Truncate(GameplayMCP::PropertyValueToText(Property, ValuePtr, Target), max_value_length));
		Entry->SetStringField(TEXT("declared_in"), Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetName() : FString());
		Entry->SetArrayField(TEXT("flags"), GameplayMCP::ToJsonArray(Flags));
		Properties.Add(Entry);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(Target));
	Payload->SetStringField(TEXT("class"), Target->GetClass()->GetName());
	Payload->SetNumberField(TEXT("count"), Properties.Num());
	Payload->SetArrayField(TEXT("properties"), GameplayMCP::ToJsonArray(Properties));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_list_functions(const FString& actor, const FString& component, const FString& name_filter, bool include_all)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	UObject* Target = ResolveTargetOrError(actor, component, World, Error);
	if (!Target)
	{
		return GameplayMCP::Fail(Error);
	}

	const FString Filter = name_filter.TrimStartAndEnd();
	TSet<FName> Seen;
	TArray<TSharedRef<FJsonObject>> Functions;
	for (TFieldIterator<UFunction> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const UFunction* Function = *It;
		if (Seen.Contains(Function->GetFName()) || Function->HasAnyFunctionFlags(FUNC_Delegate))
		{
			continue;
		}
		Seen.Add(Function->GetFName());
		if (!include_all && !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec))
		{
			continue;
		}
		if (!GameplayMCP::IsUnset(Filter) && !Function->GetName().MatchesWildcard(Filter))
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Function->GetName());
		Entry->SetStringField(TEXT("declared_in"), Function->GetOuterUClass() ? Function->GetOuterUClass()->GetName() : FString());
		Entry->SetArrayField(TEXT("flags"), GameplayMCP::ToJsonArray(FunctionFlagsToStrings(Function)));
		Entry->SetArrayField(TEXT("params"), FunctionParamsToJson(Function));
		Functions.Add(Entry);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(Target));
	Payload->SetStringField(TEXT("class"), Target->GetClass()->GetName());
	Payload->SetNumberField(TEXT("count"), Functions.Num());
	Payload->SetArrayField(TEXT("functions"), GameplayMCP::ToJsonArray(Functions));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_call_function(const FString& actor, const FString& function_name, const FString& args, const FString& component)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	UObject* Target = ResolveTargetOrError(actor, component, World, Error);
	if (!Target)
	{
		return GameplayMCP::Fail(Error);
	}

	const FString FunctionName = function_name.TrimStartAndEnd();
	UFunction* Function = FunctionName.IsEmpty() ? nullptr : Target->FindFunction(FName(*FunctionName));
	if (!Function)
	{
		TArray<FString> Similar;
		for (TFieldIterator<UFunction> It(Target->GetClass()); It && Similar.Num() < 15; ++It)
		{
			if (!FunctionName.IsEmpty() && It->GetName().Contains(FunctionName))
			{
				Similar.AddUnique(It->GetName());
			}
		}
		return GameplayMCP::Fail(FString::Printf(TEXT("%s (%s) has no function '%s'.%s Use pie_list_functions to see what can be called."),
			*DescribeObject(Target), *Target->GetClass()->GetName(), *FunctionName,
			Similar.Num() > 0 ? *FString::Printf(TEXT(" Similar: %s."), *FString::Join(Similar, TEXT(", "))) : TEXT("")));
	}
	if (Function->HasMetaData(TEXT("Latent")))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is a latent function (it completes over time through a latent action) and cannot be called directly."), *Function->GetName()));
	}
	if (Function->HasAnyFunctionFlags(FUNC_Delegate))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is a delegate signature, not a callable function."), *Function->GetName()));
	}

	TSharedPtr<FJsonObject> ArgsObject;
	if (!GameplayMCP::ParseJsonObject(args, ArgsObject, Error))
	{
		return GameplayMCP::Fail(Error);
	}

	FStructOnScope ParamsScope(Function);
	uint8* Params = ParamsScope.GetStructMemory();
	const FString WorldContextParam = Function->GetMetaData(TEXT("WorldContext"));

	// FJsonObject keys are UE::FSharedString in 5.8 (FString with legacy keys); operator* works for both.
	TArray<FString> ArgKeys;
	for (const auto& Pair : ArgsObject->Values)
	{
		ArgKeys.Add(FString(*Pair.Key));
	}

	TSet<FString> UsedArgs;
	TArray<FString> MissingArgs;
	TArray<FString> DefaultedArgs;
	TArray<FString> ParamNames;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FProperty* Param = *It;
		if (IsReturnParam(Param))
		{
			continue;
		}
		ParamNames.Add(FString::Printf(TEXT("%s (%s, %s)"), *Param->GetName(), *GameplayMCP::PropertyTypeToString(Param), *ParamDirection(Param)));

		FString MatchedKey;
		for (const FString& Key : ArgKeys)
		{
			if (Key.Equals(Param->GetName(), ESearchCase::IgnoreCase) || Key.Equals(Param->GetAuthoredName(), ESearchCase::IgnoreCase))
			{
				MatchedKey = Key;
				break;
			}
		}

		void* ValuePtr = Param->ContainerPtrToValuePtr<void>(Params);
		if (!MatchedKey.IsEmpty())
		{
			UsedArgs.Add(MatchedKey);
			if (!GameplayMCP::ImportPropertyValue(Param, ValuePtr, Target, GameplayMCP::JsonValueToText(ArgsObject->TryGetField(MatchedKey)), World, Error))
			{
				return GameplayMCP::Fail(FString::Printf(TEXT("Argument '%s': %s"), *Param->GetName(), *Error));
			}
			continue;
		}

		if (!WorldContextParam.IsEmpty() && Param->GetName() == WorldContextParam)
		{
			if (FObjectPropertyBase* ObjectParam = CastField<FObjectPropertyBase>(Param))
			{
				ObjectParam->SetObjectPropertyValue(ValuePtr, Target);
				continue;
			}
		}

		const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *Param->GetName());
		if (Function->HasMetaData(*DefaultKey))
		{
			FString DefaultError;
			if (GameplayMCP::ImportPropertyValue(Param, ValuePtr, Target, Function->GetMetaData(*DefaultKey), nullptr, DefaultError))
			{
				DefaultedArgs.Add(Param->GetName());
				continue;
			}
		}
		// Pure outputs need no argument; inputs and pass-by-reference (UPARAM(ref)) params do.
		if (!IsOutParam(Param) || Param->HasAnyPropertyFlags(CPF_ReferenceParm))
		{
			MissingArgs.Add(Param->GetName());
		}
	}

	TArray<FString> UnknownArgs;
	for (const FString& Key : ArgKeys)
	{
		if (!UsedArgs.Contains(Key))
		{
			UnknownArgs.Add(Key);
		}
	}
	if (UnknownArgs.Num() > 0)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Unknown argument(s) %s for %s. Parameters: %s."),
			*FString::Join(UnknownArgs, TEXT(", ")), *Function->GetName(), ParamNames.Num() > 0 ? *FString::Join(ParamNames, TEXT(", ")) : TEXT("none")));
	}

	Target->ProcessEvent(Function, Params);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(Target));
	Payload->SetStringField(TEXT("function"), Function->GetName());
	TSharedRef<FJsonObject> OutParams = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const void* ValuePtr = It->ContainerPtrToValuePtr<void>(Params);
		if (IsReturnParam(*It))
		{
			Payload->SetField(TEXT("return_value"), GameplayMCP::PropertyValueToJson(*It, ValuePtr));
		}
		else if (IsOutParam(*It))
		{
			OutParams->SetField(It->GetName(), GameplayMCP::PropertyValueToJson(*It, ValuePtr));
		}
	}
	Payload->SetObjectField(TEXT("out_params"), OutParams);
	Payload->SetArrayField(TEXT("missing_args"), GameplayMCP::ToJsonArray(MissingArgs));
	Payload->SetArrayField(TEXT("defaulted_args"), GameplayMCP::ToJsonArray(DefaultedArgs));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_get_anim_state(const FString& actor, const FString& component, bool include_variables, bool include_curves)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = nullptr;
	const FString MeshName = GameplayMCP::IsUnset(component) ? FString(TEXT("AnimInstance")) : component.TrimStartAndEnd() + TEXT("/AnimInstance");
	UAnimInstance* AnimInstance = Cast<UAnimInstance>(ResolveTargetOrError(actor, MeshName, World, Error));
	if (!AnimInstance)
	{
		return GameplayMCP::Fail(Error);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object"), DescribeObject(AnimInstance));
	Payload->SetStringField(TEXT("mesh_component"), AnimInstance->GetSkelMeshComponent() ? AnimInstance->GetSkelMeshComponent()->GetName() : FString());
	Payload->SetStringField(TEXT("anim_class"), AnimInstance->GetClass()->GetPathName());

	// State machines: walk the anim node properties so every instance is found, then map each
	// to its baked description through StateMachineIndexInClass.
	TArray<TSharedRef<FJsonObject>> Machines;
	if (const IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(AnimInstance->GetClass()))
	{
		const TArray<FBakedAnimationStateMachine>& Baked = AnimClass->GetBakedStateMachines();
		for (const FStructProperty* NodeProperty : AnimClass->GetAnimNodeProperties())
		{
			if (!NodeProperty || !NodeProperty->Struct->IsChildOf(FAnimNode_StateMachine::StaticStruct()))
			{
				continue;
			}
			const FAnimNode_StateMachine* Machine = NodeProperty->ContainerPtrToValuePtr<FAnimNode_StateMachine>(AnimInstance);
			const int32 MachineIndex = Machine->StateMachineIndexInClass;
			const FBakedAnimationStateMachine* Description = Baked.IsValidIndex(MachineIndex) ? &Baked[MachineIndex] : nullptr;

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Description ? Description->MachineName.ToString() : NodeProperty->GetName());
			Entry->SetNumberField(TEXT("index"), MachineIndex);
			const int32 StateIndex = Machine->GetCurrentState();
			const bool bValidState = Description && Description->States.IsValidIndex(StateIndex);
			Entry->SetStringField(TEXT("active_state"), bValidState ? Description->States[StateIndex].StateName.ToString() : FString());
			Entry->SetNumberField(TEXT("time_in_state"), Machine->GetCurrentStateElapsedTime());
			TArray<FString> States;
			if (Description)
			{
				for (const FBakedAnimationState& State : Description->States)
				{
					States.Add(State.StateName.ToString());
				}
			}
			Entry->SetArrayField(TEXT("states"), GameplayMCP::ToJsonArray(States));
			Machines.Add(Entry);
		}
	}
	Payload->SetArrayField(TEXT("state_machines"), GameplayMCP::ToJsonArray(Machines));

	TArray<TSharedRef<FJsonObject>> Montages;
	for (const FAnimMontageInstance* Instance : AnimInstance->MontageInstances)
	{
		if (!Instance || !Instance->Montage)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("montage"), Instance->Montage->GetPathName());
		Entry->SetStringField(TEXT("section"), Instance->GetCurrentSection().ToString());
		Entry->SetNumberField(TEXT("position"), Instance->GetPosition());
		Entry->SetNumberField(TEXT("play_rate"), Instance->GetPlayRate());
		Entry->SetNumberField(TEXT("weight"), Instance->GetWeight());
		Entry->SetBoolField(TEXT("is_playing"), Instance->IsPlaying());
		Entry->SetBoolField(TEXT("is_active"), Instance->IsActive());
		Montages.Add(Entry);
	}
	Payload->SetArrayField(TEXT("montages"), GameplayMCP::ToJsonArray(Montages));

	if (include_curves)
	{
		TSharedRef<FJsonObject> Curves = MakeShared<FJsonObject>();
		TArray<FName> CurveNames;
		AnimInstance->GetAllCurveNames(CurveNames);
		for (const FName& CurveName : CurveNames)
		{
			Curves->SetNumberField(CurveName.ToString(), AnimInstance->GetCurveValue(CurveName));
		}
		Payload->SetObjectField(TEXT("curves"), Curves);
	}

	if (include_variables)
	{
		// Variables declared in the anim Blueprint (and Blueprint parents), not native UAnimInstance state.
		TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		for (const UClass* Class = AnimInstance->GetClass(); Class && Cast<UBlueprintGeneratedClass>(Class); Class = Class->GetSuperClass())
		{
			for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (IsAnimInternalProperty(*It) || Variables->HasField(It->GetAuthoredName()))
				{
					continue;
				}
				Variables->SetField(It->GetAuthoredName(), GameplayMCP::PropertyValueToJson(*It, It->ContainerPtrToValuePtr<void>(AnimInstance)));
			}
		}
		Payload->SetObjectField(TEXT("variables"), Variables);
	}
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayPIEToolset::pie_get_component_tree(const FString& actor)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = GameplayMCP::GetPIEWorld(Error);
	if (!World)
	{
		return GameplayMCP::Fail(Error);
	}
	AActor* Actor = GameplayMCP::FindPIEActor(World, actor, Error);
	if (!Actor)
	{
		return GameplayMCP::Fail(Error);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("actor"), Actor->GetName());
	TSet<const UActorComponent*> Visited;
	if (USceneComponent* Root = Actor->GetRootComponent())
	{
		Payload->SetObjectField(TEXT("root"), SceneComponentToJson(Root, Actor, Visited));
	}

	TArray<TSharedRef<FJsonObject>> Others;
	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		if (Visited.Contains(Component))
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Component->GetName());
		Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
		Entry->SetBoolField(TEXT("active"), Component->IsActive());
		if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			// A scene component that is not under the root (detached or attached elsewhere).
			Entry->SetObjectField(TEXT("world"), GameplayMCP::TransformToJson(Scene->GetComponentTransform()));
			Entry->SetStringField(TEXT("attach_parent"), Scene->GetAttachParent() ? Scene->GetAttachParent()->GetName() : FString());
		}
		Others.Add(Entry);
	}
	Payload->SetArrayField(TEXT("other_components"), GameplayMCP::ToJsonArray(Others));
	return GameplayMCP::Ok(Payload);
}
