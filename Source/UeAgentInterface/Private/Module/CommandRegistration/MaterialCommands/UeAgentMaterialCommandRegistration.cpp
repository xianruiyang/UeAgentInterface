#include "Module/CommandRegistration/MaterialCommands/UeAgentMaterialCommandRegistration.h"

#include "CommandHandlers/MaterialCommands/UeAgentMaterialCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"
#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::MaterialCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("material-command-owner");

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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("material_command_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			const bool bQuery = DomainAdapters::MaterialCommands::IsQuery(Id);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			const int32 Dot = Id.Find(TEXT("."));
			Result.PythonNamespace = Dot == INDEX_NONE ? Id : Id.Left(Dot);
			Result.PythonFunction = Dot == INDEX_NONE
				? Id
				: Id.Mid(Dot + 1).Replace(TEXT("."), TEXT("_"));
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
					TEXT("Exact material mechanics parameter."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native material result with revision and readback evidence."),
			}};
			if (Id.EndsWith(TEXT(".asset.create")))
			{
				Result.ModelResultFields = {{TEXT("target_ref"), {TEXT("target_ref")}}};
			}
			Result.Errors = {
				{TEXT("material_command_authority_required"),
					TEXT("The caller lacks the public query or operation authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact schema rejects the request before mutation.")},
				{TEXT("graph_compound_operation_failed"),
					TEXT("The native material lifecycle did not verify completion.")},
				{TEXT("graph_compound_material_target_invalid"),
					TEXT("The exact material target is invalid.")},
				{TEXT("graph_compound_material_parent_invalid"),
					TEXT("The material instance parent is invalid.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Operation service and material mechanics.");
			Result.Purpose.Target = TEXT("One exact material, material function, or material instance asset.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and required revision before execution."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance = TEXT("Use native revision and independent material asset readback evidence.");
			Result.Purpose.FailureGuidance = TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = TEXT("Mutates only the exact material target through native mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("material"), TEXT("graph"), TEXT("operation")};
			Result.Purpose.Synonyms = {TEXT("Material authoring")};
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
				static const FString Id(TEXT("material-command-migration"));
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
				return CommandHandlers::MaterialCommands::TryCreateHandlers(
					Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("material-command-migration"), &TryCreateContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("material-commands"));
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& OperationId
				: DomainAdapters::MaterialCommands::GetCommandIds())
			{
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
						OperationOwners::MaterialCommands::
							FindOperationInvocationFactory(OperationId);
				if (!Factory.IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_operation_factory_missing"), TEXT("No operation invocation factory is registered for this operation."), TEXT("registration"), FString(), OperationId);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = OperationId;
					return false;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = OperationId;
				Operation.Kind = OperationId.EndsWith(TEXT(".create"))
					? DomainCapability::EDomainOperationKind::Create
					: DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::None;
				Operation.bPersists = true;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes = {
				StateSemantic::EUeAgentStatePlane::AuthoringMemory,
				StateSemantic::EUeAgentStatePlane::Persisted,
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("material-commands"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			Profiles;
		if (!DomainAdapters::MaterialCommands::TryGetActivationProfiles(
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
			: DomainAdapters::MaterialCommands::GetPublicCommandIds())
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


