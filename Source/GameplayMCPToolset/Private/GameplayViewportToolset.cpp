// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayViewportToolset.h"

#include "GameplayMCPHelpers.h"

#include "Containers/Ticker.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

namespace
{
	/** Real seconds to wait for the viewport to render before giving up (e.g. the editor is minimized). */
	constexpr double ScreenshotTimeoutSeconds = 15.0;

	struct FScreenshotJob
	{
		int32 Id = 0;
		FString FilePath;
		int32 Width = 0;
		int32 Height = 0;
		FString State = TEXT("pending");
		FString Error;
		double StartTime = 0.0;
		int64 FileSize = 0;
	};

	TArray<FScreenshotJob> Jobs;
	int32 PendingJobId = INDEX_NONE;
	FDelegateHandle CapturedHandle;
	FDelegateHandle CapturedHDRHandle;
	FTSTicker::FDelegateHandle TickerHandle;

	FScreenshotJob* FindJob(int32 Id)
	{
		return Jobs.FindByPredicate([Id](const FScreenshotJob& Job) { return Job.Id == Id; });
	}

	void Unbind()
	{
		if (CapturedHandle.IsValid())
		{
			UGameViewportClient::OnScreenshotCaptured().Remove(CapturedHandle);
			CapturedHandle.Reset();
		}
		if (CapturedHDRHandle.IsValid())
		{
			UGameViewportClient::OnHDRScreenshotCaptured().Remove(CapturedHDRHandle);
			CapturedHDRHandle.Reset();
		}
		if (TickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
		PendingJobId = INDEX_NONE;
	}

	void FinishPending(const FString& State, const FString& Error)
	{
		if (FScreenshotJob* Job = FindJob(PendingJobId))
		{
			Job->State = State;
			Job->Error = Error;
		}
		Unbind();
	}

	void OnCaptured(int32 Width, int32 Height, const TArray<FColor>& Pixels)
	{
		FScreenshotJob* Job = FindJob(PendingJobId);
		if (!Job)
		{
			Unbind();
			return;
		}
		if (Pixels.Num() != Width * Height || Width <= 0 || Height <= 0)
		{
			FinishPending(TEXT("failed"), FString::Printf(TEXT("the viewport returned %d pixels for %dx%d"), Pixels.Num(), Width, Height));
			return;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Job->FilePath), /*Tree*/ true);
		if (Png.Num() == 0 || !FFileHelper::SaveArrayToFile(Png, *Job->FilePath))
		{
			FinishPending(TEXT("failed"), FString::Printf(TEXT("could not write %s"), *Job->FilePath));
			return;
		}
		Job->Width = Width;
		Job->Height = Height;
		Job->FileSize = Png.Num();
		FinishPending(TEXT("completed"), FString());
	}

	void OnCapturedHDR(int32 Width, int32 Height, const TArray<FLinearColor>& Pixels)
	{
		// HDR viewports deliver linear colour; store an sRGB PNG like the LDR path.
		TArray<FColor> Converted;
		Converted.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels)
		{
			Converted.Add(Pixel.ToFColor(/*bSRGB*/ true));
		}
		OnCaptured(Width, Height, Converted);
	}

	bool Tick(float DeltaTime)
	{
		FScreenshotJob* Job = FindJob(PendingJobId);
		if (!Job)
		{
			Unbind();
			return false;
		}
		FString Unused;
		if (!GameplayMCP::GetPIEWorld(Unused))
		{
			FinishPending(TEXT("failed"), TEXT("the PIE session ended before the frame was captured"));
			return false;
		}
		if (FPlatformTime::Seconds() - Job->StartTime > ScreenshotTimeoutSeconds)
		{
			FinishPending(TEXT("failed"), TEXT("the game viewport did not render a frame in time (is the editor window minimized or PIE paused in a hidden window?)"));
			return false;
		}
		return true;
	}

	bool MakeFilePath(const FString& FileName, bool bOverwrite, int32 JobId, FString& OutPath, FString& OutError)
	{
		FString Name = FileName.TrimStartAndEnd();
		if (GameplayMCP::IsUnset(Name) || Name.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			Name = FString::Printf(TEXT("Shot_%s_%d"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), JobId);
		}
		if (Name.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
		{
			Name.LeftChopInline(4);
		}
		for (const TCHAR Char : Name)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-') && Char != TEXT('.'))
			{
				OutError = FString::Printf(TEXT("file_name '%s' may only contain letters, digits, '_', '-' and '.' (no folders)."), *FileName);
				return false;
			}
		}
		if (Name.IsEmpty() || Name.StartsWith(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("file_name '%s' is not a valid file name."), *FileName);
			return false;
		}
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("GameplayMCP")));
		OutPath = FPaths::Combine(Folder, Name + TEXT(".png"));
		if (!bOverwrite && IFileManager::Get().FileExists(*OutPath))
		{
			OutError = FString::Printf(TEXT("%s already exists. Pass overwrite=true or another file_name."), *OutPath);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> JobToJson(const FScreenshotJob& Job)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("job_id"), Job.Id);
		Out->SetStringField(TEXT("state"), Job.State);
		Out->SetStringField(TEXT("file_path"), Job.FilePath);
		Out->SetNumberField(TEXT("width"), Job.Width);
		Out->SetNumberField(TEXT("height"), Job.Height);
		Out->SetNumberField(TEXT("file_size"), static_cast<double>(Job.FileSize));
		Out->SetStringField(TEXT("error"), Job.Error);
		return Out;
	}
}

namespace GameplayScreenshots
{
	void Shutdown()
	{
		Unbind();
		Jobs.Empty();
	}
}

FGameplayMCPResult UGameplayViewportToolset::game_capture_screenshot(int32 width, int32 height, const FString& file_name, bool overwrite)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UWorld* World = GameplayMCP::GetPIEWorld(Error);
	if (!World)
	{
		return GameplayMCP::Fail(Error);
	}
	UGameViewportClient* ViewportClient = World->GetGameViewport();
	FSceneViewport* Viewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
	if (!Viewport)
	{
		return GameplayMCP::Fail(TEXT("The PIE session has no game viewport to capture (for example it runs as a dedicated server or in a separate process)."));
	}
	const int32 MaxDimension = FMath::Min<int32>(7680, static_cast<int32>(GetMax2DTextureDimension()));
	if (width < 64 || height < 64 || width > MaxDimension || height > MaxDimension)
	{
		return GameplayMCP::Fail(FString::Printf(TEXT("width and height must be between 64 and %d."), MaxDimension));
	}
	if (PendingJobId != INDEX_NONE || GIsHighResScreenshot)
	{
		return GameplayMCP::Fail(TEXT("Another screenshot is being captured. Poll game_screenshot_status and try again when it completes."));
	}

	const int32 JobId = Jobs.Num() > 0 ? Jobs.Last().Id + 1 : 1;
	FString FilePath;
	if (!MakeFilePath(file_name, overwrite, JobId, FilePath, Error))
	{
		return GameplayMCP::Fail(Error);
	}

	// Engine high-res screenshot path: the next viewport draw renders the game viewport client into an
	// offscreen viewport of this size (no Slate, so no editor UI) and hands the pixels to the delegate.
	GScreenshotResolutionX = width;
	GScreenshotResolutionY = height;
	CapturedHandle = UGameViewportClient::OnScreenshotCaptured().AddStatic(&OnCaptured);
	CapturedHDRHandle = UGameViewportClient::OnHDRScreenshotCaptured().AddStatic(&OnCapturedHDR);
	if (!Viewport->TakeHighResScreenShot())
	{
		Unbind();
		return GameplayMCP::Fail(FString::Printf(TEXT("The engine refused a %dx%d screenshot (larger than this GPU allows)."), width, height));
	}

	FScreenshotJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = JobId;
	Job.FilePath = FilePath;
	Job.Width = width;
	Job.Height = height;
	Job.StartTime = FPlatformTime::Seconds();
	PendingJobId = JobId;
	if (Jobs.Num() > 50)
	{
		Jobs.RemoveAt(0);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));

	TSharedRef<FJsonObject> Payload = JobToJson(Job);
	Payload->SetStringField(TEXT("next"), TEXT("Poll game_screenshot_status with this job_id until state is 'completed', then open file_path."));
	return GameplayMCP::Ok(Payload);
}

FGameplayMCPResult UGameplayViewportToolset::game_screenshot_status(const FString& job_id)
{
	GAMEPLAYMCP_REQUIRE_GAME_THREAD();

	if (Jobs.Num() == 0)
	{
		return GameplayMCP::Fail(TEXT("No screenshot has been requested with game_capture_screenshot yet."));
	}
	const FString Id = job_id.TrimStartAndEnd();
	if (GameplayMCP::IsUnset(Id) || Id.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
	{
		return GameplayMCP::Ok(JobToJson(Jobs.Last()));
	}
	if (const FScreenshotJob* Job = Id.IsNumeric() ? FindJob(FCString::Atoi(*Id)) : nullptr)
	{
		return GameplayMCP::Ok(JobToJson(*Job));
	}
	return GameplayMCP::Fail(FString::Printf(TEXT("No screenshot job %s. Use 'latest'."), *job_id));
}
