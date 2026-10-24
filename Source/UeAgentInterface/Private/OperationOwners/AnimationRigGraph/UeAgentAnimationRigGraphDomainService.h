#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AnimationRigGraph
{
	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FAnimationRigGraphCandidateCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentAnimationRigGraphCandidateCommandService
	{
	public:
		virtual ~IUeAgentAnimationRigGraphCandidateCommandService() = default;

		virtual bool Execute(
			const FAnimationRigGraphCandidateCommandRequest& Request,
			FAnimationRigGraphCandidateCommandResponse& OutResponse,
			FAnimationRigGraphCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAnimationRigGraphDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionAnimationRigGraphCandidateCommandService(
		TSharedPtr<IUeAgentAnimationRigGraphCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
