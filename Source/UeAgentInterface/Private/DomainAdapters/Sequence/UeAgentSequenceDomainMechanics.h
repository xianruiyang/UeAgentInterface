#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "CoreMinimal.h"

namespace UeAgentAuthoringOperationApply { struct FBackendOptions; }
namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UE::UAI::DomainAdapters::Sequence
{
	UEAGENTINTERFACE_API bool ApplySequenceStructureAuthoring(
		const FString& Target,
		const FString& RequestId,
		const TSharedPtr<FJsonObject>& Structure,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& OutOutcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions = nullptr);

	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError,
		HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
