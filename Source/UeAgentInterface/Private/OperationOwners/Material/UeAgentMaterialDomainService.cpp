#include "OperationOwners/Material/UeAgentMaterialDomainService.h"

#include "CommandHandlers/Material/UeAgentMaterialDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::Material
{
	using namespace HjsonWorkspace;
	using namespace CommandContract;
	using namespace QueryOwners::Material;

	namespace
	{
		const FString ProviderId = TEXT("material-domain-provider");
		const FString SchemaId = TEXT("uai.material.edit/1");
		const FString MaterialType = TEXT("/Script/Engine.Material");
		const FString FunctionType = TEXT("/Script/Engine.MaterialFunction");
		const FString InstanceType = TEXT("/Script/Engine.MaterialInstanceConstant");
		const FString ExpressionType = TEXT("/Script/Engine.MaterialExpression");
		const FString CandidateCommandId = TEXT("material.expression.candidates");

		FString RootContract(const FString& Type)
		{
			if (Type == MaterialType) return TEXT("uai.init.material.root/1");
			if (Type == FunctionType) return TEXT("uai.init.material_function.root/1");
			if (Type == InstanceType) return TEXT("uai.init.material_instance.root/1");
			return FString();
		}

		class FMaterialDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				const bool bRoot = Request.GetRole() == EInitializationNodeRole::RootTarget
					&& !RootContract(Request.GetRootTypeIdentity()).IsEmpty();
				const bool bOwnedExpression =
					Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& Request.GetRootTypeIdentity() == ExpressionType;
				if (!bRoot && !bOwnedExpression) return false;
				return Request.GetCommandId() != TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == SchemaId;
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)) return false;
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject)
				{
					OutInitializationContractId = TEXT("uai.init.material.expression/1");
				}
				else
				{
					OutInitializationContractId = RootContract(Request.GetRootTypeIdentity());
				}
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
					|| InitializationContractId != RootContract(Request.GetRootTypeIdentity()))
				{
					OutError.Set(TEXT("material_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::Material::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FUeAgentMaterialCandidateCommandError& OutError,
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
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
			return AssetName.IsEmpty() ? Target : Target + TEXT(".") + AssetName;
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

		class FMaterialCandidateCommandService final
			: public IUeAgentMaterialCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentMaterialCandidateCommandRequest& Request,
				FUeAgentMaterialCandidateCommandResponse& OutResponse,
				FUeAgentMaterialCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("material_candidate_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(
						OutError,
						TEXT("material_candidate_command_not_supported"),
						Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(
						OutError,
						TEXT("material_candidate_params_invalid"),
						Request.CommandId,
						JsonError);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("material_candidate_target_required"),
						Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				FMaterialExpressionCandidateContext CandidateContext;
				Params->TryGetStringField(
					TEXT("graph_name"), CandidateContext.GraphName);
				Params->TryGetStringField(
					TEXT("node_guid"), CandidateContext.NodeGuid);
				Params->TryGetStringField(
					TEXT("pin_guid"), CandidateContext.PinGuid);
				Params->TryGetStringField(
					TEXT("pin_name"), CandidateContext.PinName);
				int32 Limit = 20;
				int32 Cursor = 0;
				double Number = 0.0;
				if (Params->TryGetNumberField(TEXT("limit"), Number))
				{
					Limit = static_cast<int32>(Number);
				}
				if (Params->TryGetNumberField(TEXT("cursor"), Number))
				{
					Cursor = static_cast<int32>(Number);
				}
				UObject* TargetObject = StaticLoadObject(
					UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					return Fail(
						OutError,
						TEXT("material_candidate_target_unresolved"),
						Target);
				}

				FMaterialExpressionCandidateQueryResult QueryResult;
				FMaterialExpressionCandidateQueryError QueryError;
				if (!TryQueryMaterialExpressionCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError,
						Limit,
						Cursor,
						CandidateContext))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT("material_candidate_query_failed"))
							: QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CandidateCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("target"), Target);
				Payload->SetStringField(
					TEXT("target_revision"), QueryResult.TargetRevision);
				Payload->SetNumberField(
					TEXT("total_matches"), QueryResult.TotalMatches);
				Payload->SetNumberField(
					TEXT("next_cursor"), QueryResult.NextCursor);
				Payload->SetBoolField(TEXT("complete"), QueryResult.bComplete);
				Payload->SetBoolField(TEXT("has_more"), QueryResult.bHasMore);
				Payload->SetStringField(
					TEXT("coverage_scope"), QueryResult.CoverageScope);
				Payload->SetBoolField(
					TEXT("coverage_complete"), QueryResult.bCoverageComplete);
				const TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
				Context->SetStringField(
					TEXT("graph_name"), QueryResult.Context.GraphName);
				if (!QueryResult.Context.NodeGuid.IsEmpty())
				{
					Context->SetStringField(
						TEXT("node_guid"), QueryResult.Context.NodeGuid);
				}
				if (!QueryResult.Context.PinGuid.IsEmpty())
				{
					Context->SetStringField(
						TEXT("pin_guid"), QueryResult.Context.PinGuid);
				}
				if (!QueryResult.Context.PinName.IsEmpty())
				{
					Context->SetStringField(
						TEXT("pin_name"), QueryResult.Context.PinName);
				}
				Payload->SetObjectField(TEXT("query_context"), Context);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FMaterialExpressionCandidate& Candidate
					: QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), Candidate.Id);
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetStringField(
						TEXT("category"), Candidate.Category);
					Value->SetBoolField(
						TEXT("supported"), Candidate.bSupported);
					Value->SetStringField(
						TEXT("support_reason"), Candidate.SupportReason);
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
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(
						Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(
						OutError,
						TEXT("material_candidate_result_invalid"),
						Request.CommandId,
						JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};
	}

	bool TryCreateMaterialDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FMaterialDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionMaterialCandidateCommandService(
		TSharedPtr<IUeAgentMaterialCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FMaterialCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
