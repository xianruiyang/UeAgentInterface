#include "OperationOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainService.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainMechanics.h"
#include "QueryOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialQueryOwner.h"

namespace UE::UAI::OperationOwners::RigSimulationSpatial
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("rig-simulation-spatial-domain-provider");
		const FString SchemaId = TEXT("uai.rig_simulation_spatial.edit/1");
		const FString RootType = TEXT("/Script/Engine.PoseableMeshComponent");
		const FString InitializationContract =
			TEXT("uai.init.rig_simulation_spatial.pose_subject.root/1");

		class FRigSimulationSpatialDomainProvider final
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
				OutError.Reset();
				OutInitializationContractId = Matches(Request)
					? InitializationContract
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
				if (!Matches(Request)
					|| InitializationContractId != InitializationContract)
				{
					OutError.Set(
						TEXT("rig_simulation_spatial_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}

				FString Payload;
				bool bReadOnly = false;
				if (!DomainAdapters::RigSimulationSpatial::TryExecuteContentGraph(
					Request.GetCommandId(), Request.GetTargetIdentity(),
					Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
				{
					return false;
				}
				if (QueryOwners::RigSimulationSpatial::
						IsReadOnlyContentGraphCommand(Request.GetCommandId())
					!= bReadOnly)
				{
					OutError.Set(
						TEXT("rig_simulation_spatial_read_only_contract_mismatch"),
						Request.GetCommandId());
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(Payload), bReadOnly);
				return OutResponse.IsValid();
			}
		};
	}

	bool TryCreateRigSimulationSpatialDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FRigSimulationSpatialDomainProvider>();
		OutError.Reset();
		return true;
	}
}
