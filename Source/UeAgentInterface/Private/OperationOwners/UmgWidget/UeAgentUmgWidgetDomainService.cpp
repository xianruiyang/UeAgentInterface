#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

namespace UE::UAI::OperationOwners::UmgWidget
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace OperationOwners::UObjectDataAsset;

	namespace
	{
		const FString ProviderId = TEXT("umg-widget-domain-provider");
		const FString SchemaId = TEXT("uai.umg.edit/1");
		const FString WidgetBlueprintType = TEXT("/Script/UMGEditor.WidgetBlueprint");
		const FString PublicCommandId = TEXT("umg.authoring.apply");

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
}
