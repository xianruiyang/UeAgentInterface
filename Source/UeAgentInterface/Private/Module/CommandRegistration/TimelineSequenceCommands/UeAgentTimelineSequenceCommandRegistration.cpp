#include "Module/CommandRegistration/TimelineSequenceCommands/UeAgentTimelineSequenceCommandRegistration.h"

#include "CommandHandlers/TimelineSequenceCommands/UeAgentTimelineSequenceCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"
#include "QueryOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::TimelineSequenceCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner(TEXT("timeline-sequence-command-owner"));

		FString WireType(const EUeAgentCommandWireType Type)
		{
			switch (Type)
			{
			case EUeAgentCommandWireType::Boolean: return TEXT("boolean");
			case EUeAgentCommandWireType::Integer: return TEXT("integer");
			case EUeAgentCommandWireType::Number: return TEXT("number");
			case EUeAgentCommandWireType::String: return TEXT("string");
			case EUeAgentCommandWireType::Object: return TEXT("object");
			case EUeAgentCommandWireType::Array: return TEXT("array");
			default: return TEXT("any");
			}
		}

		bool TryDeclaration(
			const FString& CommandId,
			const FUeAgentCommandContractRegistry& Legacy,
			const TArray<Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile>& Profiles,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("timeline_sequence_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::TimelineSequenceCommands::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("t55_timeline_sequence_command_migration");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None : EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				FUeAgentCommandFieldDeclaration Field{
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact Timeline/Sequence mechanics parameter."),
					Parameter.GetSchemaJson(),
				};
				Result.InputFields.Add(MoveTemp(Field));
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native sequence or widget animation result with revision."),
			}};
			if (const auto* Profile = Profiles.FindByPredicate(
					[&CommandId](const auto& Candidate)
					{
						return Candidate.CommandId == CommandId;
					}))
			{
				Result.ModelResultFields = Profile->ModelResultFields;
			}
			Result.Errors = {
				{TEXT("timeline_sequence_authority_required"),
					TEXT("The caller lacks Query or Operation authority.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact Timeline/Sequence request is invalid.")},
				{TEXT("timeline_sequence_runtime_unavailable"),
					TEXT("The native TimelineAnimation mechanics are unavailable.")},
				{TEXT("operation_state_unknown"),
					TEXT("The operation requires reconciliation before retry.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native TimelineAnimation mechanics.");
			Result.Purpose.Target =
				TEXT("One exact Level Sequence or Widget Blueprint animation target.");
			Result.Purpose.Preconditions = TArray<FString>{
					TEXT("Resolve the exact target, temporal range, bindings, and expected revision."),
				};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance =
				TEXT("Verify writes with timeline.sequence.read or asset readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Queries are side-effect free; failed writes preserve lifecycle diagnostics.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only persisted timeline state.")
				: TEXT("Mutates only the explicitly resolved timeline asset.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = QueryOwners::TimelineSequenceCommands::GetReadbackFlows();
			Result.Purpose.Keywords = {TEXT("timeline"), TEXT("sequence"), TEXT("animation")};
			Result.Purpose.Synonyms = {TEXT("Sequencer"), TEXT("widget animation")};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("timeline-sequence-command-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::TimelineSequenceCommands::TryCreateHandlers(
					OutHandlers, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("timeline-sequence-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile> Profiles;
		if (!QueryOwners::TimelineSequenceCommands::TryGetProfiles(Profiles, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::TimelineSpatial::TryCreateTimelineSpatialProductionCommandContractRegistry(
				Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId
			: DomainAdapters::TimelineSequenceCommands::GetPublicCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(
					CommandId,
					*Legacy,
					Profiles,
					Declaration,
					OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		FUeAgentCommandDeclaration Candidates;
		Candidates.CommandId = TEXT("timeline.track_candidates");
		Candidates.PythonNamespace = TEXT("timeline");
		Candidates.PythonFunction = TEXT("track_candidates");
		Candidates.ContractVersion = 1;
		Candidates.PublicOwner = Owner;
		Candidates.Availability = EUeAgentCommandAvailability::Enabled;
		Candidates.SideEffect = EUeAgentCommandSideEffect::None;
		Candidates.StateLayers = {TEXT("asset_persisted_state")};
		Candidates.InputFields = {
			{TEXT("target"), TEXT("string"), true, false, TEXT("Sequence or WidgetBlueprint name returned by content search, or its exact object path.")},
			{TEXT("animation_name"), TEXT("string"), false, false, TEXT("Animation name from the WidgetBlueprint animations map; omit for a sequence asset.")},
			{TEXT("binding_kind"), TEXT("string"), false, false, TEXT("WidgetAnimation object kind: widget, slot, or root_widget; LevelSequence uses possessable objects.")},
			{TEXT("object_ref"), TEXT("string"), false, false, TEXT("LevelSequence object path, or WidgetAnimation widget name for widget/slot; omit for root_widget.")},
			{TEXT("property_path"), TEXT("string"), false, false, TEXT("Native property path on the selected binding object.")},
			{TEXT("query"), TEXT("string"), false, false, TEXT("Optional readable track class name filter.")}};
		Candidates.ResultFields = {{TEXT("names"), TEXT("array"), true, false, TEXT("Native property track names supported by the target and HJSON channel owner.")}};
		Candidates.Errors = {
			{TEXT("timeline_track_candidate_params_invalid"), TEXT("The target or query input is invalid.")},
			{TEXT("timeline_track_candidate_query_failed"), TEXT("The target cannot supply native property track candidates.")}};
		Candidates.Purpose.Summary = TEXT("Find native property track types for ordinary HJSON creation.");
		Candidates.Purpose.Target = TEXT("One MovieSceneSequence asset or a WidgetBlueprint animation.");
		Candidates.Purpose.Preconditions = {TEXT("Run in the Editor with uai:query; use a current content-search name or exact target path.")};
		Candidates.Purpose.ParameterGuidance = TEXT("Copy target from public discovery; use a returned name in a new root_tracks or binding tracks entry.");
		Candidates.Purpose.ResultGuidance = TEXT("Names resolve in the same target context during import; ambiguous names return readable select details.");
		Candidates.Purpose.FailureGuidance = TEXT("Read the error detail for an unresolved target or unsupported sequence context.");
		Candidates.Purpose.SideEffectGuidance = TEXT("Read-only discovery; target tracks are unchanged.");
		Candidates.Purpose.MinimumExample = TEXT("client.timeline.track_candidates(target, query='Float')");
		Candidates.Purpose.RelatedFlows = {TEXT("content_graph.export_workspace"), TEXT("content_graph.import_workspace")};
		Candidates.Purpose.Keywords = {TEXT("sequence"), TEXT("track"), TEXT("candidate"), TEXT("hjson")};
		Declarations.Add(MoveTemp(Candidates));
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
