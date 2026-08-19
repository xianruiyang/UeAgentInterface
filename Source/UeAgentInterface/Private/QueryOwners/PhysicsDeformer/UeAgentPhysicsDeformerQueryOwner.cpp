#include "QueryOwners/PhysicsDeformer/UeAgentPhysicsDeformerQueryOwner.h"

namespace UE::UAI::QueryOwners::PhysicsDeformer
{
	const TArray<FString>& GetStateLayers()
	{
		static const TArray<FString> Layers = {
			TEXT("asset_persisted_state"),
			TEXT("simulation_observation"),
		};
		return Layers;
	}

	const TArray<FString>& GetIndependentReadbackFlows()
	{
		static const TArray<FString> Flows = {
			TEXT("query.command.detail"),
			TEXT("asset registry or simulation observation query"),
		};
		return Flows;
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}
