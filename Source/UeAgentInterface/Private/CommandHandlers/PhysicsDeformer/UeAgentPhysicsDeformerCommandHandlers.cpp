#include "CommandHandlers/PhysicsDeformer/UeAgentPhysicsDeformerCommandHandlers.h"

#include "DomainAdapters/PhysicsDeformer/UeAgentPhysicsDeformerMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/PhysicsDeformer/UeAgentPhysicsDeformerOperationOwner.h"
#include "QueryOwners/PhysicsDeformer/UeAgentPhysicsDeformerQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::PhysicsDeformer
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner(TEXT("physics-deformer-operation-owner"));

		FUeAgentCommandResult Unauthorized(
			const FUeAgentCommandRequest& Request,
			const FString& RequiredScope)
		{
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("physics_deformer_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_authority"), FString(), Request.GetCommandId()));
        }

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InId)
				: Id(MoveTemp(InId))
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
					DomainAdapters::PhysicsDeformer::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("physics_deformer_game_thread_required"), TEXT("native Physics/Deformer mechanics must start on the game thread"), TEXT("command_submit")));
				}
				if (!bQuery)
				{
					return OperationOwners::PhysicsDeformer::Submit(
						Request, Caller, Cancellation);
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				UE::UAI::Contract::FDiagnostic Error;
				if (!OperationOwners::PhysicsDeformer::TryResolveMechanics(
						Id, Mechanics, MechanicsScopes, Error)
					|| !Mechanics.IsValid())
				{
					return Failure(Request, Error);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes)
				{
					Scopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return QueryOwners::PhysicsDeformer::Submit(
					Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation);
			}

		private:
			FString Id;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& Id
			: DomainAdapters::PhysicsDeformer::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return OutHandlers.Num()
			== DomainAdapters::PhysicsDeformer::GetCommandIds().Num();
	}
}
