#include "Module/CommandRegistration/WorldCommands/UeAgentWorldCommandRegistration.h"

#include "CommandHandlers/WorldCommands/UeAgentWorldCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"
#include "OperationOwners/WorldCommands/UeAgentWorldHlodDomainService.h"
#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::WorldCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("world-command-owner"));
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			HlodProviderRegistration(
				TEXT("world-hlod-domain-provider"),
				&OperationOwners::WorldCommands::TryCreateWorldHlodDomainProvider);

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
				OutError = TEXT("world_command_contract_missing:") + CommandId;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("world");
			Result.PythonFunction = CommandId;
			Result.PythonFunction.RemoveFromStart(TEXT("world."));
			Result.PythonFunction.ReplaceInline(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {TEXT("world_authoring_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact native World mechanics parameter."),
				});
			}
			Result.ResultFields = {
				{TEXT("operation_id"), TEXT("string"), true, false,
					TEXT("Stable identity of the completed World operation.")},
				{TEXT("projection"), TEXT("object"), true, false,
					TEXT("Verified native outcome and independent readback evidence.")},
			};
			Result.Errors = {
				{TEXT("world_command_authority_required"),
					TEXT("The caller lacks uai:operate authority.")},
				{TEXT("world_command_runtime_unavailable"),
					TEXT("The Operation owner cannot bind the current Editor world.")},
				{TEXT("operation_state_unknown"),
					TEXT("Reconcile final World state before retrying.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common OperationService and native World mechanics.");
			Result.Purpose.Target =
				TEXT("One explicit actor, data layer, streaming relation, wall, or world-partition target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact current world and all referenced assets before mutation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.detail fields; do not infer legacy payloads.");
			Result.Purpose.ResultGuidance =
				TEXT("Use terminal operation state and an independent World query readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; state_unknown requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Mutates only the declared current-world target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::WorldCommands::GetIndependentReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("world"), TEXT("data layer"), TEXT("streaming"), TEXT("HLOD"),
			};
			Result.Purpose.Synonyms = {
				TEXT("World Partition"), TEXT("level streaming"), TEXT("wall authoring"),
			};
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
				static const FString Id(TEXT("world-command-migration"));
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
				return CommandHandlers::WorldCommands::TryCreateHandlers(
					OutHandlers, OutError);
			}

		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("world-command-migration"), &TryCreateContribution);

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
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				HjsonProvider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::WorldCommands::TryCreateWorldHlodDomainProvider(
					HjsonProvider, WorkspaceError)
				|| !HjsonProvider.IsValid())
			{
				OutError = WorkspaceError.GetCode();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("world_commands"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(CommandContribution.ToSharedRef());
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(HjsonProvider.ToSharedRef());
			Declaration.Hjson.Routes.Add({
				HjsonProvider->GetProviderId(),
				TEXT("content_graph.import_workspace"),
				TEXT("uai.world_hlod_layer.edit/1"),
				TEXT("ue.world.hlod_layer.asset"),
				TEXT("/Script/Engine.HLODLayer"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.world_hlod_layer.root/1"),
				true,
			});
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds.Add(
				TEXT("uai.init.world_hlod_layer.root/1"));

			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& CommandId
				: QueryOwners::WorldCommands::GetCommandIds())
			{
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = CommandId;
				Operation.Kind = CommandId.EndsWith(TEXT(".create"))
					|| CommandId == TEXT("world.wall.create_with_opening")
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
				Operation.InvocationFactory =
					OperationOwners::WorldCommands::
						FindOperationInvocationFactory(CommandId);
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T59-WORLD-COMMAND-MIGRATION");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("world_commands"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::WorldCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(CommandId, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
