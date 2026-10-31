#include "Module/CommandRegistration/CaptureMedia/UeAgentCaptureMediaRegistration.h"

#include "CommandHandlers/CaptureMedia/UeAgentCaptureMediaCommandHandlers.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetFolderThumbnailCapture.h"
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace DomainAdapters::CaptureMediaLongJob;
			OutPolicyIdSchema.Reset();
			const TSharedRef<FJsonObject> PolicySchema = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> PolicyIds;
			for (const FUeAgentCaptureMediaImagePolicyDescriptor& Policy
				: GetCaptureMediaImagePolicies())
			{
				if (!Policy.TryValidate(OutError))
				{
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_image_policy_descriptor_invalid"), OutError, TEXT("registration"));
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetStringField(TEXT("const"), Value);
			return Contract::Json::TryCanonicalize(
				Schema, OutSchema, OutError);
		}

		bool TryAppendImageDiscoveryVariants(
			FUeAgentCommandDeclaration& Declaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace DomainAdapters::CaptureMediaLongJob;
			for (const FUeAgentCaptureMediaImagePolicyDescriptor& Policy :
				GetCaptureMediaImagePolicies())
			{
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
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Policy.GetPolicyId();
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
					"Capture may open native editors or change viewport framing as described by the selected policy. Follow that policy's restoration contract and inspect the reported native changes.");
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
			FUeAgentCommandDeclaration& Out, UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Route.CommandId);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_media_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_media_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			const bool bFolderPreview = Route.CommandId == TEXT("asset.folder.preview");
			Result.PythonNamespace = bFolderPreview ? TEXT("asset") : TEXT("capture");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bFolderPreview ? EUeAgentCommandSideEffect::ExternalState : EUeAgentCommandSideEffect::EditorState;
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
					TEXT("Exact capture operation parameter."), Parameter.GetSchemaJson()};
				if (bFolderPreview)
				{
					Field.Description = Parameter.GetName() == TEXT("folder")
						? TEXT("UE content folder, for example /Game/RuinedCrypt.")
						: TEXT("Optional recursive (default true), class_paths (full class paths; derived classes included), page_size (default 24, split into sheets of at most 24), generate_missing (default true; load/render only this page's missing assets), and after (previous next_after with unchanged folder/filters).");
					if (Parameter.GetName() == TEXT("options"))
						Field.SchemaJson = DomainAdapters::CaptureMediaLongJob::GetAssetFolderPreviewOptionsSchema();
				}
				if (Route.CommandId == TEXT("capture.image.execute")
					&& Parameter.GetName() == TEXT("policy_id"))
				{
					Field.Description = TEXT(
						"Optional exact image policy. Omit for the active Level viewport or the asset's supported native preview; explicit policies take precedence. Null, empty and unknown values are errors, not defaults.");
					Field.SchemaJson = PolicyIdSchema;
				}
				else if (Route.CommandId == TEXT("capture.image.execute")
					&& Parameter.GetName() == TEXT("parameters"))
				{
					Field.Description = TEXT(
						"Policy-specific parameters. With omitted policy_id use viewport/v1 fields for viewport:active or asset-preview/v1 fields for an asset. Other observations require an explicit policy; select its variant request_schema.");
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
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance = TEXT("Observe the returned operation_ref through T32 until terminal, then release.");
			Result.Purpose.FailureGuidance = TEXT("Cancel at the owner boundary; state_unknown forbids replay.");
			Result.Purpose.SideEffectGuidance = TEXT("Only owner-declared capture resources and output paths may be created.");
			Result.Purpose.MinimumExample = Route.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("operation.cancel"), TEXT("operation.release")};
			Result.Purpose.Keywords = {TEXT("capture"), TEXT("image"), TEXT("media"), TEXT("render")};
			Result.Purpose.Synonyms = {TEXT("capture job")};
			if (bFolderPreview)
			{
				Result.Purpose.Summary = TEXT("Browse a content folder as named native-thumbnail contact sheets, without opening asset editors or loading every asset.");
				Result.Purpose.Target = TEXT("A UE content folder and an optional recursive, class-filtered page.");
				Result.Purpose.ParameterGuidance = TEXT("Pass folder; options defaults to recursive=true and page_size=24. Reuse asset_catalog.next_after as options.after with the same filters.");
				Result.Purpose.ResultGuidance = TEXT("Finish the returned operation and materialize its image artifacts. The capture receipt's asset_catalog maps each full name, object_path and class_path to sheet_index/tile_index; missing thumbnails remain explicit rows.");
				Result.Purpose.SideEffectGuidance = TEXT("Reads registry and native thumbnail caches, creates offscreen contact-sheet artifacts, and does not open asset editors or change the world.");
				Result.Purpose.MinimumExample = TEXT("client.asset.folder_preview(folder='/Game/RuinedCrypt', options={'class_paths': ['/Script/Engine.StaticMesh']})");
				Result.Purpose.Keywords = {TEXT("asset"), TEXT("folder"), TEXT("thumbnail"), TEXT("preview"), TEXT("contact sheet"), TEXT("batch"), TEXT("browse"), TEXT("资产"), TEXT("文件夹"), TEXT("缩略图")};
				Result.Purpose.Synonyms = {TEXT("folder asset previews"), TEXT("batch asset thumbnails"), TEXT("learn imported asset pack")};
			}
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
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out, UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::CaptureMedia::TryCreateHandlers(Out, OutError);
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
					TEXT("capture-media-operation"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			const TArray<FUeAgentCaptureMediaRoute>* Routes = nullptr;
			if (!TryGetRoutes(Routes, OutError)) return false;
			for (const FUeAgentCaptureMediaRoute& Route : *Routes)
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
				if (!TryFindOperationInvocationFactory(Route.CommandId, Operation.InvocationFactory, OutError)) return false;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::RuntimeInstance);
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("capture-media-operation"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(TSharedPtr<IUeAgentActiveCommandContribution>& Out,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		Out.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::LongRunningSession::
				TryCreateLongRunningSessionProductionCommandContractRegistry(Legacy, OutError)
			|| !Legacy.IsValid()) return false;
		TArray<FUeAgentCommandDeclaration> Declarations;
		const TArray<FUeAgentCaptureMediaRoute>* Routes = nullptr;
		if (!TryGetRoutes(Routes, OutError)) return false;
		for (const FUeAgentCaptureMediaRoute& Route : *Routes)
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		Out = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
