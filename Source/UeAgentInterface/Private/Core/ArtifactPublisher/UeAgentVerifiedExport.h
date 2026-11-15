#pragma once

#include "CoreMinimal.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UE::UAI::ArtifactPublisher
{
	using UE::UAI::HjsonWorkspace::FExportWorkspacePlanPtr;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

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
		void SetWorkspacePreparation(HjsonWorkspace::FPreparedWorkspaceExport* InPrepared) { Prepared = InPrepared; }
		HjsonWorkspace::FPreparedWorkspaceExport* GetWorkspacePreparation() const { return Prepared; }
		bool IsValid() const;
		const FArtifactPublishError& GetValidationDiagnostic() const;

	private:
		TSharedRef<IVerifiedExportWorkspaceSource> Source;
		FString FinalPath;
		FString PublisherOwnerId;
		HjsonWorkspace::FPreparedWorkspaceExport* Prepared = nullptr;
		FArtifactPublishError ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryExportVerifiedWorkspace(
		const FVerifiedExportRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError);
}
