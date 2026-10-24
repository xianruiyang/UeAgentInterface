#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/Pcg/UeAgentPcgDomainCommandHandlers.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGGraph.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "QueryOwners/Pcg/UeAgentPcgDomainQueryOwner.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::Pcg
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::Pcg;

	namespace
	{
		const FString ProviderId = TEXT("pcg-domain-provider");
		const FString SchemaId = TEXT("uai.pcg.edit/1");
		const FString GraphType = TEXT("/Script/PCG.PCGGraph");
		const FString NodeType = TEXT("/Script/PCG.PCGNode");
		const FString EndpointType = TEXT("uai.pcg.endpoint");
		const FString ParameterType = TEXT("uai.pcg.parameter");
		const FString CandidateCommandId = TEXT("pcg.node.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == GraphType) return TEXT("uai.init.pcg_graph.root/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject && Type == NodeType)
			{
				return TEXT("uai.init.pcg.node/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == EndpointType) return TEXT("uai.init.pcg.endpoint/1");
				if (Type == ParameterType) return TEXT("uai.init.pcg.parameter/1");
			}
			return FString();
		}

		class FPcgDomainProvider final : public IImportInitializationDomainProvider
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_domain_contract_not_executable"),
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
				return CommandHandlers::Pcg::TryHandleDomainRequest(Request, OutResponse, OutError);
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

		class FPcgCandidateCommandService final : public IUeAgentPcgCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentPcgCandidateCommandRequest& Request,
				FUeAgentPcgCandidateCommandResponse& OutResponse,
				FUeAgentPcgCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_cancelled"), Request.CommandId,
						TEXT("The PCG candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId && !IsCurrentSpatialQuery(Request.CommandId))
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_command_not_supported"), Request.CommandId,
						TEXT("This service accepts PCG candidate discovery and supported current spatial queries."));
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
						TEXT("pcg_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_params_invalid"), Request.CommandId,
						TEXT("The PCG candidate parameters must be a JSON object."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/");
					Diagnostic.PropertyPath = TEXT("params");
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (IsCurrentSpatialQuery(Request.CommandId))
				{
					TSharedPtr<FJsonObject> Payload;
					UE::UAI::Contract::FDiagnostic SpatialError;
					if (!TryReadCurrentSpatialState(Request.CommandId, Params, Payload, SpatialError))
						return ForwardDiagnostic(OutError, SpatialError, Request.CommandId);
					if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
						return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(
							TEXT("pcg_spatial_result_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					OutResponse.bReadOnly = true;
					return true;
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("pcg_candidate_target_required"), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("non-empty asset reference")), Params->TryGetField(TEXT("target")),
						TEXT("candidate_query"), TEXT("PCG candidate discovery requires a non-empty target asset reference."));
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
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_target_unresolved"), Target,
						TEXT("The PCG candidate target asset could not be loaded."));
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
							TEXT("pcg_candidate_description_invalid"), TEXT("/describe"),
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
					auto* Graph = Cast<UPCGGraph>(TargetObject);
					if (!Graph)
					{
						auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_target_type_unsupported"), Target,
							TEXT("PCG node candidate descriptions require a PCGGraph target."));
						Diagnostic.TargetPath = Target.TrimStartAndEnd();
						Diagnostic.PropertyPath = TEXT("target_class");
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UPCGGraph::StaticClass()->GetPathName());
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
					}
					FPcgNodeCandidateQueryError ResolveError;
					TSharedPtr<FJsonObject> Described;
					if (!TryDescribePcgNodeCandidate(*Graph, DescriptionRequest, Described, ResolveError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(ResolveError, TEXT("/describe"));
						if (ResolveError.TargetPath.IsEmpty()) ResolveError.TargetPath = Target.TrimStartAndEnd();
						if (ResolveError.SourceKind.IsEmpty()) ResolveError.SourceKind = TEXT("command_input");
						return ForwardDiagnostic(OutError, ResolveError, ResolveError.SubjectId);
					}
					if (!CandidateProperties::BuildPayload(DescriptionRequest, Described, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}
				FPcgNodeCandidateQueryResult QueryResult;
				FPcgNodeCandidateQueryError QueryError;
				if (!TryQueryPcgNodeCandidates(
						*TargetObject, Query, QueryResult, QueryError,
						FString()))
				{
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target.TrimStartAndEnd();
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("pcg_candidate_query_failed"), Target,
							TEXT("PCG candidate discovery failed without providing a diagnostic."));
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
				for (const FPcgNodeCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Id, Candidate.SelectionFields});
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

	bool TryCreatePcgDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FPcgDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionPcgCandidateCommandService(
		TSharedPtr<IUeAgentPcgCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FPcgCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
