// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AutomatedAssetImportData.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentAssetOps
{

	struct FConsoleExecOutputLine
	{
		FString Category;
		FString Verbosity;
		FString Message;
	};

	static FString LogVerbosityToString(const ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal: return TEXT("fatal");
		case ELogVerbosity::Error: return TEXT("error");
		case ELogVerbosity::Warning: return TEXT("warning");
		case ELogVerbosity::Display: return TEXT("display");
		case ELogVerbosity::Log: return TEXT("log");
		case ELogVerbosity::Verbose: return TEXT("verbose");
		case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
		default: return TEXT("unknown");
		}
	}

	class FConsoleExecCaptureOutputDevice final : public FOutputDevice
	{
	public:
		explicit FConsoleExecCaptureOutputDevice(const int32 InMaxOutputChars)
			: MaxOutputChars(FMath::Max(0, InMaxOutputChars))
		{
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FConsoleExecOutputLine Line;
			Line.Category = Category.ToString();
			Line.Verbosity = LogVerbosityToString(Verbosity);
			Line.Message = V ? FString(V) : FString();
			Lines.Add(MoveTemp(Line));

			if (MaxOutputChars <= 0)
			{
				bOutputTruncated = true;
				return;
			}

			FString TextLine = FString::Printf(TEXT("[%s][%s] %s"), *Lines.Last().Category, *Lines.Last().Verbosity, *Lines.Last().Message);
			if (!CombinedOutput.IsEmpty())
			{
				TextLine = FString(LINE_TERMINATOR) + TextLine;
			}

			const int32 RemainingChars = MaxOutputChars - CombinedOutput.Len();
			if (RemainingChars <= 0)
			{
				bOutputTruncated = true;
				return;
			}

			if (TextLine.Len() > RemainingChars)
			{
				CombinedOutput += TextLine.Left(RemainingChars);
				bOutputTruncated = true;
				return;
			}

			CombinedOutput += TextLine;
		}

		TArray<TSharedPtr<FJsonValue>> MakeLinesJson(const int32 MaxLineCount) const
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			const int32 ClampedMaxLineCount = MaxLineCount < 0 ? Lines.Num() : FMath::Max(0, MaxLineCount);
			for (int32 Index = 0; Index < Lines.Num() && Index < ClampedMaxLineCount; ++Index)
			{
				TSharedPtr<FJsonObject> LineObj = MakeShared<FJsonObject>();
				LineObj->SetStringField(TEXT("category"), Lines[Index].Category);
				LineObj->SetStringField(TEXT("verbosity"), Lines[Index].Verbosity);
				LineObj->SetStringField(TEXT("message"), Lines[Index].Message);
				Values.Add(MakeShared<FJsonValueObject>(LineObj));
			}
			return Values;
		}

		const FString& GetCombinedOutput() const
		{
			return CombinedOutput;
		}

		bool IsOutputTruncated() const
		{
			return bOutputTruncated;
		}

		int32 GetLineCount() const
		{
			return Lines.Num();
		}

	private:
		int32 MaxOutputChars = 0;
		bool bOutputTruncated = false;
		FString CombinedOutput;
		TArray<FConsoleExecOutputLine> Lines;
	};

	static FString NormalizeConsoleExecToken(const FString& Token)
	{
		FString Normalized = Token;
		Normalized.TrimStartAndEndInline();
		Normalized.ToUpperInline();
		return Normalized;
	}

	static TArray<FString> TokenizeConsoleExecCommand(const FString& Command)
	{
		TArray<FString> Tokens;
		const TCHAR* Cursor = *Command;
		FString Token;
		while (FParse::Token(Cursor, Token, false))
		{
			Tokens.Add(NormalizeConsoleExecToken(Token));
			Token.Reset();
			if (Tokens.Num() >= 8)
			{
				break;
			}
		}
		return Tokens;
	}

	static FString ConsoleExecHighRiskReason(const FString& Command)
	{
		const TArray<FString> Tokens = TokenizeConsoleExecCommand(Command);
		if (Tokens.Num() == 0)
		{
			return FString();
		}

		const FString& First = Tokens[0];
		const FString Second = Tokens.Num() > 1 ? Tokens[1] : FString();

		if (First == TEXT("QUIT") || First == TEXT("QUIT_EDITOR") || First == TEXT("EXIT"))
		{
			return TEXT("editor_lifecycle_exit");
		}
		if (First == TEXT("CRASH") || First == TEXT("FATAL") || (First == TEXT("DEBUG") && (Second == TEXT("CRASH") || Second == TEXT("ASSERT"))))
		{
			return TEXT("intentional_crash_or_assert");
		}
		if (First == TEXT("PY") || First == TEXT("PYTHON") || First == TEXT("PYTHONSCRIPT"))
		{
			return TEXT("arbitrary_python_execution");
		}
		if (First == TEXT("OPEN") || First == TEXT("TRAVEL") || First == TEXT("SERVERTRAVEL") || First == TEXT("DISCONNECT") || First == TEXT("RECONNECT") || First == TEXT("RESTARTLEVEL"))
		{
			return TEXT("map_or_session_transition");
		}
		if (First == TEXT("OBJ"))
		{
			if (Second == TEXT("LIST") || Second == TEXT("REFS") || Second == TEXT("HASH") || Second == TEXT("CLASSES"))
			{
				return FString();
			}
			return TEXT("object_command_requires_explicit_allow");
		}
		if (First == TEXT("MAP"))
		{
			if (Second == TEXT("CHECK") || Second == TEXT("CHECKDEP"))
			{
				return FString();
			}
			return TEXT("map_command_requires_explicit_allow");
		}
		if (First == TEXT("DELETE") || First == TEXT("DELETESELECTED") || First == TEXT("DESTROY"))
		{
			return TEXT("delete_or_destroy_command");
		}
		if (First == TEXT("ACTOR") && (Second == TEXT("DELETE") || Second == TEXT("DESTROY") || Second == TEXT("DUPLICATE") || Second == TEXT("REPLACE")))
		{
			return TEXT("actor_mutation_command_requires_explicit_allow");
		}
		if (First == TEXT("SELECT") && Second == TEXT("DELETE"))
		{
			return TEXT("selection_delete_command");
		}

		for (const FString& Token : Tokens)
		{
			if (Token == TEXT("SAVEPACKAGE") || Token == TEXT("IMPORT") || Token == TEXT("IMPORTADD"))
			{
				return TEXT("package_or_import_command_requires_explicit_allow");
			}
		}

		return FString();
	}

	static bool IsPythonConsoleCommand(const FString& Command)
	{
		const TArray<FString> Tokens = TokenizeConsoleExecCommand(Command);
		if (Tokens.Num() == 0)
		{
			return false;
		}

		const FString& First = Tokens[0];
		return First == TEXT("PY") || First == TEXT("PYTHON") || First == TEXT("PYTHONSCRIPT");
	}

	static void AppendConsoleExecCommandLines(const FString& Input, TArray<FString>& OutCommands)
	{
		const int32 Len = Input.Len();
		TArray<TCHAR> Buffer;
		Buffer.AddZeroed(Len + 1);

		const TCHAR* ParseCursor = *Input;
		while (FParse::Line(&ParseCursor, Buffer.GetData(), Buffer.Num()))
		{
			FString Line(Buffer.GetData());
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				OutCommands.Add(Line);
			}
		}
	}

	static bool BuildConsoleExecCommandList(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutCommands, FString& OutError)
	{
		FString Command;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("command"), Command))
		{
			AppendConsoleExecCommandLines(Command, OutCommands);
		}

		const TArray<TSharedPtr<FJsonValue>>* CommandValues = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("commands"), CommandValues) && CommandValues)
		{
			for (int32 Index = 0; Index < CommandValues->Num(); ++Index)
			{
				FString Item;
				if (!(*CommandValues)[Index].IsValid() || !(*CommandValues)[Index]->TryGetString(Item))
				{
					OutError = FString::Printf(TEXT("invalid_commands_item_%d"), Index);
					return false;
				}
				AppendConsoleExecCommandLines(Item, OutCommands);
			}
		}

		if (OutCommands.Num() == 0)
		{
			OutError = TEXT("missing_console_command");
			return false;
		}
		return true;
	}

	static FString NormalizeConsoleWorldContext(FString WorldContext)
	{
		WorldContext.TrimStartAndEndInline();
		if (WorldContext.IsEmpty())
		{
			return TEXT("auto");
		}
		WorldContext.ToLowerInline();
		if (WorldContext == TEXT("cmd"))
		{
			return TEXT("auto");
		}
		return WorldContext;
	}

	static bool ExecuteConsoleCommandLikeStatusBarCmd(const FString& Command, const FString& RequestedWorldContext, FOutputDevice& Output, FString& OutWorldContextUsed, FString& OutRoute, FString& OutFailureReason)
	{
		const FString WorldContext = NormalizeConsoleWorldContext(RequestedWorldContext);
		if (WorldContext != TEXT("auto") && WorldContext != TEXT("editor") && WorldContext != TEXT("pie"))
		{
			OutFailureReason = TEXT("invalid_world_context");
			return false;
		}

		bool bWasHandled = false;
		UWorld* World = nullptr;
		UWorld* OldWorld = nullptr;

		const bool bAllowPieWorld = WorldContext != TEXT("editor");
		if (bAllowPieWorld && GIsEditor && GEditor && GEditor->PlayWorld && !GIsPlayInEditorWorld)
		{
			World = GEditor->PlayWorld;
			OldWorld = SetPlayInEditorWorld(GEditor->PlayWorld);
			OutWorldContextUsed = TEXT("pie");
		}
		else if (WorldContext == TEXT("pie"))
		{
			OutFailureReason = TEXT("pie_world_not_available");
			return false;
		}

		ULocalPlayer* Player = (bAllowPieWorld && GEngine) ? GEngine->GetDebugLocalPlayer() : nullptr;
		if (Player)
		{
			UWorld* PlayerWorld = Player->GetWorld();
			if (!World)
			{
				World = PlayerWorld;
				OutWorldContextUsed = TEXT("player");
			}
			bWasHandled = Player->Exec(PlayerWorld, *Command, Output);
			if (bWasHandled)
			{
				OutRoute = TEXT("player_exec");
			}
		}

		if (!World && GEditor)
		{
			World = GEditor->GetEditorWorldContext().World();
			OutWorldContextUsed = TEXT("editor");
		}

		if (World)
		{
			if (!bWasHandled)
			{
				AGameModeBase* const GameMode = World->GetAuthGameMode();
				AGameStateBase* const GameState = World->GetGameState();
				if (GameMode && GameMode->ProcessConsoleExec(*Command, Output, nullptr))
				{
					bWasHandled = true;
					OutRoute = TEXT("game_mode_process_console_exec");
				}
				else if (GameState && GameState->ProcessConsoleExec(*Command, Output, nullptr))
				{
					bWasHandled = true;
					OutRoute = TEXT("game_state_process_console_exec");
				}
			}

			if (!bWasHandled && !Player)
			{
				if (GIsEditor && GEditor)
				{
					bWasHandled = GEditor->Exec(World, *Command, Output);
					OutRoute = bWasHandled ? TEXT("editor_exec") : TEXT("editor_exec_unhandled");
				}
				else if (GEngine)
				{
					bWasHandled = GEngine->Exec(World, *Command, Output);
					OutRoute = bWasHandled ? TEXT("engine_exec") : TEXT("engine_exec_unhandled");
				}
			}
		}
		else
		{
			OutFailureReason = TEXT("world_not_available");
		}

		if (OldWorld)
		{
			RestoreEditorWorld(OldWorld);
		}

		if (OutRoute.IsEmpty())
		{
			OutRoute = bWasHandled ? TEXT("handled") : TEXT("unhandled");
		}
		return bWasHandled;
	}

}
bool FUeAgentHttpServer::CmdEditorConsoleExec(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();

	TArray<FString> Commands;
	if (!UeAgentAssetOps::BuildConsoleExecCommandList(Ctx.Params, Commands, OutError))
	{
		return false;
	}

	bool bAllowHighRisk = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_high_risk"), bAllowHighRisk);

	bool bStopOnError = true;
	JsonTryGetBool(Ctx.Params, TEXT("stop_on_error"), bStopOnError);

	bool bFailOnUnhandled = true;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_unhandled"), bFailOnUnhandled);

	bool bAddToHistory = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_to_history"), bAddToHistory);

	FString WorldContext = TEXT("auto");
	JsonTryGetString(Ctx.Params, TEXT("world_context"), WorldContext);
	WorldContext = UeAgentAssetOps::NormalizeConsoleWorldContext(WorldContext);

	double MaxOutputCharsNumber = 65536.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_output_chars"), MaxOutputCharsNumber);
	const int32 MaxOutputChars = FMath::Clamp(FMath::RoundToInt(MaxOutputCharsNumber), 0, 1024 * 1024);

	double MaxOutputLinesNumber = 128.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_output_lines"), MaxOutputLinesNumber);
	const int32 MaxOutputLines = FMath::Clamp(FMath::RoundToInt(MaxOutputLinesNumber), 0, 4096);

	TArray<TSharedPtr<FJsonValue>> ResultValues;
	int32 ExecutedCount = 0;
	int32 HandledCount = 0;
	int32 BlockedCount = 0;
	int32 UnhandledCount = 0;
	FString FirstFailure;
	bool bStoppedEarly = false;

	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const FString& Command = Commands[Index];
		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetNumberField(TEXT("index"), Index);
		ResultObj->SetStringField(TEXT("command"), Command);

		const FString HighRiskReason = UeAgentAssetOps::ConsoleExecHighRiskReason(Command);
		const bool bHighRisk = !HighRiskReason.IsEmpty();
		ResultObj->SetBoolField(TEXT("high_risk"), bHighRisk);
		if (bHighRisk)
		{
			ResultObj->SetStringField(TEXT("high_risk_reason"), HighRiskReason);
		}

		if (bHighRisk && !bAllowHighRisk)
		{
			++BlockedCount;
			ResultObj->SetBoolField(TEXT("blocked"), true);
			ResultObj->SetBoolField(TEXT("handled"), false);
			ResultObj->SetStringField(TEXT("error"), TEXT("high_risk_command_requires_allow_high_risk"));
			ResultValues.Add(MakeShared<FJsonValueObject>(ResultObj));

			if (FirstFailure.IsEmpty())
			{
				FirstFailure = TEXT("editor_console_command_blocked_high_risk");
			}
			if (bStopOnError)
			{
				bStoppedEarly = Index + 1 < Commands.Num();
				break;
			}
			continue;
		}

		if (bAddToHistory)
		{
			IConsoleManager::Get().AddConsoleHistoryEntry(TEXT(""), *Command);
		}

		++ExecutedCount;
		UeAgentAssetOps::FConsoleExecCaptureOutputDevice Capture(MaxOutputChars);
		FString WorldContextUsed;
		FString Route;
		FString FailureReason;
		const bool bHandled = UeAgentAssetOps::ExecuteConsoleCommandLikeStatusBarCmd(Command, WorldContext, Capture, WorldContextUsed, Route, FailureReason);
		const bool bPythonConsoleCommand = UeAgentAssetOps::IsPythonConsoleCommand(Command);
		if (bPythonConsoleCommand)
		{
			bPythonConsoleCommandExecuted = true;
		}

		ResultObj->SetBoolField(TEXT("blocked"), false);
		ResultObj->SetBoolField(TEXT("handled"), bHandled);
		ResultObj->SetBoolField(TEXT("python_console_command"), bPythonConsoleCommand);
		ResultObj->SetStringField(TEXT("route"), Route);
		ResultObj->SetStringField(TEXT("world_context_used"), WorldContextUsed);
		ResultObj->SetStringField(TEXT("output"), Capture.GetCombinedOutput());
		ResultObj->SetBoolField(TEXT("output_truncated"), Capture.IsOutputTruncated());
		ResultObj->SetNumberField(TEXT("output_line_count"), Capture.GetLineCount());
		ResultObj->SetArrayField(TEXT("output_lines"), Capture.MakeLinesJson(MaxOutputLines));

		if (bHandled)
		{
			++HandledCount;
		}
		else
		{
			++UnhandledCount;
			if (!FailureReason.IsEmpty())
			{
				ResultObj->SetStringField(TEXT("failure_reason"), FailureReason);
			}
			if (FirstFailure.IsEmpty())
			{
				FirstFailure = FailureReason.IsEmpty() ? TEXT("editor_console_command_unhandled") : FailureReason;
			}
			if (bFailOnUnhandled && bStopOnError)
			{
				ResultValues.Add(MakeShared<FJsonValueObject>(ResultObj));
				bStoppedEarly = Index + 1 < Commands.Num();
				break;
			}
		}

		ResultValues.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	OutData->SetStringField(TEXT("executor"), TEXT("Cmd"));
	OutData->SetStringField(TEXT("world_context_requested"), WorldContext);
	OutData->SetBoolField(TEXT("allow_high_risk"), bAllowHighRisk);
	OutData->SetBoolField(TEXT("stop_on_error"), bStopOnError);
	OutData->SetBoolField(TEXT("fail_on_unhandled"), bFailOnUnhandled);
	OutData->SetBoolField(TEXT("add_to_history"), bAddToHistory);
	OutData->SetNumberField(TEXT("command_count"), Commands.Num());
	OutData->SetNumberField(TEXT("executed_count"), ExecutedCount);
	OutData->SetNumberField(TEXT("handled_count"), HandledCount);
	OutData->SetNumberField(TEXT("blocked_count"), BlockedCount);
	OutData->SetNumberField(TEXT("unhandled_count"), UnhandledCount);
	OutData->SetBoolField(TEXT("stopped_early"), bStoppedEarly);
	OutData->SetArrayField(TEXT("results"), ResultValues);

	if (BlockedCount > 0)
	{
		OutError = FirstFailure.IsEmpty() ? TEXT("editor_console_command_blocked_high_risk") : FirstFailure;
		return false;
	}
	if (bFailOnUnhandled && UnhandledCount > 0)
	{
		OutError = FirstFailure.IsEmpty() ? TEXT("editor_console_command_unhandled") : FirstFailure;
		return false;
	}

	return true;
}
