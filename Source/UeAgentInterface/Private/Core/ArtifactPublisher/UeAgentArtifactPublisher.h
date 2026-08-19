#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::ArtifactPublisher
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::HjsonWorkspace::EWorkspaceKind;
	using UE::UAI::HjsonWorkspace::FExportWorkspacePlanRef;

	class FArtifactPublishError;

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
			FString InPublisherOwnerId);
		FArtifactPublishRequest(
			FExportWorkspacePlanRef InPlan,
			FString InFinalPath,
			FString InPublisherOwnerId,
			TSharedRef<IArtifactPublishVerification> InVerification);

		const FExportWorkspacePlanRef& GetPlan() const;
		const FString& GetFinalPath() const;
		const FString& GetPublisherOwnerId() const;
		const TSharedPtr<IArtifactPublishVerification>& GetVerification() const;
		bool RequiresVerification() const;
		bool IsValid() const;

	private:
		FExportWorkspacePlanRef Plan;
		FString FinalPath;
		FString PublisherOwnerId;
		TSharedPtr<IArtifactPublishVerification> Verification;
		bool bValid = false;
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

		friend UEAGENTINTERFACE_API bool TryPublishExportPlan(
			const FArtifactPublishRequest&,
			FUeAgentResourceTracker&,
			FArtifactPublishReceipt&,
			FArtifactPublishError&);
	};

	class UEAGENTINTERFACE_API FArtifactPublishError final
	{
	public:
		const FString& GetCode() const;
		const FString& GetPath() const;
		const FString& GetDetail() const;
		int64 GetResourceLimit() const;
		int64 GetResourceObserved() const;
		void Set(
			FString InCode,
			FString InPath = FString(),
			FString InDetail = FString(),
			int64 InResourceLimit = 0,
			int64 InResourceObserved = 0);

	private:
		FString Code;
		FString Path;
		FString Detail;
		int64 ResourceLimit = 0;
		int64 ResourceObserved = 0;

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
}
