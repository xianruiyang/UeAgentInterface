#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "ControlRigBlueprint.h"
#include "ControlRigGizmoLibrary.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Contract;

	class FActiveCancellation final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FRigIkAssetSet final
	{
	public:
		FRigIkAssetSet()
			: Suffix(FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
		}

		FString Add(const FString& Stem)
		{
			const FString PackageName = TEXT("/Game/UAI/Tests/GraphCompound/")
				+ Stem + TEXT("_") + Suffix;
			PackageNames.Add(PackageName);
			return PackageName;
		}

		static FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool Unload(const FString& PackageName) const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && !FindPackage(nullptr, *PackageName);
		}

		bool Cleanup()
		{
			bool bClean = true;
			for (const FString& PackageName : PackageNames)
			{
				if (UPackage* const Package = FindPackage(nullptr, *PackageName))
				{
					if (UObject* const Asset = FindObject<UObject>(
						Package, *FPackageName::GetLongPackageAssetName(PackageName)))
					{
						FAssetRegistryModule::AssetDeleted(Asset);
					}
				}
				bClean = Unload(PackageName) && bClean;
				const FString Filename = FPackageName::LongPackageNameToFilename(
					PackageName, FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					bClean = IFileManager::Get().Delete(
						*Filename, false, true) && bClean;
				}
				bClean = !IFileManager::Get().FileExists(*Filename)
					&& !FindPackage(nullptr, *PackageName) && bClean;
			}
			return bClean;
		}

		~FRigIkAssetSet()
		{
			Cleanup();
		}

	private:
		FString Suffix;
		TArray<FString> PackageNames;
	};

	FUeAgentCallerContext Caller(const FString& CapabilityId)
	{
		return FUeAgentCallerContext(
			TEXT("test.agent"),
			{TEXT("animation_rig_graph.read"),
				TEXT("animation_rig_graph.write")},
			TEXT("correlation:w03:") + CapabilityId);
	}

	bool TryJson(const TSharedRef<FJsonObject>& Object, FString& OutJson)
	{
		FString Error;
		return Json::TryCanonicalize(Object, OutJson, Error);
	}

	FUeAgentOperationResult Submit(
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const TSharedRef<FJsonObject>& Object,
		const FString& Suffix)
	{
		FString Payload;
		check(TryJson(Object, Payload));
		const FPcgAnimationGraphRouteProfile* const Profile =
			FindPcgAnimationGraphRouteProfile(CapabilityId);
		check(Profile);
		return Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w03:") + Suffix,
				TEXT("idempotency:w03:") + Suffix,
				FUeAgentContractValue(Payload)),
			Caller(CapabilityId), MakeShared<FActiveCancellation>());
	}

	bool TryOperationError(
		const FUeAgentOperationResult& Result,
		FString& OutCode,
		FString& OutDetail)
	{
		OutCode.Reset();
		OutDetail.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return Root->TryGetObjectField(TEXT("operation_error"), Error)
			&& Error && Error->IsValid()
			&& (*Error)->TryGetStringField(TEXT("code"), OutCode)
			&& ((*Error)->TryGetStringField(TEXT("detail"), OutDetail) || true);
	}

	bool TestSuccess(
		FAutomationTestBase& Test,
		const FUeAgentOperationResult& Result,
		const FString& ExpectedTarget,
		const FString& Label)
	{
		FString ErrorCode;
		FString ErrorDetail;
		if (TryOperationError(Result, ErrorCode, ErrorDetail))
		{
			Test.AddError(Label + TEXT(": ") + ErrorCode + TEXT(":") + ErrorDetail);
			return false;
		}
		if (!Test.TestTrue(*Label, Result.IsValid()))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		FString TargetRef;
		FString TerminalStatus;
		bool bMutationApplied = false;
		bool bCompileSettled = false;
		bool bSaved = false;
		bool bReloaded = false;
		const bool bExact = FJsonSerializer::Deserialize(Reader, Root)
			&& Root.IsValid()
			&& Root->TryGetStringField(TEXT("target_ref"), TargetRef)
			&& TargetRef == ExpectedTarget
			&& Root->TryGetStringField(TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& Root->TryGetBoolField(TEXT("mutation_applied"), bMutationApplied)
			&& bMutationApplied
			&& Root->TryGetBoolField(TEXT("compile_settled"), bCompileSettled)
			&& bCompileSettled
			&& Root->TryGetBoolField(TEXT("saved"), bSaved) && bSaved
			&& Root->TryGetBoolField(TEXT("reloaded"), bReloaded) && bReloaded
			&& !Root->HasField(TEXT("persistence_receipt_id"))
			&& !Root->HasField(TEXT("independent_readback_evidence_id"));
		return Test.TestTrue(
			*(Label + TEXT(" returns exact target and short lifecycle state")),
			bExact);
	}

	TSharedRef<FJsonObject> AssetCreate(const FString& PackageName)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("asset_name"),
			FPackageName::GetLongPackageAssetName(PackageName));
		Object->SetStringField(TEXT("package_path"),
			FPackageName::GetLongPackagePath(PackageName));
		return Object;
	}

	TSharedRef<FJsonObject> Edit(
		const FString& Identity,
		const FString& ElementKind,
		const FString& Intent,
		const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("identity"), Identity);
		Object->SetStringField(TEXT("element_kind"), ElementKind);
		Object->SetStringField(TEXT("intent"), Intent);
		if (Value.IsValid())
		{
			Object->SetField(TEXT("value"), Value);
		}
		return Object;
	}

	TSharedRef<FJsonObject> IkDefinition(
		const FString& TargetRef,
		const FName RootBone,
		const FName EndBone)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("target_ref"), TargetRef);
		Object->SetStringField(TEXT("retarget_root"), RootBone.ToString());
		const TSharedRef<FJsonObject> Goal = MakeShared<FJsonObject>();
		Goal->SetStringField(TEXT("name"), TEXT("W03Goal"));
		Goal->SetStringField(TEXT("bone_name"), EndBone.ToString());
		Goal->SetStringField(TEXT("intent"), TEXT("create"));
		Object->SetArrayField(TEXT("goals"), {MakeShared<FJsonValueObject>(Goal)});
		const TSharedRef<FJsonObject> Solver = MakeShared<FJsonObject>();
		Solver->SetStringField(TEXT("solver_ref"), TEXT("solver:W03FBIK"));
		Solver->SetStringField(
			TEXT("solver_class_ref"), TEXT("/Script/IKRig.IKRigFullBodyIKSolver"));
		Solver->SetNumberField(TEXT("order_index"), 0);
		Solver->SetStringField(TEXT("intent"), TEXT("create"));
		Object->SetArrayField(TEXT("solvers"), {MakeShared<FJsonValueObject>(Solver)});
		const TSharedRef<FJsonObject> Chain = MakeShared<FJsonObject>();
		Chain->SetStringField(TEXT("name"), TEXT("W03Chain"));
		Chain->SetStringField(TEXT("start_bone"), RootBone.ToString());
		Chain->SetStringField(TEXT("end_bone"), EndBone.ToString());
		Chain->SetStringField(TEXT("goal_name"), TEXT("W03Goal"));
		Chain->SetStringField(TEXT("intent"), TEXT("create"));
		Object->SetArrayField(TEXT("chains"), {MakeShared<FJsonValueObject>(Chain)});
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundRigIkProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AnimationRig."
		"RigIkProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundRigIkProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FRigIkAssetSet Assets;
	const TStrongObjectPtr<USkeletalMesh> MeshAnchor(
		LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube")));
	const TStrongObjectPtr<USkeleton> SkeletonAnchor(
		LoadObject<USkeleton>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton."
				"SkeletalCube_Skeleton")));
	const TStrongObjectPtr<UStaticMesh> ShapeMeshAnchor(
		LoadObject<UStaticMesh>(nullptr,
			TEXT("/Engine/BasicShapes/Cube.Cube")));
	const TStrongObjectPtr<UMaterial> ShapeMaterialAnchor(
		LoadObject<UMaterial>(nullptr,
			TEXT("/Engine/BasicShapes/BasicShapeMaterial."
				"BasicShapeMaterial")));
	USkeletalMesh* const Mesh = MeshAnchor.Get();
	USkeleton* const Skeleton = SkeletonAnchor.Get();
	if (!TestNotNull(TEXT("skeletal mesh fixture exists"), Mesh)
		|| !TestNotNull(TEXT("skeleton fixture exists"), Skeleton)
		|| !TestNotNull(TEXT("shape mesh fixture exists"), ShapeMeshAnchor.Get())
		|| !TestNotNull(TEXT("shape material fixture exists"), ShapeMaterialAnchor.Get())
		|| !TestTrue(TEXT("skeletal mesh exposes a chain"),
			Mesh && Mesh->GetRefSkeleton().GetNum() >= 2))
	{
		return false;
	}
	const FName RootBone = Mesh->GetRefSkeleton().GetBoneName(0);
	const FName EndBone = Mesh->GetRefSkeleton().GetBoneName(
		Mesh->GetRefSkeleton().GetNum() - 1);

	FGraphCompoundRuntimeConfig RuntimeConfig;

	FGraphCompoundAnimationProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TestTrue(TEXT("Animation/Rig production runtime builds"),
			TryCreateAnimationGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
		|| !TestNotNull(TEXT("Animation/Rig runtime exists"), Runtime.Get()))
	{
		AddError(RuntimeError.Code + TEXT(":") + RuntimeError.Detail);
		return false;
	}

	const FString RejectedPackage = Assets.Add(TEXT("CR_W03_Rejected"));
	TSharedRef<FJsonObject> Rejected = AssetCreate(RejectedPackage);
	Rejected->SetStringField(TEXT("skeleton_ref"), Skeleton->GetPathName());
	Rejected->SetBoolField(TEXT("unexpected"), true);
	const FUeAgentOperationResult RejectedResult = Submit(
		*Runtime, TEXT("animation.control_rig.asset.create"),
		Rejected, TEXT("rejected"));
	FString ErrorCode;
	FString ErrorDetail;
	TestTrue(TEXT("unknown field returns operation_error"),
		TryOperationError(RejectedResult, ErrorCode, ErrorDetail));
	TestEqual(TEXT("unknown field fails at public schema"), ErrorCode,
		FString(TEXT("contract_input_schema_mismatch")));
	TestFalse(TEXT("rejected request leaves no package file"),
		FPaths::FileExists(FPackageName::LongPackageNameToFilename(
			RejectedPackage, FPackageName::GetAssetPackageExtension())));

	const FString ControlRigPackage = Assets.Add(TEXT("CR_W03"));
	const FString ControlRigObject = FRigIkAssetSet::ObjectPath(ControlRigPackage);
	TSharedRef<FJsonObject> ControlRigCreate = AssetCreate(ControlRigPackage);
	ControlRigCreate->SetStringField(TEXT("skeleton_ref"), Skeleton->GetPathName());
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.control_rig.asset.create"),
			ControlRigCreate, TEXT("control-rig-create")), ControlRigPackage,
		TEXT("ControlRig create succeeds")))
	{
		return false;
	}
	UControlRigBlueprint* ControlRig = LoadObject<UControlRigBlueprint>(
		nullptr, *ControlRigObject);
	TestNotNull(TEXT("ControlRig reloads"), ControlRig);
	if (ControlRig)
	{
		const FSoftObjectProperty* SourceHierarchyProperty =
			FindFProperty<FSoftObjectProperty>(
				UControlRigBlueprint::StaticClass(), TEXT("SourceHierarchyImport"));
		const FSoftObjectPtr* SourceHierarchy = SourceHierarchyProperty
			? SourceHierarchyProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(
				ControlRig) : nullptr;
		TestEqual(TEXT("ControlRig keeps exact source skeleton"),
			SourceHierarchy
				? SourceHierarchy->ToSoftObjectPath().ToString() : FString(),
			Skeleton->GetPathName());
		TestTrue(TEXT("ControlRig imported native root"),
			ControlRig->GetHierarchy()->Contains(
				FRigElementKey(RootBone, ERigElementType::Bone)));
	}
	ControlRig = nullptr;
	Assets.Unload(ControlRigPackage);

	const TSharedRef<FJsonObject> RigGraph = MakeShared<FJsonObject>();
	RigGraph->SetStringField(TEXT("target_ref"), ControlRigObject);
	const TSharedRef<FJsonObject> ControlValue = MakeShared<FJsonObject>();
	ControlValue->SetStringField(TEXT("native_key"), TEXT("Control:W03Control"));
	ControlValue->SetStringField(TEXT("parent_native_key"),
		TEXT("Bone:") + RootBone.ToString());
	ControlValue->SetStringField(TEXT("control_type_ref"), TEXT("EulerTransform"));
	RigGraph->SetArrayField(TEXT("edits"), {MakeShared<FJsonValueObject>(Edit(
		TEXT("rig_element:control:W03Control"), TEXT("hierarchy"),
		TEXT("create"), MakeShared<FJsonValueObject>(ControlValue)))});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.control_rig.hierarchy_graph.apply"),
			RigGraph, TEXT("control-rig-graph")), ControlRigObject,
		TEXT("ControlRig hierarchy graph succeeds")))
	{
		return false;
	}
	ControlRig = LoadObject<UControlRigBlueprint>(nullptr, *ControlRigObject);
	const FRigElementKey ControlKey(TEXT("W03Control"), ERigElementType::Control);
	TestTrue(TEXT("ControlRig control persists"),
		ControlRig && ControlRig->GetHierarchy()->Contains(ControlKey));
	if (ControlRig && ControlRig->GetHierarchy()->Contains(ControlKey))
	{
		const FRigControlElement* Element =
			ControlRig->GetHierarchy()->Find<FRigControlElement>(ControlKey);
		TestTrue(TEXT("ControlRig control type persists"), Element
			&& Element->Settings.ControlType == ERigControlType::EulerTransform);
		TestEqual(TEXT("ControlRig parent persists"),
			ControlRig->GetHierarchy()->GetFirstParent(ControlKey),
			FRigElementKey(RootBone, ERigElementType::Bone));
	}
	ControlRig = nullptr;
	Assets.Unload(ControlRigPackage);

	const FString ShapePackage = Assets.Add(TEXT("CRSL_W03"));
	const FString ShapeObject = FRigIkAssetSet::ObjectPath(ShapePackage);
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.control_rig.shape_library.asset.create"),
			AssetCreate(ShapePackage), TEXT("shape-create")), ShapePackage,
		TEXT("ShapeLibrary create succeeds")))
	{
		return false;
	}
	const TSharedRef<FJsonObject> ShapeApply = MakeShared<FJsonObject>();
	ShapeApply->SetStringField(TEXT("target_ref"), ShapeObject);
	const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
	Shape->SetStringField(TEXT("name"), TEXT("W03Shape"));
	Shape->SetStringField(TEXT("mesh_ref"), ShapeMeshAnchor->GetPathName());
	Shape->SetStringField(TEXT("material_ref"), ShapeMaterialAnchor->GetPathName());
	Shape->SetNumberField(TEXT("order_index"), 0);
	Shape->SetStringField(TEXT("intent"), TEXT("create"));
	ShapeApply->SetArrayField(TEXT("shapes"), {MakeShared<FJsonValueObject>(Shape)});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.control_rig.shape_library.apply"),
			ShapeApply, TEXT("shape-apply")), ShapeObject,
		TEXT("ShapeLibrary apply succeeds")))
	{
		return false;
	}
	UControlRigShapeLibrary* ShapeLibrary =
		LoadObject<UControlRigShapeLibrary>(nullptr, *ShapeObject);
	TestNotNull(TEXT("ShapeLibrary reloads"), ShapeLibrary);
	if (ShapeLibrary)
	{
		TestEqual(TEXT("ShapeLibrary exact material persists"),
			ShapeLibrary->DefaultMaterial.ToSoftObjectPath().ToString(),
			ShapeMaterialAnchor->GetPathName());
		TestTrue(TEXT("ShapeLibrary exact ordered shape persists"),
			ShapeLibrary->Shapes.Num() == 1
			&& ShapeLibrary->Shapes[0].ShapeName == TEXT("W03Shape")
			&& ShapeLibrary->Shapes[0].StaticMesh.ToSoftObjectPath().ToString()
				== ShapeMeshAnchor->GetPathName());
	}
	ShapeLibrary = nullptr;
	Assets.Unload(ShapePackage);

	const FString SourceRigPackage = Assets.Add(TEXT("IKR_W03_Source"));
	const FString TargetRigPackage = Assets.Add(TEXT("IKR_W03_Target"));
	const FString SourceRigObject = FRigIkAssetSet::ObjectPath(SourceRigPackage);
	const FString TargetRigObject = FRigIkAssetSet::ObjectPath(TargetRigPackage);
	for (const TPair<FString, FString>& RigPackage : {
		TPair<FString, FString>(SourceRigPackage, TEXT("source")),
		TPair<FString, FString>(TargetRigPackage, TEXT("target"))})
	{
		TSharedRef<FJsonObject> Create = AssetCreate(RigPackage.Key);
		Create->SetStringField(TEXT("skeletal_mesh_ref"), Mesh->GetPathName());
		if (!TestSuccess(*this, Submit(
				*Runtime, TEXT("animation.ik_rig.asset.create"), Create,
				TEXT("ik-rig-create-") + RigPackage.Value), RigPackage.Key,
			TEXT("IKRig create succeeds: ") + RigPackage.Value))
		{
			return false;
		}
	}
	for (const TPair<FString, FString>& RigObject : {
		TPair<FString, FString>(SourceRigObject, TEXT("source")),
		TPair<FString, FString>(TargetRigObject, TEXT("target"))})
	{
		if (!TestSuccess(*this, Submit(
				*Runtime, TEXT("animation.ik_rig.definition.apply"),
				IkDefinition(RigObject.Key, RootBone, EndBone),
				TEXT("ik-rig-definition-") + RigObject.Value), RigObject.Key,
			TEXT("IKRig definition succeeds: ") + RigObject.Value))
		{
			return false;
		}
	}
	const TSharedRef<FJsonObject> AutoGenerate = MakeShared<FJsonObject>();
	AutoGenerate->SetStringField(TEXT("target_ref"), SourceRigObject);
	AutoGenerate->SetStringField(TEXT("skeletal_mesh_ref"), Mesh->GetPathName());
	AutoGenerate->SetStringField(TEXT("preset"), TEXT("custom"));
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.ik_rig.definition.auto_generate"),
			AutoGenerate, TEXT("ik-rig-auto-generate")), SourceRigObject,
		TEXT("IKRig custom auto-generate succeeds")))
	{
		return false;
	}

	for (const FString& RigObject : {SourceRigObject, TargetRigObject})
	{
		UIKRigDefinition* Rig = LoadObject<UIKRigDefinition>(nullptr, *RigObject);
		UIKRigController* Controller = UIKRigController::GetController(Rig);
		TestNotNull(TEXT("IKRig reloads"), Rig);
		TestNotNull(TEXT("IKRig controller reloads"), Controller);
		if (Controller)
		{
			TestEqual(TEXT("IKRig exact mesh persists"),
				Controller->GetSkeletalMesh(), Mesh);
			TestEqual(TEXT("IKRig retarget root persists"),
				Controller->GetRetargetRoot(), RootBone);
			TestNotNull(TEXT("IKRig goal persists"),
				Controller->GetGoal(TEXT("W03Goal")));
			TestTrue(TEXT("IKRig solver order/type persists"),
				Controller->GetNumSolvers() == 1
				&& Controller->GetSolverStructAtIndex(0)
				&& Controller->GetSolverStructAtIndex(0)->GetScriptStruct()
				&& Controller->GetSolverStructAtIndex(0)->GetScriptStruct()->GetPathName()
					== TEXT("/Script/IKRig.IKRigFullBodyIKSolver"));
			TestTrue(TEXT("IKRig exact chain persists"),
				Controller->GetRetargetChains().ContainsByPredicate(
					[RootBone, EndBone](const FBoneChain& Chain)
					{
						return Chain.ChainName == TEXT("W03Chain")
							&& Chain.StartBone.BoneName == RootBone
							&& Chain.EndBone.BoneName == EndBone
							&& Chain.IKGoalName == TEXT("W03Goal");
					}));
		}
		Assets.Unload(FPackageName::ObjectPathToPackageName(RigObject));
	}

	const FString RetargetPackage = Assets.Add(TEXT("RTG_W03"));
	const FString RetargetObject = FRigIkAssetSet::ObjectPath(RetargetPackage);
	TSharedRef<FJsonObject> RetargetCreate = AssetCreate(RetargetPackage);
	RetargetCreate->SetStringField(TEXT("source_ik_rig_ref"), SourceRigObject);
	RetargetCreate->SetStringField(TEXT("target_ik_rig_ref"), TargetRigObject);
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.ik_retargeter.asset.create"),
			RetargetCreate, TEXT("retarget-create")), RetargetPackage,
		TEXT("IKRetargeter create succeeds")))
	{
		return false;
	}
	const TSharedRef<FJsonObject> RetargetDefinition = MakeShared<FJsonObject>();
	RetargetDefinition->SetStringField(TEXT("target_ref"), RetargetObject);
	RetargetDefinition->SetStringField(TEXT("source_ik_rig_ref"), SourceRigObject);
	RetargetDefinition->SetStringField(TEXT("target_ik_rig_ref"), TargetRigObject);
	const TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
	Mapping->SetStringField(TEXT("target_chain"), TEXT("W03Chain"));
	Mapping->SetStringField(TEXT("source_chain"), TEXT("W03Chain"));
	Mapping->SetStringField(
		TEXT("operation_ref"), TEXT("/Script/IKRig.IKRetargetFKChainsOp"));
	Mapping->SetNumberField(TEXT("order_index"), 0);
	Mapping->SetStringField(TEXT("intent"), TEXT("create"));
	RetargetDefinition->SetArrayField(
		TEXT("chain_mappings"), {MakeShared<FJsonValueObject>(Mapping)});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.ik_retargeter.definition.apply"),
			RetargetDefinition, TEXT("retarget-definition")), RetargetObject,
		TEXT("IKRetargeter definition succeeds")))
	{
		return false;
	}
	const TSharedRef<FJsonObject> RetargetSettings = MakeShared<FJsonObject>();
	RetargetSettings->SetStringField(TEXT("target_ref"), RetargetObject);
	const TSharedRef<FJsonObject> Setting = MakeShared<FJsonObject>();
	Setting->SetStringField(TEXT("scope"), TEXT("chain"));
	Setting->SetStringField(TEXT("chain_name"), TEXT("W03Chain"));
	Setting->SetStringField(TEXT("setting_name"), TEXT("RotationMode"));
	Setting->SetStringField(TEXT("value"), TEXT("OneToOne"));
	RetargetSettings->SetArrayField(
		TEXT("settings"), {MakeShared<FJsonValueObject>(Setting)});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.ik_retargeter.settings.apply"),
			RetargetSettings, TEXT("retarget-settings")), RetargetObject,
		TEXT("IKRetargeter settings succeeds")))
	{
		return false;
	}

	UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, *RetargetObject);
	UIKRetargeterController* RetargetController =
		UIKRetargeterController::GetController(Retargeter);
	TestNotNull(TEXT("IKRetargeter reloads"), Retargeter);
	TestNotNull(TEXT("IKRetargeter controller reloads"), RetargetController);
	if (RetargetController)
	{
		TestEqual(TEXT("IKRetargeter exact source rig persists"),
			RetargetController->GetIKRig(ERetargetSourceOrTarget::Source)
				->GetPathName(), SourceRigObject);
		TestEqual(TEXT("IKRetargeter exact target rig persists"),
			RetargetController->GetIKRig(ERetargetSourceOrTarget::Target)
				->GetPathName(), TargetRigObject);
		const FRetargetChainMapping* ChainMapping =
			RetargetController->GetChainMapping(NAME_None);
		TestTrue(TEXT("IKRetargeter exact chain mapping persists"),
			ChainMapping && ChainMapping->IsReady()
			&& ChainMapping->GetChainMappedTo(
				TEXT("W03Chain"), ERetargetSourceOrTarget::Target)
				== TEXT("W03Chain"));
		FInstancedStruct* Op = RetargetController->GetRetargetOpStructAtIndex(0);
		UIKRetargetFKChainsController* OpController =
			Cast<UIKRetargetFKChainsController>(
				RetargetController->GetOpController(0));
		TestTrue(TEXT("IKRetargeter exact op type/order persists"),
			Op && Op->GetScriptStruct() == FIKRetargetFKChainsOp::StaticStruct());
		const FIKRetargetFKChainsOpSettings Settings = OpController
			? OpController->GetSettings() : FIKRetargetFKChainsOpSettings();
		TestTrue(TEXT("IKRetargeter exact chain setting persists"),
			Settings.ChainsToRetarget.ContainsByPredicate(
				[](const FRetargetFKChainSettings& Chain)
				{
					return Chain.TargetChainName == TEXT("W03Chain")
						&& Chain.RotationMode == EFKChainRotationMode::OneToOne;
				}));
	}
	Retargeter = nullptr;
	TestTrue(TEXT("all owned Rig/IK fixtures clean up"), Assets.Cleanup());
	return !HasAnyErrors();
}
