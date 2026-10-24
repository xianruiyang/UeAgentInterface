#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::Composition::LongRunningSession
{
	UEAGENTINTERFACE_API const TArray<DomainAdapters::EditorSessionOperation::
		FUeAgentEditorSessionOperationPolicyDescriptor>&
	GetLongRunningEditorSessionPolicyDescriptors();

	struct UEAGENTINTERFACE_API
	FUeAgentLongRunningSessionProductionConfig final
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		FString OwnerLeaseSecret;
		TWeakObjectPtr<UWorld> EditorWorld;

		bool IsValid() const;
	};

	using FUeAgentLongRunningSessionProductionError = Contract::FDiagnostic;

	/** Saves only explicitly requested resources; keeps the Editor running. */
	UEAGENTINTERFACE_API bool TrySaveEditorResources(
		const FString& ParamsJson, FString& OutPayload,
		FUeAgentLongRunningSessionProductionError& OutError);

	/**
	 * Composes the immutable production policy catalogs, operation-scoped
	 * native builders, one LongRunningSession runtime and exact public
	 * handlers.  No transport route or second lifecycle registry is created.
	 */
	UEAGENTINTERFACE_API bool TryCreateLongRunningSessionProduction(
		const FUeAgentLongRunningSessionProductionConfig& Config,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		FUeAgentLongRunningSessionProductionError& OutError);
}
