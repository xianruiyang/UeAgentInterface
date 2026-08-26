#include "CommandHandlers/AnimationRigCommands/UeAgentAnimationRigCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/AnimationRigCommands/UeAgentAnimationRigCommandOperationOwner.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::AnimationRigCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner(TEXT("animation-rig-command-owner"));

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InCommandId) : CommandId(MoveTemp(InCommandId)) {}
			const FString& GetCommandId() const override { return CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				const bool bQuery = QueryOwners::AnimationRigCommands::IsQuery(CommandId);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query")) : FString(TEXT("uai:operate"));
				if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(Request, TEXT("animation_rig_authority_required"),
						RequiredScope + TEXT(" is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, TEXT("animation_rig_game_thread_required"),
						TEXT("native animation rig mechanics must run on the game thread"));
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				FString Error;
				if (!OperationOwners::AnimationRigCommands::TryResolveMechanics(
						CommandId, Mechanics, MechanicsScopes, Error)
					|| !Mechanics.IsValid())
				{
					return Failure(Request, TEXT("animation_rig_runtime_unavailable"), Error);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes) Scopes.AddUnique(Scope);
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return OperationOwners::AnimationRigCommands::Submit(
					Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation);
			}
		private:
			FString CommandId;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : QueryOwners::AnimationRigCommands::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return OutHandlers.Num() == QueryOwners::AnimationRigCommands::GetCommandIds().Num();
	}
}
