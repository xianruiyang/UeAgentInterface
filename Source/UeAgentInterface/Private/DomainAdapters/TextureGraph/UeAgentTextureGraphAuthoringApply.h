#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

namespace UE::UAI::HjsonWorkspace { class FHjsonDocument; }

namespace UE::UAI::DomainAdapters::TextureGraph
{
	UEAGENTINTERFACE_API bool ApplyInitialGraph(
		const FString& Target, const TSharedRef<FJsonObject>& Graph,
		const AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding& Factory, const FString& AuthorizationId,
		const UeAgentAuthoringOperationApply::FBackendOptions& Options,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError,
		const HjsonWorkspace::FHjsonDocument* SourceDocument = nullptr);
	UEAGENTINTERFACE_API bool ApplySemanticComposite(
		const Composition::GraphCompound::FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		const UeAgentAuthoringOperationApply::FBackendOptions& Options,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);
}
