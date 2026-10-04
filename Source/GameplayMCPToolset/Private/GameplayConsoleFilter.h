// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/**
 * Decides whether editor_run_console_command may run a console command. Pure logic over strings: no
 * UObject, world or editor access, so it can be tested as a table of cases.
 *
 * How the engine reads a command line (UE 5.8.3):
 * - FParse::Line splits a line on unquoted '|' and on '\n'/'\r'.
 * - FParse::Command skips spaces/tabs and matches a command name case-insensitively when the next
 *   character is NOT alphanumeric. '_' and '.' therefore end a match: "QUIT_EDITOR_TYPO_CHECK" runs
 *   QUIT_EDITOR (and would also match QUIT). Every handled name behaves like a prefix.
 *
 * So the filter splits on '|', ';', '\n' and '\r' (a superset of what the engine splits on) and checks
 * every segment. In each segment, leading whitespace, quotes and control characters are stripped
 * and the first word is checked:
 * 1. Hard deny: the word starts with a hard-deny prefix, contains a hard-deny keyword, or is
 *    'obj savepackage'. Refused even with allow_unsafe.
 * 2. Allowlist: the word is exactly a known-safe command ('obj' only with 'list'/'dump'), or is a
 *    registered console variable (read or set).
 * 3. Anything else is refused unless allow_unsafe is true.
 */
namespace GameplayConsoleFilter
{
	enum class ERule : uint8
	{
		/** On the allowlist or a console variable. */
		Allowed,
		/** Not on the allowlist, but allow_unsafe was set. */
		AllowedUnsafe,
		/** Nothing to run. */
		Empty,
		/** Matches the hard-deny list; refused even with allow_unsafe. */
		HardDeny,
		/** Not on the allowlist and allow_unsafe was not set. */
		AllowlistMiss,
	};

	struct FSegment
	{
		/** The segment as it will be executed (normalized). */
		FString Text;
		/** Its first word, lower-case. */
		FString Word;
		ERule Rule = ERule::Empty;
		/** What matched: the hard-deny prefix/keyword, the allowlist entry, or "console variable". */
		FString Matched;
	};

	struct FResult
	{
		bool bAllowed = false;
		/** Allowed / AllowedUnsafe when allowed, otherwise the rule of the first refused segment. */
		ERule Rule = ERule::Empty;
		/** Non-empty segments in order. Only these are executed, one by one. */
		TArray<FSegment> Segments;
		/** Human-readable reason, naming the command and the rule. */
		FString Message;
	};

	/**
	 * Checks a whole command line.
	 * @param IsConsoleVariable Returns true if the given word names a registered console variable.
	 */
	FResult Check(const FString& Command, bool bAllowUnsafe, TFunctionRef<bool(const FString&)> IsConsoleVariable);

	FString RuleName(ERule Rule);

	/** Lower-case prefixes refused as the first word of any segment. */
	const TArray<FString>& HardDenyPrefixes();

	/** Lower-case substrings refused anywhere in the first word of any segment. */
	const TArray<FString>& HardDenyKeywords();

	/** Lower-case first words that are always allowed (besides console variables). */
	const TArray<FString>& AllowedCommands();
}
