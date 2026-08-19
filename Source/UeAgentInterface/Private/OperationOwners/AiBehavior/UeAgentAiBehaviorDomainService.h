#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AiBehavior
{
	struct UEAGENTINTERFACE_API FAiBehaviorCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidateCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentAiBehaviorCandidateCommandService
	{
	public:
		virtual ~IUeAgentAiBehaviorCandidateCommandService() = default;

		virtual bool Execute(
			const FAiBehaviorCandidateCommandRequest& Request,
			FAiBehaviorCandidateCommandResponse& OutResponse,
			FAiBehaviorCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAiBehaviorDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionAiBehaviorCandidateCommandService(
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService>& OutService,
		FString& OutError);
}
