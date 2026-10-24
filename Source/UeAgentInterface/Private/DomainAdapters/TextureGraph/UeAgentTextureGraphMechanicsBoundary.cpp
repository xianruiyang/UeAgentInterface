#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::TextureGraph
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("texture_graph.artifact.capture"),
			TEXT("texture_graph.artifact.bake"),
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.runtime.probe"),
		};
		return Ids;
	}

	bool TryGetActivationProfiles(
		TArray<FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId == TEXT("texture_graph.artifact.bake")
				|| CommandId == TEXT("texture_graph.runtime.probe"))
			{
				continue;
			}
			const FUeAgentGraphCompoundActivationProfile* Match =
				Canonical.FindByPredicate(
					[&CommandId](const FUeAgentGraphCompoundActivationProfile& Profile)
					{
						return Profile.GetCommandId() == CommandId;
					});
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_profile_missing"),
					TEXT("No TextureGraph activation profile is registered for this command."), TEXT("runtime_factory"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = CommandId;
				return false;
			}
			if (!Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = Match->GetValidationDiagnostic();
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return true;
	}
}
