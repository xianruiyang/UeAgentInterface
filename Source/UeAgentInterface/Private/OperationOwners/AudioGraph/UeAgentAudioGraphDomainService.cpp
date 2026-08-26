#include "OperationOwners/AudioGraph/UeAgentAudioGraphDomainService.h"

#include "CommandHandlers/AudioGraph/UeAgentAudioGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AudioGraph
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AudioGraph;

	namespace
	{
		const FString ProviderId = TEXT("audio-graph-domain-provider");
		const FString SchemaId = TEXT("uai.audio_graph.edit/1");
		const FString MetaSoundSourceType = TEXT("/Script/MetasoundEngine.MetaSoundSource");
		const FString SoundCueType = TEXT("/Script/Engine.SoundCue");
		const FString SoundNodeType = TEXT("/Script/Engine.SoundNode");
		const FString EndpointType = TEXT("uai.audio.endpoint");
		const FString ConnectionType = TEXT("uai.audio.connection");
		const FString ParameterType = TEXT("uai.audio.parameter");
		const FString ExternalReferenceType = TEXT("uai.audio.external_reference");
		const FString CandidateCommandId = TEXT("audio.graph.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == MetaSoundSourceType) return TEXT("uai.init.audio.metasound_source.root/1");
				if (Type == SoundCueType) return TEXT("uai.init.audio.sound_cue.root/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject)
			{
				if (Type == SoundNodeType) return TEXT("uai.init.audio.sound_cue_node/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == EndpointType) return TEXT("uai.init.audio.endpoint/1");
				if (Type == ConnectionType) return TEXT("uai.init.audio.connection/1");
				if (Type == ParameterType) return TEXT("uai.init.audio.parameter/1");
				if (Type == ExternalReferenceType) return TEXT("uai.init.audio.external_reference/1");
			}
			return FString();
		}

		class FAudioGraphDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				if (InitializationContract(Request.GetRootTypeIdentity(), Request.GetRole()).IsEmpty())
				{
					return false;
				}
				return Request.GetCommandId() != TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == SchemaId;
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutError.Reset();
				OutInitializationContractId = Matches(Request)
					? InitializationContract(Request.GetRootTypeIdentity(), Request.GetRole())
					: FString();
				return !OutInitializationContractId.IsEmpty();
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (Request.GetRole() != EInitializationNodeRole::RootTarget
					|| InitializationContractId != InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					OutError.Set(TEXT("audio_graph_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::AudioGraph::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FAudioGraphCandidateCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		class FAudioGraphCandidateCommandService final
			: public IUeAgentAudioGraphCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAudioGraphCandidateCommandRequest& Request,
				FAudioGraphCandidateCommandResponse& OutResponse,
				FAudioGraphCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("audio_graph_candidate_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(OutError, TEXT("audio_graph_candidate_command_not_supported"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(OutError, TEXT("audio_graph_candidate_params_invalid"), Request.CommandId, JsonError);
				}
				FString Target;
				FString Query;
				FString CandidateId;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("audio_graph_candidate_target_required"), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				Params->TryGetStringField(TEXT("id"), CandidateId);
				int32 Limit = 64;
				int32 Cursor = 0;
				double Number = 0.0;
				if (Params->TryGetNumberField(TEXT("limit"), Number)) Limit = static_cast<int32>(Number);
				if (Params->TryGetNumberField(TEXT("cursor"), Number)) Cursor = static_cast<int32>(Number);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					return Fail(OutError, TEXT("audio_graph_candidate_target_unresolved"), Target);
				}
				FAudioGraphCandidateQueryResult QueryResult;
				FAudioGraphCandidateQueryError QueryError;
				if (!TryQueryAudioGraphCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError,
						Limit,
						Cursor,
						CandidateId))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT("audio_graph_candidate_query_failed"))
							: QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CandidateCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("target"), Target);
				Payload->SetStringField(TEXT("target_revision"), QueryResult.TargetRevision);
				Payload->SetNumberField(TEXT("total_matches"), QueryResult.TotalMatches);
				Payload->SetNumberField(TEXT("next_cursor"), QueryResult.NextCursor);
				Payload->SetBoolField(TEXT("complete"), QueryResult.bComplete);
				Payload->SetBoolField(TEXT("has_more"), QueryResult.bHasMore);
				Payload->SetStringField(
					TEXT("coverage_scope"), QueryResult.CoverageScope);
				Payload->SetBoolField(
					TEXT("coverage_complete"), QueryResult.bCoverageComplete);
				Payload->SetBoolField(
					TEXT("creation_coverage_complete"),
					QueryResult.bCreationCoverageComplete);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FAudioGraphCandidate& Candidate : QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					if (!Candidate.Id.IsEmpty())
					{
						Value->SetStringField(TEXT("id"), Candidate.Id);
						Value->SetStringField(
							TEXT("action_kind"), Candidate.ActionKind);
						Value->SetStringField(
							TEXT("category"), Candidate.Category);
						Value->SetStringField(
							TEXT("summary"), Candidate.Summary);
						Value->SetBoolField(
							TEXT("supported"), Candidate.bSupported);
						Value->SetStringField(
							TEXT("write_support"), Candidate.WriteSupport);
						Value->SetStringField(
							TEXT("support_reason"), Candidate.SupportReason);
						Value->SetNumberField(TEXT("major_version"), Candidate.MajorVersion);
						Value->SetNumberField(TEXT("minor_version"), Candidate.MinorVersion);
					}
					else
					{
						Value->SetStringField(
							TEXT("target_ref"), Candidate.TargetRef);
						Value->SetStringField(
							TEXT("target_revision"), Candidate.TargetRevision);
						Value->SetArrayField(
							TEXT("supported_operations"),
							StringArray(Candidate.SupportedOperations));
						Value->SetStringField(
							TEXT("input_contract"), Candidate.InputContract);
						Value->SetStringField(
							TEXT("placement_contract"), Candidate.PlacementContract);
						Value->SetStringField(
							TEXT("required_context"), Candidate.RequiredContext);
						Value->SetStringField(
							TEXT("binding_revision"), Candidate.BindingRevision);
					}
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError, TEXT("audio_graph_candidate_result_invalid"), Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};
	}

	bool TryCreateAudioGraphDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAudioGraphDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAudioGraphCandidateCommandService(
		TSharedPtr<IUeAgentAudioGraphCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FAudioGraphCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
