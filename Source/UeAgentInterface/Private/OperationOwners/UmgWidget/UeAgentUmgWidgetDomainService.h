#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::UmgWidget
{
	struct UEAGENTINTERFACE_API FUmgWidgetCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FUmgWidgetCandidateCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentUmgWidgetCandidateCommandService
	{
	public:
		virtual ~IUeAgentUmgWidgetCandidateCommandService() = default;

		virtual bool Execute(
			const FUmgWidgetCandidateCommandRequest& Request,
			FUmgWidgetCandidateCommandResponse& OutResponse,
			FUmgWidgetCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateUmgWidgetDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionUmgWidgetCandidateCommandService(
		TSharedPtr<IUeAgentUmgWidgetCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
