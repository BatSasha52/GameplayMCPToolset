// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "GameplayMCPTypes.h"

#include "GameplayBlueprintToolset.generated.h"

/**
 * Phase 4 - edit the components of an Actor Blueprint (its Simple Construction Script), the same way the Blueprint editor's Components panel does.
 * Components are always identified by name (their variable name, e.g. 'Mesh' or 'DefaultSceneRoot'), never by index.
 * Each component has a source: 'added' (in this Blueprint's construction script; can be renamed, reparented and removed),
 * 'inherited' (added by a parent Blueprint; edit it there) or 'native' (declared in C++; can be used as a parent but not renamed, moved or removed).
 * Every change is one undoable transaction, compiles the Blueprint and marks it dirty. Nothing is saved until bp_save_asset.
 * Only Blueprints under /Game can be changed. Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UGameplayBlueprintToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Lists every component of an Actor Blueprint with its class, parent and children, and whether it is added here, inherited from a parent Blueprint or native (C++).
	 * @param blueprint_path Asset path of the Actor Blueprint, e.g. '/Game/Blueprints/BP_Door'.
	 * @return Result: {blueprint, parent_class, scene_root, components: [{name, class, parent, children, source, is_scene, is_root, socket, can_rename, can_remove, can_reparent}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_list_components(const FString& blueprint_path);

	/**
	 * Adds a component to an Actor Blueprint's construction script, then compiles and marks it dirty.
	 * @param blueprint_path Asset path of the Actor Blueprint (under /Game).
	 * @param component_class Component class name or path, e.g. 'StaticMeshComponent', 'PointLight' (the 'Component' suffix is optional), '/Script/Engine.SphereComponent', or a Blueprint component '/Game/Components/BPC_Health'. Must be spawnable from Blueprints and not abstract.
	 * @param name Variable name for the new component. Must be unused in the Blueprint.
	 * @param parent Scene component to attach to (added, inherited or native). 'none' = attach to the scene root. Must be 'none' for non-scene components (e.g. movement components).
	 * @return Result: {blueprint, component: {...}, compile_status, dirty}.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_add_component(const FString& blueprint_path, const FString& component_class, const FString& name, const FString& parent = TEXT("none"));

	/**
	 * Removes a component that was added in this Blueprint, then compiles and marks it dirty. Inherited and native components cannot be removed. A scene component with children must have them moved or removed first.
	 * @param blueprint_path Asset path of the Actor Blueprint (under /Game).
	 * @param name Component name, as listed by bp_list_components.
	 * @return Result: {blueprint, removed, compile_status, dirty}.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_remove_component(const FString& blueprint_path, const FString& name);

	/**
	 * Renames a component that was added in this Blueprint, then compiles and marks it dirty. Graph references to the variable are updated by the editor.
	 * @param blueprint_path Asset path of the Actor Blueprint (under /Game).
	 * @param name Current component name.
	 * @param new_name New variable name. Must be a valid, unused name.
	 * @return Result: {blueprint, component: {...}, compile_status, dirty}.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_rename_component(const FString& blueprint_path, const FString& name, const FString& new_name);

	/**
	 * Attaches a scene component added in this Blueprint to a different parent scene component (added, inherited or native), keeping its relative transform. Then compiles and marks it dirty.
	 * @param blueprint_path Asset path of the Actor Blueprint (under /Game).
	 * @param name Component to move. Must be a scene component added in this Blueprint and not the scene root.
	 * @param new_parent Scene component to attach to. Cannot be the component itself or one of its descendants.
	 * @return Result: {blueprint, component: {...}, compile_status, dirty}.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_reparent_component(const FString& blueprint_path, const FString& name, const FString& new_parent);

	/**
	 * Saves a Blueprint (or any asset under /Game) to disk. This is the only tool in this plugin that writes asset files.
	 * @param blueprint_path Asset path, e.g. '/Game/Blueprints/BP_Door'.
	 * @return Result: {path, saved}. saved is false when there was nothing to save.
	 */
	UFUNCTION(Category = "GameplayMCP|Blueprint", meta = (AICallable))
	static FGameplayMCPResult bp_save_asset(const FString& blueprint_path);
};
