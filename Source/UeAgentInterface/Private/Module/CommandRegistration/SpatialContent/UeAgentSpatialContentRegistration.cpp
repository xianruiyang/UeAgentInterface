#include "Module/CommandRegistration/SpatialContent/UeAgentSpatialContentRegistration.h"

#include "CommandHandlers/SpatialContent/UeAgentSpatialContentCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"
#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::SpatialContent
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("spatial-content-operation-owner"));

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
				OutError = TEXT("spatial_content_contract_missing:") + CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::SpatialContent::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("spatial_content");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				bQuery
					? FString(TEXT("editor_current_state"))
					: OperationOwners::SpatialContent::IsDurableMutation(CommandId)
						? FString(TEXT("persisted"))
						: FString(TEXT("authoring_memory")),
			};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact spatial-content mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native spatial-content result with state revision."),
			}};
			Result.Errors = {
				{TEXT("spatial_content_authority_required"),
					TEXT("The caller lacks Query or Operation authority.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact spatial-content request is invalid.")},
				{TEXT("spatial_content_runtime_unavailable"),
					TEXT("The native WorldSpatial mechanics are unavailable.")},
				{TEXT("operation_state_unknown"),
					TEXT("The operation requires reconciliation before retry.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native spatial mechanics.");
			Result.Purpose.Target =
				TEXT("One exact actor transform, mesh transform, pivot, element selection, or transform candidate set.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve explicit subject identities and semantic frames before execution."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.detail fields and explicit spatial identities.");
			Result.Purpose.ResultGuidance =
				TEXT("Verify mutation through a separate transform read or bounded domain readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Read failures are side-effect free; mutation failures preserve lifecycle diagnostics.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only bounded Editor spatial state.")
				: TEXT("Mutates only the explicitly resolved spatial subject.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::SpatialContent::GetReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("spatial"), TEXT("transform"), TEXT("mesh"), TEXT("world"),
			};
			Result.Purpose.Synonyms = {
				TEXT("object transform"), TEXT("mesh pivot"), TEXT("surface snap"),
			};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("spatial-content-operation"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return CommandHandlers::SpatialContent::TryCreateHandlers(
					OutHandlers,
					OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

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
					TEXT("spatial-content-operation"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& CommandId
				: QueryOwners::SpatialContent::GetCommandIds())
			{
				if (QueryOwners::SpatialContent::IsQuery(CommandId))
				{
					continue;
				}
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
					OperationOwners::SpatialContent::
						FindOperationInvocationFactory(CommandId);
				if (!Factory.IsValid())
				{
					OutError = TEXT("spatial_content_operation_factory_missing:")
						+ CommandId;
					return false;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = CommandId;
				Operation.Kind = DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.bPersists = OperationOwners::SpatialContent::
					IsDurableMutation(CommandId);
				Operation.TransientStatePolicy = Operation.bPersists
					? DomainCapability::EDomainTransientStatePolicy::None
					: DomainCapability::EDomainTransientStatePolicy::
						RecoverableSession;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::AuthoringMemory);
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::Persisted);
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T46-SPATIAL-CONTENT-OPERATION");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("spatial-content-operation"),
				&TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::TimelineSpatial::
				TryCreateTimelineSpatialProductionCommandContractRegistry(
					Legacy,
					OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::SpatialContent::GetCommandIds())
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
