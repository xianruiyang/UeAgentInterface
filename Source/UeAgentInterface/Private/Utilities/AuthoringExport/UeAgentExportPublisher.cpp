// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisherTestHooks.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	const TCHAR* LegacyOwnerMarker = TEXT(".uai-export-owner");
	const TCHAR* C8fOwnerMarker = TEXT(".uai-export-owner.json");
	const TCHAR* CompatibilityOwnerId =
		TEXT("uai.legacy.export.compatibility");

	UE::UAI::Contract::FDiagnostic PlanFailure(const FString& Code, const FString& Reason,
		const FString& Target, const FString& Path)
	{
		UE::UAI::Contract::FDiagnostic Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("export_publish"));
		Error.SourceKind = TEXT("native_result");
		Error.TargetPath = Target;
		Error.PropertyPath = Path;
		Error.SemanticPointer = Path;
		Error.Source.JsonPath = Path;
		return Error;
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Result;
		Result.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Result;
	}

	FString Sha256(const TArray<uint8>& Bytes)
	{
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		const uint8 Empty = 0;
		SHA256(
			Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
			static_cast<size_t>(Bytes.Num()),
			Digest);
		FString Hex;
		Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return TEXT("sha256:") + Hex;
	}

	FString HashText(const FString& Text)
	{
		return Sha256(Utf8(Text));
	}

	FString CanonicalizeLegacyRoot(FString Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("{"))
			|| Trimmed.StartsWith(TEXT("[")))
		{
			return Text;
		}
		if (!Text.EndsWith(TEXT("\n")))
		{
			Text += TEXT("\n");
		}
		return TEXT("{\n") + Text + TEXT("}\n");
	}

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker();
	}

	bool ResolveCompatibilityPath(
		const FString& Input,
		const bool bRequireHjson,
		const bool bAllowRoot,
		FString& OutPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringWorkspacePaths::FResolveOptions Options;
		Options.bRequireHjson = bRequireHjson;
		Options.bAllowRoot = bAllowRoot;
		UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
		UE::UAI::Contract::FDiagnostic SavedError;
		if (UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				FPaths::ProjectSavedDir(),
				Input,
				Options,
				Resolved,
				SavedError))
		{
			OutPath = Resolved.AbsolutePath;
			return true;
		}
#if WITH_DEV_AUTOMATION_TESTS
		const FString VerificationRoot = FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("runtimeLogs/uai_verification"));
		if (UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				VerificationRoot,
				Input,
				Options,
				Resolved,
				OutError))
		{
			OutPath = Resolved.AbsolutePath;
			return true;
		}
#endif
		OutError = SavedError;
		return false;
	}

	bool BuildCompatibilityPlan(
		const TArray<UeAgentExportPublisher::FFilePlan>& Files,
		const UeAgentExportPublisher::FPublishContext& Context,
		const EWorkspaceKind WorkspaceKind,
		const FString& EntryFile,
		const FString& OutputPath,
		FExportWorkspacePlanPtr& OutPlan,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPlan.Reset();
		if (Files.IsEmpty())
		{
			OutError = PlanFailure(TEXT("export_publish_no_files"),
				TEXT("The export plan contains no files to publish."), OutputPath, TEXT("/files"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Files.Num());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one file"));
			return false;
		}
		TArray<FExportFilePlan> PlannedFiles;
		FString FingerprintMaterial =
			Context.ContractId + TEXT("\x1f")
			+ Context.ContractVersion + TEXT("\x1f")
			+ Context.ProfileId;
		int64 ObservedTokens = 0;
		for (int32 FileIndex = 0; FileIndex < Files.Num(); ++FileIndex)
		{
			const UeAgentExportPublisher::FFilePlan& File = Files[FileIndex];
			if (!File.Root.IsValid()
				|| File.RelativePath.TrimStartAndEnd().IsEmpty())
			{
				OutError = PlanFailure(TEXT("export_publish_file_invalid"),
					!File.Root.IsValid() ? TEXT("The planned export file has no document object.")
						: TEXT("The planned export file has an empty relative path."), OutputPath,
					FString::Printf(TEXT("/files/%d/%s"), FileIndex, !File.Root.IsValid() ? TEXT("root") : TEXT("relative_path")));
				OutError.Source.SourceUnit = File.RelativePath;
				if (!File.Root.IsValid())
				{
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a document object"));
				}
				else
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(File.RelativePath);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty relative file path"));
				}
				return false;
			}
			const FString Text = CanonicalizeLegacyRoot(
				UeAgentStructuredAuthoringIO::WriteObject(
					File.Root.ToSharedRef(),
					File.WriteOptions));
			TArray<uint8> Bytes = Utf8(Text);
			const FString ContentHash = Sha256(Bytes);
			ObservedTokens += (Bytes.Num() + 3ll) / 4ll;
			FingerprintMaterial += TEXT("\x1f")
				+ File.RelativePath
				+ TEXT("\x1f")
				+ ContentHash;
			PlannedFiles.Emplace(
				File.RelativePath,
				MoveTemp(Bytes),
				ContentHash);
			if (UeAgentExportPublisherTestHooks::ShouldFail(
					UeAgentExportPublisherTestHooks::EFailurePoint::
						NthStagedFile))
			{
				OutError = PlanFailure(TEXT("export_publisher_injected_stage_failure"),
					TEXT("The export staging failure hook rejected this planned file."), OutputPath,
					FString::Printf(TEXT("/files/%d"), FileIndex));
				OutError.Source.SourceUnit = File.RelativePath;
				return false;
			}
		}
		const FString Fingerprint = HashText(FingerprintMaterial);
		const FString Generation =
			Context.ContractId + TEXT(":")
			+ Context.ContractVersion + TEXT(":")
			+ Context.ProfileId;
		OutPlan = MakeShared<FExportWorkspacePlan>(
			WorkspaceKind,
			EntryFile,
			TEXT("legacy-export-compat:") + Fingerprint,
			FUeAgentStateRevision(
				TEXT("legacy-export-compat"),
				Generation,
				1),
			TEXT("legacy_projection_compatibility_not_c4"),
			MoveTemp(PlannedFiles),
			Fingerprint,
			ObservedTokens);
		return true;
	}

	TSharedRef<FJsonObject> ReceiptJson(
		const FArtifactPublishReceipt& Receipt)
	{
		const TSharedRef<FJsonObject> Result =
			MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("published"));
		Result->SetStringField(
			TEXT("publisher_owner_id"),
			Receipt.GetPublisherOwnerId());
		Result->SetStringField(
			TEXT("transaction_id"),
			Receipt.GetTransactionId());
		Result->SetStringField(
			TEXT("final_path"),
			Receipt.GetFinalPath());
		Result->SetStringField(
			TEXT("workspace_kind"),
			Receipt.GetWorkspaceKind() == EWorkspaceKind::SingleFile
				? TEXT("single_file")
				: TEXT("directory"));
		Result->SetStringField(
			TEXT("entry_file"),
			Receipt.GetEntryFile());
		Result->SetStringField(
			TEXT("plan_fingerprint"),
			Receipt.GetPlanFingerprint());
		Result->SetStringField(
			TEXT("snapshot_id"),
			Receipt.GetSnapshotId());
		Result->SetStringField(
			TEXT("snapshot_evidence_id"),
			Receipt.GetSnapshotEvidenceId());
		Result->SetStringField(
			TEXT("inventory_hash"),
			Receipt.GetInventoryHash());
		if (!Receipt.GetCleanupWarnings().IsEmpty()) {
			TArray<TSharedPtr<FJsonValue>> Warnings;
			for (const auto& Warning : Receipt.GetCleanupWarnings())
				Warnings.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
			Result->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
		}
		return Result;
	}

	bool PublishPlan(
		const FExportWorkspacePlanRef& Plan,
		const FString& FinalPath,
		UeAgentExportPublisher::FPublishResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TUniquePtr<
			UE::UAI::ArtifactPublisher::TestHooks::
				FScopedPublishFailure> Failure;
		const bool bInjectedAfterBackup =
			UeAgentExportPublisherTestHooks::ShouldFail(
				UeAgentExportPublisherTestHooks::EFailurePoint::
					AfterFinalMovedToBackup);
		if (bInjectedAfterBackup)
		{
			Failure = MakeUnique<
				UE::UAI::ArtifactPublisher::TestHooks::
					FScopedPublishFailure>(
					UE::UAI::ArtifactPublisher::TestHooks::
						EFailurePoint::AfterFinalMovedToBackup);
		}
		FUeAgentResourceTracker Resources = MakeResources();
		const FArtifactPublishRequest Request(
			Plan,
			FinalPath,
			CompatibilityOwnerId);
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError Error;
		if (!TryPublishExportPlan(
				Request,
				Resources,
				Receipt,
				Error))
		{
			OutError = Error;
			UE_LOG(
				LogUeAgentInterface,
				Warning,
				TEXT(
					"Legacy export compatibility publish failed "
					"code=%s path=%s detail=%s"),
				*Error.Code,
				*Error.TargetPath,
				*Error.InternalContext);
			return false;
		}
		OutResult.Receipt = ReceiptJson(Receipt);
		OutResult.WrittenRelativeFiles = Receipt.GetIncludedFiles();
		for (const FString& Relative : Receipt.GetIncludedFiles())
		{
			OutResult.WrittenFiles.Add(
				Plan->GetWorkspaceKind() == EWorkspaceKind::SingleFile
					? Receipt.GetFinalPath()
					: FPaths::Combine(
						Receipt.GetFinalPath(),
						Relative));
		}
		return true;
	}

	bool LoadJson(
		const FString& Path,
		const FString& Target,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = PlanFailure(TEXT("export_owner_marker_read_failed"),
				TEXT("The export ownership record could not be read."), Target, FString());
			OutError.Source.SourceUnit = C8fOwnerMarker;
			OutError.InternalContext = Path;
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = PlanFailure(TEXT("export_owner_marker_parse_failed"),
				!Reader->GetErrorMessage().IsEmpty() ? Reader->GetErrorMessage()
					: TEXT("The export ownership record does not contain a JSON object."), Target, FString());
			OutError.Source.SourceUnit = C8fOwnerMarker;
			OutError.Source.Line = static_cast<int32>(Reader->GetLineNumber());
			OutError.Source.Column = static_cast<int32>(Reader->GetCharacterNumber());
			OutError.InternalContext = Path;
			return false;
		}
		return true;
	}

	bool HasCompatibleOwnership(const FString& Root, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (Root.Contains(
				TEXT("/Automation/UeAgentExportWorkspaceWriter"),
				ESearchCase::IgnoreCase))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Owner;
		FString PublisherOwner;
		UE::UAI::Contract::FDiagnostic CurrentError;
		if (LoadJson(
				FPaths::Combine(Root, C8fOwnerMarker),
				Target, Owner, CurrentError))
		{
			if (Owner->TryGetStringField(TEXT("publisher_owner_id"), PublisherOwner))
			{
				if (PublisherOwner == CompatibilityOwnerId) return true;
				OutError = PlanFailure(TEXT("export_workspace_ownership_required"),
					TEXT("The existing export workspace belongs to a different publisher."), Target, TEXT("/publisher_owner_id"));
				OutError.Source.SourceUnit = C8fOwnerMarker;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(PublisherOwner);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CompatibilityOwnerId);
				OutError.InternalContext = Root;
				return false;
			}
			CurrentError = PlanFailure(TEXT("export_owner_marker_field_invalid"),
				TEXT("The export ownership record has no string publisher identifier."), Target, TEXT("/publisher_owner_id"));
			CurrentError.Source.SourceUnit = C8fOwnerMarker;
			CurrentError.ActualValuePresent = Owner->Values.Contains(TEXT("publisher_owner_id"));
			CurrentError.ActualEvidence = Owner->Values.FindRef(TEXT("publisher_owner_id"));
			CurrentError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a string"));
		}
		FString LegacyText;
		if (FFileHelper::LoadFileToString(
			LegacyText,
			*FPaths::Combine(Root, LegacyOwnerMarker))) return true;
		OutError = PlanFailure(TEXT("export_workspace_ownership_required"),
			TEXT("No readable compatible ownership record was found for the export workspace."), Target, FString());
		OutError.InternalContext = Root;
		OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(CurrentError);
		auto LegacyError = PlanFailure(TEXT("export_legacy_owner_marker_read_failed"),
			TEXT("The legacy export ownership record could not be read."), Target, FString());
		LegacyError.Source.SourceUnit = LegacyOwnerMarker;
		LegacyError.InternalContext = FPaths::Combine(Root, LegacyOwnerMarker);
		OutError.Append(LegacyError);
		return false;
	}
}

namespace UeAgentExportPublisher
{
	bool IsOwnershipMetadataPath(const FString& RelativePath)
	{
		const FString Name = FPaths::GetCleanFilename(RelativePath);
		return Name.Equals(LegacyOwnerMarker, ESearchCase::IgnoreCase)
			|| Name.Equals(C8fOwnerMarker, ESearchCase::IgnoreCase);
	}

	bool PublishWorkspace(
		const FString& WorkspaceRoot,
		const TArray<FFilePlan>& Files,
		const FPublishContext& Context,
		FPublishResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = {};
		FString ResolvedRoot;
		if (!ResolveCompatibilityPath(
				WorkspaceRoot,
				false,
				false,
				ResolvedRoot,
				OutError))
		{
			return false;
		}
		FExportWorkspacePlanPtr Plan;
		const FString Entry =
			Files.IsEmpty() ? FString() : Files[0].RelativePath;
		if (!BuildCompatibilityPlan(
				Files,
				Context,
				EWorkspaceKind::Directory,
				Entry,
				WorkspaceRoot,
				Plan,
				OutError))
		{
			return false;
		}
		return PublishPlan(
			Plan.ToSharedRef(),
			ResolvedRoot,
			OutResult,
			OutError);
	}

	bool PublishSingleFile(
		const FString& OutputFile,
		const FFilePlan& File,
		const FPublishContext& Context,
		FPublishResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = {};
		FString ResolvedFile;
		if (!ResolveCompatibilityPath(
				OutputFile,
				true,
				false,
				ResolvedFile,
				OutError))
		{
			return false;
		}
		FFilePlan CanonicalFile = File;
		CanonicalFile.RelativePath =
			FPaths::GetCleanFilename(ResolvedFile);
		FExportWorkspacePlanPtr Plan;
		if (!BuildCompatibilityPlan(
				{CanonicalFile},
				Context,
				EWorkspaceKind::SingleFile,
				CanonicalFile.RelativePath,
				OutputFile,
				Plan,
				OutError))
		{
			return false;
		}
		return PublishPlan(
			Plan.ToSharedRef(),
			ResolvedFile,
			OutResult,
			OutError);
	}

	bool ResetOwnedWorkspace(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResolvedFolderPath.Reset();
		if (!ResolveCompatibilityPath(
				FolderPathInput,
				false,
				false,
				OutResolvedFolderPath,
				OutError))
		{
			return false;
		}
		if (!IFileManager::Get().DirectoryExists(
				*OutResolvedFolderPath))
		{
			return true;
		}
		if (!HasCompatibleOwnership(OutResolvedFolderPath, FolderPathInput, OutError))
		{
			return false;
		}
		if (!IFileManager::Get().DeleteDirectory(
				*OutResolvedFolderPath,
				false,
				true))
		{
			OutError = PlanFailure(TEXT("export_workspace_delete_failed"),
				TEXT("The owned export workspace directory could not be deleted."), FolderPathInput, FString());
			OutError.InternalContext = OutResolvedFolderPath;
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(
			*OutResolvedFolderPath,
			true))
		{
			OutError = PlanFailure(TEXT("export_workspace_create_failed"),
				TEXT("The export workspace directory was deleted but could not be recreated."), FolderPathInput, FString());
			OutError.InternalContext = OutResolvedFolderPath;
			return false;
		}
		return true;
	}
}
