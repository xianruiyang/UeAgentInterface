#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Contract;

	class FActiveCancellation final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FAnimationAssetSet final
	{
	public:
		FAnimationAssetSet()
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

		FString ObjectPath(const FString& PackageName) const
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool SaveNew(UObject& Asset) const
		{
			UPackage* const Package = Asset.GetOutermost();
			if (!Package || !PackageNames.Contains(Package->GetName()))
			{
				return false;
			}
			Asset.SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(&Asset);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			const bool bSaved = UPackage::SavePackage(
				Package, &Asset, *Filename, Args);
			Package->SetDirtyFlag(false);
			return bSaved;
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

		~FAnimationAssetSet()
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
			TEXT("correlation:w02:") + CapabilityId);
	}

	bool TryJson(const TSharedRef<FJsonObject>& Object, FString& OutJson)
	{
		FString Error;
		return Json::TryCanonicalize(Object, OutJson, Error);
	}

	FUeAgentOperationResult Submit(
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const FString& Payload,
		const FString& Suffix)
	{
		const FPcgAnimationGraphRouteProfile* const Profile =
			FindPcgAnimationGraphRouteProfile(CapabilityId);
		check(Profile);
		return Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w02:") + Suffix,
				TEXT("idempotency:w02:") + Suffix,
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
		const bool bExact = FJsonSerializer::Deserialize(Reader, Root)
			&& Root.IsValid()
			&& Root->TryGetStringField(TEXT("target_ref"), TargetRef)
			&& TargetRef == ExpectedTarget;
		return Test.TestTrue(*(Label + TEXT(" returns exact target_ref")), bExact);
	}

	TSharedRef<FJsonObject> AssetCreate(
		const FString& PackageName,
		const FString& SkeletonRef = FString(),
		const FString& ParentClassRef = FString(),
		const TOptional<int32>& Dimension = {})
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("asset_name"),
			FPackageName::GetLongPackageAssetName(PackageName));
		Object->SetStringField(TEXT("package_path"),
			FPackageName::GetLongPackagePath(PackageName));
		if (!SkeletonRef.IsEmpty())
		{
			Object->SetStringField(TEXT("skeleton_ref"), SkeletonRef);
		}
		if (!ParentClassRef.IsEmpty())
		{
			Object->SetStringField(TEXT("parent_class_ref"), ParentClassRef);
		}
		if (Dimension.IsSet())
		{
			Object->SetNumberField(TEXT("dimension"), Dimension.GetValue());
		}
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAnimationAssetsProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AnimationRig."
		"AnimationAssetsProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAnimationAssetsProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAnimationAssetSet Assets;
	const TStrongObjectPtr<USkeleton> EngineSkeletonAnchor(
		LoadObject<USkeleton>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton."
				"SkeletalCube_Skeleton")));
	USkeleton* const EngineSkeleton = EngineSkeletonAnchor.Get();
	if (!TestNotNull(TEXT("engine skeleton fixture exists"), EngineSkeleton))
	{
		return false;
	}
	const FString EngineSkeletonRef = EngineSkeleton->GetPathName();
	const FName EngineRoot = EngineSkeleton->GetReferenceSkeleton().GetBoneName(0);
	if (!TestFalse(TEXT("engine skeleton exposes a root bone"), EngineRoot.IsNone()))
	{
		return false;
	}

	FGraphCompoundRuntimeConfig RuntimeConfig;
	RuntimeConfig.MaxFrameBytes = 1024 * 1024;
	FGraphCompoundAnimationProductionConfig ProductionConfig;
	ProductionConfig.OperationTimeoutSeconds = 30.0;
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

	const FString RejectedPackage = Assets.Add(TEXT("ABP_W02_Rejected"));
	TSharedRef<FJsonObject> Rejected = AssetCreate(
		RejectedPackage, EngineSkeletonRef, TEXT("/Script/Engine.AnimInstance"));
	Rejected->SetBoolField(TEXT("unexpected"), true);
	FString Payload;
	TestTrue(TEXT("rejected request serializes"), TryJson(Rejected, Payload));
	const FUeAgentOperationResult RejectedResult = Submit(
		*Runtime, TEXT("animation.anim_blueprint.asset.create"),
		Payload, TEXT("rejected"));
	FString ErrorCode;
	FString ErrorDetail;
	TestTrue(TEXT("unknown field returns operation_error"),
		TryOperationError(RejectedResult, ErrorCode, ErrorDetail));
	TestEqual(TEXT("unknown field fails at public schema"), ErrorCode,
		FString(TEXT("contract_input_schema_mismatch")));
	TestFalse(TEXT("rejected request leaves no package file"),
		FPaths::FileExists(FPackageName::LongPackageNameToFilename(
			RejectedPackage, FPackageName::GetAssetPackageExtension())));

	const FString AnimPackage = Assets.Add(TEXT("ABP_W02"));
	TestTrue(TEXT("AnimBlueprint create request serializes"),
		TryJson(AssetCreate(
			AnimPackage, EngineSkeletonRef,
			TEXT("/Script/Engine.AnimInstance")), Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.anim_blueprint.asset.create"),
			Payload, TEXT("anim-create")), AnimPackage,
		TEXT("AnimBlueprint create succeeds")))
	{
		return false;
	}
	const FString AnimObject = Assets.ObjectPath(AnimPackage);
	UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(
		nullptr, *AnimObject);
	TestNotNull(TEXT("AnimBlueprint reloads"), AnimBlueprint);
	if (AnimBlueprint)
	{
		TestEqual(TEXT("AnimBlueprint keeps exact skeleton"),
			AnimBlueprint->TargetSkeleton.Get(), EngineSkeleton);
		TestEqual(TEXT("AnimBlueprint keeps exact parent class"),
			AnimBlueprint->ParentClass.Get(), UAnimInstance::StaticClass());
	}
	Assets.Unload(AnimPackage);

	const FString LayerPackage = Assets.Add(TEXT("ALI_W02"));
	TestTrue(TEXT("LayerInterface request serializes"),
		TryJson(AssetCreate(LayerPackage), Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime,
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
			Payload, TEXT("layer-create")), LayerPackage,
		TEXT("LayerInterface create succeeds")))
	{
		return false;
	}
	UAnimBlueprint* Layer = LoadObject<UAnimBlueprint>(
		nullptr, *Assets.ObjectPath(LayerPackage));
	TestNotNull(TEXT("LayerInterface reloads as UAnimBlueprint"), Layer);
	if (Layer)
	{
		TestEqual(TEXT("LayerInterface owns interface blueprint type"),
			Layer->BlueprintType, BPTYPE_Interface);
	}
	Assets.Unload(LayerPackage);

	const FString BlendPackage = Assets.Add(TEXT("BS_W02"));
	TestTrue(TEXT("BlendSpace request serializes"),
		TryJson(AssetCreate(
			BlendPackage, EngineSkeletonRef, FString(), 2), Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.blend_space.asset.create"),
			Payload, TEXT("blend-create")), BlendPackage,
		TEXT("BlendSpace create succeeds")))
	{
		return false;
	}
	UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(
		nullptr, *Assets.ObjectPath(BlendPackage));
	TestNotNull(TEXT("BlendSpace reloads"), BlendSpace);
	if (BlendSpace)
	{
		TestEqual(TEXT("BlendSpace keeps exact skeleton"),
			BlendSpace->GetSkeleton(), EngineSkeleton);
	}
	Assets.Unload(BlendPackage);

	const FString SequencePackage = Assets.Add(TEXT("AS_W02"));
	UPackage* SequenceOuter = CreatePackage(*SequencePackage);
	UAnimSequence* Sequence = SequenceOuter
		? NewObject<UAnimSequence>(SequenceOuter,
			*FPackageName::GetLongPackageAssetName(SequencePackage)) : nullptr;
	if (!TestNotNull(TEXT("sequence fixture creates"), Sequence))
	{
		return false;
	}
	Sequence->SetSkeleton(EngineSkeleton);
	Sequence->GetController().InitializeModel();
	Sequence->GetController().SetNumberOfFrames(FFrameNumber(1), false);
	Sequence->GetController().NotifyPopulated();
	if (!TestTrue(TEXT("sequence fixture saves"), Assets.SaveNew(*Sequence)))
	{
		return false;
	}
	const FString SequenceObject = Assets.ObjectPath(SequencePackage);
	Sequence = nullptr;
	Assets.Unload(SequencePackage);

	const TSharedRef<FJsonObject> BlendDefinition = MakeShared<FJsonObject>();
	BlendDefinition->SetStringField(TEXT("target_ref"),
		Assets.ObjectPath(BlendPackage));
	TArray<TSharedPtr<FJsonValue>> Axes;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const TSharedRef<FJsonObject> Axis = MakeShared<FJsonObject>();
		Axis->SetNumberField(TEXT("axis_index"), Index);
		Axis->SetStringField(TEXT("axis_name"),
			Index == 0 ? TEXT("Speed") : TEXT("Direction"));
		Axis->SetNumberField(TEXT("minimum"), Index == 0 ? 0.0 : -180.0);
		Axis->SetNumberField(TEXT("maximum"), Index == 0 ? 600.0 : 180.0);
		Axis->SetNumberField(TEXT("grid_divisions"), 4);
		Axes.Add(MakeShared<FJsonValueObject>(Axis));
	}
	BlendDefinition->SetArrayField(TEXT("axes"), MoveTemp(Axes));
	const TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetStringField(TEXT("animation_ref"), SequenceObject);
	Sample->SetStringField(TEXT("intent"), TEXT("create"));
	Sample->SetArrayField(TEXT("coordinates"), {
		MakeShared<FJsonValueNumber>(300.0),
		MakeShared<FJsonValueNumber>(45.0)});
	BlendDefinition->SetArrayField(TEXT("samples"), {
		MakeShared<FJsonValueObject>(Sample)});
	TestTrue(TEXT("BlendSpace definition serializes"),
		TryJson(BlendDefinition, Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.blend_space.definition.apply"),
			Payload, TEXT("blend-definition")), Assets.ObjectPath(BlendPackage),
		TEXT("BlendSpace definition succeeds")))
	{
		return false;
	}
	BlendSpace = LoadObject<UBlendSpace>(nullptr, *Assets.ObjectPath(BlendPackage));
	TestNotNull(TEXT("configured BlendSpace reloads"), BlendSpace);
	if (BlendSpace)
	{
		TestEqual(TEXT("axis 0 persists"),
			BlendSpace->GetBlendParameter(0).DisplayName, FString(TEXT("Speed")));
		TestEqual(TEXT("axis 1 persists"),
			BlendSpace->GetBlendParameter(1).DisplayName,
			FString(TEXT("Direction")));
		TestEqual(TEXT("one BlendSpace sample persists"),
			BlendSpace->GetBlendSamples().Num(), 1);
		if (!BlendSpace->GetBlendSamples().IsEmpty())
		{
			TestEqual(TEXT("sample animation identity persists"),
				BlendSpace->GetBlendSamples()[0].Animation->GetPathName(),
				SequenceObject);
		}
	}
	Assets.Unload(BlendPackage);

	const TSharedRef<FJsonObject> AnimGraph = MakeShared<FJsonObject>();
	AnimGraph->SetStringField(TEXT("target_ref"), AnimObject);
	const TSharedRef<FJsonObject> GraphEdit = MakeShared<FJsonObject>();
	GraphEdit->SetStringField(TEXT("identity"),
		TEXT("structure:anim_graph:AnimGraph_W02"));
	GraphEdit->SetStringField(TEXT("element_kind"), TEXT("structure"));
	GraphEdit->SetStringField(TEXT("intent"), TEXT("create"));
	const TSharedRef<FJsonObject> GraphValue = MakeShared<FJsonObject>();
	GraphValue->SetStringField(TEXT("graph_name"), TEXT("AnimGraph_W02"));
	GraphEdit->SetObjectField(TEXT("value"), GraphValue);
	AnimGraph->SetArrayField(TEXT("edits"), {
		MakeShared<FJsonValueObject>(GraphEdit)});
	TestTrue(TEXT("AnimBlueprint graph serializes"), TryJson(AnimGraph, Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.anim_blueprint.graph.apply"),
			Payload, TEXT("anim-graph")), AnimObject,
		TEXT("AnimBlueprint graph succeeds")))
	{
		return false;
	}
	AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimObject);
	TArray<UEdGraph*> Graphs;
	if (AnimBlueprint)
	{
		AnimBlueprint->GetAllGraphs(Graphs);
	}
	TestTrue(TEXT("AnimBlueprint graph persists"), Graphs.ContainsByPredicate(
		[](const UEdGraph* Graph)
		{
			return Graph && Graph->GetFName() == TEXT("AnimGraph_W02");
		}));
	Assets.Unload(AnimPackage);

	const FString SkeletonPackage = Assets.Add(TEXT("SK_W02"));
	UPackage* SkeletonOuter = CreatePackage(*SkeletonPackage);
	USkeleton* Skeleton = SkeletonOuter
		? DuplicateObject<USkeleton>(EngineSkeleton, SkeletonOuter,
			*FPackageName::GetLongPackageAssetName(SkeletonPackage)) : nullptr;
	if (!TestNotNull(TEXT("Skeleton fixture duplicates"), Skeleton)
		|| !TestTrue(TEXT("Skeleton fixture saves"), Assets.SaveNew(*Skeleton)))
	{
		return false;
	}
	const FString SkeletonObject = Assets.ObjectPath(SkeletonPackage);
	Skeleton = nullptr;
	Assets.Unload(SkeletonPackage);

	const FString CompatiblePackage = Assets.Add(TEXT("SK_W02_Compatible"));
	UPackage* CompatibleOuter = CreatePackage(*CompatiblePackage);
	USkeleton* Compatible = CompatibleOuter
		? DuplicateObject<USkeleton>(EngineSkeleton, CompatibleOuter,
			*FPackageName::GetLongPackageAssetName(CompatiblePackage)) : nullptr;
	if (!TestNotNull(TEXT("compatible Skeleton fixture duplicates"), Compatible)
		|| !TestTrue(TEXT("compatible Skeleton fixture saves"),
			Assets.SaveNew(*Compatible)))
	{
		return false;
	}
	const FString CompatibleObject = Assets.ObjectPath(CompatiblePackage);
	Compatible = nullptr;
	Assets.Unload(CompatiblePackage);

	const FString AddedBone = TEXT("W02Child");
	const TSharedRef<FJsonObject> SkeletonDefinition = MakeShared<FJsonObject>();
	SkeletonDefinition->SetStringField(TEXT("target_ref"), SkeletonObject);
	const TSharedRef<FJsonObject> BoneEdit = MakeShared<FJsonObject>();
	BoneEdit->SetStringField(TEXT("identity"),
		TEXT("hierarchy:bone:") + AddedBone);
	BoneEdit->SetStringField(TEXT("element_kind"), TEXT("hierarchy"));
	BoneEdit->SetStringField(TEXT("intent"), TEXT("create"));
	const TSharedRef<FJsonObject> BoneValue = MakeShared<FJsonObject>();
	BoneValue->SetStringField(TEXT("bone_name"), AddedBone);
	BoneValue->SetStringField(TEXT("parent_bone"), EngineRoot.ToString());
	BoneEdit->SetObjectField(TEXT("value"), BoneValue);
	SkeletonDefinition->SetArrayField(TEXT("edits"), {
		MakeShared<FJsonValueObject>(BoneEdit)});
	TestTrue(TEXT("Skeleton definition serializes"),
		TryJson(SkeletonDefinition, Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.skeleton.definition.apply"),
			Payload, TEXT("skeleton-definition")), SkeletonObject,
		TEXT("Skeleton definition succeeds")))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Compatibility = MakeShared<FJsonObject>();
	Compatibility->SetStringField(TEXT("target_ref"), SkeletonObject);
	const TSharedRef<FJsonObject> CompatibleChange = MakeShared<FJsonObject>();
	CompatibleChange->SetStringField(TEXT("skeleton_ref"), CompatibleObject);
	CompatibleChange->SetStringField(TEXT("intent"), TEXT("create"));
	Compatibility->SetArrayField(TEXT("changes"), {
		MakeShared<FJsonValueObject>(CompatibleChange)});
	TestTrue(TEXT("Skeleton compatibility serializes"),
		TryJson(Compatibility, Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.skeleton.compatibility.apply"),
			Payload, TEXT("skeleton-compatibility")), SkeletonObject,
		TEXT("Skeleton compatibility succeeds")))
	{
		return false;
	}

	const FString VirtualBoneName = TEXT("VB W02");
	const TSharedRef<FJsonObject> VirtualBone = MakeShared<FJsonObject>();
	VirtualBone->SetStringField(TEXT("target_ref"), SkeletonObject);
	VirtualBone->SetStringField(TEXT("intent"), TEXT("create"));
	VirtualBone->SetStringField(TEXT("name"), VirtualBoneName);
	VirtualBone->SetStringField(TEXT("source_bone"), EngineRoot.ToString());
	VirtualBone->SetStringField(TEXT("target_bone"), AddedBone);
	TestTrue(TEXT("Skeleton virtual bone serializes"),
		TryJson(VirtualBone, Payload));
	if (!TestSuccess(*this,
		Submit(*Runtime, TEXT("animation.skeleton.virtual_bone.apply"),
			Payload, TEXT("skeleton-virtual-bone")), SkeletonObject,
		TEXT("Skeleton virtual bone succeeds")))
	{
		return false;
	}

	Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonObject);
	Compatible = LoadObject<USkeleton>(nullptr, *CompatibleObject);
	TestNotNull(TEXT("mutated Skeleton reloads"), Skeleton);
	TestNotNull(TEXT("compatible Skeleton reloads"), Compatible);
	if (Skeleton && Compatible)
	{
		TestTrue(TEXT("new bone persists"),
			Skeleton->GetReferenceSkeleton().FindBoneIndex(*AddedBone) != INDEX_NONE);
		TestTrue(TEXT("compatible Skeleton identity persists"),
			Skeleton->GetCompatibleSkeletons().ContainsByPredicate(
				[&CompatibleObject](const TSoftObjectPtr<USkeleton>& Entry)
				{
					return Entry.ToSoftObjectPath().ToString() == CompatibleObject;
				}));
		TestTrue(TEXT("virtual bone persists"),
			Skeleton->GetVirtualBones().ContainsByPredicate(
				[&VirtualBoneName](const FVirtualBone& Bone)
				{
					return Bone.VirtualBoneName == *VirtualBoneName;
				}));
	}
	Skeleton = nullptr;
	Compatible = nullptr;
	TestTrue(TEXT("all owned animation fixtures clean up"), Assets.Cleanup());
	return !HasAnyErrors();
}
