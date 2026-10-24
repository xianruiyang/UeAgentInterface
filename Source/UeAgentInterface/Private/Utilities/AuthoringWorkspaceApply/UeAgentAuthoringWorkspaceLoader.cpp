// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace
{
	using ESourceKind = UeAgentApplyWorkspaceContract::ESourceKind;
	using FDocumentCandidate = UeAgentAuthoringWorkspaceRouter::FDocumentCandidate;
	using FLoadResult = UeAgentAuthoringWorkspaceLoader::FLoadResult;
	using FSourceProvenance = UeAgentAuthoringWorkspaceRouter::FSourceProvenance;

	

	void SetFailure(FLoadResult& OutResult, UE::UAI::Contract::FDiagnostic Diagnostic, const FSourceProvenance* Provenance = nullptr)
    {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = MoveTemp(Diagnostic);
        if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("workspace_load");
        if (OutResult.Diagnostic.SuggestedFix.IsEmpty()) OutResult.Diagnostic.SuggestedFix = TEXT("Correct the Apply workspace source or contract and retry preflight.");
        if (Provenance)
        {
            const auto Source = Provenance->ToDiagnosticSource();
            if (OutResult.Diagnostic.Source.WorkspaceSource.IsEmpty()) OutResult.Diagnostic.Source.WorkspaceSource = Source.WorkspaceSource;
            if (OutResult.Diagnostic.Source.SourceFile.IsEmpty()) OutResult.Diagnostic.Source.SourceFile = Source.SourceFile;
            if (OutResult.Diagnostic.Source.SourceUnit.IsEmpty()) OutResult.Diagnostic.Source.SourceUnit = Source.SourceUnit;
            if (OutResult.Diagnostic.Source.InternalJsonPath.IsEmpty()) OutResult.Diagnostic.Source.InternalJsonPath = Source.InternalJsonPath;
            if (OutResult.Diagnostic.Source.JsonPath.IsEmpty()) OutResult.Diagnostic.Source.JsonPath = Source.JsonPath;
            if (OutResult.Diagnostic.Source.Line <= 0) OutResult.Diagnostic.Source.Line = Source.Line;
            if (OutResult.Diagnostic.Source.Column <= 0) OutResult.Diagnostic.Source.Column = Source.Column;
        }
    }

	int64 Utf8ByteCount(const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		return Converted.Length();
	}

	bool SerializeInlineObject(
		const TSharedPtr<const FJsonObject>& Object,
		FString& OutText)
	{
		OutText.Reset();
		if (!Object.IsValid()) return false;
		const TSharedPtr<FJsonObject> ReadOnlyView = ConstCastSharedPtr<FJsonObject>(Object);
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
		return FJsonSerializer::Serialize(ReadOnlyView.ToSharedRef(), Writer);
	}

	void ApplyRootLocation(FDocumentCandidate& Candidate)
	{
		const FUeAgentHjsonSourceLocation* Location = Candidate.SourceLocations.Find(TEXT("/"));
		if (!Location && !Candidate.SourceLocations.IsEmpty())
		{
			TArray<FString> Keys;
			Candidate.SourceLocations.GetKeys(Keys);
			Keys.Sort();
			Location = Candidate.SourceLocations.Find(Keys[0]);
		}
		if (Location)
		{
			Candidate.Provenance.Line = Location->Line;
			Candidate.Provenance.Column = Location->Column;
		}
	}

	bool ParseCandidateText(
		const FString& Text,
		const FString& ParseSource,
		FDocumentCandidate& InOutCandidate,
		FLoadResult& OutResult)
	{
		if (Text.TrimStartAndEnd().IsEmpty())
		{
			InOutCandidate.bEmpty = true;
			return true;
		}

		FUeAgentStructuredHjsonDocument Document;
		UE::UAI::Contract::FDiagnostic ParseError;
		if (!FUeAgentStructuredHjsonParser::ParseDocument(Text, ParseSource, Document, ParseError))
		{
			SetFailure(OutResult, ParseError, &InOutCandidate.Provenance);
			return false;
		}
		InOutCandidate.Document = Document.Root;
		InOutCandidate.SourceLocations = MoveTemp(Document.SourceLocations);
		InOutCandidate.bEmpty = !Document.Root.IsValid() || Document.Root->Values.IsEmpty();
		ApplyRootLocation(InOutCandidate);
		return true;
	}

	bool ReadFileCandidate(
		const FString& AbsolutePath,
		const FString& RoutedRelativePath,
		const FString& ActualRelativePath,
		ESourceKind SourceKind,
		const FString& WorkspaceSource,
		const FString& SourceUnit,
		bool bVirtual,
		int64& InOutTotalBytes,
		FDocumentCandidate& OutCandidate,
		FLoadResult& OutResult)
	{
		OutCandidate = FDocumentCandidate();
		OutCandidate.AbsolutePath = AbsolutePath;
		OutCandidate.RelativePath = RoutedRelativePath;
		OutCandidate.bVirtual = bVirtual;
		OutCandidate.Provenance.SourceKind = SourceKind;
		OutCandidate.Provenance.WorkspaceSource = WorkspaceSource;
		OutCandidate.Provenance.SourceFile = AbsolutePath;
		OutCandidate.Provenance.SourceUnit = SourceUnit;
		OutCandidate.Provenance.RelativePath = ActualRelativePath;
		OutCandidate.Provenance.bVirtual = bVirtual;

		if (!FPaths::GetExtension(AbsolutePath, false).Equals(TEXT("hjson"), ESearchCase::IgnoreCase))
		{
			SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_file_unknown"), TEXT("The file is not supported by this workspace contract."), TEXT("workspace_load"), ActualRelativePath), &OutCandidate.Provenance);
			return false;
		}
		const int64 FileBytes = IFileManager::Get().FileSize(*AbsolutePath);
		if (FileBytes < 0)
		{
            SetFailure(OutResult, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("workspace_file_stat_failed"),
                TEXT("The workspace file size could not be read; the file may be missing or inaccessible."), TEXT("workspace_load"), AbsolutePath), &OutCandidate.Provenance);
			return false;
		}
		InOutTotalBytes += FileBytes;

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *AbsolutePath))
		{
            SetFailure(OutResult, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("workspace_source_invalid"), TEXT("The workspace file could not be read."), TEXT("workspace_load"), AbsolutePath), &OutCandidate.Provenance);
			return false;
		}
		return ParseCandidateText(Text, AbsolutePath, OutCandidate, OutResult);
	}

	bool LoadDirectoryCandidates(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		FString& OutWorkspaceRoot,
		TArray<FDocumentCandidate>& OutCandidates,
		FLoadResult& OutResult)
	{
		OutWorkspaceRoot = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(Source.Path);
		FSourceProvenance WorkspaceProvenance;
		WorkspaceProvenance.SourceKind = ESourceKind::Directory;
		WorkspaceProvenance.WorkspaceSource = OutWorkspaceRoot.IsEmpty() ? Source.Path : OutWorkspaceRoot;
		WorkspaceProvenance.SourceFile = WorkspaceProvenance.WorkspaceSource;
		if (OutWorkspaceRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*OutWorkspaceRoot))
		{
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("path"),
                MakeShared<FJsonValueString>(TEXT("existing directory")), MakeShared<FJsonValueString>(Source.Path), TEXT("workspace_load"),
                OutWorkspaceRoot.IsEmpty() ? TEXT("The directory workspace path is empty after normalization.") : TEXT("The workspace path does not resolve to an existing directory."));
            SetFailure(OutResult, MoveTemp(Diagnostic), &WorkspaceProvenance);
			return false;
		}

		struct FResolvedFile
		{
			FString AbsolutePath;
			FString RelativePath;
		};
		TArray<FResolvedFile> ResolvedFiles;
		TArray<FString> RelativePaths;
		OutResult.Enumeration.bPresent = true;
		const bool bEnumerationCompleted = IFileManager::Get().IterateDirectoryStatRecursively(
			*OutWorkspaceRoot,
			[&](const TCHAR* FilenameOrDirectory, const FFileStatData& StatData)
			{
			UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
			ResolveOptions.bRequireExists = true;
			ResolveOptions.bRequireDirectory = StatData.bIsDirectory;
			UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				OutWorkspaceRoot, FilenameOrDirectory, ResolveOptions, Resolved, ResolveError))
			{
				SetFailure(
					OutResult,
					ResolveError,
					&WorkspaceProvenance);
				return false;
			}
			if (StatData.bIsDirectory)
			{
				return true;
			}
			if (UeAgentExportPublisher::IsOwnershipMetadataPath(Resolved.RelativePath))
			{
				return true;
			}

			++OutResult.Enumeration.EntriesVisited;
			if (!StatData.bIsValid || StatData.FileSize < 0)
			{
                auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"),
                    !StatData.bIsValid ? TEXT("file_metadata/valid") : TEXT("file_metadata/size"),
                    !StatData.bIsValid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : MakeShared<FJsonValueString>(TEXT("nonnegative byte count")),
                    !StatData.bIsValid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : MakeShared<FJsonValueNumber>(static_cast<double>(StatData.FileSize)),
                    TEXT("workspace_load"), !StatData.bIsValid ? TEXT("Directory enumeration returned invalid metadata for this file.") : TEXT("Directory enumeration returned a negative file size."));
                Diagnostic.CandidateName = Resolved.RelativePath;
                Diagnostic.Source.SourceFile = Resolved.AbsolutePath;
                SetFailure(OutResult, MoveTemp(Diagnostic), &WorkspaceProvenance);
				return false;
			}
			OutResult.Enumeration.BytesVisited += StatData.FileSize;
			ResolvedFiles.Add({ Resolved.AbsolutePath, Resolved.RelativePath });
			RelativePaths.Add(Resolved.RelativePath);
			return true;
		});
		if (!bEnumerationCompleted)
		{
			if (OutResult.Diagnostic.IsEmpty())
			{
                SetFailure(OutResult, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("workspace_source_invalid"), TEXT("The workspace directory could not be enumerated."), TEXT("workspace_load"), OutWorkspaceRoot), &WorkspaceProvenance);
			}
			return false;
		}

		UE::UAI::Contract::FDiagnostic CollisionError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(OutWorkspaceRoot, RelativePaths, CollisionError))
		{
			SetFailure(OutResult, CollisionError, &WorkspaceProvenance);
			return false;
		}
		ResolvedFiles.Sort([](const FResolvedFile& Left, const FResolvedFile& Right)
		{
			const int32 Folded = Left.RelativePath.Compare(Right.RelativePath, ESearchCase::IgnoreCase);
			return Folded == 0
				? Left.RelativePath.Compare(Right.RelativePath, ESearchCase::CaseSensitive) < 0
				: Folded < 0;
		});

		int64 TotalBytes = 0;
		for (const FResolvedFile& File : ResolvedFiles)
		{
			FDocumentCandidate Candidate;
			if (!ReadFileCandidate(
				File.AbsolutePath,
				File.RelativePath,
				File.RelativePath,
				ESourceKind::Directory,
				OutWorkspaceRoot,
				File.RelativePath,
				false,
				TotalBytes,
				Candidate,
				OutResult)) return false;
			OutCandidates.Add(MoveTemp(Candidate));
		}

		UeAgentAuthoringWorkspacePaths::FResolveOptions EntryOptions;
		EntryOptions.bRequireExists = true;
		EntryOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath EntryPath;
		UE::UAI::Contract::FDiagnostic EntryError;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			OutWorkspaceRoot, Contract.Entry.DirectoryEntry, EntryOptions, EntryPath, EntryError))
		{
			FSourceProvenance EntryProvenance = WorkspaceProvenance;
			EntryProvenance.SourceFile = FPaths::Combine(OutWorkspaceRoot, Contract.Entry.DirectoryEntry);
			EntryProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
			EntryProvenance.RelativePath = Contract.Entry.DirectoryEntry;
			SetFailure(
				OutResult,
				EntryError,
				&EntryProvenance);
			return false;
		}
		return true;
	}

	bool LoadSingleFileCandidate(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		FString& OutWorkspaceRoot,
		TArray<FDocumentCandidate>& OutCandidates,
		FLoadResult& OutResult)
	{
		const FString AbsolutePath = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(Source.Path);
		OutWorkspaceRoot = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(FPaths::GetPath(AbsolutePath));
		FSourceProvenance SourceProvenance;
		SourceProvenance.SourceKind = ESourceKind::SingleFile;
		SourceProvenance.WorkspaceSource = AbsolutePath.IsEmpty() ? Source.Path : AbsolutePath;
		SourceProvenance.SourceFile = SourceProvenance.WorkspaceSource;
		SourceProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
		SourceProvenance.bVirtual = true;
		UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
		ResolveOptions.bRequireExists = true;
		ResolveOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
		UE::UAI::Contract::FDiagnostic ResolveError;
		if (AbsolutePath.IsEmpty()
			|| !UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				OutWorkspaceRoot, AbsolutePath, ResolveOptions, Resolved, ResolveError))
		{
			SetFailure(OutResult, ResolveError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The workspace source path is invalid."), FString(), Source.Path) : ResolveError, &SourceProvenance);
			return false;
		}

		int64 TotalBytes = 0;
		FDocumentCandidate Candidate;
		if (!ReadFileCandidate(
			Resolved.AbsolutePath,
			Contract.Entry.DirectoryEntry,
			Resolved.RelativePath,
			ESourceKind::SingleFile,
			Resolved.AbsolutePath,
			Contract.Entry.VirtualUnitId,
			true,
			TotalBytes,
			Candidate,
			OutResult)) return false;
		OutCandidates.Add(MoveTemp(Candidate));
		return true;
	}

	bool LoadInlineCandidate(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		TArray<FDocumentCandidate>& OutCandidates,
		FLoadResult& OutResult)
	{
        FDocumentCandidate Candidate;
        Candidate.RelativePath = Contract.Entry.DirectoryEntry;
        Candidate.bVirtual = true;
        Candidate.Provenance.SourceKind = ESourceKind::Inline;
        Candidate.Provenance.WorkspaceSource = Source.SourceLabel.IsEmpty() ? TEXT("inline://request") : Source.SourceLabel;
        Candidate.Provenance.SourceFile = Candidate.Provenance.WorkspaceSource;
        Candidate.Provenance.SourceUnit = Contract.Entry.VirtualUnitId;
        Candidate.Provenance.RelativePath = Contract.Entry.DirectoryEntry;
        Candidate.Provenance.bVirtual = true;
        const bool bHasObject = Source.InlineObject.IsValid();
        if (Source.bHasInlineHjson == bHasObject) {
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("inline_payload_count"),
                MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(static_cast<int32>(Source.bHasInlineHjson) + static_cast<int32>(bHasObject)),
                TEXT("workspace_load"), TEXT("Provide exactly one inline HJSON or object payload."));
            SetFailure(OutResult, MoveTemp(Diagnostic), &Candidate.Provenance);
            return false;
        }
		if (Source.bHasInlineHjson)
		{
			if (!ParseCandidateText(Source.InlineHjson, Source.SourceLabel, Candidate, OutResult)) return false;
		}
		else
		{
			FString CanonicalText;
			if (!SerializeInlineObject(Source.InlineObject, CanonicalText))
			{
				SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The inline workspace object could not be serialized."), TEXT("workspace_load"), TEXT("inline_object_serialize_failed")), &Candidate.Provenance);
				return false;
			}
			if (!ParseCandidateText(CanonicalText, Source.SourceLabel, Candidate, OutResult)) return false;
		}
		OutCandidates.Add(MoveTemp(Candidate));
		return true;
	}

	bool ExpandVirtualWorkspaceBundle(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		TArray<FDocumentCandidate>& InOutCandidates,
		FLoadResult& OutResult)
	{
		const UeAgentApplyWorkspaceContract::FSourceDescriptor* SourceDescriptor =
			Contract.FindSource(InOutCandidates.IsEmpty()
				? ESourceKind::Inline
				: InOutCandidates[0].Provenance.SourceKind);
		if (!SourceDescriptor || SourceDescriptor->EntryMode != TEXT("virtual_bundle")) return true;
		if (InOutCandidates.Num() != 1 || !InOutCandidates[0].Document.IsValid())
		{
            const bool bWrongCount = InOutCandidates.Num() != 1;
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), bWrongCount ? TEXT("document_count") : TEXT("document"),
                bWrongCount ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1)) : MakeShared<FJsonValueString>(TEXT("parsed document object")),
                bWrongCount ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(InOutCandidates.Num())) : nullptr,
                TEXT("workspace_load"), bWrongCount ? TEXT("A virtual workspace bundle requires exactly one document.") : TEXT("The virtual bundle has no parsed document object."));
            SetFailure(OutResult, MoveTemp(Diagnostic), InOutCandidates.IsEmpty() ? nullptr : &InOutCandidates[0].Provenance);
			return false;
		}

		const FDocumentCandidate Bundle = InOutCandidates[0];
		FString Schema;
		const TSharedPtr<FJsonObject>* Units = nullptr;
        UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit BundleSource;
        BundleSource.Provenance = Bundle.Provenance;
        BundleSource.SourceLocations = Bundle.SourceLocations;
        const auto BundleFailure = [&](UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Pointer) {
            if (Diagnostic.Source.SourceFile.IsEmpty()) Diagnostic.Source = BundleSource.ResolveDiagnosticSource(Pointer);
            SetFailure(OutResult, MoveTemp(Diagnostic), &Bundle.Provenance);
        };
        if (!Bundle.Document->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("uai.virtual_workspace.v1")) {
            BundleFailure(UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("/schema"),
                MakeShared<FJsonValueString>(TEXT("uai.virtual_workspace.v1")), Bundle.Document->Values.FindRef(TEXT("schema")),
                TEXT("workspace_load"), TEXT("The virtual workspace bundle schema is missing or unsupported.")), TEXT("/schema"));
            return false;
        }
        if (!Bundle.Document->TryGetObjectField(TEXT("units"), Units) || !Units || !Units->IsValid()) {
            BundleFailure(UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("/units"),
                MakeShared<FJsonValueString>(TEXT("object keyed by relative HJSON path")), Bundle.Document->Values.FindRef(TEXT("units")),
                TEXT("workspace_load"), TEXT("The virtual workspace bundle requires a units object.")), TEXT("/units"));
            return false;
        }

		const FString VirtualRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UAI/VirtualWorkspace")));
		TArray<FString> RelativePaths;
		TArray<FDocumentCandidate> Expanded;
        UeAgentAuthoringDiagnostics::FScopedSourceLocations BundleLocations(Bundle.Document.Get(), [&](const FString& Pointer) {
            return BundleSource.ResolveDiagnosticSource(Pointer);
        });
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Units)->Values)
		{
            FString EscapedKey = Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
            const FString UnitPointer = TEXT("/units/") + EscapedKey;
            const TSharedPtr<FJsonObject> UnitDocument = Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
            if (!UnitDocument) {
                BundleFailure(UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), UnitPointer,
                    MakeShared<FJsonValueString>(TEXT("object")), Pair.Value, TEXT("workspace_load"), TEXT("The virtual workspace unit document must be an object.")), UnitPointer);
                return false;
            }
			UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
			ResolveOptions.bRequireHjson = true;
			UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
			UE::UAI::Contract::FDiagnostic ResolveError;
            if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
					VirtualRoot, Pair.Key, ResolveOptions, Resolved, ResolveError))
			{
                if (ResolveError.IsEmpty()) ResolveError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"),
                    TEXT("Resolving the virtual unit path failed without reporting a diagnostic."), TEXT("workspace_load"));
                ResolveError.CandidateName = Pair.Key;
                BundleFailure(MoveTemp(ResolveError), UnitPointer);
				return false;
			}

			FDocumentCandidate Candidate;
			Candidate.AbsolutePath = Bundle.AbsolutePath;
			Candidate.RelativePath = Resolved.RelativePath;
			Candidate.bVirtual = true;
			Candidate.Document = UnitDocument;
            Candidate.ResolveOriginalSource = [Locations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*UnitDocument)](const FString& Pointer) {
                return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Locations, Pointer);
            };
			Candidate.bEmpty = UnitDocument->Values.IsEmpty();
			Candidate.Provenance = Bundle.Provenance;
			Candidate.Provenance.SourceUnit = Resolved.RelativePath;
			Candidate.Provenance.RelativePath = Resolved.RelativePath;
			Candidate.Provenance.bVirtual = true;
            const auto UnitSource = Candidate.ResolveOriginalSource(FString());
            if (!UnitSource.SourceFile.IsEmpty()) Candidate.Provenance.SourceFile = UnitSource.SourceFile;
            Candidate.Provenance.Line = UnitSource.Line;
            Candidate.Provenance.Column = UnitSource.Column;
			RelativePaths.Add(Resolved.RelativePath);
			Expanded.Add(MoveTemp(Candidate));
		}

		UE::UAI::Contract::FDiagnostic CollisionError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			VirtualRoot, RelativePaths, CollisionError))
		{
			SetFailure(OutResult, CollisionError, &Bundle.Provenance);
			return false;
		}
		Expanded.Sort([](const FDocumentCandidate& Left, const FDocumentCandidate& Right)
		{
			const int32 Folded = Left.RelativePath.Compare(Right.RelativePath, ESearchCase::IgnoreCase);
			return Folded == 0
				? Left.RelativePath.Compare(Right.RelativePath, ESearchCase::CaseSensitive) < 0
				: Folded < 0;
		});
		InOutCandidates = MoveTemp(Expanded);
		return true;
	}
}

namespace UeAgentAuthoringWorkspaceLoader
{
	FSourceRequest FSourceRequest::MakeDirectory(const FString& WorkspaceRoot)
	{
		FSourceRequest Result;
		Result.Kind = ESourceKind::Directory;
		Result.Path = WorkspaceRoot;
		return Result;
	}

	FSourceRequest FSourceRequest::MakeSingleFile(const FString& FilePath)
	{
		FSourceRequest Result;
		Result.Kind = ESourceKind::SingleFile;
		Result.Path = FilePath;
		return Result;
	}

	FSourceRequest FSourceRequest::MakeInlineHjson(const FString& Hjson, const FString& Label)
	{
		FSourceRequest Result;
		Result.Kind = ESourceKind::Inline;
		Result.InlineHjson = Hjson;
		Result.bHasInlineHjson = true;
		Result.SourceLabel = Label.IsEmpty() ? TEXT("inline://request") : Label;
		return Result;
	}

	FSourceRequest FSourceRequest::MakeInlineObject(
		const TSharedPtr<const FJsonObject>& Object,
		const FString& Label)
	{
		FSourceRequest Result;
		Result.Kind = ESourceKind::Inline;
		Result.InlineObject = Object;
		Result.SourceLabel = Label.IsEmpty() ? TEXT("inline://request") : Label;
		if (Object)
		{
			auto Locations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Object);
			TMap<FString, FString> UnitPrefixes;
			FString Schema;
			const TSharedPtr<FJsonObject>* Units = nullptr;
			if (Object->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("uai.virtual_workspace.v1")
				&& Object->TryGetObjectField(TEXT("units"), Units) && Units && *Units)
			{
				for (const auto& Pair : (*Units)->Values)
				{
					FString Path = Pair.Key;
					FPaths::NormalizeFilename(Path);
					FPaths::CollapseRelativeDirectories(Path);
					while (Path.StartsWith(TEXT("./"))) Path.RightChopInline(2);
					UnitPrefixes.Add(Path, TEXT("/units/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
				}
			}
			if (!Locations.IsEmpty())
				Result.ResolveSource = [Locations = MoveTemp(Locations), UnitPrefixes = MoveTemp(UnitPrefixes)](const FString& Path, const FString& Pointer)
				{
					FString Lookup = Pointer.IsEmpty() ? TEXT("/") : Pointer;
					if (const auto* Prefix = UnitPrefixes.Find(Path)) Lookup = *Prefix + (Lookup == TEXT("/") ? FString() : Lookup);
					const FString Requested = Lookup;
					for (;;)
					{
						if (const auto* Found = Locations.Find(Lookup))
						{
							auto Source = *Found;
							if (Lookup != Requested) Source.InternalJsonPath += Requested.Mid(Lookup == TEXT("/") ? 0 : Lookup.Len());
							return Source;
						}
						if (Lookup == TEXT("/")) break;
						int32 Slash = INDEX_NONE;
						Lookup.FindLastChar(TEXT('/'), Slash);
						Lookup = Slash > 0 ? Lookup.Left(Slash) : TEXT("/");
					}
					return UeAgentAuthoringDiagnostics::FSourceLocation();
				};
		}
		return Result;
	}

	bool FLoadResult::IsLoaded() const
	{
		return Status == TEXT("loaded") && Diagnostic.IsEmpty();
	}

	TSharedRef<FJsonObject> FLoadResult::FEnumerationReceipt::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("entries_visited"), EntriesVisited);
		Object->SetNumberField(TEXT("bytes_visited"), static_cast<double>(BytesVisited));
		return Object;
	}

	TSharedRef<FJsonObject> FLoadResult::ToJsonObject(bool bIncludeDocuments) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), IsLoaded() ? TEXT("workspace_loaded") : Diagnostic.Code);
		if (Enumeration.bPresent) Object->SetObjectField(TEXT("enumeration"), Enumeration.ToJsonObject());
		if (IsLoaded()) Object->SetObjectField(TEXT("workspace"), Workspace.ToJsonObject(bIncludeDocuments));
		else Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		return Object;
	}

	bool LoadWorkspace(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const FSourceRequest& Source,
		FLoadResult& OutResult)
	{
		OutResult = FLoadResult();
		ON_SCOPE_EXIT
		{
			if (!Source.ResolveSource || OutResult.Diagnostic.IsEmpty()) return;
			TFunction<void(UE::UAI::Contract::FDiagnostic&)> Locate = [&](auto& Diagnostic)
			{
				for (auto& Child : Diagnostic.Diagnostics) Locate(Child);
				if (Diagnostic.PublicCause)
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
					Locate(*Cause); Diagnostic.PublicCause = MoveTemp(Cause);
				}
                if (Diagnostic.SourceKind == TEXT("native_result") || Diagnostic.SourceKind == TEXT("command_input")
                    || Diagnostic.Source.bCommandInput) return;
                if (!Diagnostic.Source.SourceFile.IsEmpty() && !Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://"))) return;
				auto Original = Source.ResolveSource(Diagnostic.Source.SourceUnit,
					Diagnostic.Source.InternalJsonPath.IsEmpty() ? Diagnostic.Source.JsonPath : Diagnostic.Source.InternalJsonPath);
				if (!Original.SourceFile.IsEmpty())
				{
					Diagnostic.Source = MoveTemp(Original);
					Diagnostic.SourceKind = Diagnostic.Source.InternalJsonPath == Diagnostic.Source.JsonPath ? TEXT("authored_value") : TEXT("related_input");
				}
			};
			Locate(OutResult.Diagnostic);
		};
		FSourceProvenance RequestProvenance;
		RequestProvenance.SourceKind = Source.Kind;
		RequestProvenance.WorkspaceSource = Source.Kind == ESourceKind::Inline
			? (Source.SourceLabel.IsEmpty() ? TEXT("inline://request") : Source.SourceLabel)
			: Source.Path;
		RequestProvenance.SourceFile = RequestProvenance.WorkspaceSource;
		RequestProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
		RequestProvenance.bVirtual = Source.Kind != ESourceKind::Directory;
        UE::UAI::Contract::FDiagnostic ContractError;
        if (!UeAgentApplyWorkspaceContract::ValidateContract(Contract, ContractError))
        {
            SetFailure(OutResult, MoveTemp(ContractError));
			return false;
		}
		if (!Contract.FindSource(Source.Kind))
		{
            TArray<TSharedPtr<FJsonValue>> Allowed;
            for (const auto& Supported : Contract.Sources) Allowed.Add(MakeShared<FJsonValueString>(UeAgentApplyWorkspaceContract::SourceKindToString(Supported.Kind)));
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("source_kind"), MakeShared<FJsonValueArray>(Allowed),
                MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%d)"), *UeAgentApplyWorkspaceContract::SourceKindToString(Source.Kind), static_cast<int32>(Source.Kind))), TEXT("workspace_load"), TEXT("The source kind is not supported by this workspace contract."));
            SetFailure(OutResult, MoveTemp(Diagnostic), &RequestProvenance);
			return false;
		}

		FString WorkspaceRoot;
		TArray<FDocumentCandidate> Candidates;
		bool bLoaded = false;
		switch (Source.Kind)
		{
		case ESourceKind::Directory:
			if (Source.bHasInlineHjson || Source.InlineObject.IsValid())
			{
				SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("A directory workspace source cannot include an inline payload."), TEXT("workspace_load"), TEXT("directory_source_payload_conflict")), &RequestProvenance);
				return false;
			}
			bLoaded = LoadDirectoryCandidates(Contract, Source, WorkspaceRoot, Candidates, OutResult);
			break;
		case ESourceKind::SingleFile:
			if (Source.bHasInlineHjson || Source.InlineObject.IsValid())
			{
				SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("A file workspace source cannot include an inline payload."), TEXT("workspace_load"), TEXT("single_file_source_payload_conflict")), &RequestProvenance);
				return false;
			}
			bLoaded = LoadSingleFileCandidate(Contract, Source, WorkspaceRoot, Candidates, OutResult);
			break;
		case ESourceKind::Inline:
			if (!Source.Path.IsEmpty())
			{
				SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("An inline workspace source cannot also specify a path."), TEXT("workspace_load"), TEXT("inline_source_path_conflict")), &RequestProvenance);
				return false;
			}
			bLoaded = LoadInlineCandidate(Contract, Source, Candidates, OutResult);
			break;
		default:
			SetFailure(OutResult, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The workspace source kind is unknown."), TEXT("workspace_load"), TEXT("source_kind_unknown")), &RequestProvenance);
			return false;
		}
		if (!bLoaded || (Source.Kind != ESourceKind::Directory
			&& !ExpandVirtualWorkspaceBundle(Contract, Candidates, OutResult)))
		{
			return false;
		}

		UeAgentAuthoringWorkspaceRouter::FRouteFailure RouteFailure;
		if (!UeAgentAuthoringWorkspaceRouter::RouteDocuments(
			Contract, Source.Kind, WorkspaceRoot, Candidates, OutResult.Workspace, RouteFailure))
		{
			SetFailure(OutResult, RouteFailure.Diagnostic);
			return false;
		}

		if (Source.ResolveSource)
		{
			for (auto& Unit : OutResult.Workspace.Units)
			{
                Unit.ResolveOriginalSource = [Resolve = Source.ResolveSource, Path = Unit.RelativePath, Parsed = MoveTemp(Unit.ResolveOriginalSource)](const FString& Pointer)
                {
                    auto Original = Resolve(Path, Pointer);
                    if (!Original.SourceFile.IsEmpty()) return Original;
                    return Parsed ? Parsed(Pointer) : UeAgentAuthoringDiagnostics::FSourceLocation();
                };
			}
		}
		OutResult.Status = TEXT("loaded");
		OutResult.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic();
		return true;
	}
}
