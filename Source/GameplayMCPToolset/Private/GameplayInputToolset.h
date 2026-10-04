// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "GameplayMCPTypes.h"

#include "GameplayInputToolset.generated.h"

/**
 * Phase 2 - simulate Enhanced Input in a running Play-In-Editor session.
 * Input is injected through the local player's Enhanced Input subsystem, so the action's modifiers and
 * triggers run exactly as for a real key and gameplay bindings fire normally. No mapping context is needed.
 * Every input tool returns immediately with a job_id; the job then runs over the following frames.
 * Poll input_status until state is 'completed' (or 'failed'/'cancelled') before checking the result in game.
 * Durations are game seconds (they respect pause and time dilation). Every press and release lasts at least one frame.
 * Cancelling a job, or PIE ending, releases everything it holds. One job at a time may drive a given action for a given player.
 * Actions are given as an asset path ('/Game/Input/IA_Jump') or an asset name ('IA_Jump') when the name is unique.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UGameplayInputToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Lists Input Action assets the input tools can drive, with their value types.
	 * @param name_filter Only actions whose asset name matches, e.g. 'IA_*'. '*' = all.
	 * @return Result: {count, actions: [{name, path, value_type}]}. value_type is boolean, axis1d, axis2d or axis3d.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_list_actions(const FString& name_filter = TEXT("*"));

	/**
	 * Holds an Input Action for a number of game seconds, then releases it. Returns a job id at once; poll input_status.
	 * @param action Input Action asset path or unique asset name, e.g. 'IA_Move'.
	 * @param duration Seconds to hold (game time). 0 = one frame.
	 * @param value Value while held. Boolean/1D: '1' or '0.5'. 2D: 'X,Y' e.g. '0,1' for forward on a typical move action. 3D: 'X,Y,Z'. Also accepts '(X=0,Y=1)' or '[0,1]'.
	 * @param player_index Local player to drive. 0 = first player.
	 * @return Result: {job_id, state, action, expected_seconds}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_hold_action(const FString& action, float duration = 1.0f, const FString& value = TEXT("1"), int32 player_index = 0);

	/**
	 * Taps an Input Action several times: press for hold_time, release, wait interval, repeat. Returns a job id at once; poll input_status.
	 * @param action Input Action asset path or unique asset name, e.g. 'IA_Jump'.
	 * @param count Number of taps (1-100).
	 * @param interval Seconds between a release and the next press (game time).
	 * @param hold_time Seconds each tap stays pressed (game time). 0 = one frame.
	 * @param player_index Local player to drive. 0 = first player.
	 * @return Result: {job_id, state, action, expected_seconds}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_tap_action(const FString& action, int32 count = 1, float interval = 0.1f, float hold_time = 0.05f, int32 player_index = 0);

	/**
	 * Drives an axis (1D/2D/3D) Input Action over time with a constant value or a linear ramp, then releases it. Returns a job id at once; poll input_status.
	 * @param action Input Action asset path or unique asset name. Must not be a Boolean action (use input_hold_action).
	 * @param value Start value, e.g. '0.5' (1D), '0,1' (2D), '0,0,1' (3D).
	 * @param end_value End value for a ramp from value to end_value over the duration. 'none' = hold value constant.
	 * @param duration Seconds to drive the axis (game time).
	 * @param player_index Local player to drive. 0 = first player.
	 * @return Result: {job_id, state, action, expected_seconds}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_set_axis(const FString& action, const FString& value, const FString& end_value = TEXT("none"), float duration = 1.0f, int32 player_index = 0);

	/**
	 * Runs an ordered list of input steps as one job. Each step waits for the previous one unless the previous step has "wait": false, which lets steps overlap (e.g. keep moving while jumping). Returns a job id at once; poll input_status.
	 * @param steps JSON array of steps. Types: {"type":"hold","action":"IA_Move","value":"0,1","duration":1}, {"type":"tap","action":"IA_Jump","count":2,"interval":0.2,"hold_time":0.05}, {"type":"axis","action":"IA_Throttle","value":"0","end_value":"1","duration":1}, {"type":"wait","duration":0.5}. Optional on any step: "wait": false to start the next step immediately. Unknown fields are rejected.
	 * @param player_index Local player to drive. 0 = first player.
	 * @return Result: {job_id, state, step_count, expected_seconds}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_sequence(const FString& steps, int32 player_index = 0);

	/**
	 * Reports the state of input jobs: queued, running, completed, cancelled or failed, with a timeline of presses and releases.
	 * @param job_id A job id, 'latest' for the most recent job, or '*' for all retained jobs (newest first).
	 * @return Result: {jobs: [{job_id, state, player_index, elapsed_seconds, expected_seconds, steps, held, events, error}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_status(const FString& job_id = TEXT("*"));

	/**
	 * Cancels input jobs and releases every action they hold, so nothing stays stuck down.
	 * @param job_id A job id, 'latest', or '*' for every running job.
	 * @return Result: {cancelled: number of jobs cancelled}.
	 */
	UFUNCTION(Category = "GameplayMCP|Input", meta = (AICallable))
	static FGameplayMCPResult input_cancel(const FString& job_id = TEXT("*"));
};
