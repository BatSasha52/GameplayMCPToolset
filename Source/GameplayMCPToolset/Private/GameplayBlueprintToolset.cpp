// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayBlueprintToolset.h"

#include "GameplayMCPHelpers.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "SubobjectData.h"
#include "SubobjectDataHandle.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GameplayMCPBlueprint"

namespace
{
	/** The subobject tree of a Blueprint as the Components panel shows it. Handles[0..] include the actor itself. */
	struct FComponentTree
	{
		UBlueprint* Blueprint = nullptr;
		TArray<FSubobjectDataHandle> Handles;
		FSubobjectDataHandle ActorHandle;
	};

	UBlueprint* LoadActorBlueprint(const FString& Path, bool bForWrite, FString& OutError)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(GameplayMCP::LoadAssetChecked(Path, UBlueprint::StaticClass(), bForWrite, OutError));
		if (!Blueprint)
		{
			return nullptr;
		}
		if (!Blueprint->SimpleConstructionScript || !Blueprint->ParentClass || !Blueprint->ParentClass->IsChildOf(AActor::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not an Actor Blueprint, so it has no components."), *Blueprint->GetPathName());
			return nullptr;
		}
		if (!Blueprint->GeneratedClass)
		{
			OutError = FString::Printf(TEXT("'%s' has never been compiled; open and compile it once in the editor."), *Blueprint->GetPathName());
			return nullptr;
		}
		return Blueprint;
	}

	FString ComponentName(const FSubobjectData* Data);

	bool GatherTree(UBlueprint* Blueprint, FComponentTree& Out, FString& OutError)
	{
		USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
		if (!Subsystem)
		{
			OutError = TEXT("The Subobject Data subsystem is not available.");
			return false;
		}
		Out.Blueprint = Blueprint;
		Out.Handles.Reset();
		TArray<FSubobjectDataHandle> Gathered;
		Subsystem->K2_GatherSubobjectDataForBlueprint(Blueprint, Gathered);
		// After a reparent the gathered list can contain the moved components twice; keep the first of each.
		TSet<FString> Seen;
		for (const FSubobjectDataHandle& Handle : Gathered)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (Data && Data->IsComponent())
			{
				bool bAlreadySeen = false;
				Seen.Add(ComponentName(Data), &bAlreadySeen);
				if (bAlreadySeen)
				{
					continue;
				}
			}
			Out.Handles.Add(Handle);
		}
		for (const FSubobjectDataHandle& Handle : Out.Handles)
		{
			if (const FSubobjectData* Data = Handle.GetData(); Data && Data->IsRootActor())
			{
				Out.ActorHandle = Handle;
				break;
			}
		}
		if (!Out.ActorHandle.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not read the components of '%s'."), *Blueprint->GetPathName());
			return false;
		}
		return true;
	}

	FString ComponentName(const FSubobjectData* Data)
	{
		const FName Variable = Data->GetVariableName();
		if (!Variable.IsNone())
		{
			return Variable.ToString();
		}
		const UObject* Object = Data->GetObject();
		return Object ? Object->GetName() : Data->GetDisplayString();
	}

	FString ComponentSource(const FSubobjectData* Data)
	{
		if (Data->IsNativeComponent())
		{
			return TEXT("native");
		}
		return Data->IsInheritedComponent() ? TEXT("inherited") : TEXT("added");
	}

	TArray<FString> ListNames(const FComponentTree& Tree)
	{
		TArray<FString> Names;
		for (const FSubobjectDataHandle& Handle : Tree.Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (Data && Data->IsComponent())
			{
				Names.Add(ComponentName(Data));
			}
		}
		return Names;
	}

	FSubobjectDataHandle FindComponent(const FComponentTree& Tree, const FString& InName, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		for (const FSubobjectDataHandle& Handle : Tree.Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (Data && Data->IsComponent() && ComponentName(Data).Equals(Name, ESearchCase::IgnoreCase))
			{
				return Handle;
			}
		}
		// Fall back to the template object's name, e.g. 'CharacterMesh0' for ACharacter's 'Mesh'.
		for (const FSubobjectDataHandle& Handle : Tree.Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			const UObject* Object = Data ? Data->GetObject() : nullptr;
			if (Data && Data->IsComponent() && Object && Object->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Handle;
			}
		}
		OutError = FString::Printf(TEXT("'%s' has no component named '%s'. Components: %s."), *Tree.Blueprint->GetName(), *Name, *FString::Join(ListNames(Tree), TEXT(", ")));
		return FSubobjectDataHandle::InvalidHandle;
	}

	FSubobjectDataHandle FindSceneRoot(const FComponentTree& Tree)
	{
		for (const FSubobjectDataHandle& Handle : Tree.Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (Data && Data->IsComponent() && Data->IsSceneComponent() && Data->IsRootComponent())
			{
				return Handle;
			}
		}
		return FSubobjectDataHandle::InvalidHandle;
	}

	TSharedRef<FJsonObject> ComponentToJson(const FSubobjectData* Data)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), ComponentName(Data));
		const UActorComponent* Template = Data->GetComponentTemplate();
		Out->SetStringField(TEXT("class"), Template ? Template->GetClass()->GetName() : FString());

		const FSubobjectData* Parent = Data->GetParentHandle().GetData();
		Out->SetStringField(TEXT("parent"), Parent && Parent->IsComponent() ? ComponentName(Parent) : FString());
		TArray<FString> Children;
		for (const FSubobjectDataHandle& Child : Data->GetChildrenHandles())
		{
			if (const FSubobjectData* ChildData = Child.GetData(); ChildData && ChildData->IsComponent())
			{
				Children.Add(ComponentName(ChildData));
			}
		}
		Out->SetArrayField(TEXT("children"), GameplayMCP::ToJsonArray(Children));
		Out->SetStringField(TEXT("source"), ComponentSource(Data));
		Out->SetBoolField(TEXT("is_scene"), Data->IsSceneComponent());
		Out->SetBoolField(TEXT("is_root"), Data->IsSceneComponent() && Data->IsRootComponent());
		Out->SetStringField(TEXT("socket"), Data->GetSocketFName().IsNone() ? FString() : Data->GetSocketFName().ToString());
		Out->SetBoolField(TEXT("can_rename"), Data->CanRename());
		Out->SetBoolField(TEXT("can_remove"), Data->CanDelete());
		Out->SetBoolField(TEXT("can_reparent"), Data->CanReparent());
		return Out;
	}

	/** A clear reason why a component may not be changed, or empty if it is one this Blueprint added. */
	FString NotEditableReason(const FComponentTree& Tree, const FSubobjectData* Data, const TCHAR* Verb)
	{
		const FString Name = ComponentName(Data);
		if (Data->IsNativeComponent())
		{
			const UActorComponent* Template = Data->GetComponentTemplate();
			const UClass* Owner = Template && Template->GetOuter() ? Template->GetOuter()->GetClass() : nullptr;
			return FString::Printf(TEXT("'%s' is a native component declared in C++%s; it cannot be %s from a Blueprint. It can still be used as a parent."),
				*Name, Owner ? *FString::Printf(TEXT(" (%s)"), *Owner->GetName()) : TEXT(""), Verb);
		}
		if (Data->IsInheritedComponent())
		{
			const UBlueprint* Source = Data->GetBlueprint();
			return FString::Printf(TEXT("'%s' is inherited from the parent Blueprint%s; it cannot be %s here. Edit it in that Blueprint."),
				*Name, Source && Source != Tree.Blueprint ? *FString::Printf(TEXT(" %s"), *Source->GetPathName()) : TEXT(""), Verb);
		}
		return FString();
	}

	bool IsDescendant(const FSubobjectData* Ancestor, const FSubobjectDataHandle& Candidate)
	{
		for (const FSubobjectDataHandle& Child : Ancestor->GetChildrenHandles())
		{
			if (Child == Candidate)
			{
				return true;
			}
			if (const FSubobjectData* ChildData = Child.GetData(); ChildData && IsDescendant(ChildData, Candidate))
			{
				return true;
			}
		}
		return false;
	}

	UClass* ResolveComponentClass(const FString& InName, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		UClass* Class = nullptr;
		if (Name.StartsWith(TEXT("/Script/")))
		{
			Class = FindObject<UClass>(nullptr, *Name);
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			FString ObjectPath;
			if (GameplayMCP::NormalizeObjectPath(Name, ObjectPath, OutError))
			{
				UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
				if (UBlueprint* ComponentBlueprint = Cast<UBlueprint>(Loaded))
				{
					Class = ComponentBlueprint->GeneratedClass;
				}
				else
				{
					Class = Cast<UClass>(Loaded);
				}
			}
		}
		else
		{
			FString Bare = Name;
			if (Bare.Len() > 1 && Bare[0] == TEXT('U') && FChar::IsUpper(Bare[1]))
			{
				Bare.RightChopInline(1);
			}
			// 'PointLight' must find UPointLightComponent, not the APointLight actor: prefer a component class.
			UClass* NonComponent = nullptr;
			for (const FString& Candidate : { Bare, Bare + TEXT("Component") })
			{
				UClass* Found = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::NativeFirst);
				if (Found && Found->IsChildOf(UActorComponent::StaticClass()) && !Found->HasAnyClassFlags(CLASS_Abstract))
				{
					Class = Found;
					break;
				}
				NonComponent = NonComponent ? NonComponent : Found;
			}
			if (!Class)
			{
				// Report the exact match (abstract component or not a component) with the right error below.
				Class = NonComponent ? NonComponent : FindFirstObject<UClass>(*(Bare + TEXT("Component")), EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!Class)
		{
			OutError = FString::Printf(TEXT("No class '%s' found. Use a component class name such as 'StaticMeshComponent' or a path such as '/Script/Engine.SphereComponent' or '/Game/Components/BPC_Health'."), *Name);
			return nullptr;
		}
		if (!Class->IsChildOf(UActorComponent::StaticClass()))
		{
			OutError = FString::Printf(TEXT("%s is not a component class."), *Class->GetName());
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("%s is abstract; pick a concrete subclass."), *Class->GetName());
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("%s is deprecated or outdated."), *Class->GetName());
			return nullptr;
		}
		if (!FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(Class))
		{
			OutError = FString::Printf(TEXT("%s is not marked BlueprintSpawnableComponent, so the editor does not allow adding it to a Blueprint."), *Class->GetName());
			return nullptr;
		}
		return Class;
	}

	bool ValidateNewName(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		if (Name.TrimStartAndEnd() != Name || Name.IsEmpty())
		{
			OutError = TEXT("Component name is empty or has leading/trailing spaces.");
			return false;
		}
		FKismetNameValidator Validator(Blueprint);
		const EValidatorResult Result = Validator.IsValid(Name);
		if (Result != EValidatorResult::Ok)
		{
			OutError = FString::Printf(TEXT("'%s' cannot be used: %s"), *Name, *INameValidatorInterface::GetErrorText(Name, Result).ToString());
			return false;
		}
		return true;
	}

	FString StatusName(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_UpToDate:             return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_Error:                return TEXT("error");
		case BS_Dirty:                return TEXT("dirty");
		default:                      return TEXT("unknown");
		}
	}

	/** Compiles, marks dirty and builds the common result, describing ComponentName as it is after the change. */
	FGameplayMCPResult FinishEdit(UBlueprint* Blueprint, const FString& ComponentNameAfter, TSharedRef<FJsonObject> Payload)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
		Blueprint->MarkPackageDirty();

		Payload->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Payload->SetStringField(TEXT("compile_status"), StatusName(Blueprint->Status));
		Payload->SetBoolField(TEXT("dirty"), Blueprint->GetOutermost()->IsDirty());
		if (!ComponentNameAfter.IsEmpty())
		{
			FComponentTree Tree;
			FString Error;
			if (GatherTree(Blueprint, Tree, Error))
			{
				FSubobjectDataHandle Handle = FindComponent(Tree, ComponentNameAfter, Error);
				if (const FSubobjectData* Data = Handle.GetData())
				{
					Payload->SetObjectField(TEXT("component"), ComponentToJson(Data));
				}
			}
		}
		return GameplayMCP::Ok(Payload);
	}
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_list_components(const FString& blueprint_path)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlueprint* Blueprint = LoadActorBlueprint(blueprint_path, /*bForWrite*/ false, Error);
	FComponentTree Tree;
	if (!Blueprint || !GatherTree(Blueprint, Tree, Error))
	{
		return GameplayMCP::Fail(Error);
	}

	TArray<TSharedRef<FJsonObject>> Components;
	for (const FSubobjectDataHandle& Handle : Tree.Handles)
	{
		const FSubobjectData* Data = Handle.GetData();
		if (Data && Data->IsComponent())
		{
			Components.Add(ComponentToJson(Data));
		}
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Payload->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
	const FSubobjectData* Root = FindSceneRoot(Tree).GetData();
	Payload->SetStringField(TEXT("scene_root"), Root ? ComponentName(Root) : FString());
	Payload->SetArrayField(TEXT("components"), GameplayMCP::ToJsonArray(Components));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_add_component(const FString& blueprint_path, const FString& component_class, const FString& name, const FString& parent)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlueprint* Blueprint = LoadActorBlueprint(blueprint_path, /*bForWrite*/ true, Error);
	FComponentTree Tree;
	if (!Blueprint || !GatherTree(Blueprint, Tree, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	UClass* Class = ResolveComponentClass(component_class, Error);
	if (!Class || !ValidateNewName(Blueprint, name, Error))
	{
		return GameplayMCP::Fail(Error);
	}

	const bool bIsScene = Class->IsChildOf(USceneComponent::StaticClass());
	FSubobjectDataHandle ParentHandle;
	if (!bIsScene)
	{
		if (!GameplayMCP::IsUnset(parent))
		{
			return GameplayMCP::Fail(FString::Printf(TEXT("%s is not a scene component, so it has no transform and cannot be attached to '%s'. Pass parent='none'."), *Class->GetName(), *parent));
		}
		ParentHandle = Tree.ActorHandle;
	}
	else if (GameplayMCP::IsUnset(parent))
	{
		// Attach under the existing scene root (native, inherited or added); never replace the root.
		ParentHandle = FindSceneRoot(Tree);
		if (!ParentHandle.IsValid())
		{
			ParentHandle = Tree.ActorHandle;
		}
	}
	else
	{
		ParentHandle = FindComponent(Tree, parent, Error);
		const FSubobjectData* ParentData = ParentHandle.GetData();
		if (!ParentData)
		{
			return GameplayMCP::Fail(Error);
		}
		if (!ParentData->IsSceneComponent())
		{
			return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is not a scene component and cannot have children."), *ComponentName(ParentData)));
		}
	}

	const FScopedTransaction Transaction(FText::Format(LOCTEXT("AddComponent", "Add component {0}"), FText::FromString(name)));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();

	FAddNewSubobjectParams Params;
	Params.ParentHandle = ParentHandle;
	Params.NewClass = Class;
	Params.BlueprintContext = Blueprint;
	FText FailReason;
	USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
	const FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(Params, FailReason);
	if (!NewHandle.IsValid())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor refused to add %s: %s"), *Class->GetName(), *FailReason.ToString()));
	}
	if (!Subsystem->RenameSubobject(NewHandle, FText::FromString(name)))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Added a %s but could not name it '%s' (it keeps its default name; undo with Ctrl+Z)."), *Class->GetName(), *name));
	}
	return FinishEdit(Blueprint, name, MakeShared<FJsonObject>());
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_remove_component(const FString& blueprint_path, const FString& name)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlueprint* Blueprint = LoadActorBlueprint(blueprint_path, /*bForWrite*/ true, Error);
	FComponentTree Tree;
	if (!Blueprint || !GatherTree(Blueprint, Tree, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	const FSubobjectDataHandle Handle = FindComponent(Tree, name, Error);
	const FSubobjectData* Data = Handle.GetData();
	if (!Data)
	{
		return GameplayMCP::Fail(Error);
	}
	const FString Reason = NotEditableReason(Tree, Data, TEXT("removed"));
	if (!Reason.IsEmpty())
	{
		return GameplayMCP::Fail(Reason);
	}
	TArray<FString> Children;
	for (const FSubobjectDataHandle& Child : Data->GetChildrenHandles())
	{
		if (const FSubobjectData* ChildData = Child.GetData(); ChildData && ChildData->IsComponent())
		{
			Children.Add(ComponentName(ChildData));
		}
	}
	if (Children.Num() > 0)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' still has children (%s). Reparent or remove them first."), *ComponentName(Data), *FString::Join(Children, TEXT(", "))));
	}
	if (!Data->CanDelete())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor does not allow removing '%s'."), *ComponentName(Data)));
	}

	const FString Removed = ComponentName(Data);
	const FScopedTransaction Transaction(FText::Format(LOCTEXT("RemoveComponent", "Remove component {0}"), FText::FromString(Removed)));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();
	if (USubobjectDataSubsystem::Get()->DeleteSubobject(Tree.ActorHandle, Handle, Blueprint) == 0)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor did not remove '%s'."), *Removed));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("removed"), Removed);
	return FinishEdit(Blueprint, FString(), Payload);
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_rename_component(const FString& blueprint_path, const FString& name, const FString& new_name)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlueprint* Blueprint = LoadActorBlueprint(blueprint_path, /*bForWrite*/ true, Error);
	FComponentTree Tree;
	if (!Blueprint || !GatherTree(Blueprint, Tree, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	const FSubobjectDataHandle Handle = FindComponent(Tree, name, Error);
	const FSubobjectData* Data = Handle.GetData();
	if (!Data)
	{
		return GameplayMCP::Fail(Error);
	}
	const FString Reason = NotEditableReason(Tree, Data, TEXT("renamed"));
	if (!Reason.IsEmpty())
	{
		return GameplayMCP::Fail(Reason);
	}
	if (ComponentName(Data).Equals(new_name, ESearchCase::CaseSensitive))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' already has that name."), *new_name));
	}
	USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
	FText RenameError;
	if (new_name.TrimStartAndEnd() != new_name || !Subsystem->IsValidRename(Handle, FText::FromString(new_name), RenameError))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' cannot be used: %s"), *new_name, RenameError.IsEmpty() ? TEXT("leading or trailing spaces") : *RenameError.ToString()));
	}

	const FScopedTransaction Transaction(FText::Format(LOCTEXT("RenameComponent", "Rename component {0}"), FText::FromString(new_name)));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();
	if (!Subsystem->RenameSubobject(Handle, FText::FromString(new_name)))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor did not rename '%s'."), *name));
	}
	return FinishEdit(Blueprint, new_name, MakeShared<FJsonObject>());
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_reparent_component(const FString& blueprint_path, const FString& name, const FString& new_parent)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlueprint* Blueprint = LoadActorBlueprint(blueprint_path, /*bForWrite*/ true, Error);
	FComponentTree Tree;
	if (!Blueprint || !GatherTree(Blueprint, Tree, Error))
	{
		return GameplayMCP::Fail(Error);
	}
	const FSubobjectDataHandle Handle = FindComponent(Tree, name, Error);
	const FSubobjectData* Data = Handle.GetData();
	if (!Data)
	{
		return GameplayMCP::Fail(Error);
	}
	const FSubobjectDataHandle ParentHandle = FindComponent(Tree, new_parent, Error);
	const FSubobjectData* ParentData = ParentHandle.GetData();
	if (!ParentData)
	{
		return GameplayMCP::Fail(Error);
	}
	const FString Reason = NotEditableReason(Tree, Data, TEXT("moved"));
	if (!Reason.IsEmpty())
	{
		return GameplayMCP::Fail(Reason);
	}
	if (!Data->IsSceneComponent() || !ParentData->IsSceneComponent())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Both '%s' and '%s' must be scene components to attach one to the other."), *ComponentName(Data), *ComponentName(ParentData)));
	}
	if (Data->IsRootComponent())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is the scene root; the root cannot be attached under another component."), *ComponentName(Data)));
	}
	if (Handle == ParentHandle || IsDescendant(Data, ParentHandle))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Cannot attach '%s' to '%s': that would make it its own ancestor."), *ComponentName(Data), *ComponentName(ParentData)));
	}
	if (Data->GetParentHandle() == ParentHandle)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("'%s' is already attached to '%s'."), *ComponentName(Data), *ComponentName(ParentData)));
	}
	if (!Data->CanReparent())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor does not allow moving '%s'."), *ComponentName(Data)));
	}

	const FScopedTransaction Transaction(FText::Format(LOCTEXT("ReparentComponent", "Attach {0} to {1}"), FText::FromString(ComponentName(Data)), FText::FromString(ComponentName(ParentData))));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();
	FReparentSubobjectParams Params;
	Params.NewParentHandle = ParentHandle;
	Params.BlueprintContext = Blueprint;
	// The subsystem requires a preview actor in a Blueprint context; the CDO keeps the relative transform as is.
	Params.ActorPreviewContext = Blueprint->GeneratedClass->GetDefaultObject<AActor>();
	const FString Moved = ComponentName(Data);
	if (!USubobjectDataSubsystem::Get()->ReparentSubobject(Params, Handle))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("The editor did not attach '%s' to '%s'."), *Moved, *new_parent));
	}
	return FinishEdit(Blueprint, Moved, MakeShared<FJsonObject>());
}

FGameplayMCPResult UGameplayBlueprintToolset::bp_save_asset(const FString& blueprint_path)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UObject* Asset = GameplayMCP::LoadAssetChecked(blueprint_path, nullptr, /*bForWrite*/ true, Error);
	if (!Asset)
	{
		return GameplayMCP::Fail(Error);
	}
	UPackage* Package = Asset->GetOutermost();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), Asset->GetPathName());
	if (!Package->IsDirty())
	{
		Payload->SetBoolField(TEXT("saved"), false);
		return GameplayMCP::Ok(Payload);
	}
	if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ true))
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Saving '%s' failed. The file may be read-only or locked by source control."), *Package->GetName()));
	}
	Payload->SetBoolField(TEXT("saved"), true);
	return GameplayMCP::Ok(Payload);
}

#undef LOCTEXT_NAMESPACE
