#include "CommandHandlers/EditorSessionBuild/UeAgentEditorSessionBuildCommandHandlers.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"
#include "CommandEdge/UeAgentApplicationResultProjection.h"
#include "DomainAdapters/OperationPlan/UeAgentOperationPlan.h"

#include "OperationOwners/EditorSessionBuild/UeAgentEditorSessionBuildOwner.h"

namespace UE::UAI::CommandHandlers::EditorSessionBuild
{
	using namespace CommandEdge;
	using namespace Contract;
	using DomainAdapters::OperationPlan::FUeAgentOperationPlan;
	using namespace OperationOwners::EditorSessionBuild;

	namespace
	{
		const FString Owner = TEXT("editor-session-build-command-owner");

		FDiagnostic CommandFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
		}

		

		FUeAgentCallerContext EffectiveCaller(
			const FUeAgentCallerContext& Caller,
			const FString& CommandId)
		{
			TArray<FString> Scopes = Caller.GetAuthorityScopes();
			Scopes.AddUnique(TEXT("uai:operation"));
			Scopes.AddUnique(CommandId == TEXT("job.compile_build")
				? FString(TEXT("uai:job"))
				: FString(TEXT("uai:editor:session")));
			return FUeAgentCallerContext(
				Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentEditorSessionBuildRoute InRoute,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Route(MoveTemp(InRoute))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override { return Route.CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), CommandFailure(
							TEXT("editor_session_build_authority_required"),
							Request.GetCommandId(),
							TEXT("the public control route requires uai:operate")));
				}
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					EffectiveCaller(Caller, Route.CommandId),
					Cancellation);
				FApplicationResultProjectionPolicy Policy;
				Policy.InvalidResultCode = TEXT("editor_session_build_result_invalid");
				Policy.InvalidPayloadCode = TEXT("editor_session_build_payload_invalid");
				Policy.BoundaryFailureKind = EApplicationFailureKind::None;
				return ProjectOperationResult(Request, Route.DescriptorIdentity, Result, Policy);
			}

		private:
			FUeAgentEditorSessionBuildRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};

		class FSaveResourcesHandler final : public IUeAgentExactCommandHandler
		{
		public:
			const FString& GetCommandId() const override
			{
				static const FString Id(TEXT("editor.save_resources"));
				return Id;
			}
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), CommandFailure(
						TEXT("editor_session_build_authority_required"), GetCommandId(),
						TEXT("the public control route requires uai:operate")));
				}
				FString Payload;
				Composition::LongRunningSession::FUeAgentLongRunningSessionProductionError Error;
				return Composition::LongRunningSession::TrySaveEditorResources(
					Request.GetParamsJson(), Payload, Error)
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(Request, Payload.IsEmpty() ? TEXT("{}") : Payload,
						Error);
			}
		};

		class FPlanHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FPlanHandler(
				TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry>
					Registry)
				: Plan(MakeShared<FUeAgentOperationPlan>(MoveTemp(Registry)))
			{
			}

			const FString& GetCommandId() const override
			{
				static const FString Id(TEXT("operation.plan.execute"));
				return Id;
			}
			const FString& GetPublicOwner() const override { return Owner; }

			bool BindNestedExactDispatch(
				TWeakPtr<const FUeAgentExactCommandDispatch> Dispatch,
				TSharedRef<
					const CommandRegistry::FUeAgentImmutableCommandRegistry>
					Registry,
				UE::UAI::Contract::FDiagnostic& OutError) override
			{
				return Plan->BindNestedExactDispatch(
					MoveTemp(Dispatch), MoveTemp(Registry), OutError);
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				return Plan->Execute(Request, Caller, Cancellation);
			}

		private:
			TSharedRef<FUeAgentOperationPlan> Plan;
		};
	}

	bool TryCreateHandlers(
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentEditorSessionBuildRoute> Routes;
		if (!TryGetRoutes(Routes, OutError)) return false;
		if (!Registry->FindExact(TEXT("operation.plan.execute")))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_session_build_plan_execute_missing"), TEXT("The operation plan execution command is not registered."), TEXT("handler_factory"), FString(), TEXT("operation.plan.execute"));
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TEXT("operation.plan.execute");
			return false;
		}
		for (const FUeAgentEditorSessionBuildRoute& Route : Routes)
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("editor_session_build_route_invalid"), OutError, TEXT("handler_factory"));
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, OperationPort));
		}
		OutHandlers.Add(MakeShared<FSaveResourcesHandler>());
		OutHandlers.Add(MakeShared<FPlanHandler>(Registry));
		return true;
	}
}
