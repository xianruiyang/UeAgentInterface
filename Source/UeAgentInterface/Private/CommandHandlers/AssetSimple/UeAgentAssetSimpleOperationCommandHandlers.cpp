#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleOperationCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::AssetSimple
{
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		const FString Owner = TEXT("asset-simple-operation-owner");

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
			explicit FHandler(const FUeAgentSimpleAuthoringCommandRoute& Route)
				: CommandId(Route.CommandId)
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
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(
						Request,
						TEXT("asset_simple_authority_required"),
						TEXT("the exact AssetSimple operation requires uai:operate"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("asset_simple_game_thread_required"),
						TEXT("AssetSimple UE mechanics must start on the game thread"));
				}
				TArray<FString> EffectiveScopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : DomainAdapters::AssetSimple::
					GetAssetSimpleRequiredAuthorityScopes())
				{
					EffectiveScopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(),
					MoveTemp(EffectiveScopes),
					Caller.GetCorrelationId());
				return OperationOwners::AssetSimple::
					SubmitProductionAssetSimpleOperation(
						Request, EffectiveCaller, Cancellation);
			}

		private:
			FString CommandId;
		};
	}

	bool TryCreateAssetSimpleOperationCommandHandlers(
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TSet<FString> Seen;
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			if (!Route.IsValid() || Seen.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = TEXT("asset_simple_route_invalid_or_duplicate:")
					+ Route.CommandId;
				return false;
			}
			Seen.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route));
		}
		if (OutHandlers.Num() != 52)
		{
			OutHandlers.Reset();
			OutError = TEXT("asset_simple_route_count_mismatch");
			return false;
		}
		return true;
	}
}
