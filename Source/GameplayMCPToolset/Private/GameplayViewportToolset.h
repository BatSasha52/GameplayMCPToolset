// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "GameplayMCPTypes.h"

#include "GameplayViewportToolset.generated.h"

namespace GameplayScreenshots
{
	/** Stops tracking screenshot jobs; called when the module shuts down. */
	void Shutdown();
}

/**
 * Phase 5 - capture the game view of a running Play-In-Editor session.
 * The PIE game viewport is rendered offscreen at the requested resolution on the next frame, without any editor UI
 * (Slate widgets, including UMG, are not part of the capture; HUD canvas drawing is). The PNG is written under the
 * project's Saved/Screenshots/GameplayMCP folder and its path is returned (never the image data).
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UGameplayViewportToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Requests a screenshot of the PIE game viewport and returns a job id at once; the image is rendered and written on the next frame (usually well under a second). Poll game_screenshot_status until state is 'completed', then open file_path. Fails if no PIE session is running.
	 * @param width Image width in pixels (64-7680).
	 * @param height Image height in pixels (64-7680). The camera's field of view is kept, so a different aspect ratio than the viewport shows more or less of the scene.
	 * @param file_name File name without folder, e.g. 'after_jump' (.png is added). 'auto' = a timestamped name.
	 * @param overwrite Replace an existing file with the same name. false = fail instead.
	 * @return Result: {job_id, state, file_path, width, height}.
	 */
	UFUNCTION(Category = "GameplayMCP|Viewport", meta = (AICallable))
	static FGameplayMCPResult game_capture_screenshot(int32 width = 1280, int32 height = 720, const FString& file_name = TEXT("auto"), bool overwrite = false);

	/**
	 * Reports a screenshot requested with game_capture_screenshot.
	 * @param job_id A job id or 'latest'.
	 * @return Result: {job_id, state ('pending', 'completed' or 'failed'), file_path, width, height, file_size, error}.
	 */
	UFUNCTION(Category = "GameplayMCP|Viewport", meta = (AICallable))
	static FGameplayMCPResult game_screenshot_status(const FString& job_id = TEXT("latest"));
};
