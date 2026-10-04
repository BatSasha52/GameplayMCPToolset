// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayConsoleJobs.h"

#include "GameplayMCPHelpers.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Misc/StringOutputDevice.h"

namespace
{
	/** Frames between queueing and running, so the tool call that queued the job has fully returned. */
	constexpr uint64 FramesBeforeRun = 2;
	constexpr int32 MaxRetainedJobs = 50;
	constexpr int32 MaxCapturedLines = 2000;
}

void FGameplayConsoleJobs::FCaptureDevice::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);
	if (Lines.Num() < MaxCapturedLines)
	{
		Lines.Add({ Category.ToString(), FString(ToString(static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask))).ToLower(), Message });
	}
}

TArray<FGameplayConsoleJobs::FCaptureDevice::FLine> FGameplayConsoleJobs::FCaptureDevice::TakeLines()
{
	FScopeLock ScopeLock(&Lock);
	return MoveTemp(Lines);
}

FGameplayConsoleJobs& FGameplayConsoleJobs::Get()
{
	static FGameplayConsoleJobs Instance;
	return Instance;
}

void FGameplayConsoleJobs::Startup()
{
	PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FGameplayConsoleJobs::OnPrePIEEnded);
}

void FGameplayConsoleJobs::Shutdown()
{
	for (FJob& Job : Jobs)
	{
		if (Job.State == TEXT("queued") || Job.State == TEXT("running"))
		{
			Finish(Job, TEXT("cancelled"), TEXT("module shutting down"));
		}
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	FEditorDelegates::PrePIEEnded.Remove(PrePIEEndedHandle);
	Jobs.Empty();
}

int32 FGameplayConsoleJobs::Enqueue(const FString& Command, const TArray<FString>& Segments, const FString& Rule, const FString& Target)
{
	FJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = NextId++;
	Job.Command = Command;
	Job.Segments = Segments;
	Job.Rule = Rule;
	Job.Target = Target;
	Job.QueuedFrame = GFrameCounter;

	for (int32 Index = 0; Jobs.Num() > MaxRetainedJobs && Index < Jobs.Num(); )
	{
		if (Jobs[Index].State == TEXT("done") || Jobs[Index].State == TEXT("cancelled"))
		{
			Jobs.RemoveAt(Index);
		}
		else
		{
			++Index;
		}
	}
	EnsureTicker();
	return Job.Id;
}

void FGameplayConsoleJobs::EnsureTicker()
{
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGameplayConsoleJobs::Tick));
	}
}

bool FGameplayConsoleJobs::Tick(float DeltaTime)
{
	// Finish a job that ran on an earlier frame (its capture also covered that frame).
	for (FJob& Job : Jobs)
	{
		if (Job.State == TEXT("running") && GFrameCounter > Job.RunFrame)
		{
			Finish(Job, TEXT("done"), Job.Error);
		}
	}

	// Run at most one queued job per frame, in order, and never while another is still capturing.
	const bool bRunning = Jobs.ContainsByPredicate([](const FJob& Job) { return Job.State == TEXT("running"); });
	if (!bRunning)
	{
		if (FJob* Next = Jobs.FindByPredicate([](const FJob& Job) { return Job.State == TEXT("queued"); }))
		{
			if (GFrameCounter >= Next->QueuedFrame + FramesBeforeRun)
			{
				Execute(*Next);
			}
		}
	}

	const bool bPending = Jobs.ContainsByPredicate([](const FJob& Job) { return Job.State == TEXT("queued") || Job.State == TEXT("running"); });
	if (!bPending)
	{
		TickerHandle.Reset();
	}
	return bPending;
}

void FGameplayConsoleJobs::Execute(FJob& Job)
{
	FString Error;
	UWorld* PIEWorld = Job.Target == TEXT("editor") ? nullptr : GameplayMCP::GetPIEWorld(Error);
	if (Job.Target == TEXT("pie") && !PIEWorld)
	{
		Finish(Job, TEXT("cancelled"), TEXT("no PIE session was running when the command was due to run"));
		return;
	}
	if (!GEditor)
	{
		Finish(Job, TEXT("cancelled"), TEXT("the editor is not available"));
		return;
	}

	Job.Before = CaptureWorldState();
	Job.RanIn = PIEWorld ? TEXT("pie") : TEXT("editor");
	Job.State = TEXT("running");
	Job.RunFrame = GFrameCounter;
	Job.Device = MakeShared<FCaptureDevice>();
	GLog->AddOutputDevice(Job.Device.Get());

	// The segments were checked by GameplayConsoleFilter when the job was queued; run exactly those.
	bool bHandled = true;
	for (const FString& Segment : Job.Segments)
	{
		FString Error2;
		UWorld* World = Job.RanIn == TEXT("pie") ? GameplayMCP::GetPIEWorld(Error2) : nullptr;
		if (Job.RanIn == TEXT("pie") && !World)
		{
			Job.Error = FString::Printf(TEXT("PIE ended before segment '%s' ran"), *Segment);
			break;
		}
		if (World)
		{
			if (APlayerController* Controller = GameplayMCP::GetLocalPlayerController(World, 0))
			{
				// Same route as typing in the game console: player input, controller, pawn, cheat manager, game instance, engine.
				Job.Output += Controller->ConsoleCommand(Segment, /*bWriteToLog*/ true);
			}
			else
			{
				FStringOutputDevice Device;
				bHandled &= GEngine->Exec(World, *Segment, Device);
				Job.Output += Device;
			}
		}
		else
		{
			FStringOutputDevice Device;
			bHandled &= GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Segment, Device);
			Job.Output += Device;
		}
	}
	Job.bRecognized = bHandled && !Job.Output.Contains(TEXT("Command not recognized"));
}

void FGameplayConsoleJobs::DetachDevice(FJob& Job)
{
	if (Job.Device.IsValid())
	{
		if (GLog)
		{
			GLog->RemoveOutputDevice(Job.Device.Get());
		}
		Job.Log.Append(Job.Device->TakeLines());
		Job.Device.Reset();
	}
}

void FGameplayConsoleJobs::Finish(FJob& Job, const FString& State, const FString& Error)
{
	const bool bRan = Job.State == TEXT("running");
	DetachDevice(Job);
	if (bRan && UObjectInitialized())
	{
		Job.After = CaptureWorldState();
	}
	for (const FCaptureDevice::FLine& Line : Job.Log)
	{
		if (Line.Message.Contains(TEXT("Command not recognized")))
		{
			Job.bRecognized = false;
		}
	}
	Job.State = State;
	Job.Error = Error;
}

void FGameplayConsoleJobs::OnPrePIEEnded(const bool bIsSimulating)
{
	for (FJob& Job : Jobs)
	{
		if (Job.State == TEXT("queued"))
		{
			Finish(Job, TEXT("cancelled"), TEXT("the PIE session ended before the command ran"));
		}
		else if (Job.State == TEXT("running"))
		{
			Finish(Job, TEXT("done"), TEXT("the PIE session ended while output was being captured"));
		}
	}
}

FGameplayConsoleJobs::FWorldState FGameplayConsoleJobs::CaptureWorldState()
{
	FWorldState State;
	FString Unused;
	if (UWorld* PIEWorld = GameplayMCP::GetPIEWorld(Unused))
	{
		State.bPIERunning = true;
		State.PIEWorld = PIEWorld->GetPathName();
		State.bPaused = PIEWorld->IsPaused();
		if (const AWorldSettings* Settings = PIEWorld->GetWorldSettings())
		{
			State.TimeDilation = Settings->TimeDilation;
		}
	}
	if (GEditor)
	{
		if (const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			State.EditorWorld = EditorWorld->GetPathName();
		}
	}
	return State;
}

TSharedRef<FJsonObject> FGameplayConsoleJobs::WorldStateToJson(const FWorldState& State)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("pie_running"), State.bPIERunning);
	Out->SetStringField(TEXT("pie_world"), State.PIEWorld);
	Out->SetBoolField(TEXT("paused"), State.bPaused);
	Out->SetNumberField(TEXT("time_dilation"), State.TimeDilation);
	Out->SetStringField(TEXT("editor_world"), State.EditorWorld);
	return Out;
}

TSharedPtr<FJsonObject> FGameplayConsoleJobs::GetResult(int32 JobId) const
{
	const FJob* Job = JobId == INDEX_NONE
		? (Jobs.Num() > 0 ? &Jobs.Last() : nullptr)
		: Jobs.FindByPredicate([JobId](const FJob& Candidate) { return Candidate.Id == JobId; });
	return Job ? TSharedPtr<FJsonObject>(JobToJson(*Job)) : nullptr;
}

TSharedRef<FJsonObject> FGameplayConsoleJobs::JobToJson(const FJob& Job) const
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("job_id"), Job.Id);
	Out->SetStringField(TEXT("state"), Job.State);
	Out->SetStringField(TEXT("command"), Job.Command);
	Out->SetStringField(TEXT("rule"), Job.Rule);
	Out->SetArrayField(TEXT("segments"), GameplayMCP::ToJsonArray(Job.Segments));
	Out->SetStringField(TEXT("target"), Job.Target);
	Out->SetStringField(TEXT("ran_in"), Job.RanIn);
	Out->SetStringField(TEXT("error"), Job.Error);

	const bool bRan = Job.State == TEXT("done");
	Out->SetStringField(TEXT("output"), GameplayMCP::Truncate(Job.Output.TrimStartAndEnd(), 20000));
	TArray<TSharedRef<FJsonObject>> Log;
	for (const FCaptureDevice::FLine& Line : Job.Log)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("category"), Line.Category);
		Entry->SetStringField(TEXT("verbosity"), Line.Verbosity);
		Entry->SetStringField(TEXT("message"), Line.Message);
		Log.Add(Entry);
	}
	Out->SetArrayField(TEXT("log"), GameplayMCP::ToJsonArray(Log));
	Out->SetBoolField(TEXT("recognized"), Job.bRecognized);

	if (bRan)
	{
		TArray<FString> Changes;
		auto Note = [&Changes](const TCHAR* Field, const FString& Before, const FString& After)
		{
			if (Before != After)
			{
				Changes.Add(FString::Printf(TEXT("%s: %s -> %s"), Field, *Before, *After));
			}
		};
		Note(TEXT("pie_running"), Job.Before.bPIERunning ? TEXT("true") : TEXT("false"), Job.After.bPIERunning ? TEXT("true") : TEXT("false"));
		Note(TEXT("pie_world"), Job.Before.PIEWorld, Job.After.PIEWorld);
		Note(TEXT("paused"), Job.Before.bPaused ? TEXT("true") : TEXT("false"), Job.After.bPaused ? TEXT("true") : TEXT("false"));
		Note(TEXT("time_dilation"), FString::SanitizeFloat(Job.Before.TimeDilation), FString::SanitizeFloat(Job.After.TimeDilation));
		Note(TEXT("editor_world"), Job.Before.EditorWorld, Job.After.EditorWorld);
		Out->SetBoolField(TEXT("world_changed"), Changes.Num() > 0);
		Out->SetArrayField(TEXT("changes"), GameplayMCP::ToJsonArray(Changes));
		Out->SetObjectField(TEXT("before"), WorldStateToJson(Job.Before));
		Out->SetObjectField(TEXT("after"), WorldStateToJson(Job.After));
	}
	return Out;
}
