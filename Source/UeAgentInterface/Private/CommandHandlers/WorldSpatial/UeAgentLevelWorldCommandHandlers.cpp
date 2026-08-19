#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"

#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "QueryOwners/WorldSpatial/UeAgentLevelWorldQueryOwner.h"

namespace UE::UAI::CommandHandlers::WorldSpatial
{
	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::WorldSpatial::TryExecuteContentGraph(
				Request.GetCommandId(), Request.GetTargetIdentity(),
				Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::WorldSpatial::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(TEXT("level_world_domain_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
