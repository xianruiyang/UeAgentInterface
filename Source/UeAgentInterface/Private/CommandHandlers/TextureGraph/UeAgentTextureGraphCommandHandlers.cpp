#include "CommandHandlers/TextureGraph/UeAgentTextureGraphCommandHandlers.h"

#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "QueryOwners/TextureGraph/UeAgentTextureGraphQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::TextureGraph
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("texture-graph-operation-owner");
		bool IsQuery(const FString& Id)
		{
			return Id == TEXT("texture_graph.artifact.capture")
				|| Id == TEXT("texture_graph.runtime.probe");
		}

		FUeAgentCommandResult Unauthorized(const FUeAgentCommandRequest& Request)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("texture_graph_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(
				TEXT("detail"),
				IsQuery(Request.GetCommandId())
					? TEXT("uai:query is required")
					: TEXT("uai:operate is required"));
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FString InId,
				TSharedRef<IUeAgentCommandHandler> InMechanics)
				: Id(MoveTemp(InId)), Mechanics(MoveTemp(InMechanics))
			{
			}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const FString RequiredScope = IsQuery(Id)
					? TEXT("uai:query") : TEXT("uai:operate");
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("audio_media_graph.read"));
				if (!IsQuery(Id))
				{
					Scopes.AddUnique(TEXT("audio_media_graph.write"));
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes),
					Caller.GetCorrelationId());
				return IsQuery(Id)
					? QueryOwners::TextureGraph::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::TextureGraph::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation);
			}
		private:
			FString Id;
			TSharedRef<IUeAgentCommandHandler> Mechanics;
		};

		class FDomainOperationHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FDomainOperationHandler(
				FString InId,
				TSharedRef<const DomainCapability::
					IDomainOperationInvocationFactory> InFactory)
				: Id(MoveTemp(InId))
				, Factory(MoveTemp(InFactory))
			{
			}

			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Unauthorized(Request);
				}
				DomainCapability::FDomainOperationInvocation Invocation;
				DomainCapability::FDomainOperationInvocationError InvocationError;
				if (!Factory->TryCreate(
						Request,
						Caller,
						Cancellation,
						Invocation,
						InvocationError))
				{
					const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
					Error->SetStringField(
						TEXT("code"), InvocationError.GetCode());
					Error->SetStringField(
						TEXT("subject_id"), InvocationError.GetSubjectId());
					Error->SetStringField(
						TEXT("detail"), InvocationError.GetDetail());
					FString ErrorJson;
					const auto Writer = TJsonWriterFactory<TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
					FJsonSerializer::Serialize(Error, Writer);
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), MoveTemp(ErrorJson));
				}
				BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
				if (!Invocation.GetService()->Run(
						*Invocation.GetPlan(),
						*Invocation.GetAuthorization(),
						Receipt))
				{
					FString FailureDetail =
						Invocation.GetContext()->GetFailureDetail();
					if (FailureDetail.IsEmpty())
					{
						FailureDetail = TEXT("phase_history:")
							+ FString::Join(
								Receipt.GetPhaseHistory(), TEXT(","));
					}
					const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
					Error->SetStringField(
						TEXT("code"), Receipt.GetErrorCode());
					Error->SetStringField(TEXT("subject_id"), Id);
					Error->SetStringField(TEXT("detail"), FailureDetail);
					FString ErrorJson;
					const auto Writer = TJsonWriterFactory<TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
					FJsonSerializer::Serialize(Error, Writer);
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), MoveTemp(ErrorJson));
				}
				FString Payload;
				FString PayloadError;
				if (!Invocation.GetContext()->TryBuildSuccessPayload(
						Receipt, Payload, PayloadError))
				{
					return FUeAgentCommandResult::ProtocolError(
						Request.GetCorrelationId(),
						Request.GetRequestId(),
						Request.GetCommandId(),
						Request.GetCommandBindingHash(),
						TEXT("domain_operation_result_invalid"),
						PayloadError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(Payload));
			}

		private:
			FString Id;
			TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory> Factory;
		};
	}

	bool TryCreateHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!OperationOwners::TextureGraph::TryCreateMechanicsHandlers(
				BakeFactory, Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id : DomainAdapters::TextureGraph::GetCommandIds())
		{
			if (const TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory> Factory =
					OperationOwners::TextureGraph::
						FindTextureGraphOperationInvocationFactory(Id))
			{
				OutHandlers.Add(MakeShared<FDomainOperationHandler>(
					Id,
					Factory.ToSharedRef()));
				continue;
			}
			const TSharedRef<IUeAgentCommandHandler>* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = TEXT("texture_graph_mechanics_handler_missing:") + Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::TextureGraph::GetCommandIds().Num();
	}
}
