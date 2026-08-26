#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::OperationOwners::UmgWidget
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace OperationOwners::UObjectDataAsset;
	using namespace QueryOwners::UmgWidget;

	namespace
	{
		const FString ProviderId = TEXT("umg-widget-domain-provider");
		const FString SchemaId = TEXT("uai.umg.edit/1");
		const FString WidgetBlueprintType = TEXT("/Script/UMGEditor.WidgetBlueprint");
		const FString PublicCommandId = TEXT("umg.authoring.apply");
		const FString CandidateCommandId = TEXT("umg.widget.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget
				&& Type == WidgetBlueprintType)
			{
				return TEXT("uai.init.umg.widget_blueprint/1");
			}
			if (Role != EInitializationNodeRole::ContainerItem)
			{
				return FString();
			}
			if (Type == TEXT("uai.umg.widget")) return TEXT("uai.init.umg.widget/1");
			if (Type == TEXT("uai.umg.slot")) return TEXT("uai.init.umg.slot/1");
			if (Type == TEXT("uai.umg.binding")) return TEXT("uai.init.umg.binding/1");
			if (Type == TEXT("uai.umg.animation")) return TEXT("uai.init.umg.animation/1");
			return FString();
		}

		class FUmgWidgetDomainProvider final
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
					|| InitializationContractId != InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					OutError.Set(TEXT("umg_widget_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				return CommandHandlers::UmgWidget::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FUmgWidgetCommandError& OutError,
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
			FUmgWidgetCandidateCommandError& OutError,
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
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		class FUmgWidgetCandidateCommandService final
			: public IUeAgentUmgWidgetCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUmgWidgetCandidateCommandRequest& Request,
				FUmgWidgetCandidateCommandResponse& OutResponse,
				FUmgWidgetCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return FailCandidate(
						OutError,
						TEXT("umg_widget_candidate_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return FailCandidate(
						OutError,
						TEXT("umg_widget_candidate_command_not_supported"),
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
						TEXT("umg_widget_candidate_params_invalid"),
						Request.CommandId,
						JsonError);
				}
				FString Target;
				FString Query;
				FString CandidateId;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return FailCandidate(
						OutError,
						TEXT("umg_widget_candidate_target_required"),
						Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				Params->TryGetStringField(TEXT("id"), CandidateId);
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
				UWidgetBlueprint* const TargetObject = LoadObject<UWidgetBlueprint>(
					nullptr, *ObjectPath(Target));
				if (!TargetObject
					|| TargetObject->GetClass() != UWidgetBlueprint::StaticClass())
				{
					return FailCandidate(
						OutError,
						TEXT("umg_widget_candidate_target_unresolved"),
						Target);
				}

				FUmgWidgetCandidateQueryResult QueryResult;
				FUmgWidgetCandidateQueryError QueryError;
				if (!TryQueryUmgWidgetCandidates(
						*TargetObject,
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
							? FString(TEXT("umg_widget_candidate_query_failed"))
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
				Payload->SetNumberField(
					TEXT("total_matches"), QueryResult.TotalMatches);
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
				for (const FUmgWidgetCandidate& Candidate : QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), Candidate.Id);
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetStringField(TEXT("action_kind"), Candidate.ActionKind);
					Value->SetStringField(TEXT("category"), Candidate.Category);
					Value->SetStringField(TEXT("summary"), Candidate.Summary);
					Value->SetStringField(TEXT("class_path"), Candidate.ClassPath);
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
						TEXT("umg_widget_candidate_result_invalid"),
						Request.CommandId,
						JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};

		class FUmgWidgetCommandService final : public IUeAgentUmgWidgetCommandService
		{
		public:
			explicit FUmgWidgetCommandService(
				TSharedRef<IUeAgentUObjectDataAssetCommandService> InCommonService)
				: CommonService(MoveTemp(InCommonService))
			{
			}

			virtual bool Execute(
				const FUmgWidgetCommandRequest& Request,
				FUmgWidgetCommandResponse& OutResponse,
				FUmgWidgetCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("umg_widget_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != PublicCommandId)
				{
					return Fail(OutError, TEXT("umg_widget_command_not_supported"),
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
				if (!CommonService->Execute(CommonRequest, CommonResponse, CommonError))
				{
					return Fail(OutError,
						CommonError.Code.IsEmpty()
							? FString(TEXT("umg_widget_execution_failed"))
							: CommonError.Code,
						CommonError.SubjectId, CommonError.Detail);
				}

				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(CommonResponse.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Fail(OutError, TEXT("umg_widget_result_invalid"),
						Request.CommandId, JsonError);
				}
				Payload->SetStringField(TEXT("command_id"), PublicCommandId);
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError, TEXT("umg_widget_result_invalid"),
						Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = false;
				return true;
			}

		private:
			TSharedRef<IUeAgentUObjectDataAssetCommandService> CommonService;
		};
	}

	bool TryCreateUmgWidgetDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		FString RegistrationError;
		if (!RegisterGraphTreeTargetCompositionContracts(RegistrationError))
		{
			OutProvider.Reset();
			OutError.Set(TEXT("umg_widget_composition_registration_failed"),
				RegistrationError);
			return false;
		}
		OutProvider = MakeShared<FUmgWidgetDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionUmgWidgetCommandService(
		TSharedPtr<IUeAgentUmgWidgetCommandService>& OutService,
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
		OutService = MakeShared<FUmgWidgetCommandService>(CommonService.ToSharedRef());
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionUmgWidgetCandidateCommandService(
		TSharedPtr<IUeAgentUmgWidgetCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FUmgWidgetCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
