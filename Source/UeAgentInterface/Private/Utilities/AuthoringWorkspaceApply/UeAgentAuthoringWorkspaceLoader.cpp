// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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

	void SetFailure(
		FLoadResult& OutResult,
		const FString& ErrorCode,
		const FString& ErrorDetail,
		const FSourceProvenance* Provenance = nullptr)
	{
		OutResult.Status = TEXT("failed");
		OutResult.ErrorCode = ErrorCode;
		OutResult.ErrorDetail = ErrorDetail;
		OutResult.Diagnostic.Code = ErrorCode;
		OutResult.Diagnostic.Stage = TEXT("workspace_load");
		OutResult.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		OutResult.Diagnostic.bRetrySafe = true;
		OutResult.Diagnostic.Message = ErrorDetail;
		OutResult.Diagnostic.SuggestedFix = TEXT("Correct the Apply workspace source or contract and retry preflight.");
		if (Provenance) OutResult.Diagnostic.Source = Provenance->ToDiagnosticSource();
	}

	FString ResolveFailureCode(const FString& Error, const TCHAR* Fallback)
	{
		if (Error.StartsWith(TEXT("workspace_path_escape:"))) return TEXT("workspace_path_escape");
		if (Error.StartsWith(TEXT("workspace_reparse_escape:"))) return TEXT("workspace_reparse_escape");
		if (Error.StartsWith(TEXT("workspace_reparse_not_allowed:"))) return TEXT("workspace_reparse_not_allowed");
		if (Error.StartsWith(TEXT("workspace_reparse_resolution_failed:"))) return TEXT("workspace_reparse_resolution_failed");
		if (Error.StartsWith(TEXT("workspace_entry_missing:"))) return TEXT("workspace_entry_missing");
		return Fallback;
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
		FString ParseError;
		if (!FUeAgentStructuredHjsonParser::ParseDocument(Text, ParseSource, Document, ParseError))
		{
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("hjson_parse_failed:") + ParseError, &InOutCandidate.Provenance);
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
		const UeAgentAuthoringWorkspaceLoader::FLoadLimits& Limits,
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
			SetFailure(OutResult, TEXT("workspace_file_unknown"), ActualRelativePath, &OutCandidate.Provenance);
			return false;
		}
		const int64 FileBytes = IFileManager::Get().FileSize(*AbsolutePath);
		if (FileBytes < 0)
		{
			SetFailure(OutResult, TEXT("workspace_entry_missing"), AbsolutePath, &OutCandidate.Provenance);
			return false;
		}
		if (FileBytes > Limits.MaxFileBytes || InOutTotalBytes > Limits.MaxTotalBytes - FileBytes)
		{
			SetFailure(OutResult, TEXT("workspace_preflight_failed"), TEXT("workspace_size_limit_exceeded:") + ActualRelativePath, &OutCandidate.Provenance);
			return false;
		}
		InOutTotalBytes += FileBytes;

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *AbsolutePath))
		{
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("workspace_file_read_failed:") + ActualRelativePath, &OutCandidate.Provenance);
			return false;
		}
		return ParseCandidateText(Text, AbsolutePath, OutCandidate, OutResult);
	}

	bool LoadDirectoryCandidates(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const UeAgentAuthoringWorkspaceLoader::FLoadLimits& Limits,
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
			SetFailure(OutResult, TEXT("workspace_source_invalid"), Source.Path, &WorkspaceProvenance);
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
			FString ResolveError;
			if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				OutWorkspaceRoot, FilenameOrDirectory, ResolveOptions, Resolved, ResolveError))
			{
				SetFailure(
					OutResult,
					ResolveFailureCode(ResolveError, TEXT("workspace_source_invalid")),
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
			if (OutResult.Enumeration.EntriesVisited > Limits.MaxFiles)
			{
				OutResult.Enumeration.bStoppedEarly = true;
				SetFailure(
					OutResult,
					TEXT("workspace_preflight_failed"),
					TEXT("workspace_file_limit_exceeded"),
					&WorkspaceProvenance);
				return false;
			}
			if (!StatData.bIsValid || StatData.FileSize < 0)
			{
				SetFailure(
					OutResult,
					TEXT("workspace_source_invalid"),
					TEXT("workspace_file_stat_failed:") + Resolved.RelativePath,
					&WorkspaceProvenance);
				return false;
			}
			if (StatData.FileSize > Limits.MaxFileBytes
				|| OutResult.Enumeration.BytesVisited > Limits.MaxTotalBytes - StatData.FileSize)
			{
				OutResult.Enumeration.bStoppedEarly = true;
				SetFailure(
					OutResult,
					TEXT("workspace_preflight_failed"),
					TEXT("workspace_size_limit_exceeded:") + Resolved.RelativePath,
					&WorkspaceProvenance);
				return false;
			}
			OutResult.Enumeration.BytesVisited += StatData.FileSize;
			ResolvedFiles.Add({ Resolved.AbsolutePath, Resolved.RelativePath });
			RelativePaths.Add(Resolved.RelativePath);
			return true;
		});
		if (!bEnumerationCompleted)
		{
			if (OutResult.ErrorCode.IsEmpty())
			{
				SetFailure(
					OutResult,
					TEXT("workspace_source_invalid"),
					TEXT("workspace_enumeration_failed"),
					&WorkspaceProvenance);
			}
			return false;
		}

		FString CollisionError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(OutWorkspaceRoot, RelativePaths, CollisionError))
		{
			SetFailure(OutResult, CollisionError.StartsWith(TEXT("workspace_path_case_collision:"))
				? TEXT("workspace_path_case_collision") : TEXT("workspace_duplicate_target"), CollisionError, &WorkspaceProvenance);
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
				Limits,
				TotalBytes,
				Candidate,
				OutResult)) return false;
			OutCandidates.Add(MoveTemp(Candidate));
		}

		UeAgentAuthoringWorkspacePaths::FResolveOptions EntryOptions;
		EntryOptions.bRequireExists = true;
		EntryOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath EntryPath;
		FString EntryError;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			OutWorkspaceRoot, Contract.Entry.DirectoryEntry, EntryOptions, EntryPath, EntryError))
		{
			FSourceProvenance EntryProvenance = WorkspaceProvenance;
			EntryProvenance.SourceFile = FPaths::Combine(OutWorkspaceRoot, Contract.Entry.DirectoryEntry);
			EntryProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
			EntryProvenance.RelativePath = Contract.Entry.DirectoryEntry;
			SetFailure(
				OutResult,
				ResolveFailureCode(EntryError, TEXT("workspace_entry_missing")),
				EntryError,
				&EntryProvenance);
			return false;
		}
		return true;
	}

	bool LoadSingleFileCandidate(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const UeAgentAuthoringWorkspaceLoader::FLoadLimits& Limits,
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
		FString ResolveError;
		if (AbsolutePath.IsEmpty()
			|| !UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				OutWorkspaceRoot, AbsolutePath, ResolveOptions, Resolved, ResolveError))
		{
			const FString Code = ResolveFailureCode(ResolveError, TEXT("workspace_source_invalid"));
			SetFailure(OutResult, Code, ResolveError.IsEmpty() ? Source.Path : ResolveError, &SourceProvenance);
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
			Limits,
			TotalBytes,
			Candidate,
			OutResult)) return false;
		OutCandidates.Add(MoveTemp(Candidate));
		return true;
	}

	bool LoadInlineCandidate(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const UeAgentAuthoringWorkspaceLoader::FLoadLimits& Limits,
		TArray<FDocumentCandidate>& OutCandidates,
		FLoadResult& OutResult)
	{
		const bool bHasObject = Source.InlineObject.IsValid();
		if (Source.bHasInlineHjson == bHasObject)
		{
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("inline_requires_exactly_one_payload"));
			return false;
		}

		FDocumentCandidate Candidate;
		Candidate.RelativePath = Contract.Entry.DirectoryEntry;
		Candidate.bVirtual = true;
		Candidate.Provenance.SourceKind = ESourceKind::Inline;
		Candidate.Provenance.WorkspaceSource = Source.SourceLabel;
		Candidate.Provenance.SourceFile = Source.SourceLabel;
		Candidate.Provenance.SourceUnit = Contract.Entry.VirtualUnitId;
		Candidate.Provenance.RelativePath = Contract.Entry.DirectoryEntry;
		Candidate.Provenance.bVirtual = true;
		if (Source.bHasInlineHjson)
		{
			if (Utf8ByteCount(Source.InlineHjson) > Limits.MaxFileBytes
				|| Utf8ByteCount(Source.InlineHjson) > Limits.MaxTotalBytes)
			{
				SetFailure(OutResult, TEXT("workspace_preflight_failed"), TEXT("workspace_size_limit_exceeded:inline"), &Candidate.Provenance);
				return false;
			}
			if (!ParseCandidateText(Source.InlineHjson, Source.SourceLabel, Candidate, OutResult)) return false;
		}
		else
		{
			FString CanonicalText;
			if (!SerializeInlineObject(Source.InlineObject, CanonicalText))
			{
				SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("inline_object_serialize_failed"), &Candidate.Provenance);
				return false;
			}
			const int64 ObjectBytes = Utf8ByteCount(CanonicalText);
			if (ObjectBytes > Limits.MaxFileBytes || ObjectBytes > Limits.MaxTotalBytes)
			{
				SetFailure(OutResult, TEXT("workspace_preflight_failed"), TEXT("workspace_size_limit_exceeded:inline_object"), &Candidate.Provenance);
				return false;
			}
			if (!ParseCandidateText(CanonicalText, Source.SourceLabel, Candidate, OutResult)) return false;
		}
		OutCandidates.Add(MoveTemp(Candidate));
		return true;
	}

	bool ExpandVirtualWorkspaceBundle(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceLoader::FLoadLimits& Limits,
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
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("virtual_workspace_bundle_requires_one_document"));
			return false;
		}

		const FDocumentCandidate Bundle = InOutCandidates[0];
		FString Schema;
		const TSharedPtr<FJsonObject>* Units = nullptr;
		if (!Bundle.Document->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != TEXT("uai.virtual_workspace.v1")
			|| !Bundle.Document->TryGetObjectField(TEXT("units"), Units)
			|| !Units || !Units->IsValid())
		{
			SetFailure(
				OutResult,
				TEXT("workspace_source_invalid"),
				TEXT("virtual_workspace_bundle_schema_invalid"),
				&Bundle.Provenance);
			return false;
		}
		if ((*Units)->Values.Num() > Limits.MaxFiles)
		{
			SetFailure(
				OutResult,
				TEXT("workspace_preflight_failed"),
				TEXT("workspace_file_limit_exceeded"),
				&Bundle.Provenance);
			return false;
		}

		const FString VirtualRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UAI/VirtualWorkspace")));
		TArray<FString> RelativePaths;
		TArray<FDocumentCandidate> Expanded;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Units)->Values)
		{
			const TSharedPtr<FJsonObject> UnitDocument = Pair.Value.IsValid()
				? Pair.Value->AsObject()
				: nullptr;
			UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
			ResolveOptions.bRequireHjson = true;
			UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
			FString ResolveError;
			if (!UnitDocument.IsValid()
				|| !UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
					VirtualRoot, Pair.Key, ResolveOptions, Resolved, ResolveError))
			{
				SetFailure(
					OutResult,
					ResolveFailureCode(ResolveError, TEXT("workspace_source_invalid")),
					ResolveError.IsEmpty()
						? TEXT("virtual_workspace_unit_invalid:") + Pair.Key
						: ResolveError,
					&Bundle.Provenance);
				return false;
			}

			FDocumentCandidate Candidate;
			Candidate.AbsolutePath = Bundle.AbsolutePath;
			Candidate.RelativePath = Resolved.RelativePath;
			Candidate.bVirtual = true;
			Candidate.Document = UnitDocument;
			Candidate.bEmpty = UnitDocument->Values.IsEmpty();
			Candidate.Provenance = Bundle.Provenance;
			Candidate.Provenance.SourceUnit = Resolved.RelativePath;
			Candidate.Provenance.RelativePath = Resolved.RelativePath;
			Candidate.Provenance.bVirtual = true;
			RelativePaths.Add(Resolved.RelativePath);
			Expanded.Add(MoveTemp(Candidate));
		}

		FString CollisionError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			VirtualRoot, RelativePaths, CollisionError))
		{
			SetFailure(
				OutResult,
				CollisionError.StartsWith(TEXT("workspace_path_case_collision:"))
					? TEXT("workspace_path_case_collision")
					: TEXT("workspace_duplicate_target"),
				CollisionError,
				&Bundle.Provenance);
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
		return Result;
	}

	bool FLoadResult::IsLoaded() const
	{
		return Status == TEXT("loaded") && ErrorCode == TEXT("workspace_loaded");
	}

	TSharedRef<FJsonObject> FLoadResult::FEnumerationReceipt::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("entries_visited"), EntriesVisited);
		Object->SetNumberField(TEXT("bytes_visited"), static_cast<double>(BytesVisited));
		Object->SetBoolField(TEXT("stopped_early"), bStoppedEarly);
		return Object;
	}

	TSharedRef<FJsonObject> FLoadResult::ToJsonObject(bool bIncludeDocuments) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		if (Enumeration.bPresent) Object->SetObjectField(TEXT("enumeration"), Enumeration.ToJsonObject());
		if (IsLoaded()) Object->SetObjectField(TEXT("workspace"), Workspace.ToJsonObject(bIncludeDocuments));
		else Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
		return Object;
	}

	bool LoadWorkspace(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const FSourceRequest& Source,
		FLoadResult& OutResult,
		const FLoadLimits& Limits)
	{
		OutResult = FLoadResult();
		FSourceProvenance RequestProvenance;
		RequestProvenance.SourceKind = Source.Kind;
		RequestProvenance.WorkspaceSource = Source.Kind == ESourceKind::Inline
			? (Source.SourceLabel.IsEmpty() ? TEXT("inline://request") : Source.SourceLabel)
			: Source.Path;
		RequestProvenance.SourceFile = RequestProvenance.WorkspaceSource;
		RequestProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
		RequestProvenance.bVirtual = Source.Kind != ESourceKind::Directory;
		if (!Contract.IsValid())
		{
			SetFailure(OutResult, TEXT("apply_workspace_contract_invalid"), Contract.ContractId);
			return false;
		}
		if (!Contract.FindSource(Source.Kind))
		{
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("source_kind_not_supported:") + UeAgentApplyWorkspaceContract::SourceKindToString(Source.Kind), &RequestProvenance);
			return false;
		}
		if (Limits.MaxFiles <= 0 || Limits.MaxFileBytes < 0 || Limits.MaxTotalBytes < 0)
		{
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("load_limits_invalid"), &RequestProvenance);
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
				SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("directory_source_payload_conflict"), &RequestProvenance);
				return false;
			}
			bLoaded = LoadDirectoryCandidates(Contract, Source, Limits, WorkspaceRoot, Candidates, OutResult);
			break;
		case ESourceKind::SingleFile:
			if (Source.bHasInlineHjson || Source.InlineObject.IsValid())
			{
				SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("single_file_source_payload_conflict"), &RequestProvenance);
				return false;
			}
			bLoaded = LoadSingleFileCandidate(Contract, Source, Limits, WorkspaceRoot, Candidates, OutResult);
			break;
		case ESourceKind::Inline:
			if (!Source.Path.IsEmpty())
			{
				SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("inline_source_path_conflict"), &RequestProvenance);
				return false;
			}
			bLoaded = LoadInlineCandidate(Contract, Source, Limits, Candidates, OutResult);
			break;
		default:
			SetFailure(OutResult, TEXT("workspace_source_invalid"), TEXT("source_kind_unknown"), &RequestProvenance);
			return false;
		}
		if (!bLoaded || (Source.Kind != ESourceKind::Directory
			&& !ExpandVirtualWorkspaceBundle(Contract, Limits, Candidates, OutResult)))
		{
			return false;
		}

		UeAgentAuthoringWorkspaceRouter::FRouteFailure RouteFailure;
		if (!UeAgentAuthoringWorkspaceRouter::RouteDocuments(
			Contract, Source.Kind, WorkspaceRoot, Candidates, OutResult.Workspace, RouteFailure))
		{
			SetFailure(OutResult, RouteFailure.ErrorCode, RouteFailure.ErrorDetail, &RouteFailure.Provenance);
			return false;
		}

		OutResult.Status = TEXT("loaded");
		OutResult.ErrorCode = TEXT("workspace_loaded");
		OutResult.ErrorDetail.Reset();
		OutResult.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic();
		return true;
	}
}
