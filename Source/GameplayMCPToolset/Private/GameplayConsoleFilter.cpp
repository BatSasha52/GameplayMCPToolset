// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayConsoleFilter.h"

namespace GameplayConsoleFilter
{
	const TArray<FString>& HardDenyPrefixes()
	{
		// Each entry is matched as a prefix of the first word, because the engine matches command names
		// that way (FParse::Command). Sources in UE 5.8.3:
		//   quit, exit             UGameEngine EXIT/QUIT; in PIE they end the session
		//   quit_editor            UEditorEngine::Exec QUIT_EDITOR -> CloseEditor()
		//   close_slate_mainframe  UEditorEngine::Exec -> IMainFrameModule::RequestCloseEditor()
		//   close_editor           not an engine command in 5.8.3; refused defensively
		//   debug                  UEngine::HandleDebugCommand: crash, gpf, hang, assert, ensure, fatal,
		//                          stackoverflow, terminate, abort, gpucrash...
		//   crash, gpf, fatal, terminate, abort, stackoverflow, shutdown
		//                          the same family if reached without 'debug'; refused defensively
		//   disconnect             UEngine/UGameInstance DISCONNECT; ends the PIE session
		//   exec                   runs a file of commands this filter never sees
		//   py, python             runs arbitrary Python, which can do anything
		static const TArray<FString> Prefixes = {
			TEXT("quit"), TEXT("exit"), TEXT("quit_editor"), TEXT("close_editor"), TEXT("close_slate_mainframe"),
			TEXT("debug"), TEXT("crash"), TEXT("gpf"), TEXT("fatal"), TEXT("terminate"), TEXT("abort"),
			TEXT("stackoverflow"), TEXT("shutdown"), TEXT("disconnect"), TEXT("exec"), TEXT("py"), TEXT("python"),
		};
		return Prefixes;
	}

	const TArray<FString>& HardDenyKeywords()
	{
		// Anywhere in the first word, e.g. console variables such as csv.ExitOnCompletion or
		// commands named *Crash*. Refused even with allow_unsafe.
		static const TArray<FString> Keywords = {
			TEXT("quit"), TEXT("exit"), TEXT("crash"), TEXT("terminate"), TEXT("shutdown"),
		};
		return Keywords;
	}

	const TArray<FString>& AllowedCommands()
	{
		// Read-only or tuning commands, each checked against the 5.8.3 source:
		//   stat, pause, freezerendering, obj (list/dump only)    UEngine::Exec
		//   show, viewmode, display, displayall                    UGameViewportClient::Exec
		//   showdebug                                              AHUD exec
		//   slomo, god, fly, ghost, walk, toggledebugcamera        UCheatManager exec (PIE only)
		//   restartlevel                                           APlayerController exec (PIE only; reloads the level)
		static const TArray<FString> Commands = {
			TEXT("stat"), TEXT("pause"), TEXT("freezerendering"), TEXT("obj"),
			TEXT("show"), TEXT("viewmode"), TEXT("display"), TEXT("displayall"),
			TEXT("showdebug"),
			TEXT("slomo"), TEXT("god"), TEXT("fly"), TEXT("ghost"), TEXT("walk"), TEXT("toggledebugcamera"),
			TEXT("restartlevel"),
		};
		return Commands;
	}

	FString RuleName(ERule Rule)
	{
		switch (Rule)
		{
		case ERule::Allowed:       return TEXT("allowed");
		case ERule::AllowedUnsafe: return TEXT("allowed_unsafe");
		case ERule::Empty:         return TEXT("empty");
		case ERule::HardDeny:      return TEXT("hard_deny");
		default:                   return TEXT("allowlist_miss");
		}
	}

	namespace
	{
		bool IsSeparator(TCHAR Char)
		{
			return Char == TEXT('|') || Char == TEXT(';') || Char == TEXT('\n') || Char == TEXT('\r');
		}

		bool IsStrippable(TCHAR Char)
		{
			return FChar::IsWhitespace(Char) || Char < 32 || Char == 127 || Char == TEXT('"') || Char == TEXT('\'') || Char == TEXT('`');
		}

		/** Splits on every separator, ignoring quotes: checking more segments than the engine runs is safe. */
		TArray<FString> SplitSegments(const FString& Command)
		{
			TArray<FString> Out;
			FString Current;
			for (const TCHAR Char : Command)
			{
				if (IsSeparator(Char))
				{
					Out.Add(MoveTemp(Current));
					Current.Reset();
				}
				else
				{
					Current.AppendChar(Char);
				}
			}
			Out.Add(MoveTemp(Current));
			return Out;
		}

		FString Normalize(const FString& Segment)
		{
			int32 Start = 0;
			while (Start < Segment.Len() && IsStrippable(Segment[Start]))
			{
				++Start;
			}
			int32 End = Segment.Len();
			while (End > Start && (FChar::IsWhitespace(Segment[End - 1]) || Segment[End - 1] < 32))
			{
				--End;
			}
			return Segment.Mid(Start, End - Start);
		}

		void SplitWords(const FString& Text, FString& OutFirst, FString& OutSecond)
		{
			TArray<FString> Words;
			Text.ParseIntoArrayWS(Words);
			OutFirst = Words.Num() > 0 ? Words[0].ToLower() : FString();
			OutSecond = Words.Num() > 1 ? Words[1].ToLower() : FString();
		}

		void CheckSegment(FSegment& Segment, bool bAllowUnsafe, TFunctionRef<bool(const FString&)> IsConsoleVariable)
		{
			FString Second;
			SplitWords(Segment.Text, Segment.Word, Second);
			const FString& Word = Segment.Word;

			for (const FString& Prefix : HardDenyPrefixes())
			{
				if (Word.StartsWith(Prefix))
				{
					Segment.Rule = ERule::HardDeny;
					Segment.Matched = FString::Printf(TEXT("prefix '%s'"), *Prefix);
					return;
				}
			}
			for (const FString& Keyword : HardDenyKeywords())
			{
				if (Word.Contains(Keyword))
				{
					Segment.Rule = ERule::HardDeny;
					Segment.Matched = FString::Printf(TEXT("keyword '%s'"), *Keyword);
					return;
				}
			}
			if (Word == TEXT("obj") && Second.StartsWith(TEXT("savepackage")))
			{
				Segment.Rule = ERule::HardDeny;
				Segment.Matched = TEXT("'obj savepackage' (writes packages to disk)");
				return;
			}

			if (Word == TEXT("obj"))
			{
				if (Second == TEXT("list") || Second == TEXT("dump"))
				{
					Segment.Rule = ERule::Allowed;
					Segment.Matched = FString::Printf(TEXT("obj %s"), *Second);
					return;
				}
			}
			else if (AllowedCommands().Contains(Word))
			{
				Segment.Rule = ERule::Allowed;
				Segment.Matched = Word;
				return;
			}
			else if (IsConsoleVariable(Word))
			{
				Segment.Rule = ERule::Allowed;
				Segment.Matched = TEXT("console variable");
				return;
			}

			Segment.Rule = bAllowUnsafe ? ERule::AllowedUnsafe : ERule::AllowlistMiss;
			Segment.Matched = bAllowUnsafe ? TEXT("allow_unsafe") : FString();
		}
	}

	FResult Check(const FString& Command, bool bAllowUnsafe, TFunctionRef<bool(const FString&)> IsConsoleVariable)
	{
		FResult Result;
		for (const FString& Raw : SplitSegments(Command))
		{
			FSegment Segment;
			Segment.Text = Normalize(Raw);
			if (Segment.Text.IsEmpty())
			{
				continue;
			}
			CheckSegment(Segment, bAllowUnsafe, IsConsoleVariable);
			Result.Segments.Add(MoveTemp(Segment));
		}

		if (Result.Segments.Num() == 0)
		{
			Result.Rule = ERule::Empty;
			Result.Message = TEXT("The command is empty.");
			return Result;
		}

		// A hard deny anywhere wins over an allowlist miss elsewhere.
		const FSegment* Refused = Result.Segments.FindByPredicate([](const FSegment& Segment) { return Segment.Rule == ERule::HardDeny; });
		if (!Refused)
		{
			Refused = Result.Segments.FindByPredicate([](const FSegment& Segment) { return Segment.Rule == ERule::AllowlistMiss; });
		}
		if (Refused)
		{
			const int32 Index = static_cast<int32>(Refused - Result.Segments.GetData());
			const FString Where = Result.Segments.Num() > 1 ? FString::Printf(TEXT(" (segment %d of %d)"), Index + 1, Result.Segments.Num()) : FString();
			Result.Rule = Refused->Rule;
			if (Refused->Rule == ERule::HardDeny)
			{
				Result.Message = FString::Printf(TEXT("Refused '%s'%s by rule hard_deny: it matches %s. The engine treats any command starting with a blocked name as that command (e.g. QUIT_EDITOR_TYPO_CHECK runs QUIT_EDITOR). Commands that quit, close or crash the editor, end PIE, run scripts or save packages are never run, even with allow_unsafe=true. Use pie_stop to end PIE."),
					*Refused->Text, *Where, *Refused->Matched);
			}
			else
			{
				Result.Message = FString::Printf(TEXT("Refused '%s'%s by rule allowlist_miss: '%s' is not a known-safe command or a console variable. Allowed without allow_unsafe: %s, obj list, obj dump, and reading or setting console variables (e.g. 'r.VSync', 't.MaxFPS 60'). If you are sure the command is safe, call again with allow_unsafe=true."),
					*Refused->Text, *Where, *Refused->Word, *FString::Join(AllowedCommands().FilterByPredicate([](const FString& Name) { return Name != TEXT("obj"); }), TEXT(", ")));
			}
			return Result;
		}

		Result.bAllowed = true;
		Result.Rule = Result.Segments.ContainsByPredicate([](const FSegment& Segment) { return Segment.Rule == ERule::AllowedUnsafe; }) ? ERule::AllowedUnsafe : ERule::Allowed;
		return Result;
	}
}
