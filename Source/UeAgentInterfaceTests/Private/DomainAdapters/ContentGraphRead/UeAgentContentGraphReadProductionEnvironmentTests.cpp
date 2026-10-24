#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Components/BoxComponent.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
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
	FUeAgentContentGraphReadMaterialFunctionStateTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E2.ProductionEnvironment.MaterialFunctionState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadMaterialFunctionStateTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/ContentGraphRead/Material/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_FunctionReadback"));
	ON_SCOPE_EXIT { Package->SetDirtyFlag(false); Package->MarkAsGarbage(); };
	UeAgentPaletteSearch::FSearchParams Search;
	Search.CatalogType = TEXT("material_node");
	Search.AssetPath = Material->GetPathName();
	Search.GraphName = TEXT("MaterialGraph");
	TArray<UeAgentPaletteSearch::FCandidate> Candidates;
	TArray<FString> Warnings;
	FString Error;
	if (!UeAgentPaletteSearch::CollectMaterialCandidates(Search, Candidates, Warnings, Error))
	{ AddError(Error); return false; }
	const auto* Candidate = Candidates.FindByPredicate([](const auto& Value) {
		return Value.Kind == TEXT("material_function_call") && Value.Name == TEXT("Smooth Step") && Value.bSupportedInContext;
	});
	if (!TestNotNull(TEXT("native Smooth Step function candidate exists"), Candidate)) return false;
	UMaterialFunctionInterface* Function = LoadObject<UMaterialFunctionInterface>(nullptr,
		*Candidate->AuthoringSelectors.FindRef(TEXT("function_path")));
	if (!TestNotNull(TEXT("native candidate resolves its real function asset"), Function)) return false;
	TArray<UMaterialExpressionMaterialFunctionCall*> Calls;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), Index * 200, 100));
		if (!TestNotNull(TEXT("real function call exists"), Call)
			|| !TestTrue(TEXT("native function assignment succeeds"), Call->SetMaterialFunction(Function))) return false;
		Calls.Add(Call);
	}
	Package->SetDirtyFlag(false);
	UeAgentMaterialAuthoring::FMaterialAuthoringCapture Authoring;
	if (!UeAgentMaterialAuthoring::TryCaptureMaterialAuthoringState(*Material, TEXT("llm_authoring"), Authoring, Error))
	{ AddError(Error); return false; }
	TMap<FString, FString> ExportNames;
	for (const auto& Unit : Authoring.Model->GetUnits())
	{
		if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState) continue;
		const auto* Nodes = Unit.GetValues().Find(TEXT("nodes"));
		if (!Nodes || (*Nodes)->GetKind() != EUeAgentValueKind::Object) continue;
		for (const auto& Node : (*Nodes)->GetObjectItems())
		{
			TSharedPtr<FJsonValue> Json;
			if (!CanonicalJson::TryToJsonValue(*Node.Value, Json, Error)) { AddError(Error); return false; }
			ExportNames.Add(Json->AsObject()->GetStringField(TEXT("id")), Node.Key);
		}
	}
	FUeAgentContentGraphCapture Capture;
	FUeAgentContentGraphReadError CaptureError;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Material, Capture, CaptureError))
	{ AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail); return false; }
	for (const auto& Item : Capture.Items)
	{
		if (!Item.SemanticIdentity.IsExplicit()) continue;
		if (!TestTrue(TEXT("every explicit Material item has object state"), Item.State.IsValid()
			&& Item.State->GetKind() == EUeAgentValueKind::Object)) return false;
		TestTrue(TEXT("every explicit Material item satisfies the canonical identity contract"),
			FUeAgentCanonicalIdentity(Item.SemanticIdentity.SemanticId, Capture.DomainId,
				Item.OwnerId, Item.SemanticIdentity.Selectors, Item.SemanticIdentity.ObjectRole, TEXT("test")).IsValid());
	}
	for (auto* Call : Calls)
	{
		const FString Guid = Call->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		const auto* Item = Capture.Items.FindByPredicate([&](const auto& Value) {
			return Value.ItemKind == TEXT("material_expression") && Value.SemanticIdentity.Selectors.FindRef(TEXT("native_guid")) == Guid;
		});
		if (!TestNotNull(TEXT("native function call has captured state"), Item)) return false;
		TSharedPtr<FJsonValue> Json;
		if (!CanonicalJson::TryToJsonValue(*Item->State, Json, Error)) { AddError(Error); return false; }
		const auto State = Json->AsObject();
		TestEqual(TEXT("repeated function calls keep exact ordinary export aliases"), State->GetStringField(TEXT("name")), ExportNames.FindRef(Guid));
		TestEqual(TEXT("captured function reference is the assigned native asset"), State->GetStringField(TEXT("material_function")), Function->GetPathName());
		TestFalse(TEXT("readback does not expose private candidate identity"), State->HasField(TEXT("candidate_context")));
		const auto& Pins = State->GetObjectField(TEXT("pin_interface"))->GetArrayField(TEXT("pins"));
		TestEqual(TEXT("pin count comes from the actual function interface"), Pins.Num(), Call->FunctionInputs.Num() + Call->FunctionOutputs.Num());
		for (const auto& Input : Call->FunctionInputs)
		{
			if (!TestNotNull(TEXT("native function input is resolved"), Input.ExpressionInput.Get())) return false;
			const FString Name = Input.ExpressionInput->InputName.ToString();
			TestTrue(TEXT("readback exposes each actual function input name"), Pins.ContainsByPredicate([&](const auto& Pin) {
				return Pin->AsObject()->GetStringField(TEXT("direction")) == TEXT("input") && Pin->AsObject()->GetStringField(TEXT("name")) == Name;
			}));
		}
	}
	TestFalse(TEXT("read-only capture leaves the material clean"), Package->IsDirty());
	return !HasAnyErrors();
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
				return Relation.RelationKind == TEXT("connection");
			}).Num(),
		1);
	TestTrue(TEXT("native Blueprint edge captures precise pin identity independently of display state"),
		Capture.Relations.ContainsByPredicate([](const FUeAgentContentGraphRelation& Relation) {
			return Relation.RelationKind == TEXT("connection")
				&& !Relation.SourceEndpoint.Identity.IsEmpty() && !Relation.TargetEndpoint.Identity.IsEmpty()
				&& !Relation.SourceEndpoint.PublicName.IsEmpty() && !Relation.TargetEndpoint.PublicName.IsEmpty()
				&& Relation.SourceEndpoint.Identity != Relation.SourceItemId
				&& Relation.TargetEndpoint.Identity != Relation.TargetItemId;
		}));
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
				return Relation.RelationKind == TEXT("order");
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
		const bool bCaptured = TestTrue(
			*FString::Printf(
				TEXT("%s real owner is captured"),
				*Case.DomainId),
			FUeAgentContentGraphReadProductionEnvironment::
				TryCaptureOwner(
					*Case.Owner,
					Capture,
					Error));
		if (!bCaptured)
		{
			AddError(FString::Printf(TEXT("capture failed domain=%s code=%s source=%s detail=%s"),
				*Case.DomainId, *Error.Code, *Error.SubjectId, *Error.Detail));
			continue;
		}
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
							== TEXT("membership");
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
