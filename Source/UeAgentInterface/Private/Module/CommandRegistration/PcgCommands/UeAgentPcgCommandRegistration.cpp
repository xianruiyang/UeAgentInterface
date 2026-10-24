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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pcg_command_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t52_pcg_command_migration");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact PCG mechanics parameter."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native PCG result with revision or spatial readback evidence."),
			}};
			if (Id.EndsWith(TEXT(".asset.create"))
				|| Id == TEXT("pcg.data_asset.create_from_level"))
			{
				Result.ModelResultFields = {{TEXT("target_ref"), {TEXT("target_ref")}}};
			}
			Result.Errors = {
				{TEXT("pcg_command_authority_required"),
					TEXT("The caller lacks the Operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact graph schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure requires reconciliation.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Operation service and exact PCG mechanics.");
			Result.Purpose.Target = TEXT("One exact PCG component, graph, instance, parameter, or data asset target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and expected revision before execution."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.usage fields and explicit target identities.");
			Result.Purpose.ResultGuidance = TEXT("Use native terminal state and independent persisted PCG readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = TEXT("Mutates only the exact declared PCG target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.usage"), TEXT("PCG asset or spatial readback"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			GraphProfiles;
		if (!DomainAdapters::PcgCommands::TryGetGraphProfiles(
				GraphProfiles, OutError))
		{
			return false;
		}
		const TArray<FString>& PublicCommandIds =
			DomainAdapters::PcgCommands::GetPublicCommandIds();
		GraphProfiles.RemoveAll(
			[&PublicCommandIds](const auto& Profile)
			{
				return !PublicCommandIds.Contains(Profile.GetCommandId());
			});
		TSharedPtr<const FUeAgentCommandContractRegistry> GraphLegacy;
		if (!Composition::GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					GraphProfiles, GraphLegacy, OutError)
			|| !GraphLegacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : PublicCommandIds)
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *GraphLegacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
