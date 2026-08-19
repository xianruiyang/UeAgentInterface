#include "Module/CommandRegistration/Reference/UeAgentReferenceRegistration.h"

#include "CommandHandlers/Reference/UeAgentT43ReferenceCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/Reference/UeAgentReferenceMechanicsBoundary.h"
#include "OperationOwners/Reference/UeAgentReferenceOperationOwner.h"
#include "QueryOwners/Reference/UeAgentReferenceQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::Reference
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("reference-operation-owner"));

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
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			const FUeAgentCommandContract* Legacy =
				GetDefaultCommandContractRegistry()->FindExact(
					DomainAdapters::Reference::GetCommandId());
			if (!Legacy || !Legacy->IsValid())
			{
				OutError = TEXT("reference_contract_missing");
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = DomainAdapters::Reference::GetCommandId();
			Result.PythonNamespace = TEXT("reference");
			Result.PythonFunction = TEXT("asset_apply");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = QueryOwners::Reference::GetStateLayers();
			for (const FUeAgentCommandParameter& Parameter
				: Legacy->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact reference mutation parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"),
				TEXT("object"),
				true,
				false,
				TEXT("Terminal reference mutation result and persisted readback."),
			}};
			Result.Errors = {
				{TEXT("reference_authority_required"),
					TEXT("The caller lacks uai:operate authority.")},
				{TEXT("reference_asset_intent_invalid"),
					TEXT("The declared create, update, replace, or delete intent is invalid.")},
				{TEXT("reference_operation_result_invalid"),
					TEXT("The native reference mechanics returned an invalid terminal result.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure requires reconciliation.")},
			};
			Result.Purpose.Summary =
				TEXT("Apply one exact reference asset mutation through OperationService and the existing ReferenceCrud mechanics.");
			Result.Purpose.Target =
				TEXT("One normalized reference asset object path under the allowed project root.");
			Result.Purpose.Preconditions = {
				TEXT("Choose an explicit intent and exact object path before execution."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use object_path, intent, and proposed_text exactly as described by query.command.detail.");
			Result.Purpose.ResultGuidance =
				TEXT("Use the terminal revision and verify it through an independent public read.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite rejection leaves no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Mutates only the exact reference asset through durable native mechanics.");
			Result.Purpose.MinimumExample =
				TEXT("reference_asset_apply(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::Reference::GetIndependentReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("reference"), TEXT("asset"), TEXT("operation"),
			};
			Result.Purpose.Synonyms = {
				TEXT("reference asset write"), TEXT("reference mutation"),
			};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(FUeAgentCommandDeclaration InDeclaration)
				: Declaration(MoveTemp(InDeclaration))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("reference-operation"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(Declaration);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return CommandHandlers::Reference::TryCreateHandlers(
					OutHandlers,
					OutError);
			}
		private:
			FUeAgentCommandDeclaration Declaration;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("reference-operation"),
			&TryCreateContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			FString& OutError)
		{
			TSharedPtr<IUeAgentActiveCommandContribution> CommandContribution;
			if (!TryCreateContribution(CommandContribution, OutError)
				|| !CommandContribution.IsValid())
			{
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("reference-operation"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			DomainCapability::FDomainOperationEntry Operation;
			Operation.OperationId = DomainAdapters::Reference::GetCommandId();
			Operation.Kind = DomainCapability::EDomainOperationKind::Write;
			Operation.TerminalPolicy = DomainCapability::
				EDomainOperationTerminalPolicy::
					SucceededRejectedRolledBackOrStateUnknown;
			Operation.ExecutionPolicy = DomainCapability::
				EDomainOperationExecutionPolicy::Immediate;
			Operation.TransientStatePolicy = DomainCapability::
				EDomainTransientStatePolicy::None;
			Operation.bPersists = true;
			Operation.bIndependentReadback = true;
			Operation.InvocationFactory = OperationOwners::Reference::
				FindOperationInvocationFactory();
			Declaration.Operation.Operations.Add(MoveTemp(Operation));
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::Persisted);
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T43-REFERENCE-OPERATION");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("reference-operation"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		FUeAgentCommandDeclaration Declaration;
		if (!TryDeclaration(Declaration, OutError))
		{
			return false;
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declaration));
		return true;
	}
}
