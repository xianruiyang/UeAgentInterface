#include "CommandHandlers/DataTableCurveTable/UeAgentDataTableCurveTableDomainCommandHandlers.h"

#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableCurveTableDomainMechanics.h"
#include "QueryOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::DataTableCurveTable
{
	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::DataTableCurveTable::TryExecuteContentGraph(
				Request.GetCommandId(),
				Request.GetTargetIdentity(),
				Request.GetRootTypeIdentity(),
				Request.GetCanonicalParamsJson(),
				Payload,
				bReadOnly,
				OutError))
		{
			return false;
		}
		if (QueryOwners::DataTableCurveTable::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(
				TEXT("datatable_curvetable_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
