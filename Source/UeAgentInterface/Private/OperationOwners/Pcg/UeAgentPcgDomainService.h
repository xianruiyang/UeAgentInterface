#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::Pcg
{
	struct UEAGENTINTERFACE_API FUeAgentPcgCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentPcgCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentPcgCandidateCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentPcgCandidateCommandService
	{
	public:
		virtual ~IUeAgentPcgCandidateCommandService() = default;

		virtual bool Execute(
			const FUeAgentPcgCandidateCommandRequest& Request,
			FUeAgentPcgCandidateCommandResponse& OutResponse,
			FUeAgentPcgCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreatePcgDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionPcgCandidateCommandService(
		TSharedPtr<IUeAgentPcgCandidateCommandService>& OutService,
		FString& OutError);
}
