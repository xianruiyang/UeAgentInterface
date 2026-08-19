#include "Module/CommandRegistration/DataTableCurveTable/UeAgentDataTableCurveTableDomainRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainService.h"

namespace UE::UAI::Module::CommandRegistration::DataTableCurveTable
{
	namespace
	{
		const FString ProviderRegistrationId =
			TEXT("datatable-curvetable-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			Registration(
				TEXT("datatable-curvetable-domain-provider"),
				&OperationOwners::DataTableCurveTable::
					TryCreateDataTableCurveTableDomainProvider);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::
				IImportInitializationDomainProvider> Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::DataTableCurveTable::
				TryCreateDataTableCurveTableDomainProvider(
					Provider,
					WorkspaceError))
			{
				OutError = WorkspaceError.GetCode();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
						TEXT("datatable-curvetable"));
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {
				{
					Provider->GetProviderId(),
					TEXT("content_graph.import_workspace"),
					TEXT("uai.datatable_curvetable.edit/1"),
					TEXT("data_table"),
					TEXT("/Script/Engine.DataTable"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.datatable_curvetable.data_table.root/1"),
					true,
				},
				{
					Provider->GetProviderId(),
					TEXT("content_graph.import_workspace"),
					TEXT("uai.datatable_curvetable.edit/1"),
					TEXT("curve_table"),
					TEXT("/Script/Engine.CurveTable"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.datatable_curvetable.curve_table.root/1"),
					true,
				},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.datatable_curvetable.data_table.root/1"),
				TEXT("uai.init.datatable_curvetable.curve_table.root/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T27-TABLE-DOMAIN");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration),
				OutContribution,
				OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("datatable-curvetable"),
				&TryCreateDomainCapability);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
