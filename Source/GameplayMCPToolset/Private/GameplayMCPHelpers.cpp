// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayMCPHelpers.h"

#include "Animation/AnimInstance.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

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

	FString Truncate(const FString& Text, int32 MaxLen)
	{
		if (MaxLen <= 0 || Text.Len() <= MaxLen)
		{
			return Text;
		}
		return Text.Left(MaxLen) + FString::Printf(TEXT("... [%d more chars]"), Text.Len() - MaxLen);
	}

	// ---- Play-In-Editor --------------------------------------------------------------------

	FString NoPIEError()
	{
		return TEXT("No Play-In-Editor session is running. Start one with pie_start (or the editor's Play button), wait until pie_get_status reports running=true, then retry.");
	}

	UWorld* GetPIEWorld(FString& OutError)
	{
		if (GEditor)
		{
			if (FWorldContext* Context = GEditor->GetPIEWorldContext())
			{
				if (UWorld* World = Context->World())
				{
					return World;
				}
			}
			if (GEditor->PlayWorld)
			{
				return GEditor->PlayWorld;
			}
		}
		OutError = NoPIEError();
		return nullptr;
	}

	bool IsPIEObject(const UObject* Object)
	{
		if (!Object)
		{
			return false;
		}
		if (Object->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
		{
			return true;
		}
		const UWorld* World = Object->GetWorld();
		return World && World->WorldType == EWorldType::PIE;
	}

	APlayerController* GetLocalPlayerController(UWorld* World, int32 PlayerIndex)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		ULocalPlayer* LocalPlayer = GameInstance ? GameInstance->GetLocalPlayerByIndex(PlayerIndex) : nullptr;
		return LocalPlayer ? LocalPlayer->GetPlayerController(World) : nullptr;
	}

	FString NetModeToString(UWorld* World)
	{
		switch (World->GetNetMode())
		{
		case NM_Standalone:      return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated_server");
		case NM_ListenServer:    return TEXT("listen_server");
		case NM_Client:          return TEXT("client");
		default:                 return TEXT("unknown");
		}
	}

	namespace
	{
		AActor* ResolveShortcut(UWorld* World, const FString& Shortcut, FString& OutError)
		{
			FString Kind = Shortcut.Mid(1).ToLower();
			int32 PlayerIndex = 0;
			int32 DigitStart = Kind.Len();
			while (DigitStart > 0 && FChar::IsDigit(Kind[DigitStart - 1]))
			{
				--DigitStart;
			}
			if (DigitStart < Kind.Len())
			{
				PlayerIndex = FCString::Atoi(*Kind.Mid(DigitStart));
				Kind.LeftInline(DigitStart);
			}

			AActor* Result = nullptr;
			if (Kind == TEXT("gamemode"))
			{
				Result = World->GetAuthGameMode();
			}
			else if (Kind == TEXT("gamestate"))
			{
				Result = World->GetGameState();
			}
			else if (Kind == TEXT("pawn") || Kind == TEXT("player") || Kind == TEXT("controller") || Kind == TEXT("camera") || Kind == TEXT("hud"))
			{
				APlayerController* Controller = GetLocalPlayerController(World, PlayerIndex);
				if (!Controller)
				{
					OutError = FString::Printf(TEXT("'%s': local player %d has no player controller in the PIE world."), *Shortcut, PlayerIndex);
					return nullptr;
				}
				if (Kind == TEXT("controller"))
				{
					Result = Controller;
				}
				else if (Kind == TEXT("camera"))
				{
					Result = Controller->PlayerCameraManager;
				}
				else if (Kind == TEXT("hud"))
				{
					Result = Controller->GetHUD();
				}
				else
				{
					Result = Controller->GetPawn();
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown shortcut '%s'. Use @pawn, @controller, @camera, @hud (optionally with a local player index, e.g. @pawn1), @gamemode or @gamestate."), *Shortcut);
				return nullptr;
			}

			if (!Result)
			{
				OutError = FString::Printf(TEXT("'%s' resolved to nothing (for example the controller is not possessing a pawn)."), *Shortcut);
			}
			return Result;
		}
	}

	AActor* FindPIEActor(UWorld* World, const FString& NameOrLabel, FString& OutError)
	{
		const FString Name = NameOrLabel.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("Actor name is empty. Pass an object name (e.g. 'BP_Player_C_0'), a label, or a shortcut such as '@pawn'.");
			return nullptr;
		}
		if (Name.StartsWith(TEXT("@")))
		{
			return ResolveShortcut(World, Name, OutError);
		}

		TArray<AActor*> LabelMatches;
		TArray<FString> Similar;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (Actor->GetName().Equals(Name, ESearchCase::IgnoreCase) || Actor->GetPathName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
			const FString& Label = Actor->GetActorLabel(/*bCreateIfNone*/ false);
			if (Label.Equals(Name, ESearchCase::IgnoreCase))
			{
				LabelMatches.Add(Actor);
			}
			else if (Similar.Num() < 10 && (Actor->GetName().Contains(Name) || Label.Contains(Name)))
			{
				Similar.Add(Actor->GetName());
			}
		}

		if (LabelMatches.Num() == 1)
		{
			return LabelMatches[0];
		}
		if (LabelMatches.Num() > 1)
		{
			TArray<FString> Names;
			for (const AActor* Actor : LabelMatches)
			{
				Names.Add(Actor->GetName());
			}
			OutError = FString::Printf(TEXT("Label '%s' matches %d actors: %s. Pass the object name instead."), *Name, LabelMatches.Num(), *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}

		OutError = FString::Printf(TEXT("No actor named or labelled '%s' in the PIE world."), *Name);
		if (Similar.Num() > 0)
		{
			OutError += FString::Printf(TEXT(" Similar: %s."), *FString::Join(Similar, TEXT(", ")));
		}
		OutError += TEXT(" Use pie_list_actors to see what exists.");
		return nullptr;
	}

	UObject* ResolvePIETarget(UWorld* World, const FString& ActorName, const FString& ComponentName, FString& OutError)
	{
		AActor* Actor = FindPIEActor(World, ActorName, OutError);
		if (!Actor || IsUnset(ComponentName))
		{
			return Actor;
		}

		FString Component = ComponentName.TrimStartAndEnd();
		bool bAnimInstance = false;
		if (Component.Equals(TEXT("AnimInstance"), ESearchCase::IgnoreCase))
		{
			bAnimInstance = true;
			Component.Reset();
		}
		else if (Component.EndsWith(TEXT("/AnimInstance"), ESearchCase::IgnoreCase))
		{
			bAnimInstance = true;
			Component.LeftChopInline(FCString::Strlen(TEXT("/AnimInstance")));
		}

		TInlineComponentArray<UActorComponent*> Components(Actor);
		UActorComponent* Found = nullptr;
		if (!Component.IsEmpty())
		{
			TArray<UActorComponent*> ByClass;
			for (UActorComponent* Candidate : Components)
			{
				if (Candidate->GetName().Equals(Component, ESearchCase::IgnoreCase))
				{
					Found = Candidate;
					break;
				}
				const FString ClassName = Candidate->GetClass()->GetName();
				if (ClassName.Equals(Component, ESearchCase::IgnoreCase) || ClassName.Equals(Component + TEXT("_C"), ESearchCase::IgnoreCase))
				{
					ByClass.Add(Candidate);
				}
			}
			if (!Found)
			{
				// The C++/Blueprint property that holds the component, e.g. ACharacter::CharacterMovement
				// whose component object is named 'CharMoveComp'.
				if (const FObjectPropertyBase* Holder = CastField<FObjectPropertyBase>(FindPropertyByName(Actor->GetClass(), Component)))
				{
					UActorComponent* Held = Cast<UActorComponent>(Holder->GetObjectPropertyValue_InContainer(Actor));
					if (Held && Held->GetOwner() == Actor)
					{
						Found = Held;
					}
				}
			}
			if (!Found && ByClass.Num() == 1)
			{
				Found = ByClass[0];
			}
			if (!Found)
			{
				TArray<FString> Names;
				for (const UActorComponent* Candidate : Components)
				{
					Names.Add(FString::Printf(TEXT("%s (%s)"), *Candidate->GetName(), *Candidate->GetClass()->GetName()));
				}
				OutError = ByClass.Num() > 1
					? FString::Printf(TEXT("Several components of class '%s' on %s; pass a component name: %s."), *Component, *Actor->GetName(), *FString::Join(Names, TEXT(", ")))
					: FString::Printf(TEXT("Actor %s has no component '%s'. Components: %s."), *Actor->GetName(), *Component, *FString::Join(Names, TEXT(", ")));
				return nullptr;
			}
		}

		if (!bAnimInstance)
		{
			return Found;
		}

		USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Found);
		if (Found && !Mesh)
		{
			OutError = FString::Printf(TEXT("Component '%s' is a %s, not a skeletal mesh component, so it has no anim instance."), *Found->GetName(), *Found->GetClass()->GetName());
			return nullptr;
		}
		if (!Mesh)
		{
			for (UActorComponent* Candidate : Components)
			{
				USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(Candidate);
				if (SkelMesh && SkelMesh->GetAnimInstance())
				{
					Mesh = SkelMesh;
					break;
				}
			}
		}
		UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
		if (!AnimInstance)
		{
			OutError = FString::Printf(TEXT("Actor %s has no skeletal mesh component with a running anim instance."), *Actor->GetName());
			return nullptr;
		}
		return AnimInstance;
	}

	// ---- Assets ----------------------------------------------------------------------------

	bool NormalizeObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("Asset path is empty. Use a content path such as '/Game/Blueprints/BP_Door'.");
			return false;
		}
		if (Path.Contains(TEXT("'")))
		{
			Path = FPackageName::ExportTextPathToObjectPath(Path);
		}
		if (!Path.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a content path. Paths start with '/', e.g. '/Game/Blueprints/BP_Door'."), *InPath);
			return false;
		}
		FString PackageName = Path;
		FString ObjectName;
		if (Path.Split(TEXT("."), &PackageName, &ObjectName))
		{
			OutObjectPath = Path;
		}
		else
		{
			OutObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid long package name."), *PackageName);
			return false;
		}
		return true;
	}

	bool IsUnderGameRoot(const FString& PackageOrObjectPath)
	{
		return PackageOrObjectPath.StartsWith(TEXT("/Game/"));
	}

	UObject* LoadAssetChecked(const FString& Path, UClass* ExpectedClass, bool bForWrite, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPath(Path, ObjectPath, OutError))
		{
			return nullptr;
		}
		if (bForWrite && !IsUnderGameRoot(ObjectPath))
		{
			OutError = FString::Printf(TEXT("Refusing to modify '%s': only assets under /Game may be edited."), *ObjectPath);
			return nullptr;
		}
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("No asset found at '%s'."), *ObjectPath);
			return nullptr;
		}
		if (ExpectedClass && !Asset->IsA(ExpectedClass))
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a %s."), *ObjectPath, *Asset->GetClass()->GetName(), *ExpectedClass->GetName());
			return nullptr;
		}
		return Asset;
	}

	// ---- JSON ------------------------------------------------------------------------------

	TSharedRef<FJsonObject> VectorToJson(const FVector& Vector)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("x"), Vector.X);
		Out->SetNumberField(TEXT("y"), Vector.Y);
		Out->SetNumberField(TEXT("z"), Vector.Z);
		return Out;
	}

	TSharedRef<FJsonObject> RotatorToJson(const FRotator& Rotator)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Out->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Out->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Out;
	}

	TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("location"), VectorToJson(Transform.GetLocation()));
		Out->SetObjectField(TEXT("rotation"), RotatorToJson(Transform.Rotator()));
		Out->SetObjectField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
		return Out;
	}

	TSharedRef<FJsonObject> ActorToJson(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Actor->GetName());
		Out->SetStringField(TEXT("label"), Actor->GetActorLabel(/*bCreateIfNone*/ false));
		Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		Out->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());
		Out->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
		Out->SetObjectField(TEXT("rotation"), RotatorToJson(Actor->GetActorRotation()));
		Out->SetStringField(TEXT("guid"), Actor->GetActorGuid().IsValid() ? Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
		Out->SetStringField(TEXT("path"), Actor->GetPathName());
		TArray<FString> Tags;
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(Tag.ToString());
		}
		Out->SetArrayField(TEXT("tags"), ToJsonArray(Tags));
		return Out;
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

	bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (IsUnset(Trimmed))
		{
			OutObject = MakeShared<FJsonObject>();
			return true;
		}
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Expected a JSON object such as {\"Amount\": 5}, got: %s"), *Truncate(Trimmed, 200));
			return false;
		}
		return true;
	}

	FString JsonValueToText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return TEXT("none");
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			return Text;
		}
		// Wrap in an array so the condensed writer accepts scalars too, then strip the brackets.
		TArray<TSharedPtr<FJsonValue>> Wrapper = { Value };
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Wrapper, Writer);
		return Text.Mid(1, Text.Len() - 2);
	}

	// ---- Reflection ------------------------------------------------------------------------

	namespace
	{
		struct FPathSegment
		{
			FString Name;
			TArray<FString> Keys;
		};

		FString StripQuotes(const FString& In)
		{
			FString Out = In.TrimStartAndEnd();
			if (Out.Len() >= 2 && ((Out.StartsWith(TEXT("\"")) && Out.EndsWith(TEXT("\""))) || (Out.StartsWith(TEXT("'")) && Out.EndsWith(TEXT("'")))))
			{
				Out = Out.Mid(1, Out.Len() - 2);
			}
			return Out;
		}

		bool ParsePath(const FString& Path, TArray<FPathSegment>& Out, FString& OutError)
		{
			FPathSegment Current;
			int32 Index = 0;
			while (Index < Path.Len())
			{
				const TCHAR Char = Path[Index];
				if (Char == TEXT('.'))
				{
					Current.Name.TrimStartAndEndInline();
					if (Current.Name.IsEmpty())
					{
						OutError = FString::Printf(TEXT("Property path '%s' has an empty segment."), *Path);
						return false;
					}
					Out.Add(MoveTemp(Current));
					Current = FPathSegment();
					++Index;
					continue;
				}
				if (Char == TEXT('['))
				{
					const int32 Close = Path.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
					if (Close == INDEX_NONE || Current.Name.TrimStartAndEnd().IsEmpty())
					{
						OutError = FString::Printf(TEXT("Property path '%s' has a malformed [key]. Use forms like 'Items[2]', 'Items[Sword]' or 'Ammo[Rifle]'."), *Path);
						return false;
					}
					Current.Keys.Add(StripQuotes(Path.Mid(Index + 1, Close - Index - 1)));
					Index = Close + 1;
					continue;
				}
				if (Current.Keys.Num() > 0 && !FChar::IsWhitespace(Char))
				{
					OutError = FString::Printf(TEXT("Property path '%s': expected '.' or '[' after ']'."), *Path);
					return false;
				}
				Current.Name.AppendChar(Char);
				++Index;
			}
			Current.Name.TrimStartAndEndInline();
			if (Current.Name.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Property path '%s' is empty or ends with '.'."), *Path);
				return false;
			}
			Out.Add(MoveTemp(Current));
			return true;
		}

		FString ListPropertyNames(const UStruct* Struct)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(Struct); It && Names.Num() < 40; ++It)
			{
				Names.Add(It->GetAuthoredName());
			}
			return FString::Join(Names, TEXT(", "));
		}

		/** Text form of a name-like value used for [key] matching: names, strings and texts unquoted. */
		FString KeyText(const FProperty* Property, const void* ValuePtr)
		{
			if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
			{
				return NameProp->GetPropertyValue(ValuePtr).ToString();
			}
			if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
			{
				return StrProp->GetPropertyValue(ValuePtr);
			}
			if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
			{
				return TextProp->GetPropertyValue(ValuePtr).ToString();
			}
			if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
			{
				const UObject* Object = ObjectProp->GetObjectPropertyValue(ValuePtr);
				return Object ? Object->GetName() : FString();
			}
			FString Text;
			Property->ExportText_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
			return StripQuotes(Text);
		}

		bool ElementMatchesKey(const FProperty* Inner, const void* ElementPtr, const FString& Key)
		{
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Inner))
			{
				for (const TCHAR* Field : { TEXT("Name"), TEXT("Id"), TEXT("Key"), TEXT("Tag"), TEXT("TagName") })
				{
					if (const FProperty* KeyProp = FindPropertyByName(StructProp->Struct, Field))
					{
						if (KeyText(KeyProp, KeyProp->ContainerPtrToValuePtr<void>(ElementPtr)).Equals(Key, ESearchCase::IgnoreCase))
						{
							return true;
						}
					}
				}
				return false;
			}
			if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Inner))
			{
				const UObject* Object = ObjectProp->GetObjectPropertyValue(ElementPtr);
				if (!Object)
				{
					return false;
				}
				if (Object->GetName().Equals(Key, ESearchCase::IgnoreCase))
				{
					return true;
				}
				const AActor* Actor = Cast<AActor>(Object);
				return Actor && Actor->GetActorLabel(false).Equals(Key, ESearchCase::IgnoreCase);
			}
			return KeyText(Inner, ElementPtr).Equals(Key, ESearchCase::IgnoreCase);
		}

		bool IsIntegerText(const FString& Text)
		{
			if (Text.IsEmpty())
			{
				return false;
			}
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				if (!FChar::IsDigit(Text[Index]))
				{
					return false;
				}
			}
			return true;
		}

		/** Member names at the top level of Unreal struct text, e.g. "(X=1,Y=(A=2))" -> X, Y. */
		TArray<FString> TopLevelStructKeys(const FString& Text)
		{
			TArray<FString> Keys;
			int32 Depth = 0;
			bool bInQuotes = false;
			FString Current;
			bool bInKey = true;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				const TCHAR Char = Text[Index];
				if (Char == TEXT('"'))
				{
					bInQuotes = !bInQuotes;
				}
				if (bInQuotes)
				{
					continue;
				}
				if (Char == TEXT('(') || Char == TEXT('['))
				{
					++Depth;
					if (Depth == 1)
					{
						Current.Reset();
						bInKey = true;
					}
					continue;
				}
				if (Char == TEXT(')') || Char == TEXT(']'))
				{
					--Depth;
					continue;
				}
				if (Depth != 1)
				{
					continue;
				}
				if (Char == TEXT(','))
				{
					Current.Reset();
					bInKey = true;
				}
				else if (Char == TEXT('=') && bInKey)
				{
					Current.TrimStartAndEndInline();
					if (!Current.IsEmpty())
					{
						Keys.Add(Current);
					}
					bInKey = false;
				}
				else if (bInKey)
				{
					Current.AppendChar(Char);
				}
			}
			return Keys;
		}

		/** Imports a key or value into memory that the caller has already initialized. */
		bool ImportIntoScratch(FProperty* Property, void* Scratch, UObject* Owner, const FString& Text, UWorld* PIEWorld, FString& OutError);

		bool ImportKey(FProperty* KeyProp, void* KeyPtr, const FString& Key, FString& OutError)
		{
			return ImportIntoScratch(KeyProp, KeyPtr, nullptr, Key, nullptr, OutError);
		}

		/** Records ImportText errors instead of letting them go to the log. */
		class FImportErrorCollector : public FOutputDevice
		{
		public:
			FString Errors;

			virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
			{
				if (!Errors.IsEmpty())
				{
					Errors += TEXT("; ");
				}
				Errors += Message;
			}
		};

		bool ImportIntoScratch(FProperty* Property, void* Scratch, UObject* Owner, const FString& InText, UWorld* PIEWorld, FString& OutError)
		{
			const FString Text = InText.TrimStartAndEnd();
			const FString TypeName = PropertyTypeToString(Property);

			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
			{
				if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"))
				{
					BoolProp->SetPropertyValue(Scratch, true);
					return true;
				}
				if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text == TEXT("0"))
				{
					BoolProp->SetPropertyValue(Scratch, false);
					return true;
				}
				OutError = FString::Printf(TEXT("'%s' is not a bool. Use true or false."), *Text);
				return false;
			}

			if (FNumericProperty* NumericProp = CastField<FNumericProperty>(Property); NumericProp && !NumericProp->IsEnum())
			{
				double Number = 0.0;
				if (!FCString::IsNumeric(*Text) || !LexTryParseString(Number, *Text))
				{
					OutError = FString::Printf(TEXT("'%s' is not a number (property type %s)."), *Text, *TypeName);
					return false;
				}
				if (NumericProp->IsInteger())
				{
					if (FMath::Frac(Number) != 0.0)
					{
						OutError = FString::Printf(TEXT("'%s' is not a whole number (property type %s)."), *Text, *TypeName);
						return false;
					}
					NumericProp->SetIntPropertyValue(Scratch, static_cast<int64>(Number));
				}
				else
				{
					NumericProp->SetFloatingPointPropertyValue(Scratch, Number);
				}
				return true;
			}

			if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
			{
				StrProp->SetPropertyValue(Scratch, StripQuotes(InText));
				return true;
			}
			if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
			{
				NameProp->SetPropertyValue(Scratch, FName(*StripQuotes(Text)));
				return true;
			}
			if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
			{
				TextProp->SetPropertyValue(Scratch, FText::FromString(StripQuotes(InText)));
				return true;
			}

			// Hard object references may also name a live PIE actor or "Actor:Component".
			FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property);
			if (ObjectProp && !Property->IsA<FSoftObjectProperty>() && !Property->IsA<FClassProperty>())
			{
				if (Text.IsEmpty() || Text.Equals(TEXT("none"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("null"), ESearchCase::IgnoreCase))
				{
					ObjectProp->SetObjectPropertyValue(Scratch, nullptr);
					return true;
				}
				UObject* Resolved = nullptr;
				if (PIEWorld && !Text.StartsWith(TEXT("/")))
				{
					FString ActorPart = Text;
					FString ComponentPart;
					Text.Split(TEXT(":"), &ActorPart, &ComponentPart);
					FString ResolveError;
					Resolved = ResolvePIETarget(PIEWorld, ActorPart, ComponentPart.IsEmpty() ? FString(TEXT("none")) : ComponentPart, ResolveError);
					if (!Resolved)
					{
						OutError = FString::Printf(TEXT("Could not resolve object '%s': %s"), *Text, *ResolveError);
						return false;
					}
				}
				else
				{
					Resolved = StaticLoadObject(ObjectProp->PropertyClass, nullptr, *StripQuotes(Text), nullptr, LOAD_NoWarn);
					if (!Resolved)
					{
						OutError = FString::Printf(TEXT("No %s found at '%s'."), *ObjectProp->PropertyClass->GetName(), *Text);
						return false;
					}
				}
				if (!Resolved->IsA(ObjectProp->PropertyClass))
				{
					OutError = FString::Printf(TEXT("'%s' is a %s, but the property needs a %s."), *Text, *Resolved->GetClass()->GetName(), *ObjectProp->PropertyClass->GetName());
					return false;
				}
				ObjectProp->SetObjectPropertyValue(Scratch, Resolved);
				return true;
			}

			// JSON objects/arrays for structs, arrays, maps and sets.
			if (Text.StartsWith(TEXT("{")) || Text.StartsWith(TEXT("[")))
			{
				TSharedPtr<FJsonObject> Wrapper;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString::Printf(TEXT("{\"v\":%s}"), *Text));
				if (!FJsonSerializer::Deserialize(Reader, Wrapper) || !Wrapper.IsValid() || !Wrapper->HasField(TEXT("v")))
				{
					OutError = FString::Printf(TEXT("'%s' looks like JSON but does not parse."), *Truncate(Text, 200));
					return false;
				}
				FText FailReason;
				if (!FJsonObjectConverter::JsonValueToUProperty(Wrapper->TryGetField(TEXT("v")), Property, Scratch, 0, 0, /*bStrictMode*/ false, &FailReason))
				{
					OutError = FString::Printf(TEXT("Could not convert JSON to %s: %s"), *TypeName, *FailReason.ToString());
					return false;
				}
				return true;
			}

			if (const FStructProperty* StructProp = CastField<FStructProperty>(Property); StructProp && Text.StartsWith(TEXT("(")))
			{
				// ImportText silently skips unknown members ("(X=1,Q=2)" sets only X); reject them instead.
				TArray<FString> Unknown;
				for (const FString& Key : TopLevelStructKeys(Text))
				{
					if (!FindPropertyByName(StructProp->Struct, Key))
					{
						Unknown.Add(Key);
					}
				}
				if (Unknown.Num() > 0)
				{
					OutError = FString::Printf(TEXT("'%s' is not a valid %s value: unknown member(s) %s. Members: %s"),
						*Truncate(Text, 200), *TypeName, *FString::Join(Unknown, TEXT(", ")), *ListPropertyNames(StructProp->Struct));
					return false;
				}
			}

			FImportErrorCollector Errors;
			const TCHAR* End = Property->ImportText_Direct(*Text, Scratch, Owner, PPF_None, &Errors);
			if (!End || !Errors.Errors.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid %s value%s%s"), *Truncate(Text, 200), *TypeName,
					Errors.Errors.IsEmpty() ? TEXT(".") : TEXT(": "), *Errors.Errors);
				return false;
			}
			if (!FString(End).TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid %s value: unexpected trailing text '%s'."), *Truncate(Text, 200), *TypeName, *Truncate(FString(End), 50));
				return false;
			}
			return true;
		}
	}

	FProperty* FindPropertyByName(const UStruct* Struct, const FString& Name)
	{
		if (!Struct)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool ResolvePropertyPath(UObject* Root, const FString& Path, bool bForWrite, FPropertyLocation& Out, FString& OutError)
	{
		TArray<FPathSegment> Segments;
		if (!ParsePath(Path.TrimStartAndEnd(), Segments, OutError))
		{
			return false;
		}

		const UStruct* Struct = Root->GetClass();
		void* Container = Root;
		UObject* Owner = Root;

		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			const FPathSegment& Segment = Segments[SegmentIndex];
			FProperty* Property = FindPropertyByName(Struct, Segment.Name);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property '%s'. Properties: %s"), *Struct->GetName(), *Segment.Name, *ListPropertyNames(Struct));
				return false;
			}

			void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			FProperty* Current = Property;
			bool bWritable = true;

			for (int32 KeyIndex = 0; KeyIndex < Segment.Keys.Num(); ++KeyIndex)
			{
				const FString& Key = Segment.Keys[KeyIndex];
				if (KeyIndex == 0 && Property->ArrayDim > 1)
				{
					if (!IsIntegerText(Key) || FCString::Atoi(*Key) >= Property->ArrayDim)
					{
						OutError = FString::Printf(TEXT("'%s' is a fixed array of %d elements; '%s' is not a valid index."), *Segment.Name, Property->ArrayDim, *Key);
						return false;
					}
					Value = Property->ContainerPtrToValuePtr<void>(Container, FCString::Atoi(*Key));
					bWritable = false;
				}
				else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Current))
				{
					FScriptArrayHelper Helper(ArrayProp, Value);
					int32 ElementIndex = INDEX_NONE;
					if (IsIntegerText(Key))
					{
						ElementIndex = FCString::Atoi(*Key);
						if (!Helper.IsValidIndex(ElementIndex))
						{
							OutError = FString::Printf(TEXT("Index %d is out of range: '%s' has %d elements."), ElementIndex, *Segment.Name, Helper.Num());
							return false;
						}
					}
					else
					{
						for (int32 Index = 0; Index < Helper.Num(); ++Index)
						{
							if (ElementMatchesKey(ArrayProp->Inner, Helper.GetRawPtr(Index), Key))
							{
								ElementIndex = Index;
								break;
							}
						}
						if (ElementIndex == INDEX_NONE)
						{
							OutError = FString::Printf(TEXT("No element of '%s' (%d elements) has the name key '%s'. Name keys match a struct's Name/Id/Key/Tag field or an object's name; use a number for a plain index."), *Segment.Name, Helper.Num(), *Key);
							return false;
						}
					}
					Value = Helper.GetRawPtr(ElementIndex);
					Current = ArrayProp->Inner;
				}
				else if (FMapProperty* MapProp = CastField<FMapProperty>(Current))
				{
					FProperty* KeyProp = MapProp->KeyProp;
					void* KeyScratch = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
					KeyProp->InitializeValue(KeyScratch);
					ON_SCOPE_EXIT
					{
						KeyProp->DestroyValue(KeyScratch);
						FMemory::Free(KeyScratch);
					};
					FString KeyError;
					if (!ImportKey(KeyProp, KeyScratch, Key, KeyError))
					{
						OutError = FString::Printf(TEXT("'%s' is not a valid key for '%s': %s"), *Key, *Segment.Name, *KeyError);
						return false;
					}
					FScriptMapHelper Helper(MapProp, Value);
					uint8* Found = Helper.FindValueFromHash(KeyScratch);
					if (!Found)
					{
						OutError = FString::Printf(TEXT("Map '%s' has no key '%s' (%d entries)."), *Segment.Name, *Key, Helper.Num());
						return false;
					}
					Value = Found;
					Current = MapProp->ValueProp;
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' is a %s; only arrays and maps accept [key]."), *Segment.Name, *PropertyTypeToString(Current));
					return false;
				}
			}

			if (SegmentIndex == Segments.Num() - 1)
			{
				Out.Property = Current;
				Out.ValuePtr = Value;
				Out.Owner = Owner;
				Out.bWritable = bWritable;
				return true;
			}

			if (FStructProperty* StructProp = CastField<FStructProperty>(Current))
			{
				Struct = StructProp->Struct;
				Container = Value;
			}
			else if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Current); ObjectProp && !Current->IsA<FSoftObjectProperty>())
			{
				UObject* Object = ObjectProp->GetObjectPropertyValue(Value);
				if (!Object)
				{
					OutError = FString::Printf(TEXT("'%s' is None, so '%s' cannot be followed."), *Segment.Name, *Segments[SegmentIndex + 1].Name);
					return false;
				}
				if (bForWrite && !IsPIEObject(Object))
				{
					OutError = FString::Printf(TEXT("'%s' points to %s, which is not a PIE-world object (an asset or editor object). Writes are only allowed on live PIE objects."), *Segment.Name, *Object->GetPathName());
					return false;
				}
				Struct = Object->GetClass();
				Container = Object;
				Owner = Object;
			}
			else
			{
				OutError = FString::Printf(TEXT("'%s' is a %s and has no members to follow."), *Segment.Name, *PropertyTypeToString(Current));
				return false;
			}
		}

		OutError = TEXT("Empty property path.");
		return false;
	}

	FString PropertyTypeToString(const FProperty* Property)
	{
		FString Extended;
		const FString Type = Property->GetCPPType(&Extended);
		return Type + Extended;
	}

	FString PropertyValueToText(const FProperty* Property, const void* ValuePtr, UObject* Owner)
	{
		FString Text;
		Property->ExportText_Direct(Text, ValuePtr, nullptr, Owner, PPF_None);
		return Text;
	}

	TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Property, const void* ValuePtr)
	{
		TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(const_cast<FProperty*>(Property), ValuePtr);
		return Value.IsValid() ? Value : MakeShared<FJsonValueString>(PropertyValueToText(Property, ValuePtr, nullptr));
	}

	bool ImportPropertyValue(FProperty* Property, void* ValuePtr, UObject* Owner, const FString& Text, UWorld* PIEWorld, FString& OutError)
	{
		if (Property->ArrayDim > 1)
		{
			OutError = FString::Printf(TEXT("'%s' is a fixed-size C array; writing it is not supported."), *Property->GetName());
			return false;
		}

		// Import into a scratch copy so a failed or partial parse never touches the live value.
		void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Scratch);
		Property->CopyCompleteValue(Scratch, ValuePtr);
		ON_SCOPE_EXIT
		{
			Property->DestroyValue(Scratch);
			FMemory::Free(Scratch);
		};

		if (!ImportIntoScratch(Property, Scratch, Owner, Text, PIEWorld, OutError))
		{
			return false;
		}
		Property->CopyCompleteValue(ValuePtr, Scratch);
		return true;
	}
}
