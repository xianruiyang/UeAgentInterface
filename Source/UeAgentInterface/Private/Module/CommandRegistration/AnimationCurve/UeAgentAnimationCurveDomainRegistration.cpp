#include "Module/CommandRegistration/AnimationCurve/UeAgentAnimationCurveDomainRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationCurve
{
	namespace
	{
		const FString DomainId = TEXT("animation-curve-command-migration");
		const FString ProviderRegistrationId =
			TEXT("animation-curve-domain-provider");

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AnimationCurve::
					TryCreateAnimationCurveDomainProvider(Provider, WorkspaceError)
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
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_curve.edit/1"), TEXT("animation_curve"),
					TEXT("/Script/Engine.PoseAsset"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.animation_curve.pose_asset.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_curve.edit/1"), TEXT("animation_curve"),
					TEXT("/Script/Engine.BlendSpace"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.animation_curve.blend_space.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_curve.edit/1"), TEXT("animation_curve"),
					TEXT("/Script/Engine.BlendSpace1D"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.animation_curve.blend_space.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_curve.edit/1"), TEXT("animation_curve"),
					TEXT("/Script/Engine.AnimSequence"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.animation_curve.sequence.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_curve.edit/1"), TEXT("animation_curve"),
					TEXT("/Script/Engine.AnimMontage"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.animation_curve.montage.root/1"), true},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.animation_curve.pose_asset.root/1"),
				TEXT("uai.init.animation_curve.blend_space.root/1"),
				TEXT("uai.init.animation_curve.sequence.root/1"),
				TEXT("uai.init.animation_curve.montage.root/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			Registration(*DomainId, &TryCreateDomainCapability);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
