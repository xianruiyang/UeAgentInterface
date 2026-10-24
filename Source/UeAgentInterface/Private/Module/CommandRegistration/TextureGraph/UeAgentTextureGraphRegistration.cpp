#include "Module/CommandRegistration/TextureGraph/UeAgentTextureGraphRegistration.h"

#include "CommandHandlers/TextureGraph/UeAgentTextureGraphCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphDomainService.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::TextureGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("texture-graph-operation-owner");

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
			const bool bBake = Id == TEXT("texture_graph.artifact.bake");
			const bool bProbe = Id == TEXT("texture_graph.runtime.probe");
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!bBake && !bProbe && (!Contract || !Contract->TryValidate(OutError)))
			{
				OutError = Contract ? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_contract_missing"), OutError, TEXT("registration")) : UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			const bool bQuery = Id == TEXT("texture_graph.artifact.capture")
				|| bProbe;
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("texture_graph");
			Result.PythonFunction = Id.RightChop(
				FString(TEXT("texture_graph.")).Len()).Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			if (bBake)
			{
				Result.InputFields = {
					{TEXT("target"), TEXT("string"), true, false,
						TEXT("Exact persisted TextureGraph package or object identity.")},
					{TEXT("outputs"), TEXT("array"), true, false,
						TEXT("Unique render_output_index and destination package pairs committed by one plan.")},
				};
			}
			else if (bProbe)
			{
				Result.InputFields = {
					{TEXT("target"), TEXT("string"), true, false,
						TEXT("Exact persisted TextureGraph package or object identity.")},
					{TEXT("render_output_index"), TEXT("integer"), false, false,
						TEXT("Zero-based rendered output index; defaults to zero.")},
					{TEXT("expected_stats"), TEXT("object"), false, false,
						TEXT("Optional pixel-stat expectations evaluated without persistence.")},
				};
			}
			else
			{
				for (const FUeAgentCommandParameter& Parameter
					: Contract->GetParameters())
				{
					Result.InputFields.Add({
						Parameter.GetName(), WireType(Parameter.GetWireType()),
						Parameter.IsRequired(), Parameter.IsNullable(),
						TEXT("Exact TextureGraph mechanics parameter."),
						Parameter.GetSchemaJson(),
					});
				}
			}
			if (bBake)
			{
				Result.ResultFields = {
					{TEXT("terminal_status"), TEXT("string"), true, false,
						TEXT("Succeeded only after persistence and independent readback.")},
					{TEXT("target"), TEXT("string"), true, false,
						TEXT("The unchanged TextureGraph target identity.")},
					{TEXT("outputs"), TEXT("array"), true, false,
						TEXT("Reloaded Texture2D identities and pixel statistics.")},
					{TEXT("asset_refs"), TEXT("array"), true, false,
						TEXT("Compact identities of the verified created Texture2D assets.")},
				};
				Result.ModelResultFields = {{
					TEXT("asset_refs"), {TEXT("asset_refs")}}};
			}
			else if (bProbe)
			{
				Result.ResultFields = {
					{TEXT("command_id"), TEXT("string"), true, false,
						TEXT("The exact texture_graph.runtime.probe identity.")},
					{TEXT("target"), TEXT("string"), true, false,
						TEXT("The unchanged TextureGraph target identity.")},
					{TEXT("render_output_index"), TEXT("integer"), true, false,
						TEXT("The rendered output selected for evaluation.")},
					{TEXT("render_output_count"), TEXT("integer"), true, false,
						TEXT("The total real render outputs observed.")},
					{TEXT("pixel_stats"), TEXT("object"), true, false,
						TEXT("Dimensions and real pixel readback statistics.")},
					{TEXT("expected_stats"), TEXT("object"), false, false,
						TEXT("The optional expectation object evaluated by the probe.")},
					{TEXT("probe_passed"), TEXT("boolean"), true, false,
						TEXT("Whether every supplied expected-stat bound passed.")},
				};
			}
			else
			{
				Result.ResultFields = {{
					TEXT("result"), TEXT("object"), true, false,
					TEXT("Native TextureGraph result with revision and readback evidence."),
				}};
			}
			Result.Errors = {
				{TEXT("texture_graph_authority_required"),
					TEXT("The caller lacks uai:operate authority.")},
				{TEXT("graph_compound_input_invalid"),
					TEXT("The exact schema rejects the request before mutation.")},
				{TEXT("graph_compound_operation_failed"),
					TEXT("The native TextureGraph lifecycle did not verify completion.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common Query/Operation service and TextureGraph mechanics.");
			Result.Purpose.Target = TEXT("One exact persisted TextureGraph asset target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact target and required revision before execution."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance = TEXT("Use the native revision and independent readback evidence.");
			Result.Purpose.FailureGuidance = TEXT("Prewrite failures leave no residue; unknown state requires reconciliation.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Read-only artifact capture from a settled graph.")
				: TEXT("Mutates only the exact TextureGraph target through native mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("texture graph"), TEXT("graph"), TEXT("operation")};
			Result.Purpose.Synonyms = {TEXT("TextureGraph authoring")};
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TArray<FUeAgentCommandDeclaration> In,
				TSharedRef<const DomainCapability::
					IDomainOperationInvocationFactory> InBakeFactory)
				: Declarations(MoveTemp(In))
				, BakeFactory(MoveTemp(InBakeFactory))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("texture-graph-operation"));
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
				return CommandHandlers::TextureGraph::TryCreateHandlers(
					BakeFactory, Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory> BakeFactory;
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
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				HjsonProvider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::TextureGraph::TryCreateTextureGraphDomainProvider(
					HjsonProvider,
					WorkspaceError)
				|| !HjsonProvider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}

			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("texture-graph"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(HjsonProvider.ToSharedRef());
			Declaration.Hjson.Routes.Add({
				HjsonProvider->GetProviderId(),
				TEXT("content_graph.import_workspace"),
				HjsonWorkspace::GetSemanticWorkspaceSchemaId(),
				TEXT("ue.texture_graph.asset"),
				TEXT("/Script/TextureGraph.TextureGraph"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.texture_graph.root/1"),
				true,
			});
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds.Add(
				TEXT("uai.init.texture_graph.root/1"));

			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			DomainCapability::FDomainOperationEntry Bake;
			Bake.OperationId = TEXT("texture_graph.artifact.bake");
			Bake.Kind = DomainCapability::EDomainOperationKind::Create;
			Bake.TerminalPolicy = DomainCapability::EDomainOperationTerminalPolicy::
				SucceededRejectedRolledBackOrStateUnknown;
			Bake.ExecutionPolicy =
				DomainCapability::EDomainOperationExecutionPolicy::Immediate;
			Bake.TransientStatePolicy =
				DomainCapability::EDomainTransientStatePolicy::None;
			Bake.bPersists = true;
			Bake.bIndependentReadback = true;
			Bake.InvocationFactory = OperationOwners::TextureGraph::
				GetTextureGraphBakeInvocationFactory();
			Declaration.Operation.Operations.Add(MoveTemp(Bake));
			const TArray<FString> GraphOperationIds = {
				TEXT("texture_graph.graph.link.mutate"),
				TEXT("texture_graph.graph.node.mutate"),
				TEXT("texture_graph.graph.parameter.mutate"),
				TEXT("texture_graph.graph.validate_evaluate_settle"),
			};
			for (const FString& OperationId : GraphOperationIds)
			{
				DomainCapability::FDomainOperationEntry GraphOperation;
				GraphOperation.OperationId = OperationId;
				GraphOperation.Kind =
					DomainCapability::EDomainOperationKind::Write;
				GraphOperation.TerminalPolicy =
					DomainCapability::EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				GraphOperation.ExecutionPolicy =
					DomainCapability::EDomainOperationExecutionPolicy::Immediate;
				GraphOperation.TransientStatePolicy =
					DomainCapability::EDomainTransientStatePolicy::None;
				GraphOperation.bPersists = true;
				GraphOperation.bIndependentReadback = true;
				GraphOperation.InvocationFactory =
					OperationOwners::TextureGraph::
						FindTextureGraphOperationInvocationFactory(OperationId);
				Declaration.Operation.Operations.Add(MoveTemp(GraphOperation));
			}
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration),
				OutContribution,
				OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("texture-graph"),
				&TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
			Profiles;
		if (!DomainAdapters::TextureGraph::TryGetActivationProfiles(
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
		for (const FString& Id : DomainAdapters::TextureGraph::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations),
			OperationOwners::TextureGraph::
				GetTextureGraphBakeInvocationFactory());
		return true;
	}
}
