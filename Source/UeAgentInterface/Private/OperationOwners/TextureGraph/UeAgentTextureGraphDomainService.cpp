#include "OperationOwners/TextureGraph/UeAgentTextureGraphDomainService.h"

#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("texture-graph-domain-provider");
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
					== TEXT("content_graph.import_workspace")
					&& Request.GetSchemaId() == GetSemanticWorkspaceSchemaId();
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_domain_contract_not_executable"),
						TEXT("The initialization contract does not support the requested root type in this domain."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					Detail.PropertyPath = TEXT("domain_contract_matches");
					Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.SetFailure(MoveTemp(Detail));
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
					if (!OutError.HasErrors())
					{
						auto Detail = Contract::FDiagnostic::Failure(TEXT("texture_graph_domain_execution_failed"),
							TEXT("The TextureGraph domain command failed without providing a diagnostic."), TEXT("domain_execute"));
						Detail.SourceKind = TEXT("native_result");
						Detail.TerminalStatus = TEXT("state_unknown");
						Detail.MutationMayHaveStarted = true;
						Detail.bRetrySafe = false;
						OutError.SetFailure(MoveTemp(Detail), Request.GetTargetIdentity());
					}
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(Payload), bReadOnly);
				if (!OutResponse.IsValid())
				{
					auto Detail = Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_domain_response_invalid"),
						OutResponse.GetValidationDiagnostic(), TEXT("result_projection"), TEXT("The TextureGraph domain command returned an invalid result payload."));
					Detail.TerminalStatus = bReadOnly ? TEXT("rejected") : TEXT("state_unknown");
					Detail.MutationMayHaveStarted = !bReadOnly;
					Detail.bRetrySafe = bReadOnly;
					OutError.SetFailure(MoveTemp(Detail), Request.GetTargetIdentity());
					return false;
				}
				return true;
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
