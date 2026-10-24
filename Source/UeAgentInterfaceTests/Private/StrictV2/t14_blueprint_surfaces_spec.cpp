#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "UObject/Interface.h"

#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T14BlueprintSurfacesTest,
	"UAI.StrictV2.T14.BLUEPRINT.SURFACES",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T14BlueprintSurfacesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::Blueprint::Surfaces;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("the Blueprint Surfaces provider is created"),
		TryCreateBlueprintSurfaceDomainProvider(Provider, WorkspaceError))
		|| !Provider.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	const FImportInitializationDomainRequest SemanticSchemaRoot(
		TEXT("content_graph.import_workspace"), GetSemanticWorkspaceSchemaId(),
		TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
		TEXT("/Script/Engine.Blueprint"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestTrue(TEXT("the semantic workspace schema resolves the Blueprint root"),
		Provider->Matches(SemanticSchemaRoot));
	const FImportInitializationDomainRequest InlineSchemaRoot(
		TEXT("content_graph.import_workspace"), TEXT("uai.blueprint.edit/1"),
		TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
		TEXT("/Script/Engine.Blueprint"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestTrue(TEXT("the inline Blueprint schema remains accepted"),
		Provider->Matches(InlineSchemaRoot));
	const FImportInitializationDomainRequest UnrelatedSchemaRoot(
		TEXT("content_graph.import_workspace"), TEXT("uai.blueprint.edit/2"),
		TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
		TEXT("/Script/Engine.Blueprint"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unrelated Blueprint schema is rejected"),
		Provider->Matches(UnrelatedSchemaRoot));
	struct FContractCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FContractCase> ContractCases = {
		{TEXT("/Script/Engine.Blueprint"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.blueprint.asset/1")},
		{TEXT("uai.blueprint.interface"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.interface/1")},
		{TEXT("uai.blueprint.member"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.member/1")},
		{TEXT("uai.blueprint.component"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.component/1")},
		{TEXT("uai.blueprint.graph"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.graph/1")},
		{TEXT("uai.blueprint.node"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.node/1")},
		{TEXT("uai.blueprint.pin"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.pin/1")},
		{TEXT("uai.blueprint.edge"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.blueprint.edge/1")},
	};
	for (const FContractCase& ContractCase : ContractCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.blueprint.edit/1"),
			TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
			ContractCase.Type,
			ContractCase.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("Blueprint surface resolves: %s"),
			ContractCase.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, WorkspaceError));
		TestEqual(TEXT("the initialization contract is exact"), Contract,
			FString(ContractCase.Contract));
	}
	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"), TEXT("uai.blueprint.edit/1"),
		TEXT("/Game/UAI/StrictV2/T14/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered Blueprint root is rejected before write"),
		Provider->Matches(Unsupported));
	const FImportInitializationDomainRequest UnsupportedRole(
		TEXT("content_graph.import_workspace"), GetSemanticWorkspaceSchemaId(),
		TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
		TEXT("/Script/Engine.Blueprint"), EInitializationNodeRole::ContainerItem,
		TEXT("{}"));
	TestFalse(TEXT("a Blueprint root with a non-root role is rejected"),
		Provider->Matches(UnsupportedRole));

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	const auto* SharedIngress = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared HJSON ingress is active"), SharedIngress);
	if (SharedIngress)
	{
		TestEqual(TEXT("the shared ingress retains one public owner"),
			SharedIngress->GetPublicOwner(),
			FString(TEXT("content-graph-workspace-owner")));
	}
	TestNull(TEXT("the replaced Blueprint HJSON command is inactive"),
		Registry->FindExact(TEXT("blueprint.authoring.apply")));
	if (!TestTrue(TEXT("Blueprint authoring registration succeeds"),
		UeAgentBlueprintAuthoring::RegisterAuthoring(Error)))
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	FTargetCompositionContract BlueprintCompositionContract;
	if (!TestTrue(TEXT("Blueprint authoring registers its shared export composition contract"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.blueprint.asset"), BlueprintCompositionContract)))
	{
		return false;
	}

	const FString CompositionAssetPath = TEXT("/Game/UAI/StrictV2/T14/BP_Composition_Route");
	auto MakeCompositionEntry = [&CompositionAssetPath](
		const FString& SemanticId,
		const FString& TargetKind,
		const FString& CanonicalKey,
		TMap<FString, FString> Selectors,
		TSharedPtr<FJsonValue> Value)
	{
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = SemanticId;
		Entry.TargetIdentity.TargetKind = TargetKind;
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = MoveTemp(Selectors);
		Entry.Value = MoveTemp(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:t14:composition-route");
		return Entry;
	};
	TSharedRef<FJsonObject> InterfaceValue = MakeShared<FJsonObject>();
	InterfaceValue->SetStringField(
		TEXT("class_path"), TEXT("/Script/GameplayTags.GameplayTagAssetInterface"));
	FCompositionRequest CompositionRequest;
	CompositionRequest.TargetKind = TEXT("ue.blueprint.asset");
	CompositionRequest.ProfileId = TEXT("llm_authoring");
	CompositionRequest.BaseRevision = TEXT("fixture-t14-composition-route-r1");
	CompositionRequest.SourceProvenance = TEXT("fixture:t14:composition-route");
	CompositionRequest.Entries = {
		MakeCompositionEntry(
			UeAgentBlueprintAuthoring::ParentClassSemanticId,
			TEXT("blueprint_asset"),
			TEXT("blueprint-parent:fixture"),
			{{TEXT("asset_path"), CompositionAssetPath}},
			MakeShared<FJsonValueString>(TEXT("/Script/Engine.Actor"))),
		MakeCompositionEntry(
			UeAgentBlueprintAuthoring::InterfaceSemanticId,
			TEXT("blueprint_interface"),
			TEXT("blueprint-interface:fixture"),
			{{TEXT("asset_path"), CompositionAssetPath},
				{TEXT("interface_class"), TEXT("/Script/GameplayTags.GameplayTagAssetInterface")}},
			MakeShared<FJsonValueObject>(InterfaceValue)),
	};
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	FUeAgentResourceTracker CompositionResources;
	FAuthoringModelPtr CompositionModel;
	FWorkspaceError CompositionError;
	if (!TestTrue(TEXT("parent and interface semantics compose"),
		TryComposeAuthoringModel(
			CompositionRequest,
			CompositionResources,
			CompositionModel,
			CompositionError))
		|| !CompositionModel.IsValid())
	{
		AddError(CompositionError.GetCode());
		return false;
	}
	TestTrue(TEXT("an interface is a member-collection item"),
		CompositionModel->GetUnits().ContainsByPredicate(
			[](const FSemanticUnit& Unit)
			{
				return Unit.GetSemanticTypeRef()
						== UeAgentBlueprintAuthoring::InterfaceSemanticId
					&& Unit.GetRole() == TEXT("member_collection")
					&& Unit.GetUnitKind() == ESemanticUnitKind::ItemState;
			}));
	FExportWorkspacePlanPtr CompositionPlan;
	TestTrue(TEXT("parent and interface receive collision-free workspace paths"),
		TryBuildCompositionExportPlan(
			CompositionModel.ToSharedRef(),
			{},
			CompositionResources,
			CompositionPlan,
			CompositionError));

	for (const FString& PinName : {FString(TEXT("First")), FString(TEXT("Second"))})
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity PinIdentity;
		const FString PinGuid = PinName == TEXT("First")
			? TEXT("11111111-1111-1111-1111-111111111111")
			: TEXT("22222222-2222-2222-2222-222222222222");
		TestTrue(TEXT("each pin default accepts an independent property path"),
			UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				UeAgentBlueprintAuthoring::PinDefaultIdentityModelId,
				TEXT("blueprint_pin_default"),
				{{TEXT("asset_path"), CompositionAssetPath},
					{TEXT("graph_guid"), TEXT("33333333-3333-3333-3333-333333333333")},
					{TEXT("node_guid"), TEXT("44444444-4444-4444-4444-444444444444")},
					{TEXT("pin_guid"), PinGuid},
					{TEXT("pin_name"), PinName},
					{TEXT("property_path"), TEXT("pin_default.") + PinGuid}},
				PinIdentity,
				Error));
	}

	TSharedPtr<const FImportInitializationDomainProviderRegistry>
		DomainRegistry;
	if (!TestTrue(TEXT("the process domain providers freeze"),
		TryFreezeProcessImportInitializationDomainProviders(
			DomainRegistry, WorkspaceError))
		|| !DomainRegistry.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	const FImportInitializationDomainRequest RootReadRequest(
		TEXT("content_graph.structure"),
		TEXT("uai.content_graph.resolved_target/1"),
		TEXT("/Game/UAI/StrictV2/T14/BP_T14.BP_T14"),
		TEXT("/Script/Engine.Blueprint"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	FImportInitializationDomainResolution RootReadResolution;
	if (!TestTrue(TEXT("Blueprint root read has one domain provider"),
		DomainRegistry->TryResolve(
			RootReadRequest, RootReadResolution, WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	TestEqual(TEXT("Blueprint Surfaces owns root content-graph reads"),
		RootReadResolution.GetProvider()->GetProviderId(),
		FString(TEXT("blueprint-surface-domain-provider")));

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* InterfaceBlueprint = FKismetEditorUtilities::CreateBlueprint(
		UInterface::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BPI_T14_") + Suffix), BPTYPE_Interface,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("T14"));
	UBlueprint* ScratchBlueprint = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_T14_Scratch_") + Suffix), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("T14"));
	if (!TestNotNull(TEXT("a real Blueprint interface fixture is created"),
		InterfaceBlueprint)
		|| !TestNotNull(TEXT("a real Blueprint scratch fixture is created"),
			ScratchBlueprint))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(InterfaceBlueprint);
	if (!TestTrue(TEXT("the interface fixture has a generated class"),
		InterfaceBlueprint->GeneratedClass != nullptr))
	{
		return false;
	}

	const TSharedRef<FJsonObject> InterfaceSpec = MakeShared<FJsonObject>();
	InterfaceSpec->SetStringField(
		TEXT("class_path"), InterfaceBlueprint->GeneratedClass->GetPathName());
	const TSharedRef<FJsonObject> Interfaces = MakeShared<FJsonObject>();
	Interfaces->SetObjectField(TEXT("primary"), InterfaceSpec);
	const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("interfaces"), Interfaces);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	if (!TestTrue(TEXT("the interface surface passes complete preflight"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*ScratchBlueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	TestEqual(TEXT("one interface is added through native Blueprint mechanics"),
		Counters.InterfacesAdded, 1);
	TestEqual(TEXT("the Blueprint independently exposes the implemented interface"),
		ScratchBlueprint->ImplementedInterfaces.Num(), 1);
	if (ScratchBlueprint->ImplementedInterfaces.Num() == 1)
	{
		TestTrue(TEXT("the persisted interface identity is unchanged"),
			ScratchBlueprint->ImplementedInterfaces[0].Interface.Get()
				== InterfaceBlueprint->GeneratedClass.Get());
	}

	UEdGraph* EventGraph = ScratchBlueprint->UbergraphPages.IsEmpty()
		? nullptr
		: ScratchBlueprint->UbergraphPages[0];
	if (!TestNotNull(TEXT("the scratch Blueprint has a real EventGraph"), EventGraph))
	{
		return false;
	}
	const FString FirstNodeGuid = TEXT("55555555-5555-5555-5555-555555555555");
	const FString SecondNodeGuid = TEXT("66666666-6666-6666-6666-666666666666");
	auto MakeRotatorNode = [](const int32 X, const int32 Y)
	{
		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("node_type"), TEXT("call_function"));
		Node->SetStringField(
			TEXT("function_owner_class"), TEXT("/Script/Engine.KismetMathLibrary"));
		Node->SetStringField(TEXT("function_name"), TEXT("MakeRotator"));
		const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), X);
		Position->SetNumberField(TEXT("y"), Y);
		Node->SetObjectField(TEXT("pos"), Position);
		return Node;
	};
	const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(FirstNodeGuid, MakeRotatorNode(0, 400));
	Nodes->SetObjectField(SecondNodeGuid, MakeRotatorNode(300, 400));
	const TSharedRef<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
	GraphSpec->SetStringField(TEXT("operation"), TEXT("update"));
	GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
	const TSharedRef<FJsonObject> GraphHeader = MakeShared<FJsonObject>();
	GraphHeader->SetStringField(TEXT("name"), EventGraph->GetName());
	GraphHeader->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	GraphHeader->SetStringField(
		TEXT("guid"),
		EventGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	GraphSpec->SetObjectField(TEXT("graph"), GraphHeader);
	const TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(
		EventGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		GraphSpec);
	const TSharedRef<FJsonObject> DuplicateFunctionExtension = MakeShared<FJsonObject>();
	DuplicateFunctionExtension->SetObjectField(TEXT("graphs"), Graphs);
	Mutation.Reset();
	Counters = {};
	Error.Reset();
	if (!TestTrue(TEXT("two same-function nodes with distinct GUIDs pass preflight"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*ScratchBlueprint,
			DuplicateFunctionExtension,
			Mutation,
			Counters,
			Error)))
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	TestEqual(TEXT("both GUID-addressed function nodes are created"),
		Counters.NodesAdded, 2);
	int32 MakeRotatorCount = 0;
	TSet<FGuid> MakeRotatorGuids;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
		if (Function && Function->GetName() == TEXT("MakeRotator"))
		{
			++MakeRotatorCount;
			MakeRotatorGuids.Add(Call->NodeGuid);
		}
	}
	TestEqual(TEXT("independent readback retains both same-function nodes"),
		MakeRotatorCount, 2);
	TestEqual(TEXT("independent readback retains two distinct node GUIDs"),
		MakeRotatorGuids.Num(), 2);

	UBlueprint* OrderedBlueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_T14_Ordered_") + Suffix), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("T14"));
	UEdGraph* OrderedGraph = OrderedBlueprint
		&& !OrderedBlueprint->UbergraphPages.IsEmpty()
		? OrderedBlueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("the ordered mutation fixture exists"), OrderedBlueprint)
		|| !TestNotNull(TEXT("the ordered mutation fixture has EventGraph"), OrderedGraph))
	{
		return false;
	}
	const FString SourceGuidText =
		TEXT("11111111-1111-1111-1111-111111111111");
	const FString CandidateGuidText =
		TEXT("00000000-0000-0000-0000-000000000001");
	FGuid SourceGuid;
	FGuid CandidateGuid;
	if (!TestTrue(TEXT("the source test GUID is valid"),
			FGuid::Parse(SourceGuidText, SourceGuid))
		|| !TestTrue(TEXT("the candidate test GUID is valid"),
			FGuid::Parse(CandidateGuidText, CandidateGuid)))
	{
		return false;
	}
	UK2Node_Event* OldSourceEvent = NewObject<UK2Node_Event>(OrderedGraph);
	OldSourceEvent->NodeGuid = SourceGuid;
	OldSourceEvent->EventReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(AActor, ReceiveTick),
		AActor::StaticClass());
	OrderedGraph->AddNode(OldSourceEvent, false, false);
	OldSourceEvent->PostPlacedNewNode();
	OldSourceEvent->AllocateDefaultPins();

	UeAgentPaletteSearch::FSearchParams CandidateParams;
	CandidateParams.CatalogType = TEXT("blueprint_node");
	CandidateParams.AssetPath = OrderedBlueprint->GetPathName();
	CandidateParams.GraphName = OrderedGraph->GetName();
	CandidateParams.NodeGuid = SourceGuidText;
	CandidateParams.PinName = TEXT("then");
	CandidateParams.SearchText = TEXT("Branch");
	CandidateParams.bContextSensitive = true;
	UeAgentPaletteSearch::FSearchResult CandidateResult;
	FString CandidateError;
	if (!TestTrue(TEXT("the old source exposes the real Branch action"),
		UeAgentPaletteSearch::ExecuteSearch(
			CandidateParams, CandidateResult, CandidateError)))
	{
		AddError(CandidateError);
		return false;
	}
	TArray<const UeAgentPaletteSearch::FCandidate*> BranchCandidates;
	for (const UeAgentPaletteSearch::FCandidate& CandidateItem
		: CandidateResult.MatchingCandidates)
	{
		const FString* NodeClass =
			CandidateItem.AuthoringSelectors.Find(TEXT("node_class"));
		if (NodeClass
			&& *NodeClass
				== UK2Node_IfThenElse::StaticClass()->GetPathName())
		{
			BranchCandidates.Add(&CandidateItem);
		}
	}
	if (!TestEqual(TEXT("the contextual Branch candidate is unique"),
		BranchCandidates.Num(), 1))
	{
		return false;
	}

	const TSharedRef<FJsonObject> SourceNodeSpec = MakeShared<FJsonObject>();
	SourceNodeSpec->SetStringField(TEXT("operation"), TEXT("create"));
	SourceNodeSpec->SetStringField(TEXT("node_type"), TEXT("event"));
	SourceNodeSpec->SetStringField(TEXT("event_name"), TEXT("ReceiveTick"));
	SourceNodeSpec->SetStringField(
		TEXT("event_class"), AActor::StaticClass()->GetPathName());
	const TSharedRef<FJsonObject> CandidateSelector = MakeShared<FJsonObject>();
	CandidateSelector->SetStringField(
		TEXT("name"), BranchCandidates[0]->Name.IsEmpty()
			? BranchCandidates[0]->DisplayName
			: BranchCandidates[0]->Name);
	const TSharedRef<FJsonObject> CandidateNodeSpec = MakeShared<FJsonObject>();
	CandidateNodeSpec->SetStringField(TEXT("operation"), TEXT("create"));
	CandidateNodeSpec->SetStringField(TEXT("node_type"), TEXT("native_action"));
	CandidateNodeSpec->SetObjectField(TEXT("candidate"), CandidateSelector);
	const TSharedRef<FJsonObject> OrderedNodes = MakeShared<FJsonObject>();
	OrderedNodes->SetObjectField(CandidateGuidText, CandidateNodeSpec);
	OrderedNodes->SetObjectField(SourceGuidText, SourceNodeSpec);
	const TSharedRef<FJsonObject> OrderedGraphHeader = MakeShared<FJsonObject>();
	OrderedGraphHeader->SetStringField(TEXT("name"), OrderedGraph->GetName());
	OrderedGraphHeader->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	OrderedGraphHeader->SetStringField(
		TEXT("guid"),
		OrderedGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	const TSharedRef<FJsonObject> OrderedGraphSpec = MakeShared<FJsonObject>();
	OrderedGraphSpec->SetStringField(TEXT("operation"), TEXT("replace"));
	OrderedGraphSpec->SetObjectField(TEXT("graph"), OrderedGraphHeader);
	OrderedGraphSpec->SetObjectField(TEXT("nodes"), OrderedNodes);
	const TSharedRef<FJsonObject> OrderedGraphs = MakeShared<FJsonObject>();
	OrderedGraphs->SetObjectField(
		OrderedGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		OrderedGraphSpec);
	const TSharedRef<FJsonObject> OrderedExtension = MakeShared<FJsonObject>();
	OrderedExtension->SetObjectField(TEXT("graphs"), OrderedGraphs);
	TestTrue(TEXT("the dependent candidate key sorts before its source key"),
		CandidateGuidText.Compare(SourceGuidText) < 0);
	Mutation.Reset();
	Counters = {};
	Error.Reset();
	if (!TestTrue(
		TEXT("replace creates the source and name-selected native candidate"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*OrderedBlueprint,
			OrderedExtension,
			Mutation,
			Counters,
			Error)))
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	UEdGraphNode* RebuiltSource = nullptr;
	UK2Node_IfThenElse* CreatedBranch = nullptr;
	for (UEdGraphNode* Node : OrderedGraph->Nodes)
	{
		if (Node && Node->NodeGuid == SourceGuid)
		{
			RebuiltSource = Node;
		}
		if (Node && Node->NodeGuid == CandidateGuid)
		{
			CreatedBranch = Cast<UK2Node_IfThenElse>(Node);
		}
	}
	TestNotNull(TEXT("replace rebuilds the contextual source GUID"), RebuiltSource);
	TestNotNull(TEXT("replace creates the native Branch candidate"), CreatedBranch);
	TestTrue(TEXT("replace does not retain the removed source object"),
		RebuiltSource != OldSourceEvent);
	if (RebuiltSource && CreatedBranch)
	{
		UEdGraphPin* ThenPin = RebuiltSource->FindPin(FName(TEXT("then")));
		UEdGraphPin* ExecutePin = CreatedBranch->GetExecPin();
		TestNotNull(TEXT("the rebuilt source has its then pin"), ThenPin);
		TestNotNull(TEXT("the native Branch has its execute pin"), ExecutePin);
		if (ThenPin && ExecutePin)
		{
			TestTrue(TEXT("the real pin-context candidate path reconnects"),
				ThenPin->LinkedTo.Contains(ExecutePin));
		}
	}
	return !HasAnyErrors();
}
