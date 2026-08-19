#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h") \
	&& __has_include("OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h") \
	&& __has_include("Module/CommandRegistration/BlueprintNode/UeAgentBlueprintNodeRegistration.h")
#define UAI_T11_BLUEPRINT_NODE_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Dom/JsonObject.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Module/CommandRegistration/BlueprintNode/UeAgentBlueprintNodeRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "UObject/Package.h"
#else
#define UAI_T11_BLUEPRINT_NODE_AVAILABLE 0
#endif

#if UAI_T11_BLUEPRINT_NODE_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::Blueprint::Node;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Module::CommandRegistration::BlueprintNode;
	using namespace UE::UAI::OperationOwners::BlueprintNode;
	using namespace UE::UAI::OperationOwners::UObjectDataAsset;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentBlueprintNodeCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentBlueprintNodeCommandRequest& Request,
			FUeAgentBlueprintNodeCommandResponse& OutResponse,
			FUeAgentBlueprintNodeCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t11_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("typed fixture failure");
				return false;
			}
			OutResponse.PayloadJson = TEXT(
				"{\"command_id\":\"blueprint.node.candidates\","
				"\"read_only\":true,\"candidates\":[]}");
			OutResponse.bReadOnly = true;
			return true;
		}

		int32 Calls = 0;
		bool bFail = false;
		FUeAgentBlueprintNodeCommandRequest LastRequest;
	};

	bool DecodeRequest(
		const FString& ContractHash,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t11\",")
			TEXT("\"request_id\":\"req-t11\",")
			TEXT("\"command_id\":\"blueprint.node.candidates\",")
			TEXT("\"command_binding_hash\":\"%s\",")
			TEXT("\"params\":{\"target\":\"/Game/T11.BP_T11\",")
			TEXT("\"graph_id\":\"graph:fixture\"}}"),
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

	TSharedPtr<FJsonObject> ParsePayload(
		FAutomationTestBase* Test,
		const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FString Error;
		Test->TestTrue(
			TEXT("T11 payload is canonical JSON"),
			Json::TryParseObject(Json, Object, Error));
		return Object;
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11DomainProviderActivationTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.DomainProviderActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11DomainProviderActivationTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T11_BLUEPRINT_NODE_AVAILABLE
	TestTrue(TEXT("T11 Blueprint node provider is available"), false);
	return false;
#else
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process domain-provider catalog freezes"),
		TryFreezeProcessImportInitializationDomainProviders(Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Root;
	TestTrue(
		TEXT("the Blueprint root resolves the graph contract"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.node.edit/1"),
				TEXT("/Game/UAI/T11/BP_T11.BP_T11"),
				TEXT("/Script/Engine.Blueprint"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Root,
			Error));
	TestTrue(TEXT("the Blueprint root resolution is valid"), Root.IsValid());
	if (Root.IsValid())
	{
		TestEqual(
			TEXT("the Blueprint provider identity is exact"),
			Root.GetProvider()->GetProviderId(),
			FString(TEXT("blueprint-node-domain-provider")));
		TestEqual(
			TEXT("the graph contract is provider-owned"),
			Root.GetInitializationContractId(),
			FString(TEXT("uai.init.blueprint.graph/1")));
	}

	FImportInitializationDomainResolution Node;
	TestTrue(
		TEXT("the graph-owned Branch entry resolves its own contract"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.node.edit/1"),
				TEXT("/Game/UAI/T11/BP_T11.BP_T11#branch"),
				TEXT("/Script/BlueprintGraph.K2Node_IfThenElse"),
				EInitializationNodeRole::OwnedObject,
				TEXT("{}")),
			Node,
			Error));
	TestTrue(TEXT("the graph-owned resolution is valid"), Node.IsValid());
	if (Node.IsValid())
	{
		TestEqual(
			TEXT("the node contract is provider-owned"),
			Node.GetInitializationContractId(),
			FString(TEXT("uai.init.blueprint.node/1")));
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11ExactCandidateDispatchTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.ExactCandidateDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11ExactCandidateDispatchTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T11_BLUEPRINT_NODE_AVAILABLE
	TestTrue(TEXT("T11 exact candidate command is available"), false);
	return false;
#else
	const TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
	TSharedPtr<const UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>
		Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
		TEXT("the isolated T11 contribution composes"),
		TryCreateBlueprintNodeRegistration(
			Service, Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = Registry->FindExact(
		TEXT("blueprint.node.candidates"));
	if (!TestNotNull(TEXT("the exact candidate contract exists"), Contract))
	{
		return false;
	}
	TestEqual(
		TEXT("the candidate contract has one owner"),
		Contract->GetPublicOwner(),
		FString(TEXT("blueprint-node-authoring-owner")));
	TOptional<FUeAgentCommandRequest> Request;
	if (!TestTrue(
		TEXT("the exact request decodes"),
		DecodeRequest(Contract->GetContractHash(), Request))
		|| !Request.IsSet())
	{
		return false;
	}
	FUeAgentCommandResult Result = Dispatch->Submit(
		Request.GetValue(),
		FUeAgentCallerContext(
			TEXT("strict-v2-t11"),
			{TEXT("uai:query")},
			TEXT("corr-t11")),
		MakeShared<FNeverCancelled>());
	TestTrue(TEXT("the fixture result succeeds"), Result.IsOk());
	TestEqual(TEXT("the exact service is called once"), Service->Calls, 1);
	TestEqual(
		TEXT("the exact command identity is unchanged"),
		Service->LastRequest.CommandId,
		FString(TEXT("blueprint.node.candidates")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11ProductionCandidateTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.ProductionCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11ProductionCandidateTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T11_BLUEPRINT_NODE_AVAILABLE
	TestTrue(TEXT("T11 production candidate query is available"), false);
	return false;
#else
	const FString PackageName = TEXT("/Game/UAI/StrictV2/T11/BP_NodeNative");
	const FString ObjectPath = PackageName + TEXT(".BP_NodeNative");
	DeleteAssetIfPresent(ObjectPath);
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(),
		Package,
		TEXT("BP_NodeNative"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("the native Blueprint fixture exists"), Blueprint))
	{
		return false;
	}
	FString GraphId;
	FString GraphError;
	TestTrue(
		TEXT("the real EventGraph has a stable public identity"),
		TryGetFirstEditableGraphIdentity(*Blueprint, GraphId, GraphError));
	TSharedPtr<IUeAgentBlueprintNodeCommandService> Service;
	TestTrue(
		TEXT("the production candidate service composes"),
		TryCreateProductionBlueprintNodeCommandService(Service, GraphError));
	FUeAgentBlueprintNodeCommandRequest Request;
	Request.CommandId = TEXT("blueprint.node.candidates");
	Request.RequestId = TEXT("native-t11-candidate");
	Request.CanonicalParamsJson = FString::Printf(
		TEXT("{\"graph_id\":\"%s\",\"target\":\"%s\"}"),
		*GraphId,
		*ObjectPath);
	FUeAgentBlueprintNodeCommandResponse Response;
	FUeAgentBlueprintNodeCommandError Error;
	TestTrue(
		TEXT("the production candidate query succeeds"),
		Service.IsValid()
			&& Service->Execute(Request, Response, Error));
	const TSharedPtr<FJsonObject> Payload = ParsePayload(
		this, Response.PayloadJson);
	const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
	TestTrue(
		TEXT("the real action database yields candidates"),
		Payload.IsValid()
			&& Payload->TryGetArrayField(TEXT("candidates"), Candidates)
			&& Candidates && Candidates->Num() > 0);
	if (Candidates && Candidates->Num() > 0)
	{
		const TSharedPtr<FJsonObject> Candidate = (*Candidates)[0]->AsObject();
		TestEqual(
			TEXT("the supported candidate name is the exact node-class identity"),
			Candidate->GetStringField(TEXT("name")),
			FString(TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));
	}
	DeleteAssetIfPresent(ObjectPath);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11FailureBeforeWriteTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.FailureBeforeWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11FailureBeforeWriteTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T11_BLUEPRINT_NODE_AVAILABLE
	TestTrue(TEXT("T11 typed prewrite failures are available"), false);
	return false;
#else
	FUeAgentBlueprintNodeCommandRequest Request;
	Request.CommandId = TEXT("blueprint.node.candidates");
	Request.RequestId = TEXT("native-t11-missing-target");
	Request.CanonicalParamsJson = TEXT(
		"{\"graph_id\":\"graph:missing\","
		"\"target\":\"/Game/UAI/StrictV2/T11/Missing.Missing\"}");
	TSharedPtr<IUeAgentBlueprintNodeCommandService> Service;
	FString CreateError;
	TestTrue(
		TEXT("the production service composes for failure testing"),
		TryCreateProductionBlueprintNodeCommandService(Service, CreateError));
	FUeAgentBlueprintNodeCommandResponse Response;
	FUeAgentBlueprintNodeCommandError Error;
	TestFalse(
		TEXT("an unresolved exact Blueprint fails"),
		Service.IsValid() && Service->Execute(Request, Response, Error));
	TestEqual(
		TEXT("the unresolved-target failure is typed"),
		Error.Code,
		FString(TEXT("blueprint_node_target_unresolved")));
	return true;
#endif
}
