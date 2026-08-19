#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::OperationOwners::ModelingGeometry
{
	CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		TArray<FString> Scopes = Caller.GetAuthorityScopes();
		for (const FString& Scope : DomainAdapters::ModelingGeometry::
			GetRequiredAuthorityScopes())
		{
			Scopes.AddUnique(Scope);
		}
		const Contract::FUeAgentCallerContext EffectiveCaller(
			Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
		return AssetSimple::SubmitProductionAssetSimpleOperation(
			Request, EffectiveCaller, Cancellation);
	}
}
