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
			TEXT("content-graph-workspace-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_authority_required"), TEXT("the route-specific UAI authority scope is required"), TEXT("command_submit")), Request.GetCommandId());
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_game_thread_required"), TEXT("the public ingress must dispatch UObject mechanics on the game thread"), TEXT("command_submit")), Request.GetCommandId());
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
					auto Diagnostic = Error;
					if (Error.Code.IsEmpty())
					{
						Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_execution_failed"), TEXT("The UObject data asset service failed without a diagnostic."), TEXT("command_submit"));
						Diagnostic.CandidateName = Request.GetCommandId();
					}
					return Failure(Request, Diagnostic, Error.SubjectId);
				}
				if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Query
					&& !Response.bReadOnly)
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_query_not_read_only"), TEXT("the command service violated its query contract"), TEXT("command_submit")), Request.GetCommandId());
				}
				TSharedPtr<FJsonObject> Payload;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CommandContract::Json::TryParseObject(
						Response.PayloadJson, Payload, JsonError))
				{
					return Failure(Request, JsonError, Request.GetCommandId());
				}
				FString CanonicalPayload;
				if (!CommandContract::Json::TryCanonicalize(
						Payload, CanonicalPayload, JsonError))
				{
					return Failure(Request, JsonError, Request.GetCommandId());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FUeAgentUObjectDataAssetCommandRoute& Route
			: GetUObjectDataAssetCommandRoutes())
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("uobject_dataasset_route_invalid"), OutError, TEXT("handler_factory"));
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, Service));
		}
		return true;
	}
}
