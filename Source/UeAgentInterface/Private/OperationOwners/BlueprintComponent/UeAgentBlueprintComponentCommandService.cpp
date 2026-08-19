#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "QueryOwners/BlueprintComponent/UeAgentBlueprintComponentQueryOwner.h"

namespace UE::UAI::OperationOwners::BlueprintComponent
{
	using namespace UE::UAI::DomainAdapters::Blueprint::Component;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::QueryOwners::BlueprintComponent;

	namespace
	{
		const FString CandidateCommandId =
			TEXT("blueprint.component.candidates");
		const FString ImportCommandId =
			TEXT("content_graph.import_workspace");
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");
		const FString SceneComponentType =
			TEXT("/Script/Engine.SceneComponent");
		const FString PointLightComponentType =
			TEXT("/Script/Engine.PointLightComponent");
		const FString SchemaId = TEXT("uai.blueprint.component.edit/1");
		const FString RootContract =
			TEXT("uai.init.blueprint.component_graph/1");
		const FString ComponentContract =
			TEXT("uai.init.blueprint.scs_component/1");
		const FString ProviderId =
			TEXT("blueprint-component-domain-provider");

		bool IsSupportedComponentType(const FString& Type)
		{
			return Type == SceneComponentType
				|| Type == PointLightComponentType;
		}

		bool Fail(
			FUeAgentBlueprintComponentCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		class FProductionService final
			: public IUeAgentBlueprintComponentCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentBlueprintComponentCommandRequest& Request,
				FUeAgentBlueprintComponentCommandResponse& OutResponse,
				FUeAgentBlueprintComponentCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_command_not_supported"),
						Request.CommandId);
				}
				return TryExecuteCandidateQuery(
					Request, OutResponse, OutError);
			}
		};

		class FBlueprintComponentDomainProvider final
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
				if (Request.GetCommandId() != ImportCommandId
					|| Request.GetSchemaId() != SchemaId)
				{
					return false;
				}
				if (Request.GetRole() == EInitializationNodeRole::RootTarget)
				{
					return Request.GetRootTypeIdentity() == BlueprintType;
				}
				return Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& IsSupportedComponentType(
						Request.GetRootTypeIdentity());
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == BlueprintType)
				{
					OutInitializationContractId = RootContract;
					return true;
				}
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& IsSupportedComponentType(
						Request.GetRootTypeIdentity()))
				{
					OutInitializationContractId = ComponentContract;
					return true;
				}
				return false;
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
					|| InitializationContractId != RootContract)
				{
					OutError.Set(
						TEXT("blueprint_component_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FString Payload;
				FUeAgentBlueprintComponentMechanicsError MechanicsError;
				if (!TryExecuteBlueprintComponentImport(
						Request.GetCanonicalParamsJson(),
						Payload,
						MechanicsError))
				{
					OutError.Set(
						MechanicsError.Code.IsEmpty()
							? FString(TEXT("blueprint_component_domain_execution_failed"))
							: MechanicsError.Code,
						MechanicsError.SubjectId,
						MechanicsError.Detail.IsEmpty()
							? FString(TEXT("/"))
							: MechanicsError.Detail);
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(Payload), false);
				return OutResponse.IsValid();
			}
		};

		bool TryCreateBlueprintComponentDomainProvider(
			TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
			FWorkspaceError& OutError)
		{
			OutProvider = MakeShared<FBlueprintComponentDomainProvider>();
			OutError.Reset();
			return true;
		}

		const FImportInitializationDomainProviderFactoryRegistration
			BlueprintComponentDomainProviderRegistration(
				TEXT("blueprint-component"),
				TEXT("blueprint-component-domain-provider"),
				&TryCreateBlueprintComponentDomainProvider);
	}

	bool TryCreateProductionBlueprintComponentCommandService(
		TSharedPtr<IUeAgentBlueprintComponentCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FProductionService>();
		OutError.Reset();
		return true;
	}
}
