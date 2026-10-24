#include "Module/CommandRegistration/PhysicsDeformer/UeAgentPhysicsDeformerRegistration.h"

#include "CommandHandlers/PhysicsDeformer/UeAgentPhysicsDeformerCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/PhysicsDeformer/UeAgentPhysicsDeformerMechanicsBoundary.h"
#include "OperationOwners/PhysicsDeformer/UeAgentPhysicsDeformerOperationOwner.h"
#include "QueryOwners/PhysicsDeformer/UeAgentPhysicsDeformerQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::PhysicsDeformer
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("physics-deformer-operation-owner"));

		FString StableNamespace(const FString& Id)
		{
			int32 Separator = INDEX_NONE;
			return Id.FindChar(TEXT('.'), Separator)
				? Id.Left(Separator) : FString();
		}

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
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("physics_deformer_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("physics_deformer_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			const bool bQuery = DomainAdapters::PhysicsDeformer::IsQuery(Id);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = StableNamespace(Id);
			Result.PythonFunction = Id.RightChop(
				Result.PythonNamespace.Len() + 1).Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers =
				QueryOwners::PhysicsDeformer::GetStateLayers();
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact Physics/Deformer mechanics parameter."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native Physics/Deformer result with revision and readback evidence."),
			}};
			if (Id.EndsWith(TEXT(".asset.create")))
			{
				Result.ModelResultFields = {{TEXT("target_ref"), {TEXT("target_ref")}}};
			}
			Result.Errors = {
				{TEXT("physics_deformer_authority_required"),
					TEXT("The caller lacks the Query/Operation authority." )},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact graph schema rejects the request before mutation.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact physics/deformer schema rejects the request.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure requires reconciliation.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Query/Operation service and exact Physics/Deformer mechanics.");
			Result.Purpose.Target =
				TEXT("One exact physics, deformer asset, or simulation observation target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and expected revision before execution."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.usage fields and explicit target identities.");
			Result.Purpose.ResultGuidance =
				TEXT("Use native terminal state and an independent asset or simulation readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only a bounded deformer or simulation observation.")
				: TEXT("Mutates only the exact declared target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::PhysicsDeformer::GetIndependentReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("physics"), TEXT("deformer"), TEXT("simulation"), TEXT("operation"),
			};
			Result.Purpose.Synonyms = {
				TEXT("ML Deformer"), TEXT("physics asset"), TEXT("deformer graph"),
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
				static const FString Id(TEXT("physics-deformer-operation"));
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
				return CommandHandlers::PhysicsDeformer::TryCreateHandlers(
					Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<IUeAgentActiveCommandContribution> CommandContribution;
			if (!TryCreateContribution(CommandContribution, OutError)
				|| !CommandContribution.IsValid())
			{
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("physics_deformer"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& CommandId
				: DomainAdapters::PhysicsDeformer::GetCommandIds())
			{
				if (DomainAdapters::PhysicsDeformer::IsQuery(CommandId))
				{
					continue;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = CommandId;
				Operation.Kind = CommandId.EndsWith(TEXT(".asset.create"))
					? DomainCapability::EDomainOperationKind::Create
					: DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.TransientStatePolicy =
					CommandId == TEXT("physics.simulation.probe")
						? DomainCapability::EDomainTransientStatePolicy::
							RecoverableSession
						: DomainCapability::EDomainTransientStatePolicy::None;
				Operation.bPersists =
					CommandId != TEXT("physics.simulation.probe");
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory =
					OperationOwners::PhysicsDeformer::
						FindOperationInvocationFactory(CommandId);
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("physics_deformer"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			GraphProfiles;
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>
			TimelineProfiles;
		if (!DomainAdapters::PhysicsDeformer::TryGetGraphProfiles(
				GraphProfiles, OutError)
			|| !DomainAdapters::PhysicsDeformer::TryGetTimelineProfiles(
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
		for (const FString& Id
			: DomainAdapters::PhysicsDeformer::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			const FUeAgentCommandContractRegistry& Legacy =
				Id.StartsWith(TEXT("animation."))
					? *GraphLegacy
					: *TimelineLegacy;
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
