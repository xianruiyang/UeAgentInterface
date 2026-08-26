#include "OperationOwners/Niagara/UeAgentNiagaraDomainService.h"

#include "CommandHandlers/Niagara/UeAgentNiagaraDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::Niagara
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::Niagara;

	namespace
	{
		const FString ProviderId = TEXT("niagara-domain-provider");
		const FString SchemaId = TEXT("uai.niagara.edit/1");
		const FString SystemType = TEXT("/Script/Niagara.NiagaraSystem");
		const FString EmitterType = TEXT("/Script/Niagara.NiagaraEmitter");
		const FString ScriptType = TEXT("/Script/Niagara.NiagaraScript");
		const FString StackType = TEXT("uai.niagara.stack");
		const FString ModuleType = TEXT("/Script/NiagaraEditor.NiagaraNodeFunctionCall");
		const FString InputType = TEXT("uai.niagara.input");
		const FString ParameterType = TEXT("uai.niagara.parameter");
		const FString CandidateCommandId = TEXT("niagara.module.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == SystemType) return TEXT("uai.init.niagara_system.root/1");
				if (Type == EmitterType) return TEXT("uai.init.niagara_emitter.root/1");
				if (Type == ScriptType) return TEXT("uai.init.niagara_script.root/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == StackType) return TEXT("uai.init.niagara.stack/1");
				if (Type == InputType) return TEXT("uai.init.niagara.input/1");
				if (Type == ParameterType) return TEXT("uai.init.niagara.parameter/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject && Type == ModuleType)
			{
				return TEXT("uai.init.niagara.module/1");
			}
			return FString();
		}

		class FNiagaraDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				if (InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole()).IsEmpty())
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
					OutError.Set(TEXT("niagara_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::Niagara::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FUeAgentNiagaraCandidateCommandError& OutError,
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

		class FNiagaraCandidateCommandService final
			: public IUeAgentNiagaraCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentNiagaraCandidateCommandRequest& Request,
				FUeAgentNiagaraCandidateCommandResponse& OutResponse,
				FUeAgentNiagaraCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("niagara_candidate_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(OutError, TEXT("niagara_candidate_command_not_supported"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(OutError, TEXT("niagara_candidate_params_invalid"), Request.CommandId, JsonError);
				}
				FString Target;
				FString TargetUsage;
				FString TargetUsageId;
				FString EmitterHandleId;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("niagara_candidate_target_required"), Request.CommandId);
				}
				if (!Params->TryGetStringField(TEXT("target_usage"), TargetUsage)
					|| TargetUsage.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("niagara_candidate_target_usage_required"), Request.CommandId);
				}
				Params->TryGetStringField(
					TEXT("target_usage_id"), TargetUsageId);
				Params->TryGetStringField(
					TEXT("emitter_handle_id"), EmitterHandleId);
				Params->TryGetStringField(TEXT("query"), Query);
				int32 Limit = 20;
				int32 Cursor = 0;
				double Number = 0.0;
				if (Params->TryGetNumberField(TEXT("limit"), Number)) Limit = static_cast<int32>(Number);
				if (Params->TryGetNumberField(TEXT("cursor"), Number)) Cursor = static_cast<int32>(Number);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					return Fail(OutError, TEXT("niagara_candidate_target_unresolved"), Target);
				}

				FNiagaraModuleCandidateQueryResult QueryResult;
				FNiagaraModuleCandidateQueryError QueryError;
				FNiagaraModuleCandidateContext Context;
				Context.TargetUsage = TargetUsage;
				Context.TargetUsageId = TargetUsageId;
				Context.EmitterHandleId = EmitterHandleId;
				if (!TryQueryNiagaraModuleCandidates(
						*TargetObject,
						Query,
						Context,
						QueryResult,
						QueryError,
						Limit,
						Cursor))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty() ? FString(TEXT("niagara_candidate_query_failed")) : QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CandidateCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("target"), Target);
				Payload->SetStringField(
					TEXT("target_usage"), QueryResult.Context.TargetUsage);
				Payload->SetStringField(TEXT("target_revision"), QueryResult.TargetRevision);
				Payload->SetNumberField(TEXT("total_matches"), QueryResult.TotalMatches);
				Payload->SetNumberField(TEXT("next_cursor"), QueryResult.NextCursor);
				Payload->SetBoolField(TEXT("complete"), QueryResult.bComplete);
				Payload->SetBoolField(TEXT("has_more"), QueryResult.bHasMore);
				Payload->SetStringField(
					TEXT("coverage_scope"), QueryResult.CoverageScope);
				Payload->SetBoolField(
					TEXT("coverage_complete"),
					QueryResult.bCoverageComplete);
				Payload->SetBoolField(
					TEXT("creation_coverage_complete"),
					QueryResult.bCreationCoverageComplete);
				Payload->SetArrayField(
					TEXT("unsupported_action_kinds"),
					StringArray(QueryResult.UnsupportedActionKinds));
				const TSharedPtr<FJsonObject> QueryContext =
					MakeShared<FJsonObject>();
				QueryContext->SetStringField(
					TEXT("target_usage"), QueryResult.Context.TargetUsage);
				QueryContext->SetStringField(
					TEXT("target_usage_id"),
					QueryResult.Context.TargetUsageId);
				if (!QueryResult.Context.EmitterHandleId.IsEmpty())
				{
					QueryContext->SetStringField(
						TEXT("emitter_handle_id"),
						QueryResult.Context.EmitterHandleId);
				}
				QueryContext->SetStringField(
					TEXT("stage_key"), QueryResult.Context.StageKey);
				Payload->SetObjectField(TEXT("query_context"), QueryContext);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FNiagaraModuleCandidate& Candidate : QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), Candidate.Id);
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetStringField(
						TEXT("action_kind"), Candidate.ActionKind);
					Value->SetStringField(
						TEXT("category"), Candidate.Category);
					Value->SetBoolField(
						TEXT("supported"), Candidate.bSupported);
					Value->SetStringField(
						TEXT("support_reason"), Candidate.SupportReason);
					Value->SetArrayField(TEXT("supported_operations"), StringArray(Candidate.SupportedOperations));
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError, TEXT("niagara_candidate_result_invalid"), Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};
	}

	bool TryCreateNiagaraDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FNiagaraDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionNiagaraCandidateCommandService(
		TSharedPtr<IUeAgentNiagaraCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FNiagaraCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
