#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::Blueprint::Surfaces
{
	struct UEAGENTINTERFACE_API FBlueprintSurfaceCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FBlueprintSurfaceCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FBlueprintSurfaceCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentBlueprintSurfaceCommandService
	{
	public:
		virtual ~IUeAgentBlueprintSurfaceCommandService() = default;

		virtual bool Execute(
			const FBlueprintSurfaceCommandRequest& Request,
			FBlueprintSurfaceCommandResponse& OutResponse,
			FBlueprintSurfaceCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateBlueprintSurfaceDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionBlueprintSurfaceCommandService(
		TSharedPtr<IUeAgentBlueprintSurfaceCommandService>& OutService,
		FString& OutError);
}
