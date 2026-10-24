#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "CoreMinimal.h"

class UTextureGraph;
namespace UE::UAI::Composition::HjsonImport { class FPreparedAuthoringImport; struct FSemanticWorkspaceChangeProjection; }
namespace UE::UAI::HjsonWorkspace { class FResolvedSemanticWorkspace; }

namespace UE::UAI::Composition::GraphCompound
{
	struct FTextureGraphSemanticCompositeRequest;
}

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	struct FUeAgentContentGraphCapture;
}

namespace UE::UAI::DomainAdapters::TextureGraph
{
	UEAGENTINTERFACE_API bool TryBuildSemanticComposite(
		const HjsonWorkspace::FAuthoringModelRef& ResolvedModel,
		const ContentGraphRead::FUeAgentContentGraphCapture& CurrentCapture,
		const UTextureGraph& CurrentAsset,
		const FString& TargetObjectPath,
		Composition::GraphCompound::FTextureGraphSemanticCompositeRequest&
			OutComposite,
		HjsonWorkspace::FWorkspaceError& OutError,
		const HjsonWorkspace::FResolvedSemanticWorkspace* Workspace = nullptr);

	UEAGENTINTERFACE_API bool TryBindSettingChanges(
		const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		Composition::GraphCompound::FTextureGraphSemanticCompositeRequest& Composite,
		UE::UAI::Contract::FDiagnostic& Error);

	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError);
}
