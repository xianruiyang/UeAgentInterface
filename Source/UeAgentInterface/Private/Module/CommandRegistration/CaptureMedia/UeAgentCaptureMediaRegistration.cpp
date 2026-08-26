#include "Module/CommandRegistration/CaptureMedia/UeAgentCaptureMediaRegistration.h"

#include "CommandHandlers/CaptureMedia/UeAgentCaptureMediaCommandHandlers.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "OperationOwners/CaptureMedia/UeAgentCaptureMediaOwner.h"

namespace UE::UAI::Module::CommandRegistration::CaptureMedia
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace OperationOwners::CaptureMedia;
	namespace
	{
		const FString Owner = TEXT("capture-media-job-owner");
		bool TryBuildImagePolicyIdSchema(
			FString& OutPolicyIdSchema,
			FString& OutError)
		{
			using namespace DomainAdapters::CaptureMediaLongJob;
			OutPolicyIdSchema.Reset();
			const TSharedRef<FJsonObject> PolicySchema = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> PolicyIds;
			for (const FUeAgentCaptureMediaImagePolicyDescriptor& Policy
				: GetCaptureMediaImagePolicies())
			{
				if (!Policy.IsValid())
				{
					OutError = TEXT("capture_image_policy_descriptor_invalid:")
						+ Policy.GetPolicyId();
					return false;
				}
				PolicyIds.Add(MakeShared<FJsonValueString>(Policy.GetPolicyId()));
			}
			PolicySchema->SetArrayField(TEXT("enum"), MoveTemp(PolicyIds));
			PolicySchema->SetStringField(TEXT("type"), TEXT("string"));
			return Contract::Json::TryCanonicalize(
				PolicySchema, OutPolicyIdSchema, OutError);
		}

		bool TryConstStringSchema(
			const FString& Value,
			FString& OutSchema,
			FString& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetStringField(TEXT("const"), Value);
			return Contract::Json::TryCanonicalize(
				Schema, OutSchema, OutError);
		}

		bool TryAppendImageDiscoveryVariants(
			FUeAgentCommandDeclaration& Declaration,
			FString& OutError)
		{
			using namespace DomainAdapters::CaptureMediaLongJob;
			for (const FUeAgentCaptureMediaImagePolicyDescriptor& Policy :
				GetCaptureMediaImagePolicies())
			{
				if (!Policy.HasDiscoveryContract())
				{
					continue;
				}
				FString PolicySchema;
				FString RequestSchema;
				if (!TryConstStringSchema(
						Policy.GetPolicyId(), PolicySchema, OutError)
					|| !TryBuildCommandVariantRequestSchema(
						{
							{TEXT("subject_ref"), Policy.GetSubjectSchemaJson(), true},
							{TEXT("policy_id"), PolicySchema, true},
							{TEXT("parameters"), Policy.GetParameterSchemaJson(), true},
						},
						RequestSchema,
						OutError))
				{
					OutError = TEXT("capture_image_variant_schema_invalid:")
						+ Policy.GetPolicyId() + TEXT(":") + OutError;
					return false;
				}
				FUeAgentCommandVariantDeclaration Variant;
				Variant.VariantId = Policy.GetPolicyId();
				Variant.RequestSchemaJson = MoveTemp(RequestSchema);
				Variant.Purpose.Summary = Policy.GetParameterGuidance();
				Variant.Purpose.Target = TEXT("One exact ")
					+ Policy.GetPolicyId() + TEXT(" image capture request.");
				Variant.Purpose.Preconditions = {
					TEXT("The Editor has a render-capable RHI and the exact subject required by the selected policy is available."),
				};
				Variant.Purpose.ParameterGuidance = Policy.GetParameterGuidance();
				Variant.Purpose.ResultGuidance = Policy.GetResultGuidance();
				Variant.Purpose.FailureGuidance = TEXT(
					"Invalid selectors or parameters, unavailable RHI/viewport, capture failure or unproved restoration fail typed; do not fabricate an artifact or replay state_unknown.");
				Variant.Purpose.SideEffectGuidance = TEXT(
					"The owner may create one bounded artifact and temporary preview state only; declared restoration must complete before success.");
				Variant.Purpose.MinimumExample = Policy.GetMinimumExample();
				Variant.Purpose.RelatedFlows = {
					TEXT("operation.status"),
					TEXT("operation.receipt"),
					TEXT("operation.release"),
				};
				Variant.Purpose.Keywords = {
					Policy.GetPolicyId(),
					Policy.GetMechanicsId(),
				};
				Declaration.Variants.Add(MoveTemp(Variant));
			}
			return true;
		}

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
		bool TryDeclaration(const FUeAgentCaptureMediaRoute& Route,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out, FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Route.CommandId);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("capture_media_contract_missing:") + Route.CommandId;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("capture");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {TEXT("server_operation_registry"),
				TEXT("owned_capture_resources"), TEXT("terminal_operation_receipt")};
			FString PolicyIdSchema;
			if (Route.CommandId == TEXT("capture.image.execute")
				&& !TryBuildImagePolicyIdSchema(
					PolicyIdSchema, OutError))
			{
				return false;
			}
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				FUeAgentCommandFieldDeclaration Field{
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact capture operation parameter.")};
				if (Route.CommandId == TEXT("capture.image.execute")
					&& Parameter.GetName() == TEXT("policy_id"))
				{
					Field.Description = TEXT(
						"Select one exact image policy from this dynamic catalog.");
					Field.SchemaJson = PolicyIdSchema;
				}
				else if (Route.CommandId == TEXT("capture.image.execute")
					&& Parameter.GetName() == TEXT("parameters"))
				{
					Field.Description = TEXT(
						"Policy-specific parameters; select a search candidate variant and use its request_schema without guessing.");
				}
				else if (Route.CommandId == TEXT("capture.image.execute")
					&& Parameter.GetName() == TEXT("subject_ref"))
				{
					Field.Description = TEXT(
						"Exact capture subject. Transform previews use capture-subject:actor:<exact Actor object path>.");
				}
				Result.InputFields.Add(MoveTemp(Field));
			}
			Result.ResultFields = {{TEXT("result"), TEXT("object"), true, false,
				TEXT("Long-running capture snapshot and operation reference.")}};
			Result.Errors = {
				{TEXT("capture_media_authority_required"), TEXT("The caller lacks public operation authority.")},
				{TEXT("capture_media_long_job_input_invalid"), TEXT("The request is rejected before capture begins.")},
				{TEXT("capture_media_long_job_operation_failed"), TEXT("The owned capture operation failed.")},
			};
			Result.Purpose.Summary = TEXT("Start ") + Route.CommandId
				+ TEXT(" through the unique CaptureMedia owner and common OperationService.");
			Result.Purpose.Target = TEXT("One exact image, media, or render capture request.");
			Result.Purpose.Preconditions = {TEXT("Resolve the exact subject, policy and bounded output before execution.")};
			Result.Purpose.ParameterGuidance = TEXT("Forward only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Observe the returned operation_ref through T32 until terminal, then release.");
			Result.Purpose.FailureGuidance = TEXT("Cancel at the owner boundary; state_unknown forbids replay.");
			Result.Purpose.SideEffectGuidance = TEXT("Only owner-declared capture resources and output paths may be created.");
			Result.Purpose.MinimumExample = Route.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("operation.cancel"), TEXT("operation.release")};
			Result.Purpose.Keywords = {TEXT("capture"), TEXT("image"), TEXT("media"), TEXT("render")};
			Result.Purpose.Synonyms = {TEXT("capture job")};
			if (Route.CommandId == TEXT("capture.image.execute"))
			{
				if (!TryAppendImageDiscoveryVariants(Result, OutError))
				{
					return false;
				}
				Result.Errors.Add({
					TEXT("capture_transform_candidates_restore_unverified"),
					TEXT("The preview failed closed because transient editor state restoration could not be proved."),
				});
			}
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("capture-media-operation-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out, FString& OutError) const override
			{
				return CommandHandlers::CaptureMedia::TryCreateHandlers(Out, OutError);
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
					TEXT("capture-media-operation"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FUeAgentCaptureMediaRoute& Route : GetRoutes())
			{
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = Route.CommandId;
				Operation.Kind = DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::LongRunning;
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::RecoverableSession;
				Operation.bPersists = false;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory =
					FindOperationInvocationFactory(Route.CommandId);
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::RuntimeInstance);
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T44-CAPTURE-MEDIA-OPERATION");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("capture-media-operation"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(TSharedPtr<IUeAgentActiveCommandContribution>& Out,
		FString& OutError)
	{
		Out.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::LongRunningSession::
				TryCreateLongRunningSessionProductionCommandContractRegistry(Legacy, OutError)
			|| !Legacy.IsValid()) return false;
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FUeAgentCaptureMediaRoute& Route : GetRoutes())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		if (Declarations.Num() != 3) return false;
		Out = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
