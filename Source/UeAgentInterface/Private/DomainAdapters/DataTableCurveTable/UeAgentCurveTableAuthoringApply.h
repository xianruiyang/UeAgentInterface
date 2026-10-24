#pragma once

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

namespace UeAgentAuthoringOperationApply { struct FBackendOptions; }

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	UEAGENTINTERFACE_API bool ApplyCurveTableAuthoring(const FString& Target, const FString& RequestId,
		const TSharedPtr<FJsonObject>& Desired,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions = nullptr,
		const TArray<TSharedPtr<FJsonValue>>& Properties = {}, bool bCreate = false);
}
