#include "Module/CommandRegistration/UmgCommands/UeAgentUmgCommandRegistration.h"

#include "CommandHandlers/UmgCommands/UeAgentUmgCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/UmgCommands/UeAgentUmgCommandMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::UmgCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("umg-command-owner");

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
				OutError = TEXT("umg_command_contract_missing:") + Id;
				return false;
			}
			const bool bQuery = DomainAdapters::UmgCommands::IsQuery(Id);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t48_umg_command_migration");
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
					TEXT("Exact UMG mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native UMG result with revision and readback evidence."),
			}};
			Result.Errors = {
				{TEXT("umg_command_authority_required"),
					TEXT("The caller lacks the public query or operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact schema rejects the request before mutation.")},
				{TEXT("graph_compound_operation_failed"),
					TEXT("The native UMG lifecycle did not verify completion.")},
			};
			if (bQuery)
			{
				Result.Errors.Add({TEXT("graph_compound_umg_event_owner_invalid"),
					TEXT("The target or widget owner identity is invalid.")});
				Result.Errors.Add({TEXT("graph_compound_event_owner_not_found"),
					TEXT("The exact widget owner does not exist in the target.")});
			}
			else
			{
				Result.Errors.Add({TEXT("graph_compound_asset_target_already_exists"),
					TEXT("The create target already exists before mutation.")});
			}
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Query/Operation service and UMG mechanics.");
			Result.Purpose.Target = TEXT("One exact Widget Blueprint asset or widget target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and required revision before execution."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Use native revision and independent Widget Blueprint readback evidence.");
			Result.Purpose.FailureGuidance = TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Read-only widget event discovery.")
				: TEXT("Mutates only the exact UMG target through native mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("umg"), TEXT("widget"), TEXT("operation")};
			Result.Purpose.Synonyms = {TEXT("UMG authoring")};
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
				static const FString Id(TEXT("umg-command-migration"));
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
				return CommandHandlers::UmgCommands::TryCreateHandlers(
					Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("umg-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			Profiles;
		if (!DomainAdapters::UmgCommands::TryGetActivationProfiles(
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
			: DomainAdapters::UmgCommands::GetCommandIds())
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
