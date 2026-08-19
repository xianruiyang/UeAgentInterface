#include "CommandHandlers/Landscape/UeAgentLandscapeCommandHandlers.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/Landscape/UeAgentLandscapeOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::Landscape
{
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		const FString Owner(TEXT("landscape-operation-owner"));

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Object->SetBoolField(TEXT("retry_safe"), true);
			FString ErrorJson;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
			FJsonSerializer::Serialize(Object, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), ErrorJson);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }
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
					return Failure(
						Request,
						TEXT("landscape_authority_required"),
						TEXT("the exact Landscape operation requires uai:operate"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("landscape_game_thread_required"),
						TEXT("Landscape UE mechanics must start on the game thread"));
				}
				TArray<FString> EffectiveScopes = Caller.GetAuthorityScopes();
				for (const FString& Scope
					: DomainAdapters::Landscape::GetRequiredAuthorityScopes())
				{
					EffectiveScopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(),
					MoveTemp(EffectiveScopes),
					Caller.GetCorrelationId());
				return OperationOwners::Landscape::Submit(
					Request, EffectiveCaller, Cancellation);
			}

		private:
			FString CommandId;
		};
	}

	bool TryCreateHandlers(
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TSet<FString> Seen;
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			if (!Route.IsValid()
				|| !DomainAdapters::Landscape::IsCommand(Route.CommandId)
				|| Seen.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = TEXT("landscape_route_invalid_or_duplicate:")
					+ Route.CommandId;
				return false;
			}
			Seen.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route.CommandId));
		}
		if (OutHandlers.Num() != DomainAdapters::Landscape::GetCommandIds().Num())
		{
			OutHandlers.Reset();
			OutError = TEXT("landscape_route_count_mismatch");
			return false;
		}
		return true;
	}
}
