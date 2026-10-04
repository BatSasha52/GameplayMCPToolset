// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#include "GameplayConsoleFilter.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Table test for the console command filter. It only calls GameplayConsoleFilter::Check; no command is
 * ever executed. Run with: Automation RunTests GameplayMCPToolset.ConsoleFilter
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayConsoleFilterTest, "GameplayMCPToolset.ConsoleFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGameplayConsoleFilterTest::RunTest(const FString& Parameters)
{
	using namespace GameplayConsoleFilter;

	// Stand-in for the console manager: these names count as registered console variables.
	const TSet<FString> ConsoleVariables = { TEXT("r.vsync"), TEXT("t.maxfps"), TEXT("p.gravityz"), TEXT("csv.exitoncompletion") };
	auto IsConsoleVariable = [&ConsoleVariables](const FString& Word) { return ConsoleVariables.Contains(Word); };

	struct FCase
	{
		const TCHAR* Command;
		bool bAllowUnsafe;
		ERule Expected;
		/** Text the refusal message must contain (names the rule / match). */
		const TCHAR* MessageContains;
	};

	const FCase Cases[] = {
		// Hard deny: exact names, case, engine prefix matching, leading junk.
		{ TEXT("quit"),                     false, ERule::HardDeny, TEXT("prefix 'quit'") },
		{ TEXT("QUIT"),                     false, ERule::HardDeny, TEXT("prefix 'quit'") },
		{ TEXT("Exit"),                     false, ERule::HardDeny, TEXT("prefix 'exit'") },
		{ TEXT("quit_editor"),              false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("QUIT_EDITOR_TYPO_CHECK"),   false, ERule::HardDeny, TEXT("QUIT_EDITOR_TYPO_CHECK") },
		{ TEXT("close_editor"),             false, ERule::HardDeny, TEXT("prefix 'close_editor'") },
		{ TEXT("CLOSE_SLATE_MAINFRAME"),    false, ERule::HardDeny, TEXT("prefix 'close_slate_mainframe'") },
		{ TEXT(" quit"),                    false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("\tquit"),                   false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("\"quit\""),                 false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("\x01\x02quit"),             false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("quit.now"),                 false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("debug crash"),              false, ERule::HardDeny, TEXT("prefix 'debug'") },
		{ TEXT("DEBUG HANG"),               false, ERule::HardDeny, TEXT("prefix 'debug'") },
		{ TEXT("crash"),                    false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("disconnect"),               false, ERule::HardDeny, TEXT("prefix 'disconnect'") },
		{ TEXT("py print(1)"),              false, ERule::HardDeny, TEXT("prefix 'py'") },
		{ TEXT("exec commands.txt"),        false, ERule::HardDeny, TEXT("prefix 'exec'") },
		{ TEXT("obj savepackage /Game/A"),  false, ERule::HardDeny, TEXT("obj savepackage") },
		{ TEXT("csv.ExitOnCompletion 1"),   false, ERule::HardDeny, TEXT("keyword 'exit'") },
		{ TEXT("r.ForceGPUCrash"),          false, ERule::HardDeny, TEXT("keyword 'crash'") },

		// Hard deny wins even with allow_unsafe.
		{ TEXT("quit"),                     true,  ERule::HardDeny, TEXT("even with allow_unsafe=true") },
		{ TEXT("QUIT_EDITOR_TYPO_CHECK"),   true,  ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("debug crash"),              true,  ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("obj savepackage /Game/A"),  true,  ERule::HardDeny, TEXT("hard_deny") },

		// Chains: every segment is checked, whatever the separator.
		{ TEXT("stat fps; quit"),           false, ERule::HardDeny, TEXT("segment 2 of 2") },
		{ TEXT("stat fps;quit"),            false, ERule::HardDeny, TEXT("segment 2 of 2") },
		{ TEXT("stat fps\nquit"),           false, ERule::HardDeny, TEXT("segment 2 of 2") },
		{ TEXT("stat fps\r\nquit"),         false, ERule::HardDeny, TEXT("segment 2 of 2") },
		{ TEXT("stat fps | exit"),          false, ERule::HardDeny, TEXT("segment 2 of 2") },
		{ TEXT("stat fps|\"quit\""),        false, ERule::HardDeny, TEXT("hard_deny") },
		{ TEXT("slomo 0.5; stat unit; QUIT_EDITOR"), true, ERule::HardDeny, TEXT("segment 3 of 3") },
		{ TEXT("stat fps; transaction undo"), false, ERule::AllowlistMiss, TEXT("segment 2 of 2") },

		// Allowed without a flag.
		{ TEXT("stat fps"),                 false, ERule::Allowed, TEXT("") },
		{ TEXT("STAT unit"),                false, ERule::Allowed, TEXT("") },
		{ TEXT("slomo 0.5"),                false, ERule::Allowed, TEXT("") },
		{ TEXT("r.VSync"),                  false, ERule::Allowed, TEXT("") },
		{ TEXT("r.VSync 0"),                false, ERule::Allowed, TEXT("") },
		{ TEXT("t.MaxFPS 60"),              false, ERule::Allowed, TEXT("") },
		{ TEXT("p.GravityZ"),               false, ERule::Allowed, TEXT("") },
		{ TEXT("showdebug animation"),      false, ERule::Allowed, TEXT("") },
		{ TEXT("viewmode unlit"),           false, ERule::Allowed, TEXT("") },
		{ TEXT("pause"),                    false, ERule::Allowed, TEXT("") },
		{ TEXT("obj list class=Actor"),     false, ERule::Allowed, TEXT("") },
		{ TEXT("obj dump Foo"),             false, ERule::Allowed, TEXT("") },
		{ TEXT("god"),                      false, ERule::Allowed, TEXT("") },
		{ TEXT("restartlevel"),             false, ERule::Allowed, TEXT("") },
		{ TEXT("stat fps; slomo 0.5"),      false, ERule::Allowed, TEXT("") },
		{ TEXT("  stat fps  "),             false, ERule::Allowed, TEXT("") },

		// Unknown or not-allowlisted commands need allow_unsafe.
		{ TEXT("gmcp_not_a_command"),       false, ERule::AllowlistMiss, TEXT("allowlist_miss") },
		{ TEXT("transaction undo"),         false, ERule::AllowlistMiss, TEXT("allow_unsafe=true") },
		{ TEXT("obj gc"),                   false, ERule::AllowlistMiss, TEXT("allowlist_miss") },
		{ TEXT("stat_fps"),                 false, ERule::AllowlistMiss, TEXT("allowlist_miss") },
		{ TEXT("open MyMap"),               false, ERule::AllowlistMiss, TEXT("allowlist_miss") },
		{ TEXT("r.RecompileShaders"),       false, ERule::AllowlistMiss, TEXT("allowlist_miss") },
		{ TEXT("gmcp_not_a_command"),       true,  ERule::AllowedUnsafe, TEXT("") },
		{ TEXT("transaction undo"),         true,  ERule::AllowedUnsafe, TEXT("") },
		{ TEXT("stat fps; transaction undo"), true, ERule::AllowedUnsafe, TEXT("") },

		// Nothing to run.
		{ TEXT(""),                         false, ERule::Empty, TEXT("empty") },
		{ TEXT("  ;  |\n"),                 true,  ERule::Empty, TEXT("empty") },
	};

	int32 Index = 0;
	for (const FCase& Case : Cases)
	{
		const FResult Result = Check(Case.Command, Case.bAllowUnsafe, IsConsoleVariable);
		const FString Label = FString::Printf(TEXT("case %d '%s' (allow_unsafe=%s)"), Index++, *FString(Case.Command).ReplaceCharWithEscapedChar(), Case.bAllowUnsafe ? TEXT("true") : TEXT("false"));
		TestEqual(Label + TEXT(" rule"), RuleName(Result.Rule), RuleName(Case.Expected));
		const bool bShouldAllow = Case.Expected == ERule::Allowed || Case.Expected == ERule::AllowedUnsafe;
		TestEqual(Label + TEXT(" allowed"), Result.bAllowed, bShouldAllow);
		if (!bShouldAllow)
		{
			TestTrue(Label + TEXT(" message mentions '") + Case.MessageContains + TEXT("': ") + Result.Message,
				Result.Message.Contains(Case.MessageContains));
		}
	}

	// Only checked segments are executed, and they are the normalized text.
	const FResult Chain = Check(TEXT(" stat fps |\tslomo 0.5 ;; "), false, IsConsoleVariable);
	TestEqual(TEXT("chain segment count"), Chain.Segments.Num(), 2);
	if (Chain.Segments.Num() == 2)
	{
		TestEqual(TEXT("chain segment 1"), Chain.Segments[0].Text, FString(TEXT("stat fps")));
		TestEqual(TEXT("chain segment 2"), Chain.Segments[1].Text, FString(TEXT("slomo 0.5")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
