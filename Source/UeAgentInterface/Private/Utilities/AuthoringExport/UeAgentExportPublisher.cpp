// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Dom/JsonObject.h"
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

	void SetError(
		FString& OutCode,
		FString& OutDetail,
		const FString& Code,
		const FString& Detail = FString())
	{
		OutCode = Code;
		OutDetail = Detail.IsEmpty() ? Code : Detail;
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
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 64 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 64 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 64 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 262144},
			{EUeAgentResourceDimension::Items, 262144},
			{EUeAgentResourceDimension::Files, 4096},
			{EUeAgentResourceDimension::ReferenceEdges, 262144},
			{EUeAgentResourceDimension::WorkUnits, 1048576},
			{
				EUeAgentResourceDimension::EstimatedMemoryBytes,
				256 * 1024 * 1024,
			},
		}));
	}

	bool ResolveCompatibilityPath(
		const FString& Input,
		const bool bRequireHjson,
		const bool bAllowRoot,
		FString& OutPath,
		FString& OutError)
	{
		UeAgentAuthoringWorkspacePaths::FResolveOptions Options;
		Options.bRequireHjson = bRequireHjson;
		Options.bAllowRoot = bAllowRoot;
		UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
		FString SavedError;
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
		FExportWorkspacePlanPtr& OutPlan,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutPlan.Reset();
		if (Files.IsEmpty())
		{
			SetError(
				OutErrorCode,
				OutErrorDetail,
				TEXT("export_publish_no_files"));
			return false;
		}
		TArray<FExportFilePlan> PlannedFiles;
		FString FingerprintMaterial =
			Context.ContractId + TEXT("\x1f")
			+ Context.ContractVersion + TEXT("\x1f")
			+ Context.ProfileId;
		int64 ObservedTokens = 0;
		for (const UeAgentExportPublisher::FFilePlan& File : Files)
		{
			if (!File.Root.IsValid()
				|| File.RelativePath.TrimStartAndEnd().IsEmpty())
			{
				SetError(
					OutErrorCode,
					OutErrorDetail,
					TEXT("export_publish_file_invalid"),
					File.RelativePath);
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
				SetError(
					OutErrorCode,
					OutErrorDetail,
					TEXT("export_publisher_injected_stage_failure"),
					File.RelativePath);
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
		return Result;
	}

	bool PublishPlan(
		const FExportWorkspacePlanRef& Plan,
		const FString& FinalPath,
		UeAgentExportPublisher::FPublishResult& OutResult,
		FString& OutErrorCode,
		FString& OutErrorDetail)
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
			SetError(
				OutErrorCode,
				OutErrorDetail,
				bInjectedAfterBackup
					? TEXT("export_publish_failed")
					: Error.GetCode(),
				Error.GetPath().IsEmpty()
					? Error.GetDetail()
					: Error.GetPath() + TEXT(":") + Error.GetDetail());
			UE_LOG(
				LogUeAgentInterface,
				Warning,
				TEXT(
					"Legacy export compatibility publish failed "
					"code=%s path=%s detail=%s"),
				*Error.GetCode(),
				*Error.GetPath(),
				*Error.GetDetail());
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
		TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject)
			&& OutObject.IsValid();
	}

	bool HasCompatibleOwnership(const FString& Root)
	{
		if (Root.Contains(
				TEXT("/Automation/UeAgentExportWorkspaceWriter"),
				ESearchCase::IgnoreCase))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Owner;
		FString PublisherOwner;
		if (LoadJson(
				FPaths::Combine(Root, C8fOwnerMarker),
				Owner)
			&& Owner->TryGetStringField(
				TEXT("publisher_owner_id"),
				PublisherOwner))
		{
			return PublisherOwner == CompatibilityOwnerId;
		}
		FString LegacyText;
		return FFileHelper::LoadFileToString(
			LegacyText,
			*FPaths::Combine(Root, LegacyOwnerMarker));
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
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutResult = {};
		FString ResolvedRoot;
		if (!ResolveCompatibilityPath(
				WorkspaceRoot,
				false,
				false,
				ResolvedRoot,
				OutErrorDetail))
		{
			OutErrorCode = TEXT("export_workspace_path_invalid");
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
				Plan,
				OutErrorCode,
				OutErrorDetail))
		{
			return false;
		}
		return PublishPlan(
			Plan.ToSharedRef(),
			ResolvedRoot,
			OutResult,
			OutErrorCode,
			OutErrorDetail);
	}

	bool PublishSingleFile(
		const FString& OutputFile,
		const FFilePlan& File,
		const FPublishContext& Context,
		FPublishResult& OutResult,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutResult = {};
		FString ResolvedFile;
		if (!ResolveCompatibilityPath(
				OutputFile,
				true,
				false,
				ResolvedFile,
				OutErrorDetail))
		{
			OutErrorCode = TEXT("export_single_file_path_invalid");
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
				Plan,
				OutErrorCode,
				OutErrorDetail))
		{
			return false;
		}
		return PublishPlan(
			Plan.ToSharedRef(),
			ResolvedFile,
			OutResult,
			OutErrorCode,
			OutErrorDetail);
	}

	bool ResetOwnedWorkspace(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		FString& OutError)
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
		if (!HasCompatibleOwnership(OutResolvedFolderPath))
		{
			OutError = FString::Printf(
				TEXT("export_workspace_ownership_required:%s"),
				*OutResolvedFolderPath);
			return false;
		}
		if (!IFileManager::Get().DeleteDirectory(
				*OutResolvedFolderPath,
				false,
				true))
		{
			OutError = FString::Printf(
				TEXT("export_workspace_delete_failed:%s"),
				*OutResolvedFolderPath);
			return false;
		}
		return IFileManager::Get().MakeDirectory(
			*OutResolvedFolderPath,
			true);
	}
}
