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
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact Timeline/Sequence mechanics parameter."),
				});
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
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native TimelineAnimation mechanics.");
			Result.Purpose.Target =
				TEXT("One exact Level Sequence or Widget Blueprint animation target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target, temporal range, bindings, and expected revision."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
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
