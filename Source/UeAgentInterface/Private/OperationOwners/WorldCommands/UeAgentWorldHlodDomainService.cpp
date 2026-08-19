#include "OperationOwners/WorldCommands/UeAgentWorldHlodDomainService.h"

#include "DomainAdapters/WorldCommands/UeAgentWorldHlodHjsonMechanics.h"

namespace UE::UAI::OperationOwners::WorldCommands
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("world-hlod-domain-provider");
		const FString SchemaId = TEXT("uai.world_hlod_layer.edit/1");
		const FString RootType = TEXT("/Script/Engine.HLODLayer");
		const FString RootContract = TEXT("uai.init.world_hlod_layer.root/1");

		class FWorldHlodDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			const FString& GetProviderId() const override { return ProviderId; }

			bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetRole() != EInitializationNodeRole::RootTarget
					|| Request.GetRootTypeIdentity() != RootType)
				{
					return false;
				}
				return Request.GetCommandId()
					!= TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == SchemaId;
			}

			bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)) return false;
				OutInitializationContractId = RootContract;
				return true;
			}

			bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (!Matches(Request) || InitializationContractId != RootContract)
				{
					OutError.Set(
						TEXT("world_hlod_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FString Payload;
				bool bReadOnly = false;
				if (!DomainAdapters::WorldCommands::TryExecuteHlodContentGraph(
					Request.GetCommandId(),
					Request.GetTargetIdentity(),
					Request.GetCanonicalParamsJson(),
					Payload,
					bReadOnly,
					OutError))
				{
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(Payload), bReadOnly);
				return OutResponse.IsValid();
			}
		};
	}

	bool TryCreateWorldHlodDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FWorldHlodDomainProvider>();
		OutError.Reset();
		return OutProvider.IsValid();
	}
}
