#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

namespace UE::UAI::OperationOwners::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace OperationOwners::UObjectDataAsset;

	namespace
	{
		const FString ProviderId = TEXT("animation-blueprint-domain-provider");
		const FString SchemaId = TEXT("uai.animation_blueprint.edit/1");
		const FString RootType = TEXT("/Script/Engine.AnimBlueprint");
		const FString PublicCommandId = TEXT("animation_blueprint.authoring.apply");

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
}
