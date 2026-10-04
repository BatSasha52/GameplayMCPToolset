// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "InputActionValue.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UInputAction;
class UEnhancedInputLocalPlayerSubsystem;

/**
 * Runs simulated Enhanced Input as jobs that advance on the core ticker, so no tool ever blocks the
 * game thread. Input is injected with UEnhancedInputLocalPlayerSubsystem's continuous injection,
 * which runs the action's own modifiers and triggers exactly like a real key would.
 *
 * Timing uses the PIE world's game time (it respects pause and time dilation). Every press and every
 * release lasts at least one frame, so triggers always see both edges. Cancelling a job, or PIE
 * ending, releases everything the job was holding.
 */
class FGameplayInputJobs
{
public:
	enum class EStepKind : uint8 { Hold, Tap, Axis, Wait };

	struct FStep
	{
		EStepKind Kind = EStepKind::Wait;
		TWeakObjectPtr<const UInputAction> Action;
		FString ActionPath;
		FVector Value = FVector(1, 0, 0);
		FVector EndValue = FVector::ZeroVector;
		bool bRamp = false;
		double Duration = 0.0;
		int32 Count = 1;
		double Interval = 0.1;
		double HoldTime = 0.05;
		/** If false the next step starts immediately and this one runs alongside it. */
		bool bWait = true;
	};

	static FGameplayInputJobs& Get();

	void Startup();
	void Shutdown();

	/** Validates and queues a job. Returns the job id, or INDEX_NONE with OutError. */
	int32 StartJob(int32 PlayerIndex, TArray<FStep>&& Steps, FString& OutError);

	/** Cancels one job (or all running jobs when JobId is INDEX_NONE) and releases held input. Returns how many were cancelled. */
	int32 Cancel(int32 JobId, const FString& Reason);

	/** Status of one job, or of all retained jobs when JobId is INDEX_NONE. Empty if the id is unknown. */
	TArray<TSharedRef<FJsonObject>> GetStatus(int32 JobId) const;

	int32 GetLatestJobId() const { return NextJobId - 1; }

	/** Expected duration in game seconds of a step list, for reporting. */
	static double EstimateDuration(const TArray<FStep>& Steps);

	/** Longest single job accepted, in game seconds. */
	static constexpr double MaxJobSeconds = 300.0;

private:
	enum class EJobState : uint8 { Queued, Running, Completed, Cancelled, Failed };
	enum class EPhase : uint8 { NotStarted, Pressed, Released, Done };

	struct FSegment
	{
		int32 StepIndex = 0;
		EPhase Phase = EPhase::NotStarted;
		double StartTime = 0.0;
		double PhaseTime = 0.0;
		uint64 PhaseFrame = 0;
		int32 TapsDone = 0;
	};

	struct FJob
	{
		int32 Id = 0;
		int32 PlayerIndex = 0;
		TArray<FStep> Steps;
		int32 NextStep = 0;
		TArray<FSegment> Active;
		EJobState State = EJobState::Queued;
		FString Error;
		double StartTime = 0.0;
		double LastTime = 0.0;
		TArray<FString> Events;
		/** Actions currently being injected by this job. */
		TSet<TWeakObjectPtr<const UInputAction>> Held;
	};

	bool Tick(float DeltaTime);
	void TickJob(FJob& Job);
	bool UpdateSegment(FJob& Job, FSegment& Segment, UEnhancedInputLocalPlayerSubsystem* Input, double Now, uint64 Frame);
	void Press(FJob& Job, const FStep& Step, const FVector& Value, UEnhancedInputLocalPlayerSubsystem* Input, double Now);
	void Release(FJob& Job, const FStep& Step, UEnhancedInputLocalPlayerSubsystem* Input, double Now);
	void ReleaseAll(FJob& Job);
	void Finish(FJob& Job, EJobState State, const FString& Error);
	void OnPrePIEEnded(const bool bIsSimulating);
	TSharedRef<FJsonObject> JobToJson(const FJob& Job) const;
	void AddEvent(FJob& Job, double Now, const FString& Text);
	static bool IsActive(const FJob& Job) { return Job.State == EJobState::Queued || Job.State == EJobState::Running; }

	TArray<FJob> Jobs;
	int32 NextJobId = 1;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PrePIEEndedHandle;
};
