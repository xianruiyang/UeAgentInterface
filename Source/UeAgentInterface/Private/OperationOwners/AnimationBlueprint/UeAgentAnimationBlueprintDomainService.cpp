#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

namespace UE::UAI::OperationOwners::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace OperationOwners::UObjectDataAsset;
	using namespace QueryOwners::AnimationBlueprint;

	namespace
	{
		const FString ProviderId = TEXT("animation-blueprint-domain-provider");
		const FString SchemaId = TEXT("uai.animation_blueprint.edit/1");
		const FString RootType = TEXT("/Script/Engine.AnimBlueprint");
		const FString PublicCommandId = TEXT("animation_blueprint.authoring.apply");
		const FString CandidateCommandId =
			TEXT("animation.state_machine.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget && Type == RootType)
			{
				return TEXT("uai.init.animation_blueprint.asset/1");
			}
			if (Role != EInitializationNodeRole::ContainerItem)
			{
				return FString();
			}
			if (Type == TEXT("uai.animation_blueprint.graph")) return TEXT("uai.init.animation_blueprint.graph/1");
			if (Type == TEXT("uai.animation_blueprint.state_machine")) return TEXT("uai.init.animation_blueprint.state_machine/1");
			if (Type == TEXT("uai.animation_blueprint.state")) return TEXT("uai.init.animation_blueprint.state/1");
			if (Type == TEXT("uai.animation_blueprint.transition")) return TEXT("uai.init.animation_blueprint.transition/1");
			if (Type == TEXT("uai.animation_blueprint.class_settings")) return TEXT("uai.init.animation_blueprint.class_settings/1");
			if (Type == TEXT("uai.blueprint.interface")) return TEXT("uai.init.blueprint.interface/1");
			if (Type == TEXT("uai.blueprint.member")) return TEXT("uai.init.blueprint.member/1");
			if (Type == TEXT("uai.blueprint.component")) return TEXT("uai.init.blueprint.component/1");
			if (Type == TEXT("uai.blueprint.graph")) return TEXT("uai.init.blueprint.graph/1");
			if (Type == TEXT("uai.blueprint.node")) return TEXT("uai.init.blueprint.node/1");
			if (Type == TEXT("uai.blueprint.pin")) return TEXT("uai.init.blueprint.pin/1");
			if (Type == TEXT("uai.blueprint.edge")) return TEXT("uai.init.blueprint.edge/1");
			return FString();
		}

		class FAnimationBlueprintDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (InitializationContract(
					Request.GetRootTypeIdentity(), Request.GetRole()).IsEmpty())
				{
					return false;
				}
				return Request.GetCommandId()
						!= TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == SchemaId;
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutError.Reset();
				OutInitializationContractId = Matches(Request)
					? InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole())
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
					|| InitializationContractId
						!= InitializationContract(
							Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					OutError.Set(
						TEXT("animation_blueprint_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				return CommandHandlers::AnimationBlueprint::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FAnimationBlueprintCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool FailCandidate(
			FAnimationBlueprintCandidateCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		class FAnimationBlueprintCandidateCommandService final
			: public IUeAgentAnimationBlueprintCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAnimationBlueprintCandidateCommandRequest& Request,
				FAnimationBlueprintCandidateCommandResponse& OutResponse,
				FAnimationBlueprintCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_command_not_supported"),
						Request.CommandId);
				}

				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_params_invalid"),
						Request.CommandId,
						JsonError);
				}
				FString Target;
				FString Surface;
				FString Query;
				FString CandidateId;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_target_required"),
						Request.CommandId);
				}
				if (!Params->TryGetStringField(TEXT("surface"), Surface)
					|| Surface.TrimStartAndEnd().IsEmpty())
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_surface_required"),
						Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				Params->TryGetStringField(TEXT("id"), CandidateId);
				const TSharedPtr<FJsonObject>* ClassSettingsField = nullptr;
				TSharedPtr<FJsonObject> ClassSettings = MakeShared<FJsonObject>();
				if (Params->TryGetObjectField(
						TEXT("class_settings"), ClassSettingsField)
					&& ClassSettingsField && ClassSettingsField->IsValid())
				{
					ClassSettings = *ClassSettingsField;
				}
				int32 Limit = 64;
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

				FAnimationBlueprintCandidateQueryResult QueryResult;
				FAnimationBlueprintCandidateQueryError QueryError;
				if (!TryQueryAnimationBlueprintCandidates(
						Target,
						Surface,
						ClassSettings,
						Query,
						QueryResult,
						QueryError,
						Limit,
						Cursor,
						CandidateId))
				{
					return FailCandidate(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT("animation_blueprint_candidate_query_failed"))
							: QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CandidateCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("target"), QueryResult.Target);
				Payload->SetStringField(
					TEXT("target_revision"), QueryResult.TargetRevision);
				Payload->SetStringField(TEXT("surface"), QueryResult.Surface);
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
				Payload->SetBoolField(
					TEXT("creation_coverage_complete"),
					QueryResult.bCreationCoverageComplete);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FAnimationBlueprintCandidate& Candidate
					: QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), Candidate.Id);
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("surface"), Candidate.Surface);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetStringField(
						TEXT("action_kind"), Candidate.ActionKind);
					Value->SetStringField(TEXT("category"), Candidate.Category);
					Value->SetStringField(TEXT("summary"), Candidate.Summary);
					Value->SetStringField(
						TEXT("node_class"), Candidate.NodeClassPath);
					Value->SetStringField(
						TEXT("source_origin"), Candidate.SourceOrigin);
					Value->SetBoolField(TEXT("supported"), Candidate.bSupported);
					Value->SetStringField(
						TEXT("write_support"), Candidate.WriteSupport);
					Value->SetStringField(
						TEXT("support_reason"), Candidate.SupportReason);
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(
						Payload, OutResponse.PayloadJson, JsonError))
				{
					return FailCandidate(
						OutError,
						TEXT("animation_blueprint_candidate_result_invalid"),
						Request.CommandId,
						JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};

		class FAnimationBlueprintCommandService final
			: public IUeAgentAnimationBlueprintCommandService
		{
		public:
			explicit FAnimationBlueprintCommandService(
				TSharedRef<IUeAgentUObjectDataAssetCommandService> InCommonService)
				: CommonService(MoveTemp(InCommonService))
			{
			}

			virtual bool Execute(
				const FAnimationBlueprintCommandRequest& Request,
				FAnimationBlueprintCommandResponse& OutResponse,
				FAnimationBlueprintCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError,
						TEXT("animation_blueprint_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != PublicCommandId)
				{
					return Fail(OutError,
						TEXT("animation_blueprint_command_not_supported"),
						Request.CommandId);
				}

				FUeAgentUObjectDataAssetCommandRequest CommonRequest;
				CommonRequest.CommandId = TEXT("content_graph.import_workspace");
				CommonRequest.RequestId = Request.RequestId;
				CommonRequest.CanonicalParamsJson = Request.CanonicalParamsJson;
				CommonRequest.CallerId = Request.CallerId;
				CommonRequest.AuthorityScopes = Request.AuthorityScopes;
				CommonRequest.CorrelationId = Request.CorrelationId;
				CommonRequest.bCancellationRequested = Request.bCancellationRequested;
				FUeAgentUObjectDataAssetCommandResponse CommonResponse;
				FUeAgentUObjectDataAssetCommandError CommonError;
				if (!CommonService->Execute(
					CommonRequest, CommonResponse, CommonError))
				{
					return Fail(
						OutError,
						CommonError.Code.IsEmpty()
							? FString(TEXT("animation_blueprint_execution_failed"))
							: CommonError.Code,
						CommonError.SubjectId,
						CommonError.Detail);
				}

				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(
					CommonResponse.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Fail(OutError,
						TEXT("animation_blueprint_result_invalid"),
						Request.CommandId, JsonError);
				}
				Payload->SetStringField(TEXT("command_id"), PublicCommandId);
				if (!Json::TryCanonicalize(
					Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError,
						TEXT("animation_blueprint_result_invalid"),
						Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = false;
				return true;
			}

		private:
			TSharedRef<IUeAgentUObjectDataAssetCommandService> CommonService;
		};
	}

	bool TryCreateAnimationBlueprintDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationBlueprintDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAnimationBlueprintCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCommandService>& OutService,
		FString& OutError)
	{
		TSharedPtr<IUeAgentUObjectDataAssetCommandService> CommonService;
		if (!TryCreateProductionUObjectDataAssetCommandService(
			CommonService, OutError)
			|| !CommonService.IsValid())
		{
			OutService.Reset();
			return false;
		}
		OutService = MakeShared<FAnimationBlueprintCommandService>(
			CommonService.ToSharedRef());
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAnimationBlueprintCandidateCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FAnimationBlueprintCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
