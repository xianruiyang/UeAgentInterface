#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleOperationCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::CommandHandlers::AssetSimple
{
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		const FString Owner = TEXT("asset-simple-operation-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
            Diagnostic.TerminalStatus = TEXT("not_started");
            Diagnostic.bRetrySafe = true;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_authority_required"), TEXT("the exact AssetSimple operation requires uai:operate"), TEXT("command")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_game_thread_required"), TEXT("AssetSimple UE mechanics must start on the game thread"), TEXT("command")));
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
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("asset_simple_route_invalid_or_duplicate"), OutError, TEXT("handler_factory"));
				return false;
			}
			if (Seen.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_route_invalid_or_duplicate"), TEXT("The command route is registered more than once."), TEXT("handler_factory"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			Seen.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route));
		}
		if (OutHandlers.Num() != Routes.Num())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_route_count_mismatch"),
				FString::Printf(TEXT("Created %d handlers; expected %d public routes."), OutHandlers.Num(), Routes.Num()), TEXT("handler_factory"));
			OutHandlers.Reset();
			return false;
		}
		return true;
	}
}
