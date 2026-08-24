#include "Module/CommandRegistration/TimelineSequenceCommands/UeAgentTimelineSequenceCommandRegistration.h"

#include "CommandHandlers/TimelineSequenceCommands/UeAgentTimelineSequenceCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
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

		const FString& LevelSequenceSettingsInputSchema()
		{
			static const FString Schema = TEXT(R"json({
				"type":"object",
				"properties":{
					"display_rate":{
						"type":"object",
						"properties":{
							"numerator":{"type":"string","minLength":1},
							"denominator":{"type":"string","minLength":1}
						},
						"required":["numerator","denominator"],
						"additionalProperties":false
					},
					"playback_range":{
						"type":"object",
						"properties":{
							"clock_id":{"type":"string","minLength":1},
							"state":{"type":"string","const":"authored"},
							"lower":{
								"type":"object",
								"properties":{
									"kind":{"type":"string","const":"inclusive"},
									"point":{
										"type":"object",
										"properties":{
											"clock_id":{"type":"string","minLength":1},
											"tick":{"type":"string","minLength":1},
											"rate":{
												"type":"object",
												"properties":{
													"numerator":{"type":"string","minLength":1},
													"denominator":{"type":"string","minLength":1}
												},
												"required":["numerator","denominator"],
												"additionalProperties":false
											},
											"state":{"type":"string","const":"authored"}
										},
										"required":["clock_id","tick","rate","state"],
										"additionalProperties":false
									}
								},
								"required":["kind","point"],
								"additionalProperties":false
							},
							"upper":{
								"type":"object",
								"properties":{
									"kind":{"type":"string","const":"exclusive"},
									"point":{
										"type":"object",
										"properties":{
											"clock_id":{"type":"string","minLength":1},
											"tick":{"type":"string","minLength":1},
											"rate":{
												"type":"object",
												"properties":{
													"numerator":{"type":"string","minLength":1},
													"denominator":{"type":"string","minLength":1}
												},
												"required":["numerator","denominator"],
												"additionalProperties":false
											},
											"state":{"type":"string","const":"authored"}
										},
										"required":["clock_id","tick","rate","state"],
										"additionalProperties":false
									}
								},
								"required":["kind","point"],
								"additionalProperties":false
							}
						},
						"required":["clock_id","state","lower","upper"],
						"additionalProperties":false
					},
					"expected_revision":{
						"type":"object",
						"properties":{
							"source_id":{"type":"string","minLength":1},
							"generation":{"type":"string","minLength":1},
							"sequence":{"type":"string","minLength":1}
						},
						"required":["source_id","generation","sequence"],
						"additionalProperties":false
					}
				},
				"required":["expected_revision"],
				"anyOf":[
					{"required":["display_rate"]},
					{"required":["playback_range"]}
				],
				"additionalProperties":false
			})json");
			return Schema;
		}

		bool TryDeclaration(
			const FString& CommandId,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("timeline_sequence_contract_missing:") + CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::TimelineSequenceCommands::IsQuery(CommandId);
			const bool bSettingsUpdate =
				CommandId == TEXT("timeline.sequence.settings.update");
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
				};
				if (bSettingsUpdate && Parameter.GetName() == TEXT("input"))
				{
					Field.Description =
						TEXT("Non-destructive Level Sequence settings patch; use at least one supported setting and the exact native revision.");
					Field.SchemaJson = LevelSequenceSettingsInputSchema();
				}
				Result.InputFields.Add(MoveTemp(Field));
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native sequence or widget animation result with revision."),
			}};
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
			Result.Purpose.Summary = bSettingsUpdate
				? TEXT("Update Level Sequence display rate or playback range without replacing bindings, tracks, sections, channels, or keys.")
				: TEXT("Execute ") + CommandId
					+ TEXT(" through the common Query/Operation service and native TimelineAnimation mechanics.");
			Result.Purpose.Target = bSettingsUpdate
				? TEXT("One exact persisted Level Sequence target and its current native revision.")
				: TEXT("One exact Level Sequence or Widget Blueprint animation target.");
			Result.Purpose.Preconditions = bSettingsUpdate
				? TArray<FString>{
					TEXT("Read the exact target and native expected_revision before mutation."),
					TEXT("Provide display_rate, playback_range, or both; tick-resolution retiming is not part of this command."),
				}
				: TArray<FString>{
					TEXT("Resolve the exact target, temporal range, bindings, and expected revision."),
				};
			Result.Purpose.ParameterGuidance = bSettingsUpdate
				? TEXT("Copy the nested input schema from query.command.detail; preserve exact clock, rate, bound, and revision identities.")
				: TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = bSettingsUpdate
				? TEXT("Use timeline.sequence.read to verify settings changed and existing structure remained present after save and reload.")
				: TEXT("Verify writes with timeline.sequence.read or asset readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Queries are side-effect free; failed writes preserve lifecycle diagnostics.");
			Result.Purpose.SideEffectGuidance = bSettingsUpdate
				? TEXT("Mutates only display_rate and/or playback_range on the exact asset, then saves, reloads, and reads it back.")
				: bQuery
				? TEXT("Reads only persisted timeline state.")
				: TEXT("Mutates only the explicitly resolved timeline asset.");
			Result.Purpose.MinimumExample = bSettingsUpdate
				? TEXT("client.timeline.sequence_settings_update(target, input)")
				: Result.PythonFunction + TEXT("(client, params)");
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
				FString& OutError) const override
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
		FString& OutError)
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
		for (const FString& CommandId : QueryOwners::TimelineSequenceCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(CommandId, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
