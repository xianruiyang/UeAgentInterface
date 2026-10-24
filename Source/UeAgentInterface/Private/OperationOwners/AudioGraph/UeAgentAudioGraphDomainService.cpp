#include "OperationOwners/AudioGraph/UeAgentAudioGraphDomainService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/AudioGraph/UeAgentAudioGraphDomainCommandHandlers.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AudioGraph
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
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
					|| MatchesDomainImportSchema(Request, SchemaId);
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_domain_contract_not_executable"),
						Request.GetRole() != EInitializationNodeRole::RootTarget
						? TEXT("This domain executor requires a root-target initialization request.")
						: TEXT("The initialization contract does not match the requested root type."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					const bool bRootTarget = Request.GetRole() == EInitializationNodeRole::RootTarget;
					Detail.PropertyPath = bRootTarget ? TEXT("initialization_contract_matches") : TEXT("root_target_role");
					Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					if (bRootTarget) Detail.PublicMessage += FString::Printf(
						TEXT(" Requested root type: '%s'."), *Request.GetRootTypeIdentity());
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::AudioGraph::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
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
					auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_cancelled"), Request.CommandId,
						TEXT("The audio graph candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_command_not_supported"), Request.CommandId,
						FString::Printf(TEXT("This service accepts only '%s'."), *CandidateCommandId));
					Diagnostic.PropertyPath = TEXT("command_supported");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					if (!JsonError.IsEmpty()) return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(
						TEXT("audio_graph_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_params_invalid"), Request.CommandId,
						TEXT("The audio graph candidate parameters must be a JSON object."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/");
					Diagnostic.PropertyPath = TEXT("params");
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("audio_graph_candidate_target_required"), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("non-empty asset reference")), Params->TryGetField(TEXT("target")),
						TEXT("candidate_query"), TEXT("Audio graph candidate discovery requires a non-empty target asset reference."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_target_unresolved"), Target,
						TEXT("The audio graph candidate target asset could not be loaded."));
					Diagnostic.TargetPath = Target.TrimStartAndEnd();
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable asset"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
				}
				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				if (Params->HasField(TEXT("describe")))
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					CandidateProperties::FRequest DescriptionRequest;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
					{
						auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
							TEXT("audio_graph_candidate_description_invalid"), TEXT("/describe"),
							MakeShared<FJsonValueString>(TEXT("object")), Params->TryGetField(TEXT("describe")),
							TEXT("candidate_query"), TEXT("The candidate description request must be an object."));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
					}
					if (!CandidateProperties::ReadRequest(*Description, DescriptionRequest, JsonError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.SourceKind.IsEmpty()) JsonError.SourceKind = TEXT("command_input");
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, MoveTemp(JsonError), Target);
					}
					FAudioGraphCandidate Candidate;
					FAudioGraphCandidateQueryError ResolveError;
					const bool bSoundCue = TargetObject->GetClass() == USoundCue::StaticClass();
					if (!(bSoundCue ? TryResolveSoundCueCandidate(*TargetObject, DescriptionRequest.Name, Candidate, ResolveError, nullptr, DescriptionRequest.Select)
						: TryResolveAudioGraphCandidate(*TargetObject, DescriptionRequest.Name, Candidate, ResolveError, nullptr, DescriptionRequest.Select)))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(ResolveError, TEXT("/describe"));
						if (ResolveError.TargetPath.IsEmpty()) ResolveError.TargetPath = Target.TrimStartAndEnd();
						if (ResolveError.SourceKind.IsEmpty()) ResolveError.SourceKind = TEXT("command_input");
						return ForwardDiagnostic(OutError, ResolveError, ResolveError.SubjectId);
					}
					TSharedPtr<FJsonObject> Described;
					if (bSoundCue)
					{
						UClass* Class = FindObject<UClass>(nullptr, *Candidate.NativeClassName);
						if (!Class || !Class->IsChildOf(USoundNode::StaticClass()))
						{
							auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_not_found"), DescriptionRequest.Name,
								TEXT("The selected audio candidate does not resolve to a loaded SoundNode class."));
							Diagnostic.TargetPath = Target.TrimStartAndEnd();
							Diagnostic.CandidateName = DescriptionRequest.Name;
							Diagnostic.PropertyPath = TEXT("candidate_class");
							Diagnostic.ActualValue = MakeShared<FJsonValueString>(Candidate.NativeClassName);
							Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(USoundNode::StaticClass()->GetPathName());
							return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
						}
						UeAgentAuthoringState::FEditablePropertyReadOptions Policy;
						Policy.ExcludedProperties = UeAgentAudioAuthoring::SoundNodeStructuralProperties();
						if (!CandidateProperties::DescribeObject(*Class->GetDefaultObject(), DescriptionRequest, MoveTemp(Policy), Described, JsonError))
						{
							auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("audio_graph_candidate_description_failed"),
								JsonError, TEXT("candidate_query"));
							if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TrimStartAndEnd();
							if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = DescriptionRequest.Name;
							return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
						}
					}
					else
					{
						Described = MakeShared<FJsonObject>();
						Described->SetStringField(TEXT("name"), DescriptionRequest.Name);
						Described->SetStringField(TEXT("basis"), TEXT("requires_created_instance"));
						Described->SetStringField(TEXT("reason"), TEXT("frontend_node_inputs_are_resolved_in_the_created_graph"));
					}
					if (!CandidateProperties::BuildPayload(DescriptionRequest, Described, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}

				FAudioGraphCandidateQueryResult QueryResult;
				FAudioGraphCandidateQueryError QueryError;
				if (!TryQueryAudioGraphCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError))
				{
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target.TrimStartAndEnd();
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("audio_graph_candidate_query_failed"), Target,
							TEXT("Audio graph candidate discovery failed without providing a diagnostic."));
						QueryError.PropertyPath = TEXT("query_succeeded");
						QueryError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						QueryError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target.TrimStartAndEnd();
					return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
				}
				const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
					QueryResult.TotalMatches, QueryResult.Candidates.Num(), QueryResult.CatalogDiagnostic};
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(Completeness, JsonError))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}

				TArray<ContentDiscovery::FNameSelectionEntry> Names;
				for (const FAudioGraphCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Identity.CanonicalKey.IsEmpty()
						? Candidate.NativeClassName : Candidate.Identity.CanonicalKey, Candidate.SelectionFields});
				}
				if (!ContentDiscovery::TryBuildCandidateNamesOnlyPayload(
					Names, ContentDiscovery::ENameComparison::IgnoreCase,
					OutResponse.PayloadJson, JsonError, QueryResult.CatalogDiagnostic))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}
				if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FAudioGraphCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
