#include "CommandHandlers/AnimationCurve/UeAgentAnimationCurveDomainCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

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
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.SourceKind = TEXT("native_result");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::AnimationCurve::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
