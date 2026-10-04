// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "GameplayMCPTypes.h"

class AActor;
class APlayerController;
class FProperty;
class UStruct;
class UWorld;

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

	/** Cuts long text and appends a marker so results stay readable. MaxLen <= 0 means no limit. */
	FString Truncate(const FString& Text, int32 MaxLen);

	// ---- Play-In-Editor --------------------------------------------------------------------

	/** The error every PIE tool returns when no session is running. */
	FString NoPIEError();

	/** The world of the first PIE instance, or nullptr with NoPIEError() when no session is running. */
	UWorld* GetPIEWorld(FString& OutError);

	/** True if the object lives in a Play-In-Editor world (never an editor-world actor or an asset). */
	bool IsPIEObject(const UObject* Object);

	/** The player controller of a local player in the PIE world, or nullptr. */
	APlayerController* GetLocalPlayerController(UWorld* World, int32 PlayerIndex);

	FString NetModeToString(UWorld* World);

	/**
	 * Resolves one live actor in the PIE world. Accepts, in order:
	 * the shortcuts "@pawn", "@controller", "@gamemode", "@gamestate", "@camera", "@hud" (optionally
	 * followed by a local player index, e.g. "@pawn1"), the exact object name (BP_Player_C_0), the
	 * editor label, or the full object path. Ambiguous labels fail and list the candidates.
	 */
	AActor* FindPIEActor(UWorld* World, const FString& NameOrLabel, FString& OutError);

	/**
	 * Resolves the object a property/function tool acts on: the actor itself, one of its components
	 * (by name, or by class name when unique), or an anim instance ("AnimInstance" for the first
	 * skeletal mesh with one, "<MeshComponent>/AnimInstance" for a specific mesh).
	 */
	UObject* ResolvePIETarget(UWorld* World, const FString& ActorName, const FString& ComponentName, FString& OutError);

	// ---- Assets ----------------------------------------------------------------------------

	/**
	 * Converts any accepted spelling of an asset path ("/Game/A/B", "/Game/A/B.B",
	 * "/Script/Engine.Blueprint'/Game/A/B.B'") into a full object path "/Game/A/B.B".
	 */
	bool NormalizeObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError);

	/** True if the package lives under /Game. */
	bool IsUnderGameRoot(const FString& PackageOrObjectPath);

	/** Loads an asset of the expected class. With bForWrite the asset must live under /Game. */
	UObject* LoadAssetChecked(const FString& Path, UClass* ExpectedClass, bool bForWrite, FString& OutError);

	// ---- JSON ------------------------------------------------------------------------------

	TSharedRef<FJsonObject> VectorToJson(const FVector& Vector);
	TSharedRef<FJsonObject> RotatorToJson(const FRotator& Rotator);
	TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform);
	TSharedRef<FJsonObject> ActorToJson(const AActor* Actor);

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<TSharedRef<FJsonObject>>& Objects);
	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings);

	/** Parses text that must be a JSON object. Empty or unset text gives an empty object. */
	bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError);

	/** Serializes any JSON value back to compact text ("3", "true", "{...}"). Strings are returned unquoted. */
	FString JsonValueToText(const TSharedPtr<FJsonValue>& Value);

	// ---- Reflection ------------------------------------------------------------------------

	/** Where a property path ended up: the leaf property, its value memory and the object that owns it. */
	struct FPropertyLocation
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		UObject* Owner = nullptr;
		/** False when the leaf is one element of a fixed-size C array (read-only here). */
		bool bWritable = true;
	};

	/**
	 * Walks a property path such as "Stats.Level", "Inventory[2].Count", "Inventory[Sword].Count"
	 * or "AmmoByType[Rifle]" starting at Root. Struct members and object references are followed;
	 * arrays accept a numeric index or a name key (matched against an element's Name/Id/Key/Tag
	 * field, or the object name for object arrays); maps accept a key in text form.
	 * With bForWrite, object references are only followed into PIE-world objects.
	 */
	bool ResolvePropertyPath(UObject* Root, const FString& Path, bool bForWrite, FPropertyLocation& Out, FString& OutError);

	/** Finds a property by name, then by authored (display) name, case-insensitively, including super structs. */
	FProperty* FindPropertyByName(const UStruct* Struct, const FString& Name);

	FString PropertyTypeToString(const FProperty* Property);
	FString PropertyValueToText(const FProperty* Property, const void* ValuePtr, UObject* Owner);
	TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Property, const void* ValuePtr);

	/**
	 * Writes a value into ValuePtr. The text is imported into a scratch copy first; the live value is
	 * only touched if the whole import succeeded. Accepted forms: plain text for strings, names and
	 * text; true/false for bools; numbers; enum names; Unreal text syntax such as "(X=1,Y=2,Z=3)";
	 * JSON objects/arrays ({"X":1} patches only the listed struct fields); and for object references
	 * "none", an asset/object path, or (when PIEWorld is given) a PIE actor name, label or shortcut.
	 */
	bool ImportPropertyValue(FProperty* Property, void* ValuePtr, UObject* Owner, const FString& Text, UWorld* PIEWorld, FString& OutError);
}
