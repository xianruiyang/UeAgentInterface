#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::PieRuntimeDebug
{
	struct UEAGENTINTERFACE_API FUeAgentPieRuntimeDebugRoute final
	{
		FString CommandId;
		FString PythonNamespace;
		FString PythonFunction;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API bool TryGetRoute(
		const FUeAgentPieRuntimeDebugRoute*& OutRoute,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		UE::UAI::Contract::FDiagnostic& OutError);
}
