#include "Module/CommandRegistration/AiCommands/UeAgentAiCommandRegistration.h"

#include "CommandHandlers/AiCommands/UeAgentAiCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::AiCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("ai-command-owner");

		FString WireType(EUeAgentCommandWireType Type)
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
				OutError = TEXT("ai_command_contract_missing:") + Id;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t53_ai_command_migration");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact AI mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native AI result with revision and independent readback evidence."),
			}};
			Result.Errors = {
				{TEXT("ai_command_authority_required"),
					TEXT("The caller lacks public operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact schema rejects the request before mutation.")},
				{TEXT("graph_compound_operation_failed"),
					TEXT("The native AI lifecycle did not verify completion.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Operation service and exact AI mechanics.");
			Result.Purpose.Target =
				TEXT("One exact behavior tree, blackboard, EQS, Smart Object, or StateTree target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and required revision before execution."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance =
				TEXT("Use native revision and independent persisted asset readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Mutates only the exact AI target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("ai"), TEXT("behavior"), TEXT("operation")};
			Result.Purpose.Synonyms = {TEXT("AI authoring")};
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> In)
				: Declarations(MoveTemp(In)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("ai-command-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out,
				FString& OutError) const override
			{
				return CommandHandlers::AiCommands::TryCreateHandlers(Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("ai-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::AiCommands::TryGetActivationProfiles(Profiles, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					Profiles, Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : DomainAdapters::AiCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
