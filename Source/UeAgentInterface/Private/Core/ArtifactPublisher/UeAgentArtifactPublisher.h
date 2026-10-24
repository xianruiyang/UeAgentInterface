#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::ArtifactPublisher
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::HjsonWorkspace::EWorkspaceKind;
	using UE::UAI::HjsonWorkspace::FExportWorkspacePlanRef;

	using FArtifactPublishError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IArtifactPublishVerification
	{
	public:
		virtual ~IArtifactPublishVerification() = default;

		virtual bool TryVerifyStagedWorkspace(
			const FString& StagedWorkspacePath,
			EWorkspaceKind WorkspaceKind,
			const FString& EntryFile,
			FUeAgentResourceTracker& Resources,
			FArtifactPublishError& OutError) = 0;
		virtual bool TryRecheckTargetVersion(
			FArtifactPublishError& OutError) = 0;
		virtual TMap<FString, FString> GetSemanticVerificationFields() const = 0;
	};

	enum class EArtifactPublishStatus : uint8
	{
		Published,
	};

	class UEAGENTINTERFACE_API FArtifactPublishRequest final
	{
	public:
		FArtifactPublishRequest(
			FExportWorkspacePlanRef InPlan,
			FString InFinalPath,
			FString InPublisherOwnerId,
			TMap<FString, TArray<uint8>> InPreservedFiles = {});
		FArtifactPublishRequest(
			FExportWorkspacePlanRef InPlan,
			FString InFinalPath,
			FString InPublisherOwnerId,
			TSharedRef<IArtifactPublishVerification> InVerification);

		const FExportWorkspacePlanRef& GetPlan() const;
		const FString& GetFinalPath() const;
		const FString& GetPublisherOwnerId() const;
		const TMap<FString, TArray<uint8>>& GetPreservedFiles() const;
		const TSharedPtr<IArtifactPublishVerification>& GetVerification() const;
		bool RequiresVerification() const;
		bool IsValid() const;
		const FArtifactPublishError& GetValidationDiagnostic() const;

	private:
		FExportWorkspacePlanRef Plan;
		FString FinalPath;
		FString PublisherOwnerId;
		TMap<FString, TArray<uint8>> PreservedFiles;
		TSharedPtr<IArtifactPublishVerification> Verification;
		FArtifactPublishError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FArtifactPublishReceipt final
	{
	public:
		EArtifactPublishStatus GetStatus() const;
		const FString& GetFinalPath() const;
		const FString& GetPublisherOwnerId() const;
		const FString& GetTransactionId() const;
		EWorkspaceKind GetWorkspaceKind() const;
		const FString& GetEntryFile() const;
		const FString& GetPlanFingerprint() const;
		const FString& GetSnapshotId() const;
		const FString& GetSnapshotRevisionSourceId() const;
		const FString& GetSnapshotRevisionGeneration() const;
		int64 GetSnapshotRevisionSequence() const;
		const FString& GetSnapshotEvidenceId() const;
		const TArray<FString>& GetIncludedFiles() const;
		const TMap<FString, FString>& GetFileHashes() const;
		const FString& GetInventoryHash() const;
		bool WasSemanticWorkspaceVerified() const;
		bool WasTargetVersionRechecked() const;
		const TMap<FString, FString>& GetSemanticVerificationFields() const;
		const TArray<FArtifactPublishError>& GetCleanupWarnings() const { return CleanupWarnings; }
		FArtifactPublishError MakeResultEncodingFailure(const FArtifactPublishError& Cause) const;

	private:
		void SetPublished(
			const FArtifactPublishRequest& Request,
			FString InFinalPath,
			FString InTransactionId,
			TArray<FString> InIncludedFiles,
			TMap<FString, FString> InFileHashes,
			FString InInventoryHash);

		EArtifactPublishStatus Status = EArtifactPublishStatus::Published;
		FString FinalPath;
		FString PublisherOwnerId;
		FString TransactionId;
		EWorkspaceKind WorkspaceKind = EWorkspaceKind::Directory;
		FString EntryFile;
		FString PlanFingerprint;
		FString SnapshotId;
		FString SnapshotRevisionSourceId;
		FString SnapshotRevisionGeneration;
		int64 SnapshotRevisionSequence = -1;
		FString SnapshotEvidenceId;
		TArray<FString> IncludedFiles;
		TMap<FString, FString> FileHashes;
		FString InventoryHash;
		bool bSemanticWorkspaceVerified = false;
		bool bTargetVersionRechecked = false;
		TMap<FString, FString> SemanticVerificationFields;
		TArray<FArtifactPublishError> CleanupWarnings;

		friend UEAGENTINTERFACE_API bool TryPublishExportPlan(
			const FArtifactPublishRequest&,
			FUeAgentResourceTracker&,
			FArtifactPublishReceipt&,
			FArtifactPublishError&);
	};

	

	UEAGENTINTERFACE_API bool TryPublishExportPlan(
		const FArtifactPublishRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);

	/** Republish a fresh canonical export into its original workspace, retaining
	 * files outside the explicitly consumed/previously managed source set. */
	UEAGENTINTERFACE_API bool TryRefreshPublishedWorkspace(
		const FString& CanonicalWorkspace,
		const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);
}
