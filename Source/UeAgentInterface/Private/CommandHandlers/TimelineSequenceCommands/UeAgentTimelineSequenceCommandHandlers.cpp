#include "CommandHandlers/TimelineSequenceCommands/UeAgentTimelineSequenceCommandHandlers.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandOperationOwner.h"
#include "QueryOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Domains/UMG/Authoring/UeAgentUmgAnimationState.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::CommandHandlers::TimelineSequenceCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner(TEXT("timeline-sequence-command-owner"));

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
			explicit FHandler(FString InCommandId) : CommandId(MoveTemp(InCommandId)) {}
			const FString& GetCommandId() const override { return CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				const bool bCandidates = CommandId == TEXT("timeline.track_candidates");
				const bool bQuery = bCandidates || QueryOwners::TimelineSequenceCommands::IsQuery(CommandId);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query")) : FString(TEXT("uai:operate"));
				if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_submit")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_game_thread_required"), TEXT("native timeline mechanics must run on the game thread"), TEXT("command_submit")));
				}
				if (bCandidates)
				{
					TSharedPtr<FJsonObject> Params;
					UE::UAI::Contract::FDiagnostic Error; FString Target, Query, AnimationName, BindingKind, ObjectRef, PropertyPath, Payload;
					if (!CommandContract::Json::TryParseObject(Request.GetParamsJson(), Params, Error))
						return Failure(Request, Error);
					if (!Params->TryGetStringField(TEXT("target"), Target)) {
						Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_track_candidate_target_invalid"),
							TEXT("/target"), MakeShared<FJsonValueString>(TEXT("asset reference string")), Params->TryGetField(TEXT("target")),
							TEXT("command_submit"), TEXT("Track candidate queries require a string target."));
						Error.SourceKind = TEXT("command_input");
						Error.Source.JsonPath = TEXT("/target");
						return Failure(Request, MoveTemp(Error));
					}
					Params->TryGetStringField(TEXT("query"), Query);
					UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
					if (!DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
						return Failure(Request, TargetError);
					Params->TryGetStringField(TEXT("animation_name"), AnimationName);
					Params->TryGetStringField(TEXT("binding_kind"), BindingKind);
					Params->TryGetStringField(TEXT("object_ref"), ObjectRef);
					Params->TryGetStringField(TEXT("property_path"), PropertyPath);
					const bool bResolved = AnimationName.IsEmpty()
						? DomainAdapters::TimelineAnimation::TryQueryLevelSequenceTrackCandidates(Target, Query, ObjectRef, PropertyPath, Payload, Error)
						: UeAgentUmgAuthoring::TryQueryAnimationTrackCandidates(Target, AnimationName, Query, Payload, Error, BindingKind, ObjectRef, PropertyPath);
					if (!bResolved) {
						if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
						return Failure(Request, MoveTemp(Error));
					}
					return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				UE::UAI::Contract::FDiagnostic Error;
				if (!OperationOwners::TimelineSequenceCommands::TryResolveMechanics(
						CommandId, Mechanics, MechanicsScopes, Error)
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
				return OperationOwners::TimelineSequenceCommands::Submit(
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
		for (const FString& CommandId
			: DomainAdapters::TimelineSequenceCommands::GetPublicCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		OutHandlers.Add(MakeShared<FHandler>(TEXT("timeline.track_candidates")));
		return true;
	}
}
