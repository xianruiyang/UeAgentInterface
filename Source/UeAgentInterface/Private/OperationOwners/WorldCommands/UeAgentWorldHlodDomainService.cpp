#include "OperationOwners/WorldCommands/UeAgentWorldHlodDomainService.h"

#include "DomainAdapters/WorldCommands/UeAgentWorldHlodHjsonMechanics.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::OperationOwners::WorldCommands
{
	using namespace HjsonWorkspace;
	namespace
	{
		const FString ProviderId = TEXT("world-hlod-domain-provider");
		const FString RootType = TEXT("/Script/Engine.HLODLayer");
		const FString RootContract = TEXT("uai.init.world_hlod_layer.root/1");
		class FProvider final : public IImportInitializationDomainProvider
		{
		public:
			const FString& GetProviderId() const override { return ProviderId; }
			bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				return Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == RootType;
			}
			bool TryResolveInitializationContract(const FImportInitializationDomainRequest& Request,
				FString& OutId, FWorkspaceError& OutError) const override
			{
				OutId.Reset(); OutError.Reset();
				if (!Matches(Request)) return false;
				OutId = RootContract; return true;
			}
			bool TryExecute(const FImportInitializationDomainRequest& Request,
				const FString& ContractId, FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {}; OutError.Reset();
				if (!Matches(Request) || ContractId != RootContract)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_contract_not_executable"),
						TEXT("The initialization contract does not match the HLOD execution contract."), TEXT("domain_execute"));
					if (Request.GetRole() != EInitializationNodeRole::RootTarget) {
						Detail.PublicMessage = TEXT("HLOD execution requires a root-target request.");
						Detail.PropertyPath = TEXT("root_target_role");
						Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
					} else if (Request.GetRootTypeIdentity() != RootType) {
						Detail.PublicMessage = TEXT("The request root type does not match the HLODLayer workspace type.");
						Detail.PropertyPath = TEXT("root_type");
						Detail.ExpectedValue = MakeShared<FJsonValueString>(RootType);
						Detail.ActualValue = MakeShared<FJsonValueString>(Request.GetRootTypeIdentity());
					} else {
						Detail.PropertyPath = TEXT("initialization_contract_matches");
						Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Detail.ExpectedEvidence = MakeShared<FJsonValueString>(RootContract);
						Detail.ActualEvidence = MakeShared<FJsonValueString>(ContractId);
					}
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = ContractId;
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				FString Payload; bool bReadOnly = false;
				if (!DomainAdapters::WorldCommands::TryExecuteHlodContentGraph(
					Request.GetCommandId(), Request.GetTargetIdentity(),
					Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError)) return false;
				OutResponse = FImportInitializationDomainResponse(MoveTemp(Payload), bReadOnly);
				if (!OutResponse.IsValid()) {
					OutError.SetFailure(OutResponse.GetValidationDiagnostic(), Request.GetTargetIdentity());
					return false;
				}
				return true;
			}
		};
	}

	bool TryCreateWorldHlodDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FProvider>(); OutError.Reset();
		return OutProvider.IsValid();
	}
}
