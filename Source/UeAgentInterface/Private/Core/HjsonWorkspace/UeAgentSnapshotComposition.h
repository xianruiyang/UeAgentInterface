#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	class UEAGENTINTERFACE_API FSnapshotCompositionRequest final
	{
	public:
		FSnapshotCompositionRequest(
			FString InTargetKind,
			FString InProfileId,
			UE::UAI::StateSemantic::
				FUeAgentCanonicalSnapshotRef InSnapshot);

		const FString& GetTargetKind() const;
		const FString& GetProfileId() const;
		const UE::UAI::StateSemantic::
			FUeAgentCanonicalSnapshotRef& GetSnapshot() const;
		bool IsValid() const;

	private:
		FString TargetKind;
		FString ProfileId;
		UE::UAI::StateSemantic::
			FUeAgentCanonicalSnapshotRef Snapshot;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryComposeSnapshotAuthoringModel(
		const FSnapshotCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildSnapshotExportPlan(
		const FSnapshotCompositionRequest& Request,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);
}
