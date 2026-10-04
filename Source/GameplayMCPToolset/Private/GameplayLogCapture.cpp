// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayLogCapture.h"

#include "Algo/Reverse.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

FGameplayLogCapture& FGameplayLogCapture::Get()
{
	static FGameplayLogCapture Instance;
	return Instance;
}

void FGameplayLogCapture::Startup()
{
	if (!bInstalled && GLog)
	{
		GLog->AddOutputDevice(this);
		bInstalled = true;
	}
}

void FGameplayLogCapture::Shutdown()
{
	if (bInstalled && GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
	bInstalled = false;
	FScopeLock ScopeLock(&Lock);
	Lines.Empty();
}

void FGameplayLogCapture::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FLine Line;
	Line.Time = FPlatformTime::Seconds() - GStartTime;
	Line.Category = Category;
	Line.Verbosity = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	Line.Message = Message;

	FScopeLock ScopeLock(&Lock);
	Line.Id = NextId++;
	Lines.Add(MoveTemp(Line));
	// Trim in chunks so appending stays cheap.
	if (Lines.Num() > Capacity + Capacity / 5)
	{
		Lines.RemoveAt(0, Lines.Num() - Capacity, EAllowShrinking::No);
	}
}

uint64 FGameplayLogCapture::GetLastId() const
{
	FScopeLock ScopeLock(&Lock);
	return NextId - 1;
}

int32 FGameplayLogCapture::Num() const
{
	FScopeLock ScopeLock(&Lock);
	return Lines.Num();
}

TArray<FGameplayLogCapture::FLine> FGameplayLogCapture::GetLines(uint64 AfterId, int32 MaxLines, TFunctionRef<bool(const FLine&)> Filter) const
{
	TArray<FLine> Out;
	FScopeLock ScopeLock(&Lock);
	for (int32 Index = Lines.Num() - 1; Index >= 0 && Out.Num() < MaxLines; --Index)
	{
		const FLine& Line = Lines[Index];
		if (Line.Id <= AfterId)
		{
			break;
		}
		if (Filter(Line))
		{
			Out.Add(Line);
		}
	}
	Algo::Reverse(Out);
	return Out;
}
