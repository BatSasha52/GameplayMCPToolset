// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Misc/OutputDevice.h"

/**
 * Runs console commands for editor_run_console_command as deferred jobs.
 *
 * A command never runs inside the tool call: the tool validates and queues it, returns a job id, and
 * this runner executes it on the core ticker at least two frames later, after the tool result has gone
 * back. A command that tears down PIE or other state therefore cannot pull the world out from under an
 * in-flight tool call.
 *
 * While a job runs, a job-owned FOutputDevice on GLog captures what is logged during execution and the
 * frame after it; it is removed when the job finishes, is cancelled, or the module shuts down.
 */
class FGameplayConsoleJobs
{
public:
	static FGameplayConsoleJobs& Get();

	void Startup();
	void Shutdown();

	/** Queues already-checked segments. Target is 'auto', 'pie' or 'editor'. Returns the job id. */
	int32 Enqueue(const FString& Command, const TArray<FString>& Segments, const FString& Rule, const FString& Target);

	/** Result of a job, or nullptr if the id is unknown. INDEX_NONE = latest. */
	TSharedPtr<FJsonObject> GetResult(int32 JobId) const;

	bool HasJobs() const { return Jobs.Num() > 0; }

private:
	/** Captures log lines for one job. Thread-safe; owned by the job. */
	class FCaptureDevice : public FOutputDevice
	{
	public:
		struct FLine
		{
			FString Category;
			FString Verbosity;
			FString Message;
		};

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		TArray<FLine> TakeLines();

	private:
		FCriticalSection Lock;
		TArray<FLine> Lines;
	};

	struct FWorldState
	{
		bool bPIERunning = false;
		FString PIEWorld;
		bool bPaused = false;
		double TimeDilation = 1.0;
		FString EditorWorld;
	};

	struct FJob
	{
		int32 Id = 0;
		FString Command;
		TArray<FString> Segments;
		FString Rule;
		FString Target;
		FString RanIn;
		/** queued -> running (executed, still capturing) -> done; or cancelled. */
		FString State = TEXT("queued");
		uint64 QueuedFrame = 0;
		uint64 RunFrame = 0;
		FString Output;
		TArray<FCaptureDevice::FLine> Log;
		bool bRecognized = true;
		FString Error;
		FWorldState Before;
		FWorldState After;
		TSharedPtr<FCaptureDevice> Device;
	};

	bool Tick(float DeltaTime);
	void Execute(FJob& Job);
	void Finish(FJob& Job, const FString& State, const FString& Error);
	void DetachDevice(FJob& Job);
	void OnPrePIEEnded(const bool bIsSimulating);
	void EnsureTicker();
	static FWorldState CaptureWorldState();
	static TSharedRef<FJsonObject> WorldStateToJson(const FWorldState& State);
	TSharedRef<FJsonObject> JobToJson(const FJob& Job) const;

	TArray<FJob> Jobs;
	int32 NextId = 1;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PrePIEEndedHandle;
};
