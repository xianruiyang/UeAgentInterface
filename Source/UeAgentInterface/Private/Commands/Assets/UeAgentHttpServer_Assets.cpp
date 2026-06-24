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
	FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

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

	bool DetachStaleObjectAtPath(const FString& ObjectPath, bool* bOutFoundStaleObject)
	{
		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = false;
		}
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Existing)
		{
			return false;
		}
		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = true;
		}
		if (IsValid(Existing) && Existing->HasAnyFlags(RF_Public | RF_Standalone))
		{
			return false;
		}

		if (!IsValid(Existing))
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			return false;
		}

		Existing->ClearFlags(RF_Public | RF_Standalone);
		if (Existing->IsRooted())
		{
			Existing->RemoveFromRoot();
		}
		const ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders;
		const FName TrashName = MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass(), Existing->GetFName());
		const bool bRenamed = Existing->Rename(*TrashName.ToString(), GetTransientPackage(), RenameFlags);
		Existing->MarkAsGarbage();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return bRenamed;
	}

	FString NormalizeReferenceInputToPackageName(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return FString();
		}

		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), FirstQuote) && Path.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			Path = Path.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
			Path.TrimStartAndEndInline();
		}

		if (Path.StartsWith(TEXT("\"")) && Path.EndsWith(TEXT("\"")) && Path.Len() >= 2)
		{
			Path = Path.Mid(1, Path.Len() - 2);
			Path.TrimStartAndEndInline();
		}

		return NormalizeAssetPath(Path);
	}

	static FString DependencyCategoryToString(UE::AssetRegistry::EDependencyCategory Category)
	{
		using UE::AssetRegistry::EDependencyCategory;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			return TEXT("package");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return TEXT("manage");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return TEXT("searchable_name");
		}
		return TEXT("none");
	}

	static TArray<TSharedPtr<FJsonValue>> DependencyPropertyNames(UE::AssetRegistry::EDependencyCategory Category, UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;

		TArray<TSharedPtr<FJsonValue>> Names;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			Names.Add(MakeShared<FJsonValueString>(EnumHasAnyFlags(Properties, EDependencyProperty::Hard) ? TEXT("hard") : TEXT("soft")));
			if (EnumHasAnyFlags(Properties, EDependencyProperty::Game))
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("game")));
			}
			else
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("editor_only")));
			}
			if (EnumHasAnyFlags(Properties, EDependencyProperty::Build))
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("build")));
			}
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			Names.Add(MakeShared<FJsonValueString>(EnumHasAnyFlags(Properties, EDependencyProperty::Direct) ? TEXT("direct") : TEXT("indirect")));
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			Names.Add(MakeShared<FJsonValueString>(TEXT("searchable_name")));
		}
		return Names;
	}

	static FString DependencyRelationKind(UE::AssetRegistry::EDependencyCategory Category, UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			return EnumHasAnyFlags(Properties, EDependencyProperty::Hard) ? TEXT("hard_package") : TEXT("soft_package");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return EnumHasAnyFlags(Properties, EDependencyProperty::Direct) ? TEXT("direct_manage") : TEXT("indirect_manage");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return TEXT("searchable_name");
		}
		return TEXT("unknown");
	}

	bool ShouldIncludeDependencyEdge(
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties,
		bool bIncludeHard,
		bool bIncludeSoft,
		bool bIncludeManage,
		bool bIncludeSearchableNames)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			const bool bHard = EnumHasAnyFlags(Properties, EDependencyProperty::Hard);
			return bHard ? bIncludeHard : bIncludeSoft;
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return bIncludeManage;
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return bIncludeSearchableNames;
		}
		return false;
	}

	static FString AssetIdentifierPackageName(const FAssetIdentifier& Identifier)
	{
		return Identifier.PackageName.IsNone() ? FString() : Identifier.PackageName.ToString();
	}

	FString AssetDataClassPathString(const FAssetData& AssetData)
	{
		return AssetData.AssetClassPath.IsValid() ? AssetData.AssetClassPath.ToString() : FString();
	}

	static TSharedPtr<FJsonObject> MakeAssetRegistryNodeJson(IAssetRegistry& AssetRegistry, const FAssetIdentifier& Identifier, const int32 Depth, const FString& Role)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		const FString IdentifierString = Identifier.ToString();
		const FString PackageName = AssetIdentifierPackageName(Identifier);
		Node->SetStringField(TEXT("id"), IdentifierString);
		Node->SetStringField(TEXT("identifier"), IdentifierString);
		Node->SetStringField(TEXT("package_name"), PackageName);
		Node->SetStringField(TEXT("primary_asset_id"), Identifier.GetPrimaryAssetId().ToString());
		Node->SetStringField(TEXT("object_name"), Identifier.ObjectName.ToString());
		Node->SetStringField(TEXT("value_name"), Identifier.ValueName.ToString());
		Node->SetBoolField(TEXT("is_package_identifier"), Identifier.IsPackage());
		Node->SetBoolField(TEXT("is_object_identifier"), Identifier.IsObject());
		Node->SetBoolField(TEXT("is_value_identifier"), Identifier.IsValue());
		Node->SetNumberField(TEXT("depth"), Depth);
		Node->SetStringField(TEXT("role"), Role);

		TArray<FAssetData> Assets;
		if (!PackageName.IsEmpty())
		{
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, false);
		}

		Node->SetBoolField(TEXT("asset_found"), Assets.Num() > 0);
		Node->SetNumberField(TEXT("asset_data_count"), Assets.Num());
		TArray<TSharedPtr<FJsonValue>> AssetDataJson;
		for (const FAssetData& AssetData : Assets)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			Item->SetStringField(TEXT("object_path"), AssetData.GetSoftObjectPath().ToString());
			Item->SetStringField(TEXT("asset_class"), AssetDataClassPathString(AssetData));
			Item->SetBoolField(TEXT("is_redirector"), AssetDataClassPathString(AssetData).EndsWith(TEXT("ObjectRedirector")));
			AssetDataJson.Add(MakeShared<FJsonValueObject>(Item));
		}
		Node->SetArrayField(TEXT("asset_data"), AssetDataJson);
		if (Assets.Num() > 0)
		{
			const FAssetData& PrimaryAssetData = Assets[0];
			Node->SetStringField(TEXT("asset_name"), PrimaryAssetData.AssetName.ToString());
			Node->SetStringField(TEXT("object_path"), PrimaryAssetData.GetSoftObjectPath().ToString());
			Node->SetStringField(TEXT("asset_class"), AssetDataClassPathString(PrimaryAssetData));
			Node->SetBoolField(TEXT("is_redirector"), AssetDataClassPathString(PrimaryAssetData).EndsWith(TEXT("ObjectRedirector")));
		}
		else
		{
			Node->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(PackageName));
			Node->SetStringField(TEXT("object_path"), PackageName.IsEmpty() ? FString() : ToObjectPath(PackageName));
			Node->SetStringField(TEXT("asset_class"), FString());
			Node->SetBoolField(TEXT("is_redirector"), false);
		}
		return Node;
	}

	TSharedPtr<FJsonObject> MakeAssetRegistryEdgeJson(
		const FAssetIdentifier& Source,
		const FAssetIdentifier& Target,
		const FString& QueryDirection,
		const int32 Depth,
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;

		TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetStringField(TEXT("source"), Source.ToString());
		Edge->SetStringField(TEXT("source_package"), AssetIdentifierPackageName(Source));
		Edge->SetStringField(TEXT("target"), Target.ToString());
		Edge->SetStringField(TEXT("target_package"), AssetIdentifierPackageName(Target));
		Edge->SetStringField(TEXT("query_direction"), QueryDirection);
		Edge->SetNumberField(TEXT("depth"), Depth);
		Edge->SetNumberField(TEXT("category_flags"), static_cast<int64>(Category));
		Edge->SetNumberField(TEXT("property_flags"), static_cast<int64>(Properties));
		Edge->SetStringField(TEXT("category"), DependencyCategoryToString(Category));
		Edge->SetStringField(TEXT("relation_kind"), DependencyRelationKind(Category, Properties));
		Edge->SetArrayField(TEXT("properties"), DependencyPropertyNames(Category, Properties));
		Edge->SetBoolField(TEXT("hard"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Hard));
		Edge->SetBoolField(TEXT("soft"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && !EnumHasAnyFlags(Properties, EDependencyProperty::Hard));
		Edge->SetBoolField(TEXT("game"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Game));
		Edge->SetBoolField(TEXT("build"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Build));
		Edge->SetBoolField(TEXT("direct_manage"), EnumHasAnyFlags(Category, EDependencyCategory::Manage) && EnumHasAnyFlags(Properties, EDependencyProperty::Direct));
		Edge->SetBoolField(TEXT("indirect_manage"), EnumHasAnyFlags(Category, EDependencyCategory::Manage) && !EnumHasAnyFlags(Properties, EDependencyProperty::Direct));
		return Edge;
	}

	static void AddOrUpdateGraphNode(
		IAssetRegistry& AssetRegistry,
		TMap<FString, TSharedPtr<FJsonObject>>& NodesById,
		const FAssetIdentifier& Identifier,
		const int32 Depth,
		const FString& Role)
	{
		const FString Id = Identifier.ToString();
		if (TSharedPtr<FJsonObject>* Existing = NodesById.Find(Id))
		{
			const int32 ExistingDepth = (*Existing)->HasTypedField<EJson::Number>(TEXT("depth")) ? static_cast<int32>((*Existing)->GetIntegerField(TEXT("depth"))) : Depth;
			if (Depth < ExistingDepth)
			{
				(*Existing)->SetNumberField(TEXT("depth"), Depth);
			}
			if (!Role.IsEmpty())
			{
				const FString ExistingRole = (*Existing)->GetStringField(TEXT("role"));
				if (!ExistingRole.Contains(Role))
				{
					(*Existing)->SetStringField(TEXT("role"), ExistingRole.IsEmpty() ? Role : ExistingRole + TEXT(",") + Role);
				}
			}
			return;
		}
		NodesById.Add(Id, MakeAssetRegistryNodeJson(AssetRegistry, Identifier, Depth, Role));
	}

	static TArray<TSharedPtr<FJsonValue>> SortedNodeValues(const TMap<FString, TSharedPtr<FJsonObject>>& NodesById)
	{
		TArray<FString> Keys;
		NodesById.GetKeys(Keys);
		Keys.Sort();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Key : Keys)
		{
			if (const TSharedPtr<FJsonObject>* Node = NodesById.Find(Key))
			{
				Values.Add(MakeShared<FJsonValueObject>(*Node));
			}
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> CollectPackagePathStringHits(
		const FString& PackageName,
		const int32 MaxHits,
		bool& bOutTruncated)
	{
		bOutTruncated = false;

		TArray<FString> PackageFiles;
		IFileManager::Get().FindFilesRecursive(PackageFiles, *FPaths::ProjectContentDir(), TEXT("*.uasset"), true, false);
		IFileManager::Get().FindFilesRecursive(PackageFiles, *FPaths::ProjectContentDir(), TEXT("*.umap"), true, false);
		PackageFiles.Sort();

		TArray<TSharedPtr<FJsonValue>> HitsJson;
		const FTCHARToUTF8 SearchBytes(*PackageName);
		const TArrayView<const uint8> SearchView(reinterpret_cast<const uint8*>(SearchBytes.Get()), SearchBytes.Length());
		if (SearchView.Num() <= 0)
		{
			return HitsJson;
		}

		for (const FString& Filename : PackageFiles)
		{
			if (HitsJson.Num() >= MaxHits)
			{
				bOutTruncated = true;
				break;
			}

			TArray<uint8> FileBytes;
			if (!FFileHelper::LoadFileToArray(FileBytes, *Filename) || FileBytes.Num() < SearchView.Num())
			{
				continue;
			}

			bool bFound = false;
			for (int32 Index = 0; Index <= FileBytes.Num() - SearchView.Num(); ++Index)
			{
				if (FMemory::Memcmp(FileBytes.GetData() + Index, SearchView.GetData(), SearchView.Num()) == 0)
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				continue;
			}

			FString HitPackageName;
			FPackageName::TryConvertFilenameToLongPackageName(Filename, HitPackageName);
			TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
			Hit->SetStringField(TEXT("package_name"), HitPackageName);
			Hit->SetStringField(TEXT("filename"), Filename);
			Hit->SetBoolField(TEXT("self_hit"), HitPackageName == PackageName);
			HitsJson.Add(MakeShared<FJsonValueObject>(Hit));
		}

		return HitsJson;
	}

	static UPackage* ResolvePackageByAssetPath(const FString& InPath)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(InPath);
		if (NormalizedAssetPath.IsEmpty())
		{
			return nullptr;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *NormalizedAssetPath))
		{
			return ExistingPackage;
		}

		if (UObject* AssetObj = LoadAssetObject(NormalizedAssetPath))
		{
			return AssetObj->GetOutermost();
		}

		return nullptr;
	}

	bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FString Item = Value->AsString();
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				OutValues.Add(Item);
			}
		}

		return true;
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}

		FString OutputPath = OutputPathInput;
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!OutputDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}

		OutOutputPath = OutputPath;
		return true;
	}

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();

		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}

		InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		OutResolvedPath = InputPath;
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}

			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject)
				{
					return false;
				}

				CurrentStruct = NestedObject->GetClass();
				CurrentContainer = NestedObject;
				continue;
			}

			return false;
		}

		return false;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property->GetCPPType();
		}
		if (Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
		}
		return true;
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static void GetDefaultAssetPropertyPaths(UObject* Asset, TArray<FString>& OutPropertyPaths, FString& OutPresetName)
	{
		OutPropertyPaths.Reset();
		OutPresetName.Reset();
		if (!Asset)
		{
			return;
		}

		if (Asset->IsA<UAnimSequence>())
		{
			OutPresetName = TEXT("anim_sequence_defaults");
			OutPropertyPaths = {
				TEXT("RateScale"),
				TEXT("bEnableRootMotion"),
				TEXT("bForceRootLock"),
				TEXT("RootMotionRootLock"),
				TEXT("Interpolation")
			};
			return;
		}

		if (Asset->IsA<UTexture>())
		{
			OutPresetName = TEXT("texture_defaults");
			OutPropertyPaths = {
				TEXT("SRGB"),
				TEXT("CompressionSettings"),
				TEXT("MipGenSettings"),
				TEXT("LODGroup"),
				TEXT("NeverStream"),
				TEXT("VirtualTextureStreaming"),
				TEXT("Filter")
			};
			return;
		}

		if (Asset->IsA<UStaticMesh>())
		{
			OutPresetName = TEXT("static_mesh_defaults");
			OutPropertyPaths = {
				TEXT("LightMapResolution"),
				TEXT("LightMapCoordinateIndex"),
				TEXT("LODGroup"),
				TEXT("bAllowCPUAccess")
			};
			return;
		}

		if (Asset->IsA<USkeletalMesh>())
		{
			OutPresetName = TEXT("skeletal_mesh_defaults");
			OutPropertyPaths = {
				TEXT("bEnablePerPolyCollision"),
				TEXT("LODSettings"),
				TEXT("DefaultAnimatingRig")
			};
			return;
		}
	}

	static bool ShouldSkipDirtyPackage(UPackage* Package)
	{
		if (!Package)
		{
			return true;
		}

		if (Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_CompiledIn))
		{
			return true;
		}

		const FString PackageName = Package->GetName();
		if (PackageName.IsEmpty() ||
			PackageName.StartsWith(TEXT("/Script/")) ||
			PackageName.StartsWith(TEXT("/Engine/Transient")) ||
			PackageName.StartsWith(TEXT("/Temp/")))
		{
			return true;
		}

		return !FPackageName::IsValidLongPackageName(PackageName, false);
	}

	static void AddPackageIfValid(TSet<UPackage*>& OutSet, UPackage* Package)
	{
		if (!Package || ShouldSkipDirtyPackage(Package))
		{
			return;
		}
		OutSet.Add(Package);
	}

	static int32 ClearDirtyFlagForPackages(const TSet<UPackage*>& Packages)
	{
		int32 ClearedCount = 0;
		for (UPackage* Package : Packages)
		{
			if (!Package || !Package->IsDirty())
			{
				continue;
			}

			Package->SetDirtyFlag(false);
			++ClearedCount;
		}
		return ClearedCount;
	}

	struct FDirtyResolutionOptions
	{
		bool bSaveCurrentLevel = false;
		bool bDiscardCurrentLevel = false;
		bool bSaveAllDirty = false;
		bool bDiscardAllDirty = false;
		bool bCloseAllAssetEditors = false;
		bool bOnlySaveDirty = true;
		TArray<FString> SaveResourcePaths;
		TArray<FString> DiscardResourcePaths;
	};

	struct FDirtyResolutionSummary
	{
		int32 SavedPackageCount = 0;
		int32 ExplicitDiscardedPackageCount = 0;
		int32 AutoDiscardedPackageCount = 0;
		TArray<FString> SaveResourceNotFound;
		TArray<FString> DiscardResourceNotFound;
		TArray<UPackage*> DirtyPackagesBefore;
		TArray<UPackage*> DirtyPackagesAfter;
	};

	static void CollectDirtyPackages(TArray<UPackage*>& OutPackages)
	{
		OutPackages.Reset();
		TSet<UPackage*> UniquePackages;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			UniquePackages.Add(Package);
		}
		for (UPackage* Package : UniquePackages)
		{
			OutPackages.Add(Package);
		}
		OutPackages.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});
	}

	static UObject* FindRepresentativeObjectInPackage(UPackage* Package)
	{
		if (!Package)
		{
			return nullptr;
		}

		const FString PackageName = Package->GetName();
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (!AssetName.IsEmpty())
		{
			if (UObject* NamedObject = StaticFindObjectFast(UObject::StaticClass(), Package, FName(*AssetName)))
			{
				return NamedObject;
			}
		}

		UObject* FirstTopLevelObject = nullptr;
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Obj = *It;
			if (!Obj || Obj == Package || Obj->HasAnyFlags(RF_Transient))
			{
				continue;
			}
			if (Obj->GetOutermost() != Package || Obj->GetOuter() != Package)
			{
				continue;
			}

			if (Obj->IsA<UWorld>())
			{
				return Obj;
			}
			if (!FirstTopLevelObject)
			{
				FirstTopLevelObject = Obj;
			}
		}

		return FirstTopLevelObject;
	}

	static bool IsPackageOpenInEditor(UPackage* Package, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		if (!Package || !AssetEditorSubsystem)
		{
			return false;
		}

		const TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
		for (UObject* Asset : OpenAssets)
		{
			if (Asset && Asset->GetOutermost() == Package)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeDirtyResourceJson(UPackage* Package, UWorld* EditorWorld, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!Package)
		{
			return Item;
		}

		UObject* RepresentativeObject = FindRepresentativeObjectInPackage(Package);
		const FString ResourcePath = Package->GetName();
		const bool bIsCurrentLevel = EditorWorld && EditorWorld->GetOutermost() == Package;
		const bool bIsLevel = bIsCurrentLevel || (RepresentativeObject && RepresentativeObject->IsA<UWorld>());

		Item->SetStringField(TEXT("resource_path"), ResourcePath);
		Item->SetStringField(TEXT("asset_path"), ResourcePath);
		Item->SetStringField(TEXT("package_name"), ResourcePath);
		Item->SetBoolField(TEXT("dirty"), Package->IsDirty());
		Item->SetBoolField(TEXT("decision_required"), Package->IsDirty());
		Item->SetBoolField(TEXT("is_current_level"), bIsCurrentLevel);
		Item->SetBoolField(TEXT("is_level"), bIsLevel);
		Item->SetBoolField(TEXT("is_open_in_editor"), IsPackageOpenInEditor(Package, AssetEditorSubsystem));
		Item->SetStringField(TEXT("kind"), bIsLevel ? TEXT("level") : TEXT("asset"));

		if (RepresentativeObject)
		{
			Item->SetStringField(TEXT("display_name"), RepresentativeObject->GetName());
			Item->SetStringField(TEXT("object_path"), RepresentativeObject->GetPathName());
			Item->SetStringField(TEXT("class"), RepresentativeObject->GetClass() ? RepresentativeObject->GetClass()->GetPathName() : TEXT(""));
		}
		else
		{
			Item->SetStringField(TEXT("display_name"), FPackageName::GetLongPackageAssetName(ResourcePath));
			Item->SetStringField(TEXT("object_path"), TEXT(""));
			Item->SetStringField(TEXT("class"), TEXT(""));
		}

		return Item;
	}

	static void MakeDirtyResourceArrayJson(
		const TArray<UPackage*>& Packages,
		UWorld* EditorWorld,
		UAssetEditorSubsystem* AssetEditorSubsystem,
		TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		OutItems.Reset();
		OutItems.Reserve(Packages.Num());
		for (UPackage* Package : Packages)
		{
			OutItems.Add(MakeShared<FJsonValueObject>(MakeDirtyResourceJson(Package, EditorWorld, AssetEditorSubsystem)));
		}
	}

	static void ParseResourcePathArrays(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutSavePaths, TArray<FString>& OutDiscardPaths)
	{
		OutSavePaths.Reset();
		OutDiscardPaths.Reset();

		if (!ReadStringArrayParam(Params, TEXT("save_resource_paths"), OutSavePaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("save_asset_paths"), OutSavePaths))
			{
				ReadStringArrayParam(Params, TEXT("save_resources"), OutSavePaths);
			}
		}
		if (!ReadStringArrayParam(Params, TEXT("discard_resource_paths"), OutDiscardPaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("discard_asset_paths"), OutDiscardPaths))
			{
				ReadStringArrayParam(Params, TEXT("discard_resources"), OutDiscardPaths);
			}
		}
	}

	static bool ResolveImportSourceFilenameWithExtensions(
		const FString& InFilename,
		const TSet<FString>& AllowedExtensions,
		const TCHAR* ExtensionError,
		FString& OutAbsoluteFilename,
		FString& OutError)
	{
		OutAbsoluteFilename = InFilename;
		OutAbsoluteFilename.TrimStartAndEndInline();
		if (OutAbsoluteFilename.IsEmpty())
		{
			OutError = TEXT("missing_source_filename");
			return false;
		}

		if (FPaths::IsRelative(OutAbsoluteFilename))
		{
			OutAbsoluteFilename = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutAbsoluteFilename);
		}
		else
		{
			OutAbsoluteFilename = FPaths::ConvertRelativePathToFull(OutAbsoluteFilename);
		}
		FPaths::NormalizeFilename(OutAbsoluteFilename);
		if (!FPaths::FileExists(OutAbsoluteFilename))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}

		const FString Extension = FPaths::GetExtension(OutAbsoluteFilename).ToLower();
		if (AllowedExtensions.Num() > 0 && !AllowedExtensions.Contains(Extension))
		{
			OutError = ExtensionError ? ExtensionError : TEXT("source_file_extension_not_allowed");
			return false;
		}

		return true;
	}

	static bool ResolveImportSourceFilename(const FString& InFilename, FString& OutAbsoluteFilename, FString& OutError)
	{
		const TSet<FString> FbxExtensions = { TEXT("fbx") };
		return ResolveImportSourceFilenameWithExtensions(
			InFilename,
			FbxExtensions,
			TEXT("source_file_is_not_fbx"),
			OutAbsoluteFilename,
			OutError);
	}

	template<typename TEnum>
	static FString EnumValueToString(TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::Printf(TEXT("%d"), static_cast<int32>(Value));
	}

	template<typename TEnum>
	static bool TryParseEnumValue(const FString& InText, const TCHAR* FieldName, TEnum& OutValue, FString& OutError)
	{
		FString Text = InText;
		Text.TrimStartAndEndInline();
		if (Text.IsEmpty())
		{
			OutError = FString::Printf(TEXT("empty_enum_value:%s"), FieldName ? FieldName : TEXT("enum"));
			return false;
		}

		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			OutError = FString::Printf(TEXT("enum_type_not_found:%s"), FieldName ? FieldName : TEXT("enum"));
			return false;
		}

		int64 Value = Enum->GetValueByNameString(Text, EGetByNameFlags::CheckAuthoredName);
		if (Value == INDEX_NONE)
		{
			Value = Enum->GetValueByNameString(Text, EGetByNameFlags::None);
		}
		if (Value == INDEX_NONE && !Text.Contains(TEXT("::")))
		{
			const FString QualifiedText = FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *Text);
			Value = Enum->GetValueByNameString(QualifiedText, EGetByNameFlags::CheckAuthoredName);
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(QualifiedText, EGetByNameFlags::None);
			}
		}
		if (Value == INDEX_NONE)
		{
			int64 NumericValue = 0;
			if (LexTryParseString(NumericValue, *Text) && Enum->IsValidEnumValue(NumericValue))
			{
				Value = NumericValue;
			}
		}
		if (Value == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("invalid_enum_value:%s:%s"), FieldName ? FieldName : TEXT("enum"), *Text);
			return false;
		}

		OutValue = static_cast<TEnum>(Value);
		return true;
	}

	static bool ResolveImportDestinationPath(const FString& InDestinationPath, FString& OutDestinationPath, FString& OutError)
	{
		OutDestinationPath = NormalizeAssetPath(InDestinationPath);
		if (!FPackageName::IsValidLongPackageName(OutDestinationPath))
		{
			OutError = TEXT("invalid_destination_path");
			return false;
		}
		return true;
	}

	static void GatherImportedObjects(const TArray<UObject*>& ImportedObjects, TArray<UObject*>& OutImportedObjects)
	{
		OutImportedObjects.Reset();

		TSet<UObject*> UniqueObjects;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject)
			{
				UniqueObjects.Add(ImportedObject);
			}
		}

		for (UObject* ImportedObject : UniqueObjects)
		{
			if (ImportedObject)
			{
				OutImportedObjects.Add(ImportedObject);
			}
		}

		OutImportedObjects.Sort([](const UObject& A, const UObject& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
	}

	static void FillImportedObjectsResponse(const TArray<UObject*>& ImportedObjectsIn, TSharedPtr<FJsonObject>& OutData)
	{
		TArray<UObject*> ImportedObjects;
		GatherImportedObjects(ImportedObjectsIn, ImportedObjects);

		TArray<TSharedPtr<FJsonValue>> ImportedObjectsJson;
		TArray<TSharedPtr<FJsonValue>> ImportedObjectPathsJson;
		TArray<TSharedPtr<FJsonValue>> SkeletalMeshPathsJson;
		TArray<TSharedPtr<FJsonValue>> SkeletonPathsJson;
		TArray<TSharedPtr<FJsonValue>> AnimSequencePathsJson;
		TArray<TSharedPtr<FJsonValue>> PhysicsAssetPathsJson;
		TSet<FString> SeenSkeletalMeshPaths;
		TSet<FString> SeenSkeletonPaths;
		TSet<FString> SeenAnimSequencePaths;
		TSet<FString> SeenPhysicsAssetPaths;

		UObject* PrimaryObject = nullptr;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (!ImportedObject)
			{
				continue;
			}

			if (!PrimaryObject)
			{
				PrimaryObject = ImportedObject;
			}

			const FString AssetPath = ImportedObject->GetOutermost() ? ImportedObject->GetOutermost()->GetName() : FString();
			const FString ObjectPath = ImportedObject->GetPathName();
			const FString ClassPath = ImportedObject->GetClass() ? ImportedObject->GetClass()->GetPathName() : FString();

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), AssetPath);
			Item->SetStringField(TEXT("object_path"), ObjectPath);
			Item->SetStringField(TEXT("name"), ImportedObject->GetName());
			Item->SetStringField(TEXT("class"), ClassPath);
			ImportedObjectsJson.Add(MakeShared<FJsonValueObject>(Item));
			ImportedObjectPathsJson.Add(MakeShared<FJsonValueString>(ObjectPath));

			if (ImportedObject->IsA<USkeletalMesh>())
			{
				if (!SeenSkeletalMeshPaths.Contains(AssetPath))
				{
					SeenSkeletalMeshPaths.Add(AssetPath);
					SkeletalMeshPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
				if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(ImportedObject))
				{
					if (const USkeleton* Skeleton = SkeletalMesh->GetSkeleton())
					{
						const FString SkeletonAssetPath = Skeleton->GetOutermost() ? Skeleton->GetOutermost()->GetName() : FString();
						if (!SkeletonAssetPath.IsEmpty() && !SeenSkeletonPaths.Contains(SkeletonAssetPath))
						{
							SeenSkeletonPaths.Add(SkeletonAssetPath);
							SkeletonPathsJson.Add(MakeShared<FJsonValueString>(SkeletonAssetPath));
						}
					}
					if (UPhysicsAsset* PhysicsAsset = SkeletalMesh->GetPhysicsAsset())
					{
						const FString PhysicsAssetPath = PhysicsAsset->GetOutermost() ? PhysicsAsset->GetOutermost()->GetName() : FString();
						if (!PhysicsAssetPath.IsEmpty() && !SeenPhysicsAssetPaths.Contains(PhysicsAssetPath))
						{
							SeenPhysicsAssetPaths.Add(PhysicsAssetPath);
							PhysicsAssetPathsJson.Add(MakeShared<FJsonValueString>(PhysicsAssetPath));
						}
					}
				}
			}
			else if (ImportedObject->IsA<USkeleton>())
			{
				if (!SeenSkeletonPaths.Contains(AssetPath))
				{
					SeenSkeletonPaths.Add(AssetPath);
					SkeletonPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
			else if (ImportedObject->IsA<UAnimSequence>())
			{
				if (!SeenAnimSequencePaths.Contains(AssetPath))
				{
					SeenAnimSequencePaths.Add(AssetPath);
					AnimSequencePathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
			else if (ImportedObject->IsA<UPhysicsAsset>())
			{
				if (!SeenPhysicsAssetPaths.Contains(AssetPath))
				{
					SeenPhysicsAssetPaths.Add(AssetPath);
					PhysicsAssetPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
		}

		OutData->SetNumberField(TEXT("imported_object_count"), ImportedObjectsJson.Num());
		OutData->SetArrayField(TEXT("imported_objects"), ImportedObjectsJson);
		OutData->SetArrayField(TEXT("imported_object_paths"), ImportedObjectPathsJson);
		OutData->SetArrayField(TEXT("skeletal_mesh_asset_paths"), SkeletalMeshPathsJson);
		OutData->SetArrayField(TEXT("skeleton_asset_paths"), SkeletonPathsJson);
		OutData->SetArrayField(TEXT("anim_sequence_asset_paths"), AnimSequencePathsJson);
		OutData->SetArrayField(TEXT("physics_asset_paths"), PhysicsAssetPathsJson);

		if (PrimaryObject)
		{
			OutData->SetStringField(TEXT("primary_asset_path"), PrimaryObject->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("primary_object_path"), PrimaryObject->GetPathName());
			OutData->SetStringField(TEXT("primary_class"), PrimaryObject->GetClass() ? PrimaryObject->GetClass()->GetPathName() : TEXT(""));
		}
	}

	static bool ResolveDirtyResources(
		const FDirtyResolutionOptions& Options,
		UWorld* EditorWorld,
		FDirtyResolutionSummary& OutSummary,
		FString& OutError)
	{
		OutSummary = FDirtyResolutionSummary();

		if (Options.bSaveCurrentLevel && Options.bDiscardCurrentLevel)
		{
			OutError = TEXT("save_and_discard_current_level_conflict");
			return false;
		}

		TSet<UPackage*> DirtyPackagesBeforeSet;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			DirtyPackagesBeforeSet.Add(Package);
		}
		for (UPackage* Package : DirtyPackagesBeforeSet)
		{
			OutSummary.DirtyPackagesBefore.Add(Package);
		}
		OutSummary.DirtyPackagesBefore.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});

		TSet<UPackage*> SavePackagesSet;
		TSet<UPackage*> DiscardPackagesSet;

		for (const FString& SavePath : Options.SaveResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(SavePath);
			if (!Package)
			{
				OutSummary.SaveResourceNotFound.Add(SavePath);
				continue;
			}
			AddPackageIfValid(SavePackagesSet, Package);
		}

		for (const FString& DiscardPath : Options.DiscardResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(DiscardPath);
			if (!Package)
			{
				OutSummary.DiscardResourceNotFound.Add(DiscardPath);
				continue;
			}
			AddPackageIfValid(DiscardPackagesSet, Package);
		}

		if (EditorWorld && (Options.bSaveCurrentLevel || Options.bDiscardCurrentLevel))
		{
			if (UPackage* WorldPackage = EditorWorld->GetOutermost())
			{
				if (Options.bSaveCurrentLevel)
				{
					SavePackagesSet.Add(WorldPackage);
				}
				if (Options.bDiscardCurrentLevel)
				{
					DiscardPackagesSet.Add(WorldPackage);
				}
			}
		}

		for (UPackage* DiscardPackage : DiscardPackagesSet)
		{
			SavePackagesSet.Remove(DiscardPackage);
		}

		if (Options.bSaveAllDirty)
		{
			for (UPackage* DirtyPackage : DirtyPackagesBeforeSet)
			{
				if (!DiscardPackagesSet.Contains(DirtyPackage))
				{
					SavePackagesSet.Add(DirtyPackage);
				}
			}
		}

		TArray<UPackage*> PackagesToSave;
		for (UPackage* Package : SavePackagesSet)
		{
			if (!Package)
			{
				continue;
			}
			if (Options.bOnlySaveDirty && !Package->IsDirty())
			{
				continue;
			}
			PackagesToSave.Add(Package);
		}

		if (PackagesToSave.Num() > 0)
		{
			if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
			{
				OutError = TEXT("save_packages_failed");
				return false;
			}
			OutSummary.SavedPackageCount = PackagesToSave.Num();
		}

		OutSummary.ExplicitDiscardedPackageCount = ClearDirtyFlagForPackages(DiscardPackagesSet);

		if (Options.bDiscardAllDirty)
		{
			TSet<UPackage*> RemainingDirty;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
				{
					continue;
				}
				if (SavePackagesSet.Contains(Package))
				{
					continue;
				}
				RemainingDirty.Add(Package);
			}
			OutSummary.AutoDiscardedPackageCount = ClearDirtyFlagForPackages(RemainingDirty);
		}

		if (Options.bCloseAllAssetEditors && GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditorSubsystem->CloseAllAssetEditors();
			}
		}

		CollectDirtyPackages(OutSummary.DirtyPackagesAfter);
		return true;
	}
}

bool FUeAgentHttpServer::CmdAssetMove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPath) || SourceAssetPath.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), SourceAssetPath);
	}
	if (SourceAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentAssetOps::LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}

	const FString SourcePackageName = UeAgentAssetOps::NormalizeAssetPath(SourceAsset->GetOutermost()->GetName());
	const FString SourceAssetName = SourceAsset->GetName();

	FString DestinationAssetPath;
	JsonTryGetString(Ctx.Params, TEXT("destination_asset_path"), DestinationAssetPath);
	DestinationAssetPath.TrimStartAndEndInline();

	FString DestinationFolder;
	JsonTryGetString(Ctx.Params, TEXT("destination_folder"), DestinationFolder);
	DestinationFolder.TrimStartAndEndInline();

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	if (DestinationAssetPath.IsEmpty())
	{
		if (DestinationFolder.IsEmpty())
		{
			OutError = TEXT("missing_destination_asset_path_or_folder");
			return false;
		}
		FString NormalizedFolder = UeAgentAssetOps::NormalizeAssetPath(DestinationFolder);
		if (NormalizedFolder.EndsWith(TEXT("/")))
		{
			NormalizedFolder.LeftChopInline(1);
		}
		DestinationAssetPath = NormalizedFolder / (DestinationName.IsEmpty() ? SourceAssetName : DestinationName);
	}

	const FString NormalizedDestinationAssetPath = UeAgentAssetOps::NormalizeAssetPath(DestinationAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	const FString DestinationAssetName = FPackageName::GetLongPackageAssetName(NormalizedDestinationAssetPath);
	const FString DestinationPackagePath = FPackageName::GetLongPackagePath(NormalizedDestinationAssetPath);
	if (DestinationAssetName.IsEmpty() || DestinationPackagePath.IsEmpty())
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	if (SourcePackageName.Equals(NormalizedDestinationAssetPath, ESearchCase::IgnoreCase))
	{
		OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
		OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
		OutData->SetStringField(TEXT("object_path"), SourceAsset->GetPathName());
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetBoolField(TEXT("saved"), false);
		return true;
	}

	bool bOverwriteExisting = false;
	JsonTryGetBool(Ctx.Params, TEXT("overwrite_existing"), bOverwriteExisting);
	if (!bOverwriteExisting && UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("destination_asset_already_exists");
		return false;
	}

	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
		OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
		OutData->SetStringField(TEXT("destination_folder"), DestinationPackagePath);
		OutData->SetStringField(TEXT("destination_name"), DestinationAssetName);
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetBoolField(TEXT("dry_run"), true);
		return true;
	}

	if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
	{
		if (AssetEditorSubsystem->FindEditorForAsset(SourceAsset, false))
		{
			AssetEditorSubsystem->CloseAllEditorsForAsset(SourceAsset);
		}
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(SourceAsset, DestinationPackagePath, DestinationAssetName);
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		if (!AssetToolsModule.Get().RenameAssets(RenameData))
		{
			OutError = TEXT("rename_asset_failed");
			return false;
		}
	}

	UObject* MovedAsset = UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath);
	if (!MovedAsset)
	{
		OutError = TEXT("moved_asset_not_found_after_rename");
		return false;
	}

	bool bSaveAfterMove = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_move"), bSaveAfterMove);
	if (bSaveAfterMove && !UeAgentAssetOps::SaveAssetPackage(MovedAsset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
	OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
	OutData->SetStringField(TEXT("object_path"), MovedAsset->GetPathName());
	OutData->SetStringField(TEXT("destination_folder"), DestinationPackagePath);
	OutData->SetStringField(TEXT("destination_name"), DestinationAssetName);
	OutData->SetStringField(TEXT("class"), MovedAsset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterMove);
	return true;
}

bool FUeAgentHttpServer::CmdAssetFixupRedirectors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<FString> RequestedAssetPaths;
	auto AddStringField = [&Ctx, &RequestedAssetPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				RequestedAssetPaths.Add(Value);
			}
		}
	};
	auto AddStringArrayField = [&Ctx, &RequestedAssetPaths](const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(Key, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					RequestedAssetPaths.Add(Text);
				}
			}
		}
	};
	AddStringField(TEXT("asset_path"));
	AddStringField(TEXT("redirector_path"));
	AddStringArrayField(TEXT("asset_paths"));
	AddStringArrayField(TEXT("redirector_paths"));

	TArray<FString> RequestedFolderPaths;
	auto AddFolderField = [&Ctx, &RequestedFolderPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				RequestedFolderPaths.Add(Value);
			}
		}
	};
	auto AddFolderArrayField = [&Ctx, &RequestedFolderPaths](const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(Key, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					RequestedFolderPaths.Add(Text);
				}
			}
		}
	};
	AddFolderField(TEXT("folder_path"));
	AddFolderField(TEXT("redirector_folder"));
	AddFolderArrayField(TEXT("folder_paths"));
	AddFolderArrayField(TEXT("redirector_folders"));

	if (RequestedAssetPaths.IsEmpty() && RequestedFolderPaths.IsEmpty())
	{
		OutError = TEXT("missing_asset_path_or_folder_path");
		return false;
	}

	bool bRecursive = true;
	JsonTryGetBool(Ctx.Params, TEXT("recursive"), bRecursive);

	TMap<FString, UObjectRedirector*> RedirectorByPackage;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	auto AddSkipped = [&Skipped](const FString& Path, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("reason"), Reason);
		Skipped.Add(MakeShared<FJsonValueObject>(Entry));
	};

	for (const FString& RequestedPath : RequestedAssetPaths)
	{
		const FString PackageName = UeAgentAssetOps::NormalizeAssetPath(RequestedPath);
		UObject* LoadedObject = UeAgentAssetOps::LoadAssetObject(PackageName);
		UObjectRedirector* Redirector = Cast<UObjectRedirector>(LoadedObject);
		if (!Redirector)
		{
			AddSkipped(PackageName, LoadedObject ? TEXT("not_redirector") : TEXT("asset_not_found"));
			continue;
		}
		RedirectorByPackage.Add(PackageName, Redirector);
	}

	if (!RequestedFolderPaths.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		for (FString FolderPath : RequestedFolderPaths)
		{
			FolderPath.TrimStartAndEndInline();
			FolderPath.RemoveFromEnd(TEXT("/"));
			if (FolderPath.IsEmpty())
			{
				continue;
			}

			FARFilter Filter;
			Filter.PackagePaths.Add(*FolderPath);
			Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = bRecursive;

			TArray<FAssetData> RedirectorAssets;
			AssetRegistry.GetAssets(Filter, RedirectorAssets);
			for (const FAssetData& RedirectorAsset : RedirectorAssets)
			{
				UObjectRedirector* Redirector = Cast<UObjectRedirector>(RedirectorAsset.GetAsset());
				if (Redirector)
				{
					RedirectorByPackage.Add(RedirectorAsset.PackageName.ToString(), Redirector);
				}
			}
		}
	}

	TArray<UObjectRedirector*> Redirectors;
	RedirectorByPackage.GenerateValueArray(Redirectors);

	TArray<TSharedPtr<FJsonValue>> RedirectorReports;
	for (const TPair<FString, UObjectRedirector*>& Pair : RedirectorByPackage)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Pair.Key);
		Entry->SetStringField(TEXT("object_path"), Pair.Value ? Pair.Value->GetPathName() : FString());
		if (Pair.Value && Pair.Value->DestinationObject)
		{
			Entry->SetStringField(TEXT("destination_object_path"), Pair.Value->DestinationObject->GetPathName());
			Entry->SetStringField(TEXT("destination_asset_path"), UeAgentAssetOps::NormalizeAssetPath(Pair.Value->DestinationObject->GetOutermost()->GetName()));
		}
		RedirectorReports.Add(MakeShared<FJsonValueObject>(Entry));
	}

	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<TSharedPtr<FJsonValue>> Blocked;
	TArray<UObject*> RedirectorsToDelete;
	for (UObjectRedirector* Redirector : Redirectors)
	{
		if (!Redirector)
		{
			continue;
		}

		const FString PackageName = UeAgentAssetOps::NormalizeAssetPath(Redirector->GetOutermost()->GetName());
		TArray<FName> ReferencerNames;
		AssetRegistry.GetReferencers(*PackageName, ReferencerNames);
		if (!ReferencerNames.IsEmpty())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), PackageName);
			TArray<TSharedPtr<FJsonValue>> ReferencerValues;
			for (const FName& ReferencerName : ReferencerNames)
			{
				ReferencerValues.Add(MakeShared<FJsonValueString>(ReferencerName.ToString()));
			}
			Entry->SetArrayField(TEXT("referencers"), ReferencerValues);
			Entry->SetNumberField(TEXT("referencer_count"), ReferencerNames.Num());
			Entry->SetStringField(TEXT("reason"), TEXT("redirector_still_referenced"));
			Blocked.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		RedirectorsToDelete.Add(Redirector);
	}

	bool bFailOnBlocked = true;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_blocked"), bFailOnBlocked);
	if (!Blocked.IsEmpty() && bFailOnBlocked)
	{
		OutData->SetNumberField(TEXT("redirector_count"), Redirectors.Num());
		OutData->SetNumberField(TEXT("deletable_redirector_count"), RedirectorsToDelete.Num());
		OutData->SetNumberField(TEXT("blocked_redirector_count"), Blocked.Num());
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetArrayField(TEXT("redirectors"), RedirectorReports);
		OutData->SetArrayField(TEXT("blocked"), Blocked);
		OutData->SetArrayField(TEXT("skipped"), Skipped);
		OutError = TEXT("redirector_still_referenced");
		return false;
	}

	int32 DeletedCount = 0;
	if (!bDryRun && !RedirectorsToDelete.IsEmpty())
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		DeletedCount = ObjectTools::DeleteObjectsUnchecked(RedirectorsToDelete);
	}

	int32 RemainingPackageCount = 0;
	TArray<TSharedPtr<FJsonValue>> RemainingPackages;
	for (const TPair<FString, UObjectRedirector*>& Pair : RedirectorByPackage)
	{
		FString ExistingPackageFilename;
		if (FPackageName::DoesPackageExist(Pair.Key, &ExistingPackageFilename))
		{
			++RemainingPackageCount;
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), Pair.Key);
			Entry->SetStringField(TEXT("filename"), ExistingPackageFilename);
			RemainingPackages.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	OutData->SetNumberField(TEXT("redirector_count"), Redirectors.Num());
	OutData->SetNumberField(TEXT("deletable_redirector_count"), RedirectorsToDelete.Num());
	OutData->SetNumberField(TEXT("blocked_redirector_count"), Blocked.Num());
	OutData->SetNumberField(TEXT("deleted_redirector_count"), bDryRun ? 0 : DeletedCount);
	OutData->SetNumberField(TEXT("remaining_package_count"), RemainingPackageCount);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetArrayField(TEXT("redirectors"), RedirectorReports);
	OutData->SetArrayField(TEXT("blocked"), Blocked);
	OutData->SetArrayField(TEXT("skipped"), Skipped);
	OutData->SetArrayField(TEXT("remaining_packages"), RemainingPackages);
	return true;
}

bool FUeAgentHttpServer::CmdAssetReferenceGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAssetOps;
	using UE::AssetRegistry::EDependencyCategory;

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("package_name"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_path"), AssetPathInput);
		}
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = NormalizeReferenceInputToPackageName(AssetPathInput);
	if (PackageName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}

	FString Direction = TEXT("both");
	JsonTryGetString(Ctx.Params, TEXT("direction"), Direction);
	Direction.TrimStartAndEndInline();
	Direction = Direction.ToLower();
	if (Direction.IsEmpty())
	{
		Direction = TEXT("both");
	}
	const bool bQueryDependencies = Direction == TEXT("both") || Direction == TEXT("dependencies") || Direction == TEXT("dependency") || Direction == TEXT("downstream");
	const bool bQueryReferencers = Direction == TEXT("both") || Direction == TEXT("referencers") || Direction == TEXT("referencer") || Direction == TEXT("upstream");
	if (!bQueryDependencies && !bQueryReferencers)
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	bool bRecursive = false;
	JsonTryGetBool(Ctx.Params, TEXT("recursive"), bRecursive);

	double MaxDepthNumber = bRecursive ? 3.0 : 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_depth"), MaxDepthNumber);
	const int32 MaxDepth = FMath::Clamp(static_cast<int32>(MaxDepthNumber), 1, 16);

	double MaxNodesNumber = 1024.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_nodes"), MaxNodesNumber);
	const int32 MaxNodes = FMath::Clamp(static_cast<int32>(MaxNodesNumber), 1, 20000);

	bool bIncludeHard = true;
	bool bIncludeSoft = true;
	bool bIncludeManage = true;
	bool bIncludeSearchableNames = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_hard"), bIncludeHard);
	JsonTryGetBool(Ctx.Params, TEXT("include_soft"), bIncludeSoft);
	JsonTryGetBool(Ctx.Params, TEXT("include_manage"), bIncludeManage);
	JsonTryGetBool(Ctx.Params, TEXT("include_searchable_names"), bIncludeSearchableNames);

	bool bForceScan = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_scan"), bForceScan);

	bool bAllowMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_missing"), bAllowMissing);

	bool bIncludePathStringHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_path_string_hits"), bIncludePathStringHits);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (bForceScan)
	{
		AssetRegistry.SearchAllAssets(true);
		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()) && FPaths::FileExists(PackageFilename))
		{
			AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
		}
		else if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetMapPackageExtension()) && FPaths::FileExists(PackageFilename))
		{
			AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
		}
		AssetRegistry.WaitForCompletion();
	}

	const FAssetIdentifier RootIdentifier{ FName(*PackageName) };
	TArray<FAssetData> RootAssetData;
	AssetRegistry.GetAssetsByPackageName(FName(*PackageName), RootAssetData, false);
	if (RootAssetData.Num() == 0 && !bAllowMissing)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	EDependencyCategory CategoryMask = EDependencyCategory::None;
	if (bIncludeHard || bIncludeSoft)
	{
		CategoryMask |= EDependencyCategory::Package;
	}
	if (bIncludeManage)
	{
		CategoryMask |= EDependencyCategory::Manage;
	}
	if (bIncludeSearchableNames)
	{
		CategoryMask |= EDependencyCategory::SearchableName;
	}

	struct FQueuedReferenceNode
	{
		FAssetIdentifier Identifier;
		int32 Depth = 0;
		FString QueryDirection;
	};

	TMap<FString, TSharedPtr<FJsonObject>> NodesById;
	TArray<TSharedPtr<FJsonValue>> EdgesJson;
	TArray<TSharedPtr<FJsonValue>> DirectDependenciesJson;
	TArray<TSharedPtr<FJsonValue>> DirectReferencersJson;
	TSet<FString> VisitedDependencyNodes;
	TSet<FString> VisitedReferencerNodes;
	TSet<FString> EdgeKeys;
	TArray<FQueuedReferenceNode> Queue;
	bool bTruncated = false;

	AddOrUpdateGraphNode(AssetRegistry, NodesById, RootIdentifier, 0, TEXT("root"));
	if (bQueryDependencies)
	{
		VisitedDependencyNodes.Add(RootIdentifier.ToString());
		Queue.Add({ RootIdentifier, 0, TEXT("dependencies") });
	}
	if (bQueryReferencers)
	{
		VisitedReferencerNodes.Add(RootIdentifier.ToString());
		Queue.Add({ RootIdentifier, 0, TEXT("referencers") });
	}

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		if (NodesById.Num() >= MaxNodes)
		{
			bTruncated = true;
			break;
		}

		const FQueuedReferenceNode Current = Queue[QueueIndex];
		if (Current.Depth >= MaxDepth)
		{
			continue;
		}

		TArray<FAssetDependency> FoundEdges;
		const bool bIsDependencyQuery = Current.QueryDirection == TEXT("dependencies");
		const bool bQueryOk = bIsDependencyQuery
			? AssetRegistry.GetDependencies(Current.Identifier, FoundEdges, CategoryMask)
			: AssetRegistry.GetReferencers(Current.Identifier, FoundEdges, CategoryMask);
		if (!bQueryOk)
		{
			continue;
		}

		for (const FAssetDependency& FoundEdge : FoundEdges)
		{
			if (!ShouldIncludeDependencyEdge(FoundEdge.Category, FoundEdge.Properties, bIncludeHard, bIncludeSoft, bIncludeManage, bIncludeSearchableNames))
			{
				continue;
			}

			const FAssetIdentifier SourceIdentifier = bIsDependencyQuery ? Current.Identifier : FoundEdge.AssetId;
			const FAssetIdentifier TargetIdentifier = bIsDependencyQuery ? FoundEdge.AssetId : Current.Identifier;
			const FAssetIdentifier OtherIdentifier = bIsDependencyQuery ? TargetIdentifier : SourceIdentifier;
			const int32 EdgeDepth = Current.Depth + 1;

			AddOrUpdateGraphNode(AssetRegistry, NodesById, SourceIdentifier, bIsDependencyQuery ? Current.Depth : EdgeDepth, bIsDependencyQuery ? TEXT("source") : TEXT("referencer"));
			AddOrUpdateGraphNode(AssetRegistry, NodesById, TargetIdentifier, bIsDependencyQuery ? EdgeDepth : Current.Depth, bIsDependencyQuery ? TEXT("dependency") : TEXT("target"));

			const FString EdgeKey = SourceIdentifier.ToString()
				+ TEXT("->") + TargetIdentifier.ToString()
				+ TEXT("|") + LexToString(static_cast<int64>(FoundEdge.Category))
				+ TEXT("|") + LexToString(static_cast<int64>(FoundEdge.Properties));
			if (!EdgeKeys.Contains(EdgeKey))
			{
				EdgeKeys.Add(EdgeKey);
				TSharedPtr<FJsonObject> EdgeJson = MakeAssetRegistryEdgeJson(SourceIdentifier, TargetIdentifier, Current.QueryDirection, EdgeDepth, FoundEdge.Category, FoundEdge.Properties);
				EdgesJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				if (Current.Identifier == RootIdentifier && bIsDependencyQuery)
				{
					DirectDependenciesJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				}
				if (Current.Identifier == RootIdentifier && !bIsDependencyQuery)
				{
					DirectReferencersJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				}
			}

			if (bRecursive && EdgeDepth < MaxDepth && NodesById.Num() < MaxNodes)
			{
				TSet<FString>& VisitedSet = bIsDependencyQuery ? VisitedDependencyNodes : VisitedReferencerNodes;
				const FString OtherKey = OtherIdentifier.ToString();
				if (!VisitedSet.Contains(OtherKey))
				{
					VisitedSet.Add(OtherKey);
					Queue.Add({ OtherIdentifier, EdgeDepth, Current.QueryDirection });
				}
			}
			else if (NodesById.Num() >= MaxNodes)
			{
				bTruncated = true;
				break;
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("package_name"), PackageName);
	OutData->SetStringField(TEXT("input_asset_path"), AssetPathInput);
	OutData->SetStringField(TEXT("direction"), Direction);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetNumberField(TEXT("max_depth"), MaxDepth);
	OutData->SetNumberField(TEXT("max_nodes"), MaxNodes);
	OutData->SetBoolField(TEXT("asset_found"), RootAssetData.Num() > 0);
	OutData->SetNumberField(TEXT("root_asset_data_count"), RootAssetData.Num());
	OutData->SetBoolField(TEXT("truncated"), bTruncated);
	OutData->SetNumberField(TEXT("dependency_count"), DirectDependenciesJson.Num());
	OutData->SetNumberField(TEXT("referencer_count"), DirectReferencersJson.Num());
	OutData->SetNumberField(TEXT("edge_count"), EdgesJson.Num());
	OutData->SetNumberField(TEXT("node_count"), NodesById.Num());
	OutData->SetArrayField(TEXT("dependencies"), DirectDependenciesJson);
	OutData->SetArrayField(TEXT("referencers"), DirectReferencersJson);
	OutData->SetArrayField(TEXT("edges"), EdgesJson);
	OutData->SetArrayField(TEXT("nodes"), SortedNodeValues(NodesById));

	TSharedPtr<FJsonObject> OptionsJson = MakeShared<FJsonObject>();
	OptionsJson->SetBoolField(TEXT("include_hard"), bIncludeHard);
	OptionsJson->SetBoolField(TEXT("include_soft"), bIncludeSoft);
	OptionsJson->SetBoolField(TEXT("include_manage"), bIncludeManage);
	OptionsJson->SetBoolField(TEXT("include_searchable_names"), bIncludeSearchableNames);
	OptionsJson->SetBoolField(TEXT("force_scan"), bForceScan);
	OptionsJson->SetBoolField(TEXT("allow_missing"), bAllowMissing);
	OptionsJson->SetBoolField(TEXT("include_path_string_hits"), bIncludePathStringHits);
	OutData->SetObjectField(TEXT("options"), OptionsJson);

	if (bIncludePathStringHits)
	{
		double MaxHitsNumber = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("max_path_string_hits"), MaxHitsNumber);
		const int32 MaxHits = FMath::Clamp(static_cast<int32>(MaxHitsNumber), 1, 5000);

		bool bPathStringHitsTruncated = false;
		TArray<TSharedPtr<FJsonValue>> HitsJson = CollectPackagePathStringHits(PackageName, MaxHits, bPathStringHitsTruncated);
		bTruncated |= bPathStringHitsTruncated;

		OutData->SetArrayField(TEXT("path_string_hits"), HitsJson);
		OutData->SetNumberField(TEXT("path_string_hit_count"), HitsJson.Num());
		OutData->SetBoolField(TEXT("truncated"), bTruncated);
	}

	return true;
}

bool FUeAgentHttpServer::CmdAssetImportTexture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	const TSet<FString> TextureExtensions = {
		TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"),
		TEXT("exr"), TEXT("hdr"), TEXT("dds"), TEXT("psd"), TEXT("tif"), TEXT("tiff")
	};

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilenameWithExtensions(
		SourceFilenameInput,
		TextureExtensions,
		TEXT("source_file_is_not_supported_texture_extension"),
		SourceFilename,
		OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();
	if (!DestinationName.IsEmpty())
	{
		const FString DestinationAssetPath = DestinationPath / DestinationName;
		if (!FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			OutError = TEXT("invalid_destination_name");
			return false;
		}
	}

	bool bReplaceExisting = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("replace_existing")) && !JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting))
	{
		OutError = TEXT("invalid_replace_existing");
		return false;
	}

	bool bReplaceExistingSettings = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("replace_existing_settings")) && !JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings))
	{
		OutError = TEXT("invalid_replace_existing_settings");
		return false;
	}

	bool bSaveAfterImport = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("save_after_import")) && !JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport))
	{
		OutError = TEXT("invalid_save_after_import");
		return false;
	}

	bool bOpenEditor = false;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("open_editor")) && !JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor))
	{
		OutError = TEXT("invalid_open_editor");
		return false;
	}

	UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!TextureFactory || !ImportData)
	{
		OutError = TEXT("create_texture_import_task_failed");
		return false;
	}

	TextureFactory->AddToRoot();
	ImportData->AddToRoot();

	bool bHasSRGB = false;
	bool bSRGB = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("srgb")))
	{
		bHasSRGB = true;
		if (!JsonTryGetBool(Ctx.Params, TEXT("srgb"), bSRGB))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			OutError = TEXT("invalid_srgb");
			return false;
		}
		TextureFactory->ColorSpaceMode = bSRGB ? ETextureSourceColorSpace::SRGB : ETextureSourceColorSpace::Linear;
	}

	auto ReadOptionalBool = [&](const TCHAR* FieldName, bool& InOutValue) -> bool
	{
		if (!Ctx.Params.IsValid() || !Ctx.Params->HasField(FieldName))
		{
			return true;
		}
		if (!JsonTryGetBool(Ctx.Params, FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		return true;
	};

	bool bNoCompression = false;
	if (!ReadOptionalBool(TEXT("no_compression"), bNoCompression))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->NoCompression = bNoCompression;

	bool bNoAlpha = false;
	if (!ReadOptionalBool(TEXT("no_alpha"), bNoAlpha))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->NoAlpha = bNoAlpha;

	bool bDeferCompression = false;
	if (!ReadOptionalBool(TEXT("defer_compression"), bDeferCompression))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->bDeferCompression = bDeferCompression;

	bool bCreateMaterial = false;
	if (!ReadOptionalBool(TEXT("create_material"), bCreateMaterial))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->bCreateMaterial = bCreateMaterial;

	bool bHasCompressionSettings = false;
	TextureCompressionSettings CompressionSettings = TC_Default;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("compression_settings")))
	{
		FString CompressionSettingsText;
		if (!JsonTryGetString(Ctx.Params, TEXT("compression_settings"), CompressionSettingsText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureCompressionSettings>(CompressionSettingsText, TEXT("compression_settings"), CompressionSettings, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasCompressionSettings = true;
		TextureFactory->CompressionSettings = CompressionSettings;
	}

	bool bHasMipGenSettings = false;
	TextureMipGenSettings MipGenSettings = TMGS_FromTextureGroup;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("mip_gen_settings")))
	{
		FString MipGenSettingsText;
		if (!JsonTryGetString(Ctx.Params, TEXT("mip_gen_settings"), MipGenSettingsText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureMipGenSettings>(MipGenSettingsText, TEXT("mip_gen_settings"), MipGenSettings, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasMipGenSettings = true;
		TextureFactory->MipGenSettings = MipGenSettings;
	}

	bool bHasLODGroup = false;
	TextureGroup LODGroup = TEXTUREGROUP_World;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("lod_group")))
	{
		FString LODGroupText;
		if (!JsonTryGetString(Ctx.Params, TEXT("lod_group"), LODGroupText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureGroup>(LODGroupText, TEXT("lod_group"), LODGroup, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasLODGroup = true;
		TextureFactory->LODGroup = LODGroup;
	}

	if (!TextureFactory->FactoryCanImport(SourceFilename))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		OutError = TEXT("source_file_not_supported_by_texture_factory");
		return false;
	}

	UTextureFactory::SuppressImportOverwriteDialog(bReplaceExistingSettings);
	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = TextureFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = TextureFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	TArray<UTexture2D*> ImportedTextures;
	for (UObject* ImportedObject : ImportedObjects)
	{
		if (UTexture2D* Texture = Cast<UTexture2D>(ImportedObject))
		{
			ImportedTextures.AddUnique(Texture);
		}
	}

	if (ImportedTextures.Num() <= 0)
	{
		UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
		OutData->SetStringField(TEXT("source_filename"), SourceFilename);
		OutData->SetStringField(TEXT("destination_path"), DestinationPath);
		OutData->SetStringField(TEXT("destination_name"), DestinationName);
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		OutError = TEXT("texture_import_created_no_texture2d");
		return false;
	}

	for (UTexture2D* Texture : ImportedTextures)
	{
		if (!Texture)
		{
			continue;
		}
		if (bHasSRGB)
		{
			Texture->SRGB = bSRGB;
		}
		if (bHasCompressionSettings)
		{
			Texture->CompressionSettings = CompressionSettings;
		}
		if (bHasMipGenSettings)
		{
			Texture->MipGenSettings = MipGenSettings;
		}
		if (bHasLODGroup)
		{
			Texture->LODGroup = LODGroup;
		}
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
	}

	if (bSaveAfterImport)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
		}
		for (UTexture2D* Texture : ImportedTextures)
		{
			if (Texture && Texture->GetOutermost())
			{
				PackagesToSave.AddUnique(Texture->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);
	OutData->SetBoolField(TEXT("factory_can_import"), true);
	OutData->SetStringField(TEXT("factory_class"), TextureFactory->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> TextureAssetPathsJson;
	TArray<TSharedPtr<FJsonValue>> TextureObjectPathsJson;
	for (UTexture2D* Texture : ImportedTextures)
	{
		if (!Texture)
		{
			continue;
		}
		TextureAssetPathsJson.Add(MakeShared<FJsonValueString>(Texture->GetOutermost() ? Texture->GetOutermost()->GetName() : FString()));
		TextureObjectPathsJson.Add(MakeShared<FJsonValueString>(Texture->GetPathName()));
	}
	OutData->SetNumberField(TEXT("texture_count"), ImportedTextures.Num());
	OutData->SetArrayField(TEXT("texture_asset_paths"), TextureAssetPathsJson);
	OutData->SetArrayField(TEXT("texture_object_paths"), TextureObjectPathsJson);

	if (UTexture2D* PrimaryTexture = ImportedTextures[0])
	{
		OutData->SetStringField(TEXT("texture_asset_path"), PrimaryTexture->GetOutermost() ? PrimaryTexture->GetOutermost()->GetName() : FString());
		OutData->SetStringField(TEXT("texture_object_path"), PrimaryTexture->GetPathName());
		OutData->SetNumberField(TEXT("texture_size_x"), PrimaryTexture->GetSizeX());
		OutData->SetNumberField(TEXT("texture_size_y"), PrimaryTexture->GetSizeY());
		OutData->SetNumberField(TEXT("texture_num_mips"), PrimaryTexture->GetNumMips());
		OutData->SetBoolField(TEXT("texture_has_alpha"), PrimaryTexture->HasAlphaChannel());
		OutData->SetBoolField(TEXT("srgb"), PrimaryTexture->SRGB != 0);
		OutData->SetStringField(TEXT("compression_settings"), UeAgentAssetOps::EnumValueToString<TextureCompressionSettings>(static_cast<TextureCompressionSettings>(PrimaryTexture->CompressionSettings.GetValue())));
		OutData->SetStringField(TEXT("mip_gen_settings"), UeAgentAssetOps::EnumValueToString<TextureMipGenSettings>(static_cast<TextureMipGenSettings>(PrimaryTexture->MipGenSettings.GetValue())));
		OutData->SetStringField(TEXT("lod_group"), UeAgentAssetOps::EnumValueToString<TextureGroup>(static_cast<TextureGroup>(PrimaryTexture->LODGroup.GetValue())));
	}

	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(ImportedTextures[0]);
		}
	}

	ImportData->RemoveFromRoot();
	TextureFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetImportFbxSkeletalMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilename(SourceFilenameInput, SourceFilename, OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	bool bReplaceExisting = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting);

	bool bReplaceExistingSettings = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings);

	bool bSaveAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);

	bool bImportMaterials = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_materials"), bImportMaterials);

	bool bImportTextures = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_textures"), bImportTextures);

	bool bCreatePhysicsAsset = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_physics_asset"), bCreatePhysicsAsset);

	bool bImportAnimations = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_animations"), bImportAnimations);

	bool bImportMorphTargets = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_morph_targets"), bImportMorphTargets);

	bool bImportSkinWeightProfiles = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_skin_weight_profiles"), bImportSkinWeightProfiles);

	bool bUpdateSkeletonReferencePose = false;
	JsonTryGetBool(Ctx.Params, TEXT("update_skeleton_reference_pose"), bUpdateSkeletonReferencePose);

	bool bPreserveExistingMorphTargets = true;
	JsonTryGetBool(Ctx.Params, TEXT("preserve_existing_morph_targets"), bPreserveExistingMorphTargets);

	bool bValidateAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("validate_after_import"), bValidateAfterImport);

	int32 ExpectedLodCount = INDEX_NONE;
	double ExpectedLodCountNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("expected_lod_count"), ExpectedLodCountNumber))
	{
		ExpectedLodCount = static_cast<int32>(ExpectedLodCountNumber);
	}

	TSet<FString> ExpectedMorphTargets;
	if (const TArray<TSharedPtr<FJsonValue>>* ExpectedMorphArray = nullptr; Ctx.Params->TryGetArrayField(TEXT("expected_morph_targets"), ExpectedMorphArray) && ExpectedMorphArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExpectedMorphArray)
		{
			if (Value.IsValid())
			{
				FString Name = Value->AsString();
				Name.TrimStartAndEndInline();
				if (!Name.IsEmpty())
				{
					ExpectedMorphTargets.Add(Name);
				}
			}
		}
	}

	TSet<FString> ExpectedMaterialSlots;
	if (const TArray<TSharedPtr<FJsonValue>>* ExpectedMaterialArray = nullptr; Ctx.Params->TryGetArrayField(TEXT("expected_material_slots"), ExpectedMaterialArray) && ExpectedMaterialArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExpectedMaterialArray)
		{
			if (Value.IsValid())
			{
				FString Name = Value->AsString();
				Name.TrimStartAndEndInline();
				if (!Name.IsEmpty())
				{
					ExpectedMaterialSlots.Add(Name);
				}
			}
		}
	}

	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);
	SkeletonPath.TrimStartAndEndInline();

	USkeleton* ExistingSkeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		ExistingSkeleton = Cast<USkeleton>(UeAgentAssetOps::LoadAssetObject(SkeletonPath));
		if (!ExistingSkeleton)
		{
			OutError = TEXT("skeleton_not_found");
			return false;
		}
	}

	UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!FbxFactory || !ImportData || !FbxFactory->ImportUI)
	{
		OutError = TEXT("create_fbx_import_task_failed");
		return false;
	}

	FbxFactory->AddToRoot();
	ImportData->AddToRoot();

	FbxFactory->SetDetectImportTypeOnImport(false);
	FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
	FbxFactory->ImportUI->bImportAsSkeletal = true;
	FbxFactory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
	FbxFactory->ImportUI->OriginalImportType = FBXIT_SkeletalMesh;
	FbxFactory->ImportUI->bIsReimport = false;
	FbxFactory->ImportUI->ReimportMesh = nullptr;
	FbxFactory->ImportUI->bAllowContentTypeImport = true;
	FbxFactory->ImportUI->bImportAnimations = bImportAnimations;
	FbxFactory->ImportUI->bCreatePhysicsAsset = bCreatePhysicsAsset;
	FbxFactory->ImportUI->bImportMaterials = bImportMaterials;
	FbxFactory->ImportUI->bImportTextures = bImportTextures;
	FbxFactory->ImportUI->bImportMesh = true;
	FbxFactory->ImportUI->bImportRigidMesh = false;
	FbxFactory->ImportUI->bIsObjImport = false;
	FbxFactory->ImportUI->bOverrideFullName = true;
	FbxFactory->ImportUI->Skeleton = ExistingSkeleton;

	if (FbxFactory->ImportUI->SkeletalMeshImportData)
	{
		FbxFactory->ImportUI->SkeletalMeshImportData->bImportMeshLODs = false;
		FbxFactory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = bImportMorphTargets;
		FbxFactory->ImportUI->SkeletalMeshImportData->bUpdateSkeletonReferencePose = bUpdateSkeletonReferencePose;
		FbxFactory->ImportUI->SkeletalMeshImportData->ImportContentType = EFBXImportContentType::FBXICT_All;
	}

	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = FbxFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = FbxFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);
	TArray<UObject*> ImportedObjectsUnique;
	UeAgentAssetOps::GatherImportedObjects(ImportedObjects, ImportedObjectsUnique);

	if (bSaveAfterImport && ImportedObjects.Num() > 0)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
			if (USkeletalMesh* ImportedMesh = Cast<USkeletalMesh>(ImportedObject))
			{
				if (USkeleton* ImportedSkeleton = ImportedMesh->GetSkeleton())
				{
					if (ImportedSkeleton->GetOutermost())
					{
						PackagesToSave.AddUnique(ImportedSkeleton->GetOutermost());
					}
				}
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			FbxFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);
	OutData->SetBoolField(TEXT("import_morph_targets"), bImportMorphTargets);
	OutData->SetBoolField(TEXT("import_skin_weight_profiles"), bImportSkinWeightProfiles);
	OutData->SetBoolField(TEXT("update_skeleton_reference_pose"), bUpdateSkeletonReferencePose);
	OutData->SetBoolField(TEXT("preserve_existing_morph_targets"), bPreserveExistingMorphTargets);
	OutData->SetBoolField(TEXT("validate_after_import"), bValidateAfterImport);

	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	TArray<TSharedPtr<FJsonValue>> ImportedSkinWeightProfilesJson;
	USkeletalMesh* ImportedSkeletalMesh = nullptr;
	for (UObject* ImportedObject : ImportedObjectsUnique)
	{
		if (USkeletalMesh* CandidateMesh = Cast<USkeletalMesh>(ImportedObject))
		{
			ImportedSkeletalMesh = CandidateMesh;
			break;
		}
	}

	if (bImportSkinWeightProfiles)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("code"), TEXT("skin_weight_profiles_use_explicit_command"));
		Issue->SetStringField(TEXT("field"), TEXT("import_skin_weight_profiles"));
		Issue->SetStringField(TEXT("message"), TEXT("FBX skeletal mesh import does not implicitly import skin weight profiles. Use skeletal_mesh_import_skin_weight_profile after mesh import."));
		ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	if (!bPreserveExistingMorphTargets)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("code"), TEXT("preserve_existing_morph_targets_not_supported_by_new_import"));
		Issue->SetStringField(TEXT("field"), TEXT("preserve_existing_morph_targets"));
		Issue->SetStringField(TEXT("message"), TEXT("This parameter is reported for design compatibility. New FBX import has no existing morph targets to preserve; reimport-specific preservation needs a dedicated reimport path."));
		ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	if (bValidateAfterImport && ImportedSkeletalMesh)
	{
		OutData->SetStringField(TEXT("skeletal_mesh_asset_path"), ImportedSkeletalMesh->GetOutermost() ? ImportedSkeletalMesh->GetOutermost()->GetName() : FString());
		OutData->SetNumberField(TEXT("lod_count"), ImportedSkeletalMesh->GetLODNum());
		OutData->SetNumberField(TEXT("material_slot_count"), ImportedSkeletalMesh->GetMaterials().Num());
		OutData->SetNumberField(TEXT("morph_target_count"), ImportedSkeletalMesh->GetMorphTargets().Num());
		OutData->SetNumberField(TEXT("skin_weight_profile_count"), ImportedSkeletalMesh->GetNumSkinWeightProfiles());
		if (USkeleton* ImportedSkeleton = ImportedSkeletalMesh->GetSkeleton())
		{
			OutData->SetStringField(TEXT("skeleton_asset_path"), ImportedSkeleton->GetOutermost() ? ImportedSkeleton->GetOutermost()->GetName() : FString());
		}
		else
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), TEXT("imported_skeletal_mesh_missing_skeleton"));
			Issue->SetStringField(TEXT("field"), TEXT("skeleton"));
			Issue->SetStringField(TEXT("message"), TEXT("Imported skeletal mesh has no USkeleton reference; animation authoring assets cannot use it."));
			ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
		}
		int32 SectionCount = 0;
		if (const FSkeletalMeshModel* ImportedModel = ImportedSkeletalMesh->GetImportedModel())
		{
			for (const FSkeletalMeshLODModel& LodModel : ImportedModel->LODModels)
			{
				SectionCount += LodModel.Sections.Num();
			}
		}
		OutData->SetNumberField(TEXT("section_count"), SectionCount);

		for (const FSkinWeightProfileInfo& ProfileInfo : ImportedSkeletalMesh->GetSkinWeightProfiles())
		{
			ImportedSkinWeightProfilesJson.Add(MakeShared<FJsonValueString>(ProfileInfo.Name.ToString()));
		}
		OutData->SetArrayField(TEXT("skin_weight_profiles_imported"), ImportedSkinWeightProfilesJson);

		TSet<FString> ImportedMorphNames;
		TArray<TSharedPtr<FJsonValue>> ImportedMorphNamesJson;
		for (const TObjectPtr<UMorphTarget>& MorphTarget : ImportedSkeletalMesh->GetMorphTargets())
		{
			if (MorphTarget)
			{
				ImportedMorphNames.Add(MorphTarget->GetName());
				ImportedMorphNamesJson.Add(MakeShared<FJsonValueString>(MorphTarget->GetName()));
			}
		}
		OutData->SetArrayField(TEXT("morph_targets_imported"), ImportedMorphNamesJson);

		TSet<FString> ImportedMaterialSlotNames;
		TArray<TSharedPtr<FJsonValue>> ImportedMaterialSlotNamesJson;
		for (const FSkeletalMaterial& Material : ImportedSkeletalMesh->GetMaterials())
		{
			const FString SlotName = Material.MaterialSlotName.ToString();
			ImportedMaterialSlotNames.Add(SlotName);
			ImportedMaterialSlotNamesJson.Add(MakeShared<FJsonValueString>(SlotName));
		}
		OutData->SetArrayField(TEXT("material_slots_imported"), ImportedMaterialSlotNamesJson);

		if (ExpectedLodCount != INDEX_NONE && ImportedSkeletalMesh->GetLODNum() != ExpectedLodCount)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), TEXT("expected_lod_count_mismatch"));
			Issue->SetStringField(TEXT("field"), TEXT("expected_lod_count"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Expected %d LODs but imported %d."), ExpectedLodCount, ImportedSkeletalMesh->GetLODNum()));
			ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		for (const FString& ExpectedMorph : ExpectedMorphTargets)
		{
			if (!ImportedMorphNames.Contains(ExpectedMorph))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("expected_morph_target_missing"));
				Issue->SetStringField(TEXT("field"), TEXT("expected_morph_targets"));
				Issue->SetStringField(TEXT("message"), ExpectedMorph);
				ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}

		for (const FString& ExpectedSlot : ExpectedMaterialSlots)
		{
			if (!ImportedMaterialSlotNames.Contains(ExpectedSlot))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("expected_material_slot_missing"));
				Issue->SetStringField(TEXT("field"), TEXT("expected_material_slots"));
				Issue->SetStringField(TEXT("message"), ExpectedSlot);
				ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}
	}

	if (OutData->GetIntegerField(TEXT("imported_object_count")) <= 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_import_created_no_assets");
		return false;
	}

	int32 ValidationErrorCount = 0;
	for (const TSharedPtr<FJsonValue>& IssueValue : ValidationIssues)
	{
		const TSharedPtr<FJsonObject> IssueObj = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
		FString Severity;
		if (IssueObj.IsValid() && IssueObj->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			++ValidationErrorCount;
		}
	}
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	OutData->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
	OutData->SetBoolField(TEXT("validation_passed"), ValidationErrorCount == 0);
	if (!OutData->HasField(TEXT("skin_weight_profiles_imported")))
	{
		OutData->SetArrayField(TEXT("skin_weight_profiles_imported"), ImportedSkinWeightProfilesJson);
	}
	TSharedPtr<FJsonObject> ValidationReport = MakeShared<FJsonObject>();
	ValidationReport->SetBoolField(TEXT("validate_after_import"), bValidateAfterImport);
	ValidationReport->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	ValidationReport->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
	ValidationReport->SetBoolField(TEXT("validation_passed"), ValidationErrorCount == 0);
	ValidationReport->SetArrayField(TEXT("issues"), ValidationIssues);
	ValidationReport->SetNumberField(TEXT("imported_object_count"), OutData->GetIntegerField(TEXT("imported_object_count")));
	ValidationReport->SetNumberField(TEXT("skin_weight_profile_count"), ImportedSkeletalMesh ? ImportedSkeletalMesh->GetNumSkinWeightProfiles() : 0);
	ValidationReport->SetNumberField(TEXT("section_count"), ImportedSkeletalMesh && OutData->HasField(TEXT("section_count")) ? OutData->GetIntegerField(TEXT("section_count")) : 0);
	OutData->SetObjectField(TEXT("validation_report"), ValidationReport);
	if (ValidationErrorCount > 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_skeletal_mesh_import_validation_failed");
		return false;
	}

	if (bOpenEditor)
	{
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->IsA<USkeletalMesh>())
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					AssetEditorSubsystem->OpenEditorForAsset(ImportedObject);
				}
				break;
			}
		}
	}

	ImportData->RemoveFromRoot();
	FbxFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetImportFbxAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);
	SkeletonPath.TrimStartAndEndInline();

	FString SkeletalMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	SkeletalMeshPath.TrimStartAndEndInline();

	if (SkeletonPath.IsEmpty() && SkeletalMeshPath.IsEmpty())
	{
		OutError = TEXT("missing_skeleton_or_skeletal_mesh_path");
		return false;
	}

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilename(SourceFilenameInput, SourceFilename, OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	USkeleton* TargetSkeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		TargetSkeleton = Cast<USkeleton>(UeAgentAssetOps::LoadAssetObject(SkeletonPath));
	}
	if (!TargetSkeleton && !SkeletalMeshPath.IsEmpty())
	{
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(UeAgentAssetOps::LoadAssetObject(SkeletalMeshPath)))
		{
			TargetSkeleton = SkeletalMesh->GetSkeleton();
			if (TargetSkeleton && SkeletonPath.IsEmpty())
			{
				SkeletonPath = TargetSkeleton->GetOutermost() ? TargetSkeleton->GetOutermost()->GetName() : FString();
			}
		}
	}
	if (!TargetSkeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	bool bReplaceExisting = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting);

	bool bReplaceExistingSettings = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings);

	bool bSaveAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);

	UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!FbxFactory || !ImportData || !FbxFactory->ImportUI)
	{
		OutError = TEXT("create_fbx_import_task_failed");
		return false;
	}

	FbxFactory->AddToRoot();
	ImportData->AddToRoot();

	FbxFactory->SetDetectImportTypeOnImport(false);
	FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
	FbxFactory->ImportUI->bImportAsSkeletal = true;
	FbxFactory->ImportUI->MeshTypeToImport = FBXIT_Animation;
	FbxFactory->ImportUI->OriginalImportType = FBXIT_Animation;
	FbxFactory->ImportUI->bIsReimport = false;
	FbxFactory->ImportUI->ReimportMesh = nullptr;
	FbxFactory->ImportUI->bAllowContentTypeImport = true;
	FbxFactory->ImportUI->bImportAnimations = true;
	FbxFactory->ImportUI->bCreatePhysicsAsset = false;
	FbxFactory->ImportUI->bImportMaterials = false;
	FbxFactory->ImportUI->bImportTextures = false;
	FbxFactory->ImportUI->bImportMesh = false;
	FbxFactory->ImportUI->bImportRigidMesh = false;
	FbxFactory->ImportUI->bIsObjImport = false;
	FbxFactory->ImportUI->bOverrideFullName = true;
	FbxFactory->ImportUI->Skeleton = TargetSkeleton;
	if (!DestinationName.IsEmpty())
	{
		FbxFactory->ImportUI->OverrideAnimationName = DestinationName;
	}

	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = FbxFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = FbxFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (bSaveAfterImport && ImportedObjects.Num() > 0)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			FbxFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetStringField(TEXT("skeleton_path"), TargetSkeleton->GetOutermost()->GetName());
	if (!SkeletalMeshPath.IsEmpty())
	{
		OutData->SetStringField(TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	}
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);

	if (OutData->GetIntegerField(TEXT("imported_object_count")) <= 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_import_created_no_assets");
		return false;
	}

	if (bOpenEditor)
	{
		TArray<UObject*> ImportedObjectsUnique;
		UeAgentAssetOps::GatherImportedObjects(ImportedObjects, ImportedObjectsUnique);
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->IsA<UAnimSequence>())
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					AssetEditorSubsystem->OpenEditorForAsset(ImportedObject);
				}
				break;
			}
		}
	}

	ImportData->RemoveFromRoot();
	FbxFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetExportPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	TArray<FString> PropertyNames;
	const bool bHasExplicitPropertyNames = UeAgentAssetOps::ReadStringArrayParam(Ctx.Params, TEXT("property_names"), PropertyNames) && PropertyNames.Num() > 0;
	FString PresetName;
	if (!bHasExplicitPropertyNames)
	{
		UeAgentAssetOps::GetDefaultAssetPropertyPaths(Asset, PropertyNames, PresetName);
	}

	if (PropertyNames.Num() == 0)
	{
		OutError = TEXT("missing_property_names_and_no_default_preset");
		return false;
	}

	PropertyNames.Sort();
	for (int32 Index = PropertyNames.Num() - 1; Index > 0; --Index)
	{
		if (PropertyNames[Index].Equals(PropertyNames[Index - 1], ESearchCase::CaseSensitive))
		{
			PropertyNames.RemoveAt(Index);
		}
	}

	TArray<TSharedPtr<FJsonValue>> PropertiesJson;
	TArray<TSharedPtr<FJsonValue>> MissingPropertiesJson;
	int32 ExportedPropertyCount = 0;
	for (const FString& PropertyPath : PropertyNames)
	{
		FString ValueText;
		FString CppType;
		if (!UeAgentAssetOps::GetObjectPropertyText(Asset, PropertyPath, ValueText, CppType))
		{
			MissingPropertiesJson.Add(MakeShared<FJsonValueString>(PropertyPath));
			continue;
		}

		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		if (!CppType.IsEmpty())
		{
			PropertyObj->SetStringField(TEXT("cpp_type"), CppType);
		}
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyPath, Property, ValuePtr) && Property && ValuePtr)
		{
			TSharedPtr<FJsonObject> CurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
			{
				PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
				PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
				PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
			}
		}
		PropertiesJson.Add(MakeShared<FJsonValueObject>(PropertyObj));
		++ExportedPropertyCount;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("format_version"), 1);
	Root->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
	Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		Root->SetStringField(TEXT("property_preset"), PresetName);
	}
	Root->SetArrayField(TEXT("properties"), PropertiesJson);
	if (MissingPropertiesJson.Num() > 0)
	{
		Root->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentAssetOps::SaveJsonFile(OutputFile, Root, SavedOutputFile, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		OutData->SetStringField(TEXT("property_preset"), PresetName);
	}
	OutData->SetArrayField(TEXT("properties"), PropertiesJson);
	OutData->SetNumberField(TEXT("property_count"), ExportedPropertyCount);
	if (MissingPropertiesJson.Num() > 0)
	{
		OutData->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
		OutData->SetNumberField(TEXT("missing_property_count"), MissingPropertiesJson.Num());
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdAssetApplyPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && JsonRoot.IsValid())
	{
		JsonRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
	{
		if (!JsonRoot.IsValid() || !JsonRoot->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			OutError = TEXT("missing_properties");
			return false;
		}
	}

	int32 AppliedPropertyCount = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResultsJson;
	int32 PropertyIndex = 0;
	for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
	{
		const FString PropertyPath = FString::Printf(TEXT("properties[%d]"), PropertyIndex++);
		TArray<TSharedPtr<FJsonValue>> PropertyJsonIssues;
		TSharedPtr<FJsonObject> PropertyObj;
		if (!UeAgentJsonDiagnostics::ReadObjectFromValue(PropertyValue, PropertyPath, PropertyObj, PropertyJsonIssues))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("invalid_property_entry"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("invalid_property_entry");
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			PropertyObj,
			PropertyPath,
			{ TEXT("property_name"), TEXT("value_text"), TEXT("cpp_type"), TEXT("value_json"), TEXT("curve_json"), TEXT("value_json_schema") },
			PropertyJsonIssues);

		FString PropertyName;
		FString ValueText;
		if (!UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("property_name"), PropertyPath + TEXT(".property_name"), PropertyName, PropertyJsonIssues, true)
			|| PropertyName.IsEmpty())
		{
			if (PropertyName.IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(PropertyJsonIssues, TEXT("warning"), TEXT("json_missing_required_field"), PropertyPath + TEXT(".property_name"), TEXT("Required string field is empty."));
			}
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("missing_property_name"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_property_name");
			return false;
		}

		TSharedPtr<FJsonObject> TypedCurveJson;
		bool bHasTypedCurveJson = false;
		const TSharedPtr<FJsonObject>* CurveJsonPtr = nullptr;
		FString TypedCurveJsonFieldName;
		if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("curve_json");
		}
		else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("value_json");
		}

		if (!bHasTypedCurveJson && !UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("value_text"), PropertyPath + TEXT(".value_text"), ValueText, PropertyJsonIssues, true))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("missing_value_text"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_value_text");
			return false;
		}

		FString AppliedValueText;
		FString CppType;
		FString ImportError;
		if (bHasTypedCurveJson)
		{
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("property_not_found"), TEXT("property_not_found"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("property_not_found");
				return false;
			}

			if (!UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, TypedCurveJson, PropertyPath + TEXT(".") + TypedCurveJsonFieldName, PropertyJsonIssues))
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), TEXT(""), TEXT("curve_json_apply_failed"), TEXT("curve_json_apply_failed"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("curve_json_apply_failed");
				return false;
			}

			Property->ExportTextItem_Direct(AppliedValueText, ValuePtr, nullptr, Asset, PPF_None);
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), AppliedValueText, TEXT("curve_json_applied_and_read_back"));
			TSharedPtr<FJsonObject> ReadbackCurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, ReadbackCurveJson) && ReadbackCurveJson.IsValid())
			{
				ResultObj->SetObjectField(TEXT("value_json_read_back"), ReadbackCurveJson);
			}
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			++AppliedPropertyCount;
			continue;
		}

		if (!UeAgentAssetOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
		{
			const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
			ResultObj->SetStringField(TEXT("cpp_type"), CppType);
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("asset_property_apply_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
			return false;
		}

		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		if (PropertyJsonIssues.Num() > 0)
		{
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
		}
		PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
		++AppliedPropertyCount;
	}

	if (AppliedPropertyCount > 0)
	{
		Asset->Modify();
	}
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("applied_property_count"), AppliedPropertyCount);
	OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TSharedPtr<FJsonObject> CurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (!CurveJson.IsValid())
	{
		OutError = TEXT("unsupported_curve_asset");
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentAssetOps::SaveJsonFile(OutputFile, CurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetObjectField(TEXT("curve"), CurveJson);
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> CurveJson;
	const TSharedPtr<FJsonObject>* CurveObjPtr = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid())
	{
		CurveJson = JsonRoot;
	}
	else
	{
		CurveJson = Ctx.Params;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && CurveJson.IsValid())
	{
		CurveJson->TryGetStringField(TEXT("asset_path"), AssetPathInput);
		AssetPathInput.TrimStartAndEndInline();
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	bool bCreatedAsset = false;
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	if (!Asset && bCreateIfMissing)
	{
		FString CurveKind;
		JsonTryGetString(Ctx.Params, TEXT("curve_kind"), CurveKind);
		if (CurveKind.TrimStartAndEnd().IsEmpty() && CurveJson.IsValid())
		{
			CurveJson->TryGetStringField(TEXT("curve_kind"), CurveKind);
		}
		Asset = UeAgentCurveJson::CreateCurveAsset(AssetPathInput, CurveKind, OutError);
		bCreatedAsset = Asset != nullptr;
	}
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> JsonIssues;
	if (!UeAgentCurveJson::ApplyCurveAssetJson(Asset, CurveJson, TEXT("curve"), JsonIssues))
	{
		OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
		OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
		OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
		if (!ResolvedJsonFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
		}
		OutError = TEXT("curve_json_apply_failed");
		return false;
	}

	Asset->Modify();
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	TSharedPtr<FJsonObject> ReadbackCurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (ReadbackCurveJson.IsValid())
	{
		OutData->SetObjectField(TEXT("curve_read_back"), ReadbackCurveJson);
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && ReadbackCurveJson.IsValid() && !UeAgentAssetOps::SaveJsonFile(OutputFile, ReadbackCurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("created"), bCreatedAsset);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
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

		ResultObj->SetBoolField(TEXT("blocked"), false);
		ResultObj->SetBoolField(TEXT("handled"), bHandled);
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

bool FUeAgentHttpServer::CmdEditorListDirtyResources(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetBoolField(TEXT("has_unresolved_dirty_resources"), DirtyResourcesJson.Num() > 0);
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);
	return true;
}

bool FUeAgentHttpServer::CmdEditorResolveDirtyResources(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	UeAgentAssetOps::FDirtyResolutionOptions Options;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), Options.bSaveCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), Options.bDiscardCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), Options.bSaveAllDirty);
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), Options.bDiscardAllDirty);
	if (!Options.bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), Options.bSaveAllDirty);
	}
	if (!Options.bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), Options.bDiscardAllDirty);
	}
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), Options.bOnlySaveDirty);
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, Options.SaveResourcePaths, Options.DiscardResourcePaths);

	UeAgentAssetOps::FDirtyResolutionSummary Summary;
	if (!UeAgentAssetOps::ResolveDirtyResources(Options, EditorWorld, Summary, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> DirtyBeforeJson;
	TArray<TSharedPtr<FJsonValue>> DirtyAfterJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesBefore, EditorWorld, AssetEditorSubsystem, DirtyBeforeJson);
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesAfter, EditorWorld, AssetEditorSubsystem, DirtyAfterJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : Summary.SaveResourceNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : Summary.DiscardResourceNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	OutData->SetNumberField(TEXT("saved_package_count"), Summary.SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), Summary.ExplicitDiscardedPackageCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), Summary.AutoDiscardedPackageCount);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	OutData->SetArrayField(TEXT("save_resource_not_found"), SaveNotFoundJson);
	OutData->SetArrayField(TEXT("discard_resource_not_found"), DiscardNotFoundJson);
	OutData->SetArrayField(TEXT("dirty_resources_before"), DirtyBeforeJson);
	OutData->SetArrayField(TEXT("dirty_resources_after"), DirtyAfterJson);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), DirtyAfterJson.Num());
	OutData->SetBoolField(TEXT("all_dirty_resources_resolved"), DirtyAfterJson.Num() == 0);
	return true;
}

bool FUeAgentHttpServer::CmdEditorClose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);

	if (DirtyResourcesJson.Num() > 0)
	{
		OutData->SetBoolField(TEXT("close_requested"), false);
		OutData->SetBoolField(TEXT("closed"), false);
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystemLocal = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystemLocal->CloseAllAssetEditors();
		}
	}

	if (bRequestExit)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.0f);
	}

	OutData->SetBoolField(TEXT("close_requested"), bRequestExit);
	OutData->SetBoolField(TEXT("closed"), bRequestExit);
	return true;
}

bool FUeAgentHttpServer::CmdEditorPrepareExit(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bSaveCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), bSaveCurrentLevel);

	bool bDiscardCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), bDiscardCurrentLevel);
	if (bSaveCurrentLevel && bDiscardCurrentLevel)
	{
		OutError = TEXT("save_and_discard_current_level_conflict");
		return false;
	}

	bool bSaveAllDirty = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), bSaveAllDirty);
	if (!bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), bSaveAllDirty);
	}

	bool bDiscardAllDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), bDiscardAllDirty);
	if (!bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), bDiscardAllDirty);
	}

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	bool bOnlySaveDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), bOnlySaveDirty);

	TArray<FString> SaveAssetPaths;
	TArray<FString> DiscardAssetPaths;
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, SaveAssetPaths, DiscardAssetPaths);

	TSet<UPackage*> SavePackagesSet;
	TSet<UPackage*> DiscardPackagesSet;
	TArray<FString> SaveAssetNotFound;
	TArray<FString> DiscardAssetNotFound;

	for (const FString& SavePath : SaveAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(SavePath);
		if (!Package)
		{
			SaveAssetNotFound.Add(SavePath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(SavePackagesSet, Package);
	}

	for (const FString& DiscardPath : DiscardAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(DiscardPath);
		if (!Package)
		{
			DiscardAssetNotFound.Add(DiscardPath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(DiscardPackagesSet, Package);
	}

	UWorld* EditorWorld = nullptr;
	if (bSaveCurrentLevel || bDiscardCurrentLevel)
	{
		EditorWorld = GetEditorWorld(OutError);
		if (!EditorWorld)
		{
			return false;
		}
		if (UPackage* WorldPackage = EditorWorld->GetOutermost())
		{
			if (bSaveCurrentLevel)
			{
				SavePackagesSet.Add(WorldPackage);
			}
			if (bDiscardCurrentLevel)
			{
				DiscardPackagesSet.Add(WorldPackage);
			}
		}
	}

	TSet<UPackage*> DirtyPackagesBefore;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
		{
			continue;
		}
		DirtyPackagesBefore.Add(Package);
	}

	for (UPackage* DiscardPackage : DiscardPackagesSet)
	{
		SavePackagesSet.Remove(DiscardPackage);
	}

	if (bSaveAllDirty)
	{
		for (UPackage* DirtyPackage : DirtyPackagesBefore)
		{
			if (!DiscardPackagesSet.Contains(DirtyPackage))
			{
				SavePackagesSet.Add(DirtyPackage);
			}
		}
	}

	TArray<UPackage*> PackagesToSave;
	for (UPackage* Package : SavePackagesSet)
	{
		if (!Package)
		{
			continue;
		}
		if (bOnlySaveDirty && !Package->IsDirty())
		{
			continue;
		}
		PackagesToSave.Add(Package);
	}

	int32 SavedPackageCount = 0;
	if (PackagesToSave.Num() > 0)
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
		if (!bSaved)
		{
			OutError = TEXT("save_packages_failed");
			return false;
		}
		SavedPackageCount = PackagesToSave.Num();
	}

	const int32 ExplicitDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(DiscardPackagesSet);

	int32 AutoDiscardedCount = 0;
	if (bDiscardAllDirty)
	{
		TSet<UPackage*> RemainingDirty;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			if (SavePackagesSet.Contains(Package))
			{
				continue;
			}
			RemainingDirty.Add(Package);
		}
		AutoDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(RemainingDirty);
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->CloseAllAssetEditors();
		}
	}

	TArray<UPackage*> RemainingDirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(RemainingDirtyPackages);
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	TArray<TSharedPtr<FJsonValue>> RemainingDirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(RemainingDirtyPackages, EditorWorld, AssetEditorSubsystem, RemainingDirtyResourcesJson);

	OutData->SetNumberField(TEXT("saved_package_count"), SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), ExplicitDiscardedCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), AutoDiscardedCount);
	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), RemainingDirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), RemainingDirtyResourcesJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : SaveAssetNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("save_asset_not_found"), SaveNotFoundJson);

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : DiscardAssetNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("discard_asset_not_found"), DiscardNotFoundJson);

	FUeAgentInterfaceLogger::Log(
		TEXT("EditorPrepareExit: save_paths=%d discard_paths=%d saved=%d discard_explicit=%d discard_auto=%d request_exit=%s"),
		SaveAssetPaths.Num(),
		DiscardAssetPaths.Num(),
		SavedPackageCount,
		ExplicitDiscardedCount,
		AutoDiscardedCount,
		bRequestExit ? TEXT("true") : TEXT("false"));

	if (RemainingDirtyResourcesJson.Num() > 0)
	{
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	if (bRequestExit)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.0f);
	}

	return true;
}
