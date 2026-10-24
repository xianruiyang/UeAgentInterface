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

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            if (Diagnostic.TerminalStatus.IsEmpty()) { Diagnostic.TerminalStatus = TEXT("not_started"); Diagnostic.bRetrySafe = true; }
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("landscape_authority_required"), TEXT("the exact Landscape operation requires uai:operate"), TEXT("command_submit")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("landscape_game_thread_required"), TEXT("Landscape UE mechanics must start on the game thread"), TEXT("command_submit")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TSet<FString> Seen;
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("landscape_route_invalid_or_duplicate"), OutError, TEXT("handler_factory"));
				return false;
			}
			if (Seen.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("landscape_route_invalid_or_duplicate"), TEXT("The command route is registered more than once."), TEXT("handler_factory"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			if (!DomainAdapters::Landscape::IsCommand(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("landscape_route_invalid_or_duplicate"), TEXT("The command route is not owned by Landscape."), TEXT("handler_factory"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			Seen.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route.CommandId));
		}
		if (OutHandlers.Num() != DomainAdapters::Landscape::GetCommandIds().Num())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("landscape_route_count_mismatch"),
				FString::Printf(TEXT("Created %d handlers; expected %d public routes."), OutHandlers.Num(), DomainAdapters::Landscape::GetCommandIds().Num()), TEXT("handler_factory"));
			OutHandlers.Reset();
			return false;
		}
		return true;
	}
}
