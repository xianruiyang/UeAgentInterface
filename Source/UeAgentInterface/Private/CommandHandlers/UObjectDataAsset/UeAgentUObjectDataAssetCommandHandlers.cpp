#include "CommandHandlers/UObjectDataAsset/UeAgentUObjectDataAssetCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::UObjectDataAsset
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::UObjectDataAsset;

	namespace
	{
		const FString Owner =
			TEXT("uobject-dataasset-recursive-initialization-owner");

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
			if (!CommandContract::Json::TryCanonicalize(
					Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT(
					"{\"code\":\"uobject_dataasset_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		bool HasAuthority(
			const FUeAgentCallerContext& Caller,
			const EUeAgentUObjectDataAssetRouteKind Kind)
		{
			if (!Caller.IsValid())
			{
				return false;
			}
			const TCHAR* Required = Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("uai:operate")
				: Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("uai:export")
					: TEXT("uai:query");
			return Caller.GetAuthorityScopes().Contains(Required);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentUObjectDataAssetCommandRoute InRoute,
				TSharedRef<IUeAgentUObjectDataAssetCommandService> InService)
				: Route(MoveTemp(InRoute))
				, Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!HasAuthority(Caller, Route.Kind))
				{
					return Failure(
						Request,
						TEXT("uobject_dataasset_authority_required"),
						Request.GetCommandId(),
						TEXT("the route-specific UAI authority scope is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("uobject_dataasset_game_thread_required"),
						Request.GetCommandId(),
						TEXT("the public ingress must dispatch UObject mechanics on the game thread"));
				}

				FUeAgentUObjectDataAssetCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested =
					Cancellation->IsCancelled();

				FUeAgentUObjectDataAssetCommandResponse Response;
				FUeAgentUObjectDataAssetCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(
						Request,
						Error.Code.IsEmpty()
							? FString(TEXT("uobject_dataasset_execution_failed"))
							: Error.Code,
						Error.SubjectId,
						Error.Detail);
				}
				if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Query
					&& !Response.bReadOnly)
				{
					return Failure(
						Request,
						TEXT("uobject_dataasset_query_not_read_only"),
						Request.GetCommandId(),
						TEXT("the command service violated its query contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!CommandContract::Json::TryParseObject(
						Response.PayloadJson, Payload, JsonError))
				{
					return Failure(
						Request,
						TEXT("uobject_dataasset_result_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				FString CanonicalPayload;
				if (!CommandContract::Json::TryCanonicalize(
						Payload, CanonicalPayload, JsonError))
				{
					return Failure(
						Request,
						TEXT("uobject_dataasset_result_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(CanonicalPayload));
			}

		private:
			FUeAgentUObjectDataAssetCommandRoute Route;
			TSharedRef<IUeAgentUObjectDataAssetCommandService> Service;
		};
	}

	bool TryCreateUObjectDataAssetCommandHandlers(
		TSharedRef<IUeAgentUObjectDataAssetCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FUeAgentUObjectDataAssetCommandRoute& Route
			: GetUObjectDataAssetCommandRoutes())
		{
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("uobject_dataasset_route_invalid");
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, Service));
		}
		return true;
	}
}
