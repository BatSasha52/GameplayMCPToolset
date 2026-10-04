// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "GameplayMCPTypes.h"

#include "GameplayPIEToolset.generated.h"

/**
 * Phase 1 - inspect and poke a running Play-In-Editor (PIE) session.
 * Use pie_get_status first. Every other tool fails with a clear error when no PIE session is running.
 * Actors are addressed by object name (e.g. 'BP_Player_C_0'), editor label, full path, or a shortcut:
 * '@pawn', '@controller', '@camera', '@hud' (append a local player index, e.g. '@pawn1'), '@gamemode', '@gamestate'.
 * Set and call tools only ever act on objects that live in the PIE world, never on editor-world actors or assets.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UGameplayPIEToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Reports whether a Play-In-Editor session is running and, if so, its world, net mode and pause state. Never fails because PIE is not running.
	 * @return Result: {running, requested, simulating, world, map, net_mode, paused, time_seconds, real_time_seconds, local_players: [{index, controller, pawn}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_get_status();

	/**
	 * Starts a Play-In-Editor session in the active level viewport (or a new window if there is none). The session starts on the next editor tick, so poll pie_get_status until running=true. Fails if a session is already running or queued.
	 * @return Result: {requested: true, destination}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_start();

	/**
	 * Stops the running Play-In-Editor session. The session ends on the next editor tick. Running input jobs are cancelled and their inputs released.
	 * @return Result: {requested: true}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_stop();

	/**
	 * Lists live actors in the PIE world, optionally filtered. All filters are case-insensitive and accept * wildcards.
	 * @param class_filter Class name or path. A name matches the actor's class or any parent class, with or without the Blueprint '_C' suffix (e.g. 'Character', 'BP_Enemy', '/Script/Engine.Pawn'). '*' = any class.
	 * @param label_filter Match against the actor's editor label or object name, e.g. 'Enemy*'. '*' = any.
	 * @param tag_filter Only actors that have this actor tag. '*' = any.
	 * @param limit Maximum number of actors to return.
	 * @return Result: {count, total_matches, actors: [{name, label, class, class_path, location, rotation, guid, path, tags}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_list_actors(const FString& class_filter = TEXT("*"), const FString& label_filter = TEXT("*"), const FString& tag_filter = TEXT("*"), int32 limit = 200);

	/**
	 * Resolves one live actor by object name, label, path or shortcut and returns its details.
	 * @param actor Object name (e.g. 'BP_Player_C_0'), editor label, full object path, or a shortcut such as '@pawn'.
	 * @return Result: {name, label, class, class_path, location, rotation, guid, path, tags, parent_classes, owner, instigator, controller, is_pawn, hidden, component_count}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_find_actor(const FString& actor);

	/**
	 * Reads one property of a live actor, component or anim instance by property path.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param property_path Path such as 'Health', 'Stats.Level', 'Inventory[2].Count', 'Inventory[Sword].Count' (name key: matches an element's Name/Id/Key/Tag field or object name), 'Ammo[Rifle]' (map key), or 'CharacterMovement.MaxWalkSpeed' (follows object references).
	 * @param component 'none' for the actor itself, a component name (e.g. 'CharacterMovement'), a component class name when unique, 'AnimInstance' (first skeletal mesh's anim instance) or '<MeshComponent>/AnimInstance'.
	 * @return Result: {object, property_path, type, value (JSON), text (Unreal export text)}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_get_property(const FString& actor, const FString& property_path, const FString& component = TEXT("none"));

	/**
	 * Writes one property of a live PIE actor, component or anim instance. The value is parsed into a scratch copy first; if parsing fails nothing changes. This is a raw write: setters, OnRep and PostEditChange are not called (use pie_call_function for setters). Only PIE-world objects can be changed.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param property_path Path as in pie_get_property, e.g. 'Health', 'Stats.Offset', 'Inventory[Sword].Count', 'Ammo[Rifle]'.
	 * @param value New value. Forms: plain text for strings/names/text; true/false; numbers; enum names; Unreal text such as '(X=1,Y=2,Z=3)'; JSON such as '{"X":1}' (patches only the listed struct fields) or '[1,2,3]'; for object references 'none', an asset path, or a PIE actor name/shortcut ('Actor:Component' for a component).
	 * @param component 'none' for the actor itself, a component name, 'AnimInstance' or '<MeshComponent>/AnimInstance'.
	 * @return Result: {object, property_path, type, old_text, value (JSON), text}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_set_property(const FString& actor, const FString& property_path, const FString& value, const FString& component = TEXT("none"));

	/**
	 * Lists the reflected properties of a live object with their current values.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param component 'none' for the actor itself, a component name, 'AnimInstance' or '<MeshComponent>/AnimInstance'.
	 * @param name_filter Only properties whose name matches, e.g. '*Health*'. '*' = all.
	 * @param include_all false = only properties visible to Blueprints or editable in the editor. true = every reflected property.
	 * @param max_value_length Values longer than this are cut. 0 = never cut.
	 * @return Result: {object, class, count, properties: [{name, type, value, declared_in, flags}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_list_properties(const FString& actor, const FString& component = TEXT("none"), const FString& name_filter = TEXT("*"), bool include_all = false, int32 max_value_length = 200);

	/**
	 * Lists the UFUNCTIONs that pie_call_function can call on a live object, with their parameters.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param component 'none' for the actor itself, a component name, 'AnimInstance' or '<MeshComponent>/AnimInstance'.
	 * @param name_filter Only functions whose name matches, e.g. '*Jump*'. '*' = all.
	 * @param include_all false = only BlueprintCallable/BlueprintPure/Exec functions. true = every UFUNCTION.
	 * @return Result: {object, class, count, functions: [{name, declared_in, flags, params: [{name, type, direction}]}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_list_functions(const FString& actor, const FString& component = TEXT("none"), const FString& name_filter = TEXT("*"), bool include_all = false);

	/**
	 * Calls a UFUNCTION (BlueprintCallable or any reflected function) on a live PIE object with named arguments and returns the return value and out parameters. Latent functions are rejected.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param function_name Function name, e.g. 'Jump', 'AddHealth', 'Montage_Play'. Case-insensitive.
	 * @param args JSON object of named arguments, e.g. '{"Amount": 25, "bClamp": true}'. Values use the same forms as pie_set_property; object arguments accept asset paths or PIE actor names. Missing arguments use their default (zero) value. '{}' = no arguments.
	 * @param component 'none' for the actor itself, a component name, 'AnimInstance' or '<MeshComponent>/AnimInstance'.
	 * @return Result: {object, function, return_value (JSON, if any), out_params: {name: value}, missing_args}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_call_function(const FString& actor, const FString& function_name, const FString& args = TEXT("{}"), const FString& component = TEXT("none"));

	/**
	 * Reads the live animation state of a skeletal mesh's anim instance: the active state of every state machine with time in state, playing montages, curve values and the anim Blueprint's own variables.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @param component Skeletal mesh component name, e.g. 'Mesh'. '*' = the first skeletal mesh component that has an anim instance.
	 * @param include_variables Include the values of variables declared in the anim Blueprint.
	 * @param include_curves Include active curve values.
	 * @return Result: {object, anim_class, state_machines: [{name, index, active_state, time_in_state, states}], montages: [{montage, section, position, play_rate, weight, is_playing}], curves: {name: value}, variables: {name: value}}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_get_anim_state(const FString& actor, const FString& component = TEXT("*"), bool include_variables = true, bool include_curves = true);

	/**
	 * Returns the component hierarchy of a live actor with relative and world transforms.
	 * @param actor Actor name, label, path or shortcut (e.g. '@pawn').
	 * @return Result: {actor, root: {name, class, socket, visible, relative, world, children: [...]}, other_components: [{name, class, active}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|PIE", meta = (AICallable))
	static FGameplayMCPResult pie_get_component_tree(const FString& actor);
};
