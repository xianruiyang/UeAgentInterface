#include "CommandHandlers/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandHandlers.h"

#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandOperationOwner.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "QueryOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("animation_blueprint_command_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), RequiredScope + TEXT(" is required"));
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		FUeAgentCommandResult NormalizeFailure(
			const FUeAgentCommandRequest& Request,
			FUeAgentCommandResult Result)
		{
			if (Result.IsOk())
			{
				return Result;
			}
			TSharedPtr<FJsonObject> Error;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetErrorJson());
			if (!FJsonSerializer::Deserialize(Reader, Error)
				|| !Error.IsValid())
			{
				return Result;
			}
			FString Code;
			if (!Error->TryGetStringField(TEXT("code"), Code))
			{
				return Result;
			}
			static const TMap<FString, FString> Details = {
				{TEXT("graph_compound_animation_target_already_exists"),
					TEXT("target_ref already resolves to an existing animation asset.")},
				{TEXT("graph_compound_animation_target_invalid"),
					TEXT("target_ref does not identify the required Animation Blueprint target.")},
				{TEXT("graph_compound_animation_target_class_mismatch"),
					TEXT("target_ref resolves to an incompatible animation asset class.")},
			};
			const FString* Detail = Details.Find(Code);
			if (!Detail)
			{
				return Result;
			}
			Error->SetStringField(TEXT("detail"), *Detail);
			FString ErrorJson;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
			FJsonSerializer::Serialize(Error.ToSharedRef(), Writer);
			return FUeAgentCommandResult::Failure(
				Request, Result.GetPayloadJson(), MoveTemp(ErrorJson));
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
				return NormalizeFailure(Request, MoveTemp(Result));
			}
		private:
			FString Id;
			TSharedRef<IUeAgentCommandHandler> Mechanics;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!OperationOwners::AnimationBlueprintCommands::TryCreateMechanicsHandlers(
				Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id
			: DomainAdapters::AnimationBlueprintCommands::GetCommandIds())
		{
			const TSharedRef<IUeAgentCommandHandler>* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = TEXT("animation_blueprint_command_mechanics_handler_missing:") + Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::AnimationBlueprintCommands::GetCommandIds().Num();
	}
}

