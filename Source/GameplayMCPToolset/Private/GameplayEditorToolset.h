// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "GameplayMCPTypes.h"

#include "GameplayEditorToolset.generated.h"

namespace GameplayLiveCoding
{
	/** Stops tracking Live Coding jobs; called when the module shuts down. */
	void Shutdown();
}

/**
 * Phase 3 - console commands, the output log and Live Coding.
 * editor_run_console_command runs a command and returns what it printed. A few commands are always refused because they
 * quit the editor, end PIE, crash on purpose, run scripts or save packages: quit, exit, disconnect, debug, crash, exec, py, python, obj savepackage.
 * editor_live_coding_compile starts a Live Coding compile without blocking the game thread and returns a job id; poll editor_live_coding_status.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UGameplayEditorToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Runs a console command and returns its output and the log lines it produced while running. Refused commands: quit, exit, disconnect, debug, crash, exec, py, python, obj savepackage (each part of a '|'-separated command is checked).
	 * @param command The console command, e.g. 'stat fps', 'slomo 0.5', 'r.ScreenPercentage 50', 'showdebug animation'.
	 * @param target 'auto' = the PIE session's first local player if PIE is running (so cheats and game commands work), otherwise the editor. 'pie' = require PIE. 'editor' = always run in the editor world.
	 * @return Result: {command, target, output, log: [{id, category, verbosity, message}], recognized}. Some commands only take effect on the next frame.
	 */
	UFUNCTION(Category = "GameplayMCP|Editor", meta = (AICallable))
	static FGameplayMCPResult editor_run_console_command(const FString& command, const FString& target = TEXT("auto"));

	/**
	 * Returns the most recent output-log lines, filtered. Only lines logged after this plugin loaded are available (up to the last 10000).
	 * @param max_lines Maximum number of lines to return (the newest matching ones), 1-2000.
	 * @param log_category Category filter with * wildcards, e.g. 'LogTemp', 'LogBlueprint*'. '*' = any.
	 * @param min_verbosity Least severe level to include: 'fatal', 'error', 'warning', 'display', 'log', 'verbose' or 'all'.
	 * @param contains Only lines containing this text (case-insensitive). '*' = any.
	 * @param after_id Only lines with an id greater than this (use latest_id from a previous call to read only new lines). 0 = no limit.
	 * @return Result: {latest_id, count, lines: [{id, time, category, verbosity, message}]}.
	 */
	UFUNCTION(Category = "GameplayMCP|Editor", meta = (AICallable))
	static FGameplayMCPResult editor_get_recent_log(int32 max_lines = 100, const FString& log_category = TEXT("*"), const FString& min_verbosity = TEXT("log"), const FString& contains = TEXT("*"), int32 after_id = 0);

	/**
	 * Starts a Live Coding compile (the same as Ctrl+Alt+F11) and returns a job id at once; the compile runs in the background. Poll editor_live_coding_status until state is 'completed'. Live Coding must be available (Win64 editor, enabled in Editor Preferences, project has C++ modules).
	 * @return Result: {job_id, state}.
	 */
	UFUNCTION(Category = "GameplayMCP|Editor", meta = (AICallable))
	static FGameplayMCPResult editor_live_coding_compile();

	/**
	 * Reports a Live Coding compile started by editor_live_coding_compile.
	 * @param job_id A job id or 'latest'.
	 * @return Result: {job_id, state ('compiling' or 'completed'), result ('success', 'no_changes', 'failure', 'cancelled', 'not_started' or 'unknown'), elapsed_seconds, errors: [...], warnings: [...], output: [LogLiveCoding lines]}.
	 */
	UFUNCTION(Category = "GameplayMCP|Editor", meta = (AICallable))
	static FGameplayMCPResult editor_live_coding_status(const FString& job_id = TEXT("latest"));
};
