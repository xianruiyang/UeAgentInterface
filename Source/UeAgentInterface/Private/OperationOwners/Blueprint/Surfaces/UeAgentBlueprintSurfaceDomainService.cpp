#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"

#include "CommandHandlers/Blueprint/Surfaces/UeAgentBlueprintSurfaceCommandHandlers.h"

namespace UE::UAI::OperationOwners::Blueprint::Surfaces
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("blueprint-surface-domain-provider");
		const FString SchemaId = TEXT("uai.blueprint.edit/1");
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget
				&& Type == BlueprintType)
			{
				return TEXT("uai.init.blueprint.asset/1");
			}
			if (Role != EInitializationNodeRole::ContainerItem)
			{
				return FString();
			}
			if (Type == TEXT("uai.blueprint.interface")) return TEXT("uai.init.blueprint.interface/1");
			if (Type == TEXT("uai.blueprint.member")) return TEXT("uai.init.blueprint.member/1");
			if (Type == TEXT("uai.blueprint.component")) return TEXT("uai.init.blueprint.component/1");
			if (Type == TEXT("uai.blueprint.graph")) return TEXT("uai.init.blueprint.graph/1");
			if (Type == TEXT("uai.blueprint.node")) return TEXT("uai.init.blueprint.node/1");
			if (Type == TEXT("uai.blueprint.pin")) return TEXT("uai.init.blueprint.pin/1");
			if (Type == TEXT("uai.blueprint.edge")) return TEXT("uai.init.blueprint.edge/1");
			return FString();
		}

		class FBlueprintSurfaceDomainProvider final
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
				return MatchesDomainImportSchema(Request, SchemaId);
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
					|| InitializationContractId
						!= InitializationContract(
							Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_surface_domain_contract_not_executable"),
						Request.GetRole() != EInitializationNodeRole::RootTarget
						? TEXT("This domain executor requires a root-target initialization request.")
						: TEXT("The initialization contract does not match the requested root type."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::Blueprint::Surfaces::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateBlueprintSurfaceDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FBlueprintSurfaceDomainProvider>();
		OutError.Reset();
		return true;
	}
}
