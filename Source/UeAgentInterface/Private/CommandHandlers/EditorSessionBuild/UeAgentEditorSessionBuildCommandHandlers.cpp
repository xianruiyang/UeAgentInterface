#include "CommandHandlers/EditorSessionBuild/UeAgentEditorSessionBuildCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "DomainAdapters/OperationPlan/UeAgentOperationPlan.h"
#include "Dom/JsonObject.h"
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

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			FString CanonicalError;
			return Json::TryCanonicalize(Error, Text, CanonicalError)
				? Text : FString(TEXT("{}"));
		}

		bool TryReadOperationError(
			const FString& PayloadJson,
			FString& OutErrorJson)
		{
			OutErrorJson.Reset();
			TSharedPtr<FJsonObject> Root;
			FString ParseError;
			if (!Json::TryParseObject(PayloadJson, Root, ParseError)
				|| !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(TEXT("operation_error"), Error)
				&& Error && Error->IsValid())
			{
				return Json::TryCanonicalize(
					Error->ToSharedRef(), OutErrorJson, ParseError);
			}
			return true;
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
						Request, TEXT("{}"), ErrorJson(
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
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity() == Route.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(
							TEXT("editor_session_build_result_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned an invalid or mismatched result.")));
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(PayloadJson, OperationError))
				{
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(
							TEXT("editor_session_build_payload_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned a non-object payload.")));
				}
				return OperationError.IsEmpty()
					? FUeAgentCommandResult::Success(Request, PayloadJson)
					: FUeAgentCommandResult::Failure(Request, PayloadJson, OperationError);
			}

		private:
			FUeAgentEditorSessionBuildRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
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
				FString& OutError) override
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
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		const auto& Routes = GetRoutes();
		if (Routes.Num() != 3 || !Registry->FindExact(TEXT("operation.plan.execute")))
		{
			OutError = TEXT("editor_session_build_route_count_invalid");
			return false;
		}
		for (const FUeAgentEditorSessionBuildRoute& Route : Routes)
		{
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("editor_session_build_route_invalid:") + Route.CommandId;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, OperationPort));
		}
		OutHandlers.Add(MakeShared<FPlanHandler>(Registry));
		return true;
	}
}
