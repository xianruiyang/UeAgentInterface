#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::UmgWidget
{
	struct UEAGENTINTERFACE_API FUmgWidgetCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentUmgWidgetCommandService
	{
	public:
		virtual ~IUeAgentUmgWidgetCommandService() = default;

		virtual bool Execute(
			const FUmgWidgetCommandRequest& Request,
			FUmgWidgetCommandResponse& OutResponse,
			FUmgWidgetCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateUmgWidgetDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionUmgWidgetCommandService(
		TSharedPtr<IUeAgentUmgWidgetCommandService>& OutService,
		FString& OutError);
}
