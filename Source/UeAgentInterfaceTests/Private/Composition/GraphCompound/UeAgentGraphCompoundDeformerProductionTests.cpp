#include "Misc/AutomationTest.h"

#include "Animation/MeshDeformerCollection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

	class FDeformerAssetSet final
	{
	public:
		FDeformerAssetSet()
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

		~FDeformerAssetSet()
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
			TEXT("correlation:w04:") + CapabilityId);
	}

	FUeAgentOperationResult Submit(
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const TSharedRef<FJsonObject>& Object,
		const FString& Suffix)
	{
		FString Payload;
		FString CanonicalError;
		check(Json::TryCanonicalize(Object, Payload, CanonicalError));
		const FPcgAnimationGraphRouteProfile* const Profile =
			FindPcgAnimationGraphRouteProfile(CapabilityId);
		check(Profile);
		return Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w04:") + Suffix,
				TEXT("idempotency:w04:") + Suffix,
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
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return FJsonSerializer::Deserialize(Reader, Root)
			&& Root.IsValid()
			&& Root->TryGetObjectField(TEXT("operation_error"), Error)
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
		return Test.TestTrue(*(Label + TEXT(" returns exact target_ref")),
			FJsonSerializer::Deserialize(Reader, Root)
				&& Root.IsValid()
				&& Root->TryGetStringField(TEXT("target_ref"), TargetRef)
				&& TargetRef == ExpectedTarget);
	}

	bool TestUnavailable(
		FAutomationTestBase& Test,
		const FUeAgentOperationResult& Result,
		const FString& Label)
	{
		FString ErrorCode;
		FString ErrorDetail;
		const bool bHasError = TryOperationError(Result, ErrorCode, ErrorDetail);
		Test.TestTrue(*(Label + TEXT(" returns operation_error")), bHasError);
		return bHasError && Test.TestEqual(
			*(Label + TEXT(" fails closed for unavailable concrete class")),
			ErrorCode,
			FString(TEXT("animation_rig_graph_concrete_type_unavailable")));
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundDeformerProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AnimationRig."
		"DeformerProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundDeformerProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FDeformerAssetSet Assets;
	const TStrongObjectPtr<USkeletalMesh> MeshAnchor(
		LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube")));
	if (!TestNotNull(TEXT("skeletal mesh fixture exists"), MeshAnchor.Get()))
	{
		return false;
	}

	FGraphCompoundRuntimeConfig RuntimeConfig;

	FGraphCompoundAnimationProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TestTrue(TEXT("Deformer production runtime builds"),
			TryCreateAnimationGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
		|| !TestNotNull(TEXT("Deformer production runtime exists"), Runtime.Get()))
	{
		AddError(RuntimeError.Code + TEXT(":") + RuntimeError.Detail);
		return false;
	}

	const FString DeformerPackage = Assets.Add(TEXT("DG_W04"));
	const FString DeformerObject = FDeformerAssetSet::ObjectPath(DeformerPackage);
	TSharedRef<FJsonObject> DeformerCreate = AssetCreate(DeformerPackage);
	DeformerCreate->SetStringField(
		TEXT("skeletal_mesh_ref"), MeshAnchor->GetPathName());
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.deformer_graph.asset.create"),
			DeformerCreate, TEXT("deformer-create")), DeformerPackage,
		TEXT("DeformerGraph create succeeds")))
	{
		return false;
	}

	UOptimusDeformer* Deformer = LoadObject<UOptimusDeformer>(
		nullptr, *DeformerObject);
	TestNotNull(TEXT("DeformerGraph reloads"), Deformer);
	if (Deformer)
	{
		TestEqual(TEXT("DeformerGraph exact preview mesh persists"),
			Deformer->Mesh.Get(), MeshAnchor.Get());
		TestTrue(TEXT("DeformerGraph primary binding persists"),
			Deformer->GetComponentBindings().ContainsByPredicate(
				[](const UOptimusComponentSourceBinding* Binding)
				{
					return Binding
						&& Binding->GetFName()
							== UOptimusComponentSourceBinding::GetPrimaryBindingName();
				}));
	}
	Deformer = nullptr;
	Assets.Unload(DeformerPackage);

	const TSharedRef<FJsonObject> TopologyApply = MakeShared<FJsonObject>();
	TopologyApply->SetStringField(TEXT("target_ref"), DeformerObject);
	const TSharedRef<FJsonObject> CommentEdit = MakeShared<FJsonObject>();
	CommentEdit->SetStringField(TEXT("identity"), TEXT("item:W04Comment"));
	CommentEdit->SetStringField(TEXT("element_kind"), TEXT("item"));
	CommentEdit->SetStringField(TEXT("intent"), TEXT("create"));
	const TSharedRef<FJsonObject> CommentValue = MakeShared<FJsonObject>();
	CommentValue->SetStringField(TEXT("node_name"), TEXT("Comment_W04"));
	CommentEdit->SetObjectField(TEXT("value"), CommentValue);
	TopologyApply->SetArrayField(
		TEXT("edits"), {MakeShared<FJsonValueObject>(CommentEdit)});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.deformer_graph.topology.apply"),
			TopologyApply, TEXT("deformer-topology")), DeformerObject,
		TEXT("DeformerGraph topology succeeds")))
	{
		return false;
	}
	Deformer = LoadObject<UOptimusDeformer>(nullptr, *DeformerObject);
	TestTrue(TEXT("DeformerGraph native comment node persists"),
		Deformer && Deformer->GetUpdateGraph()
			&& Deformer->GetUpdateGraph()->GetAllNodes().ContainsByPredicate(
				[](const UOptimusNode* Node)
				{
					return Node && Node->GetFName() == TEXT("Comment_W04");
				}));
	Deformer = nullptr;
	Assets.Unload(DeformerPackage);

	const FString ChildCollectionPackage = Assets.Add(TEXT("MDC_W04_Child"));
	const FString ParentCollectionPackage = Assets.Add(TEXT("MDC_W04_Parent"));
	for (const TPair<FString, FString>& Collection : {
		TPair<FString, FString>(ChildCollectionPackage, TEXT("child")),
		TPair<FString, FString>(ParentCollectionPackage, TEXT("parent"))})
	{
		if (!TestSuccess(*this, Submit(
				*Runtime,
				TEXT("animation.mesh_deformer_collection.asset.create"),
				AssetCreate(Collection.Key),
				TEXT("collection-create-") + Collection.Value),
			Collection.Key, TEXT("MeshDeformerCollection create succeeds")))
		{
			return false;
		}
	}

	const FString ParentCollectionObject =
		FDeformerAssetSet::ObjectPath(ParentCollectionPackage);
	const FString ChildCollectionObject =
		FDeformerAssetSet::ObjectPath(ChildCollectionPackage);
	const TSharedRef<FJsonObject> EntriesApply = MakeShared<FJsonObject>();
	EntriesApply->SetStringField(TEXT("target_ref"), ParentCollectionObject);
	const TSharedRef<FJsonObject> DeformerEntry = MakeShared<FJsonObject>();
	DeformerEntry->SetStringField(TEXT("entry_kind"), TEXT("deformer"));
	DeformerEntry->SetStringField(TEXT("entry_ref"), DeformerObject);
	DeformerEntry->SetStringField(TEXT("intent"), TEXT("create"));
	DeformerEntry->SetNumberField(TEXT("order_index"), 0);
	const TSharedRef<FJsonObject> ChildEntry = MakeShared<FJsonObject>();
	ChildEntry->SetStringField(TEXT("entry_kind"), TEXT("child_collection"));
	ChildEntry->SetStringField(TEXT("entry_ref"), ChildCollectionObject);
	ChildEntry->SetStringField(TEXT("intent"), TEXT("create"));
	ChildEntry->SetNumberField(TEXT("order_index"), 1);
	EntriesApply->SetArrayField(TEXT("entries"), {
		MakeShared<FJsonValueObject>(DeformerEntry),
		MakeShared<FJsonValueObject>(ChildEntry)});
	if (!TestSuccess(*this, Submit(
			*Runtime, TEXT("animation.mesh_deformer_collection.entries.apply"),
			EntriesApply, TEXT("collection-entries")), ParentCollectionObject,
		TEXT("MeshDeformerCollection entries succeed")))
	{
		return false;
	}
	UMeshDeformerCollection* ParentCollection =
		LoadObject<UMeshDeformerCollection>(nullptr, *ParentCollectionObject);
	TestTrue(TEXT("MeshDeformerCollection exact deformer entry persists"),
		ParentCollection && ParentCollection->MeshDeformers.Num() == 1
			&& ParentCollection->MeshDeformers[0].ToSoftObjectPath().ToString()
				== DeformerObject);
	TestTrue(TEXT("MeshDeformerCollection exact child entry persists"),
		ParentCollection && ParentCollection->MeshDeformerCollections.Num() == 1
			&& ParentCollection->MeshDeformerCollections[0]
			&& ParentCollection->MeshDeformerCollections[0]->GetPathName()
				== ChildCollectionObject);
	ParentCollection = nullptr;
	Assets.Unload(ParentCollectionPackage);

	const FString UnsupportedClass = TEXT("/Script/UAI.DoesNotExist");
	const FString SourcePackage = Assets.Add(TEXT("DSL_W04_Unsupported"));
	TSharedRef<FJsonObject> SourceCreate = AssetCreate(SourcePackage);
	SourceCreate->SetStringField(TEXT("concrete_class_ref"), UnsupportedClass);
	TestUnavailable(*this, Submit(
		*Runtime, TEXT("animation.deformer_source_library.asset.create"),
		SourceCreate, TEXT("source-create-unavailable")),
		TEXT("DeformerSourceLibrary create"));
	const TSharedRef<FJsonObject> SourceApply = MakeShared<FJsonObject>();
	SourceApply->SetStringField(
		TEXT("target_ref"), FDeformerAssetSet::ObjectPath(SourcePackage));
	SourceApply->SetStringField(TEXT("concrete_class_ref"), UnsupportedClass);
	const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("asset_ref"), MeshAnchor->GetPathName());
	Binding->SetStringField(TEXT("binding_key"), TEXT("W04Binding"));
	Binding->SetStringField(TEXT("intent"), TEXT("create"));
	Binding->SetStringField(TEXT("property_type_ref"), TEXT("SkeletalMesh"));
	SourceApply->SetArrayField(
		TEXT("bindings"), {MakeShared<FJsonValueObject>(Binding)});
	TestUnavailable(*this, Submit(
		*Runtime, TEXT("animation.deformer_source_library.bindings.apply"),
		SourceApply, TEXT("source-apply-unavailable")),
		TEXT("DeformerSourceLibrary bindings"));

	const FString MlPackage = Assets.Add(TEXT("MLD_W04_Unsupported"));
	TSharedRef<FJsonObject> MlCreate = AssetCreate(MlPackage);
	MlCreate->SetStringField(TEXT("model_class_ref"), UnsupportedClass);
	TestUnavailable(*this, Submit(
		*Runtime, TEXT("animation.ml_deformer.asset.create"),
		MlCreate, TEXT("ml-create-unavailable")),
		TEXT("MLDeformer create"));
	const TSharedRef<FJsonObject> MlApply = MakeShared<FJsonObject>();
	MlApply->SetStringField(
		TEXT("target_ref"), FDeformerAssetSet::ObjectPath(MlPackage));
	MlApply->SetStringField(TEXT("concrete_model_class_ref"), UnsupportedClass);
	MlApply->SetStringField(TEXT("skeletal_mesh_ref"), MeshAnchor->GetPathName());
	MlApply->SetStringField(TEXT("animation_ref"), MeshAnchor->GetPathName());
	MlApply->SetStringField(
		TEXT("target_geometry_cache_ref"), MeshAnchor->GetPathName());
	MlApply->SetStringField(TEXT("deformer_graph_ref"), DeformerObject);
	TestUnavailable(*this, Submit(
		*Runtime, TEXT("animation.ml_deformer.configuration.apply"),
		MlApply, TEXT("ml-apply-unavailable")),
		TEXT("MLDeformer configuration"));

	TestTrue(TEXT("W04 assets clean up"), Assets.Cleanup());
	return true;
}
