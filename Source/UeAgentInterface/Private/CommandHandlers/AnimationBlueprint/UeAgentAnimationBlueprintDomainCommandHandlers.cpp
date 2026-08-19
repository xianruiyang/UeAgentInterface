#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

namespace UE::UAI::CommandHandlers::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::AnimationBlueprint;

	namespace
	{
		const FString CommandId = TEXT("animation_blueprint.authoring.apply");
		const FString Owner = TEXT("animation-blueprint-domain-owner");

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString JsonError;
			if (!Json::TryCanonicalize(Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT("{\"code\":\"animation_blueprint_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		class FAnimationBlueprintHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			explicit FAnimationBlueprintHandler(
				TSharedRef<IUeAgentAnimationBlueprintCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(Request,
						TEXT("animation_blueprint_authority_required"),
						Request.GetCommandId(),
						TEXT("the uai:operate authority is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(Request,
						TEXT("animation_blueprint_game_thread_required"),
						Request.GetCommandId(),
						TEXT("Animation Blueprint mechanics run on the UE game thread"));
				}

				FAnimationBlueprintCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FAnimationBlueprintCommandResponse Response;
				FAnimationBlueprintCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request,
						Error.Code.IsEmpty()
							? FString(TEXT("animation_blueprint_execution_failed"))
							: Error.Code,
						Error.SubjectId,
						Error.Detail);
				}
				if (Response.bReadOnly)
				{
					return Failure(Request,
						TEXT("animation_blueprint_mutation_reported_read_only"),
						Request.GetCommandId(),
						TEXT("the Animation Blueprint service violated its mutation contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(
					Response.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Failure(Request,
						TEXT("animation_blueprint_result_invalid"),
						Request.GetCommandId(), JsonError);
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(Request,
						TEXT("animation_blueprint_result_invalid"),
						Request.GetCommandId(), JsonError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(CanonicalPayload));
			}

		private:
			TSharedRef<IUeAgentAnimationBlueprintCommandService> Service;
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::AnimationBlueprint::IsReadOnlyContentGraphCommand(
			Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(
				TEXT("animation_blueprint_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateAnimationBlueprintCommandHandlers(
		TSharedRef<IUeAgentAnimationBlueprintCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(
			MakeShared<FAnimationBlueprintHandler>(MoveTemp(Service)));
		return true;
	}
}
