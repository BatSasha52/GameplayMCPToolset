// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Keeps the most recent output-log lines in memory so tools can read them back with their category
 * and verbosity. Installed on GLog when the module starts, so it sees everything logged after that.
 * Safe to call from any thread.
 */
class FGameplayLogCapture : public FOutputDevice
{
public:
	struct FLine
	{
		uint64 Id = 0;
		double Time = 0.0;
		FName Category;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FString Message;
	};

	static FGameplayLogCapture& Get();

	void Startup();
	void Shutdown();

	/** Id of the newest captured line (0 if none). Lines logged later have larger ids. */
	uint64 GetLastId() const;

	/** Lines with Id > AfterId that pass Filter, oldest first, at most MaxLines (the newest ones). */
	TArray<FLine> GetLines(uint64 AfterId, int32 MaxLines, TFunctionRef<bool(const FLine&)> Filter) const;

	int32 Num() const;

	static constexpr int32 Capacity = 10000;

	//~ FOutputDevice
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	mutable FCriticalSection Lock;
	TArray<FLine> Lines;
	uint64 NextId = 1;
	bool bInstalled = false;
};
