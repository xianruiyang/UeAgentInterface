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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

	bool TryReadProjectedSnapshot(
		FAutomationTestBase& Test,
		const FString& Stage,
		const FUeAgentCommandResult& Result,
		TSharedPtr<FJsonObject>& OutSnapshot)
	{
		OutSnapshot.Reset();
		if (!Result.IsOk())
		{
			Test.AddError(Stage + TEXT(": ") + Result.GetErrorJson());
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetPayloadJson());
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TSharedPtr<FJsonObject>* Snapshot = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("projection"), Projection)
			|| !Projection
			|| !Projection->IsValid()
			|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload
			|| !Payload->IsValid()
			|| !(*Payload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
			|| !Snapshot
			|| !Snapshot->IsValid())
		{
			Test.AddError(Stage + TEXT(": public projection snapshot is invalid: ")
				+ Result.GetPayloadJson());
			return false;
		}
		OutSnapshot = *Snapshot;
		return true;
	}

	bool TryCopyRevision(
		FAutomationTestBase& Test,
		const FString& Stage,
		const TSharedPtr<FJsonObject>& Snapshot,
		const FString& ExpectedSource,
		TSharedPtr<FJsonObject>& OutRevision)
	{
		OutRevision.Reset();
		const TSharedPtr<FJsonObject>* Revision = nullptr;
		FString SourceId;
		FString Generation;
		double Sequence = -1.0;
		if (!Snapshot.IsValid()
			|| !Snapshot->TryGetObjectField(TEXT("revision"), Revision)
			|| !Revision
			|| !Revision->IsValid()
			|| !(*Revision)->TryGetStringField(TEXT("source_id"), SourceId)
			|| !(*Revision)->TryGetStringField(TEXT("generation"), Generation)
			|| !(*Revision)->TryGetNumberField(TEXT("sequence"), Sequence)
			|| SourceId != ExpectedSource
			|| Generation.IsEmpty()
			|| Sequence < 0.0)
		{
			Test.AddError(Stage + TEXT(": projected target revision is invalid"));
			return false;
		}
		OutRevision = MakeShared<FJsonObject>();
		OutRevision->SetStringField(TEXT("source_id"), SourceId);
		OutRevision->SetStringField(TEXT("generation"), Generation);
		OutRevision->SetNumberField(TEXT("sequence"), Sequence);
		return true;
	}

	bool TryReadSpatialComponents(
		const TSharedPtr<FJsonObject>& Snapshot,
		const FString& ObjectRole,
		TArray<double>& OutComponents)
	{
		OutComponents.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Snapshot.IsValid()
			|| !Snapshot->TryGetArrayField(TEXT("entries"), Entries)
			|| !Entries)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!EntryValue.IsValid()
				|| !EntryValue->TryGetObject(Entry)
				|| !Entry
				|| !Entry->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Identity = nullptr;
			FString Role;
			if (!(*Entry)->TryGetObjectField(TEXT("identity"), Identity)
				|| !Identity
				|| !Identity->IsValid()
				|| !(*Identity)->TryGetStringField(TEXT("object_role"), Role)
				|| Role != ObjectRole)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!(*Entry)->TryGetObjectField(TEXT("value"), Value)
				|| !Value
				|| !Value->IsValid())
			{
				return false;
			}
			FString ValueKind;
			if (!(*Value)->TryGetStringField(TEXT("kind"), ValueKind))
			{
				return false;
			}
			if (ValueKind == TEXT("spatial"))
			{
				if (!(*Value)->TryGetArrayField(TEXT("components"), Components)
					|| !Components)
				{
					return false;
				}
			}
			else if (ValueKind == TEXT("object"))
			{
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				const TSharedPtr<FJsonObject>* CanonicalComponents = nullptr;
				FString ComponentsKind;
				if (!(*Value)->TryGetObjectField(TEXT("fields"), Fields)
					|| !Fields
					|| !Fields->IsValid()
					|| !(*Fields)->TryGetObjectField(
						TEXT("components"), CanonicalComponents)
					|| !CanonicalComponents
					|| !CanonicalComponents->IsValid()
					|| !(*CanonicalComponents)->TryGetStringField(
						TEXT("kind"), ComponentsKind)
					|| ComponentsKind != TEXT("array")
					|| !(*CanonicalComponents)->TryGetArrayField(
						TEXT("items"), Components)
					|| !Components)
				{
					return false;
				}
			}
			else
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Component : *Components)
			{
				FString Lexeme;
				const TSharedPtr<FJsonObject>* CanonicalNumber = nullptr;
				const bool bDirectString = Component.IsValid()
					&& Component->TryGetString(Lexeme);
				const bool bCanonicalNumber = Component.IsValid()
					&& Component->TryGetObject(CanonicalNumber)
					&& CanonicalNumber
					&& CanonicalNumber->IsValid()
					&& (*CanonicalNumber)->TryGetStringField(
						TEXT("lexeme"), Lexeme);
				if ((!bDirectString && !bCanonicalNumber) || !Lexeme.IsNumeric())
				{
					OutComponents.Reset();
					return false;
				}
				OutComponents.Add(FCString::Atod(*Lexeme));
			}
			return !OutComponents.IsEmpty();
		}
		return false;
	}

	TSharedRef<FJsonObject> TransformReadInput(
		const FString& FrameId,
		const FString& Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("verify_frame"),
			SpatialFrame(FrameId, TEXT("centimeter"), Tick));
		return Result;
	}

	TSharedRef<FJsonObject> AlignInput(
		const FString& SubjectRef,
		const FString& AnchorRef,
		const TSharedRef<FJsonObject>& Revision)
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
		const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("target_ref"), SubjectRef);
		Expected->SetObjectField(TEXT("revision"), Revision);
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(
			TEXT("subject_refs"),
			{MakeShared<FJsonValueString>(SubjectRef)});
		Result->SetObjectField(TEXT("alignment"), Alignment);
		Result->SetArrayField(
			TEXT("expected_revisions"),
			{MakeShared<FJsonValueObject>(Expected)});
		Result->SetStringField(TEXT("anchor_ref"), AnchorRef);
		return Result;
	}

	TSharedRef<FJsonObject> BakeInput(
		const FString& MeshRef,
		const TSharedRef<FJsonObject>& Revision)
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
		Result->SetObjectField(TEXT("expected_revision"), Revision);
		Result->SetBoolField(TEXT("preserve_placement"), true);
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
	FUeAgentWorldSpatialPublicQueryOperationProductTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E3.PublicProduct.QueryOperationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialPublicQueryOperationProductTest::RunTest(
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
		|| !TestNotNull(TEXT("owned mesh package exists"), MeshPackage)
		|| !TestNotNull(TEXT("owned mesh exists"), Mesh))
	{
		return false;
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
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

	TSharedPtr<FJsonObject> ActorBefore;
	TSharedPtr<FJsonObject> ActorRevision;
	if (!TryReadProjectedSnapshot(
			*this,
			TEXT("actor query before align"),
			Call(
				TEXT("world.object_transform.read"),
				SubjectRef,
				TransformReadInput(TEXT("unreal.world"), TEXT("1"))),
			ActorBefore)
		|| !TryCopyRevision(
			*this,
			TEXT("actor query before align"),
			ActorBefore,
			SubjectRef,
			ActorRevision))
	{
		return false;
	}

	TSharedPtr<FJsonObject> MeshBefore;
	TSharedPtr<FJsonObject> MeshRevision;
	if (!TryReadProjectedSnapshot(
			*this,
			TEXT("mesh query before bake"),
			Call(
				TEXT("world.object_transform.read"),
				MeshObjectPath,
				TransformReadInput(TEXT("mesh.geometry.local"), TEXT("2"))),
			MeshBefore)
		|| !TryCopyRevision(
			*this,
			TEXT("mesh query before bake"),
			MeshBefore,
			MeshObjectPath,
			MeshRevision))
	{
		return false;
	}
	TArray<double> MeshBeforeVertex;
	if (!TestTrue(
			TEXT("mesh query returns geometry-local vertex observation"),
			TryReadSpatialComponents(
				MeshBefore,
				TEXT("mesh_vertex:0"),
				MeshBeforeVertex)
				&& MeshBeforeVertex.Num() >= 3
				&& FMath::IsNearlyZero(MeshBeforeVertex[0])
				&& FMath::IsNearlyZero(MeshBeforeVertex[1])
				&& FMath::IsNearlyZero(MeshBeforeVertex[2])))
	{
		return false;
	}

	const FUeAgentCommandResult Align = Call(
		TEXT("world.object_align"),
		SubjectRef,
		AlignInput(SubjectRef, AnchorRef, ActorRevision.ToSharedRef()));
	if (!TestTrue(TEXT("public bounds alignment succeeds"), Align.IsOk()))
	{
		AddError(Align.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> ActorAfter;
	TArray<double> ActorComponents;
	if (!TryReadProjectedSnapshot(
			*this,
			TEXT("actor query after align"),
			Call(
				TEXT("world.object_transform.read"),
				SubjectRef,
				TransformReadInput(TEXT("unreal.world"), TEXT("5"))),
			ActorAfter)
		|| !TestTrue(
			TEXT("public actor readback observes the aligned world transform"),
			TryReadSpatialComponents(
				ActorAfter,
				TEXT("actor_transform:") + SubjectRef,
				ActorComponents)
				&& ActorComponents.Num() == 10
				&& FMath::IsNearlyEqual(ActorComponents[0], 100.0, 0.01)
				&& Subject->GetActorLocation().Equals(
					FVector(100.0, 0.0, 0.0),
					0.01)))
	{
		return false;
	}

	const FUeAgentCommandResult Bake = Call(
		TEXT("mesh.transform.bake"),
		MeshObjectPath,
		BakeInput(MeshObjectPath, MeshRevision.ToSharedRef()));
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
	TSharedPtr<FJsonObject> MeshAfter;
	TSharedPtr<FJsonObject> MeshAfterRevision;
	TArray<double> MeshAfterVertex;
	if (!TryReadProjectedSnapshot(
			*this,
			TEXT("mesh query after bake"),
			Call(
				TEXT("world.object_transform.read"),
				MeshObjectPath,
				TransformReadInput(TEXT("mesh.geometry.local"), TEXT("6"))),
			MeshAfter)
		|| !TryCopyRevision(
			*this,
			TEXT("mesh query after bake"),
			MeshAfter,
			MeshObjectPath,
			MeshAfterRevision)
		|| !TestTrue(
			TEXT("public mesh readback observes transformed geometry"),
			TryReadSpatialComponents(
				MeshAfter,
				TEXT("mesh_vertex:0"),
				MeshAfterVertex)
				&& MeshAfterVertex.Num() >= 3
				&& FMath::IsNearlyEqual(MeshAfterVertex[0], 10.0, 0.01)
				&& FMath::IsNearlyEqual(MeshAfterVertex[1], 20.0, 0.01)
				&& FMath::IsNearlyEqual(MeshAfterVertex[2], 30.0, 0.01)))
	{
		return false;
	}

	const FVector3f BeforeStale = FirstVertexPosition(*Mesh);
	const FUeAgentCommandResult StaleBake = Call(
		TEXT("mesh.transform.bake"),
		MeshObjectPath,
		BakeInput(MeshObjectPath, MeshRevision.ToSharedRef()));
	if (!TestFalse(TEXT("stale public mesh revision fails"), StaleBake.IsOk())
		|| !TestTrue(
			TEXT("stale mesh failure preserves the owned geometry"),
			StaleBake.GetErrorJson().Contains(
				TEXT("world_spatial_expected_revision_mismatch"))
				&& FirstVertexPosition(*Mesh).Equals(BeforeStale, 0.01f)))
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
				FVector3f(10.0f, 20.0f, 30.0f),
				0.01f)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> MeshReloaded;
	TArray<double> MeshReloadedVertex;
	if (!TryReadProjectedSnapshot(
			*this,
			TEXT("mesh query after reload"),
			Call(
				TEXT("world.object_transform.read"),
				MeshObjectPath,
				TransformReadInput(TEXT("mesh.geometry.local"), TEXT("7"))),
			MeshReloaded)
		|| !TestTrue(
			TEXT("public query independently observes reloaded mesh geometry"),
			TryReadSpatialComponents(
				MeshReloaded,
				TEXT("mesh_vertex:0"),
				MeshReloadedVertex)
				&& MeshReloadedVertex.Num() >= 3
				&& FMath::IsNearlyEqual(MeshReloadedVertex[0], 10.0, 0.01)
				&& FMath::IsNearlyEqual(MeshReloadedVertex[1], 20.0, 0.01)
				&& FMath::IsNearlyEqual(MeshReloadedVertex[2], 30.0, 0.01)))
	{
		return false;
	}
	return !HasAnyErrors();
}

#endif
