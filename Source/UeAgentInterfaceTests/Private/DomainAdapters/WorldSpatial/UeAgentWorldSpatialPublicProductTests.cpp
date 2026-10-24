#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "StaticMeshAttributes.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace UE::UAI::Tests::WorldSpatialPublicProduct
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;

	TSharedRef<FJsonObject> TemporalPoint(const FString& Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("editor"));
		Result->SetStringField(TEXT("tick"), Tick);
		Result->SetStringField(TEXT("rate_numerator"), TEXT("60"));
		Result->SetStringField(TEXT("rate_denominator"), TEXT("1"));
		Result->SetStringField(TEXT("state"), TEXT("frozen"));
		return Result;
	}

	TSharedRef<FJsonObject> SpatialFrame(
		const FString& FrameId,
		const FString& Unit,
		const FString& Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), FrameId);
		Result->SetStringField(TEXT("basis_frame_id"), FrameId);
		Result->SetNumberField(TEXT("dimension"), 3);
		Result->SetStringField(TEXT("unit"), Unit);
		Result->SetStringField(
			TEXT("basis_evidence_id"),
			TEXT("t42-public-identity-basis:") + FrameId);
		Result->SetObjectField(TEXT("observed_at"), TemporalPoint(Tick));
		return Result;
	}

	TSharedRef<FJsonObject> PublicParams(
		const FString& Target,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), Target);
		Result->SetObjectField(TEXT("input"), Input);
		Result->SetObjectField(TEXT("context"), Context);
		return Result;
	}

	TSharedRef<FJsonObject> AlignInput(
		const FString& SubjectRef,
		const FString& AnchorRef)
	{
		const TSharedRef<FJsonObject> Alignment = MakeShared<FJsonObject>();
		Alignment->SetStringField(TEXT("kind"), TEXT("bounds"));
		Alignment->SetObjectField(
			TEXT("verify_frame"),
			SpatialFrame(TEXT("unreal.world"), TEXT("centimeter"), TEXT("3")));
		Alignment->SetStringField(TEXT("axis"), TEXT("x"));
		Alignment->SetStringField(TEXT("source_anchor"), TEXT("center"));
		Alignment->SetStringField(TEXT("target_anchor"), TEXT("center"));
		Alignment->SetStringField(TEXT("rotation_policy"), TEXT("preserve"));
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(
			TEXT("subject_refs"),
			{MakeShared<FJsonValueString>(SubjectRef)});
		Result->SetObjectField(TEXT("alignment"), Alignment);
		Result->SetStringField(TEXT("anchor_ref"), AnchorRef);
		return Result;
	}

	TSharedRef<FJsonObject> BakeInput(const FString& MeshRef)
	{
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetStringField(TEXT("subject"), MeshRef);
		Transform->SetStringField(TEXT("quantity"), TEXT("transform"));
		Transform->SetObjectField(
			TEXT("frame"),
			SpatialFrame(TEXT("mesh.asset.local"), TEXT("centimeter"), TEXT("4")));
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const FString& Component : {
			TEXT("10"), TEXT("20"), TEXT("30"),
			TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1"),
			TEXT("2"), TEXT("2"), TEXT("2")})
		{
			Components.Add(MakeShared<FJsonValueString>(Component));
		}
		Transform->SetArrayField(TEXT("components"), MoveTemp(Components));
		Transform->SetArrayField(
			TEXT("conversion_evidence_ids"),
			{MakeShared<FJsonValueString>(TEXT("t42-public-identity-conversion"))});
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("transform"), Transform);
		Result->SetBoolField(TEXT("preserve_placement"), true);
		return Result;
	}

	TSharedRef<FJsonObject> SpatialValue(
		const FString& Subject,
		const FString& Quantity,
		const FString& FrameId,
		const FString& Unit,
		const TArray<FString>& ComponentLexemes,
		const FString& Tick)
	{
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const FString& Component : ComponentLexemes)
		{
			Components.Add(MakeShared<FJsonValueString>(Component));
		}
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), Subject);
		Result->SetStringField(TEXT("quantity"), Quantity);
		Result->SetObjectField(
			TEXT("frame"),
			SpatialFrame(FrameId, Unit, Tick));
		Result->SetArrayField(TEXT("components"), MoveTemp(Components));
		Result->SetArrayField(TEXT("conversion_evidence_ids"), {});
		return Result;
	}

	TSharedRef<FJsonObject> SnapInput(const FString& SubjectRef)
	{
		const TSharedRef<FJsonObject> SurfaceQuery = MakeShared<FJsonObject>();
		SurfaceQuery->SetObjectField(
			TEXT("origin"),
			SpatialValue(
				TEXT("trace:origin"),
				TEXT("point"),
				TEXT("unreal.world"),
				TEXT("centimeter"),
				{TEXT("0"), TEXT("0"), TEXT("100")},
				TEXT("8")));
		SurfaceQuery->SetObjectField(
			TEXT("direction"),
			SpatialValue(
				TEXT("trace:direction"),
				TEXT("direction"),
				TEXT("unreal.world"),
				TEXT("unitless"),
				{TEXT("0"), TEXT("0"), TEXT("-1")},
				TEXT("8")));
		SurfaceQuery->SetNumberField(TEXT("max_distance_cm"), 500.0);
		SurfaceQuery->SetStringField(TEXT("trace_channel"), TEXT("visibility"));
		SurfaceQuery->SetBoolField(TEXT("trace_complex"), true);
		SurfaceQuery->SetArrayField(
			TEXT("ignore_target_refs"),
			{MakeShared<FJsonValueString>(SubjectRef)});

		const TSharedRef<FJsonObject> PlacementPolicy = MakeShared<FJsonObject>();
		PlacementPolicy->SetNumberField(TEXT("clearance_cm"), 5.0);
		PlacementPolicy->SetBoolField(TEXT("align_to_surface_normal"), true);
		PlacementPolicy->SetBoolField(TEXT("preserve_yaw"), true);

		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(
			TEXT("subject_refs"),
			{MakeShared<FJsonValueString>(SubjectRef)});
		Result->SetObjectField(TEXT("surface_query"), SurfaceQuery);
		Result->SetObjectField(TEXT("placement_policy"), PlacementPolicy);
		return Result;
	}

	TSharedRef<FJsonObject> PivotInput(const FString& MeshRef)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("pivot"),
			SpatialValue(
				MeshRef,
				TEXT("point"),
				TEXT("mesh.geometry.local"),
				TEXT("centimeter"),
				{TEXT("10"), TEXT("0"), TEXT("0")},
				TEXT("9")));
		Result->SetBoolField(TEXT("preserve_world_geometry"), true);
		return Result;
	}

	AActor* CreateBoxActor(
		UWorld& World,
		const FString& BaseName,
		const FVector& Location)
	{
		FActorSpawnParameters Spawn;
		Spawn.Name = MakeUniqueObjectName(
			&World,
			AActor::StaticClass(),
			FName(*BaseName));
		AActor* const Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(),
			Location,
			FRotator::ZeroRotator,
			Spawn);
		if (!Actor)
		{
			return nullptr;
		}
		UBoxComponent* const Root = NewObject<UBoxComponent>(
			Actor,
			MakeUniqueObjectName(
				Actor,
				UBoxComponent::StaticClass(),
				TEXT("T42PublicRoot")),
			RF_Transactional);
		if (!Root)
		{
			Actor->Destroy();
			return nullptr;
		}
		Root->InitBoxExtent(FVector(10.0, 10.0, 10.0));
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponentWithWorld(&World);
		Actor->SetActorLocation(Location);
		return Actor;
	}

	UStaticMesh* CreateOwnedMesh(
		UPackage& Package,
		const FString& AssetName)
	{
		UStaticMesh* const Mesh = NewObject<UStaticMesh>(
			&Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Mesh)
		{
			return nullptr;
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
		Mesh->MarkPackageDirty();
		return Mesh;
	}

	FVector3f FirstVertexPosition(const UStaticMesh& Mesh)
	{
		const FMeshDescription* const Description = Mesh.GetMeshDescription(0);
		if (!Description || Description->Vertices().Num() == 0)
		{
			return FVector3f::ZeroVector;
		}
		FStaticMeshConstAttributes Attributes(*Description);
		const TVertexAttributesConstRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
		{
			return Positions[Vertex];
		}
		return FVector3f::ZeroVector;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialPublicOperationProductTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.PublicOperationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialPublicOperationProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::WorldSpatialPublicProduct;
	(void)Parameters;

	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the public product has an Editor world"), World))
	{
		return false;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	AActor* Subject = CreateBoxActor(
		*World,
		TEXT("T42PublicSpatialSubject_") + Suffix,
		FVector::ZeroVector);
	AActor* Anchor = CreateBoxActor(
		*World,
		TEXT("T42PublicSpatialAnchor_") + Suffix,
		FVector(100.0, 0.0, 0.0));
	AActor* Surface = CreateBoxActor(
		*World,
		TEXT("T42PublicSpatialSurface_") + Suffix,
		FVector(0.0, 0.0, -10.0));
	if (Surface)
	{
		if (UBoxComponent* const SurfaceRoot =
			Cast<UBoxComponent>(Surface->GetRootComponent()))
		{
			SurfaceRoot->SetBoxExtent(FVector(100.0, 100.0, 10.0), true);
			SurfaceRoot->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			SurfaceRoot->SetCollisionObjectType(ECC_WorldStatic);
			SurfaceRoot->SetCollisionResponseToAllChannels(ECR_Block);
		}
	}
	const FString PackageName = FString(TEXT("/Game/UAI/Tests/WorldSpatialPublic/"))
		+ TEXT("SM_T42PublicSpatial_") + Suffix;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString MeshObjectPath = PackageName + TEXT(".") + AssetName;
	const FString MeshFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	UPackage* MeshPackage = CreatePackage(*PackageName);
	UStaticMesh* Mesh = MeshPackage
		? CreateOwnedMesh(*MeshPackage, AssetName)
		: nullptr;
	ON_SCOPE_EXIT
	{
		if (Subject && IsValid(Subject))
		{
			Subject->Destroy();
		}
		if (Anchor && IsValid(Anchor))
		{
			Anchor->Destroy();
		}
		if (Surface && IsValid(Surface))
		{
			Surface->Destroy();
		}
		if (UStaticMesh* Existing = FindObject<UStaticMesh>(
			nullptr,
			*MeshObjectPath))
		{
			FAssetRegistryModule::AssetDeleted(Existing);
		}
		if (UPackage* LoadedPackage = FindPackage(nullptr, *PackageName))
		{
			LoadedPackage->SetDirtyFlag(false);
			FText UnloadError;
			UPackageTools::UnloadPackages({LoadedPackage}, UnloadError, true);
		}
		CollectGarbage(RF_NoFlags, true);
		if (IFileManager::Get().FileExists(*MeshFilename))
		{
			IFileManager::Get().Delete(*MeshFilename, false, true);
		}
	};
	if (!TestNotNull(TEXT("owned subject actor exists"), Subject)
		|| !TestNotNull(TEXT("owned anchor actor exists"), Anchor)
		|| !TestNotNull(TEXT("owned surface actor exists"), Surface)
		|| !TestNotNull(TEXT("owned mesh package exists"), MeshPackage)
		|| !TestNotNull(TEXT("owned mesh exists"), Mesh))
	{
		return false;
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the compatibility-capable public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	const FString SubjectRef = Subject->GetPathName();
	const FString AnchorRef = Anchor->GetPathName();
	auto Call = [&](const FString& CommandId,
		const FString& Target,
		const TSharedRef<FJsonObject>& Input)
	{
		return UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
			Registry,
			Dispatch,
			CommandId,
			PublicParams(Target, Input),
			TEXT("t42-world-spatial-public"));
	};

	const FUeAgentCommandResult Align = Call(
		TEXT("world.object_align"),
		SubjectRef,
		AlignInput(SubjectRef, AnchorRef));
	if (!TestTrue(TEXT("public bounds alignment succeeds"), Align.IsOk()))
	{
		AddError(Align.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("public alignment changes only the owned actor"),
			Subject->GetActorLocation().Equals(
				FVector(100.0, 0.0, 0.0),
				0.01)))
	{
		return false;
	}

	const FUeAgentCommandResult Snap = Call(
		TEXT("world.object_snap_to_surface"),
		SubjectRef,
		SnapInput(SubjectRef));
	if (!TestTrue(TEXT("public surface snap succeeds"), Snap.IsOk()))
	{
		AddError(Snap.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("public surface snap changes only the owned actor"),
			Subject->GetActorLocation().Equals(
				FVector(0.0, 0.0, 5.0),
				0.01)))
	{
		return false;
	}

	const FUeAgentCommandResult Bake = Call(
		TEXT("mesh.transform.bake"),
		MeshObjectPath,
		BakeInput(MeshObjectPath));
	if (!TestTrue(TEXT("public mesh transform bake succeeds"), Bake.IsOk()))
	{
		AddError(Bake.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("public mesh bake saves only the owned package"),
			IFileManager::Get().FileExists(*MeshFilename)
				&& !MeshPackage->IsDirty()
				&& FirstVertexPosition(*Mesh).Equals(
					FVector3f(10.0f, 20.0f, 30.0f),
					0.01f)))
	{
		return false;
	}
	const FUeAgentCommandResult Pivot = Call(
		TEXT("mesh.pivot.update"),
		MeshObjectPath,
		PivotInput(MeshObjectPath));
	if (!TestTrue(TEXT("public mesh pivot update succeeds"), Pivot.IsOk()))
	{
		AddError(Pivot.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("public mesh pivot changes only the owned geometry"),
			FirstVertexPosition(*Mesh).Equals(
				FVector3f(0.0f, 20.0f, 30.0f),
				0.01f)))
	{
		return false;
	}

	MeshPackage->SetDirtyFlag(false);
	FText UnloadError;
	if (!TestTrue(
			TEXT("saved public mesh package unloads"),
			UPackageTools::UnloadPackages({MeshPackage}, UnloadError, true)))
	{
		AddError(UnloadError.ToString());
		return false;
	}
	Mesh = nullptr;
	MeshPackage = nullptr;
	CollectGarbage(RF_NoFlags, true);
	UStaticMesh* const ReloadedMesh = LoadObject<UStaticMesh>(
		nullptr,
		*MeshObjectPath);
	if (!TestNotNull(TEXT("saved public mesh reloads"), ReloadedMesh)
		|| !TestTrue(
			TEXT("reloaded mesh preserves transformed geometry"),
			FirstVertexPosition(*ReloadedMesh).Equals(
				FVector3f(0.0f, 20.0f, 30.0f),
				0.01f)))
	{
		return false;
	}
	return !HasAnyErrors();
}

#endif
