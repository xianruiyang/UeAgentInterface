#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Components/BoxComponent.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "PCGGraph.h"
#include "Sound/SoundCue.h"
#include "TextureGraph.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;

	struct FProductionOwnerCase
	{
		FString DomainId;
		UObject* Owner = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadGranularBlueprintCaptureTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E2.ProductionEnvironment."
		"GranularBlueprintCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadGranularBlueprintCaptureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/ContentGraphRead/Blueprint/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("Blueprint package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	FBlueprintUmgGraphNativeHook Hook;
	FBlueprintUmgGraphAdapterError HookError;
	UBlueprint* Blueprint = nullptr;
	if (!TestTrue(
		TEXT("thin hook creates the real Blueprint fixture"),
		Hook.TryCreateBlueprint(
			*Package,
			TEXT("BP_ContentGraphGranular"),
			*AActor::StaticClass(),
			Blueprint,
			HookError))
		|| !Blueprint)
	{
		AddError(HookError.GetCode() + TEXT(":") + HookError.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}
	Blueprint->BlueprintDescription = TEXT("granular Blueprint description");

	UEdGraph* Graph = nullptr;
	FBlueprintUmgGraphNativeMutationReceipt Mutation;
	if (!TestTrue(
		TEXT("thin hook creates the real Blueprint graph"),
		Hook.TryCreateUberGraph(
			*Blueprint,
			TEXT("GranularGraph"),
			Graph,
			Mutation,
			HookError))
		|| !Graph)
	{
		AddError(HookError.GetCode() + TEXT(":") + HookError.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}

	UEdGraphNode* First = nullptr;
	UEdGraphNode* Second = nullptr;
	TestTrue(
		TEXT("first real Blueprint node is created"),
		Hook.TryAddKnotNode(
			*Graph,
			FGuid::NewGuid(),
			120,
			80,
			First,
			Mutation,
			HookError));
	TestTrue(
		TEXT("second real Blueprint node is created"),
		Hook.TryAddKnotNode(
			*Graph,
			FGuid::NewGuid(),
			420,
			160,
			Second,
			Mutation,
			HookError));
	if (!First || !Second)
	{
		AddError(HookError.GetCode() + TEXT(":") + HookError.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}
	First->NodeComment = TEXT("first granular node");
	First->bCommentBubbleVisible = true;
	Second->NodeComment = TEXT("second granular node");
	Second->bCommentBubbleVisible = true;
	UEdGraphPin* FromPin = First->FindPin(TEXT("OutputPin"));
	UEdGraphPin* ToPin = Second->FindPin(TEXT("InputPin"));
	if (!TestNotNull(TEXT("Blueprint source pin exists"), FromPin)
		|| !TestNotNull(TEXT("Blueprint target pin exists"), ToPin)
		|| !TestTrue(
			TEXT("real Blueprint edge is created"),
			Hook.TryConnectPins(*FromPin, *ToPin, Mutation, HookError)))
	{
		AddError(HookError.GetCode() + TEXT(":") + HookError.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}

	USCS_Node* Component = nullptr;
	TestTrue(
		TEXT("real Blueprint SCS component is created"),
		Hook.TryAddSCSComponent(
			*Blueprint,
			*UBoxComponent::StaticClass(),
			TEXT("GranularRoot"),
			FGuid::NewGuid(),
			nullptr,
			Component,
			Mutation,
			HookError));

	FUeAgentContentGraphCapture Capture;
	FUeAgentContentGraphReadError CaptureError;
	if (!TestTrue(
		TEXT("production C4 captures the real Blueprint"),
		FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
			*Blueprint,
			Capture,
			CaptureError)))
	{
		AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail);
		Package->MarkAsGarbage();
		return false;
	}

	TestTrue(
		TEXT("Blueprint graph is a granular structure fact"),
		Capture.Containers.ContainsByPredicate(
			[](const FUeAgentContentGraphContainer& Container)
			{
				return Container.ContainerKind == TEXT("blueprint_graph");
			}));
	TestTrue(
		TEXT("Blueprint nodes are granular item facts"),
		Capture.Items.FilterByPredicate(
			[](const FUeAgentContentGraphItem& Item)
			{
				return Item.ItemKind == TEXT("blueprint_node");
			}).Num() >= 2);
	TestEqual(
		TEXT("Blueprint edge is a granular relation fact"),
		Capture.Relations.FilterByPredicate(
			[](const FUeAgentContentGraphRelation& Relation)
			{
				return Relation.RelationKind == TEXT("blueprint_edge");
			}).Num(),
		1);
	TestEqual(
		TEXT("every authored node exposes one compact comment property"),
		Capture.Properties.FilterByPredicate(
			[First, Second](const FUeAgentContentGraphProperty& Property)
			{
				return Property.FieldId == TEXT("node_comment")
					&& (Property.OwnerId == TEXT("node:")
							+ First->NodeGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower)
						|| Property.OwnerId == TEXT("node:")
							+ Second->NodeGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower));
			}).Num(),
		2);
	TestTrue(
		TEXT("Blueprint description uses its compact external field"),
		Capture.Properties.ContainsByPredicate(
			[Blueprint](const FUeAgentContentGraphProperty& Property)
			{
				return Property.OwnerId == Blueprint->GetPathName()
					&& Property.FieldId == TEXT("blueprint_description");
			}));
	TestTrue(
		TEXT("Blueprint production capture does not dump every subobject"),
		Capture.Items.Num() <= 16);
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadGranularNiagaraCaptureTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E2.ProductionEnvironment."
		"GranularNiagaraCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadGranularNiagaraCaptureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/ContentGraphRead/Niagara/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("Niagara package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		Package,
		TEXT("NE_ContentGraphGranular"),
		RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package,
		TEXT("NS_ContentGraphGranular"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("real Niagara emitter exists"), Emitter)
		|| !TestNotNull(TEXT("real Niagara system exists"), System))
	{
		Package->MarkAsGarbage();
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, true);
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	System->SetWarmupTime(1.25f);
	const FNiagaraEmitterHandle Handle = System->AddEmitterHandle(
		*Emitter,
		TEXT("GranularEmitter"),
		FGuid());

	FMaterialNiagaraGraphNativeHook Hook;
	FMaterialNiagaraGraphAdapterError HookError;
	FMaterialNiagaraGraphNativeMutationReceipt Mutation;
	const ENiagaraScriptUsage Usage =
		ENiagaraScriptUsage::ParticleSpawnScript;
	for (const TCHAR* Name : {
		TEXT("Particles.GranularAlpha"),
		TEXT("Particles.GranularBeta")})
	{
		UNiagaraNodeFunctionCall* Module = nullptr;
		const FNiagaraVariable Variable(
			FNiagaraTypeDefinition::GetFloatDef(),
			FName(Name));
		if (!TestTrue(
			TEXT("real Niagara assignment module is created"),
			Hook.TryAddNiagaraAssignmentModule(
				*System,
				Handle.GetId(),
				Usage,
				FGuid(),
				{Variable},
				INDEX_NONE,
				Module,
				Mutation,
				HookError))
			|| !Module)
		{
			AddError(HookError.GetCode() + TEXT(":") + HookError.GetDetail());
			Package->MarkAsGarbage();
			return false;
		}
	}

	FUeAgentContentGraphCapture Capture;
	FUeAgentContentGraphReadError CaptureError;
	if (!TestTrue(
		TEXT("production C4 captures the real Niagara System"),
		FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
			*System,
			Capture,
			CaptureError)))
	{
		AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail);
		Package->MarkAsGarbage();
		return false;
	}

	TestTrue(
		TEXT("Niagara stage is a granular stack structure fact"),
		Capture.Containers.ContainsByPredicate(
			[](const FUeAgentContentGraphContainer& Container)
			{
				return Container.ContainerKind == TEXT("niagara_stack");
			}));
	TestTrue(
		TEXT("Niagara emitter is a granular hierarchy item"),
		Capture.Items.ContainsByPredicate(
			[](const FUeAgentContentGraphItem& Item)
			{
				return Item.ItemKind == TEXT("niagara_emitter");
			}));
	TestTrue(
		TEXT("Niagara modules are granular stack items"),
		Capture.Items.FilterByPredicate(
			[](const FUeAgentContentGraphItem& Item)
			{
				return Item.ItemKind == TEXT("niagara_module");
			}).Num() >= 2);
	TestTrue(
		TEXT("Niagara stack order is captured explicitly"),
		Capture.Relations.FilterByPredicate(
			[](const FUeAgentContentGraphRelation& Relation)
			{
				return Relation.RelationKind == TEXT("niagara_order");
			}).Num() >= 2);
	TestTrue(
		TEXT("every assignment module exposes compact assignment targets"),
		Capture.Properties.FilterByPredicate(
			[](const FUeAgentContentGraphProperty& Property)
			{
				return Property.FieldId == TEXT("assignment_targets");
			}).Num() >= 2);
	TestTrue(
		TEXT("Niagara warmup uses its compact external field"),
		Capture.Properties.ContainsByPredicate(
			[System](const FUeAgentContentGraphProperty& Property)
			{
				return Property.OwnerId == System->GetPathName()
					&& Property.FieldId == TEXT("warmup_time");
			}));
	TestTrue(
		TEXT("Niagara production capture does not dump every subobject"),
		Capture.Items.Num() <= 32);
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadProductionEnvironmentTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E2.ProductionEnvironment."
		"SixRealUObjectOwners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadProductionEnvironmentTest::RunTest(
	const FString& Parameters)
{
	UPackage* Package = CreatePackage(
		TEXT("/Engine/Transient/RE008ProductionOwners"));
	TestNotNull(TEXT("transient package exists"), Package);
	if (!Package)
	{
		return false;
	}
	UTextureGraph* TextureGraph =
		NewObject<UTextureGraph>(Package, TEXT("TG_Owner"));
	if (TextureGraph)
	{
		TextureGraph->Construct(TEXT("TG_Owner"));
	}
	const TArray<FProductionOwnerCase> Cases = {
		{TEXT("blueprint"), NewObject<UBlueprint>(Package, TEXT("BP_Owner"))},
		{TEXT("material"), NewObject<UMaterial>(Package, TEXT("M_Owner"))},
		{TEXT("niagara"), NewObject<UNiagaraSystem>(Package, TEXT("NS_Owner"))},
		{TEXT("pcg"), NewObject<UPCGGraph>(Package, TEXT("PCG_Owner"))},
		{TEXT("animation"), NewObject<UAnimBlueprint>(Package, TEXT("ABP_Owner"))},
		{TEXT("audio"), NewObject<USoundCue>(Package, TEXT("SC_Owner"))},
		{TEXT("texture_graph"), TextureGraph},
	};
	const bool bDirtyBeforeCapture = Package->IsDirty();

	for (const FProductionOwnerCase& Case : Cases)
	{
		TestNotNull(*Case.DomainId, Case.Owner);
		if (!Case.Owner)
		{
			continue;
		}
		FUeAgentContentGraphCapture Capture;
		FUeAgentContentGraphReadError Error;
		TestTrue(
			*FString::Printf(
				TEXT("%s real owner is captured"),
				*Case.DomainId),
			FUeAgentContentGraphReadProductionEnvironment::
				TryCaptureOwner(
					*Case.Owner,
					Capture,
					Error));
		TestEqual(
			*FString::Printf(
				TEXT("%s domain is exact"),
				*Case.DomainId),
			Capture.DomainId,
			Case.DomainId);
		TestTrue(
			*FString::Printf(
				TEXT("%s keeps the actual owner"),
				*Case.DomainId),
			Capture.Owner == Case.Owner);
		TestTrue(
			*FString::Printf(
				TEXT("%s has a stable revision"),
				*Case.DomainId),
			!Capture.Revision.IsEmpty());
		TestTrue(
			*FString::Printf(
				TEXT("%s has an owner-backed root container"),
				*Case.DomainId),
			Capture.Containers.ContainsByPredicate(
				[Case](const FUeAgentContentGraphContainer& Container)
				{
					return Container.OwnerId
						== Case.Owner->GetPathName();
				}));
		if (Case.DomainId == TEXT("texture_graph"))
		{
			TestTrue(
				TEXT("TextureGraph exposes native nodes"),
				Capture.Items.ContainsByPredicate(
					[](const FUeAgentContentGraphItem& Item)
					{
						return Item.ItemKind == TEXT("texture_graph_node");
					}));
			TestTrue(
				TEXT("TextureGraph exposes native pins"),
				Capture.Items.ContainsByPredicate(
					[](const FUeAgentContentGraphItem& Item)
					{
						return Item.ItemKind.StartsWith(
							TEXT("texture_graph_pin_"));
					}));
			TestTrue(
				TEXT("TextureGraph exposes node/pin topology"),
				Capture.Relations.ContainsByPredicate(
					[](const FUeAgentContentGraphRelation& Relation)
					{
						return Relation.RelationKind
							== TEXT("texture_graph_node_pin");
					}));
			TestTrue(
				TEXT("TextureGraph exposes pin values and output state"),
				Capture.Properties.ContainsByPredicate(
					[](const FUeAgentContentGraphProperty& Property)
					{
						return Property.FieldId == TEXT("value")
							&& Property.SemanticIdentity.IsExplicit();
					}));
		}
	}
	TestEqual(
		TEXT("capture does not change the package dirty state"),
		Package->IsDirty(),
		bDirtyBeforeCapture);
	return true;
}
