#include "OperationOwners/Sequence/UeAgentSequenceDomainService.h"

#include "CommandHandlers/Sequence/UeAgentSequenceDomainCommandHandlers.h"

namespace UE::UAI::OperationOwners::Sequence
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("sequence-domain-provider");
		const FString SchemaId = TEXT("uai.sequence.edit/1");
		const FString RootType = TEXT("/Script/LevelSequence.LevelSequence");
		const FString RootContract = TEXT("uai.init.sequence.root/1");

		class FSequenceDomainProvider final
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
				return CommandId == TEXT("content_graph.authoring_schema")
					|| CommandId == TEXT("content_graph.export_workspace");
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request)
				const override
			{
				return Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == RootType
					&& (Request.GetCommandId()
							!= TEXT("content_graph.import_workspace")
						|| Request.GetSchemaId() == SchemaId);
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
				OutInitializationContractId = RootContract;
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
				if (InitializationContractId != RootContract)
				{
					OutError.Set(
						TEXT("sequence_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				return CommandHandlers::Sequence::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateSequenceDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FSequenceDomainProvider>();
		OutError.Reset();
		return true;
	}
}
