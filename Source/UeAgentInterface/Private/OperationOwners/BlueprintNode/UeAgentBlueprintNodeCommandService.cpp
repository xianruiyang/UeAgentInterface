#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "QueryOwners/BlueprintNode/UeAgentBlueprintNodeQueryOwner.h"

namespace UE::UAI::OperationOwners::BlueprintNode
{
	using namespace UE::UAI::DomainAdapters::Blueprint::Node;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::QueryOwners::BlueprintNode;

	namespace
	{
		const FString CandidateCommandId = TEXT("blueprint.node.candidates");
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");
		const FString BranchType =
			TEXT("/Script/BlueprintGraph.K2Node_IfThenElse");
		const FString SchemaId = TEXT("uai.blueprint.node.edit/1");
		const FString GraphContract = TEXT("uai.init.blueprint.graph/1");
		const FString NodeContract = TEXT("uai.init.blueprint.node/1");
		const FString ProviderId = TEXT("blueprint-node-domain-provider");

		bool Fail(
			FUeAgentBlueprintNodeCommandError& OutError,
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
			: public IUeAgentBlueprintNodeCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentBlueprintNodeCommandRequest& Request,
				FUeAgentBlueprintNodeCommandResponse& OutResponse,
				FUeAgentBlueprintNodeCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_command_not_supported"),
						Request.CommandId);
				}
				return TryExecuteCandidateQuery(
					Request, OutResponse, OutError);
			}
		};

		class FBlueprintNodeDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request)
				const override
			{
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject)
				{
					return Request.GetCommandId()
							== TEXT("content_graph.import_workspace")
						&& Request.GetSchemaId() == SchemaId
						&& Request.GetRootTypeIdentity() == BranchType;
				}
				return Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == BlueprintType
					&& Request.GetCommandId()
						== TEXT("content_graph.import_workspace")
					&& Request.GetSchemaId() == SchemaId;
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
					OutInitializationContractId = GraphContract;
					return true;
				}
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& Request.GetRootTypeIdentity() == BranchType)
				{
					OutInitializationContractId = NodeContract;
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
					|| InitializationContractId != GraphContract)
				{
					OutError.Set(
						TEXT("blueprint_node_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FString Payload;
				bool bReadOnly = false;
				FUeAgentBlueprintNodeMechanicsError MechanicsError;
				if (!TryExecuteBlueprintNodeContentGraph(
						Request.GetCommandId(),
						TEXT("domain:") + Request.GetTargetIdentity(),
						Request.GetCanonicalParamsJson(),
						Payload,
						bReadOnly,
						MechanicsError))
				{
					OutError.Set(
						MechanicsError.Code.IsEmpty()
							? FString(TEXT("blueprint_node_domain_execution_failed"))
							: MechanicsError.Code,
						MechanicsError.SubjectId,
						MechanicsError.Detail.IsEmpty()
							? FString(TEXT("/"))
							: MechanicsError.Detail);
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(Payload), bReadOnly);
				return OutResponse.IsValid();
			}
		};

		bool TryCreateBlueprintNodeDomainProvider(
			TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
			FWorkspaceError& OutError)
		{
			OutProvider = MakeShared<FBlueprintNodeDomainProvider>();
			OutError.Reset();
			return true;
		}

		const FImportInitializationDomainProviderFactoryRegistration
			BlueprintNodeDomainProviderRegistration(
				TEXT("blueprint-node"),
				TEXT("blueprint-node-domain-provider"),
				&TryCreateBlueprintNodeDomainProvider);
	}

	bool TryCreateProductionBlueprintNodeCommandService(
		TSharedPtr<IUeAgentBlueprintNodeCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FProductionService>();
		OutError.Reset();
		return true;
	}
}
