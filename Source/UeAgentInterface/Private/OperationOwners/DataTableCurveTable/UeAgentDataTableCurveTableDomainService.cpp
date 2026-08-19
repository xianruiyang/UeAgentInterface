#include "OperationOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainService.h"

#include "CommandHandlers/DataTableCurveTable/UeAgentDataTableCurveTableDomainCommandHandlers.h"

namespace UE::UAI::OperationOwners::DataTableCurveTable
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("datatable-curvetable-domain-provider");
		const FString SchemaId = TEXT("uai.datatable_curvetable.edit/1");
		const FString DataTableType = TEXT("/Script/Engine.DataTable");
		const FString CurveTableType = TEXT("/Script/Engine.CurveTable");
		const FString DataTableContract =
			TEXT("uai.init.datatable_curvetable.data_table.root/1");
		const FString CurveTableContract =
			TEXT("uai.init.datatable_curvetable.curve_table.root/1");

		class FDataTableCurveTableDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				const FString& Type = Request.GetRootTypeIdentity();
				return Request.GetRole() == EInitializationNodeRole::RootTarget
					&& (Type == DataTableType || Type == CurveTableType)
					&& (Request.GetCommandId()
							!= TEXT("content_graph.import_workspace")
						|| Request.GetSchemaId() == SchemaId);
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)) return false;
				OutInitializationContractId =
					Request.GetRootTypeIdentity() == DataTableType
						? DataTableContract
						: CurveTableContract;
				return true;
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				const bool bDataTable =
					Request.GetRootTypeIdentity() == DataTableType
					&& InitializationContractId == DataTableContract;
				const bool bCurveTable =
					Request.GetRootTypeIdentity() == CurveTableType
					&& InitializationContractId == CurveTableContract;
				if (!bDataTable && !bCurveTable)
				{
					OutError.Set(
						TEXT("datatable_curvetable_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				return CommandHandlers::DataTableCurveTable::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateDataTableCurveTableDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FDataTableCurveTableDomainProvider>();
		OutError.Reset();
		return true;
	}
}
