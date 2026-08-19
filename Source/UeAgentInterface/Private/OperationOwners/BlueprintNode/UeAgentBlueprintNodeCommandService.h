#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::BlueprintNode
{
	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentBlueprintNodeCommandService
	{
	public:
		virtual ~IUeAgentBlueprintNodeCommandService() = default;

		virtual bool Execute(
			const FUeAgentBlueprintNodeCommandRequest& Request,
			FUeAgentBlueprintNodeCommandResponse& OutResponse,
			FUeAgentBlueprintNodeCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateProductionBlueprintNodeCommandService(
		TSharedPtr<IUeAgentBlueprintNodeCommandService>& OutService,
		FString& OutError);
}

