#include "Module/CommandRegistration/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistration.h"

#include "Dom/JsonValue.h"

#include "CommandHandlers/PieRuntimeDebug/UeAgentPieRuntimeDebugCommandHandler.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "OperationOwners/PieRuntimeDebug/UeAgentPieRuntimeDebugOwner.h"

namespace UE::UAI::Module::CommandRegistration::PieRuntimeDebug
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace OperationOwners::PieRuntimeDebug;

	namespace
	{
		const FString Owner(TEXT("pie-runtime-debug-command-owner"));

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
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<const FUeAgentCommandContractRegistry> Contracts;
			TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
			if (!Composition::LongRunningSession::
					TryCreateLongRunningSessionProductionCommandContractRegistry(
						Contracts, OutError)
				|| !Composition::LongRunningSession::
					TryCreateLongRunningSessionDescriptorRegistry(
						Descriptors, OutError)
				|| !Contracts.IsValid()
				|| !Descriptors.IsValid())
			{
				return false;
			}
			const FUeAgentPieRuntimeDebugRoute* RoutePtr = nullptr;
			if (!TryGetRoute(RoutePtr, OutError)) return false;
			const FUeAgentPieRuntimeDebugRoute& Route = *RoutePtr;
			const FUeAgentCommandContract* Contract =
				Contracts->FindExact(Route.CommandId);
			const Contract::FUeAgentOperationDescriptor* Descriptor =
				Descriptors->FindExact(Route.DescriptorIdentity);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_debug_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_contract_missing"), TEXT("No command contract is registered for the runtime scenario route."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			if (Contract->GetNamespace() != Route.PythonNamespace || Contract->GetMethod() != Route.PythonFunction)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_contract_missing"), TEXT("The command contract Python binding differs from the runtime scenario route."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				OutError.PropertyPath = TEXT("python_binding");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Route.PythonNamespace + TEXT(".") + Route.PythonFunction);
				OutError.ActualValue = MakeShared<FJsonValueString>(Contract->GetNamespace() + TEXT(".") + Contract->GetMethod());
				return false;
			}
			if (!Descriptor)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_contract_missing"), TEXT("The exact operation descriptor for the runtime scenario route is not registered."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}

			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = Route.PythonNamespace;
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			// The exact route is retained for RuntimeSessionHandle.  Session and
			// pause/workspace fences must not be model-facing command arguments.
			Result.Availability = EUeAgentCommandAvailability::Dormant;
			Result.SideEffect = EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				TEXT("server_operation_registry"),
				TEXT("pie_runtime_instance"),
				TEXT("terminal_operation_receipt"),
			};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact runtime scenario activation parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Long-running snapshot with operation_ref and generation."),
			}};
			Result.Errors = {
				{TEXT("pie_runtime_debug_authority_required"),
					TEXT("The caller lacks runtime operation authority.")},
				{TEXT("long_running_session_input_invalid"),
					TEXT("The exact scenario request is invalid or stale.")},
				{TEXT("long_running_session_operation_failed"),
					TEXT("The bounded runtime scenario failed.")},
			};
			Result.Purpose.Summary = TEXT(
				"Run one bounded PIE scenario from one verified pause barrier to the next.");
			Result.Purpose.Target = TEXT(
				"One exact runtime-session:pie-debug session and LocalPlayer.");
			Result.Purpose.Preconditions = {
				TEXT("Use the RuntimeSessionHandle attached to the latest editor.session operation result."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Use only the activation descriptor request schema and aliases from the latest Current HJSON. property reads and writes use the common typed value wrapper; actor transforms and existing scene cameras use the same session alias resolver. input_action accepts exact UInputAction asset refs with bool, Axis1D, Axis2D or Axis3D samples. capture selects pie_output_surface, the evaluated pie_player_camera, or an aliased/operation-owned transient scene_camera; only scene_camera accepts a transient pose or FOV.");
			Result.Purpose.ResultGuidance = TEXT(
				"Observe the typed operation until terminal; SDK custody refreshes the runtime-session handle.");
			Result.Purpose.FailureGuidance = TEXT(
				"Stale identity is rejected before resume; state_unknown forbids replay.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"The owner may resume the exact PIE session temporarily and always releases operation-scoped input before re-pausing. Existing scene and player cameras are read-only; extra viewpoints use a transient offscreen camera, while output-surface capture has no movement semantics.");
			Result.Purpose.MinimumExample = TEXT(
				"result.operation.runtime_session.scenario_execute(parameters=parameters)");
			Result.Purpose.RelatedFlows = {
				TEXT("editor.session"), TEXT("operation.status"),
				TEXT("operation.cancel"), TEXT("operation.receipt"),
				TEXT("operation.release"),
			};
			Result.Purpose.Keywords = {
				TEXT("PIE"), TEXT("runtime scenario"), TEXT("pause epoch"),
				TEXT("player camera"), TEXT("scene camera"), TEXT("output surface"),
			};

			FUeAgentCommandVariantDeclaration Variant;
			Variant.VariantId = TEXT("policy:runtime-scenario:bounded/v2");
			Variant.RequestSchemaJson = Descriptor->GetInputSchema().GetJsonValue();
			Variant.Purpose = Result.Purpose;
			Result.Variants.Add(MoveTemp(Variant));
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				FUeAgentCommandDeclaration InDeclaration,
				TSharedRef<Contract::IUeAgentOperationApplicationPort> InPort)
				: Declaration(MoveTemp(InDeclaration))
				, Port(MoveTemp(InPort))
			{
			}

			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("pie-runtime-debug-command"));
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TSharedPtr<IUeAgentExactCommandHandler> Handler;
				if (!CommandHandlers::PieRuntimeDebug::TryCreateHandler(
						Port, Handler, OutError)
					|| !Handler.IsValid())
				{
					return false;
				}
				OutHandlers.Add(Handler.ToSharedRef());
				return true;
			}

		private:
			FUeAgentCommandDeclaration Declaration;
			TSharedRef<Contract::IUeAgentOperationApplicationPort> Port;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("pie-runtime-debug-command"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		FUeAgentCommandDeclaration Declaration;
		TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
		if (!TryDeclaration(Declaration, OutError)
			|| !TryGetProductionOperationPort(Port, OutError)
			|| !Port.IsValid())
		{
			return false;
		}
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declaration), Port.ToSharedRef());
		return true;
	}
}
