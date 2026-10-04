// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayEditorToolset.h"

#include "GameplayConsoleFilter.h"
#include "GameplayLogCapture.h"
#include "GameplayMCPHelpers.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace
{
	/** Runs the console-variable lookup the filter needs against the live console manager. */
	bool IsRegisteredConsoleVariable(const FString& Word)
	{
		IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Word, /*bTrackFrequentCalls*/ false);
		return Object && Object->AsVariable() != nullptr;
	}

	FString VerbosityName(ELogVerbosity::Type Verbosity)
	{
		return FString(ToString(Verbosity)).ToLower();
	}

	bool ParseVerbosity(const FString& Text, ELogVerbosity::Type& Out)
	{
		const FString Name = Text.TrimStartAndEnd().ToLower();
		if (Name == TEXT("all") || GameplayMCP::IsUnset(Name)) { Out = ELogVerbosity::All; return true; }
		if (Name == TEXT("fatal")) { Out = ELogVerbosity::Fatal; return true; }
		if (Name == TEXT("error")) { Out = ELogVerbosity::Error; return true; }
		if (Name == TEXT("warning")) { Out = ELogVerbosity::Warning; return true; }
		if (Name == TEXT("display")) { Out = ELogVerbosity::Display; return true; }
		if (Name == TEXT("log")) { Out = ELogVerbosity::Log; return true; }
		if (Name == TEXT("verbose")) { Out = ELogVerbosity::Verbose; return true; }
		if (Name == TEXT("veryverbose")) { Out = ELogVerbosity::VeryVerbose; return true; }
		return false;
	}

	TSharedRef<FJsonObject> LineToJson(const FGameplayLogCapture::FLine& Line, bool bWithTime)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("id"), static_cast<double>(Line.Id));
		if (bWithTime)
		{
			Out->SetNumberField(TEXT("time"), Line.Time);
		}
		Out->SetStringField(TEXT("category"), Line.Category.ToString());
		Out->SetStringField(TEXT("verbosity"), VerbosityName(Line.Verbosity));
		Out->SetStringField(TEXT("message"), Line.Message);
		return Out;
	}

	// ---- Live Coding jobs ----------------------------------------------------------------------

	struct FLiveCodingJob
	{
		int32 Id = 0;
		bool bCompiling = true;
		FString Result = TEXT("unknown");
		uint64 FirstLogId = 0;
		double StartTime = 0.0;
		double EndTime = 0.0;
		/** When the module stopped reporting a compile; the result line can arrive a few ticks later. */
		double CompileStoppedTime = 0.0;
		FDateTime UBTLogTimestamp;
		TArray<FString> Output;
		TArray<FString> Errors;
		TArray<FString> Warnings;
	};

	TArray<FLiveCodingJob> LiveCodingJobs;
	FTSTicker::FDelegateHandle LiveCodingTicker;

	/**
	 * UnrealBuildTool's log. Like UBT itself (Unreal.EngineProgramSavedDirectory): installed engines write it to
	 * the user settings folder (%LOCALAPPDATA%/UnrealBuildTool), source builds to Engine/Programs/UnrealBuildTool.
	 */
	FString GetUBTLogPath()
	{
		const FString Root = FApp::IsEngineInstalled() ? FString(FPlatformProcess::UserSettingsDir()) : FPaths::Combine(FPaths::EngineDir(), TEXT("Programs"));
		return FPaths::Combine(Root, TEXT("UnrealBuildTool"), TEXT("Log.txt"));
	}

	bool IsDiagnostic(const FString& Line, const TCHAR* Kind)
	{
		return Line.Contains(FString::Printf(TEXT("): %s"), Kind)) || Line.Contains(FString::Printf(TEXT(": %s C"), Kind)) || Line.Contains(FString::Printf(TEXT(": %s LNK"), Kind))
			|| Line.Contains(FString::Printf(TEXT("): fatal %s"), Kind));
	}

	void FinishLiveCodingJob(FLiveCodingJob& Job)
	{
		Job.bCompiling = false;
		Job.EndTime = FPlatformTime::Seconds();
		for (const FString& Line : Job.Output)
		{
			if (IsDiagnostic(Line, TEXT("error")))
			{
				Job.Errors.AddUnique(Line);
			}
			else if (IsDiagnostic(Line, TEXT("warning")))
			{
				Job.Warnings.AddUnique(Line);
			}
		}
		// UBT rewrites its log on every run; only read it if this compile produced a new one.
		const FString UBTLog = GetUBTLogPath();
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*UBTLog);
		FString Content;
		if (Stamp != FDateTime::MinValue() && Stamp != Job.UBTLogTimestamp && FFileHelper::LoadFileToString(Content, *UBTLog))
		{
			TArray<FString> Lines;
			Content.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				if (IsDiagnostic(Line, TEXT("error")))
				{
					Job.Errors.AddUnique(Line.TrimStartAndEnd());
				}
				else if (IsDiagnostic(Line, TEXT("warning")))
				{
					Job.Warnings.AddUnique(Line.TrimStartAndEnd());
				}
			}
		}
	}

	bool TickLiveCoding(float DeltaTime)
	{
		bool bAnyRunning = false;
		for (FLiveCodingJob& Job : LiveCodingJobs)
		{
			if (!Job.bCompiling)
			{
				continue;
			}
			// Collect what Live Coding logged since the compile started.
			static const FName LiveCodingCategory(TEXT("LogLiveCoding"));
			const TArray<FGameplayLogCapture::FLine> Lines = FGameplayLogCapture::Get().GetLines(Job.FirstLogId, FGameplayLogCapture::Capacity,
				[](const FGameplayLogCapture::FLine& Line) { return Line.Category == LiveCodingCategory; });
			for (const FGameplayLogCapture::FLine& Line : Lines)
			{
				Job.Output.Add(Line.Message);
				Job.FirstLogId = Line.Id;
				// These are the result lines FLiveCodingModule logs once a compile is over.
				if (Line.Message.Contains(TEXT("no code changes detected")))
				{
					Job.Result = TEXT("no_changes");
				}
				else if (Line.Message.StartsWith(TEXT("Live coding succeeded")))
				{
					Job.Result = TEXT("success");
				}
				else if (Line.Message.StartsWith(TEXT("Live coding canceled")))
				{
					Job.Result = TEXT("cancelled");
				}
				else if (Line.Message.StartsWith(TEXT("Live coding failed")))
				{
					Job.Result = TEXT("failure");
				}
			}

			bool bModuleCompiling = false;
#if WITH_LIVE_CODING
			if (ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
			{
				bModuleCompiling = LiveCoding->IsCompiling();
			}
#endif
			const double Now = FPlatformTime::Seconds();
			if (!bModuleCompiling && Job.CompileStoppedTime == 0.0)
			{
				Job.CompileStoppedTime = Now;
			}
			// Done once the result line is in, or a few seconds after the compile stopped without one.
			if (Job.Result != TEXT("unknown") || (!bModuleCompiling && Now - Job.CompileStoppedTime > 10.0) || Now - Job.StartTime > 1800.0)
			{
				FinishLiveCodingJob(Job);
			}
			else
			{
				bAnyRunning = true;
			}
		}
		if (!bAnyRunning)
		{
			LiveCodingTicker.Reset();
		}
		return bAnyRunning;
	}

	TSharedRef<FJsonObject> LiveCodingJobToJson(const FLiveCodingJob& Job)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("job_id"), Job.Id);
		Out->SetStringField(TEXT("state"), Job.bCompiling ? TEXT("compiling") : TEXT("completed"));
		Out->SetStringField(TEXT("result"), Job.bCompiling ? TEXT("pending") : Job.Result);
		Out->SetNumberField(TEXT("elapsed_seconds"), (Job.bCompiling ? FPlatformTime::Seconds() : Job.EndTime) - Job.StartTime);
		Out->SetArrayField(TEXT("errors"), GameplayMCP::ToJsonArray(Job.Errors));
		Out->SetArrayField(TEXT("warnings"), GameplayMCP::ToJsonArray(Job.Warnings));
		Out->SetArrayField(TEXT("output"), GameplayMCP::ToJsonArray(Job.Output));
		return Out;
	}
}

namespace GameplayLiveCoding
{
	void Shutdown()
	{
		if (LiveCodingTicker.IsValid())
		{
			FTSTicker::RemoveTicker(LiveCodingTicker);
			LiveCodingTicker.Reset();
		}
		LiveCodingJobs.Empty();
	}
}

FGameplayMCPResult UGameplayEditorToolset::editor_run_console_command(const FString& command, const FString& target, bool allow_unsafe)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	const FString Command = command;
	const GameplayConsoleFilter::FResult Filter = GameplayConsoleFilter::Check(Command, allow_unsafe, &IsRegisteredConsoleVariable);
	if (!Filter.bAllowed)
	{
		return GameplayMCP::Fail(Filter.Message);
	}

	const FString Target = GameplayMCP::IsUnset(target) ? FString(TEXT("auto")) : target.TrimStartAndEnd().ToLower();
	if (Target != TEXT("auto") && Target != TEXT("pie") && Target != TEXT("editor"))
	{
		return GameplayMCP::Fail(TEXT("target must be 'auto', 'pie' or 'editor'."));
	}
	FString Error;
	UWorld* PIEWorld = Target == TEXT("editor") ? nullptr : GameplayMCP::GetPIEWorld(Error);
	if (Target == TEXT("pie") && !PIEWorld)
	{
		return GameplayMCP::Fail(Error);
	}
	if (!GEditor)
	{
		return GameplayMCP::Fail(TEXT("The editor is not available."));
	}

	// Run exactly the segments the filter checked, one at a time.
	const uint64 FirstLogId = FGameplayLogCapture::Get().GetLastId();
	FString Output;
	FString UsedTarget = PIEWorld ? TEXT("pie") : TEXT("editor");
	bool bHandled = true;
	for (const GameplayConsoleFilter::FSegment& Segment : Filter.Segments)
	{
		if (PIEWorld)
		{
			if (APlayerController* Controller = GameplayMCP::GetLocalPlayerController(PIEWorld, 0))
			{
				// Same route as typing in the game console: player input, controller, pawn, cheat manager, game instance, engine.
				Output += Controller->ConsoleCommand(Segment.Text, /*bWriteToLog*/ true);
			}
			else
			{
				FStringOutputDevice Device;
				bHandled &= GEngine->Exec(PIEWorld, *Segment.Text, Device);
				Output += Device;
			}
		}
		else
		{
			FStringOutputDevice Device;
			bHandled &= GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Segment.Text, Device);
			Output += Device;
		}
	}

	TArray<TSharedRef<FJsonObject>> Log;
	bool bRecognized = bHandled && !Output.Contains(TEXT("Command not recognized"));
	for (const FGameplayLogCapture::FLine& Line : FGameplayLogCapture::Get().GetLines(FirstLogId, 500, [](const FGameplayLogCapture::FLine&) { return true; }))
	{
		if (Line.Message.Contains(TEXT("Command not recognized")))
		{
			bRecognized = false;
		}
		Log.Add(LineToJson(Line, false));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("command"), Command);
	Payload->SetStringField(TEXT("rule"), GameplayConsoleFilter::RuleName(Filter.Rule));
	TArray<FString> Segments;
	for (const GameplayConsoleFilter::FSegment& Segment : Filter.Segments)
	{
		Segments.Add(Segment.Text);
	}
	Payload->SetArrayField(TEXT("segments"), GameplayMCP::ToJsonArray(Segments));
	Payload->SetStringField(TEXT("target"), UsedTarget);
	Payload->SetStringField(TEXT("output"), GameplayMCP::Truncate(Output.TrimStartAndEnd(), 20000));
	Payload->SetArrayField(TEXT("log"), GameplayMCP::ToJsonArray(Log));
	Payload->SetBoolField(TEXT("recognized"), bRecognized);
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayEditorToolset::editor_get_recent_log(int32 max_lines, const FString& log_category, const FString& min_verbosity, const FString& contains, int32 after_id)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	ELogVerbosity::Type Threshold = ELogVerbosity::Log;
	if (!ParseVerbosity(min_verbosity, Threshold))
	{
		return GameplayMCP::Fail(TEXT("min_verbosity must be fatal, error, warning, display, log, verbose, veryverbose or all."));
	}
	if (max_lines < 1 || max_lines > 2000)
	{
		return GameplayMCP::Fail(TEXT("max_lines must be between 1 and 2000."));
	}
	if (after_id < 0)
	{
		return GameplayMCP::Fail(TEXT("after_id must be 0 or a line id."));
	}

	const FString Category = log_category.TrimStartAndEnd();
	const FString Contains = contains.TrimStartAndEnd();
	const TArray<FGameplayLogCapture::FLine> Lines = FGameplayLogCapture::Get().GetLines(static_cast<uint64>(after_id), max_lines,
		[&](const FGameplayLogCapture::FLine& Line)
		{
			if (Line.Verbosity > Threshold)
			{
				return false;
			}
			if (!GameplayMCP::IsUnset(Category) && !Line.Category.ToString().MatchesWildcard(Category))
			{
				return false;
			}
			return GameplayMCP::IsUnset(Contains) || Line.Message.Contains(Contains);
		});

	TArray<TSharedRef<FJsonObject>> Out;
	for (const FGameplayLogCapture::FLine& Line : Lines)
	{
		Out.Add(LineToJson(Line, true));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("latest_id"), static_cast<double>(FGameplayLogCapture::Get().GetLastId()));
	Payload->SetNumberField(TEXT("count"), Out.Num());
	Payload->SetArrayField(TEXT("lines"), GameplayMCP::ToJsonArray(Out));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayEditorToolset::editor_live_coding_compile()
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return GameplayMCP::Fail(TEXT("The Live Coding module is not loaded. Enable Live Coding in Editor Preferences > General > Live Coding."));
	}
	if (LiveCoding->IsCompiling() || LiveCodingJobs.ContainsByPredicate([](const FLiveCodingJob& Job) { return Job.bCompiling; }))
	{
		return GameplayMCP::Fail(TEXT("A Live Coding compile is already running. Poll editor_live_coding_status."));
	}
	if (!LiveCoding->IsEnabledForSession() && !LiveCoding->CanEnableForSession())
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("Live Coding cannot be enabled for this session: %s"), *LiveCoding->GetEnableErrorText().ToString()));
	}

	FLiveCodingJob Job;
	Job.Id = LiveCodingJobs.Num() > 0 ? LiveCodingJobs.Last().Id + 1 : 1;
	Job.FirstLogId = FGameplayLogCapture::Get().GetLastId();
	Job.StartTime = FPlatformTime::Seconds();
	Job.UBTLogTimestamp = IFileManager::Get().GetTimeStamp(*GetUBTLogPath());

	// Without WaitForCompletion the call returns at once with InProgress; the compile runs in the
	// Live Coding console process and the result is picked up on later ticks.
	ELiveCodingCompileResult StartResult = ELiveCodingCompileResult::NotStarted;
	LiveCoding->Compile(ELiveCodingCompileFlags::None, &StartResult);
	if (StartResult != ELiveCodingCompileResult::InProgress)
	{
		const TCHAR* Why = StartResult == ELiveCodingCompileResult::CompileStillActive ? TEXT("a previous compile is still active")
			: StartResult == ELiveCodingCompileResult::NotStarted ? TEXT("the Live Coding console could not be started")
			: TEXT("Live Coding refused the request");
		return GameplayMCP::Fail(FString::Printf(TEXT("Live Coding compile did not start: %s. %s"), Why, *LiveCoding->GetEnableErrorText().ToString()));
	}

	LiveCodingJobs.Add(MoveTemp(Job));
	if (LiveCodingJobs.Num() > 20)
	{
		LiveCodingJobs.RemoveAt(0);
	}
	if (!LiveCodingTicker.IsValid())
	{
		LiveCodingTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickLiveCoding));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("job_id"), LiveCodingJobs.Last().Id);
	Payload->SetStringField(TEXT("state"), TEXT("compiling"));
	Payload->SetStringField(TEXT("next"), TEXT("Poll editor_live_coding_status with this job_id until state is 'completed'."));
	return GameplayMCP::Ok(Payload);
#else
	return GameplayMCP::Fail(TEXT("Live Coding is not available in this build (it requires a Win64 editor built with Live Coding)."));
#endif
}

FGameplayMCPResult UGameplayEditorToolset::editor_live_coding_status(const FString& job_id)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	if (LiveCodingJobs.Num() == 0)
	{
		return GameplayMCP::Fail(TEXT("No Live Coding compile has been started with editor_live_coding_compile yet."));
	}
	const FString Id = job_id.TrimStartAndEnd();
	if (GameplayMCP::IsUnset(Id) || Id.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
	{
		return GameplayMCP::Ok(LiveCodingJobToJson(LiveCodingJobs.Last()));
	}
	for (const FLiveCodingJob& Job : LiveCodingJobs)
	{
		if (Id.IsNumeric() && Job.Id == FCString::Atoi(*Id))
		{
			return GameplayMCP::Ok(LiveCodingJobToJson(Job));
		}
	}
	return GameplayMCP::Fail(FString::Printf(TEXT("No Live Coding job %s. Use 'latest'."), *job_id));
}
