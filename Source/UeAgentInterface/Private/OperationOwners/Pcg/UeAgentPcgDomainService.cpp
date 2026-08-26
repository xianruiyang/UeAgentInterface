#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"

#include "CommandHandlers/Pcg/UeAgentPcgDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::Pcg
{
	using namespace CommandContract;
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
					OutError.Set(TEXT("pcg_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::Pcg::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FUeAgentPcgCandidateCommandError& OutError,
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
					return Fail(OutError, TEXT("pcg_candidate_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(OutError, TEXT("pcg_candidate_command_not_supported"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(OutError, TEXT("pcg_candidate_params_invalid"), Request.CommandId, JsonError);
				}
				FString Target;
				FString Query;
				FString CandidateId;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("pcg_candidate_target_required"), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				Params->TryGetStringField(TEXT("id"), CandidateId);
				int32 Limit = 20;
				int32 Cursor = 0;
				double Number = 0.0;
				if (Params->TryGetNumberField(TEXT("limit"), Number)) Limit = static_cast<int32>(Number);
				if (Params->TryGetNumberField(TEXT("cursor"), Number)) Cursor = static_cast<int32>(Number);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					return Fail(OutError, TEXT("pcg_candidate_target_unresolved"), Target);
				}
				FPcgNodeCandidateQueryResult QueryResult;
				FPcgNodeCandidateQueryError QueryError;
				if (!TryQueryPcgNodeCandidates(
						*TargetObject, Query, QueryResult, QueryError,
						Limit, Cursor, CandidateId))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty() ? FString(TEXT("pcg_candidate_query_failed")) : QueryError.Code,
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
				Payload->SetBoolField(
					TEXT("coverage_complete"), QueryResult.bCoverageComplete);
				Payload->SetBoolField(
					TEXT("creation_coverage_complete"),
					QueryResult.bCreationCoverageComplete);
				Payload->SetNumberField(
					TEXT("native_action_count"), QueryResult.NativeActionCount);
				Payload->SetNumberField(
					TEXT("executable_action_count"),
					QueryResult.ExecutableActionCount);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FPcgNodeCandidate& Candidate : QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), Candidate.Id);
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetStringField(
						TEXT("action_kind"), Candidate.ActionKind);
					Value->SetStringField(TEXT("category"), Candidate.Category);
					Value->SetStringField(TEXT("summary"), Candidate.Summary);
					Value->SetBoolField(TEXT("supported"), Candidate.bSupported);
					Value->SetStringField(
						TEXT("write_support"), Candidate.WriteSupport);
					Value->SetStringField(
						TEXT("support_reason"), Candidate.SupportReason);
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError, TEXT("pcg_candidate_result_invalid"), Request.CommandId, JsonError);
				}
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
		FString& OutError)
	{
		OutService = MakeShared<FPcgCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
