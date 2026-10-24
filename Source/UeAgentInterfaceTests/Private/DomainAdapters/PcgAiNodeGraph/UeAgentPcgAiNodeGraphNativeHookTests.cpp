#include "Misc/AutomationTest.h"

// GC-007 rebuilds this source once before production and once after it.
#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphNativeHook.h"

#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "PCGParamData.h"
#include "Elements/PCGCreatePointsGrid.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "GameFramework/Actor.h"
#include "GameplayInteractionSmartObjectBehaviorDefinition.h"
#include "PCGComponent.h"
#include "PCGDataAsset.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPcgAiNodeGraphNativeHookTest,
	"UeAgentInterface.DomainAdapters.PcgAiNodeGraph.E2.Native."
		"OwnedTenFamilyMutationBarrierAndIndependentReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentPcgAiNodeGraphNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::PcgAiNodeGraph;

	FPcgAiNodeGraphNativeHook Hook;
	FPcgAiNodeGraphAdapterError Error;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/PcgAiNodeGraph/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	UObject* CreatedObject = nullptr;
	FPcgAiNodeGraphNativeMutationReceipt AssetCreateReceipt;
	FPcgAiNodeGraphBarrierReceipt AssetCreateBarrier;
	FPcgAiNodeGraphAssetIdentity AssetCreateIdentity;
	FString AssetCreateRevision;
	FString AssetCreateEvidence;
	TestTrue(TEXT("asset lifecycle creates, refreshes and independently captures"),
		Hook.TryCreateAndCaptureOwnedAssetObject(
			*Package, TEXT("behavior_tree"),
			*UBehaviorTree::StaticClass(), TEXT("BT_GC007_Owned"),
			CreatedObject, AssetCreateReceipt, AssetCreateBarrier,
			AssetCreateIdentity, AssetCreateRevision,
			AssetCreateEvidence, Error));
	UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(CreatedObject);
	if (!TestNotNull(TEXT("owned BehaviorTree exists"), BehaviorTree))
	{
		Package->MarkAsGarbage();
		return false;
	}
	TestFalse(TEXT("asset lifecycle evidence is non-empty"),
		AssetCreateReceipt.GetEvidenceId().IsEmpty());
	TestEqual(TEXT("asset lifecycle uses refresh barrier"),
		AssetCreateBarrier.GetBarrierKind(),
		EPcgAiNodeGraphBarrierKind::Refresh);
	TestTrue(TEXT("asset lifecycle typed identity validates"),
		TryValidatePcgAiNodeGraphAssetIdentity(AssetCreateIdentity, Error));
	TestEqual(TEXT("asset lifecycle reads exact native class"),
		AssetCreateIdentity.GetNativeClass(),
		UBehaviorTree::StaticClass()->GetPathName());

	UBehaviorTreeGraph* BehaviorGraph = NewObject<UBehaviorTreeGraph>(
		BehaviorTree, TEXT("BehaviorTreeGraph"), RF_Transactional);
	BehaviorTree->BTGraph = BehaviorGraph;
	BehaviorGraph->OnCreated();
	BehaviorGraph->Initialize();
	UBehaviorTreeGraphNode_Root* BehaviorRoot =
		NewObject<UBehaviorTreeGraphNode_Root>(BehaviorGraph);
	BehaviorRoot->CreateNewGuid();
	BehaviorRoot->PostPlacedNewNode();
	BehaviorRoot->AllocateDefaultPins();
	BehaviorGraph->AddNode(BehaviorRoot, false, false);
	UBehaviorTreeGraphNode_Composite* BehaviorSelector =
		NewObject<UBehaviorTreeGraphNode_Composite>(BehaviorGraph);
	BehaviorSelector->CreateNewGuid();
	BehaviorSelector->NodeInstance = NewObject<UBTComposite_Selector>(
		BehaviorTree, TEXT("Selector_GC007"), RF_Transactional);
	BehaviorSelector->PostPlacedNewNode();
	BehaviorSelector->AllocateDefaultPins();
	BehaviorGraph->AddNode(BehaviorSelector, false, false);
	if (BehaviorRoot->GetOutputPin() && BehaviorSelector->GetInputPin())
	{
		BehaviorRoot->GetOutputPin()->MakeLinkTo(
			BehaviorSelector->GetInputPin());
	}

	FPcgAiNodeGraphNativeMutationReceipt BehaviorApply;
	FPcgAiNodeGraphBarrierReceipt BehaviorBarrier;
	FPcgAiNodeGraphTopology BehaviorTopology;
	FString BehaviorRevision;
	FString BehaviorEvidence;
	TestTrue(TEXT("BehaviorTree graph compiles and is independently captured"),
		Hook.TryCompileAndCaptureBehaviorTree(
			*BehaviorTree, BehaviorApply, BehaviorBarrier,
			BehaviorTopology, BehaviorRevision, BehaviorEvidence, Error));
	TestTrue(TEXT("BehaviorTree topology validates"),
		TryValidatePcgAiNodeGraphTopology(BehaviorTopology, Error));
	TestTrue(TEXT("BehaviorTree keeps editor carrier and runtime type"),
		BehaviorTopology.GetItems().ContainsByPredicate(
			[&](const FPcgAiNodeGraphItemIdentity& Item)
			{
				return Item.GetNativeIdentity()
					== BehaviorSelector->NodeGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower)
					&& Item.GetNativeCarrierTypeIdentity()
						== BehaviorSelector->GetClass()->GetPathName()
					&& Item.GetNativeTypeIdentity()
						== UBTComposite_Selector::StaticClass()->GetPathName();
			}));

	UEnvQuery* Query = NewObject<UEnvQuery>(
		Package, TEXT("EQS_GC007_Owned"), RF_Transactional);
	UEnvironmentQueryGraph* QueryGraph = NewObject<UEnvironmentQueryGraph>(
		Query, TEXT("EnvironmentQueryGraph"), RF_Transactional);
	Query->EdGraph = QueryGraph;
	QueryGraph->Initialize();
	UEnvironmentQueryGraphNode_Root* QueryRoot =
		NewObject<UEnvironmentQueryGraphNode_Root>(QueryGraph);
	QueryRoot->CreateNewGuid();
	QueryRoot->PostPlacedNewNode();
	QueryRoot->AllocateDefaultPins();
	QueryGraph->AddNode(QueryRoot, false, false);
	UEnvironmentQueryGraphNode_Option* QueryOptionNode =
		NewObject<UEnvironmentQueryGraphNode_Option>(QueryGraph);
	QueryOptionNode->CreateNewGuid();
	UEnvQueryOption* QueryOption = NewObject<UEnvQueryOption>(
		Query, TEXT("Option_GC007"), RF_Transactional);
	QueryOption->Generator = NewObject<UEnvQueryGenerator_ActorsOfClass>(
		QueryOption, TEXT("Generator_GC007"), RF_Transactional);
	QueryOptionNode->NodeInstance = QueryOption;
	QueryOptionNode->PostPlacedNewNode();
	QueryOptionNode->AllocateDefaultPins();
	QueryGraph->AddNode(QueryOptionNode, false, false);
	if (QueryRoot->GetOutputPin() && QueryOptionNode->GetInputPin())
	{
		QueryRoot->GetOutputPin()->MakeLinkTo(QueryOptionNode->GetInputPin());
	}
	FPcgAiNodeGraphNativeMutationReceipt QueryApply;
	FPcgAiNodeGraphBarrierReceipt QueryBarrier;
	FPcgAiNodeGraphTopology QueryTopology;
	FString QueryRevision;
	FString QueryEvidence;
	TestTrue(TEXT("EQS graph compiles and is independently captured"),
		Hook.TryCompileAndCaptureEqs(
			*Query, QueryApply, QueryBarrier, QueryTopology,
			QueryRevision, QueryEvidence, Error));
	TestTrue(TEXT("EQS option keeps generator and graph carrier types"),
		QueryTopology.GetItems().ContainsByPredicate(
			[&](const FPcgAiNodeGraphItemIdentity& Item)
			{
				return Item.GetNativeIdentity()
					== QueryOptionNode->NodeGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower)
					&& Item.GetNativeCarrierTypeIdentity()
						== QueryOptionNode->GetClass()->GetPathName()
					&& Item.GetNativeTypeIdentity()
						== QueryOption->Generator->GetClass()->GetPathName();
			}));

	UStateTree* StateTree = NewObject<UStateTree>(
		Package, TEXT("ST_GC007_Owned"), RF_Transactional);
	FPcgAiNodeGraphNativeMutationReceipt StateApply;
	FPcgAiNodeGraphBarrierReceipt StateBarrier;
	FPcgAiNodeGraphTopology StateTopology;
	FString StateRevision;
	FString StateEvidence;
	const FGuid StateTransitionGuid = FGuid::NewGuid();
	TestTrue(TEXT("StateTree root writes, compiles and reads back"),
		Hook.TryEnsureCompileAndCaptureStateTree(
			*StateTree, TEXT("GC007Root"), TEXT("GC007Child"),
			StateTransitionGuid, StateApply, StateBarrier,
			StateTopology, StateRevision, StateEvidence, Error));
	TestTrue(TEXT("StateTree capture preserves native GUID"),
		StateTopology.GetItems().ContainsByPredicate(
			[](const FPcgAiNodeGraphItemIdentity& Item)
			{
				return !Item.GetNativeIdentity().IsEmpty()
					&& Item.GetNativeTypeIdentity() == TEXT("state_tree_state");
			}));
	TestTrue(TEXT("StateTree capture preserves hierarchy ordering"),
		StateTopology.GetRelations().ContainsByPredicate(
			[](const FPcgAiNodeGraphRelationIdentity& Relation)
			{
				return Relation.GetRelationKind() == TEXT("hierarchy_child")
					&& Relation.GetOrderIndex() == 0;
			}));
	TestTrue(TEXT("StateTree capture preserves transition GUID"),
		StateTopology.GetRelations().ContainsByPredicate(
			[&](const FPcgAiNodeGraphRelationIdentity& Relation)
			{
				return Relation.GetRelationKind() == TEXT("state_transition")
					&& Relation.GetNativeRelationIdentity()
						== StateTransitionGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower);
			}));

	UPCGGraph* PcgGraph = NewObject<UPCGGraph>(
		Package, TEXT("PCG_GC007_Owned"), RF_Transactional);
	FPcgAiNodeGraphTopology PcgBefore;
	TArray<FPcgAiNodeGraphParameterIdentity> ParametersBefore;
	FPcgAiNodeGraphBarrierReceipt PcgBeforeBarrier;
	FString PcgBeforeRevision;
	FString PcgBeforeEvidence;
	TestTrue(TEXT("initial PCG graph is independently captured"),
		Hook.TryCompileAndCapturePcgGraph(
			*PcgGraph, PcgBeforeBarrier, PcgBefore, ParametersBefore,
			PcgBeforeRevision, PcgBeforeEvidence, Error));

	UPCGNode* PcgNode = nullptr;
	FPcgAiNodeGraphNativeMutationReceipt PcgApply;
	TestTrue(TEXT("PCG node is added through native graph API"),
		Hook.TryAddPcgNode(
			*PcgGraph, *UPCGCreatePointsGridSettings::StaticClass(),
			PcgNode, PcgApply, Error));
	if (!TestNotNull(TEXT("added PCG node exists"), PcgNode))
	{
		Package->MarkAsGarbage();
		return false;
	}
	const UPCGPin* InputOutput = PcgGraph->GetInputNode()->GetOutputPins().IsEmpty()
		? nullptr : PcgGraph->GetInputNode()->GetOutputPins()[0];
	const UPCGPin* NodeInput = PcgNode->GetInputPins().IsEmpty()
		? nullptr : PcgNode->GetInputPins()[0];
	const UPCGPin* NodeOutput = PcgNode->GetOutputPins().IsEmpty()
		? nullptr : PcgNode->GetOutputPins()[0];
	const UPCGPin* GraphInput = PcgGraph->GetOutputNode()->GetInputPins().IsEmpty()
		? nullptr : PcgGraph->GetOutputNode()->GetInputPins()[0];
	if (InputOutput && NodeInput)
	{
		TestTrue(TEXT("PCG input connects to native node pin"),
			Hook.TryConnectPcgNodes(
				*PcgGraph, *PcgGraph->GetInputNode(),
				InputOutput->Properties.Label, *PcgNode,
				NodeInput->Properties.Label, PcgApply, Error));
	}
	if (NodeOutput && GraphInput)
	{
		TestTrue(TEXT("PCG node connects to graph output"),
			Hook.TryConnectPcgNodes(
				*PcgGraph, *PcgNode, NodeOutput->Properties.Label,
				*PcgGraph->GetOutputNode(), GraphInput->Properties.Label,
				PcgApply, Error));
	}
	const FGuid DensityGuid = FGuid::NewGuid();
	FPcgAiNodeGraphBarrierReceipt ParameterBarrier;
	FPcgAiNodeGraphParameterIdentity ParameterIdentity;
	FString ParameterRevision;
	FString ParameterEvidence;
	TestTrue(TEXT("PCG parameter keeps GUID name and type"),
		Hook.TrySetPcgDoubleParameter(
			*PcgGraph, DensityGuid, TEXT("Density"), 0.625,
			PcgApply, ParameterBarrier, ParameterIdentity,
			ParameterRevision, ParameterEvidence, Error));
	TestEqual(TEXT("PCG parameter uses refresh barrier"),
		ParameterBarrier.GetBarrierKind(),
		EPcgAiNodeGraphBarrierKind::Refresh);
	TestTrue(TEXT("PCG parameter typed identity validates"),
		TryValidatePcgAiNodeGraphParameterIdentity(ParameterIdentity, Error));

	FPcgAiNodeGraphBarrierReceipt PcgBarrier;
	FPcgAiNodeGraphTopology PcgReadback;
	TArray<FPcgAiNodeGraphParameterIdentity> ParameterReadback;
	FString PcgRevision;
	FString PcgEvidence;
	TestTrue(TEXT("PCG graph compiles and independently reads back"),
		Hook.TryCompileAndCapturePcgGraph(
			*PcgGraph, PcgBarrier, PcgReadback, ParameterReadback,
			PcgRevision, PcgEvidence, Error));
	TestNotEqual(TEXT("PCG mutation advances structural revision"),
		PcgBeforeRevision, PcgRevision);
	TestTrue(TEXT("PCG readback contains native settings type"),
		PcgReadback.GetItems().ContainsByPredicate(
			[&](const FPcgAiNodeGraphItemIdentity& Item)
			{
				return Item.GetNativeIdentity() == PcgNode->GetPathName(PcgGraph)
					&& Item.GetNativeTypeIdentity()
						== UPCGCreatePointsGridSettings::StaticClass()->GetPathName();
			}));
	TestTrue(TEXT("PCG parameter readback preserves property GUID"),
		ParameterReadback.ContainsByPredicate(
			[&](const FPcgAiNodeGraphParameterIdentity& Identity)
			{
				return Identity.GetPropertyGuid() == DensityGuid
					&& Identity.GetPropertyName() == TEXT("Density")
					&& Identity.GetPropertyType() == TEXT("double");
			}));

	UWorld* World = UWorld::CreateWorld(
		EWorldType::EditorPreview, false,
		FName(*FString::Printf(TEXT("GC007World_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits))), Package);
	AActor* Actor = World ? World->SpawnActor<AActor>() : nullptr;
	UPCGComponent* Component = Actor
		? NewObject<UPCGComponent>(Actor, TEXT("PCGComponent_GC007"))
		: nullptr;
	if (!TestNotNull(TEXT("owned PCG actor exists"), Actor)
		|| !TestNotNull(TEXT("owned PCG component exists"), Component))
	{
		if (World) World->DestroyWorld(false);
		Package->MarkAsGarbage();
		return false;
	}
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	FPcgAiNodeGraphNativeMutationReceipt ComponentApply;
	FPcgAiNodeGraphBarrierReceipt ComponentBarrier;
	FPcgAiNodeGraphComponentBindingIdentity ComponentReadback;
	FString ComponentRevision;
	FString ComponentEvidence;
	TestTrue(TEXT("PCG component binding refreshes and reads back"),
		Hook.TryBindAndCapturePcgComponent(
			*Component, *PcgGraph, TEXT("generate_on_demand"),
			ComponentApply, ComponentBarrier, ComponentReadback,
			ComponentRevision, ComponentEvidence, Error));
	TestTrue(TEXT("PCG component binding identity validates"),
		TryValidatePcgAiNodeGraphComponentBindingIdentity(
			ComponentReadback, Error));

	UPCGDataAsset* DataAsset = NewObject<UPCGDataAsset>(
		Package, TEXT("PCGData_GC007_Owned"), RF_Transactional);
	UPCGParamData* ParamData = NewObject<UPCGParamData>(DataAsset);
	FPcgAiNodeGraphNativeMutationReceipt DataApply;
	FPcgAiNodeGraphBarrierReceipt DataBarrier;
	FPcgAiNodeGraphDataAssetIdentity DataReadback;
	FString DataEvidence;
	TestTrue(TEXT("PCG Data Asset content validates and reads back"),
		Hook.TrySetAndCapturePcgDataAsset(
			*DataAsset, *ParamData,
			FSoftObjectPath(TEXT("/Game/Maps/WB_WhiteBox01")),
			DataApply, DataBarrier, DataReadback, DataEvidence, Error));
	TestTrue(TEXT("PCG Data Asset identity validates"),
		TryValidatePcgAiNodeGraphDataAssetIdentity(DataReadback, Error));

	TestTrue(TEXT("all family barriers publish exact distinct evidence"),
		AssetCreateReceipt.GetEvidenceId()
				!= AssetCreateBarrier.GetSettleEvidenceId()
			&& BehaviorApply.GetEvidenceId()
				!= BehaviorBarrier.GetSettleEvidenceId()
			&& QueryApply.GetEvidenceId() != QueryBarrier.GetSettleEvidenceId()
			&& StateApply.GetEvidenceId() != StateBarrier.GetSettleEvidenceId()
			&& PcgApply.GetEvidenceId()
				!= ParameterBarrier.GetSettleEvidenceId()
			&& PcgApply.GetEvidenceId() != PcgBarrier.GetSettleEvidenceId()
			&& ComponentApply.GetEvidenceId()
				!= ComponentBarrier.GetSettleEvidenceId()
			&& DataApply.GetEvidenceId() != DataBarrier.GetSettleEvidenceId());

	Component->UnregisterComponent();
	World->DestroyWorld(false);
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
