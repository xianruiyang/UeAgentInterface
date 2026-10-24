#include "Module/CommandRegistration/EditorSessionBuild/UeAgentEditorSessionBuildRegistration.h"

#include "CommandHandlers/EditorSessionBuild/UeAgentEditorSessionBuildCommandHandlers.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/EditorSessionBuild/UeAgentEditorSessionBuildOwner.h"

namespace UE::UAI::Module::CommandRegistration::EditorSessionBuild
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace OperationOwners::EditorSessionBuild;

	namespace
	{
		const FString Owner = TEXT("editor-session-build-command-owner");

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

		bool TryConstStringSchema(
			const FString& Value,
			FString& OutSchema,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetStringField(TEXT("const"), Value);
			return Json::TryCanonicalize(Schema, OutSchema, OutError);
		}

		bool TryAppendDiscoveryVariants(
			FUeAgentCommandDeclaration& Declaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace DomainAdapters::EditorSessionOperation;
			for (const FUeAgentEditorSessionOperationPolicyDescriptor& Policy :
				Composition::LongRunningSession::
					GetLongRunningEditorSessionPolicyDescriptors())
			{
				// Host lifecycle is exposed by the fixed editor.safe_exit command.
				if (Policy.GetFamily() == EUeAgentEditorSessionOperationFamily::HostLifecycle)
				{
					continue;
				}
				const FUeAgentEditorSessionOperationFamilyContract* Family =
					FindEditorSessionOperationFamilyContract(Policy.GetFamily());
				if (Family == nullptr)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_session_variant_family_missing"), TEXT("The editor session policy refers to a family with no registered discovery contract."), TEXT("registration"), FString(), Policy.GetPolicyId());
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Policy.GetPolicyId();
					return false;
				}
				FString FamilySchema;
				FString PolicySchema;
				FString RequestSchema;
				if (!TryConstStringSchema(
						Family->GetFamilyId(), FamilySchema, OutError)
					|| !TryConstStringSchema(
						Policy.GetPolicyId(), PolicySchema, OutError)
					|| !TryBuildCommandVariantRequestSchema(
						{
							{TEXT("family_id"), FamilySchema, true},
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
					+ Policy.GetPolicyId() + TEXT(" call in family ")
					+ Family->GetFamilyId() + TEXT(".");
				Variant.Purpose.Preconditions = {
					TEXT("Use the exact subject and policy returned by public discovery through the named SDK or CLI."),
				};
				Variant.Purpose.ParameterGuidance = Policy.GetParameterGuidance();
				Variant.Purpose.ResultGuidance = Policy.GetResultGuidance();
				Variant.Purpose.FailureGuidance = TEXT(
					"Invalid subject, selector, parameters, save or load state fails typed; do not guess another policy or replay state_unknown.");
				Variant.Purpose.SideEffectGuidance = TEXT(
					"Follow the selected policy for saved resources, session lifetime and cleanup. Inspect the operation receipt and affected Editor state before release.");
				Variant.Purpose.MinimumExample = Policy.GetMinimumExample();
				Variant.Purpose.RelatedFlows = {
					TEXT("operation.status"),
					TEXT("operation.receipt"),
					TEXT("operation.release"),
				};
				Variant.Purpose.Keywords = {
					Policy.GetPolicyId(),
					Family->GetFamilyId(),
				};
				if (Policy.GetPolicyId()
					== TEXT("policy:world-context:set-current/v1"))
				{
					Variant.Purpose.Summary = TEXT(
						"Open or switch the Editor to one existing target map; "
						"this variant does not create a map asset.");
					Variant.Purpose.Target = TEXT(
						"One existing target map that resolves before operation registration.");
					Variant.Purpose.Preconditions = {
						TEXT("Resolve an existing world:/Game/... map target before calling; set-current does not create missing assets."),
						TEXT("Use the editor.session named SDK or CLI with this exact policy; connection and contract identities are managed by the client."),
					};
					Variant.Purpose.ParameterGuidance = TEXT(
						"Pass the existing target as subject_ref and optionally save_existing_map; no create or factory parameters are accepted.");
					Variant.Purpose.ResultGuidance = TEXT(
						"accepted means operation registration only. Preserve the original operation_ref and call operation.resume with the latest generation until typed latest_signal reports phase entered/ready; only then is the session usable. After the task, call operation.cancel, operation.resume to a safe boundary until terminal, read operation.receipt, then operation.release.");
					Variant.Purpose.FailureGuidance = TEXT(
						"An invalid, missing or unresolvable target map fails typed before operation registration; do not retry it as asset creation or keep a running operation.");
					Variant.Purpose.SideEffectGuidance = TEXT(
						"This variant may save the current map and switch the Editor to the resolved existing map only; it does not create an asset.");
					Variant.Purpose.RelatedFlows = {
						TEXT("operation.status"),
						TEXT("operation.resume"),
						TEXT("operation.cancel"),
						TEXT("operation.receipt"),
						TEXT("operation.release"),
					};
					Variant.Purpose.Keywords = {
						TEXT("open"),
						TEXT("existing"),
						TEXT("level"),
						TEXT("map"),
						TEXT("switch"),
						Policy.GetPolicyId(),
						Family->GetFamilyId(),
					};
					Variant.Purpose.Synonyms = {
						TEXT("set current world"),
						TEXT("open level"),
					};
				}
				Declaration.Variants.Add(MoveTemp(Variant));
			}
			return true;
		}

		bool TryDeclaration(
			const FUeAgentEditorSessionBuildRoute& Route,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Route.CommandId);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("editor_session_build_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_session_build_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = Contract->GetNamespace();
			Result.PythonFunction = Contract->GetMethod();
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				TEXT("server_operation_registry"),
				TEXT("owned_operation_resources"),
				TEXT("terminal_operation_receipt"),
			};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact Editor session/build operation parameter."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Long-running snapshot with operation_ref, generation and terminal state."),
			}};
			Result.Errors = {
				{TEXT("editor_session_build_authority_required"),
					TEXT("The caller lacks public operation authority.")},
				{TEXT("long_running_session_input_invalid"),
					TEXT("The exact request is rejected before native work begins.")},
				{TEXT("long_running_session_operation_failed"),
					TEXT("The owned session or build operation failed.")},
			};
			if (Route.CommandId == TEXT("editor.session"))
			{
				Result.Errors.Add({TEXT("world_context_subject_invalid"),
					TEXT("The set-current world subject is not a valid map package identity.")});
				Result.Errors.Add({TEXT("world_context_map_not_found"),
					TEXT("The set-current target map does not exist.")});
				Result.Errors.Add({TEXT("world_context_map_unresolvable"),
					TEXT("The set-current package exists but does not resolve to a map target.")});
			}
			Result.Purpose.Summary = TEXT("Start ") + Route.CommandId
				+ TEXT(" through the unique EditorSessionBuild owner and common OperationService.");
			Result.Purpose.Target = TEXT("One exact Editor session, safe exit, or build operation.");
			Result.Purpose.Preconditions = {
				TEXT("Use the generated naming SDK method and its public parameters for the exact family, subject, policy and parameter contract."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Forward only the exact public fields.");
			Result.Purpose.ResultGuidance = TEXT("Use operation_ref with T32 lifecycle commands until terminal, then release.");
			Result.Purpose.FailureGuidance = TEXT("Transport loss requires status/reconcile; state_unknown forbids replay.");
			Result.Purpose.SideEffectGuidance = TEXT("The owner controls Editor and worker resources and restores or closes only as declared.");
			Result.Purpose.MinimumExample = Route.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("operation.receipt"), TEXT("operation.release")};
			Result.Purpose.Keywords = {TEXT("editor"), TEXT("session"), TEXT("build"), TEXT("safe exit")};
			Result.Purpose.Synonyms = {TEXT("control operation")};
			if (Route.CommandId == TEXT("editor.session")
				&& !TryAppendDiscoveryVariants(Result, OutError))
			{
				return false;
			}
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TArray<FUeAgentCommandDeclaration> InDeclarations,
				TSharedRef<Contract::IUeAgentOperationApplicationPort> InPort)
				: Declarations(MoveTemp(InDeclarations)), Port(MoveTemp(InPort)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("editor-session-build-command-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::EditorSessionBuild::TryCreateHandlers(
					Port, Registry, Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TSharedRef<Contract::IUeAgentOperationApplicationPort> Port;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("editor-session-build-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::LongRunningSession::
				TryCreateLongRunningSessionProductionCommandContractRegistry(
					Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		TArray<FUeAgentEditorSessionBuildRoute> Routes;
		if (!TryGetRoutes(Routes, OutError)) return false;
		for (const FUeAgentEditorSessionBuildRoute& Route : Routes)
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		FUeAgentCommandDeclaration Save;
		Save.CommandId = TEXT("editor.save_resources");
		Save.PythonNamespace = TEXT("editor");
		Save.PythonFunction = TEXT("save_resources");
		Save.ContractVersion = 1;
		Save.PublicOwner = Owner;
		Save.Availability = EUeAgentCommandAvailability::Enabled;
		Save.SideEffect = EUeAgentCommandSideEffect::EditorState;
		Save.StateLayers = {TEXT("authoring_memory"), TEXT("persisted")};
		Save.InputFields = {{TEXT("resource_refs"), TEXT("array"), true, false,
			TEXT("Only the exact package/object paths or config refs to save; [] saves nothing."),
			TEXT(R"json({"items":{"minLength":1,"type":"string"},"type":"array"})json")}};
		Save.ResultFields = {
			{TEXT("resources"), TEXT("array"), true, false, TEXT("Per-resource saved, unchanged or failed status with failure reason.")},
			{TEXT("remaining_dirty_resources"), TEXT("array"), true, false, TEXT("Dirty resources left after this request.")},
			{TEXT("editor_open"), TEXT("boolean"), true, false, TEXT("The Editor remains open.")},
		};
		Save.Errors = {
			{TEXT("editor_session_build_authority_required"), TEXT("uai:operate is required.")},
			{TEXT("editor_save_resources_input_invalid"), TEXT("resource_refs must be an array of nonempty strings.")},
			{TEXT("editor_save_resources_failed"), TEXT("One or more requested resources failed; inspect every item.")},
		};
		Save.Purpose.Summary = TEXT("Save specified Editor resources synchronously and keep the Editor open.");
		Save.Purpose.Target = TEXT("Any persistent asset package or map in a mounted content root, plus supported config resources.");
		Save.Purpose.Preconditions = {TEXT("The Editor is open. Asset refs identify existing packages on disk or newly created in memory; unloaded packages are resolved automatically.")};
		Save.Purpose.ParameterGuidance = TEXT("Use package/object paths or refs from editor.dirty_resources. Asset types require no separate save routes. Empty lists are a no-op.");
		Save.Purpose.ResultGuidance = TEXT("Inspect saved/unchanged/failed for each resource and remaining_dirty_resources; independent resources are all attempted.");
		Save.Purpose.FailureGuidance = TEXT("Failed items report the reason; successful items are not rolled back.");
		Save.Purpose.SideEffectGuidance = TEXT("Only requested resources, native-owned external packages and map build data are saved. Empty packages use native file cleanup. Unrelated dirty assets and configs remain untouched. Config persistence is confirmed against native serialized output. No operation handle is created.");
		Save.Purpose.MinimumExample = TEXT("client.editor.save_resources(resource_refs=['/Game/MyMap'])");
		Save.Purpose.RelatedFlows = {TEXT("editor.dirty_resources")};
		Save.Purpose.Keywords = {TEXT("save"), TEXT("dirty"), TEXT("map"), TEXT("package"), TEXT("config")};
		Declarations.Add(MoveTemp(Save));
		FUeAgentCommandDeclaration Plan;
		Plan.CommandId = TEXT("operation.plan.execute");
		Plan.PythonNamespace = TEXT("operation");
		Plan.PythonFunction = TEXT("plan_execute");
		Plan.ContractVersion = 1;
		Plan.PublicOwner = Owner;
		Plan.Availability = EUeAgentCommandAvailability::Enabled;
		Plan.SideEffect = EUeAgentCommandSideEffect::EditorState;
		Plan.StateLayers = {TEXT("authoring_memory"), TEXT("runtime_instance")};
		Plan.InputFields = {
			{TEXT("steps"), TEXT("array"), true, false, TEXT("Bounded exact server-side plan steps.")},
			{TEXT("on_error"), TEXT("string"), true, false, TEXT("stop or continue.")},
			{TEXT("atomicity"), TEXT("string"), true, false, TEXT("reject_non_atomic or best_effort.")},
		};
		Plan.ResultFields = {{TEXT("result"), TEXT("object"), true, false,
			TEXT("Per-step server receipt and actual observation.")}};
		Plan.Errors = {
			{TEXT("operation_plan_authority_required"), TEXT("uai:operate is required.")},
			{TEXT("operation_plan_input_invalid"), TEXT("The bounded plan is invalid.")},
			{TEXT("operation_plan_step_failed"), TEXT("A planned provider step failed.")},
		};
		Plan.Purpose.Summary = TEXT("Execute one bounded server-planned control operation.");
		Plan.Purpose.Target = TEXT("Exact public-command, editor-console, or transaction-history steps.");
		Plan.Purpose.Preconditions = {TEXT("Resolve exact contracts and expected revisions before calling.")};
		Plan.Purpose.ParameterGuidance = TEXT("Use typed steps; historical command names are invalid.");
		Plan.Purpose.ResultGuidance = TEXT("Inspect every step receipt and observation.");
		Plan.Purpose.FailureGuidance = TEXT("Partial completion is explicit; never replay state_unknown.");
		Plan.Purpose.SideEffectGuidance = TEXT("The server freezes ordering, authority and provider routing.");
		Plan.Purpose.MinimumExample = TEXT("client.operation.plan_execute(steps, 'stop', 'reject_non_atomic')");
		Plan.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("query.command.usage")};
		Plan.Purpose.Keywords = {TEXT("batch"), TEXT("console"), TEXT("undo"), TEXT("redo")};
		Plan.Purpose.Synonyms = {TEXT("server plan")};
		Declarations.Add(MoveTemp(Plan));
		TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
		if (!TryGetProductionOperationPort(Port, OutError)
			|| !Port.IsValid())
		{
			return false;
		}
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations), Port.ToSharedRef());
		return true;
	}
}
