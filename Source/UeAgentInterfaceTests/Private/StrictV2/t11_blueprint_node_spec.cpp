#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintEditorSettings.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
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
				OutError.PublicMessage = TEXT("typed fixture failure");
				return false;
			}
			OutResponse.PayloadJson = TEXT("{\"names\":[]}");
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
			TEXT("\"graph_name\":\"EventGraph\"}}"),
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11DomainProviderActivationTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.DomainProviderActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11DomainProviderActivationTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11ExactCandidateDispatchTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.ExactCandidateDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11ExactCandidateDispatchTest::RunTest(
	const FString& Parameters)
{
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
	const TSharedPtr<FJsonObject> Payload = ParsePayload(this, Result.GetPayloadJson());
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	TestTrue(
		TEXT("the fixture exposes only the names result"),
		Payload.IsValid()
			&& Payload->Values.Num() == 1
			&& Payload->TryGetArrayField(TEXT("names"), Names));
	TestEqual(TEXT("the exact service is called once"), Service->Calls, 1);
	TestEqual(
		TEXT("the exact command identity is unchanged"),
		Service->LastRequest.CommandId,
		FString(TEXT("blueprint.node.candidates")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11ProductionCandidateTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.ProductionCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11ProductionCandidateTest::RunTest(
	const FString& Parameters)
{
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
	FString ResolvedGraphName;
	FString GraphError;
	if (!TestTrue(
		TEXT("the candidate fixture resolves its exact graph identity"),
		UeAgentPaletteSearch::TryResolveBlueprintGraphIdentity(
			ObjectPath, TEXT("EventGraph"), GraphId, ResolvedGraphName,
			GraphError)))
	{
		AddError(GraphError);
		return false;
	}
	const FString GraphName = TEXT("EventGraph");
	TestEqual(
		TEXT("the resolved candidate graph keeps its readable name"),
		ResolvedGraphName,
		GraphName);
	TSharedPtr<IUeAgentBlueprintNodeCommandService> Service;
	TestTrue(
		TEXT("the production candidate service composes"),
		TryCreateProductionBlueprintNodeCommandService(Service, GraphError));
	{
		UeAgentPaletteSearch::FSearchParams UnscopedMultiplyParams;
		UnscopedMultiplyParams.CatalogType = TEXT("blueprint_node");
		UnscopedMultiplyParams.AssetPath = ObjectPath;
		UnscopedMultiplyParams.GraphName = GraphId;
		UnscopedMultiplyParams.SearchText = TEXT("Multiply");
		UnscopedMultiplyParams.bContextSensitive = true;
		UeAgentPaletteSearch::FSearchResult UnscopedMultiplyResult;
		FString UnscopedMultiplyError;
		TestTrue(
			TEXT("unscoped Multiply discovery succeeds"),
			UeAgentPaletteSearch::ExecuteSearch(
				UnscopedMultiplyParams,
				UnscopedMultiplyResult,
				UnscopedMultiplyError));
		int32 ExactMultiplyOperationCount = 0;
		bool bUnscopedMultiplyReturnedPower = false;
		for (const UeAgentPaletteSearch::FCandidate& Candidate
			: UnscopedMultiplyResult.MatchingCandidates)
		{
			ExactMultiplyOperationCount +=
				Candidate.AuthoringSelectors.FindRef(TEXT("operation_name"))
					== TEXT("Multiply") ? 1 : 0;
			bUnscopedMultiplyReturnedPower |=
				Candidate.AuthoringSelectors.FindRef(TEXT("function_name"))
					== TEXT("MultiplyMultiply_FloatFloat");
		}
		if (ExactMultiplyOperationCount != 1
			|| bUnscopedMultiplyReturnedPower)
		{
			TArray<FString> Diagnostics;
			for (const UeAgentPaletteSearch::FCandidate& Candidate
				: UnscopedMultiplyResult.MatchingCandidates)
			{
				Diagnostics.Add(
					UeAgentPaletteSearch::GetBlueprintActionPublicName(Candidate)
					+ TEXT(" {category=") + Candidate.CategoryPath
					+ TEXT(", operation=")
					+ Candidate.AuthoringSelectors.FindRef(TEXT("operation_name"))
					+ TEXT(", function=")
					+ Candidate.AuthoringSelectors.FindRef(TEXT("function_name"))
					+ TEXT("}"));
			}
			AddError(TEXT("unscoped Multiply diagnostics: ")
				+ FString::Join(Diagnostics, TEXT(", ")));
		}
		TestEqual(
			TEXT("unscoped Multiply has one exact readable operation"),
			ExactMultiplyOperationCount,
			1);
		TestFalse(
			TEXT("unscoped Multiply does not expose Power under its internal name"),
			bUnscopedMultiplyReturnedPower);
	}
	{
		UeAgentPaletteSearch::FSearchParams SelectParams;
		SelectParams.CatalogType = TEXT("blueprint_node");
		SelectParams.AssetPath = ObjectPath;
		SelectParams.GraphName = GraphId;
		SelectParams.SearchText = TEXT("Select");
		SelectParams.bContextSensitive = true;
		UeAgentPaletteSearch::FSearchResult SelectResult;
		FString SelectError;
		TestTrue(
			TEXT("unscoped Select discovery succeeds"),
			UeAgentPaletteSearch::ExecuteSearch(
				SelectParams, SelectResult, SelectError));
		TArray<FString> SelectDiagnostics;
		for (const UeAgentPaletteSearch::FCandidate& Candidate
			: SelectResult.MatchingCandidates)
		{
			SelectDiagnostics.Add(
				UeAgentPaletteSearch::GetBlueprintActionPublicName(Candidate)
				+ TEXT(" {type=") + Candidate.Type
				+ TEXT(", function=")
				+ Candidate.AuthoringSelectors.FindRef(TEXT("function_name"))
				+ TEXT(", operation=")
				+ Candidate.AuthoringSelectors.FindRef(TEXT("operation_name"))
				+ TEXT("}"));
		}
		AddInfo(TEXT("Select candidates: ")
			+ FString::Join(SelectDiagnostics, TEXT(", ")));
	}
	{
		UeAgentPaletteSearch::FSearchParams RawInterpParams;
		RawInterpParams.CatalogType = TEXT("blueprint_node");
		RawInterpParams.AssetPath = ObjectPath;
		RawInterpParams.GraphName = GraphId;
		RawInterpParams.bContextSensitive = true;
		UeAgentPaletteSearch::FSearchResult RawInterpResult;
		FString RawInterpError;
		TestTrue(
			TEXT("raw Blueprint candidate collection succeeds"),
			UeAgentPaletteSearch::ExecuteSearch(
				RawInterpParams, RawInterpResult, RawInterpError));
		TArray<FString> RawInterpDiagnostics;
		for (const UeAgentPaletteSearch::FCandidate& Candidate
			: RawInterpResult.MatchingCandidates)
		{
			if (Candidate.AuthoringSelectors.FindRef(TEXT("function_name"))
				== TEXT("FInterpTo"))
			{
				RawInterpDiagnostics.Add(
					UeAgentPaletteSearch::GetBlueprintActionPublicName(Candidate));
			}
		}
		if (RawInterpDiagnostics.IsEmpty())
		{
			AddError(TEXT("raw FInterp To candidate is absent"));
		}
		else
		{
			AddInfo(TEXT("raw FInterp To names: ")
				+ FString::Join(RawInterpDiagnostics, TEXT(", ")));
		}

		UeAgentPaletteSearch::FSearchParams InterpParams;
		InterpParams.CatalogType = TEXT("blueprint_node");
		InterpParams.AssetPath = ObjectPath;
		InterpParams.GraphName = GraphId;
		InterpParams.SearchText = TEXT("FInterp To");
		InterpParams.bContextSensitive = true;
		UeAgentPaletteSearch::FSearchResult InterpResult;
		FString InterpError;
		TestTrue(
			TEXT("unscoped FInterp To discovery succeeds"),
			UeAgentPaletteSearch::ExecuteSearch(
				InterpParams, InterpResult, InterpError));
		bool bFoundFInterpTo = false;
		TArray<FString> Diagnostics;
		for (const UeAgentPaletteSearch::FCandidate& Candidate
			: InterpResult.MatchingCandidates)
		{
			bFoundFInterpTo |= Candidate.AuthoringSelectors.FindRef(
				TEXT("function_name")) == TEXT("FInterpTo");
			Diagnostics.Add(
				UeAgentPaletteSearch::GetBlueprintActionPublicName(Candidate)
				+ TEXT(" {function=")
				+ Candidate.AuthoringSelectors.FindRef(TEXT("function_name"))
				+ TEXT("}"));
		}
		if (!bFoundFInterpTo)
		{
			AddError(TEXT("FInterp To diagnostics: ")
				+ FString::Join(Diagnostics, TEXT(", ")));
		}
		TestTrue(
			TEXT("FInterp To resolves by its readable function name"),
			bFoundFInterpTo);
	}
	auto QueryCandidates = [&Service, &GraphId, &GraphName, &ObjectPath, this](
		const FString& Query,
		const FString& RequestId,
		const TArray<TSharedPtr<FJsonValue>>*& OutNames,
		TSharedPtr<FJsonObject>& OutPayload) -> bool
	{
		FUeAgentBlueprintNodeCommandRequest CandidateRequest;
		CandidateRequest.CommandId = TEXT("blueprint.node.candidates");
		CandidateRequest.RequestId = RequestId;
		CandidateRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_name\":\"%s\",\"limit\":3,\"query\":\"%s\",\"target\":\"%s\"}"),
			*GraphName,
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
				*CandidateError.PublicMessage));
			return false;
		}
		OutPayload = ParsePayload(this, CandidateResponse.PayloadJson);
		return OutPayload.IsValid()
			&& OutPayload->TryGetArrayField(TEXT("names"), OutNames)
			&& OutNames;
	};
	auto FindSemanticNodeOccurrence = [](const UEdGraph& Graph,
		const UEdGraphNode& Target, const FString& SemanticName) -> int32
	{
		int32 Occurrence = 0;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!Node || !UeAgentBlueprintAuthoring::NodeSemanticName(*Node).Equals(
					SemanticName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (Node == &Target)
			{
				return Occurrence;
			}
			++Occurrence;
		}
		return INDEX_NONE;
	};
	{
		const TArray<TSharedPtr<FJsonValue>>* SelectNames = nullptr;
		TSharedPtr<FJsonObject> SelectPayload;
		if (TestTrue(
				TEXT("Select discovery projects a bounded names-only page"),
				QueryCandidates(
					TEXT("Select"), TEXT("native-t11-select"),
					SelectNames, SelectPayload))
			&& SelectNames)
		{
			TestTrue(TEXT("Select discovery remains bounded"),
				SelectNames->Num() <= 3);
			TestTrue(TEXT("Select discovery keeps the generic node name"),
				SelectNames->ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->Type == EJson::String
							&& Value->AsString() == TEXT("Select");
					}));
		}
	}
	{
		const TArray<TSharedPtr<FJsonValue>>* SelectFloatNames = nullptr;
		TSharedPtr<FJsonObject> SelectFloatPayload;
		if (TestTrue(
				TEXT("Select Float is discoverable by its stable semantic name"),
				QueryCandidates(
					TEXT("Select Float"), TEXT("native-t11-select-float"),
					SelectFloatNames, SelectFloatPayload))
			&& SelectFloatNames)
		{
			TestTrue(TEXT("Select Float discovery returns the exact function"),
				SelectFloatNames->ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->Type == EJson::String
							&& Value->AsString() == TEXT("Select Float");
					}));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GenericCandidates = nullptr;
	TSharedPtr<FJsonObject> GenericPayload;
	FString GenericCandidateName;
	int32 GenericCandidateCount = 0;
	UeAgentPaletteSearch::FSearchParams GenericPerformParams;
	GenericPerformParams.CatalogType = TEXT("blueprint_node");
	GenericPerformParams.AssetPath = ObjectPath;
	GenericPerformParams.GraphName = GraphId;
	GenericPerformParams.bContextSensitive = true;
	if (TestTrue(
			TEXT("the unbound component function can be queried by readable name"),
			QueryCandidates(
				TEXT("Set Relative Rotation"),
				TEXT("native-t11-unbound-component-function"),
				GenericCandidates,
				GenericPayload)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *GenericCandidates)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				continue;
			}
			UeAgentPaletteSearch::FCandidate ResolvedCandidate;
			FString ResolveError;
			if (UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
					GenericPerformParams, Value->AsString(), FString(),
					ResolvedCandidate, ResolveError)
				&& ResolvedCandidate.AuthoringSelectors.FindRef(
						TEXT("function_name")) == TEXT("K2_SetRelativeRotation"))
			{
				++GenericCandidateCount;
				GenericCandidateName = Value->AsString();
			}
		}
	}
	TestEqual(
		TEXT("the generic component function action is unique"),
		GenericCandidateCount,
		1);
	if (!GenericCandidateName.IsEmpty())
	{
		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}
		UEdGraphNode* GenericNode = nullptr;
		UeAgentPaletteSearch::FCandidate PerformedGenericCandidate;
		FString GenericPerformError;
		TestTrue(
			TEXT("the generic component function action creates without an implicit component"),
			UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
				GenericPerformParams,
				GenericCandidateName,
				FString(),
				FVector2f(900.0f, -320.0f),
				GenericNode,
				PerformedGenericCandidate,
				GenericPerformError));
		TArray<UEdGraphNode*> GenericCreatedNodes;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				GenericCreatedNodes.Add(Node);
			}
		}
		TestEqual(
			TEXT("the generic action creates only its declared node"),
			GenericCreatedNodes.Num(),
			1);
		UK2Node_CallFunction* GenericCall =
			Cast<UK2Node_CallFunction>(GenericNode);
		TestNotNull(
			TEXT("the returned node is the native function call"),
			GenericCall);
		if (GenericCall)
		{
			const UFunction* TargetFunction = GenericCall->GetTargetFunction();
			TestTrue(
				TEXT("the returned node keeps K2_SetRelativeRotation semantics"),
				TargetFunction
					&& TargetFunction->GetFName()
						== TEXT("K2_SetRelativeRotation"));
			const UEdGraphPin* SelfPin = GenericCall->FindPin(
				UEdGraphSchema_K2::PN_Self, EGPD_Input);
			TestTrue(
				TEXT("the generic call self pin remains available for explicit HJSON wiring"),
				SelfPin && SelfPin->LinkedTo.IsEmpty());
		}
	}

	UEdGraphPin* OtherActorPin = ActorOverlapEvent->FindPin(
		TEXT("OtherActor"), EGPD_Output);
	TestNotNull(
		TEXT("the actor-overlap fixture exposes its native OtherActor pin"),
		OtherActorPin);
	if (OtherActorPin)
	{
		const FString ActorOverlapSemanticName =
			UeAgentBlueprintAuthoring::NodeSemanticName(*ActorOverlapEvent);
		const int32 ActorOverlapOccurrence = FindSemanticNodeOccurrence(
			*EventGraph, *ActorOverlapEvent, ActorOverlapSemanticName);
		FUeAgentBlueprintNodeCommandRequest EqualRequest;
		EqualRequest.CommandId = TEXT("blueprint.node.candidates");
		EqualRequest.RequestId = TEXT("native-t11-equal-semantics");
		EqualRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_name\":\"%s\",\"limit\":3,\"node_name\":\"%s\",\"node_occurrence\":%d,\"pin_name\":\"OtherActor\",\"query\":\"Equal\",\"target\":\"%s\"}"),
			*GraphName,
			*ActorOverlapSemanticName,
			ActorOverlapOccurrence,
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse EqualResponse;
		FUeAgentBlueprintNodeCommandError EqualError;
		const bool bEqualQuerySucceeded = Service.IsValid()
			&& Service->Execute(EqualRequest, EqualResponse, EqualError);
		TestTrue(
			TEXT("Equal candidates use the real OtherActor pin context"),
			bEqualQuerySucceeded);
		if (!bEqualQuerySucceeded)
		{
			AddError(EqualError.Code + TEXT(":") + EqualError.PublicMessage);
		}
		const TSharedPtr<FJsonObject> EqualPayload = ParsePayload(
			this, EqualResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* EqualNames = nullptr;
		if (EqualPayload.IsValid())
		{
			EqualPayload->TryGetArrayField(TEXT("names"), EqualNames);
		}
		FString EqualCandidateName;
		UeAgentPaletteSearch::FSearchParams EqualPerformParams;
		EqualPerformParams.CatalogType = TEXT("blueprint_node");
		EqualPerformParams.AssetPath = ObjectPath;
		EqualPerformParams.GraphName = GraphId;
		EqualPerformParams.NodeGuid = ActorOverlapEvent->NodeGuid.ToString(
			EGuidFormats::DigitsWithHyphensLower);
		EqualPerformParams.PinName = TEXT("OtherActor");
		EqualPerformParams.bContextSensitive = true;
		if (EqualNames)
		{
			for (const TSharedPtr<FJsonValue>& Value : *EqualNames)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					continue;
				}
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString ResolveError;
				if (UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
						EqualPerformParams, Value->AsString(), FString(),
						ResolvedCandidate, ResolveError)
					&& ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("operation_name")) == TEXT("EqualEqual"))
				{
					EqualCandidateName = Value->AsString();
					break;
				}
			}
		}
		TestFalse(
			TEXT("EqualEqual has a native contextual name"),
			EqualCandidateName.IsEmpty());
		if (!EqualCandidateName.IsEmpty())
		{
			UEdGraphNode* CreatedNode = nullptr;
			UeAgentPaletteSearch::FCandidate PerformedCandidate;
			FString PerformError;
			TestTrue(
				TEXT("the native EqualEqual candidate creates from OtherActor"),
				UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
					EqualPerformParams,
					EqualCandidateName,
					FString(),
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
	bool bMultiplyQueryReturnedPower = false;
	FString TickSemanticNameForCandidate;
	if (DeltaSecondsPin)
	{
		const FString TickSemanticName =
			UeAgentBlueprintAuthoring::NodeSemanticName(*EventNode);
		const int32 TickOccurrence = FindSemanticNodeOccurrence(
			*EventGraph, *EventNode, TickSemanticName);
		TickSemanticNameForCandidate = TickSemanticName;
		FUeAgentBlueprintNodeCommandRequest MultiplyRequest;
		MultiplyRequest.CommandId = TEXT("blueprint.node.candidates");
		MultiplyRequest.RequestId = TEXT("native-t11-multiply-semantics");
		MultiplyRequest.CanonicalParamsJson = FString::Printf(
			TEXT("{\"graph_name\":\"%s\",\"limit\":3,\"node_name\":\"%s\",\"node_occurrence\":%d,\"pin_name\":\"DeltaSeconds\",\"query\":\"Multiply\",\"target\":\"%s\"}"),
			*GraphName,
			*TickSemanticName,
			TickOccurrence,
			*ObjectPath);
		FUeAgentBlueprintNodeCommandResponse MultiplyResponse;
		FUeAgentBlueprintNodeCommandError MultiplyError;
		const bool bMultiplyQuerySucceeded = Service.IsValid()
			&& Service->Execute(
				MultiplyRequest, MultiplyResponse, MultiplyError);
		TestTrue(
			TEXT("Multiply candidates use the real DeltaSeconds pin context"),
			bMultiplyQuerySucceeded);
		if (!bMultiplyQuerySucceeded)
		{
			AddError(MultiplyError.Code + TEXT(":") + MultiplyError.PublicMessage);
		}
		const TSharedPtr<FJsonObject> MultiplyPayload = ParsePayload(
			this, MultiplyResponse.PayloadJson);
		const TArray<TSharedPtr<FJsonValue>>* MultiplyNames = nullptr;
		if (MultiplyPayload.IsValid())
		{
			MultiplyPayload->TryGetArrayField(
				TEXT("names"), MultiplyNames);
		}
		if (MultiplyNames)
		{
			UeAgentPaletteSearch::FSearchParams ResolveParams;
			ResolveParams.CatalogType = TEXT("blueprint_node");
			ResolveParams.AssetPath = ObjectPath;
			ResolveParams.GraphName = GraphId;
			ResolveParams.NodeGuid = EventNode->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			ResolveParams.PinName = TEXT("DeltaSeconds");
			ResolveParams.bContextSensitive = true;
			for (const TSharedPtr<FJsonValue>& Value : *MultiplyNames)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					continue;
				}
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString ResolveError;
				if (UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
						ResolveParams, Value->AsString(), FString(),
						ResolvedCandidate, ResolveError))
				{
					bMultiplyQueryReturnedPower =
						bMultiplyQueryReturnedPower
						|| ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("function_name"))
							== TEXT("MultiplyMultiply_FloatFloat");
					if (ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("operation_name")) == TEXT("Multiply"))
					{
						MultiplyCandidateName = Value->AsString();
					}
				}
			}
		}
	}
	TestFalse(
		TEXT("Multiply discovery does not surface the Power function through its internal name"),
		bMultiplyQueryReturnedPower);
	TestFalse(
		TEXT("Multiply has a stable promotable-operator name"),
		MultiplyCandidateName.IsEmpty());

	if (DeltaSecondsPin && !MultiplyCandidateName.IsEmpty())
	{
		auto BuildMultiplyAssertion = [
			&MultiplyCandidateName, &TickSemanticNameForCandidate,
			EventNode]()
		{
			const TSharedPtr<FJsonObject> Candidate =
				MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), MultiplyCandidateName);
			const TSharedPtr<FJsonObject> CandidateContext =
				MakeShared<FJsonObject>();
			CandidateContext->SetStringField(
				TEXT("node"),
				EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			CandidateContext->SetStringField(
				TEXT("alias"), TickSemanticNameForCandidate);
			CandidateContext->SetStringField(
				TEXT("pin"), TEXT("DeltaSeconds"));
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
			TEXT("matching short select normalizes successfully"),
			UE::UAI::DomainAdapters::Blueprint::Surfaces::
				TryNormalizeBlueprintCandidateBindings(
					ObjectPath,
					BuildMultiplyAssertion(),
					ValidMultiplyAssertionError));

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
				FString(),
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

	const TArray<TSharedPtr<FJsonValue>>* VariableCandidates = nullptr;
	TSharedPtr<FJsonObject> VariablePayload;
	bool bFoundVariableGet = false;
	bool bFoundVariableSet = false;
	FString VariableGetCandidateName;
	FString VariableSetCandidateName;
	UeAgentPaletteSearch::FSearchParams VariableResolveParams;
	VariableResolveParams.CatalogType = TEXT("blueprint_node");
	VariableResolveParams.AssetPath = ObjectPath;
	VariableResolveParams.GraphName = GraphId;
	VariableResolveParams.bContextSensitive = true;
	if (TestTrue(
			TEXT("local variable candidates can be queried directly"),
			QueryCandidates(
				TEXT("T84SemanticValue"), TEXT("native-t11-variable-semantics"),
				VariableCandidates, VariablePayload)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *VariableCandidates)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString ResolveError;
				if (!UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
						VariableResolveParams, Value->AsString(), FString(),
						ResolvedCandidate, ResolveError)
					|| ResolvedCandidate.AuthoringSelectors.FindRef(
						TEXT("variable_name")) != TEXT("T84SemanticValue"))
				{
					continue;
				}
				const FString NodeType =
					ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("node_type"));
				if (NodeType == TEXT("get"))
				{
					bFoundVariableGet = true;
					VariableGetCandidateName = Value->AsString();
				}
				else if (NodeType == TEXT("set"))
				{
					bFoundVariableSet = true;
					VariableSetCandidateName = Value->AsString();
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
	if ((!bFoundVariableGet || !bFoundVariableSet) && VariableCandidates)
	{
		TArray<FString> Diagnostics;
		for (const TSharedPtr<FJsonValue>& Value : *VariableCandidates)
		{
			if (!Value.IsValid() || Value->Type != EJson::String
				|| Diagnostics.Num() == 5) continue;
			UeAgentPaletteSearch::FCandidate ResolvedCandidate;
			FString ResolveError;
			const bool bResolved =
				UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
					VariableResolveParams, Value->AsString(), FString(),
					ResolvedCandidate, ResolveError);
			Diagnostics.Add(Value->AsString()
				+ TEXT(" {variable=")
				+ ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("variable_name"))
				+ TEXT(", type=")
				+ ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("node_type"))
				+ (bResolved ? TEXT("}") : TEXT(", resolve=") + ResolveError + TEXT("}")));
		}
		AddError(TEXT("variable discovery diagnostics: ")
			+ FString::Join(Diagnostics, TEXT(", ")));
	}
	{
		auto BuildVariableProvider = [](const FString& CandidateName,
			const FString& StableNodeId,
			const bool bNormalizedShape)
		{
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), CandidateName);
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
			TEXT("{\"graph_name\":\"%s\",\"limit\":3,\"query\":\"ScratchRebindValue\",\"target\":\"%s\"}"),
			*RebindGraphName,
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
		const TArray<TSharedPtr<FJsonValue>>* RebindNames = nullptr;
		if (RebindPayload.IsValid())
		{
			RebindPayload->TryGetArrayField(
				TEXT("names"), RebindNames);
		}
		TArray<FString> RebindSetNames;
		UeAgentPaletteSearch::FSearchParams RebindResolveParams;
		RebindResolveParams.CatalogType = TEXT("blueprint_node");
		RebindResolveParams.AssetPath = RebindBlueprint->GetPathName();
		RebindResolveParams.GraphName = RebindGraphId;
		RebindResolveParams.bContextSensitive = true;
		if (RebindNames)
		{
			for (const TSharedPtr<FJsonValue>& Value : *RebindNames)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					continue;
				}
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString ResolveError;
				if (UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
						RebindResolveParams, Value->AsString(), FString(),
						ResolvedCandidate, ResolveError)
					&& ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("variable_name")) == TEXT("ScratchRebindValue")
					&& ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("node_type")) == TEXT("set"))
				{
					RebindSetNames.Add(Value->AsString());
				}
			}
		}
		if (RebindSetNames.IsEmpty() && RebindNames)
		{
			TArray<FString> ReturnedNames;
			for (const TSharedPtr<FJsonValue>& Value : *RebindNames)
			{
				if (Value.IsValid() && Value->Type == EJson::String
					&& ReturnedNames.Num() < 5)
				{
					ReturnedNames.Add(Value->AsString());
				}
			}
			AddError(TEXT("rebind readable names: ")
				+ FString::Join(ReturnedNames, TEXT(", ")));
		}
		if (!TestEqual(
				TEXT("the isolated local-variable set action is unique"),
				RebindSetNames.Num(),
				1))
		{
			return false;
		}
		const FString RebindCandidateName = RebindSetNames[0];

		const FString ScratchNodeId =
			TEXT("native_action:scratch_rebind_set");
		TSharedPtr<FJsonObject> NormalizedProvider = BuildVariableProvider(
			RebindCandidateName, ScratchNodeId, true);
		FWorkspaceError NormalizeScratchError;
		TestTrue(
			TEXT("name-only local candidate normalizes on the real target"),
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
					FString(),
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
				TEXT("native_action:missing_candidate");
			TSharedPtr<FJsonObject> MissingProvider = BuildVariableProvider(
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
					+ MissingNodeId)
					&& MissingError.Contains(TEXT("selector=name"))
					&& !MissingError.Contains(TEXT("candidate_id")));
		}
	}
	if (!VariableGetCandidateName.IsEmpty()
		&& !VariableSetCandidateName.IsEmpty())
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
				const FString& UnusedLegacySelector,
				const FVector2f Position) -> UEdGraphNode*
		{
			(void)UnusedLegacySelector;
			UEdGraphNode* CreatedNode = nullptr;
			UeAgentPaletteSearch::FCandidate PerformedCandidate;
			FString PerformError;
			if (!TestTrue(
					TEXT("the native variable candidate performs through its menu action"),
					UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
						VariablePerformParams,
						CandidateName,
						FString(),
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
				FString(),
				FVector2f(900.0f, 160.0f)));
		const UK2Node_VariableSet* VariableSetNode =
			Cast<UK2Node_VariableSet>(PerformVariableCandidate(
				VariableSetCandidateName,
				FString(),
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

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T11FailureBeforeWriteTest,
	"UAI.StrictV2.T11.BLUEPRINT.NODE.FailureBeforeWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T11FailureBeforeWriteTest::RunTest(
	const FString& Parameters)
{
	FUeAgentBlueprintNodeCommandRequest Request;
	Request.CommandId = TEXT("blueprint.node.candidates");
	Request.RequestId = TEXT("native-t11-missing-target");
	Request.CanonicalParamsJson = TEXT(
		"{\"graph_name\":\"graph:missing\","
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
}
