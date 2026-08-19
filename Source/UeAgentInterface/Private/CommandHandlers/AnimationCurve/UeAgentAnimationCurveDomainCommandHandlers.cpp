#include "CommandHandlers/AnimationCurve/UeAgentAnimationCurveDomainCommandHandlers.h"

#include "DomainAdapters/AnimationCurve/UeAgentAnimationCurveDomainMechanics.h"
#include "QueryOwners/AnimationCurve/UeAgentAnimationCurveDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::AnimationCurve
{
	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::AnimationCurve::TryExecuteContentGraph(
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
		if (QueryOwners::AnimationCurve::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(
				TEXT("animation_curve_domain_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
