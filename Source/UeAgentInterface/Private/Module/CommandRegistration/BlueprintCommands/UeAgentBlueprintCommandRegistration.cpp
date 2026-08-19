#include "Module/CommandRegistration/BlueprintCommands/UeAgentBlueprintCommandRegistration.h"

#include "CommandHandlers/BlueprintCommands/UeAgentBlueprintCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("blueprint-command-owner");

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
				OutError = TEXT("blueprint_command_contract_missing:") + Id;
				return false;
			}
			const bool bQuery = DomainAdapters::BlueprintCommands::IsQuery(Id);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t47_blueprint_command_migration");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact Blueprint mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native Blueprint result with revision and readback evidence."),
			}};
			Result.Errors = {
				{TEXT("blueprint_command_authority_required"),
					TEXT("The caller lacks the public query or operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact schema rejects the request before mutation.")},
				{TEXT("graph_compound_blueprint_parent_invalid"),
					TEXT("The requested parent class is not Blueprint-capable.")},
				{TEXT("graph_compound_operation_failed"),
					TEXT("The native Blueprint lifecycle did not verify completion.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Query/Operation service and Blueprint mechanics.");
			Result.Purpose.Target = TEXT("One exact Blueprint asset or component target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and required revision before execution."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Use native revision and independent Blueprint readback evidence.");
			Result.Purpose.FailureGuidance = TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Read-only component event discovery.")
				: TEXT("Mutates only the exact Blueprint target through native mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("blueprint"), TEXT("component"), TEXT("operation")};
			Result.Purpose.Synonyms = {TEXT("Blueprint authoring")};
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
				static const FString Id(TEXT("blueprint-command-migration"));
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
				return CommandHandlers::BlueprintCommands::TryCreateHandlers(
					Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("blueprint-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			Profiles;
		if (!DomainAdapters::BlueprintCommands::TryGetActivationProfiles(
				Profiles, OutError))
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
		for (const FString& Id
			: DomainAdapters::BlueprintCommands::GetCommandIds())
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
