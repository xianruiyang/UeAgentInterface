#include "CommandHandlers/Sequence/UeAgentSequenceDomainCommandHandlers.h"

#include "DomainAdapters/Sequence/UeAgentSequenceDomainMechanics.h"
#include "QueryOwners/Sequence/UeAgentSequenceDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::Sequence
{
	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::Sequence::TryExecuteContentGraph(
				Request.GetCommandId(),
				Request.GetTargetIdentity(),
				Request.GetCanonicalParamsJson(),
				Payload,
				bReadOnly,
				OutError))
		{
			return false;
		}
		if (QueryOwners::Sequence::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(
				TEXT("sequence_domain_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
