#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "CommandHandlers/AnimationRigGraph/UeAgentAnimationRigGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "ControlRigBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AnimationRigGraph
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AnimationRigGraph;

	namespace
	{
		const FString ProviderId = TEXT("animation-rig-graph-domain-provider");
		const FString SchemaId = TEXT("uai.animation_rig_graph.edit/1");
		const FString ControlRigType =
			TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
		const FString IkRigType = TEXT("/Script/IKRig.IKRigDefinition");
		const FString IkRetargeterType = TEXT("/Script/IKRig.IKRetargeter");
		const FString SkeletonType = TEXT("/Script/Engine.Skeleton");
		const FString DeformerGraphType =
			TEXT("/Script/OptimusCore.OptimusDeformer");
		const FString ControlType = TEXT("uai.rig.control");
		const FString BoneType = TEXT("uai.rig.bone");
		const FString ConstraintType = TEXT("uai.rig.constraint");
		const FString SpaceType = TEXT("uai.rig.space");
		const FString CandidateCommandId =
			TEXT("animation_rig_graph.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == ControlRigType) return TEXT("uai.init.rig.control_rig.root/1");
				if (Type == IkRigType) return TEXT("uai.init.rig.ik_rig.root/1");
				if (Type == IkRetargeterType) return TEXT("uai.init.rig.ik_retargeter.root/1");
				if (Type == SkeletonType) return TEXT("uai.init.rig.skeleton.root/1");
				if (Type == DeformerGraphType) return TEXT("uai.init.rig.deformer_graph.root/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == ControlType) return TEXT("uai.init.rig.control/1");
				if (Type == BoneType) return TEXT("uai.init.rig.bone/1");
				if (Type == ConstraintType) return TEXT("uai.init.rig.constraint/1");
				if (Type == SpaceType) return TEXT("uai.init.rig.space/1");
			}
			return FString();
		}

		class FAnimationRigGraphDomainProvider final : public IImportInitializationDomainProvider
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_domain_contract_not_executable"),
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
				return CommandHandlers::AnimationRigGraph::TryHandleDomainRequest(Request, OutResponse, OutError);
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

		class FAnimationRigGraphCandidateCommandService final
			: public IUeAgentAnimationRigGraphCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAnimationRigGraphCandidateCommandRequest& Request,
				FAnimationRigGraphCandidateCommandResponse& OutResponse,
				FAnimationRigGraphCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_cancelled"), Request.CommandId,
						TEXT("The RigVM candidate request was cancelled before discovery started."));
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_command_not_supported"), Request.CommandId,
						TEXT("This service accepts only the RigVM candidate discovery command."));
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
						TEXT("animation_rig_graph_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_params_invalid"), Request.CommandId,
						TEXT("The RigVM candidate parameters must be a JSON object."));
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
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_target_required"), Request.CommandId,
						TEXT("RigVM candidate discovery requires a non-empty target asset reference."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.ActualValue = Params->TryGetField(TEXT("target"));
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty asset reference"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_target_unresolved"), Target,
						TEXT("The target asset could not be loaded for RigVM candidate discovery."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.TargetPath = Target;
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable ControlRigBlueprint asset"));
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
						auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_description_invalid"), Target,
							TEXT("The candidate description request must be an object."));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.Source.JsonPath = TEXT("/describe");
						Diagnostic.TargetPath = Target;
						Diagnostic.ActualValue = Params->TryGetField(TEXT("describe"));
						Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
					}
					if (!CandidateProperties::ReadRequest(*Description, DescriptionRequest, JsonError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, MoveTemp(JsonError));
					}
					UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(TargetObject);
					if (!Blueprint)
					{
						auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_target_type_unsupported"), Target,
							TEXT("Native RigVM creation candidates require a ControlRigBlueprint asset."));
						Diagnostic.SourceKind = TEXT("native_result");
						Diagnostic.TargetPath = Target;
						Diagnostic.PropertyPath = TEXT("target");
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(TargetObject->GetClass()->GetName());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UControlRigBlueprint::StaticClass()->GetName());
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
					}
					FAnimationRigGraphCandidate Candidate;
					FAnimationRigGraphCandidateQueryError QueryError;
					if (!TryResolveAnimationRigGraphCandidate(*Blueprint, DescriptionRequest.Name, Candidate, QueryError, nullptr, DescriptionRequest.Select))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(QueryError, TEXT("/describe"));
						if (QueryError.SourceKind.IsEmpty()) QueryError.SourceKind = TEXT("command_input");
						if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target;
						return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
					}
					if (!CandidateProperties::BuildCreationSupportPayload(DescriptionRequest,
						Candidate.bSupported, Candidate.SupportDiagnostic.IsEmpty() ? Candidate.SupportReason : Candidate.SupportDiagnostic.PublicMessage, TEXT("native_rigvm_action"),
						TEXT("creation_support_only; pin_interface_resolves_in_created_graph_context"),
						OutResponse.PayloadJson, JsonError))
					{
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_graph_candidate_description_failed"), JsonError), DescriptionRequest.Name);
					}
					TSharedPtr<FJsonObject> CreationDescription;
					if (!Json::TryParseObject(OutResponse.PayloadJson, CreationDescription, JsonError))
					{
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_graph_candidate_description_failed"), JsonError), DescriptionRequest.Name);
					}
					const FString ActionForm = Candidate.SelectionFields.FindRef(TEXT("form"));
					if (!ActionForm.IsEmpty()) CreationDescription->SetStringField(TEXT("action_kind"), ActionForm);
					if (!Json::TryCanonicalize(CreationDescription, OutResponse.PayloadJson, JsonError))
					{
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_graph_candidate_description_failed"), JsonError), DescriptionRequest.Name);
					}
					if (!TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}
				FAnimationRigGraphCandidateQueryResult QueryResult;
				FAnimationRigGraphCandidateQueryError QueryError;
				if (!TryQueryAnimationRigGraphCandidates(
						*TargetObject, Query, QueryResult, QueryError))
				{
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_query_failed"), Target,
							TEXT("RigVM candidate discovery failed without providing a diagnostic."));
						QueryError.SourceKind = TEXT("native_result");
						QueryError.PropertyPath = TEXT("query_succeeded");
						QueryError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						QueryError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target;
					return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
				}
				const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
					QueryResult.TotalMatches, QueryResult.Candidates.Num(), QueryResult.CatalogDiagnostic};
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(Completeness, JsonError))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}

			TArray<ContentDiscovery::FNameSelectionEntry> Names;
				for (const FAnimationRigGraphCandidate& Candidate : QueryResult.Candidates)
				{
				Names.Add({Candidate.Name, Candidate.Id, Candidate.SelectionFields});
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

	bool TryCreateAnimationRigGraphDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationRigGraphDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAnimationRigGraphCandidateCommandService(
		TSharedPtr<IUeAgentAnimationRigGraphCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FAnimationRigGraphCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
