#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h") \
	&& __has_include("OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h") \
	&& __has_include("Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h")
#define UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"
#else
#define UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE 0
#endif

#if UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::Blueprint::Component;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Module::CommandRegistration::BlueprintComponent;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentBlueprintComponentCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentBlueprintComponentCommandRequest& Request,
			FUeAgentBlueprintComponentCommandResponse& OutResponse,
			FUeAgentBlueprintComponentCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			OutResponse.PayloadJson = TEXT(
				"{\"command_id\":\"blueprint.component.candidates\"," 
				"\"read_only\":true,\"candidates\":[]}");
			OutResponse.bReadOnly = true;
			return true;
		}

		int32 Calls = 0;
		FUeAgentBlueprintComponentCommandRequest LastRequest;
	};

	bool DecodeRequest(
		const FString& ContractHash,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t12\",")
			TEXT("\"request_id\":\"req-t12\",")
			TEXT("\"command_id\":\"blueprint.component.candidates\",")
			TEXT("\"command_binding_hash\":\"%s\",")
			TEXT("\"params\":{\"target\":\"/Game/T12.BP_T12\",")
			TEXT("\"component_class\":\"/Script/Engine.SceneComponent\",")
			TEXT("\"requested_name\":\"SceneRootT12\"}}"),
			*ContractHash);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T12BlueprintComponentTest,
	"UAI.StrictV2.T12.BLUEPRINT.COMPONENT",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T12BlueprintComponentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE
	TestTrue(TEXT("T12 Blueprint Component authority is available"), false);
	return false;
#else
	FWorkspaceError WorkspaceError;
	TSharedPtr<const FImportInitializationDomainProviderRegistry>
		ProviderRegistry;
	if (!TestTrue(
		TEXT("the process provider catalog freezes with T12"),
		TryFreezeProcessImportInitializationDomainProviders(
			ProviderRegistry, WorkspaceError))
		|| !ProviderRegistry.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}

	FImportInitializationDomainResolution RootResolution;
	TestTrue(
		TEXT("the T12 Blueprint root resolves its unique contract"),
		ProviderRegistry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.component.edit/1"),
				TEXT("/Game/UAI/T12/BP_Component.BP_Component"),
				TEXT("/Script/Engine.Blueprint"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			RootResolution,
			WorkspaceError));
	if (RootResolution.IsValid())
	{
		TestEqual(
			TEXT("the T12 provider identity is exact"),
			RootResolution.GetProvider()->GetProviderId(),
			FString(TEXT("blueprint-component-domain-provider")));
		TestEqual(
			TEXT("the component graph contract is exact"),
			RootResolution.GetInitializationContractId(),
			FString(TEXT("uai.init.blueprint.component_graph/1")));
	}

	for (const FString ComponentClass : {
		FString(TEXT("/Script/Engine.SceneComponent")),
		FString(TEXT("/Script/Engine.PointLightComponent"))})
	{
		FImportInitializationDomainResolution ComponentResolution;
		TestTrue(
			*FString::Printf(
				TEXT("the registered Component class resolves: %s"),
				*ComponentClass),
			ProviderRegistry->TryResolve(
				FImportInitializationDomainRequest(
					TEXT("content_graph.import_workspace"),
					TEXT("uai.blueprint.component.edit/1"),
					TEXT("/Game/UAI/T12/BP_Component.BP_Component#component"),
					ComponentClass,
					EInitializationNodeRole::OwnedObject,
					TEXT("{}")),
				ComponentResolution,
				WorkspaceError));
		if (ComponentResolution.IsValid())
		{
			TestEqual(
				TEXT("the shared recursive Component contract is exact"),
				ComponentResolution.GetInitializationContractId(),
				FString(TEXT("uai.init.blueprint.scs_component/1")));
		}
	}

	FImportInitializationDomainResolution UnsupportedResolution;
	TestFalse(
		TEXT("an unregistered Component class is rejected before write"),
		ProviderRegistry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.component.edit/1"),
				TEXT("/Game/UAI/T12/BP_Component.BP_Component#unsupported"),
				TEXT("/Script/Engine.AudioComponent"),
				EInitializationNodeRole::OwnedObject,
				TEXT("{}")),
			UnsupportedResolution,
			WorkspaceError));

	const TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
	TSharedPtr<const UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>
		CommandRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
		TEXT("the isolated T12 contribution composes"),
		TryCreateBlueprintComponentRegistration(
			Service, CommandRegistry, Dispatch, Error))
		|| !CommandRegistry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = CommandRegistry->FindExact(
		TEXT("blueprint.component.candidates"));
	if (!TestNotNull(TEXT("the exact Component candidate contract exists"), Contract))
	{
		return false;
	}
	TestEqual(
		TEXT("the Component candidate contract has one owner"),
		Contract->GetPublicOwner(),
		FString(TEXT("blueprint-component-authoring-owner")));

	TOptional<FUeAgentCommandRequest> Request;
	if (!TestTrue(
		TEXT("the exact T12 request decodes"),
		DecodeRequest(Contract->GetContractHash(), Request))
		|| !Request.IsSet())
	{
		return false;
	}
	const FUeAgentCommandResult Result = Dispatch->Submit(
		Request.GetValue(),
		FUeAgentCallerContext(
			TEXT("strict-v2-t12"),
			{TEXT("uai:query")},
			TEXT("corr-t12")),
		MakeShared<FNeverCancelled>());
	TestTrue(TEXT("the exact T12 fixture dispatch succeeds"), Result.IsOk());
	TestEqual(TEXT("the T12 fixture service is called once"), Service->Calls, 1);
	TestEqual(
		TEXT("the requested Component name crosses the exact edge unchanged"),
		Service->LastRequest.CanonicalParamsJson,
		FString(TEXT(
			"{\"component_class\":\"/Script/Engine.SceneComponent\"," 
			"\"requested_name\":\"SceneRootT12\"," 
			"\"target\":\"/Game/T12.BP_T12\"}")));
	return true;
#endif
}
