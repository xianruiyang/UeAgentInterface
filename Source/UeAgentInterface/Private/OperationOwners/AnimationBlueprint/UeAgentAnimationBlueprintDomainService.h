#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AnimationBlueprint
{
	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FAnimationBlueprintCandidateCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentAnimationBlueprintCandidateCommandService
	{
	public:
		virtual ~IUeAgentAnimationBlueprintCandidateCommandService() = default;

		virtual bool Execute(
			const FAnimationBlueprintCandidateCommandRequest& Request,
			FAnimationBlueprintCandidateCommandResponse& OutResponse,
			FAnimationBlueprintCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAnimationBlueprintDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionAnimationBlueprintCandidateCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
