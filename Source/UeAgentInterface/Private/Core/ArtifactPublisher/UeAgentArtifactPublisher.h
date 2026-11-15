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
	/** End this owner's optional prior-publication observation lifetime. Input
	 * proofs retain their own observations; later publication rechecks bytes. */
	void ReleaseCachedArtifactFileObservations(const FString& WorkspaceRoot);

	class UEAGENTINTERFACE_API IArtifactPublishVerification
	{
	public:
		virtual ~IArtifactPublishVerification() = default;

		virtual bool TryRecheckTargetVersion(
			FArtifactPublishError& OutError) = 0;
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
		void SetVerification(TSharedPtr<IArtifactPublishVerification> InVerification) { Verification = MoveTemp(InVerification); }
		void SetSourceCheckpoint(TFunction<bool(FArtifactPublishError&)> InCheckpoint);
		bool HasSourceCheckpoint() const { return static_cast<bool>(SourceCheckpoint); }
		bool RecheckSource(FArtifactPublishError& OutError) const;
		void SetManagedSourcePaths(TArray<FString> InPaths) { ManagedSourcePaths = MoveTemp(InPaths); }
		const TArray<FString>& GetManagedSourcePaths() const { return ManagedSourcePaths; }
		void SetConsumerState(FString InState) { ConsumerState = MoveTemp(InState); }
		const FString& GetConsumerState() const { return ConsumerState; }
		bool ShouldRememberSource(const FString& RelativePath) const { return !NonSourceFiles.Contains(FPathRef{&RelativePath}); }
		bool IsValid() const;
		const FArtifactPublishError& GetValidationDiagnostic() const;

	private:
		FExportWorkspacePlanRef Plan;
		FString FinalPath;
		FString PublisherOwnerId;
		TMap<FString, TArray<uint8>> PreservedFiles;
		TSharedPtr<IArtifactPublishVerification> Verification;
		FArtifactPublishError ValidationDiagnostic;
		TFunction<bool(FArtifactPublishError&)> SourceCheckpoint;
		TArray<FString> ManagedSourcePaths;
		FString ConsumerState;
		struct FPathRef {
			const FString* Path;
			friend uint32 GetTypeHash(const FPathRef& Key) { return GetTypeHash(*Key.Path); }
			bool operator==(const FPathRef& Other) const { return *Path == *Other.Path; }
		};
		// Borrow stable strings from Plan; no duplicate path strings or model.
		TSet<FPathRef> NonSourceFiles;
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

	/** Export-only recovery. Readers reject an incomplete /3 generation and
	 * never replay a native import. A live publisher's exclusive lock rejects recovery. */
	UEAGENTINTERFACE_API bool TryRecoverWorkspacePublication(
		const FString& WorkspaceRoot, FArtifactPublishError& OutError);

	/** Republish a fresh canonical export into its original workspace, retaining
	 * files outside the explicitly consumed/previously managed source set. */
	UEAGENTINTERFACE_API bool TryRefreshPublishedWorkspace(
		const FString& CanonicalWorkspace,
		const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);

	/** Publish a fresh in-memory export into its source workspace using the same
	 * preservation and transactional publication rules as published refresh. */
	UEAGENTINTERFACE_API bool TryRefreshPlannedWorkspace(
		FExportWorkspacePlanRef Plan,
		const FString& PublisherOwnerId,
		const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);
	UEAGENTINTERFACE_API bool TryRefreshPlannedWorkspace(
		FExportWorkspacePlanRef Plan, const FString& PublisherOwnerId, const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths, FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt, FArtifactPublishError& OutError,
		TFunction<bool(FArtifactPublishError&)> SourceCheckpoint, FString ConsumerState = FString(),
		TSharedPtr<IArtifactPublishVerification> Verification = nullptr);
	/** Read only the private state published with a matching complete workspace.
	 * This is not a source acceptance or current native-state proof. */
	UEAGENTINTERFACE_API bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot,
		const FString& PublisherOwnerId, FString& OutState, FArtifactPublishError& OutError);
	UEAGENTINTERFACE_API bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot,
		const FString& PublisherOwnerId, FString& OutState, FString& OutPublisherOwnerId, FArtifactPublishError& OutError,
		TMap<FString, FString>* OutFileHashes);
	UEAGENTINTERFACE_API bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot,
		const FString& PublisherOwnerId, FString& OutState, FString& OutPublisherOwnerId, FArtifactPublishError& OutError);
	/** Same parser/owner validation on bytes already read by the synchronous
	 * Source owner. That owner must keep its original current-version fence;
	 * this function neither observes files nor retries another representation. */
	bool TryReadObservedWorkspaceConsumerState(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		const FString& ObservedText, FString& OutState, FString& OutPublisherOwnerId,
		FArtifactPublishError& OutError, TMap<FString, FString>* OutFileHashes);
	/** Historical generated-byte hints only. The consumer must independently
	 * prove current source bytes and complete current output dependencies. */
	bool TryReadPublishedWorkspaceFileHashes(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		TMap<FString, FString>& OutHashes, FArtifactPublishError& OutError, bool bGeneratedOnly = false);
}
