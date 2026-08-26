#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "UObject/Interface.h"

#if __has_include("OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h")
#define UAI_T14_BLUEPRINT_SURFACES_AVAILABLE 1
#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"
#else
#define UAI_T14_BLUEPRINT_SURFACES_AVAILABLE 0
#endif

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
#if !UAI_T14_BLUEPRINT_SURFACES_AVAILABLE
	TestTrue(TEXT("T14 Blueprint Surfaces authority is available"), false);
	return false;
#else
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
	TestTrue(TEXT("Blueprint Surfaces explicitly supports authoring-schema discovery"),
		Provider->SupportsExplicitCommand(
			TEXT("content_graph.authoring_schema")));

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

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* SharedIngress = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared HJSON ingress is active"), SharedIngress);
	if (SharedIngress)
	{
		TestEqual(TEXT("the shared ingress retains one public owner"),
			SharedIngress->GetPublicOwner(),
			FString(TEXT("uobject-dataasset-recursive-initialization-owner")));
	}
	TestNull(TEXT("the replaced Blueprint HJSON command is inactive"),
		Registry->FindExact(TEXT("blueprint.authoring.apply")));
	if (!TestTrue(TEXT("Blueprint authoring registration succeeds"),
		UeAgentBlueprintAuthoring::RegisterAuthoring(Error)))
	{
		AddError(Error);
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
	using UE::UAI::ContextResource::FUeAgentResourceBudget;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	const TMap<EUeAgentResourceDimension, int64> CompositionLimits{
		{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096},
		{EUeAgentResourceDimension::Files, 1024},
		{EUeAgentResourceDimension::ReferenceEdges, 4096},
		{EUeAgentResourceDimension::WorkUnits, 32768},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
	};
	FUeAgentResourceTracker CompositionResources{
		FUeAgentResourceBudget(CompositionLimits)};
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
	UBlueprint* RevisionBlueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_T14_Revision_") + Suffix), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("T14"));
	if (!TestNotNull(TEXT("a transient Actor Blueprint revision fixture is created"),
		RevisionBlueprint)
		|| !TestNotNull(TEXT("the revision fixture has an SCS"),
			RevisionBlueprint
				? RevisionBlueprint->SimpleConstructionScript.Get()
				: nullptr))
	{
		return false;
	}
	USCS_Node* RevisionNode =
		RevisionBlueprint->SimpleConstructionScript->CreateNode(
			USceneComponent::StaticClass(), TEXT("RevisionRootT14"));
	if (!TestNotNull(TEXT("the revision fixture has a SceneComponent node"),
		RevisionNode))
	{
		return false;
	}
	RevisionBlueprint->SimpleConstructionScript->AddNode(RevisionNode);
	USceneComponent* RevisionTemplate =
		Cast<USceneComponent>(RevisionNode->ComponentTemplate);
	if (!TestNotNull(TEXT("the revision node has a SceneComponent template"),
		RevisionTemplate))
	{
		return false;
	}
	auto ReadAuthoringSchemaRevision = [this, &Provider, &WorkspaceError](
		const FString& Target,
		FString& OutRevision) -> bool
	{
		OutRevision.Reset();
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.authoring_schema"),
			TEXT("uai.content_graph.resolved_target/1"),
			Target,
			TEXT("/Script/Engine.Blueprint"),
			EInitializationNodeRole::RootTarget,
			FString::Printf(TEXT("{\"target\":\"%s\"}"), *Target));
		FString Contract;
		FImportInitializationDomainResponse Response;
		if (!Provider->TryResolveInitializationContract(
				Request, Contract, WorkspaceError)
			|| !Provider->TryExecute(
				Request, Contract, Response, WorkspaceError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Payload;
		FString JsonError;
		const TSharedPtr<FJsonObject>* QueryResult = nullptr;
		return UE::UAI::CommandContract::Json::TryParseObject(
				Response.GetPayloadJson(), Payload, JsonError)
			&& Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("query_result"), QueryResult)
			&& QueryResult && QueryResult->IsValid()
			&& (*QueryResult)->TryGetStringField(TEXT("revision"), OutRevision)
			&& !OutRevision.IsEmpty();
	};
	const FString RevisionTarget = RevisionBlueprint->GetPathName();
	FString InitialRevision;
	FString StableInitialRevision;
	FString ChangedRevision;
	FString StableChangedRevision;
	TestTrue(TEXT("the initial Component template revision is readable"),
		ReadAuthoringSchemaRevision(RevisionTarget, InitialRevision));
	TestTrue(TEXT("the unchanged Component template revision is readable"),
		ReadAuthoringSchemaRevision(RevisionTarget, StableInitialRevision));
	TestEqual(TEXT("the unchanged Component template revision is stable"),
		StableInitialRevision, InitialRevision);
	RevisionTemplate->SetRelativeLocation(FVector(125.0, 25.0, 5.0));
	TestTrue(TEXT("the changed Component template revision is readable"),
		ReadAuthoringSchemaRevision(RevisionTarget, ChangedRevision));
	TestTrue(TEXT("a Component template property changes the revision"),
		ChangedRevision != InitialRevision);
	TestTrue(TEXT("the repeated changed Component template revision is readable"),
		ReadAuthoringSchemaRevision(RevisionTarget, StableChangedRevision));
	TestEqual(TEXT("the changed Component template revision is stable"),
		StableChangedRevision, ChangedRevision);
	const FString SchemaTarget = ScratchBlueprint->GetPathName();
	const FImportInitializationDomainRequest SchemaRequest(
		TEXT("content_graph.authoring_schema"),
		TEXT("uai.content_graph.resolved_target/1"),
		SchemaTarget,
		TEXT("/Script/Engine.Blueprint"),
		EInitializationNodeRole::RootTarget,
		FString::Printf(TEXT("{\"target\":\"%s\"}"), *SchemaTarget));
	FString SchemaContract;
	FImportInitializationDomainResponse SchemaResponse;
	TestTrue(TEXT("the Blueprint authoring-schema route resolves"),
		Provider->TryResolveInitializationContract(
			SchemaRequest, SchemaContract, WorkspaceError));
	if (TestTrue(TEXT("the Blueprint authoring-schema mechanics execute"),
		Provider->TryExecute(
			SchemaRequest, SchemaContract, SchemaResponse, WorkspaceError)))
	{
		TestTrue(TEXT("the domain response is read-only"),
			SchemaResponse.IsReadOnly());
		TSharedPtr<FJsonObject> SchemaPayload;
		FString JsonError;
		TestTrue(TEXT("the Blueprint authoring-schema payload is valid JSON"),
			UE::UAI::CommandContract::Json::TryParseObject(
				SchemaResponse.GetPayloadJson(), SchemaPayload, JsonError)
				&& SchemaPayload.IsValid());
		if (SchemaPayload.IsValid())
		{
			const TSharedPtr<FJsonObject>* QueryResult = nullptr;
			TestEqual(TEXT("the schema response command identity is exact"),
				SchemaPayload->GetStringField(TEXT("command_id")),
				FString(TEXT("content_graph.authoring_schema")));
			TestTrue(TEXT("the schema response is complete and read-only"),
				SchemaPayload->GetBoolField(TEXT("read_only"))
				&& SchemaPayload->GetBoolField(TEXT("complete"))
				&& !SchemaPayload->GetBoolField(TEXT("has_more")));
			TestTrue(TEXT("the schema query result is present"),
				SchemaPayload->TryGetObjectField(TEXT("query_result"), QueryResult)
				&& QueryResult && QueryResult->IsValid());
			if (QueryResult && QueryResult->IsValid())
			{
				TestEqual(TEXT("the Blueprint schema id is exact"),
					(*QueryResult)->GetStringField(TEXT("schema_id")),
					FString(TEXT("uai.blueprint.edit/1")));
				TestEqual(TEXT("the Blueprint schema root type is exact"),
					(*QueryResult)->GetStringField(TEXT("root_type")),
					FString(TEXT("/Script/Engine.Blueprint")));
				TestEqual(TEXT("the Blueprint schema provider is exact"),
					(*QueryResult)->GetStringField(TEXT("provider")),
					FString(TEXT("blueprint-surface-domain-provider")));
				TestFalse(TEXT("the Blueprint schema carries the current revision"),
					(*QueryResult)->GetStringField(TEXT("revision")).IsEmpty());
				const TSharedPtr<FJsonObject>* Template = nullptr;
				const TSharedPtr<FJsonObject>* Root = nullptr;
				const TSharedPtr<FJsonObject>* Document = nullptr;
				const TSharedPtr<FJsonObject>* Target = nullptr;
				const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
				TestTrue(TEXT("the schema exposes an executable update template"),
					(*QueryResult)->TryGetObjectField(TEXT("template"), Template)
					&& Template && Template->IsValid()
					&& (*Template)->GetStringField(TEXT("schema"))
						== TEXT("uai.blueprint.edit/1")
					&& (*Template)->TryGetObjectField(TEXT("root"), Root)
					&& Root && Root->IsValid()
					&& (*Root)->GetStringField(TEXT("type"))
						== TEXT("/Script/Engine.Blueprint")
					&& (*Root)->TryGetObjectField(TEXT("document"), Document)
					&& Document && Document->IsValid()
					&& (*Document)->GetStringField(TEXT("operation")) == TEXT("update")
					&& (*Document)->TryGetObjectField(TEXT("target"), Target)
					&& Target && Target->IsValid()
					&& (*Target)->GetStringField(TEXT("asset_path")) == SchemaTarget
					&& (*Document)->TryGetObjectField(
						TEXT("provider_extension"), ProviderExtension)
					&& ProviderExtension && ProviderExtension->IsValid());
			}
		}
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
		AddError(Error);
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
		AddError(Error);
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
	return !HasAnyErrors();
#endif
}
