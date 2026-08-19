#include "Module/CommandRegistration/LocalizationStringTable/UeAgentLocalizationStringTableDomainRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/LocalizationStringTable/UeAgentLocalizationStringTableDomainService.h"

namespace UE::UAI::Module::CommandRegistration::LocalizationStringTable
{
	namespace
	{
		const FString ProviderRegistrationId = TEXT("localization-string-table-hjson-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration Registration(
			TEXT("localization-string-table-hjson-provider"),
			&OperationOwners::LocalizationStringTable::TryCreateLocalizationStringTableDomainProvider);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::
				IImportInitializationDomainProvider> Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::LocalizationStringTable::
				TryCreateLocalizationStringTableDomainProvider(
					Provider,
					WorkspaceError))
			{
				OutError = WorkspaceError.GetCode();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
						TEXT("localization-string-table"));
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {
				{
					Provider->GetProviderId(),
					TEXT("content_graph.import_workspace"),
					TEXT("uai.localization.edit/1"),
					TEXT("localization_settings"),
					TEXT("/Script/Localization.LocalizationSettings"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.localization.settings.root/1"),
					true,
				},
				{
					Provider->GetProviderId(),
					TEXT("content_graph.import_workspace"),
					TEXT("uai.string_table.edit/1"),
					TEXT("string_table"),
					TEXT("/Script/Engine.StringTable"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.localization.string_table.root/1"),
					true,
				},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.localization.settings.root/1"),
				TEXT("uai.init.localization.string_table.root/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T30-EXTERNAL-PROVENANCE-DOMAIN");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration),
				OutContribution,
				OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("localization-string-table"),
				&TryCreateDomainCapability);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
