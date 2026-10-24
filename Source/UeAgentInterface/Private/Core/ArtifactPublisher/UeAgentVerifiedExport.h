#pragma once

#include "CoreMinimal.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

namespace UE::UAI::ArtifactPublisher
{
	using UE::UAI::HjsonWorkspace::FExportWorkspacePlanPtr;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	class UEAGENTINTERFACE_API FExportSemanticSignature final
	{
	public:
		FExportSemanticSignature();
		FExportSemanticSignature(
			FString InInventory,
			FString InReferenceClosure,
			FString InContentDigests,
			FString InContractSemantic,
			FString InBusinessSemantic);

		const FString& GetInventory() const;
		const FString& GetReferenceClosure() const;
		const FString& GetContentDigests() const;
		const FString& GetContractSemantic() const;
		const FString& GetBusinessSemantic() const;
		bool IsValid() const;
		const FArtifactPublishError& GetValidationDiagnostic() const;
		bool operator==(const FExportSemanticSignature& Other) const;
		bool operator!=(const FExportSemanticSignature& Other) const;
		TMap<FString, FString> ToReceiptFields() const;

	private:
		FString Inventory;
		FString ReferenceClosure;
		FString ContentDigests;
		FString ContractSemantic;
		FString BusinessSemantic;
		FArtifactPublishError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IVerifiedExportWorkspaceSource
	{
	public:
		virtual ~IVerifiedExportWorkspaceSource() = default;

		virtual bool TryFreezeTarget(
			FUeAgentStateRevision& OutRevision,
			FArtifactPublishError& OutError) = 0;
		virtual bool TryCaptureWorkspace(
			const FUeAgentStateRevision& FrozenRevision,
			FExportWorkspacePlanPtr& OutPlan,
			FExportSemanticSignature& OutSemantic,
			FArtifactPublishError& OutError) = 0;
		virtual bool TryRebuildStagedWorkspace(
			const FString& StagedWorkspacePath,
			EWorkspaceKind WorkspaceKind,
			const FString& EntryFile,
			FUeAgentResourceTracker& Resources,
			FExportSemanticSignature& OutSemantic,
			FArtifactPublishError& OutError) = 0;
		virtual bool TryReadCurrentTargetRevision(
			FUeAgentStateRevision& OutRevision,
			FArtifactPublishError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FVerifiedExportRequest final
	{
	public:
		FVerifiedExportRequest(
			TSharedRef<IVerifiedExportWorkspaceSource> InSource,
			FString InFinalPath,
			FString InPublisherOwnerId);

		const TSharedRef<IVerifiedExportWorkspaceSource>& GetSource() const;
		const FString& GetFinalPath() const;
		const FString& GetPublisherOwnerId() const;
		bool IsValid() const;
		const FArtifactPublishError& GetValidationDiagnostic() const;

	private:
		TSharedRef<IVerifiedExportWorkspaceSource> Source;
		FString FinalPath;
		FString PublisherOwnerId;
		FArtifactPublishError ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryExportVerifiedWorkspace(
		const FVerifiedExportRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);
}
