#pragma once

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

namespace UeAgentAuthoringOperationApply { struct FBackendOptions; }

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	UEAGENTINTERFACE_API bool ApplyStringTableAuthoring(const FString& Target, const FString& RequestId,
		const TSharedPtr<FJsonObject>& Desired, HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions = nullptr, bool bCreate = false);
}
