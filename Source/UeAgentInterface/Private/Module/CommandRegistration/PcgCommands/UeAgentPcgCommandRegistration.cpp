#include "Module/CommandRegistration/PcgCommands/UeAgentPcgCommandRegistration.h"

#include "CommandHandlers/PcgCommands/UeAgentPcgCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::PcgCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("pcg-command-owner"));

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
			const FString& Id,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("pcg_command_contract_missing:") + Id;
				return false;
			}
			const bool bQuery = DomainAdapters::PcgCommands::IsQuery(Id);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t52_pcg_command_migration");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = bQuery
				? TArray<FString>{TEXT("world_runtime_state")}
				: TArray<FString>{TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact PCG mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native PCG result with revision or spatial readback evidence."),
			}};
			Result.Errors = {
				{TEXT("pcg_command_authority_required"),
					TEXT("The caller lacks the Query/Operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact graph schema rejects the request before mutation.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact PCG spatial query schema rejects the request.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure requires reconciliation.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Query/Operation service and exact PCG mechanics.");
			Result.Purpose.Target = bQuery
				? TEXT("One exact PCG generation or partition observation target.")
				: TEXT("One exact PCG component, graph, instance, parameter, or data asset target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and expected revision before execution."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.detail fields and explicit target identities.");
			Result.Purpose.ResultGuidance = bQuery
				? TEXT("Use the independent native spatial observation returned by the Query service.")
				: TEXT("Use native terminal state and independent persisted PCG readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only a bounded PCG spatial observation.")
				: TEXT("Mutates only the exact declared PCG target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.detail"), TEXT("PCG asset or spatial readback"),
			};
			Result.Purpose.Keywords = {
				TEXT("pcg"), TEXT("procedural"), TEXT("graph"), TEXT("operation"),
			};
			Result.Purpose.Synonyms = {
				TEXT("Procedural Content Generation"), TEXT("PCG authoring"),
			};
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> In)
				: Declarations(MoveTemp(In))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("pcg-command-migration"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out,
				FString& OutError) const override
			{
				return CommandHandlers::PcgCommands::TryCreateHandlers(Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("pcg-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			GraphProfiles;
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>
			TimelineProfiles;
		if (!DomainAdapters::PcgCommands::TryGetGraphProfiles(
				GraphProfiles, OutError)
			|| !DomainAdapters::PcgCommands::TryGetTimelineProfiles(
				TimelineProfiles, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> GraphLegacy;
		TSharedPtr<const FUeAgentCommandContractRegistry> TimelineLegacy;
		if (!Composition::GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					GraphProfiles, GraphLegacy, OutError)
			|| !GraphLegacy.IsValid()
			|| !Composition::TimelineSpatial::
				TryCreateTimelineSpatialProductionCommandContractRegistry(
					TimelineLegacy, OutError)
			|| !TimelineLegacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : DomainAdapters::PcgCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			const FUeAgentCommandContractRegistry& Legacy =
				DomainAdapters::PcgCommands::IsQuery(Id)
					? *TimelineLegacy
					: *GraphLegacy;
			if (!TryDeclaration(Id, Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
