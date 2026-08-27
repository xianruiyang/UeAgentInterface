#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeTestSupport.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::Tests::WorldSpatial;

	class FOwnedWorldMeshFixture final : public FOwnedNativeEnvironmentBase
	{
	public:
		FOwnedWorldMeshFixture()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString PackageName = FString::Printf(
				TEXT("/Temp/UAI/WorldSpatial/%s"),
				*Suffix);
			Package = CreatePackage(*PackageName);
			MeshPackageName = FString::Printf(
				TEXT("/Game/UAI/Tests/WorldSpatial/SM_WorldSpatialOwned_%s"),
				*Suffix);
			MeshPackage = CreatePackage(*MeshPackageName);
			MeshFilename = FPackageName::LongPackageNameToFilename(
				MeshPackageName,
				FPackageName::GetAssetPackageExtension());
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("WorldSpatialOwnedWorld")),
				Package);
			if (!World)
			{
				return;
			}

			Subject = World->SpawnActor<AActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator);
			Anchor = World->SpawnActor<AActor>(
				FVector(100.0, 0.0, 0.0),
				FRotator::ZeroRotator);
			Surface = World->SpawnActor<AActor>(
				FVector(0.0, 0.0, -10.0),
				FRotator::ZeroRotator);
			if (!Subject || !Anchor || !Surface)
			{
				return;
			}
			Subject->Rename(TEXT("WorldSpatialSubject"));
			Anchor->Rename(TEXT("WorldSpatialAnchor"));
			Surface->Rename(TEXT("WorldSpatialSurface"));
			SubjectRoot = NewObject<UBoxComponent>(
				Subject,
				TEXT("WorldSpatialSubjectRoot"));
			AnchorRoot = NewObject<UBoxComponent>(
				Anchor,
				TEXT("WorldSpatialAnchorRoot"));
			if (!SubjectRoot || !AnchorRoot)
			{
				return;
			}
			SubjectRoot->InitBoxExtent(FVector(10.0, 10.0, 10.0));
			AnchorRoot->InitBoxExtent(FVector(10.0, 10.0, 10.0));
			Subject->SetRootComponent(SubjectRoot);
			Subject->AddInstanceComponent(SubjectRoot);
			SubjectRoot->RegisterComponentWithWorld(World);
			SubjectRoot->SetWorldLocation(FVector::ZeroVector);
			Anchor->SetRootComponent(AnchorRoot);
			Anchor->AddInstanceComponent(AnchorRoot);
			AnchorRoot->RegisterComponentWithWorld(World);
			AnchorRoot->SetWorldLocation(FVector(100.0, 0.0, 0.0));
			SurfaceCollision = NewObject<UBoxComponent>(
				Surface,
				TEXT("WorldSpatialSurfaceCollision"));
			SurfaceCollision->InitBoxExtent(FVector(100.0, 100.0, 10.0));
			SurfaceCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			SurfaceCollision->SetCollisionObjectType(ECC_WorldStatic);
			SurfaceCollision->SetCollisionResponseToAllChannels(ECR_Block);
			Surface->SetRootComponent(SurfaceCollision);
			Surface->AddInstanceComponent(SurfaceCollision);
			SurfaceCollision->RegisterComponentWithWorld(World);
			SurfaceCollision->SetWorldLocation(FVector(0.0, 0.0, -10.0));

			Mesh = MeshPackage
				? NewObject<UStaticMesh>(
					MeshPackage,
					*FPackageName::GetLongPackageAssetName(MeshPackageName),
					RF_Public | RF_Standalone)
				: nullptr;
			if (!Mesh)
			{
				return;
			}
			FAssetRegistryModule::AssetCreated(Mesh);
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions =
				Attributes.GetVertexPositions();
			const FVertexID V0 = Description.CreateVertex();
			const FVertexID V1 = Description.CreateVertex();
			const FVertexID V2 = Description.CreateVertex();
			Positions[V0] = FVector3f(0.0f, 0.0f, 0.0f);
			Positions[V1] = FVector3f(10.0f, 0.0f, 0.0f);
			Positions[V2] = FVector3f(0.0f, 10.0f, 0.0f);
			const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
			const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
			const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			Description.CreatePolygon(Group, {I0, I1, I2});
			Mesh->BuildFromMeshDescriptions({&Description});
			SubjectMeshComponent = NewObject<UStaticMeshComponent>(
				Subject, TEXT("WorldSpatialSubjectMesh"));
			AnchorMeshComponent = NewObject<UStaticMeshComponent>(
				Anchor, TEXT("WorldSpatialAnchorMesh"));
			if (!SubjectMeshComponent || !AnchorMeshComponent)
			{
				return;
			}
			SubjectMeshComponent->SetStaticMesh(Mesh);
			SubjectMeshComponent->SetupAttachment(SubjectRoot);
			Subject->AddInstanceComponent(SubjectMeshComponent);
			SubjectMeshComponent->RegisterComponentWithWorld(World);
			AnchorMeshComponent->SetStaticMesh(Mesh);
			AnchorMeshComponent->SetupAttachment(AnchorRoot);
			Anchor->AddInstanceComponent(AnchorMeshComponent);
			AnchorMeshComponent->RegisterComponentWithWorld(World);
			Package->SetDirtyFlag(false);

			Objects.Add(TEXT("actor:subject"), Subject);
			Objects.Add(TEXT("actor:anchor"), Anchor);
			Objects.Add(TEXT("actor:target"), Anchor);
			Objects.Add(TEXT("mesh:fixture"), Mesh);
		}

		~FOwnedWorldMeshFixture()
		{
			if (Mesh)
			{
				FAssetRegistryModule::AssetDeleted(Mesh);
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (MeshPackage)
			{
				MeshPackage->SetDirtyFlag(false);
				MeshPackage->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags, true);
			if (!MeshFilename.IsEmpty()
				&& IFileManager::Get().FileExists(*MeshFilename))
			{
				IFileManager::Get().Delete(*MeshFilename, false, true);
			}
		}

		bool IsValid() const
		{
			return Package && MeshPackage && World && Subject && Anchor && Surface
				&& SubjectRoot && AnchorRoot && SurfaceCollision && Mesh
				&& SubjectMeshComponent && AnchorMeshComponent;
		}

		bool TryResolveObject(
			const FString& ExplicitRef,
			UObject*& OutObject,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutObject = Objects.FindRef(ExplicitRef);
			if (!OutObject)
			{
				OutError.Code = TEXT("owned_world_mesh_object_unresolved");
				OutError.SubjectId = ExplicitRef;
				return false;
			}
			return true;
		}

		bool TryReadRevision(
			const FString& ExplicitRef,
			FWorldSpatialRevision& OutRevision,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutRevision = {};
			OutError = {};
			if (ExplicitRef == TEXT("actor:subject"))
			{
				const FVector Location = Subject->GetActorLocation();
				const int64 Sequence = Location.Equals(FVector::ZeroVector, 0.01)
					? 3
					: Location.Equals(FVector(100.0, 0.0, 0.0), 0.01)
						? 4
						: 5;
				OutRevision = {TEXT("actor:subject"), TEXT("gen-1"), Sequence};
				return true;
			}
			if (ExplicitRef == TEXT("mesh:fixture"))
			{
				const FVector3f First = FirstVertexPosition();
				const int64 Sequence = First.Equals(FVector3f::ZeroVector, 0.01f)
					? 5
					: First.Equals(FVector3f(10.0f, 20.0f, 30.0f), 0.01f)
						? 6
						: 7;
				OutRevision = {TEXT("mesh:fixture"), TEXT("gen-1"), Sequence};
				return true;
			}
			OutError.Code = TEXT("owned_world_mesh_revision_unresolved");
			OutError.SubjectId = ExplicitRef;
			return false;
		}

		FVector3f FirstVertexPosition() const
		{
			const FMeshDescription* Description = Mesh
				? Mesh->GetMeshDescription(0)
				: nullptr;
			if (!Description || Description->Vertices().Num() == 0)
			{
				return FVector3f::ZeroVector;
			}
			FStaticMeshConstAttributes Attributes(*Description);
			const TVertexAttributesConstRef<FVector3f> Positions =
				Attributes.GetVertexPositions();
			for (const FVertexID VertexId
				: Description->Vertices().GetElementIDs())
			{
				return Positions[VertexId];
			}
			return FVector3f::ZeroVector;
		}

		UPackage* Package = nullptr;
		UPackage* MeshPackage = nullptr;
		FString MeshPackageName;
		FString MeshFilename;
		UWorld* World = nullptr;
		AActor* Subject = nullptr;
		AActor* Anchor = nullptr;
		AActor* Surface = nullptr;
		UBoxComponent* SubjectRoot = nullptr;
		UBoxComponent* AnchorRoot = nullptr;
		UBoxComponent* SurfaceCollision = nullptr;
		UStaticMeshComponent* SubjectMeshComponent = nullptr;
		UStaticMeshComponent* AnchorMeshComponent = nullptr;
		UStaticMesh* Mesh = nullptr;
		TMap<FString, UObject*> Objects;
	};

	class FOwnedPersistentTransformWorld final
	{
	public:
		FOwnedPersistentTransformWorld()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			PackageName = FString(TEXT("/Game/UAI/Tests/WorldSpatial/"))
				+ TEXT("W_TransformUpdate_") + Suffix;
			HeaderPath = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetMapPackageExtension());
			Package = CreatePackage(*PackageName);
			World = Package
				? UWorld::CreateWorld(
					EWorldType::EditorPreview,
					false,
					FName(*FPackageName::GetLongPackageAssetName(PackageName)),
					Package)
				: nullptr;
			if (!World || !World->PersistentLevel || !GEngine)
			{
				return;
			}
			World->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			World->PersistentLevel->SetUseExternalActors(false);
			World->SetCurrentLevel(World->PersistentLevel);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(
				EWorldType::EditorPreview);
			WorldContext.SetCurrentWorld(World);
			bHasWorldContext = true;

			Subject = World->SpawnActor<AActor>();
			if (!Subject)
			{
				return;
			}
			Subject->Rename(TEXT("WorldSpatialPersistentTransformSubject"));
			Root = NewObject<USceneComponent>(
				Subject,
				TEXT("WorldSpatialPersistentTransformRoot"),
				RF_Transactional);
			if (!Root)
			{
				return;
			}
			Subject->SetRootComponent(Root);
			Subject->AddInstanceComponent(Root);
			Root->RegisterComponentWithWorld(World);
			Subject->SetActorTransform(FTransform::Identity);
			Package->SetDirtyFlag(false);
		}

		~FOwnedPersistentTransformWorld()
		{
			ReleaseLoadedWorld();
			for (const FString& Path : {
				HeaderPath,
				FPaths::ChangeExtension(HeaderPath, TEXT("uexp")),
				FPaths::ChangeExtension(HeaderPath, TEXT("ubulk")),
				FPaths::ChangeExtension(HeaderPath, TEXT("upayload"))})
			{
				IFileManager::Get().Delete(*Path, false, true, true);
			}
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& Subject
				&& Root
				&& bHasWorldContext;
		}

		bool TryReload(FString& OutError)
		{
			OutError.Reset();
			if (!IsValid())
			{
				OutError = TEXT("world_spatial_transform_reload_source_invalid");
				return false;
			}
			const FName SubjectName = Subject->GetFName();
			const TWeakObjectPtr<UWorld> OriginalWorld(World);
			ReleaseLoadedWorld();
			if (OriginalWorld.IsValid())
			{
				OutError = TEXT("world_spatial_transform_reload_source_still_live");
				return false;
			}

			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
			if (!World || !World->PersistentLevel || !GEngine)
			{
				OutError = TEXT("world_spatial_transform_reload_failed:")
					+ PackageName;
				return false;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(
				EWorldType::EditorPreview);
			WorldContext.SetCurrentWorld(World);
			bHasWorldContext = true;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetFName() == SubjectName)
				{
					Subject = *It;
					Root = Subject->GetRootComponent();
					return true;
				}
			}
			OutError = TEXT("world_spatial_transform_reloaded_actor_missing:")
				+ SubjectName.ToString();
			return false;
		}

		FString PackageName;
		FString HeaderPath;
		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		AActor* Subject = nullptr;
		USceneComponent* Root = nullptr;

	private:
		void ReleaseLoadedWorld()
		{
			UPackage* const PackageToUnload = Package;
			UWorld* const WorldToDestroy = World;
			Subject = nullptr;
			Root = nullptr;
			if (bHasWorldContext && GEngine && WorldToDestroy)
			{
				GEngine->DestroyWorldContext(WorldToDestroy);
				bHasWorldContext = false;
			}
			if (PackageToUnload)
			{
				PackageToUnload->SetDirtyFlag(false);
			}
			if (WorldToDestroy)
			{
				WorldToDestroy->DestroyWorld(false);
				WorldToDestroy->MarkAsGarbage();
			}
			World = nullptr;
			Package = nullptr;
			if (PackageToUnload)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages(
					{PackageToUnload},
					UnloadError,
					true);
			}
			CollectGarbage(RF_NoFlags, true);
		}

		bool bHasWorldContext = false;
	};

	TSharedPtr<FJsonObject> MakeAlignmentTypedInputs(
		FAutomationTestBase& Test,
		const FString& Kind)
	{
		FString KindFields;
		FString AnchorField;
		if (Kind == TEXT("anchors"))
		{
			KindFields = TEXT(R"json(,
				"subject_anchor":{"subject":"actor:subject","quantity":"point","frame":{"frame_id":"fixture.subject.semantic","basis_frame_id":"unreal.world","dimension":3,"unit":"centimeter","basis_evidence_id":"basis:subject","observed_at":{"clock_id":"editor","tick":"201","rate_numerator":"60","rate_denominator":"1","state":"frozen"}},"components":["0","0","0"],"conversion_evidence_ids":["convert:subject"]},
				"reference_anchor":{"subject":"actor:anchor","quantity":"point","frame":{"frame_id":"fixture.subject.semantic","basis_frame_id":"unreal.world","dimension":3,"unit":"centimeter","basis_evidence_id":"basis:anchor","observed_at":{"clock_id":"editor","tick":"201","rate_numerator":"60","rate_denominator":"1","state":"frozen"}},"components":["100","0","0"],"conversion_evidence_ids":["convert:anchor"]})json");
		}
		else if (Kind == TEXT("bounds"))
		{
			KindFields = TEXT(",\"axis\":\"x\",\"source_anchor\":\"center\",\"target_anchor\":\"center\"");
			AnchorField = TEXT(",\"anchor_ref\":\"actor:anchor\"");
		}
		else if (Kind == TEXT("face"))
		{
			KindFields = TEXT(",\"source_face\":\"+x\",\"target_face\":\"-x\",\"offset_cm\":0");
			AnchorField = TEXT(",\"anchor_ref\":\"actor:anchor\"");
		}
		else if (Kind == TEXT("vertex"))
		{
			AnchorField = TEXT(",\"anchor_ref\":\"actor:anchor\"");
		}
		else
		{
			Test.AddError(TEXT("unknown alignment kind: ") + Kind);
			return nullptr;
		}
		const FString InputText = FString::Printf(
			TEXT(R"json({
				"subject_refs":["actor:subject"],
				"alignment":{"kind":"%s","verify_frame":{"frame_id":"fixture.subject.semantic","basis_frame_id":"unreal.world","dimension":3,"unit":"centimeter","basis_evidence_id":"basis:world","observed_at":{"clock_id":"editor","tick":"201","rate_numerator":"60","rate_denominator":"1","state":"frozen"}}%s,"rotation_policy":"preserve"},
				"expected_revisions":[{"target_ref":"actor:subject","revision":{"source_id":"placeholder","generation":"placeholder","sequence":0}}]%s
			})json"),
			*Kind,
			*KindFields,
			*AnchorField);
		TSharedPtr<FJsonObject> Input;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(InputText);
		if (!Test.TestTrue(TEXT("parse alignment mode input: ") + Kind,
				FJsonSerializer::Deserialize(Reader, Input) && Input.IsValid()))
		{
			return nullptr;
		}
		const TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
		Case->SetStringField(TEXT("capability_id"), TEXT("world.object_align"));
		Case->SetObjectField(TEXT("valid_input"), Input);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("cases"), {
			MakeShared<FJsonValueObject>(Case),
		});
		return Root;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionObjectTransformC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.ObjectMesh."
		"ObjectTransformC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionObjectTransformC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("production object fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *Fixture.World, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs = {
		{TEXT("actor:subject"), Fixture.Subject->GetPathName()},
		{TEXT("actor:anchor"), Fixture.Anchor->GetPathName()},
		{TEXT("actor:target"), Fixture.Anchor->GetPathName()},
	};
	const FString SubjectRef = Fixture.Subject->GetPathName();
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(), FString(), FString(), {}};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("object_transform_owned_editor_world"),
		Runtime.ToSharedRef(),
		Context,
		{{TEXT("world.object_transform.read"), SubjectRef}},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("world.object_align"))
			{
				Test.TestTrue(TEXT("production C3 aligns the exact actor"),
					Fixture.Subject->GetActorLocation().Equals(
						FVector(100.0, 0.0, 0.0), 0.1));
			}
			else if (CapabilityId == TEXT("world.object_snap_to_surface"))
			{
				Test.TestTrue(TEXT("production C3 snaps the exact actor"),
					Fixture.Subject->GetActorLocation().Equals(
						FVector(0.0, 0.0, 5.0), 0.1));
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionAlignmentModesTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.ObjectMesh."
		"AlignmentModesRealMechanics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionAlignmentModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	(void)Parameters;
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("alignment-mode fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *Fixture.World, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs = {
		{TEXT("actor:subject"), Fixture.Subject->GetPathName()},
		{TEXT("actor:anchor"), Fixture.Anchor->GetPathName()},
	};
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(), FString(), FString(), {}};
	struct FModeCase
	{
		const TCHAR* Kind;
		double ExpectedX;
	};
	const FModeCase Cases[] = {
		{TEXT("anchors"), 100.0},
		{TEXT("bounds"), 100.0},
		{TEXT("face"), 80.0},
		{TEXT("vertex"), 90.0},
	};
	for (const FModeCase& Case : Cases)
	{
		Fixture.Subject->SetActorLocation(FVector::ZeroVector);
		const TSharedPtr<FJsonObject> Inputs =
			MakeAlignmentTypedInputs(*this, Case.Kind);
		const bool bPassed = RunProductionCapabilitiesThroughC0C3(
			*this,
			TEXT("alignment_mode_") + FString(Case.Kind),
			{TEXT("world.object_align")},
			Inputs,
			Runtime.ToSharedRef(),
			Context,
			{},
			[&](FAutomationTestBase& Test, const FString& CapabilityId,
				const TSharedPtr<FJsonObject>& RawInput)
			{
				return PrepareProductionCapabilityInput(
					Test, CapabilityId, RawInput, *Runtime, Bindings);
			});
		TestTrue(TEXT("alignment mode reaches verified real mechanics: ")
			+ FString(Case.Kind), bPassed);
		TestTrue(TEXT("alignment mode has exact independent transform result: ")
			+ FString(Case.Kind),
			Fixture.Subject->GetActorLocation().Equals(
				FVector(Case.ExpectedX, 0.0, 0.0), 0.1));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionMeshGeometryC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.ObjectMesh."
		"MeshGeometryC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionMeshGeometryC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("production mesh fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *Fixture.World, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs.Add(TEXT("mesh:fixture"), Fixture.Mesh->GetPathName());
	const FVector3f OriginalFirstVertex = Fixture.FirstVertexPosition();
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(), FString(), FString(), {}};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("mesh_geometry_owned_asset"),
		Runtime.ToSharedRef(),
		Context,
		{},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture, OriginalFirstVertex](
			FAutomationTestBase& Test, const FString& CapabilityId)
		{
			const FVector3f FirstVertex = Fixture.FirstVertexPosition();
			if (CapabilityId == TEXT("mesh.transform.bake"))
			{
				Test.TestTrue(TEXT("production C3 bakes real mesh geometry"),
					!FirstVertex.Equals(OriginalFirstVertex, 0.01f)
						&& FirstVertex.Equals(
							FVector3f(10.0f, 20.0f, 30.0f), 0.01f));
			}
			else if (CapabilityId == TEXT("mesh.pivot.update"))
			{
				Test.TestTrue(TEXT("production C3 updates real mesh pivot"),
					FirstVertex.Equals(
						FVector3f(0.0f, 20.0f, 30.0f), 0.01f));
			}
			if (CapabilityId == TEXT("mesh.transform.bake")
				|| CapabilityId == TEXT("mesh.pivot.update"))
			{
				Test.TestFalse(
					TEXT("production C3 leaves the exact mesh package clean after save"),
					Fixture.MeshPackage->IsDirty());
				Test.TestTrue(
					TEXT("production C3 writes the exact owned mesh package"),
					IFileManager::Get().FileExists(*Fixture.MeshFilename));
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialObjectTransformOwnedEditorWorldTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"ObjectTransformOwnedEditorWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialObjectTransformOwnedEditorWorldTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("object_transform_owned_editor_world")))
	{
		return false;
	}
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("owned editor-preview world is valid"), Fixture.IsValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FString(),
	};
	const TMap<FString, FString> QueryTargets{
		{TEXT("world.object_transform.read"), TEXT("actor:subject")},
	};
	return RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("object_transform_owned_editor_world"),
		MakeShareable(&Fixture, [](FOwnedWorldMeshFixture*) {}),
		Context,
		QueryTargets,
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("world.object_align"))
			{
				Test.TestTrue(
					TEXT("owned subject aligns to the explicit anchor"),
					Fixture.Subject->GetActorLocation().Equals(
						FVector(100.0, 0.0, 0.0),
						0.1));
			}
			else if (CapabilityId == TEXT("world.object_snap_to_surface"))
			{
				Test.TestTrue(
					TEXT("owned subject snaps to the explicit surface query"),
					Fixture.Subject->GetActorLocation().Equals(
						FVector(0.0, 0.0, 5.0),
						0.1));
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialObjectTransformUpdateOwnedActorTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"ObjectTransformUpdateOwnedActor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialObjectTransformUpdateOwnedActorTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	(void)Parameters;
	if (!RequireNativeExecution(
			*this, TEXT("object_transform_update_owned_actor")))
	{
		return false;
	}
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("owned transform-update actor is valid"), Fixture.IsValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FString(),
	};
	return RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("object_transform_update_owned_actor"),
		MakeShareable(&Fixture, [](FOwnedWorldMeshFixture*) {}),
		Context,
		{},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("world.object_transform.update"))
			{
				Test.TestTrue(
					TEXT("typed update writes and independently reads all actor transform components"),
					Fixture.Subject->GetActorTransform().Equals(FTransform(
						FQuat::Identity,
						FVector(100.0, 200.0, 300.0),
						FVector::OneVector),
						0.001));
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialObjectTransformUpdatePersistenceTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.ObjectMesh."
		"ObjectTransformUpdatePersistence",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialObjectTransformUpdatePersistenceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	(void)Parameters;
	FOwnedPersistentTransformWorld Fixture;
	if (!TestTrue(TEXT("persistent transform world is valid"), Fixture.IsValid()))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *Fixture.World, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs = {
		{TEXT("actor:subject"), Fixture.Subject->GetPathName()},
	};
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(), FString(), FString(), {}};
	const bool bExecuted = RunProductionScenarioThroughC0C3(
		*this,
		TEXT("object_transform_update_owned_actor"),
		Runtime.ToSharedRef(),
		Context,
		{},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("world.object_transform.update"))
			{
				Test.TestTrue(
					TEXT("production update saves the owned map header"),
					IFileManager::Get().FileExists(*Fixture.HeaderPath));
				Test.TestFalse(
					TEXT("production update leaves the owned map clean"),
					Fixture.Package->IsDirty());
			}
		});
	Runtime.Reset();
	if (!bExecuted)
	{
		return false;
	}
	FString ReloadError;
	if (!TestTrue(
			TEXT("saved transform world reloads from disk"),
			Fixture.TryReload(ReloadError)))
	{
		AddError(ReloadError);
		return false;
	}
	TestTrue(
		TEXT("reloaded Actor preserves all persisted transform components"),
		Fixture.Subject
			&& Fixture.Subject->GetActorTransform().Equals(
				FTransform(
					FQuat::Identity,
					FVector(100.0, 200.0, 300.0),
					FVector::OneVector),
				0.001));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialMeshGeometryOwnedAssetTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"MeshGeometryOwnedAsset",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialMeshGeometryOwnedAssetTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("mesh_geometry_owned_asset")))
	{
		return false;
	}
	FOwnedWorldMeshFixture Fixture;
	if (!TestTrue(TEXT("owned StaticMesh fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	const FVector3f OriginalFirstVertex = Fixture.FirstVertexPosition();
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FString(),
	};
	const bool bResult = RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("mesh_geometry_owned_asset"),
		MakeShareable(&Fixture, [](FOwnedWorldMeshFixture*) {}),
		Context,
		{},
		[&Fixture, OriginalFirstVertex](
			FAutomationTestBase& Test,
			const FString& CapabilityId)
		{
			const FVector3f FirstVertex = Fixture.FirstVertexPosition();
			if (CapabilityId == TEXT("mesh.transform.bake"))
			{
				Test.TestTrue(
					TEXT("bake changes real mesh geometry"),
					!FirstVertex.Equals(OriginalFirstVertex, 0.01f));
				Test.TestTrue(
					TEXT("bake applies the typed translation"),
					FirstVertex.Equals(FVector3f(10.0f, 20.0f, 30.0f), 0.01f));
			}
			else if (CapabilityId == TEXT("mesh.pivot.update"))
			{
				Test.TestTrue(
					TEXT("pivot update is read from real geometry"),
					FirstVertex.Equals(FVector3f(0.0f, 20.0f, 30.0f), 0.01f));
			}
		});
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialActorTransformAuthoringSchemaTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.AuthoringSchema."
		"ActorTransformContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialActorTransformAuthoringSchemaTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using UE::UAI::HjsonWorkspace::FWorkspaceError;
	(void)Parameters;
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("authoring-schema editor world exists"), World))
	{
		return false;
	}
	const FString Target = World->GetPathName();
	const FString Params = FString::Printf(
		TEXT("{\"target\":\"%s\"}"), *Target);
	FString PayloadJson;
	bool bReadOnly = false;
	FWorkspaceError Error;
	if (!TestTrue(
			TEXT("world authoring schema is available through its domain owner"),
			TryExecuteContentGraph(
				TEXT("content_graph.authoring_schema"), Target, Params,
				PayloadJson, bReadOnly, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
	TestTrue(TEXT("authoring schema is read-only"), bReadOnly);

	TSharedPtr<FJsonObject> Payload;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(PayloadJson);
	if (!TestTrue(
			TEXT("authoring schema payload parses"),
			FJsonSerializer::Deserialize(Reader, Payload) && Payload.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Schema = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	if (!TestTrue(
			TEXT("schema exposes field contracts"),
			Payload->TryGetObjectField(TEXT("query_result"), Schema)
				&& Schema && (*Schema).IsValid()
				&& (*Schema)->TryGetArrayField(TEXT("fields"), Fields)
				&& Fields && Fields->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Transform = (*Fields)[0]->AsObject();
	if (!TestTrue(TEXT("transform field contract is structured"), Transform.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("transform path"), Transform->GetStringField(TEXT("path")),
		FString(TEXT("/root/document/provider_extension/actors/*/transform")));
	TestEqual(TEXT("transform owner"), Transform->GetStringField(TEXT("owner")),
		FString(TEXT("level-world-domain-provider")));
	TestEqual(TEXT("transform field set"),
		Transform->GetStringField(TEXT("field_set")),
		FString(TEXT("actor_upsert")));
	TestEqual(TEXT("transform value contract"),
		Transform->GetStringField(TEXT("value_contract")),
		FString(TEXT("world_actor_transform.v1")));
	TestEqual(TEXT("missing transform preserves state"),
		Transform->GetStringField(TEXT("missing")), FString(TEXT("preserve")));
	TestFalse(TEXT("transform is not nullable"),
		Transform->GetBoolField(TEXT("nullable")));
	TestFalse(TEXT("transform has no default intent"),
		Transform->GetBoolField(TEXT("default_supported")));
	TestEqual(TEXT("transform write status"),
		Transform->GetStringField(TEXT("write_status")),
		FString(TEXT("supported")));
	TestEqual(TEXT("actor transform semantic frame"),
		Transform->GetStringField(TEXT("semantic_frame")), FString(TEXT("ue_world")));
	TestEqual(TEXT("actor transform unit"),
		Transform->GetStringField(TEXT("translation_unit")),
		FString(TEXT("centimeter")));
	TestEqual(TEXT("transform supports preserve and set"),
		Transform->GetArrayField(TEXT("allowed_intents")).Num(), 2);

	const TSharedPtr<FJsonObject>* Canonical = nullptr;
	const TSharedPtr<FJsonObject>* NamedAxes = nullptr;
	const TSharedPtr<FJsonObject>* Example = nullptr;
	const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
	TestTrue(TEXT("canonical transform contract is present"),
		Transform->TryGetObjectField(TEXT("canonical"), Canonical)
			&& Canonical && (*Canonical).IsValid()
			&& (*Canonical)->HasTypedField<EJson::Object>(TEXT("translation"))
			&& (*Canonical)->HasTypedField<EJson::Object>(TEXT("rotation"))
			&& (*Canonical)->HasTypedField<EJson::Object>(TEXT("scale")));
	if (Canonical && (*Canonical).IsValid())
	{
		for (const TCHAR* Field : {
			TEXT("translation"), TEXT("rotation"), TEXT("scale")})
		{
			const TSharedPtr<FJsonObject>* ValueContract = nullptr;
			const TSharedPtr<FJsonObject>* Components = nullptr;
			const bool bComplete = (*Canonical)->TryGetObjectField(
				Field, ValueContract)
				&& ValueContract && (*ValueContract).IsValid()
				&& (*ValueContract)->TryGetObjectField(
					TEXT("components"), Components)
				&& Components && (*Components).IsValid()
				&& (*Components)->GetStringField(TEXT("item_type"))
					== TEXT("finite_number")
				&& (*Components)->GetNumberField(TEXT("item_count")) == 3.0;
			TestTrue(FString(TEXT("canonical finite vector contract: ")) + Field,
				bComplete);
		}
	}
	TestTrue(TEXT("named-axis aliases are present"),
		Transform->TryGetObjectField(TEXT("accepted_named_axes"), NamedAxes)
			&& NamedAxes && (*NamedAxes).IsValid()
			&& (*NamedAxes)->HasTypedField<EJson::Object>(TEXT("location"))
			&& (*NamedAxes)->HasTypedField<EJson::Object>(TEXT("rotation"))
			&& (*NamedAxes)->HasTypedField<EJson::Object>(TEXT("scale")));
	TestTrue(TEXT("Actor world space contract is structured"),
		Transform->TryGetObjectField(TEXT("space_contract"), SpaceContract)
			&& SpaceContract && (*SpaceContract).IsValid()
			&& (*SpaceContract)->GetStringField(TEXT("subject"))
				== TEXT("actor.transform")
			&& (*SpaceContract)->GetNumberField(TEXT("dimension")) == 3.0
			&& (*SpaceContract)->GetStringField(TEXT("semantic_frame"))
				== TEXT("ue_world")
			&& (*SpaceContract)->GetStringField(TEXT("write_frame"))
				== TEXT("ue_world")
			&& (*SpaceContract)->GetStringField(TEXT("read_frame"))
				== TEXT("ue_world")
			&& (*SpaceContract)->GetStringField(TEXT("unit"))
				== TEXT("centimeter")
			&& (*SpaceContract)->GetStringField(TEXT("level_contract"))
				== TEXT("server_injected"));
	TestTrue(TEXT("conditional transform example is present"),
		Transform->TryGetObjectField(TEXT("conditional_example"), Example)
			&& Example && (*Example).IsValid()
			&& (*Example)->HasTypedField<EJson::Object>(TEXT("transform")));
	TestTrue(TEXT("conflict semantics are explicit"),
		!Transform->GetStringField(TEXT("conflict_semantics")).IsEmpty());

	const TSharedPtr<FJsonObject>* Template = nullptr;
	const TSharedPtr<FJsonObject>* Root = nullptr;
	const TSharedPtr<FJsonObject>* Document = nullptr;
	const TSharedPtr<FJsonObject>* Extension = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	const bool bTemplateResolved = (*Schema)->TryGetObjectField(
		TEXT("template"), Template)
		&& Template && (*Template).IsValid()
		&& (*Template)->TryGetObjectField(TEXT("root"), Root)
		&& Root && (*Root).IsValid()
		&& (*Root)->TryGetObjectField(TEXT("document"), Document)
		&& Document && (*Document).IsValid()
		&& (*Document)->TryGetObjectField(TEXT("provider_extension"), Extension)
		&& Extension && (*Extension).IsValid()
		&& (*Extension)->TryGetArrayField(TEXT("actors"), Actors)
		&& Actors && Actors->Num() == 1;
	if (TestTrue(TEXT("minimal world template resolves"), bTemplateResolved))
	{
		const TSharedPtr<FJsonObject> TemplateActor = (*Actors)[0]->AsObject();
		TestTrue(TEXT("minimal actor template is structured"), TemplateActor.IsValid());
		TestFalse(TEXT("minimal actor template omits transform"),
			TemplateActor.IsValid() && TemplateActor->HasField(TEXT("transform")));
	}
	return !HasAnyErrors();
}
