#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#if __has_include("DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h") \
	&& __has_include("DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h") \
	&& __has_include("OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h") \
	&& __has_include("Module/CommandRegistration/BlueprintNode/UeAgentBlueprintNodeRegistration.h")
#define UAI_T11_BLUEPRINT_NODE_AVAILABLE 1
#include "BlueprintActionDatabase.h"
#include "BlueprintEditorSettings.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Module/CommandRegistration/BlueprintNode/UeAgentBlueprintNodeRegistration.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "UObject/Package.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"
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
	TestFalse(
		TEXT("the retired node-only schema has no runtime provider"),
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
	TestFalse(TEXT("the retired node-only resolution is invalid"), Root.IsValid());
	TestEqual(
		TEXT("the retired node-only schema fails at provider authority"),
		Error.GetCode(),
		FString(TEXT("hjson_domain_provider_unresolved")));

	Error.Reset();
	TestTrue(
		TEXT("the unified Blueprint root remains the sole write provider"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.edit/1"),
				TEXT("/Game/UAI/T11/BP_T11.BP_T11"),
				TEXT("/Script/Engine.Blueprint"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Root,
			Error));
	TestTrue(TEXT("the unified Blueprint root resolution is valid"), Root.IsValid());
	if (Root.IsValid())
	{
		TestEqual(
			TEXT("the unified Blueprint provider identity is exact"),
			Root.GetProvider()->GetProviderId(),
			FString(TEXT("blueprint-surface-domain-provider")));
		TestEqual(
			TEXT("the unified Blueprint root contract is exact"),
			Root.GetInitializationContractId(),
			FString(TEXT("uai.init.blueprint.asset/1")));
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
	const FString BlueprintName = TEXT("BP_NodeNative_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprintEditorSettings* BlueprintEditorSettings =
		GetMutableDefault<UBlueprintEditorSettings>();
	const bool bOriginalCompactCallOnMemberNodes =
		BlueprintEditorSettings->bCompactCallOnMemberNodes;
	BlueprintEditorSettings->bCompactCallOnMemberNodes = false;
	ON_SCOPE_EXIT
	{
		BlueprintEditorSettings->bCompactCallOnMemberNodes =
			bOriginalCompactCallOnMemberNodes;
	};
	const FString PackageName = TEXT("/Temp/UAI/StrictV2/T11/")
		+ BlueprintName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("the native Blueprint fixture package exists"), Package))
	{
		return false;
	}
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		*BlueprintName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("the native Blueprint fixture exists"), Blueprint))
	{
		return false;
	}
	Blueprint->SetFlags(RF_Public | RF_Standalone);
	TStrongObjectPtr<UBlueprint> BlueprintGuard(Blueprint);
	const FString ObjectPath = Blueprint->GetPathName();
	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0
		? Blueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("the native Blueprint has an event graph"), EventGraph))
	{
		return false;
	}
	FEdGraphPinType FloatPinType;
	FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	TestTrue(
		TEXT("the native Blueprint has a local variable for candidate semantics"),
		FBlueprintEditorUtils::AddMemberVariable(
			Blueprint, FName(TEXT("T84SemanticValue")), FloatPinType));
	USCS_Node* HingeLeftSCS = Blueprint->SimpleConstructionScript
		? Blueprint->SimpleConstructionScript->CreateNode(
			USceneComponent::StaticClass(), TEXT("HingeLeft"))
		: nullptr;
	if (!TestNotNull(
		TEXT("the native Blueprint has a bound SceneComponent fixture"),
		HingeLeftSCS))
	{
		return false;
	}
	Blueprint->SimpleConstructionScript->AddNode(HingeLeftSCS);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(
		TEXT("the variable fixture is compiled before native menu discovery"),
			Blueprint->SkeletonGeneratedClass != nullptr
				&& Blueprint->Status != BS_Error))
	{
		return false;
	}
	TestTrue(
		TEXT("the variable fixture is a native database-eligible asset"),
		Blueprint->IsAsset());
	FBlueprintActionDatabase::Get().RefreshAssetActions(Blueprint);
	ON_SCOPE_EXIT
	{
		FBlueprintActionDatabase::Get().ClearAssetActions(Blueprint);
	};
	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(EventGraph);
	EventNode->NodeGuid = FGuid::NewGuid();
	EventNode->EventReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(AActor, ReceiveTick),
		AActor::StaticClass());
	EventGraph->AddNode(EventNode, false, false);
	EventNode->AllocateDefaultPins();
	UK2Node_Event* ActorOverlapEvent = NewObject<UK2Node_Event>(EventGraph);
	ActorOverlapEvent->NodeGuid = FGuid::NewGuid();
	ActorOverlapEvent->EventReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(AActor, ReceiveActorBeginOverlap),
		AActor::StaticClass());
	EventGraph->AddNode(ActorOverlapEvent, false, false);
	ActorOverlapEvent->AllocateDefaultPins();
	UK2Node_ComponentBoundEvent* BoundEvent =
		NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
	BoundEvent->NodeGuid = FGuid::NewGuid();
	BoundEvent->ComponentPropertyName = TEXT("DetectionVolume");
	BoundEvent->DelegatePropertyName = TEXT("OnComponentBeginOverlap");
	BoundEvent->DelegateOwnerClass = UPrimitiveComponent::StaticClass();
	EventGraph->AddNode(BoundEvent, false, false);
	UK2Node_CallFunction* FunctionNode =
		NewObject<UK2Node_CallFunction>(EventGraph);
	FunctionNode->NodeGuid = FGuid::NewGuid();
	FunctionNode->SetFromFunction(
		UKismetMathLibrary::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FInterpTo)));
	EventGraph->AddNode(FunctionNode, false, false);
	FString GraphId;
	FString GraphError;
	FString StructurePayloadJson;
	bool bStructureReadOnly = false;
	FWorkspaceError StructureError;
	const FString StructureParams = FString::Printf(
		TEXT("{\"target\":\"%s\"}"), *ObjectPath);
	if (!TestTrue(
		TEXT("the active Blueprint exposes its public content graph"),
		UE::UAI::DomainAdapters::Blueprint::Surfaces::TryExecuteContentGraph(
			TEXT("content_graph.structure"), ObjectPath, StructureParams,
			StructurePayloadJson, bStructureReadOnly, StructureError)))
	{
		AddError(StructureError.GetCode());
		return false;
	}
	TestTrue(TEXT("the public content graph is read-only"), bStructureReadOnly);
	const TSharedPtr<FJsonObject> StructurePayload = ParsePayload(
		this, StructurePayloadJson);
	const TArray<TSharedPtr<FJsonValue>>* Containers = nullptr;
	int32 MatchingGraphCount = 0;
	if (StructurePayload.IsValid()
		&& StructurePayload->TryGetArrayField(TEXT("containers"), Containers)
		&& Containers)
	{
		for (const TSharedPtr<FJsonValue>& ContainerValue : *Containers)
		{
			const TSharedPtr<FJsonObject> Container = ContainerValue->AsObject();
			const TSharedPtr<FJsonObject>* State = nullptr;
			FString Kind;
			FString Name;
			FString GraphKind;
			FString CandidateGraphId;
			if (Container.IsValid()
				&& Container->TryGetStringField(TEXT("kind"), Kind)
				&& Kind == TEXT("blueprint_graph")
				&& Container->TryGetObjectField(TEXT("value"), State)
				&& State && State->IsValid()
				&& (*State)->TryGetStringField(TEXT("name"), Name)
				&& Name == TEXT("EventGraph")
				&& (*State)->TryGetStringField(TEXT("graph_kind"), GraphKind)
				&& GraphKind == TEXT("event_graph")
				&& Container->TryGetStringField(TEXT("id"), CandidateGraphId)
				&& !CandidateGraphId.IsEmpty())
			{
				++MatchingGraphCount;
				GraphId = MoveTemp(CandidateGraphId);
			}
		}
	}
	if (!TestEqual(
		TEXT("the public payload uniquely identifies EventGraph"),
		MatchingGraphCount, 1))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	bool bFoundEventSemantics = false;
	bool bFoundBoundEventSemantics = false;
	bool bFoundFunctionSemantics = false;
	if (StructurePayload.IsValid()
		&& StructurePayload->TryGetArrayField(TEXT("items"), Items)
		&& Items)
	{
		for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
		{
			const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
			const TSharedPtr<FJsonObject>* State = nullptr;
			FString Kind;
			if (!Item.IsValid()
				|| !Item->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("blueprint_node")
				|| !Item->TryGetObjectField(TEXT("value"), State)
				|| !State || !State->IsValid())
			{
				continue;
			}
			FString EventName;
			FString EventClass;
			bFoundEventSemantics |=
				(*State)->TryGetStringField(TEXT("event_name"), EventName)
				&& EventName == TEXT("ReceiveTick")
				&& (*State)->TryGetStringField(TEXT("event_class"), EventClass)
				&& EventClass == AActor::StaticClass()->GetPathName();

			FString ComponentName;
			FString DelegateName;
			FString DelegateOwnerClass;
			bFoundBoundEventSemantics |=
				(*State)->TryGetStringField(
					TEXT("component_name"), ComponentName)
				&& ComponentName == TEXT("DetectionVolume")
				&& (*State)->TryGetStringField(
					TEXT("delegate_property_name"), DelegateName)
				&& DelegateName == TEXT("OnComponentBeginOverlap")
				&& (*State)->TryGetStringField(
					TEXT("delegate_owner_class"), DelegateOwnerClass)
				&& DelegateOwnerClass
					== UPrimitiveComponent::StaticClass()->GetPathName();

			FString FunctionName;
			FString FunctionOwnerClass;
			bFoundFunctionSemantics |=
				(*State)->TryGetStringField(
					TEXT("function_name"), FunctionName)
				&& FunctionName == TEXT("FInterpTo")
				&& (*State)->TryGetStringField(
					TEXT("function_owner_class"), FunctionOwnerClass)
				&& FunctionOwnerClass
					== UKismetMathLibrary::StaticClass()->GetPathName();
		}
	}
	TestTrue(
		TEXT("the public structure exposes the native event identity"),
		bFoundEventSemantics);
	TestTrue(
		TEXT("the public structure exposes component-bound event identity"),
		bFoundBoundEventSemantics);
	TestTrue(
		TEXT("the public structure reuses the authoring function identity"),
		bFoundFunctionSemantics);
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
	const bool bProductionQuerySucceeded = Service.IsValid()
		&& Service->Execute(Request, Response, Error);
	TestTrue(
		TEXT("the production candidate query succeeds"),
		bProductionQuerySucceeded);
	if (!bProductionQuerySucceeded)
	{
		AddError(FString::Printf(
			TEXT("production candidate query failed: code=%s subject=%s detail=%s"),
			*Error.Code,
			*Error.SubjectId,
			*Error.Detail));
	}
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
		FString CandidateName;
		FString CandidateId;
		TestTrue(
			TEXT("the native candidate exposes a direct non-empty name"),
			Candidate.IsValid()
				&& Candidate->TryGetStringField(TEXT("name"), CandidateName)
				&& !CandidateName.IsEmpty());
		TestTrue(
			TEXT("the native candidate exposes a compact non-empty id"),
			Candidate.IsValid()
				&& Candidate->TryGetStringField(TEXT("id"), CandidateId)
				&& !CandidateId.IsEmpty());

		FUeAgentBlueprintNodeCommandRequest ResolveRequest;
		ResolveRequest.CommandId = TEXT("blueprint.node.candidates");
		ResolveRequest.RequestId = TEXT("native-t11-candidate-resolve");
		ResolveRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"id\":\"%s\",\"target\":\"%s\"}"),
			*GraphId,
			*CandidateId,
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse ResolveResponse;
		FUeAgentBlueprintNodeCommandError ResolveError;
		TestTrue(
			TEXT("the compact id re-enumerates the same native context"),
			Service->Execute(ResolveRequest, ResolveResponse, ResolveError));
		const TSharedPtr<FJsonObject> ResolvePayload = ParsePayload(
			this, ResolveResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* ResolvedCandidates = nullptr;
		TestTrue(
			TEXT("the compact id resolves exactly one candidate"),
			ResolvePayload.IsValid()
				&& ResolvePayload->TryGetArrayField(
					TEXT("candidates"), ResolvedCandidates)
				&& ResolvedCandidates && ResolvedCandidates->Num() == 1);
		if (ResolvedCandidates && ResolvedCandidates->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Resolved =
				(*ResolvedCandidates)[0]->AsObject();
			TestEqual(
				TEXT("id resolution preserves the native candidate name"),
				Resolved->GetStringField(TEXT("name")),
				CandidateName);
			TestEqual(
				TEXT("id resolution preserves the compact candidate identity"),
				Resolved->GetStringField(TEXT("id")),
				CandidateId);
		}
	}

	auto QueryCandidates = [&Service, &GraphId, &ObjectPath, this](
		const FString& Query,
		const FString& RequestId,
		const TArray<TSharedPtr<FJsonValue>>*& OutCandidates,
		TSharedPtr<FJsonObject>& OutPayload) -> bool
	{
		FUeAgentBlueprintNodeCommandRequest CandidateRequest;
		CandidateRequest.CommandId = TEXT("blueprint.node.candidates");
		CandidateRequest.RequestId = RequestId;
		CandidateRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"limit\":20,\"query\":\"%s\",\"target\":\"%s\"}"),
			*GraphId,
			*Query,
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse CandidateResponse;
		FUeAgentBlueprintNodeCommandError CandidateError;
		if (!Service.IsValid()
			|| !Service->Execute(
				CandidateRequest, CandidateResponse, CandidateError))
		{
			AddError(FString::Printf(
				TEXT("candidate query failed: code=%s subject=%s detail=%s"),
				*CandidateError.Code,
				*CandidateError.SubjectId,
				*CandidateError.Detail));
			return false;
		}
		OutPayload = ParsePayload(this, CandidateResponse.PayloadJson);
		return OutPayload.IsValid()
			&& OutPayload->TryGetArrayField(TEXT("candidates"), OutCandidates)
			&& OutCandidates;
	};

	const TArray<TSharedPtr<FJsonValue>>* BoundCandidates = nullptr;
	TSharedPtr<FJsonObject> BoundPayload;
	FString BoundCandidateName;
	FString BoundCandidateId;
	int32 BoundCandidateCount = 0;
	if (TestTrue(
			TEXT("the component-bound action can be queried by its native menu context"),
			QueryCandidates(
				TEXT("HingeLeft Set Relative Rotation"),
				TEXT("native-t11-component-bound-action"),
				BoundCandidates,
				BoundPayload)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *BoundCandidates)
		{
			const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
			FString FunctionName;
			FString FunctionOwnerClass;
			if (Candidate.IsValid()
				&& Candidate->TryGetStringField(
					TEXT("function_name"), FunctionName)
				&& FunctionName == TEXT("K2_SetRelativeRotation")
				&& Candidate->TryGetStringField(
					TEXT("function_owner_class"), FunctionOwnerClass)
				&& FunctionOwnerClass
					== USceneComponent::StaticClass()->GetPathName())
			{
				++BoundCandidateCount;
				Candidate->TryGetStringField(
					TEXT("name"), BoundCandidateName);
				Candidate->TryGetStringField(
					TEXT("id"), BoundCandidateId);
			}
		}
	}
	TestEqual(
		TEXT("the targeted component-bound action is unique"),
		BoundCandidateCount,
		1);
	if (!BoundCandidateId.IsEmpty())
	{
		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}
		UeAgentPaletteSearch::FSearchParams BoundPerformParams;
		BoundPerformParams.CatalogType = TEXT("blueprint_node");
		BoundPerformParams.AssetPath = ObjectPath;
		BoundPerformParams.GraphName = GraphId;
		BoundPerformParams.bContextSensitive = true;
		UEdGraphNode* BoundPrimaryNode = nullptr;
		UeAgentPaletteSearch::FCandidate PerformedBoundCandidate;
		FString BoundPerformError;
		TestTrue(
			TEXT("one native component-bound action may create its companion node"),
			UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
				BoundPerformParams,
				BoundCandidateName,
				BoundCandidateId,
				FVector2f(900.0f, -320.0f),
				BoundPrimaryNode,
				PerformedBoundCandidate,
				BoundPerformError));
		TArray<UEdGraphNode*> BoundCreatedNodes;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				BoundCreatedNodes.Add(Node);
			}
		}
		TestEqual(
			TEXT("the native bound action creates one primary and one companion"),
			BoundCreatedNodes.Num(),
			2);
		UK2Node_CallFunction* BoundCall =
			Cast<UK2Node_CallFunction>(BoundPrimaryNode);
		TestNotNull(
			TEXT("the returned primary is the native function call"),
			BoundCall);
		UK2Node_VariableGet* BoundGetter = nullptr;
		for (UEdGraphNode* Node : BoundCreatedNodes)
		{
			if (UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
			{
				if (Getter->GetVarName() == TEXT("HingeLeft"))
				{
					BoundGetter = Getter;
					break;
				}
			}
		}
		TestNotNull(
			TEXT("the native action preserves its HingeLeft getter companion"),
			BoundGetter);
		if (BoundCall && BoundGetter)
		{
			const UFunction* TargetFunction = BoundCall->GetTargetFunction();
			TestTrue(
				TEXT("the returned primary keeps K2_SetRelativeRotation semantics"),
				TargetFunction
					&& TargetFunction->GetFName()
						== TEXT("K2_SetRelativeRotation"));
			const UEdGraphPin* SelfPin = BoundCall->FindPin(
				UEdGraphSchema_K2::PN_Self, EGPD_Input);
			TestTrue(
				TEXT("the native companion remains linked to the call self pin"),
				SelfPin
					&& SelfPin->LinkedTo.Contains(BoundGetter->GetValuePin()));
		}
	}

	UEdGraphPin* OtherActorPin = ActorOverlapEvent->FindPin(
		TEXT("OtherActor"), EGPD_Output);
	TestNotNull(
		TEXT("the actor-overlap fixture exposes its native OtherActor pin"),
		OtherActorPin);
	if (OtherActorPin)
	{
		FUeAgentBlueprintNodeCommandRequest EqualRequest;
		EqualRequest.CommandId = TEXT("blueprint.node.candidates");
		EqualRequest.RequestId = TEXT("native-t11-equal-semantics");
		EqualRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"limit\":20,\"node_guid\":\"%s\",\"pin_name\":\"OtherActor\",\"query\":\"Equal\",\"target\":\"%s\"}"),
			*GraphId,
			*ActorOverlapEvent->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse EqualResponse;
		FUeAgentBlueprintNodeCommandError EqualError;
		TestTrue(
			TEXT("Equal candidates use the real OtherActor pin context"),
			Service.IsValid()
				&& Service->Execute(EqualRequest, EqualResponse, EqualError));
		const TSharedPtr<FJsonObject> EqualPayload = ParsePayload(
			this, EqualResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* EqualCandidates = nullptr;
		if (EqualPayload.IsValid())
		{
			EqualPayload->TryGetArrayField(TEXT("candidates"), EqualCandidates);
		}
		FString EqualCandidateName;
		FString EqualCandidateId;
		if (EqualCandidates)
		{
			for (const TSharedPtr<FJsonValue>& Value : *EqualCandidates)
			{
				const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
				FString OperationName;
				if (!Candidate.IsValid()
					|| !Candidate->TryGetStringField(
						TEXT("operation_name"), OperationName)
					|| OperationName != TEXT("EqualEqual"))
				{
					continue;
				}
				TestEqual(
					TEXT("EqualEqual projects the promotable operator node class"),
					Candidate->GetStringField(TEXT("node_class")),
					UK2Node_PromotableOperator::StaticClass()->GetPathName());
				TestFalse(
					TEXT("EqualEqual does not claim one representative function"),
					Candidate->HasField(TEXT("function_name"))
						|| Candidate->HasField(TEXT("function_owner_class")));
				Candidate->TryGetStringField(TEXT("name"), EqualCandidateName);
				Candidate->TryGetStringField(TEXT("id"), EqualCandidateId);
				break;
			}
		}
		TestFalse(
			TEXT("EqualEqual has a native contextual candidate"),
			EqualCandidateId.IsEmpty());
		if (!EqualCandidateId.IsEmpty())
		{
			UeAgentPaletteSearch::FSearchParams PerformParams;
			PerformParams.CatalogType = TEXT("blueprint_node");
			PerformParams.AssetPath = ObjectPath;
			PerformParams.GraphName = GraphId;
			PerformParams.NodeGuid = ActorOverlapEvent->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			PerformParams.PinName = TEXT("OtherActor");
			PerformParams.bContextSensitive = true;
			UEdGraphNode* CreatedNode = nullptr;
			UeAgentPaletteSearch::FCandidate PerformedCandidate;
			FString PerformError;
			TestTrue(
				TEXT("the native EqualEqual candidate creates from OtherActor"),
				UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
					PerformParams,
					EqualCandidateName,
					EqualCandidateId,
					FVector2f(640.0f, -80.0f),
					CreatedNode,
					PerformedCandidate,
					PerformError));
			const UK2Node_PromotableOperator* CreatedOperator =
				Cast<UK2Node_PromotableOperator>(CreatedNode);
			TestNotNull(
				TEXT("EqualEqual creates a promotable operator node"),
				CreatedOperator);
			if (CreatedOperator)
			{
				TestEqual(
					TEXT("the created operator preserves native EqualEqual"),
					CreatedOperator->GetOperationName(),
					FName(TEXT("EqualEqual")));
				TestTrue(
					TEXT("the created EqualEqual is wired from OtherActor"),
					OtherActorPin->LinkedTo.ContainsByPredicate(
						[CreatedOperator](const UEdGraphPin* LinkedPin)
						{
							return LinkedPin
								&& LinkedPin->GetOwningNode()
									== CreatedOperator;
						}));
			}
		}
	}

	UEdGraphPin* DeltaSecondsPin = EventNode->FindPin(
		TEXT("DeltaSeconds"), EGPD_Output);
	TestNotNull(
		TEXT("the ReceiveTick fixture exposes its native DeltaSeconds pin"),
		DeltaSecondsPin);
	FString MultiplyCandidateName;
	FString MultiplyCandidateId;
	if (DeltaSecondsPin)
	{
		FUeAgentBlueprintNodeCommandRequest MultiplyRequest;
		MultiplyRequest.CommandId = TEXT("blueprint.node.candidates");
		MultiplyRequest.RequestId = TEXT("native-t11-multiply-semantics");
		MultiplyRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"limit\":20,\"node_guid\":\"%s\",\"pin_name\":\"DeltaSeconds\",\"query\":\"Multiply\",\"target\":\"%s\"}"),
			*GraphId,
			*EventNode->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse MultiplyResponse;
		FUeAgentBlueprintNodeCommandError MultiplyError;
		TestTrue(
			TEXT("Multiply candidates use the real DeltaSeconds pin context"),
			Service.IsValid()
				&& Service->Execute(
					MultiplyRequest, MultiplyResponse, MultiplyError));
		const TSharedPtr<FJsonObject> MultiplyPayload = ParsePayload(
			this, MultiplyResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* MultiplyCandidates = nullptr;
		if (MultiplyPayload.IsValid())
		{
			MultiplyPayload->TryGetArrayField(
				TEXT("candidates"), MultiplyCandidates);
		}
		if (MultiplyCandidates)
		{
			for (const TSharedPtr<FJsonValue>& Value : *MultiplyCandidates)
			{
				const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
				FString OperationName;
				if (!Candidate.IsValid()
					|| !Candidate->TryGetStringField(
						TEXT("operation_name"), OperationName)
					|| OperationName != TEXT("Multiply"))
				{
					continue;
				}
				TestEqual(
					TEXT("Multiply projects the promotable operator node class"),
					Candidate->GetStringField(TEXT("node_class")),
					UK2Node_PromotableOperator::StaticClass()->GetPathName());
				TestFalse(
					TEXT("Multiply does not claim one representative function"),
					Candidate->HasField(TEXT("function_name"))
						|| Candidate->HasField(TEXT("function_owner_class")));
				TestFalse(
					TEXT("Multiply does not expose its private action signature"),
					Candidate->HasField(TEXT("native_action_signature")));
				Candidate->TryGetStringField(
					TEXT("name"), MultiplyCandidateName);
				Candidate->TryGetStringField(
					TEXT("id"), MultiplyCandidateId);
				break;
			}
		}
	}
	TestFalse(
		TEXT("Multiply has a stable promotable-operator candidate"),
		MultiplyCandidateId.IsEmpty());

	if (DeltaSecondsPin && !MultiplyCandidateId.IsEmpty())
	{
		FUeAgentBlueprintNodeCommandRequest ResolveMultiplyRequest;
		ResolveMultiplyRequest.CommandId = TEXT("blueprint.node.candidates");
		ResolveMultiplyRequest.RequestId =
			TEXT("native-t11-multiply-resolve");
		ResolveMultiplyRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"id\":\"%s\",\"node_guid\":\"%s\",\"pin_name\":\"DeltaSeconds\",\"target\":\"%s\"}"),
			*GraphId,
			*MultiplyCandidateId,
			*EventNode->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse ResolveMultiplyResponse;
		FUeAgentBlueprintNodeCommandError ResolveMultiplyError;
		TestTrue(
			TEXT("Multiply compact id re-resolves in the same pin context"),
			Service->Execute(
				ResolveMultiplyRequest,
				ResolveMultiplyResponse,
				ResolveMultiplyError));
		const TSharedPtr<FJsonObject> ResolveMultiplyPayload = ParsePayload(
			this, ResolveMultiplyResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* ResolvedMultiplyCandidates =
			nullptr;
		TestTrue(
			TEXT("Multiply compact id resolves exactly one candidate"),
			ResolveMultiplyPayload.IsValid()
				&& ResolveMultiplyPayload->TryGetArrayField(
					TEXT("candidates"), ResolvedMultiplyCandidates)
				&& ResolvedMultiplyCandidates
				&& ResolvedMultiplyCandidates->Num() == 1);
		if (ResolvedMultiplyCandidates
			&& ResolvedMultiplyCandidates->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Resolved =
				(*ResolvedMultiplyCandidates)[0]->AsObject();
			TestEqual(
				TEXT("Multiply id resolution preserves operator semantics"),
				Resolved->GetStringField(TEXT("operation_name")),
				FString(TEXT("Multiply")));
			TestFalse(
				TEXT("resolved Multiply still has no representative function"),
				Resolved->HasField(TEXT("function_name"))
					|| Resolved->HasField(TEXT("function_owner_class")));
		}

		auto BuildMultiplyAssertion = [
			&MultiplyCandidateId,
			&EventNode](const FString& OperationName)
		{
			const TSharedPtr<FJsonObject> Candidate =
				MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("id"), MultiplyCandidateId);
			Candidate->SetStringField(TEXT("operation_name"), OperationName);
			const TSharedPtr<FJsonObject> CandidateContext =
				MakeShared<FJsonObject>();
			CandidateContext->SetStringField(
				TEXT("node_guid"),
				EventNode->NodeGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower));
			CandidateContext->SetStringField(
				TEXT("pin_name"), TEXT("DeltaSeconds"));
			const TSharedPtr<FJsonObject> NodeSpec =
				MakeShared<FJsonObject>();
			NodeSpec->SetObjectField(TEXT("candidate"), Candidate);
			NodeSpec->SetObjectField(
				TEXT("candidate_context"), CandidateContext);
			const TSharedPtr<FJsonObject> Nodes = MakeShared<FJsonObject>();
			Nodes->SetObjectField(TEXT("multiply_assertion"), NodeSpec);
			const TSharedPtr<FJsonObject> GraphSpec =
				MakeShared<FJsonObject>();
			GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
			const TSharedPtr<FJsonObject> Graphs = MakeShared<FJsonObject>();
			Graphs->SetObjectField(TEXT("event_graph:EventGraph"), GraphSpec);
			const TSharedPtr<FJsonObject> ProviderExtension =
				MakeShared<FJsonObject>();
			ProviderExtension->SetObjectField(TEXT("graphs"), Graphs);
			return ProviderExtension;
		};
		FWorkspaceError ValidMultiplyAssertionError;
		TestTrue(
			TEXT("matching operation_name assertion normalizes successfully"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					ObjectPath,
					BuildMultiplyAssertion(TEXT("Multiply")),
					ValidMultiplyAssertionError));
		FWorkspaceError InvalidMultiplyAssertionError;
		TestFalse(
			TEXT("conflicting operation_name assertion fails before write"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					ObjectPath,
					BuildMultiplyAssertion(TEXT("Add")),
					InvalidMultiplyAssertionError));
		TestEqual(
			TEXT("conflicting operation_name has a typed failure"),
			InvalidMultiplyAssertionError.GetCode(),
			FString(TEXT(
				"blueprint_surface_node_candidate_metadata_mismatch")));

		UeAgentPaletteSearch::FSearchParams PerformParams;
		PerformParams.CatalogType = TEXT("blueprint_node");
		PerformParams.AssetPath = ObjectPath;
		PerformParams.GraphName = GraphId;
		PerformParams.NodeGuid = EventNode->NodeGuid.ToString(
			EGuidFormats::DigitsWithHyphensLower);
		PerformParams.PinName = TEXT("DeltaSeconds");
		PerformParams.bContextSensitive = true;
		UEdGraphNode* CreatedNode = nullptr;
		UeAgentPaletteSearch::FCandidate PerformedCandidate;
		FString PerformError;
		const int32 NodeCountBeforeMultiply = EventGraph->Nodes.Num();
		TestTrue(
			TEXT("the ordinary unbound Multiply candidate still creates normally"),
			UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
				PerformParams,
				MultiplyCandidateName,
				MultiplyCandidateId,
				FVector2f(640.0f, 160.0f),
				CreatedNode,
				PerformedCandidate,
				PerformError));
		TestEqual(
			TEXT("the ordinary unbound action creates only its primary node"),
			EventGraph->Nodes.Num(),
			NodeCountBeforeMultiply + 1);
		UK2Node_PromotableOperator* CreatedOperator =
			Cast<UK2Node_PromotableOperator>(CreatedNode);
		TestNotNull(
			TEXT("Multiply creates a promotable operator node"),
			CreatedOperator);
		if (CreatedOperator)
		{
			TestEqual(
				TEXT("the created promotable operator preserves Multiply"),
				CreatedOperator->GetOperationName(),
				FName(TEXT("Multiply")));
			TestTrue(
				TEXT("the created Multiply is wired from DeltaSeconds"),
				DeltaSecondsPin->LinkedTo.ContainsByPredicate(
					[CreatedOperator](const UEdGraphPin* LinkedPin)
					{
						return LinkedPin
							&& LinkedPin->GetOwningNode() == CreatedOperator;
					}));
		}
	}

	FString OperatorStructureJson;
	bool bOperatorStructureReadOnly = false;
	FWorkspaceError OperatorStructureError;
	TestTrue(
		TEXT("the public structure reads back created operator semantics"),
		UE::UAI::DomainAdapters::Blueprint::Surfaces::TryExecuteContentGraph(
			TEXT("content_graph.structure"), ObjectPath, StructureParams,
			OperatorStructureJson,
			bOperatorStructureReadOnly,
			OperatorStructureError));
	const TSharedPtr<FJsonObject> OperatorStructurePayload = ParsePayload(
		this, OperatorStructureJson);
	const TArray<TSharedPtr<FJsonValue>>* OperatorItems = nullptr;
	bool bFoundEqualReadback = false;
	bool bFoundMultiplyReadback = false;
	bool bOrdinaryCallClaimsOperation = false;
	if (OperatorStructurePayload.IsValid()
		&& OperatorStructurePayload->TryGetArrayField(
			TEXT("items"), OperatorItems)
		&& OperatorItems)
	{
		for (const TSharedPtr<FJsonValue>& ItemValue : *OperatorItems)
		{
			const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
			const TSharedPtr<FJsonObject>* State = nullptr;
			FString Kind;
			if (!Item.IsValid()
				|| !Item->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("blueprint_node")
				|| !Item->TryGetObjectField(TEXT("value"), State)
				|| !State || !State->IsValid())
			{
				continue;
			}

			FString FunctionName;
			FString FunctionOwnerClass;
			FString OperationName;
			FString NodeClass;
			const bool bHasFunctionSemantics =
				(*State)->TryGetStringField(TEXT("function_name"), FunctionName)
				&& (*State)->TryGetStringField(
					TEXT("function_owner_class"), FunctionOwnerClass);
			const bool bHasOperatorSemantics =
				(*State)->TryGetStringField(TEXT("operation_name"), OperationName)
				&& (*State)->TryGetStringField(TEXT("node_class"), NodeClass);
			const bool bMathLibraryFunction = bHasFunctionSemantics
				&& FunctionOwnerClass
					== UKismetMathLibrary::StaticClass()->GetPathName();
			const bool bPromotableOperator = bHasOperatorSemantics
				&& NodeClass
					== UK2Node_PromotableOperator::StaticClass()->GetPathName();
			bFoundEqualReadback |= bMathLibraryFunction
				&& bPromotableOperator
				&& FunctionName == TEXT("EqualEqual_ObjectObject")
				&& OperationName == TEXT("EqualEqual");
			bFoundMultiplyReadback |= bMathLibraryFunction
				&& bPromotableOperator
				&& FunctionName == TEXT("Multiply_DoubleDouble")
				&& OperationName == TEXT("Multiply");
			bOrdinaryCallClaimsOperation |=
				bMathLibraryFunction && FunctionName == TEXT("FInterpTo")
				&& bHasOperatorSemantics;
		}
	}
	TestTrue(
		TEXT("EqualEqual readback keeps operator and resolved function semantics"),
		bFoundEqualReadback);
	TestTrue(
		TEXT("Multiply readback keeps operator and resolved function semantics"),
		bFoundMultiplyReadback);
	TestFalse(
		TEXT("ordinary CallFunction readback does not claim operator semantics"),
		bOrdinaryCallClaimsOperation);

	const TArray<TSharedPtr<FJsonValue>>* FunctionCandidates = nullptr;
	TSharedPtr<FJsonObject> FunctionPayload;
	FString FInterpCandidateId;
	FString FInterpFunctionOwnerClass;
	bool bFoundPrivateActionSignature = false;
	if (TestTrue(
			TEXT("FInterpTo candidates can be queried directly"),
			QueryCandidates(
				TEXT("FInterp To"), TEXT("native-t11-function-semantics"),
				FunctionCandidates, FunctionPayload)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *FunctionCandidates)
		{
			const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
			bFoundPrivateActionSignature |= Candidate.IsValid()
				&& Candidate->HasField(TEXT("native_action_signature"));
			FString FunctionName;
			FString FunctionOwnerClass;
			if (Candidate.IsValid()
				&& Candidate->TryGetStringField(
					TEXT("function_name"), FunctionName)
				&& FunctionName == TEXT("FInterpTo")
				&& Candidate->TryGetStringField(
					TEXT("function_owner_class"), FunctionOwnerClass)
				&& FunctionOwnerClass
					== UKismetMathLibrary::StaticClass()->GetPathName())
			{
				Candidate->TryGetStringField(TEXT("id"), FInterpCandidateId);
				FInterpFunctionOwnerClass = FunctionOwnerClass;
				break;
			}
		}
	}
	TestFalse(
		TEXT("candidate projection never exposes private action signatures"),
		bFoundPrivateActionSignature);
	TestFalse(
		TEXT("FInterpTo is identifiable without localized display-name inference"),
		FInterpCandidateId.IsEmpty());

	const TArray<TSharedPtr<FJsonValue>>* VariableCandidates = nullptr;
	TSharedPtr<FJsonObject> VariablePayload;
	bool bFoundVariableGet = false;
	bool bFoundVariableSet = false;
	FString VariableGetCandidateId;
	FString VariableGetCandidateName;
	FString VariableSetCandidateId;
	FString VariableSetCandidateName;
	if (TestTrue(
			TEXT("local variable candidates can be queried directly"),
			QueryCandidates(
				TEXT("T84SemanticValue"), TEXT("native-t11-variable-semantics"),
				VariableCandidates, VariablePayload)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *VariableCandidates)
		{
			const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
			FString VariableName;
			FString NodeType;
			if (Candidate.IsValid()
				&& Candidate->TryGetStringField(
					TEXT("variable_name"), VariableName)
				&& VariableName == TEXT("T84SemanticValue")
				&& Candidate->TryGetStringField(TEXT("node_type"), NodeType))
			{
				bFoundVariableGet |= NodeType == TEXT("get");
				bFoundVariableSet |= NodeType == TEXT("set");
				if (NodeType == TEXT("get"))
				{
					Candidate->TryGetStringField(
						TEXT("id"), VariableGetCandidateId);
					Candidate->TryGetStringField(
						TEXT("name"), VariableGetCandidateName);
				}
				else if (NodeType == TEXT("set"))
				{
					Candidate->TryGetStringField(
						TEXT("id"), VariableSetCandidateId);
					Candidate->TryGetStringField(
						TEXT("name"), VariableSetCandidateName);
				}
			}
		}
	}
	TestTrue(
		TEXT("the public candidate semantics distinguish variable get"),
		bFoundVariableGet);
	TestTrue(
		TEXT("the public candidate semantics distinguish variable set"),
		bFoundVariableSet);
	{
		auto BuildVariableProvider = [](const FString& CandidateId,
			const FString& CandidateName,
			const FString& StableNodeId,
			const bool bNormalizedShape)
		{
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("id"), CandidateId);
			if (!CandidateName.IsEmpty())
			{
				Candidate->SetStringField(TEXT("name"), CandidateName);
			}
			const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("operation"), TEXT("create"));
			Node->SetObjectField(TEXT("candidate"), Candidate);
			if (!bNormalizedShape)
			{
				Node->SetStringField(TEXT("node_type"), TEXT("native_action"));
			}
			const TSharedPtr<FJsonObject> Nodes = MakeShared<FJsonObject>();
			Nodes->SetObjectField(StableNodeId, Node);
			const TSharedPtr<FJsonObject> Graph = MakeShared<FJsonObject>();
			Graph->SetStringField(TEXT("operation"), TEXT("update"));
			const TSharedPtr<FJsonObject> GraphHeader = MakeShared<FJsonObject>();
			GraphHeader->SetStringField(
				TEXT("graph_kind"), TEXT("event_graph"));
			Graph->SetObjectField(TEXT("graph"), GraphHeader);
			Graph->SetObjectField(TEXT("nodes"), Nodes);
			const TSharedPtr<FJsonObject> Graphs = MakeShared<FJsonObject>();
			Graphs->SetObjectField(TEXT("event_graph:EventGraph"), Graph);
			const TSharedPtr<FJsonObject> Provider = MakeShared<FJsonObject>();
			Provider->SetObjectField(TEXT("graphs"), Graphs);
			return Provider;
		};

		const FString RebindBlueprintName = TEXT("BP_T11Rebind_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		UPackage* RebindPackage = CreatePackage(*(
			TEXT("/Temp/UAI/StrictV2/T11/") + RebindBlueprintName));
		UBlueprint* RebindBlueprint = RebindPackage
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				RebindPackage,
				*RebindBlueprintName,
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass())
			: nullptr;
		if (!TestNotNull(
				TEXT("the scratch-rebind regression has an isolated Blueprint"),
				RebindBlueprint))
		{
			return false;
		}
		RebindBlueprint->SetFlags(RF_Public | RF_Standalone);
		TStrongObjectPtr<UBlueprint> RebindBlueprintGuard(RebindBlueprint);
		TestTrue(
			TEXT("the isolated Blueprint has one local rebind variable"),
			FBlueprintEditorUtils::AddMemberVariable(
				RebindBlueprint,
				FName(TEXT("ScratchRebindValue")),
				FloatPinType));
		FKismetEditorUtilities::CompileBlueprint(RebindBlueprint);
		FBlueprintActionDatabase::Get().RefreshAssetActions(RebindBlueprint);
		ON_SCOPE_EXIT
		{
			FBlueprintActionDatabase::Get().ClearAssetActions(RebindBlueprint);
		};
		UEdGraph* RebindGraph = RebindBlueprint->UbergraphPages.Num() > 0
			? RebindBlueprint->UbergraphPages[0]
			: nullptr;
		if (!TestNotNull(
				TEXT("the isolated Blueprint has a normal compiled EventGraph"),
				RebindGraph))
		{
			return false;
		}
		FString RebindGraphId;
		FString RebindGraphName;
		FString RebindGraphError;
		TestTrue(
			TEXT("the isolated Blueprint exposes its public EventGraph identity"),
			UeAgentPaletteSearch::TryResolveBlueprintGraphIdentity(
				RebindBlueprint->GetPathName(),
				RebindGraph->GetName(),
				RebindGraphId,
				RebindGraphName,
				RebindGraphError));
		FUeAgentBlueprintNodeCommandRequest RebindRequest;
		RebindRequest.CommandId = TEXT("blueprint.node.candidates");
		RebindRequest.RequestId = TEXT("native-t11-scratch-rebind-variable");
		RebindRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_id\":\"%s\",\"limit\":20,\"query\":\"ScratchRebindValue\",\"target\":\"%s\"}"),
			*RebindGraphId,
			*RebindBlueprint->GetPathName());
		FUeAgentBlueprintNodeCommandResponse RebindResponse;
		FUeAgentBlueprintNodeCommandError RebindError;
		TestTrue(
			TEXT("the isolated Blueprint exposes its native local-variable actions"),
			Service.IsValid()
				&& Service->Execute(
					RebindRequest, RebindResponse, RebindError));
		const TSharedPtr<FJsonObject> RebindPayload = ParsePayload(
			this, RebindResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* RebindCandidates = nullptr;
		if (RebindPayload.IsValid())
		{
			RebindPayload->TryGetArrayField(
				TEXT("candidates"), RebindCandidates);
		}
		TArray<TSharedPtr<FJsonObject>> RebindSetCandidates;
		if (RebindCandidates)
		{
			for (const TSharedPtr<FJsonValue>& Value : *RebindCandidates)
			{
				const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
				FString VariableName;
				FString NodeType;
				if (Candidate.IsValid()
					&& Candidate->TryGetStringField(
						TEXT("variable_name"), VariableName)
					&& VariableName == TEXT("ScratchRebindValue")
					&& Candidate->TryGetStringField(
						TEXT("node_type"), NodeType)
					&& NodeType == TEXT("set"))
				{
					RebindSetCandidates.Add(Candidate);
				}
			}
		}
		if (!TestEqual(
				TEXT("the isolated local-variable set action is unique"),
				RebindSetCandidates.Num(),
				1))
		{
			return false;
		}
		const FString RebindCandidateId =
			RebindSetCandidates[0]->GetStringField(TEXT("id"));
		const FString RebindCandidateName =
			RebindSetCandidates[0]->GetStringField(TEXT("name"));

		const FString ScratchNodeId =
			TEXT("84a00000-0000-0000-0000-000000000001");
		TSharedPtr<FJsonObject> NormalizedProvider = BuildVariableProvider(
			RebindCandidateId, FString(), ScratchNodeId, true);
		FWorkspaceError NormalizeScratchError;
		TestTrue(
			TEXT("id-only local candidate normalizes on the real target"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					RebindBlueprint->GetPathName(),
					NormalizedProvider,
					NormalizeScratchError));
		UPackage* ScratchPackage = CreatePackage(*FString::Printf(
			TEXT("/Temp/UAI/StrictV2/T11/Scratch_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UBlueprint* ScratchBlueprint =
			UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(
				RebindBlueprint, ScratchPackage);
		if (TestNotNull(
				TEXT("the lifecycle creates a duplicated validation Blueprint"),
				ScratchBlueprint))
		{
			FBlueprintActionDatabase::Get().RefreshAssetActions(ScratchBlueprint);
			TestTrue(
				TEXT("the scratch lifecycle registers its action database entry"),
				FBlueprintActionDatabase::Get().GetAllActions().Contains(
					FObjectKey(ScratchBlueprint)));
			UEdGraph* ScratchGraph = ScratchBlueprint->UbergraphPages.Num() > 0
				? ScratchBlueprint->UbergraphPages[0]
				: nullptr;
			if (!TestNotNull(
					TEXT("the duplicated validation Blueprint keeps EventGraph"),
					ScratchGraph))
			{
				return false;
			}
			UeAgentPaletteSearch::FSearchParams ScratchResolveParams;
			ScratchResolveParams.CatalogType = TEXT("blueprint_node");
			ScratchResolveParams.AssetPath = ScratchBlueprint->GetPathName();
			ScratchResolveParams.GraphName = ScratchGraph->GetName();
			ScratchResolveParams.bContextSensitive = true;
			ScratchResolveParams.RequiredAuthoringSelectors.Add(
				TEXT("variable_name"), TEXT("ScratchRebindValue"));
			ScratchResolveParams.RequiredAuthoringSelectors.Add(
				TEXT("node_type"), TEXT("set"));
			UeAgentPaletteSearch::FCandidate ScratchResolvedCandidate;
			FString ScratchResolveError;
			TestTrue(
				TEXT("the refreshed scratch database enumerates the frozen local candidate"),
				UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
					ScratchResolveParams,
					RebindCandidateName,
					RebindCandidateId,
					ScratchResolvedCandidate,
					ScratchResolveError));
			FBlueprintActionDatabase::Get().ClearAssetActions(ScratchBlueprint);
			TestFalse(
				TEXT("the explicit scratch action registration can be cleaned"),
				FBlueprintActionDatabase::Get().GetAllActions().Contains(
					FObjectKey(ScratchBlueprint)));
			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				PreparedMutation;
			UeAgentBlueprintAuthoring::FMutationCounters ExpectedCounters;
			FString PrepareError;
			const bool bPrepared =
				UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
					*ScratchBlueprint,
					NormalizedProvider,
					PreparedMutation,
					ExpectedCounters,
					PrepareError);
			TestTrue(
				TEXT("the normalized local candidate rebinds on the scratch Blueprint"),
				bPrepared);
			if (!bPrepared)
			{
				AddError(TEXT("scratch Prepare failed: ") + PrepareError);
			}
			TestFalse(
				TEXT("scratch Prepare clears its bounded action registration"),
				FBlueprintActionDatabase::Get().GetAllActions().Contains(
					FObjectKey(ScratchBlueprint)));

			const FString MissingNodeId =
				TEXT("84a00000-0000-0000-0000-000000000002");
			TSharedPtr<FJsonObject> MissingProvider = BuildVariableProvider(
				TEXT("candidate:00000000000000000000000000000000"),
				TEXT("Definitely Missing Candidate"),
				MissingNodeId,
				false);
			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				MissingMutation;
			UeAgentBlueprintAuthoring::FMutationCounters MissingCounters;
			FString MissingError;
			TestFalse(
				TEXT("an unresolved scratch candidate still fails closed"),
				UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
					*ScratchBlueprint,
					MissingProvider,
					MissingMutation,
					MissingCounters,
					MissingError));
			TestTrue(
				TEXT("scratch candidate failure identifies its public node and selector"),
				MissingError.StartsWith(
					TEXT("blueprint_action_candidate_not_found:node=")
					+ MissingNodeId
					+ TEXT(",selector=id(candidate:")));
		}
	}
	if (!VariableGetCandidateId.IsEmpty()
		&& !VariableSetCandidateId.IsEmpty())
	{
		UeAgentPaletteSearch::FSearchParams VariablePerformParams;
		VariablePerformParams.CatalogType = TEXT("blueprint_node");
		VariablePerformParams.AssetPath = ObjectPath;
		VariablePerformParams.GraphName = GraphId;
		VariablePerformParams.bContextSensitive = true;
		auto PerformVariableCandidate = [
			this,
			&VariablePerformParams](
				const FString& CandidateName,
				const FString& CandidateId,
				const FVector2f Position) -> UEdGraphNode*
		{
			UEdGraphNode* CreatedNode = nullptr;
			UeAgentPaletteSearch::FCandidate PerformedCandidate;
			FString PerformError;
			if (!TestTrue(
					TEXT("the native variable candidate performs through its menu action"),
					UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
						VariablePerformParams,
						CandidateName,
						CandidateId,
						Position,
						CreatedNode,
						PerformedCandidate,
						PerformError)))
			{
				AddError(PerformError);
				return nullptr;
			}
			return CreatedNode;
		};
		const UK2Node_VariableGet* VariableGetNode =
			Cast<UK2Node_VariableGet>(PerformVariableCandidate(
				VariableGetCandidateName,
				VariableGetCandidateId,
				FVector2f(900.0f, 160.0f)));
		const UK2Node_VariableSet* VariableSetNode =
			Cast<UK2Node_VariableSet>(PerformVariableCandidate(
				VariableSetCandidateName,
				VariableSetCandidateId,
				FVector2f(1160.0f, 160.0f)));
		TestNotNull(
			TEXT("the native get candidate creates a variable-get node"),
			VariableGetNode);
		TestNotNull(
			TEXT("the native set candidate creates a variable-set node"),
			VariableSetNode);
		if (VariableGetNode && VariableSetNode)
		{
			TestEqual(
				TEXT("the native get candidate preserves the variable identity"),
				VariableGetNode->GetVarName(),
				FName(TEXT("T84SemanticValue")));
			TestEqual(
				TEXT("the native set candidate preserves the variable identity"),
				VariableSetNode->GetVarName(),
				FName(TEXT("T84SemanticValue")));
		}
	}

	if (!FInterpCandidateId.IsEmpty())
	{
		auto BuildSemanticAssertion = [
			&FInterpCandidateId,
			&FInterpFunctionOwnerClass](const FString& FunctionName)
		{
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("id"), FInterpCandidateId);
			Candidate->SetStringField(TEXT("function_name"), FunctionName);
			Candidate->SetStringField(
				TEXT("function_owner_class"), FInterpFunctionOwnerClass);
			const TSharedPtr<FJsonObject> NodeSpec = MakeShared<FJsonObject>();
			NodeSpec->SetObjectField(TEXT("candidate"), Candidate);
			const TSharedPtr<FJsonObject> Nodes = MakeShared<FJsonObject>();
			Nodes->SetObjectField(TEXT("semantic_assertion"), NodeSpec);
			const TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
			GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
			const TSharedPtr<FJsonObject> Graphs = MakeShared<FJsonObject>();
			Graphs->SetObjectField(TEXT("event_graph:EventGraph"), GraphSpec);
			const TSharedPtr<FJsonObject> ProviderExtension =
				MakeShared<FJsonObject>();
			ProviderExtension->SetObjectField(TEXT("graphs"), Graphs);
			return ProviderExtension;
		};
		FWorkspaceError ValidAssertionError;
		TestTrue(
			TEXT("matching optional candidate assertions normalize successfully"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					ObjectPath,
					BuildSemanticAssertion(TEXT("FInterpTo")),
					ValidAssertionError));
		FWorkspaceError AssertionError;
		TestFalse(
			TEXT("a conflicting optional candidate assertion fails before write"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					ObjectPath,
					BuildSemanticAssertion(TEXT("DefinitelyNotFInterpTo")),
					AssertionError));
		TestEqual(
			TEXT("a conflicting optional assertion has a typed failure"),
			AssertionError.GetCode(),
			FString(TEXT(
				"blueprint_surface_node_candidate_metadata_mismatch")));
	}
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
