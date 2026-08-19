#include "OperationOwners/TextureGraph/UeAgentTextureGraphDomainService.h"

#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("texture-graph-domain-provider");
		const FString SchemaId = TEXT("uai.texture_graph.edit/1");
		const FString TextureGraphType = TEXT("/Script/TextureGraph.TextureGraph");
		const FString RootContract = TEXT("uai.init.texture_graph.root/1");

		class FTextureGraphDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetRole() != EInitializationNodeRole::RootTarget
					|| Request.GetRootTypeIdentity() != TextureGraphType)
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
				if (!Matches(Request))
				{
					return false;
				}
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
				if (!Matches(Request)
					|| InitializationContractId != RootContract)
				{
					OutError.Set(
						TEXT("texture_graph_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FString Payload;
				bool bReadOnly = false;
				if (!DomainAdapters::TextureGraph::TryExecuteContentGraph(
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

	bool TryCreateTextureGraphDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FTextureGraphDomainProvider>();
		OutError.Reset();
		return OutProvider.IsValid();
	}
}
