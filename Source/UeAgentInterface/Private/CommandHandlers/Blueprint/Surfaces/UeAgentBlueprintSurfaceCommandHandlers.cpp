#include "CommandHandlers/Blueprint/Surfaces/UeAgentBlueprintSurfaceCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceQueryOwner.h"

namespace UE::UAI::CommandHandlers::Blueprint::Surfaces
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::Blueprint::Surfaces;

	namespace
	{
		const FString CommandId = TEXT("blueprint.authoring.apply");
		const FString Owner = TEXT("blueprint-surface-domain-owner");

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString JsonError;
			if (!Json::TryCanonicalize(Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT("{\"code\":\"blueprint_surface_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		class FRetiredBlueprintSurfaceHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			virtual const FString& GetCommandId() const override
			{
				return CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				(void)Caller;
				(void)Cancellation;
				return Failure(
					Request,
					TEXT("blueprint_legacy_authoring_retired"),
					Request.GetCommandId(),
					TEXT("use content_graph.import_workspace"));
			}
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::Blueprint::Surfaces::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::Blueprint::Surfaces::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(TEXT("blueprint_surface_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateBlueprintSurfaceCommandHandlers(
		TSharedRef<IUeAgentBlueprintSurfaceCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		(void)Service;
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FRetiredBlueprintSurfaceHandler>());
		return true;
	}
}
