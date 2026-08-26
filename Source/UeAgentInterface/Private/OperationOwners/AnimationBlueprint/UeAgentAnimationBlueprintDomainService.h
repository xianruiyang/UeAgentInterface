#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AnimationBlueprint
{
	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentAnimationBlueprintCandidateCommandService
	{
	public:
		virtual ~IUeAgentAnimationBlueprintCandidateCommandService() = default;

		virtual bool Execute(
			const FAnimationBlueprintCandidateCommandRequest& Request,
			FAnimationBlueprintCandidateCommandResponse& OutResponse,
			FAnimationBlueprintCandidateCommandError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentAnimationBlueprintCommandService
	{
	public:
		virtual ~IUeAgentAnimationBlueprintCommandService() = default;

		virtual bool Execute(
			const FAnimationBlueprintCommandRequest& Request,
			FAnimationBlueprintCommandResponse& OutResponse,
			FAnimationBlueprintCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAnimationBlueprintDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionAnimationBlueprintCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCommandService>& OutService,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionAnimationBlueprintCandidateCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCandidateCommandService>& OutService,
		FString& OutError);
}
