#include "OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h"

#include "CommandHandlers/AnimationCurve/UeAgentAnimationCurveDomainCommandHandlers.h"

namespace UE::UAI::OperationOwners::AnimationCurve
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("animation-curve-domain-provider");
		const FString SchemaId = TEXT("uai.animation_curve.edit/1");
		const FString BlendSpaceType = TEXT("/Script/Engine.BlendSpace");
		const FString SequenceType = TEXT("/Script/Engine.AnimSequence");
		const FString BlendSpaceContract =
			TEXT("uai.init.animation_curve.blend_space.root/1");
		const FString SequenceContract =
			TEXT("uai.init.animation_curve.sequence.root/1");

		class FAnimationCurveDomainProvider final
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
					&& (Type == BlendSpaceType || Type == SequenceType)
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
				if (!Matches(Request))
				{
					return false;
				}
				OutInitializationContractId =
					Request.GetRootTypeIdentity() == BlendSpaceType
						? BlendSpaceContract
						: SequenceContract;
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
				const bool bBlend =
					Request.GetRootTypeIdentity() == BlendSpaceType
					&& InitializationContractId == BlendSpaceContract;
				const bool bSequence =
					Request.GetRootTypeIdentity() == SequenceType
					&& InitializationContractId == SequenceContract;
				if (!bBlend && !bSequence)
				{
					OutError.Set(
						TEXT("animation_curve_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				return CommandHandlers::AnimationCurve::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateAnimationCurveDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationCurveDomainProvider>();
		OutError.Reset();
		return true;
	}
}
