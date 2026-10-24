#include "DomainAdapters/PhysicsDeformer/UeAgentPhysicsDeformerMechanicsBoundary.h"

#include "Dom/JsonValue.h"

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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
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
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = Contract::FDiagnostic::Failure(TEXT("physics_deformer_graph_profile_missing"),
					TEXT("No graph activation profile is registered for this command."), TEXT("runtime_factory"));
				OutError.CandidateName = CommandId;
				OutError.PropertyPath = TEXT("graph_profile.available");
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			if (!Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = Match->GetValidationDiagnostic();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return true;
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>&
			OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, OutError)) return false;
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId.StartsWith(TEXT("animation.")))
			{
				continue;
			}
			const auto* Match = Canonical->FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = Contract::FDiagnostic::Failure(TEXT("physics_deformer_timeline_profile_missing"),
					TEXT("No timeline/spatial activation profile is registered for this command."), TEXT("runtime_factory"));
				OutError.CandidateName = CommandId;
				OutError.PropertyPath = TEXT("timeline_profile.available");
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			if (!Match->TryValidate(OutError))
			{
				OutProfiles.Reset();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return true;
	}
}
