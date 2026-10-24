#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/AiBehavior/UeAgentAiBehaviorDomainCommandHandlers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraphNode.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AiBehavior
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AiBehavior;

	namespace
	{
		const FString ProviderId = TEXT("ai-behavior-domain-provider");
		const FString SchemaId = TEXT("uai.ai_behavior.edit/1");
		const FString BehaviorTreeType = TEXT("/Script/AIModule.BehaviorTree");
		const FString BlackboardType = TEXT("/Script/AIModule.BlackboardData");
		const FString EqsType = TEXT("/Script/AIModule.EnvQuery");
		const FString SmartObjectType =
			TEXT("/Script/SmartObjectsModule.SmartObjectDefinition");
		const FString StateTreeType = TEXT("/Script/StateTreeModule.StateTree");
		const FString BehaviorNodeType = TEXT("/Script/AIModule.BTNode");
		const FString BehaviorServiceType = TEXT("/Script/AIModule.BTService");
		const FString BehaviorDecoratorType = TEXT("/Script/AIModule.BTDecorator");
		const FString StateTreeNodeType =
			TEXT("/Script/StateTreeModule.StateTreeNodeBlueprintBase");
		const FString BlackboardEntryType = TEXT("uai.ai.blackboard_entry");
		const FString ConnectionType = TEXT("uai.ai.connection");
		const FString CandidateCommandId = TEXT("ai.behavior.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == BehaviorTreeType) return TEXT("uai.init.ai.behavior_tree.root/1");
				if (Type == BlackboardType) return TEXT("uai.init.ai.blackboard.root/1");
				if (Type == EqsType) return TEXT("uai.init.ai.eqs.root/1");
				if (Type == SmartObjectType)
				{
					return TEXT("uai.init.ai.smart_object_definition.root/1");
				}
				if (Type == StateTreeType) return TEXT("uai.init.ai.state_tree.root/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject)
			{
				if (Type == BehaviorNodeType) return TEXT("uai.init.ai.behavior_node/1");
				if (Type == BehaviorServiceType) return TEXT("uai.init.ai.behavior_service/1");
				if (Type == BehaviorDecoratorType) return TEXT("uai.init.ai.behavior_decorator/1");
				if (Type == StateTreeNodeType) return TEXT("uai.init.ai.state_tree_node/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == BlackboardEntryType) return TEXT("uai.init.ai.blackboard_entry/1");
				if (Type == ConnectionType) return TEXT("uai.init.ai.connection/1");
			}
			return FString();
		}

		class FAiBehaviorDomainProvider final : public IImportInitializationDomainProvider
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_behavior_domain_contract_not_executable"),
						Request.GetRole() != EInitializationNodeRole::RootTarget
						? TEXT("This domain executor requires a root-target initialization request.")
						: TEXT("The initialization contract does not match the requested root type."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::AiBehavior::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("candidate_query"), Reason);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}


		class FAiBehaviorCandidateCommandService final
			: public IUeAgentAiBehaviorCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAiBehaviorCandidateCommandRequest& Request,
				FAiBehaviorCandidateCommandResponse& OutResponse,
				FAiBehaviorCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_cancelled"), TEXT("The candidate query was cancelled before execution."),
						TEXT("/cancellation_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_command_not_supported"), TEXT("This service does not handle the requested command."),
						TEXT("/command_id"), MakeShared<FJsonValueString>(Request.CommandId), MakeShared<FJsonValueString>(CandidateCommandId)), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return ForwardDiagnostic(OutError, JsonError.IsValid() ? MoveTemp(JsonError)
						: CandidateDiagnostic(TEXT("ai_behavior_candidate_params_invalid"), TEXT("The candidate query decoder did not provide a parameters object."),
							TEXT("/params"), nullptr, MakeShared<FJsonValueString>(TEXT("decoded object"))), Request.CommandId);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_target_required"), TEXT("Candidate queries require a nonempty target asset reference."),
						TEXT("/target"), Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("nonempty asset reference"))), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_target_unresolved"), TEXT("The target asset could not be loaded for candidate discovery."),
						TEXT("/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("loadable asset")), Target);
					Diagnostic.SourceKind = TEXT("native_result");
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				FAiBehaviorCandidateQueryResult QueryResult;
				FAiBehaviorCandidateQueryError QueryError;
				FAiBehaviorCandidateContext Context;
				if (Params->HasField(TEXT("graph_owner")))
				{
					FString Owner;
					UBehaviorTree* Tree = Cast<UBehaviorTree>(TargetObject);
					if (!Tree)
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_graph_context_invalid"), TEXT("A graph_owner context requires a BehaviorTree asset."),
							TEXT("/target"), MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName()), MakeShared<FJsonValueString>(BehaviorTreeType), Target));
					if (!Params->TryGetStringField(TEXT("graph_owner"), Owner) || Owner.IsEmpty())
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_graph_context_invalid"), TEXT("The graph owner reference must be a nonempty string."),
							TEXT("/graph_owner"), Params->TryGetField(TEXT("graph_owner")), MakeShared<FJsonValueString>(TEXT("nonempty graph owner reference")), Target));
					if (Params->HasField(TEXT("parent")) || Params->HasField(TEXT("attachment_kind")))
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_graph_context_invalid"), TEXT("A graph_owner context cannot be combined with attachment context fields."),
							Params->HasField(TEXT("parent")) ? TEXT("/parent") : TEXT("/attachment_kind"), MakeShared<FJsonValueBoolean>(true),
							MakeShared<FJsonValueString>(TEXT("attachment context absent with graph_owner")), Target));
					FWorkspaceError ContextError;
					if (!UeAgentBehaviorTreeAuthoring::TryResolveCapturedGraphOwnerReference(*Tree, Owner, Context.Graph, ContextError))
						return ForwardDiagnostic(OutError, ContextError.GetDiagnostic(), Owner);
				}
				if (Params->HasField(TEXT("parent")) || Params->HasField(TEXT("attachment_kind")))
				{
					FString Parent;
					UBehaviorTree* Tree = Cast<UBehaviorTree>(TargetObject);
					UEnvQuery* Eqs = Cast<UEnvQuery>(TargetObject);
					if (!Tree && !Eqs)
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_attachment_context_invalid"), TEXT("Attachment candidate context requires a BehaviorTree or EnvQuery asset."),
							TEXT("/target"), MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("BehaviorTree or EnvQuery")), Target));
					if (!Params->TryGetStringField(TEXT("parent"), Parent) || Parent.IsEmpty())
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_attachment_context_invalid"), TEXT("Attachment candidate context requires a nonempty parent reference."),
							TEXT("/parent"), Params->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("nonempty parent reference")), Target));
					if (!Params->TryGetStringField(TEXT("attachment_kind"), Context.AttachmentKind))
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_attachment_context_invalid"), TEXT("Attachment candidate context requires a string attachment_kind."),
							TEXT("/attachment_kind"), Params->TryGetField(TEXT("attachment_kind")), MakeShared<FJsonValueString>(TEXT("string")), Target));
					if ((Tree && Context.AttachmentKind != TEXT("service") && Context.AttachmentKind != TEXT("decorator"))
						|| (Eqs && Context.AttachmentKind != TEXT("test")))
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_attachment_context_invalid"), TEXT("The attachment kind is not supported for this asset type."),
							TEXT("/attachment_kind"), MakeShared<FJsonValueString>(Context.AttachmentKind),
							MakeShared<FJsonValueString>(Tree ? TEXT("service or decorator") : TEXT("test")), Target));
					FWorkspaceError ContextError;
					if (Tree)
					{
						UBehaviorTreeGraphNode* ParentNode = nullptr;
						if (!UeAgentBehaviorTreeAuthoring::TryResolveCapturedNodeReference(*Tree, Parent, ParentNode, ContextError))
							return ForwardDiagnostic(OutError, ContextError.GetDiagnostic(), Parent);
						Context.Parent = ParentNode;
					}
					else if (!UeAgentEqsAuthoring::TryResolveCapturedNodeReference(*Eqs, Parent, Context.Parent, ContextError))
						return ForwardDiagnostic(OutError, ContextError.GetDiagnostic(), Parent);
				}
				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				for (const TCHAR* Key : {TEXT("parent"), TEXT("attachment_kind"), TEXT("graph_owner")})
				{
					FString Value;
					if (Params->TryGetStringField(Key, Value)) Scope->SetStringField(Key, Value);
				}
				if (Params->HasField(TEXT("describe")))
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					CandidateProperties::FRequest DescriptionRequest;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
						return ForwardDiagnostic(OutError, CandidateDiagnostic(TEXT("ai_behavior_candidate_description_invalid"), TEXT("The candidate description request must be an object."),
							TEXT("/describe"), Params->TryGetField(TEXT("describe")), MakeShared<FJsonValueString>(TEXT("object")), Target));
					if (!CandidateProperties::ReadRequest(*Description, DescriptionRequest, JsonError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, MoveTemp(JsonError));
					}
					FAiBehaviorCandidate Candidate;
					if (!TryResolveAiBehaviorCandidate(*TargetObject, DescriptionRequest.Name,
						FString(), Candidate, QueryError, DescriptionRequest.Select, Context))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(QueryError, TEXT("/describe"));
						if (QueryError.SourceKind.IsEmpty()) QueryError.SourceKind = TEXT("command_input");
						if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target;
						return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
					}
					if (!CandidateProperties::BuildCreationSupportPayload(DescriptionRequest,
						Candidate.bSupported, Candidate.SupportDiagnostic.IsEmpty() ? Candidate.SupportReason : Candidate.SupportDiagnostic.PublicMessage, TEXT("native_ai_action"),
						TEXT("creation_support_only; editable_fields_resolve_after_creation"),
						OutResponse.PayloadJson, JsonError))
					{
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, MoveTemp(JsonError), DescriptionRequest.Name);
					}
					if (!TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}
				if (!TryQueryAiBehaviorCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError,
						FString(), Context))
				{
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target;
					return ForwardDiagnostic(OutError, !QueryError.IsEmpty() ? QueryError : CandidateDiagnostic(TEXT("ai_behavior_candidate_query_failed"),
						TEXT("Candidate discovery failed without providing a diagnostic."), TEXT("/query"),
						MakeShared<FJsonValueString>(Query), MakeShared<FJsonValueString>(TEXT("completed candidate query")), Target), QueryError.SubjectId);
				}
				if (!QueryResult.CatalogDiagnostic.IsEmpty() && QueryResult.CatalogDiagnostic.TargetPath.IsEmpty())
					QueryResult.CatalogDiagnostic.TargetPath = Target;
				const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
					QueryResult.TotalMatches, QueryResult.Candidates.Num(), QueryResult.CatalogDiagnostic};
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(Completeness, JsonError))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}
				TArray<ContentDiscovery::FNameSelectionEntry> Names;
				for (const FAiBehaviorCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Identity.CanonicalKey, Candidate.SelectionFields});
				}
				if (!TryBuildCandidateNamesOnlyPayload(
						Names, ENameComparison::IgnoreCase,
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

	bool TryCreateAiBehaviorDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAiBehaviorDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAiBehaviorCandidateCommandService(
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FAiBehaviorCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
