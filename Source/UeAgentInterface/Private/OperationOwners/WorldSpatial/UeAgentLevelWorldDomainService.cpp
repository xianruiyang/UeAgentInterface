#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"

#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"
#include "Components/ActorComponent.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "GameFramework/Actor.h"

namespace UE::UAI::OperationOwners::WorldSpatial
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("level-world-domain-provider");
		const FString SchemaId = TEXT("uai.world.edit/1");
		const FString WorldType = TEXT("/Script/Engine.World");
		const FString DataLayerType = TEXT("uai.world.data_layer");
		const FString WorldContract = TEXT("uai.init.world.world/1");
		const FString ActorContract = TEXT("uai.init.world.actor/1");
		const FString ComponentContract = TEXT("uai.init.world.component/1");
		const FString DataLayerContract = TEXT("uai.init.world.data_layer/1");

		bool IsChildOf(const FString& ClassPath, const UClass* Base)
		{
			UClass* Candidate = StaticLoadClass(UObject::StaticClass(), nullptr,
				*ClassPath, nullptr, LOAD_NoWarn);
			return Candidate && Candidate->IsChildOf(Base);
		}

		class FLevelWorldDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool SupportsExplicitCommand(
				const FString& CommandId) const override
			{
				return CommandId == TEXT("content_graph.export_workspace");
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetCommandId() == TEXT("content_graph.import_workspace")
					&& Request.GetSchemaId() != SchemaId
					&& Request.GetSchemaId() != GetSemanticWorkspaceSchemaId())
				{
					return false;
				}
				if (Request.GetRole() == EInitializationNodeRole::RootTarget)
				{
					return Request.GetRootTypeIdentity() == WorldType;
				}
				if (Request.GetRole() == EInitializationNodeRole::ContainerItem)
				{
					return Request.GetRootTypeIdentity() == DataLayerType
						|| IsChildOf(Request.GetRootTypeIdentity(), AActor::StaticClass());
				}
				return Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& IsChildOf(Request.GetRootTypeIdentity(),
						UActorComponent::StaticClass());
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request))
				{
					return false;
				}
				if (Request.GetRole() == EInitializationNodeRole::RootTarget)
				{
					OutInitializationContractId = WorldContract;
				}
				else if (Request.GetRootTypeIdentity() == DataLayerType)
				{
					OutInitializationContractId = DataLayerContract;
				}
				else if (Request.GetRole() == EInitializationNodeRole::ContainerItem)
				{
					OutInitializationContractId = ActorContract;
				}
				else
				{
					OutInitializationContractId = ComponentContract;
				}
				return true;
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
					|| InitializationContractId != WorldContract)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_world_domain_contract_not_executable"),
						Request.GetRole() != EInitializationNodeRole::RootTarget
							? TEXT("Level World execution requires a root-target initialization request.")
							: TEXT("The initialization contract does not match the Level World execution contract."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::WorldSpatial::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateLevelWorldDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FLevelWorldDomainProvider>();
		OutError.Reset();
		return true;
	}
}
