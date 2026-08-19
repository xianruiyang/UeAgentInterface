#include "Misc/AutomationTest.h"

#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"

#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentMaterialNiagaraGraphNativeHookTest,
	"UeAgentInterface.DomainAdapters.MaterialNiagaraGraph.E2.Native."
		"OwnedMaterialRootNiagaraStackCompileAndReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentMaterialNiagaraGraphNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;

	FMaterialNiagaraGraphNativeHook Hook;
	FMaterialNiagaraGraphAdapterError Error;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/MaterialNiagaraGraph/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned transient package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	UMaterial* Material = NewObject<UMaterial>(
		Package,
		TEXT("M_GC005_Owned"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("owned Material exists"), Material))
	{
		Package->MarkAsGarbage();
		return false;
	}

	FMaterialNiagaraGraphTopology BeforeMaterial;
	FString BeforeMaterialRevision;
	FString BeforeMaterialEvidence;
	TestTrue(
		TEXT("pre-mutation Material graph is captured from UE state"),
		Hook.TryCaptureMaterialGraph(
			*Material,
			TEXT("material:owned"),
			BeforeMaterial,
			BeforeMaterialRevision,
			BeforeMaterialEvidence,
			Error));

	UMaterialExpression* Expression = nullptr;
	FMaterialNiagaraGraphNativeMutationReceipt MaterialApply;
	const FGuid ExpressionGuid = FGuid::NewGuid();
	TestTrue(
		TEXT("typed Material expression is written through the thin hook"),
		Hook.TryAddMaterialExpression(
			*Material,
			*UMaterialExpressionConstant3Vector::StaticClass(),
			ExpressionGuid,
			120,
			80,
			Expression,
			MaterialApply,
			Error));
	if (!Expression)
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}
	TestTrue(
		TEXT("expression output connects to the exact Material root property"),
		Hook.TryConnectMaterialExpressionToProperty(
			*Expression,
			FString(),
			MP_BaseColor,
			MaterialApply,
			Error));

	FMaterialNiagaraGraphCompileSettleReceipt MaterialSettle;
	TestTrue(
		TEXT("Material compile and settle complete"),
		Hook.TryCompileAndSettleMaterial(
			*Material,
			MaterialSettle,
			Error));
	TestTrue(TEXT("Material compile receipt is complete"),
		MaterialSettle.IsCompilationComplete());
	TestEqual(TEXT("Material compile has no errors"),
		MaterialSettle.GetCompileErrorCount(), 0);

	FMaterialNiagaraGraphTopology MaterialReadback;
	FString MaterialRevision;
	FString MaterialReadbackEvidence;
	TestTrue(
		TEXT("Material graph is independently recaptured from UE"),
		Hook.TryCaptureMaterialGraph(
			*Material,
			TEXT("material:owned"),
			MaterialReadback,
			MaterialRevision,
			MaterialReadbackEvidence,
			Error));
	TestNotEqual(TEXT("Material mutation advances structural revision"),
		BeforeMaterialRevision, MaterialRevision);
	TestTrue(TEXT("Material readback contains expression GUID"),
		MaterialReadback.GetItems().ContainsByPredicate(
			[&](const FMaterialNiagaraGraphItemIdentity& Item)
			{
				return Item.GetNativeGuid()
					== ExpressionGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower);
			}));
	TestTrue(TEXT("Material readback contains exact root connection"),
		MaterialReadback.GetLinks().ContainsByPredicate(
			[](const FMaterialNiagaraGraphLinkIdentity& Link)
			{
				return Link.GetToPinKey().EndsWith(TEXT(":base_color"));
			}));

	const FMaterialNiagaraGraphLifecycleEvidence MaterialLifecycle(
		BeforeMaterialRevision,
		MaterialApply.GetEvidenceId(),
		MaterialSettle.GetSettleEvidenceId(),
		MaterialRevision,
		MaterialReadbackEvidence,
		MaterialSettle.IsCompilationComplete(),
		MaterialSettle.GetCompileErrorCount(),
		0,
		true,
		true);
	TestTrue(
		TEXT("real Material apply/settle/readback evidence is valid"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			MaterialLifecycle,
			Error));

	UMaterialInstanceConstant* MaterialInstance =
		NewObject<UMaterialInstanceConstant>(
			Package,
			TEXT("MI_GC005_Owned"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("owned Material Instance exists"), MaterialInstance))
	{
		Package->MarkAsGarbage();
		return false;
	}
	MaterialInstance->SetParentEditorOnly(Material);
	const FMaterialParameterInfo ScalarInfo(
		FName(TEXT("GC005Scalar")),
		EMaterialParameterAssociation::LayerParameter,
		2);
	TArray<FMaterialNiagaraGraphParameterIdentity> BeforeParameters;
	FString BeforeParameterRevision;
	FString BeforeParameterEvidence;
	TestTrue(
		TEXT("pre-mutation Material Instance parameters are captured"),
		Hook.TryCaptureMaterialInstanceParameters(
			*MaterialInstance,
			BeforeParameters,
			BeforeParameterRevision,
			BeforeParameterEvidence,
			Error));
	FMaterialNiagaraGraphNativeMutationReceipt ParameterApply;
	TestTrue(
		TEXT("scalar parameter mutation preserves full native identity"),
		Hook.TrySetMaterialInstanceScalarParameter(
			*MaterialInstance,
			ScalarInfo,
			0.625f,
			ParameterApply,
			Error));
	TArray<FMaterialNiagaraGraphParameterIdentity> ParameterReadback;
	FString ParameterRevision;
	FString ParameterEvidence;
	TestTrue(
		TEXT("Material Instance parameters are independently recaptured"),
		Hook.TryCaptureMaterialInstanceParameters(
			*MaterialInstance,
			ParameterReadback,
			ParameterRevision,
			ParameterEvidence,
			Error));
	TestNotEqual(TEXT("parameter mutation advances native revision"),
		BeforeParameterRevision, ParameterRevision);
	TestTrue(TEXT("parameter readback keeps name association and index"),
		ParameterReadback.ContainsByPredicate(
			[](const FMaterialNiagaraGraphParameterIdentity& Identity)
			{
				return Identity.GetParameterName() == TEXT("GC005Scalar")
					&& Identity.GetParameterAssociation() == TEXT("layer")
					&& Identity.GetParameterIndex() == 2
					&& Identity.GetParameterType()
						== EMaterialNiagaraGraphParameterType::Scalar;
			}));
	TestTrue(TEXT("native scalar override keeps exact FMaterialParameterInfo"),
		MaterialInstance->ScalarParameterValues.ContainsByPredicate(
			[&](const FScalarParameterValue& Parameter)
			{
				return Parameter.ParameterInfo == ScalarInfo
					&& FMath::IsNearlyEqual(Parameter.ParameterValue, 0.625f);
			}));
	FMaterialNiagaraGraphCompileSettleReceipt InstanceSettle;
	TestTrue(TEXT("Material Instance refresh and settle complete"),
		Hook.TryCompileAndSettleMaterial(
			*MaterialInstance,
			InstanceSettle,
			Error));

	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		Package,
		TEXT("NE_GC005_Owned"),
		RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package,
		TEXT("NS_GC005_Owned"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("owned Niagara emitter exists"), Emitter)
		|| !TestNotNull(TEXT("owned Niagara system exists"), System))
	{
		Package->MarkAsGarbage();
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, true);
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	const FNiagaraEmitterHandle Handle =
		System->AddEmitterHandle(*Emitter, TEXT("GC005Emitter"), FGuid());
	const FGuid EmitterHandleId = Handle.GetId();
	const ENiagaraScriptUsage Usage =
		ENiagaraScriptUsage::ParticleSpawnScript;

	FMaterialNiagaraGraphTopology BeforeStack;
	FString BeforeStackRevision;
	FString BeforeStackEvidence;
	TestTrue(
		TEXT("pre-mutation Niagara stack is captured from UE state"),
		Hook.TryCaptureNiagaraStack(
			*System,
			EmitterHandleId,
			Usage,
			FGuid(),
			BeforeStack,
			BeforeStackRevision,
			BeforeStackEvidence,
			Error));

	UNiagaraNodeFunctionCall* FirstModule = nullptr;
	UNiagaraNodeFunctionCall* SecondModule = nullptr;
	FMaterialNiagaraGraphNativeMutationReceipt NiagaraApply;
	const FNiagaraVariable FirstVariable(
		FNiagaraTypeDefinition::GetFloatDef(),
		FName(TEXT("Particles.GC005Alpha")));
	const FNiagaraVariable SecondVariable(
		FNiagaraTypeDefinition::GetFloatDef(),
		FName(TEXT("Particles.GC005Beta")));
	TestTrue(
		TEXT("first typed assignment module is added to the real stack"),
		Hook.TryAddNiagaraAssignmentModule(
			*System,
			EmitterHandleId,
			Usage,
			FGuid(),
			{FirstVariable},
			INDEX_NONE,
			FirstModule,
			NiagaraApply,
			Error));
	TestTrue(
		TEXT("second typed assignment module is added to the real stack"),
		Hook.TryAddNiagaraAssignmentModule(
			*System,
			EmitterHandleId,
			Usage,
			FGuid(),
			{SecondVariable},
			INDEX_NONE,
			SecondModule,
			NiagaraApply,
			Error));
	if (!FirstModule || !SecondModule)
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}
	int32 NewModuleIndex = INDEX_NONE;
	TestTrue(
		TEXT("native Niagara module ordering is changed by node identity"),
		Hook.TryMoveNiagaraModule(
			*System,
			EmitterHandleId,
			Usage,
			FGuid(),
			*SecondModule,
			0,
			NewModuleIndex,
			NiagaraApply,
			Error));
	TestEqual(TEXT("moved Niagara module reaches requested stack index"),
		NewModuleIndex, 0);

	FMaterialNiagaraGraphCompileSettleReceipt NiagaraSettle;
	TestTrue(
		TEXT("Niagara compile, stack inspection and settle complete"),
		Hook.TryCompileAndSettleNiagara(
			*System,
			NiagaraSettle,
			Error));
	TestTrue(TEXT("Niagara compilation is complete"),
		NiagaraSettle.IsCompilationComplete());
	TestEqual(TEXT("Niagara compile has no errors"),
		NiagaraSettle.GetCompileErrorCount(), 0);
	TestEqual(TEXT("Niagara stack has no blocking issues"),
		NiagaraSettle.GetStackIssueCount(), 0);

	FMaterialNiagaraGraphTopology StackReadback;
	FString StackRevision;
	FString StackReadbackEvidence;
	TestTrue(
		TEXT("Niagara stage and order are independently recaptured"),
		Hook.TryCaptureNiagaraStack(
			*System,
			EmitterHandleId,
			Usage,
			FGuid(),
			StackReadback,
			StackRevision,
			StackReadbackEvidence,
			Error));
	TestNotEqual(TEXT("Niagara stack mutation advances revision"),
		BeforeStackRevision, StackRevision);
	const FString SecondModuleKey = TEXT("module:")
		+ SecondModule->NodeGuid.ToString(
			EGuidFormats::DigitsWithHyphensLower);
	TestTrue(TEXT("Niagara readback contains moved module identity"),
		StackReadback.GetItems().ContainsByPredicate(
			[&](const FMaterialNiagaraGraphItemIdentity& Item)
			{
				return Item.GetItemKey() == SecondModuleKey;
			}));
	TestTrue(TEXT("Niagara readback records moved module at order zero"),
		StackReadback.GetOrdering().ContainsByPredicate(
			[&](const FMaterialNiagaraGraphOrderingIdentity& Ordering)
			{
				return Ordering.GetItemKey() == SecondModuleKey
					&& Ordering.GetPredecessorItemKey().IsEmpty();
			}));

	FMaterialNiagaraGraphTopology RejectedCapture;
	FString RejectedRevision;
	FString RejectedEvidence;
	TestFalse(
		TEXT("missing Niagara stage cannot become an empty successful capture"),
		Hook.TryCaptureNiagaraStack(
			*System,
			FGuid::NewGuid(),
			Usage,
			FGuid(),
			RejectedCapture,
			RejectedRevision,
			RejectedEvidence,
			Error));
	TestEqual(TEXT("missing stage capture error is stable"),
		Error.GetCode(),
		FString(TEXT("material_niagara_graph_stage_not_found")));
	TestTrue(TEXT("failed stack capture does not publish items"),
		RejectedCapture.GetItems().IsEmpty());
	TestTrue(TEXT("failed stack capture does not publish revision"),
		RejectedRevision.IsEmpty());
	TestTrue(TEXT("failed stack capture does not publish evidence"),
		RejectedEvidence.IsEmpty());

	TestFalse(
		TEXT("delegated dynamic-input failure cannot become success"),
		TryValidateMaterialNiagaraGraphCompoundCapture(
			false,
			TEXT("niagara_compound_branch_conflict"),
			TEXT("literal and dynamic branches are both active"),
			Error));
	TestEqual(TEXT("delegated compound error is not translated or hidden"),
		Error.GetCode(),
		FString(TEXT("niagara_compound_branch_conflict")));

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
