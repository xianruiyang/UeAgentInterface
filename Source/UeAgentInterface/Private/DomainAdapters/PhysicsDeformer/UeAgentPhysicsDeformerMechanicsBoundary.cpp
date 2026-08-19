#include "DomainAdapters/PhysicsDeformer/UeAgentPhysicsDeformerMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::PhysicsDeformer
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.deformer_graph.asset.create"),
			TEXT("animation.deformer_graph.topology.apply"),
			TEXT("animation.deformer_source_library.asset.create"),
			TEXT("animation.deformer_source_library.bindings.apply"),
			TEXT("animation.mesh_deformer_collection.asset.create"),
			TEXT("animation.mesh_deformer_collection.entries.apply"),
			TEXT("animation.ml_deformer.asset.create"),
			TEXT("animation.ml_deformer.configuration.apply"),
			TEXT("deformer.evaluation.sample"),
			TEXT("deformer.geometry.sample"),
			TEXT("deformer.training_inputs.validate"),
			TEXT("physics.asset.structure.replace"),
			TEXT("physics.constraint.update"),
			TEXT("physics.material.update"),
			TEXT("physics.simulation.probe"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool IsQuery(const FString& CommandId)
	{
		return CommandId == TEXT("deformer.evaluation.sample")
			|| CommandId == TEXT("deformer.geometry.sample")
			|| CommandId == TEXT("deformer.training_inputs.validate");
	}

	bool TryGetGraphProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical =
			Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (!CommandId.StartsWith(TEXT("animation.")))
			{
				continue;
			}
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.GetCommandId() == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("physics_deformer_graph_profile_missing:")
					+ CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == 8;
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical =
			Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId.StartsWith(TEXT("animation.")))
			{
				continue;
			}
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("physics_deformer_timeline_profile_missing:")
					+ CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == 7;
	}
}
