#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::BlueprintComponent
{
	struct UEAGENTINTERFACE_API FUeAgentBlueprintComponentCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentBlueprintComponentCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FUeAgentBlueprintComponentCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentBlueprintComponentCommandService
	{
	public:
		virtual ~IUeAgentBlueprintComponentCommandService() = default;

		virtual bool Execute(
			const FUeAgentBlueprintComponentCommandRequest& Request,
			FUeAgentBlueprintComponentCommandResponse& OutResponse,
			FUeAgentBlueprintComponentCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateProductionBlueprintComponentCommandService(
		TSharedPtr<IUeAgentBlueprintComponentCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
