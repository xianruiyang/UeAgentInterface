#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::Niagara
{
	struct UEAGENTINTERFACE_API FUeAgentNiagaraCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentNiagaraCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FUeAgentNiagaraCandidateCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentNiagaraCandidateCommandService
	{
	public:
		virtual ~IUeAgentNiagaraCandidateCommandService() = default;

		virtual bool Execute(
			const FUeAgentNiagaraCandidateCommandRequest& Request,
			FUeAgentNiagaraCandidateCommandResponse& OutResponse,
			FUeAgentNiagaraCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateNiagaraDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionNiagaraCandidateCommandService(
		TSharedPtr<IUeAgentNiagaraCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
