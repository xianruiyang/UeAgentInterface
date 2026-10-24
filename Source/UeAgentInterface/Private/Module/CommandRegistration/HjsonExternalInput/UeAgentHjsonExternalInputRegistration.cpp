#include "Module/CommandRegistration/HjsonExternalInput/UeAgentHjsonExternalInputRegistration.h"

#include "CommandHandlers/HjsonExternalInput/UeAgentHjsonExternalInputCommandHandlers.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/HjsonExternalInput/UeAgentHjsonExternalInputProvider.h"

namespace UE::UAI::Module::CommandRegistration::HjsonExternalInput
{
	namespace
	{
		const FString DomainId = TEXT("hjson-external-input-domain-provider");

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::HjsonExternalInput::
					TryCreateProductionHjsonExternalInputDomainProvider(
						Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {
				{DomainId, TEXT("content_graph.import_workspace"),
					TEXT("uai.hjson.external_input/1"),
					TEXT("hjson_external_input"),
					TEXT("uai.external_input.factory"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.hjson_external_input.factory_importer/1"), true},
				{DomainId, TEXT("content_graph.import_workspace"),
					TEXT("uai.hjson.external_input/1"),
					TEXT("hjson_external_input"),
					TEXT("uai.external_input.skin_weight_profile"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.hjson_external_input.skin_weight_importer/1"), true},
				{DomainId, TEXT("content_graph.import_workspace"),
					TEXT("uai.hjson.external_input/1"),
					TEXT("hjson_external_input"),
					TEXT("uai.external_input.localization"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.hjson_external_input.localization_importer/1"), true},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.hjson_external_input.factory_importer/1"),
				TEXT("uai.init.hjson_external_input.skin_weight_importer/1"),
				TEXT("uai.init.hjson_external_input.localization_importer/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);
	}

	const FString& GetHjsonExternalInputProviderContributionId()
	{
		static const FString Id(TEXT("hjson-external-input-domain-provider"));
		return Id;
	}

	bool ValidateHjsonExternalInputContribution(UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const TArray<FString> Commands = CommandHandlers::HjsonExternalInput::
			GetHjsonExternalInputDelegatedCommandIds();
		for (int32 Index = 1; Index < Commands.Num(); ++Index)
		{
			if (Commands[Index - 1] == Commands[Index])
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_importer_disposition_duplicate"), FString::Printf(TEXT("The delegated importer command is repeated at entries %d and %d."), Index - 1, Index), TEXT("registration"), FString(), Commands[Index]);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Commands[Index];
				return false;
			}
		}
		return true;
	}
}
