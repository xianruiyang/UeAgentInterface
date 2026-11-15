#include "CommandHandlers/DataTableCurveTable/UeAgentDataTableCurveTableDomainCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

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
				OutError,
				Request.GetWorkspaceExportPreparation().Get()))
		{
			return false;
		}
		if (QueryOwners::DataTableCurveTable::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("datatable_curvetable_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.SourceKind = TEXT("native_result");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::DataTableCurveTable::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
