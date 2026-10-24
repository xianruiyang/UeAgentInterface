#include "Module/CommandRegistration/ViewportGraph/UeAgentViewportGraphRegistration.h"

#include "Dom/JsonValue.h"

#include "CommandHandlers/ViewportGraph/UeAgentViewportGraphCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"
#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::ViewportGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("viewport-graph-control-owner"));

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
			const Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile& Profile,
			const Contract::FUeAgentOperationDescriptor& Descriptor,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("viewport_graph_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::ViewportGraph::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("viewport_graph");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				bQuery ? FString(TEXT("editor_current_state"))
					: FString(TEXT("editor_transient_state")),
			};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				FUeAgentCommandFieldDeclaration Field{
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact viewport or graph mechanics parameter."),
				};
				if (!Composition::TimelineSpatial::
						TryBuildTimelineSpatialPublicParameterSchema(
							Profile,
							Descriptor,
							Parameter.GetName(),
							Field.SchemaJson,
							OutError))
				{
					return false;
				}
				Result.InputFields.Add(MoveTemp(Field));
			}
			if (Descriptor.GetCapabilityId() != CommandId
				|| Descriptor.GetInputSchema().GetJsonValue().IsEmpty()
				|| Result.InputFields.ContainsByPredicate(
					[](const FUeAgentCommandFieldDeclaration& Field)
					{
						return Field.SchemaJson.IsEmpty();
					}))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_descriptor_schema_projection_invalid"), Descriptor.GetCapabilityId() != CommandId ? TEXT("The descriptor capability differs from the command ID.") : Descriptor.GetInputSchema().GetJsonValue().IsEmpty() ? TEXT("The descriptor input schema is empty.") : TEXT("A projected command field has an empty schema."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				OutError.PropertyPath = TEXT("input_schema");
				if (Descriptor.GetCapabilityId() != CommandId)
				{
					OutError.PropertyPath = TEXT("capability_id");
					OutError.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
					OutError.ActualValue = MakeShared<FJsonValueString>(Descriptor.GetCapabilityId());
				}
				else if (const auto* InvalidField = Result.InputFields.FindByPredicate([](const auto& Field) { return Field.SchemaJson.IsEmpty(); }))
				{
					OutError.PropertyPath = TEXT("input_fields.") + InvalidField->Name;
				}
				return false;
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native viewport or graph result with state revision."),
			}};
			Result.Errors = {
				{TEXT("viewport_graph_authority_required"),
					TEXT("The caller lacks Query or Operation authority.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact viewport request is invalid.")},
				{TEXT("viewport_graph_runtime_unavailable"),
					TEXT("The native Editor viewport mechanics are unavailable.")},
				{TEXT("operation_state_unknown"),
					TEXT("A transient-state mutation requires reconciliation and restore.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native viewport mechanics.");
			Result.Purpose.Target =
				TEXT("One exact graph view, viewport client, actor selection, ray, or transient Editor state.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact viewport context and capture recoverable state before mutation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.usage fields and explicit viewport identities.");
			Result.Purpose.ResultGuidance =
				TEXT("Use the returned state revision and a separate read command for verification.");
			Result.Purpose.FailureGuidance =
				TEXT("Read failures are side-effect free; mutation failures preserve restore semantics.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only bounded Editor or viewport state.")
				: TEXT("Mutates only recoverable Editor transient state and must be restored.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::ViewportGraph::GetReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("viewport"), TEXT("graph"), TEXT("editor"), TEXT("control"),
			};
			Result.Purpose.Synonyms = {
				TEXT("viewport state"), TEXT("graph view"), TEXT("camera control"),
			};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		const Contract::FUeAgentOperationDescriptor* FindUniqueDescriptor(
			const FString& CommandId,
			const Contract::FUeAgentDescriptorRegistry& Descriptors,
			const Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile*& OutProfile,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutProfile = nullptr;
			const Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile* Match = nullptr;
			const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Profiles = nullptr;
			if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Profiles, OutError)) return nullptr;
			for (const Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile& Profile : *Profiles)
			{
				if (Profile.CommandId != CommandId)
				{
					continue;
				}
				if (Match)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_descriptor_ambiguous"), TEXT("More than one activation profile is registered for this command."), TEXT("registration"), FString(), CommandId);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					OutError.ActualValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(Match->DescriptorIdentity.GetDescriptorId()), MakeShared<FJsonValueString>(Profile.DescriptorIdentity.GetDescriptorId())});
					return nullptr;
				}
				Match = &Profile;
			}
			if (!Match)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_descriptor_profile_missing"), TEXT("No activation profile is registered for this command."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return nullptr;
			}
			const Contract::FUeAgentOperationDescriptor* Descriptor =
				Descriptors.FindExact(Match->DescriptorIdentity);
			if (!Descriptor || Descriptor->GetCapabilityId() != CommandId)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_descriptor_missing"), Descriptor ? TEXT("The operation descriptor capability does not match the command.") : TEXT("No operation descriptor was found for the command binding."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				if (Descriptor)
				{
					OutError.PropertyPath = TEXT("capability_id");
					OutError.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
					OutError.ActualValue = MakeShared<FJsonValueString>(Descriptor->GetCapabilityId());
				}
				return nullptr;
			}
			OutProfile = Match;
			return Descriptor;
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
				static const FString Id(TEXT("viewport-graph-control"));
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::ViewportGraph::TryCreateHandlers(
					OutHandlers,
					OutError);
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
					TEXT("viewport-graph-control"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& CommandId
				: QueryOwners::ViewportGraph::GetCommandIds())
			{
				if (QueryOwners::ViewportGraph::IsQuery(CommandId))
				{
					continue;
				}
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
					OperationOwners::ViewportGraph::
						FindOperationInvocationFactory(CommandId);
				if (!Factory.IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_operation_factory_missing"), TEXT("No operation invocation factory is registered for this command."), TEXT("registration"), FString(), CommandId);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
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
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::RecoverableSession;
				Operation.bPersists = false;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::AuthoringMemory);
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("viewport-graph-control"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
		if (!Composition::TimelineSpatial::
				TryCreateTimelineSpatialProductionCommandContractRegistry(
					Legacy,
					OutError)
			|| !Legacy.IsValid()
			|| !Composition::TimelineSpatial::
				TryCreateTimelineSpatialDescriptorRegistry(Descriptors, OutError)
			|| !Descriptors.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::ViewportGraph::GetCommandIds())
		{
			const Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile* Profile = nullptr;
			const Contract::FUeAgentOperationDescriptor* Descriptor =
				FindUniqueDescriptor(CommandId, *Descriptors, Profile, OutError);
			if (!Descriptor || !Profile)
			{
				return false;
			}
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(
					CommandId,
					*Legacy,
					*Profile,
					*Descriptor,
					Declaration,
					OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
