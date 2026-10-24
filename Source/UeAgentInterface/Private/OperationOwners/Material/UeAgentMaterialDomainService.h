#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::Material
{
	struct UEAGENTINTERFACE_API FUeAgentMaterialCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentMaterialCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	using FUeAgentMaterialCandidateCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentMaterialCandidateCommandService
	{
	public:
		virtual ~IUeAgentMaterialCandidateCommandService() = default;

		virtual bool Execute(
			const FUeAgentMaterialCandidateCommandRequest& Request,
			FUeAgentMaterialCandidateCommandResponse& OutResponse,
			FUeAgentMaterialCandidateCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateMaterialDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionMaterialCandidateCommandService(
		TSharedPtr<IUeAgentMaterialCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
