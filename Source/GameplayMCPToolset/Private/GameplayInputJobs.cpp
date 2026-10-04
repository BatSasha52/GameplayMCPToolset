// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayInputJobs.h"

#include "GameplayMCPHelpers.h"

#include "Editor.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"

namespace
{
	/** Jobs that have finished are kept this long so their status can still be read. */
	constexpr int32 MaxRetainedInputJobs = 50;

	UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem(UWorld* World, int32 PlayerIndex)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		ULocalPlayer* LocalPlayer = GameInstance ? GameInstance->GetLocalPlayerByIndex(PlayerIndex) : nullptr;
		return LocalPlayer ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer) : nullptr;
	}

	FInputActionValue MakeActionValue(const UInputAction* Action, const FVector& Value)
	{
		const EInputActionValueType Type = Action->ValueType;
		if (Type == EInputActionValueType::Boolean)
		{
			return FInputActionValue(Type, FVector(Value.X != 0.0 ? 1.0 : 0.0, 0.0, 0.0));
		}
		return FInputActionValue(Type, Value);
	}

	FString ValueToString(const FVector& Value)
	{
		return FString::Printf(TEXT("(%g, %g, %g)"), Value.X, Value.Y, Value.Z);
	}

	const TCHAR* KindName(FGameplayInputJobs::EStepKind Kind)
	{
		switch (Kind)
		{
		case FGameplayInputJobs::EStepKind::Hold: return TEXT("hold");
		case FGameplayInputJobs::EStepKind::Tap:  return TEXT("tap");
		case FGameplayInputJobs::EStepKind::Axis: return TEXT("axis");
		default:                                  return TEXT("wait");
		}
	}

	double StepDuration(const FGameplayInputJobs::FStep& Step)
	{
		if (Step.Kind == FGameplayInputJobs::EStepKind::Tap)
		{
			return Step.Count * Step.HoldTime + FMath::Max(0, Step.Count - 1) * Step.Interval;
		}
		return Step.Duration;
	}
}

FGameplayInputJobs& FGameplayInputJobs::Get()
{
	static FGameplayInputJobs Instance;
	return Instance;
}

void FGameplayInputJobs::Startup()
{
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGameplayInputJobs::Tick));
	PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FGameplayInputJobs::OnPrePIEEnded);
}

void FGameplayInputJobs::Shutdown()
{
	if (UObjectInitialized())
	{
		Cancel(INDEX_NONE, TEXT("module shutting down"));
	}
	FTSTicker::RemoveTicker(TickerHandle);
	FEditorDelegates::PrePIEEnded.Remove(PrePIEEndedHandle);
	Jobs.Empty();
}

double FGameplayInputJobs::EstimateDuration(const TArray<FStep>& Steps)
{
	// Each step starts at the cursor; only waiting steps move the cursor to their end, so a
	// non-waiting step overlaps with the steps after it. The job ends when the last step ends.
	double Cursor = 0.0;
	double LastEnd = 0.0;
	for (const FStep& Step : Steps)
	{
		const double End = Cursor + StepDuration(Step);
		LastEnd = FMath::Max(LastEnd, End);
		if (Step.bWait)
		{
			Cursor = End;
		}
	}
	return FMath::Max(LastEnd, Cursor);
}

int32 FGameplayInputJobs::StartJob(int32 PlayerIndex, TArray<FStep>&& Steps, FString& OutError)
{
	UWorld* World = GameplayMCP::GetPIEWorld(OutError);
	if (!World)
	{
		return INDEX_NONE;
	}
	if (!GetInputSubsystem(World, PlayerIndex))
	{
		OutError = FString::Printf(TEXT("Local player %d has no Enhanced Input subsystem in the PIE world. Check player_index (pie_get_status lists local players) and that the project uses Enhanced Input."), PlayerIndex);
		return INDEX_NONE;
	}
	if (Steps.Num() == 0)
	{
		OutError = TEXT("The job has no steps.");
		return INDEX_NONE;
	}
	const double Expected = EstimateDuration(Steps);
	if (Expected > MaxJobSeconds)
	{
		OutError = FString::Printf(TEXT("The job would run for %.1f s; the limit is %.0f s. Split it into smaller jobs."), Expected, MaxJobSeconds);
		return INDEX_NONE;
	}

	// One driver per action and player at a time, so jobs cannot fight over the same input.
	for (const FJob& Other : Jobs)
	{
		if (!IsActive(Other) || Other.PlayerIndex != PlayerIndex)
		{
			continue;
		}
		for (const FStep& Step : Steps)
		{
			for (const FStep& OtherStep : Other.Steps)
			{
				if (Step.Kind != EStepKind::Wait && Step.Action.IsValid() && Step.Action == OtherStep.Action)
				{
					OutError = FString::Printf(TEXT("%s is already being driven by input job %d for player %d. Wait for it (input_status) or cancel it (input_cancel) first."),
						*Step.ActionPath, Other.Id, PlayerIndex);
					return INDEX_NONE;
				}
			}
		}
	}

	FJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = NextJobId++;
	Job.PlayerIndex = PlayerIndex;
	Job.Steps = MoveTemp(Steps);

	// Drop the oldest finished jobs.
	int32 Finished = 0;
	for (const FJob& Existing : Jobs)
	{
		Finished += IsActive(Existing) ? 0 : 1;
	}
	for (int32 Index = 0; Index < Jobs.Num() && Finished > MaxRetainedInputJobs; )
	{
		if (!IsActive(Jobs[Index]))
		{
			Jobs.RemoveAt(Index);
			--Finished;
		}
		else
		{
			++Index;
		}
	}
	return NextJobId - 1;
}

int32 FGameplayInputJobs::Cancel(int32 JobId, const FString& Reason)
{
	int32 Cancelled = 0;
	for (FJob& Job : Jobs)
	{
		if (IsActive(Job) && (JobId == INDEX_NONE || Job.Id == JobId))
		{
			ReleaseAll(Job);
			Finish(Job, EJobState::Cancelled, Reason);
			++Cancelled;
		}
	}
	return Cancelled;
}

void FGameplayInputJobs::OnPrePIEEnded(const bool bIsSimulating)
{
	Cancel(INDEX_NONE, TEXT("PIE session ended"));
}

bool FGameplayInputJobs::Tick(float DeltaTime)
{
	for (FJob& Job : Jobs)
	{
		if (IsActive(Job))
		{
			TickJob(Job);
		}
	}
	return true;
}

void FGameplayInputJobs::TickJob(FJob& Job)
{
	FString Error;
	UWorld* World = GameplayMCP::GetPIEWorld(Error);
	UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(World, Job.PlayerIndex);
	if (!World || !Input)
	{
		ReleaseAll(Job);
		Finish(Job, World ? EJobState::Failed : EJobState::Cancelled,
			World ? FString::Printf(TEXT("local player %d lost its Enhanced Input subsystem"), Job.PlayerIndex) : FString(TEXT("PIE session ended")));
		return;
	}

	const double Now = World->GetTimeSeconds();
	const uint64 Frame = GFrameCounter;
	if (Job.State == EJobState::Queued)
	{
		Job.State = EJobState::Running;
		Job.StartTime = Now;
	}
	Job.LastTime = Now;

	// Start steps until one that must finish before the next can start.
	auto IsBlocked = [&Job]()
	{
		return Job.Active.ContainsByPredicate([&Job](const FSegment& Segment) { return Job.Steps[Segment.StepIndex].bWait; });
	};
	while (Job.NextStep < Job.Steps.Num() && !IsBlocked())
	{
		const FStep& Step = Job.Steps[Job.NextStep];
		if (Step.Kind != EStepKind::Wait)
		{
			if (!Step.Action.IsValid())
			{
				ReleaseAll(Job);
				Finish(Job, EJobState::Failed, FString::Printf(TEXT("input action %s was unloaded"), *Step.ActionPath));
				return;
			}
			if (Job.Held.Contains(Step.Action))
			{
				ReleaseAll(Job);
				Finish(Job, EJobState::Failed, FString::Printf(TEXT("step %d drives %s while an earlier parallel step is still driving it"), Job.NextStep, *Step.ActionPath));
				return;
			}
		}
		FSegment& Segment = Job.Active.AddDefaulted_GetRef();
		Segment.StepIndex = Job.NextStep++;
		Segment.StartTime = Now;
	}

	for (int32 Index = 0; Index < Job.Active.Num(); )
	{
		if (UpdateSegment(Job, Job.Active[Index], Input, Now, Frame))
		{
			Job.Active.RemoveAt(Index);
		}
		else
		{
			++Index;
		}
	}

	if (Job.NextStep >= Job.Steps.Num() && Job.Active.Num() == 0)
	{
		Finish(Job, EJobState::Completed, FString());
	}
}

bool FGameplayInputJobs::UpdateSegment(FJob& Job, FSegment& Segment, UEnhancedInputLocalPlayerSubsystem* Input, double Now, uint64 Frame)
{
	const FStep& Step = Job.Steps[Segment.StepIndex];
	const double Elapsed = Now - Segment.StartTime;
	const double InPhase = Now - Segment.PhaseTime;
	const bool bNewFrame = Frame > Segment.PhaseFrame;

	auto EnterPhase = [&](EPhase Phase)
	{
		Segment.Phase = Phase;
		Segment.PhaseTime = Now;
		Segment.PhaseFrame = Frame;
	};

	switch (Step.Kind)
	{
	case EStepKind::Wait:
		return Elapsed >= Step.Duration;

	case EStepKind::Hold:
	case EStepKind::Axis:
		if (Segment.Phase == EPhase::NotStarted)
		{
			Press(Job, Step, Step.Value, Input, Now);
			EnterPhase(EPhase::Pressed);
			return false;
		}
		if (Segment.Phase == EPhase::Pressed)
		{
			if (InPhase >= Step.Duration && bNewFrame)
			{
				Release(Job, Step, Input, Now);
				EnterPhase(EPhase::Released);
			}
			else if (Step.Kind == EStepKind::Axis && Step.bRamp && Step.Action.IsValid())
			{
				const double Alpha = Step.Duration > 0.0 ? FMath::Clamp(InPhase / Step.Duration, 0.0, 1.0) : 1.0;
				Input->UpdateValueOfContinuousInputInjectionForAction(Step.Action.Get(), MakeActionValue(Step.Action.Get(), Step.Value + (Step.EndValue - Step.Value) * Alpha));
			}
			return false;
		}
		// Released: keep the segment one more frame so the release is seen before anything else presses.
		return bNewFrame;

	case EStepKind::Tap:
		if (Segment.Phase == EPhase::NotStarted || (Segment.Phase == EPhase::Released && Segment.TapsDone < Step.Count && InPhase >= Step.Interval && bNewFrame))
		{
			Press(Job, Step, Step.Value, Input, Now);
			EnterPhase(EPhase::Pressed);
			return false;
		}
		if (Segment.Phase == EPhase::Pressed)
		{
			if (InPhase >= Step.HoldTime && bNewFrame)
			{
				Release(Job, Step, Input, Now);
				++Segment.TapsDone;
				EnterPhase(EPhase::Released);
			}
			return false;
		}
		return Segment.TapsDone >= Step.Count && bNewFrame;
	}
	return true;
}

void FGameplayInputJobs::Press(FJob& Job, const FStep& Step, const FVector& Value, UEnhancedInputLocalPlayerSubsystem* Input, double Now)
{
	const UInputAction* Action = Step.Action.Get();
	if (!Action)
	{
		return;
	}
	Input->StartContinuousInputInjectionForAction(Action, MakeActionValue(Action, Value), {}, {});
	Job.Held.Add(Step.Action);
	AddEvent(Job, Now, FString::Printf(TEXT("press %s %s"), *Action->GetName(), *ValueToString(Value)));
}

void FGameplayInputJobs::Release(FJob& Job, const FStep& Step, UEnhancedInputLocalPlayerSubsystem* Input, double Now)
{
	if (const UInputAction* Action = Step.Action.Get())
	{
		Input->StopContinuousInputInjectionForAction(Action);
		AddEvent(Job, Now, FString::Printf(TEXT("release %s"), *Action->GetName()));
	}
	Job.Held.Remove(Step.Action);
}

void FGameplayInputJobs::ReleaseAll(FJob& Job)
{
	if (Job.Held.Num() == 0)
	{
		return;
	}
	FString Unused;
	UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(GameplayMCP::GetPIEWorld(Unused), Job.PlayerIndex);
	for (const TWeakObjectPtr<const UInputAction>& Action : Job.Held)
	{
		if (Input && Action.IsValid())
		{
			Input->StopContinuousInputInjectionForAction(Action.Get());
			AddEvent(Job, Job.LastTime, FString::Printf(TEXT("release %s (cancelled)"), *Action->GetName()));
		}
	}
	Job.Held.Reset();
	Job.Active.Reset();
}

void FGameplayInputJobs::Finish(FJob& Job, EJobState State, const FString& Error)
{
	Job.State = State;
	Job.Error = Error;
	AddEvent(Job, Job.LastTime, State == EJobState::Completed ? FString(TEXT("completed")) : FString::Printf(TEXT("%s: %s"), State == EJobState::Cancelled ? TEXT("cancelled") : TEXT("failed"), *Error));
}

void FGameplayInputJobs::AddEvent(FJob& Job, double Now, const FString& Text)
{
	if (Job.Events.Num() < 200)
	{
		Job.Events.Add(FString::Printf(TEXT("t=%.3f %s"), Job.State == EJobState::Queued ? 0.0 : Now - Job.StartTime, *Text));
	}
}

TArray<TSharedRef<FJsonObject>> FGameplayInputJobs::GetStatus(int32 JobId) const
{
	TArray<TSharedRef<FJsonObject>> Out;
	for (int32 Index = Jobs.Num() - 1; Index >= 0; --Index)
	{
		if (JobId == INDEX_NONE || Jobs[Index].Id == JobId)
		{
			Out.Add(JobToJson(Jobs[Index]));
		}
	}
	return Out;
}

TSharedRef<FJsonObject> FGameplayInputJobs::JobToJson(const FJob& Job) const
{
	static const TCHAR* StateNames[] = { TEXT("queued"), TEXT("running"), TEXT("completed"), TEXT("cancelled"), TEXT("failed") };
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("job_id"), Job.Id);
	Out->SetStringField(TEXT("state"), StateNames[static_cast<int32>(Job.State)]);
	Out->SetNumberField(TEXT("player_index"), Job.PlayerIndex);
	Out->SetNumberField(TEXT("elapsed_seconds"), Job.State == EJobState::Queued ? 0.0 : Job.LastTime - Job.StartTime);
	Out->SetNumberField(TEXT("expected_seconds"), EstimateDuration(Job.Steps));
	Out->SetNumberField(TEXT("steps_total"), Job.Steps.Num());
	Out->SetNumberField(TEXT("steps_started"), Job.NextStep);
	TArray<FString> Steps;
	for (const FStep& Step : Job.Steps)
	{
		Steps.Add(Step.Kind == EStepKind::Wait
			? FString::Printf(TEXT("wait %.3fs"), Step.Duration)
			: FString::Printf(TEXT("%s %s"), KindName(Step.Kind), *Step.ActionPath));
	}
	Out->SetArrayField(TEXT("steps"), GameplayMCP::ToJsonArray(Steps));
	TArray<FString> Held;
	for (const TWeakObjectPtr<const UInputAction>& Action : Job.Held)
	{
		Held.Add(Action.IsValid() ? Action->GetName() : FString(TEXT("?")));
	}
	Out->SetArrayField(TEXT("held"), GameplayMCP::ToJsonArray(Held));
	Out->SetArrayField(TEXT("events"), GameplayMCP::ToJsonArray(Job.Events));
	Out->SetStringField(TEXT("error"), Job.Error);
	return Out;
}
