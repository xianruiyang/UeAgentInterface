#include "CommandHandlers/TextureGraph/UeAgentTextureGraphCommandHandlers.h"

#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"
#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"
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
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("texture_graph_authority_required"),
				IsQuery(Request.GetCommandId()) ? TEXT("uai:query is required") : TEXT("uai:operate is required"),
				TEXT("authorization"), FString(), Request.GetCommandId());
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic);
		}

		TOptional<FUeAgentCommandResult> RequireOfficialPlugin(
			const FUeAgentCommandRequest& Request)
		{
			const DomainCapability::OfficialPluginAvailability::
				FOfficialPluginAvailabilityObservation Availability =
					DomainCapability::OfficialPluginAvailability::Inspect(
						TEXT("TextureGraph"));
			if (Availability.IsAvailable())
			{
				return {};
			}
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("texture_graph_official_plugin_unavailable"), Availability.GetReason(),
				TEXT("availability"), FString(), TEXT("TextureGraph"));
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic);
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
				if (TOptional<FUeAgentCommandResult> Unavailable =
						RequireOfficialPlugin(Request))
				{
					return MoveTemp(Unavailable.GetValue());
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
				if (TOptional<FUeAgentCommandResult> Unavailable =
						RequireOfficialPlugin(Request))
				{
					return MoveTemp(Unavailable.GetValue());
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
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), InvocationError);
				}
				BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
				if (!Invocation.GetService()->Run(
						*Invocation.GetPlan(),
						*Invocation.GetAuthorization(),
						Receipt))
				{
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Receipt.GetDiagnostic());
				}
				FString Payload;
				UE::UAI::Contract::FDiagnostic PayloadError;
				if (!Invocation.GetContext()->TryBuildSuccessPayload(
						Receipt, Payload, PayloadError))
				{
					PayloadError.TerminalStatus = TEXT("succeeded");
					PayloadError.MutationStarted = Receipt.DidMutationStart();
					PayloadError.MutationMayHaveStarted = Receipt.DidMutationStart();
					PayloadError.bRetrySafe = false;
					return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), PayloadError);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_mechanics_handler_missing"), TEXT("No mechanics handler is registered for this public command."), TEXT("texture_graph"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::TextureGraph::GetCommandIds().Num();
	}
}
