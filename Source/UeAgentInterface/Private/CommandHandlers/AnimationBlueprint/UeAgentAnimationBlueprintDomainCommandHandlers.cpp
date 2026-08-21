#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

namespace UE::UAI::CommandHandlers::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::AnimationBlueprint;

	namespace
	{
		const FString CommandId = TEXT("animation_blueprint.authoring.apply");
		const FString Owner = TEXT("animation-blueprint-domain-owner");

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
				ErrorJson = TEXT("{\"code\":\"animation_blueprint_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		class FRetiredAnimationBlueprintHandler final
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
					TEXT("animation_blueprint_legacy_authoring_retired"),
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
		if (!DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::AnimationBlueprint::IsReadOnlyContentGraphCommand(
			Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(
				TEXT("animation_blueprint_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateAnimationBlueprintCommandHandlers(
		TSharedRef<IUeAgentAnimationBlueprintCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		(void)Service;
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(
			MakeShared<FRetiredAnimationBlueprintHandler>());
		return true;
	}
}
