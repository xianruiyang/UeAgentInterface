#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AudioGraph
{
	struct UEAGENTINTERFACE_API FAudioGraphCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAudioGraphCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FAudioGraphCandidateCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentAudioGraphCandidateCommandService
	{
	public:
		virtual ~IUeAgentAudioGraphCandidateCommandService() = default;

		virtual bool Execute(
			const FAudioGraphCandidateCommandRequest& Request,
			FAudioGraphCandidateCommandResponse& OutResponse,
			FAudioGraphCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAudioGraphDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionAudioGraphCandidateCommandService(
		TSharedPtr<IUeAgentAudioGraphCandidateCommandService>& OutService,
		FString& OutError);
}
