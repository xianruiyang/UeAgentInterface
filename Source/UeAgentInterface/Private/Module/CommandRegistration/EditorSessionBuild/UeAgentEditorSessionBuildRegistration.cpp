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
			FString& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetStringField(TEXT("const"), Value);
			return Json::TryCanonicalize(Schema, OutSchema, OutError);
		}

		bool TryAppendDiscoveryVariants(
			FUeAgentCommandDeclaration& Declaration,
			FString& OutError)
		{
			using namespace DomainAdapters::EditorSessionOperation;
			for (const FUeAgentEditorSessionOperationPolicyDescriptor& Policy :
				Composition::LongRunningSession::
					GetLongRunningEditorSessionPolicyDescriptors())
			{
				if (!Policy.HasDiscoveryContract())
				{
					continue;
				}
				const FUeAgentEditorSessionOperationFamilyContract* Family =
					FindEditorSessionOperationFamilyContract(Policy.GetFamily());
				if (Family == nullptr)
				{
					OutError = TEXT("editor_session_variant_family_missing:")
						+ Policy.GetPolicyId();
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
					OutError = TEXT("editor_session_variant_schema_invalid:")
						+ Policy.GetPolicyId() + TEXT(":") + OutError;
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
					TEXT("Resolve the exact target subject before calling and preserve the candidate command/hash/variant identities."),
				};
				Variant.Purpose.ParameterGuidance = Policy.GetParameterGuidance();
				Variant.Purpose.ResultGuidance = Policy.GetResultGuidance();
				Variant.Purpose.FailureGuidance = TEXT(
					"Invalid subject, selector, parameters, save or load state fails typed; do not guess another policy or replay state_unknown.");
				Variant.Purpose.SideEffectGuidance = TEXT(
					"The operation may change declared Editor session state only; verify current World and dirty resources independently and release the operation.");
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
				Declaration.Variants.Add(MoveTemp(Variant));
			}
			return true;
		}

		bool TryDeclaration(
			const FUeAgentEditorSessionBuildRoute& Route,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Route.CommandId);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("editor_session_build_contract_missing:") + Route.CommandId;
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
			Result.Purpose.Summary = TEXT("Start ") + Route.CommandId
				+ TEXT(" through the unique EditorSessionBuild owner and common OperationService.");
			Result.Purpose.Target = TEXT("One exact Editor session, safe exit, or build operation.");
			Result.Purpose.Preconditions = {
				TEXT("Use query.command.detail for the exact family, subject, policy and parameter contract."),
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
				FString& OutError) const override
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
		FString& OutError)
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
		for (const FUeAgentEditorSessionBuildRoute& Route : GetRoutes())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
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
		Plan.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("query.command.detail")};
		Plan.Purpose.Keywords = {TEXT("batch"), TEXT("console"), TEXT("undo"), TEXT("redo")};
		Plan.Purpose.Synonyms = {TEXT("server plan")};
		Declarations.Add(MoveTemp(Plan));
		TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
		if (Declarations.Num() != 4
			|| !TryGetProductionOperationPort(Port, OutError)
			|| !Port.IsValid())
		{
			return false;
		}
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations), Port.ToSharedRef());
		return true;
	}
}
