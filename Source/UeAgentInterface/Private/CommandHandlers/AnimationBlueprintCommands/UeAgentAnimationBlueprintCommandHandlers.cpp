#include "CommandHandlers/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandHandlers.h"

#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"
#include "OperationOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandOperationOwner.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "QueryOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandQueryOwner.h"

namespace UE::UAI::CommandHandlers::AnimationBlueprintCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("animation-blueprint-command-owner");

		FUeAgentCommandResult Unauthorized(
            const FUeAgentCommandRequest& Request,
            const FString& RequiredScope)
        {
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_authority_required"),
                    RequiredScope + TEXT(" is required."), TEXT("authorization"),
                    FString(), Request.GetCommandId()));
        }

		

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(FString InId, TSharedRef<IUeAgentCommandHandler> InMechanics)
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
				const bool bQuery =
					DomainAdapters::AnimationBlueprintCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("animation_rig_graph.read"));
				if (!bQuery)
				{
					Scopes.AddUnique(TEXT("animation_rig_graph.write"));
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes),
					Caller.GetCorrelationId());
				FUeAgentCommandResult Result = bQuery
					? QueryOwners::AnimationBlueprintCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::AnimationBlueprintCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation);
				return Result;
			}
		private:
			FString Id;
			TSharedRef<IUeAgentCommandHandler> Mechanics;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!OperationOwners::AnimationBlueprintCommands::TryCreateMechanicsHandlers(
				Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id
			: DomainAdapters::AnimationBlueprintCommands::GetPublicCommandIds())
		{
			const TSharedRef<IUeAgentCommandHandler>* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_mechanics_handler_missing"), TEXT("No mechanics handler is registered for this public command."), TEXT("handler_factory"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::AnimationBlueprintCommands::GetPublicCommandIds().Num();
	}
}

