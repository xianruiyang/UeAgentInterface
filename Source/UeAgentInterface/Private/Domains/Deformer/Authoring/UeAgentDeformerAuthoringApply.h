#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentDeformerAuthoring
{
	UEAGENTINTERFACE_API bool ApplySemanticWorkspace(
		const FString& TargetPath, const FString& RequestId,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		const UeAgentAuthoringOperationApply::FBackendOptions& Options,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError);
}
