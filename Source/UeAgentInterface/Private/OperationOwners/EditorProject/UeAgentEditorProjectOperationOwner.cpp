#include "OperationOwners/EditorProject/UeAgentEditorProjectOperationOwner.h"

#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::OperationOwners::EditorProject
{
	CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		TArray<FString> Scopes = Caller.GetAuthorityScopes();
		for (const FString& Scope : DomainAdapters::EditorProject::
			GetRequiredAuthorityScopes(Request.GetCommandId()))
		{
			Scopes.AddUnique(Scope);
		}
		const Contract::FUeAgentCallerContext EffectiveCaller(
			Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
		return AssetSimple::SubmitProductionAssetSimpleOperation(
			Request, EffectiveCaller, Cancellation);
	}
}
