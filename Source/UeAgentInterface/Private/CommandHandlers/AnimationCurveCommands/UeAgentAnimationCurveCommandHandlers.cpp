#include "CommandHandlers/AnimationCurveCommands/UeAgentAnimationCurveCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandOperationOwner.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "QueryOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::AnimationCurveCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner(TEXT("animation-curve-command-owner"));

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
				const bool bQuery = QueryOwners::AnimationCurveCommands::IsQuery(CommandId);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query")) : FString(TEXT("uai:operate"));
				if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_game_thread_required"), TEXT("native animation mechanics must run on the game thread"), TEXT("command")));
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				UE::UAI::Contract::FDiagnostic Error;
				if (!OperationOwners::AnimationCurveCommands::TryResolveMechanics(
						CommandId, Mechanics, MechanicsScopes, Error))
				{
					return Failure(Request, Error);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes) Scopes.AddUnique(Scope);
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return OperationOwners::AnimationCurveCommands::Submit(
					Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation);
			}
		private:
			FString CommandId;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : QueryOwners::AnimationCurveCommands::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return OutHandlers.Num() == QueryOwners::AnimationCurveCommands::GetCommandIds().Num();
	}
}
