#pragma once

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

	struct UEAGENTINTERFACE_API FUmgWidgetCandidateCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentUmgWidgetCandidateCommandService
	{
	public:
		virtual ~IUeAgentUmgWidgetCandidateCommandService() = default;

		virtual bool Execute(
			const FUmgWidgetCandidateCommandRequest& Request,
			FUmgWidgetCandidateCommandResponse& OutResponse,
			FUmgWidgetCandidateCommandError& OutError) = 0;
	};

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

	UEAGENTINTERFACE_API bool TryCreateProductionUmgWidgetCandidateCommandService(
		TSharedPtr<IUeAgentUmgWidgetCandidateCommandService>& OutService,
		FString& OutError);
}
