#pragma once

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

namespace UeAgentAuthoringOperationApply { struct FBackendOptions; }

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	UEAGENTINTERFACE_API bool ApplyDataTableAuthoring(const FString& Target, const FString& RequestId,
		const TSharedPtr<FJsonObject>& Desired,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& OutOutcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions = nullptr,
		const TArray<TSharedPtr<FJsonValue>>& Properties = {}, bool bCreate = false);
}
