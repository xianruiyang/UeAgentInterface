#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "EditorLevelUtils.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PCGComponent.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Server/UeAgentCommandRegistry.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition::DestructiveImport;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::Tests::PublicCommand;

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<
			TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FUeAgentCommandContract ExpectedContract(const FString& CommandId)
	{
		TArray<FUeAgentCommandParameter> Parameters;
		auto String = [&Parameters](const TCHAR* Name)
		{
			Parameters.Emplace(
				Name, EUeAgentCommandWireType::String, true, false);
		};
		auto Integer = [&Parameters](const TCHAR* Name)
		{
			Parameters.Emplace(
				Name, EUeAgentCommandWireType::Integer, true, false);
		};
		if (CommandId == TEXT("world.data_layer.delete"))
		{
			String(TEXT("layer"));
		}
		else if (CommandId == TEXT("world.level_content.delete_scope"))
		{
			String(TEXT("folder_path"));
		}
		else if (CommandId == TEXT("world.pcg.cleanup"))
		{
			String(TEXT("actor_ref"));
			String(TEXT("component_name"));
		}
		else if (CommandId == TEXT("world.level_streaming.move_actors"))
		{
			String(TEXT("actor_ref"));
			String(TEXT("level_package"));
		}
		else if (CommandId == TEXT("world.level_streaming.remove"))
		{
			String(TEXT("level_package"));
		}
		else if (CommandId == TEXT("modeling.actor.mesh.replace"))
		{
			String(TEXT("actor_ref"));
			String(TEXT("mesh_ref"));
		}
		else if (CommandId == TEXT("modeling.asset.material_slot.remove"))
		{
			String(TEXT("target_ref"));
			Integer(TEXT("index"));
		}
		else if (CommandId == TEXT("modeling.voxel.bounds.remove"))
		{
			String(TEXT("actor_ref"));
			String(TEXT("component_name"));
			String(TEXT("space"));
			for (const TCHAR* Name : {
				TEXT("min_x"), TEXT("min_y"), TEXT("min_z"),
				TEXT("max_x"), TEXT("max_y"), TEXT("max_z")})
			{
				Integer(Name);
			}
		}
		FString Namespace;
		FString Method;
		CommandId.Split(TEXT("."), &Namespace, &Method);
		Method.ReplaceInline(TEXT("."), TEXT("_"));
		return FUeAgentCommandContract(
			CommandId, Namespace, Method, MoveTemp(Parameters),
			1, 1, EUeAgentCommandStatus::Active);
	}

	bool TryInvoke(
		const FScopedPublicCommandComposition& Harness,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		TOptional<FUeAgentCommandResult>& OutResult,
		TSharedPtr<FJsonObject>& OutPayload,
		FString& OutError)
	{
		OutResult.Reset();
		OutPayload.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
		if (!TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, OutError)
			|| !Registry.IsValid())
		{
			return false;
		}
		const FUeAgentCommandContract Expected = ExpectedContract(CommandId);
		const FUeAgentCommandContract* Contract = Registry->FindExact(CommandId);
		if (!Contract)
		{
			Contract = &Expected;
		}
		const FString RequestId = TEXT("world-production-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"") + Contract->GetBindingHash()
			+ TEXT("\",\"command_id\":\"") + CommandId
			+ TEXT("\",\"correlation_id\":\"") + CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",\"params\":")
			+ Serialize(Params)
			+ TEXT(",\"request_id\":\"") + RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
		FUeAgentHttpRequest Request;
		Request.Method = TEXT("POST");
		Request.RoutePath = GetUaiCommandRoutePath();
		Request.Headers.Add(
			TEXT("X-UeAgentInterface-Token"), Harness.GetConfig().AuthToken);
		Request.Headers.Add(
			TEXT("X-Transport-Correlation"), CorrelationId);
		Request.Body = Utf8Bytes(Body);
		Request.PeerId = TEXT("world-production-test-peer");
		const FUeAgentHttpResponse Response =
			Harness.GetRouteHost().Invoke(Request);
		if (Response.StatusCode != 200)
		{
			OutError = FString::Printf(
				TEXT("command_http_status:%d"), Response.StatusCode);
			return false;
		}
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		FUeAgentCommandProtocolCodec Codec;
		if (!Codec.DecodeResult(Response.Body, OutResult, Diagnostic)
			|| !OutResult.IsSet())
		{
			OutError = Diagnostic.Code + TEXT(":") + Diagnostic.Message;
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(OutResult->GetPayloadJson());
		return FJsonSerializer::Deserialize(Reader, OutPayload)
			&& OutPayload.IsValid();
	}

	class FOwnedWorld final
	{
	public:
		FOwnedWorld()
		{
			const FString FixtureId =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/WorldDestructive/")
					+ FixtureId));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview, false,
				FName(TEXT("UAIWorldDestructive")), Package);
			if (World && GEngine)
			{
				GEngine->CreateNewWorldContext(EWorldType::EditorPreview)
					.SetCurrentWorld(World);
				bWorldContextRegistered = true;
			}
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->PersistentLevel->SetUseExternalActors(true);
			WorldPartition = UWorldPartition::CreateOrRepairWorldPartition(
				World->GetWorldSettings());
			if (WorldPartition && !WorldPartition->IsInitialized())
			{
				WorldPartition->Initialize(World, FTransform::Identity);
			}
			World->SetCurrentLevel(World->PersistentLevel);
			WorldDataLayers = World->GetWorldDataLayers();
			DataLayerAsset = NewObject<UDataLayerAsset>(
				Package, TEXT("OwnedDataLayer"), RF_Transactional);
			if (DataLayerAsset)
			{
				DataLayerAsset->SetType(EDataLayerType::Runtime);
			}
			if (WorldDataLayers && DataLayerAsset)
			{
				DataLayer = WorldDataLayers->CreateDataLayer<
					UDataLayerInstanceWithAsset>(DataLayerAsset);
			}
			DeleteActor = World->SpawnActor<AActor>();
			if (DeleteActor)
			{
				DeleteActor->SetFolderPath(TEXT("UAI/W08/Delete"));
			}
			PcgActor = World->SpawnActor<AActor>();
			if (PcgActor)
			{
				PcgComponent = NewObject<UPCGComponent>(
					PcgActor, TEXT("OwnedPCG"), RF_Transactional);
				PcgActor->AddInstanceComponent(PcgComponent);
				PcgComponent->RegisterComponent();
			}
			MoveActor = World->SpawnActor<AActor>();
			MoveActorLabel = TEXT("UAI_W08_Move_") + FixtureId;
			if (MoveActor)
			{
				MoveActor->SetActorLabel(MoveActorLabel);
			}
			StreamingAsset = LoadObject<UWorld>(
				nullptr,
				TEXT("/Engine/Maps/Templates/Template_Default.Template_Default"));
			if (StreamingAsset)
			{
				StreamingLevel = UEditorLevelUtils::AddLevelToWorld(
					World, *StreamingAsset->GetOutermost()->GetName(),
					ULevelStreamingDynamic::StaticClass(), FTransform::Identity);
			}
			MeshActor = World->SpawnActor<AStaticMeshActor>();
			MeshActorLabel = TEXT("UAI_W08_Mesh_") + FixtureId;
			if (MeshActor)
			{
				MeshActor->SetActorLabel(MeshActorLabel);
			}
			Cube = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			Sphere = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (MeshActor && Cube)
			{
				MeshActor->GetStaticMeshComponent()->SetStaticMesh(Cube);
				MeshActorPath = MeshActor->GetPathName();
			}
			SpherePath = Sphere ? Sphere->GetPathName() : FString();
			MeshPackageName = TEXT("/Game/UAI/Tests/WorldDestructive/")
				+ FixtureId + TEXT("/OwnedMesh");
			UPackage* const MeshPackage = CreatePackage(*MeshPackageName);
			OwnedMesh = Cube && MeshPackage
				? DuplicateObject<UStaticMesh>(
					Cube, MeshPackage, TEXT("OwnedMesh"))
				: nullptr;
			if (OwnedMesh)
			{
				OwnedMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
				OwnedMesh->GetStaticMaterials().Add(FStaticMaterial());
				OwnedMeshInitialSlotCount =
					OwnedMesh->GetStaticMaterials().Num();
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				const FString Filename =
					FPackageName::LongPackageNameToFilename(
						MeshPackageName,
						FPackageName::GetAssetPackageExtension());
				bMeshSaved = UPackage::SavePackage(
					MeshPackage, OwnedMesh, *Filename, SaveArgs);
				OwnedMeshPath = OwnedMesh->GetPathName();
			}
			VoxelActor = World->SpawnActor<AActor>();
			VoxelActorLabel = TEXT("UAI_W08_Voxel_") + FixtureId;
			if (VoxelActor)
			{
				VoxelActor->SetActorLabel(VoxelActorLabel);
			}
			if (VoxelActor && Cube)
			{
				VoxelComponent = NewObject<UInstancedStaticMeshComponent>(
					VoxelActor, TEXT("OwnedVoxelGrid"), RF_Transactional);
				VoxelActor->AddInstanceComponent(VoxelComponent);
				VoxelComponent->SetStaticMesh(Cube);
				VoxelComponent->RegisterComponent();
				VoxelComponent->AddInstance(FTransform(FVector(0, 0, 0)));
				VoxelComponent->AddInstance(FTransform(FVector(100, 0, 0)));
				VoxelComponent->AddInstance(FTransform(FVector(300, 0, 0)));
				VoxelActorPath = VoxelActor->GetPathName();
				VoxelComponentName = VoxelComponent->GetName();
			}
			Package->SetDirtyFlag(false);
		}

		~FOwnedWorld()
		{
			CleanupMesh();
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				if (bWorldContextRegistered && GEngine)
				{
					GEngine->DestroyWorldContext(World);
					bWorldContextRegistered = false;
				}
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		}

		bool IsValid() const
		{
			return Package && World && bWorldContextRegistered
				&& WorldPartition && DataLayer
				&& DeleteActor && PcgActor && PcgComponent && MoveActor
				&& StreamingLevel && StreamingLevel->GetLoadedLevel()
				&& MeshActor && Cube && Sphere && OwnedMesh && bMeshSaved
				&& VoxelActor && VoxelComponent
				&& VoxelComponent->GetInstanceCount() == 3;
		}

		int32 ReloadedMaterialSlotCount()
		{
			OwnedMesh = nullptr;
			if (UPackage* const Loaded = FindPackage(nullptr, *MeshPackageName))
			{
				Loaded->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Loaded}, Error, true))
				{
					return INDEX_NONE;
				}
			}
			CollectGarbage(RF_NoFlags, true);
			UStaticMesh* const Reloaded = LoadObject<UStaticMesh>(
				nullptr, *OwnedMeshPath);
			return Reloaded
				? Reloaded->GetStaticMaterials().Num() : INDEX_NONE;
		}

		AStaticMeshActor* FindMeshActor() const
		{
			return Cast<AStaticMeshActor>(FindActorByLabel(MeshActorLabel));
		}

		AActor* FindMoveActor() const
		{
			return FindActorByLabel(MoveActorLabel);
		}

		UStaticMesh* FindSphere() const
		{
			return LoadObject<UStaticMesh>(nullptr, *SpherePath);
		}

		UInstancedStaticMeshComponent* FindVoxelComponent() const
		{
			AActor* const Actor = FindActorByLabel(VoxelActorLabel);
			return Actor
				? FindObject<UInstancedStaticMeshComponent>(
					Actor, *VoxelComponentName)
				: nullptr;
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		UWorldPartition* WorldPartition = nullptr;
		AWorldDataLayers* WorldDataLayers = nullptr;
		UDataLayerAsset* DataLayerAsset = nullptr;
		UDataLayerInstance* DataLayer = nullptr;
		AActor* DeleteActor = nullptr;
		AActor* PcgActor = nullptr;
		UPCGComponent* PcgComponent = nullptr;
		AActor* MoveActor = nullptr;
		FString MoveActorLabel;
		UWorld* StreamingAsset = nullptr;
		ULevelStreaming* StreamingLevel = nullptr;
		AStaticMeshActor* MeshActor = nullptr;
		FString MeshActorPath;
		FString MeshActorLabel;
		UStaticMesh* Cube = nullptr;
		UStaticMesh* Sphere = nullptr;
		FString SpherePath;
		UStaticMesh* OwnedMesh = nullptr;
		FString MeshPackageName;
		FString OwnedMeshPath;
		bool bMeshSaved = false;
		int32 OwnedMeshInitialSlotCount = 0;
		bool bWorldContextRegistered = false;
		AActor* VoxelActor = nullptr;
		UInstancedStaticMeshComponent* VoxelComponent = nullptr;
		FString VoxelActorPath;
		FString VoxelActorLabel;
		FString VoxelComponentName;

	private:
		AActor* FindActorByLabel(const FString& Label) const
		{
			if (!World)
			{
				return nullptr;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == Label)
				{
					return *It;
				}
			}
			return nullptr;
		}

		void CleanupMesh()
		{
			OwnedMesh = nullptr;
			if (OwnedMeshPath.IsEmpty() || MeshPackageName.IsEmpty())
			{
				return;
			}
			if (UObject* const Asset = LoadObject<UObject>(
					nullptr, *OwnedMeshPath))
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				ObjectTools::DeleteObjectsUnchecked({Asset});
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				MeshPackageName, FPackageName::GetAssetPackageExtension());
			IFileManager::Get().Delete(*Filename, false, true, true);
		}
	};

	TSharedRef<FJsonObject> Params()
	{
		return MakeShared<FJsonObject>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportWorldProductionTest,
	"UeAgentInterface.Composition.DestructiveImport.WorldProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportWorldProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorld Fixture;
	if (!TestTrue(TEXT("owned world fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	FScopedPublicCommandComposition Harness(TEXT("world-destructive-production"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*Fixture.World->GetWorldSettings(), Error)))
	{
		AddError(Error);
		return false;
	}

	auto Invoke = [this, &Harness, &Error](
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Input,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		TOptional<FUeAgentCommandResult> Result;
		const bool bInvoked = TryInvoke(
			Harness, CommandId, Input, Result, OutPayload, Error);
		TestTrue(TEXT("formal route executes: ") + CommandId,
			bInvoked && Result.IsSet());
		if (!bInvoked || !Result.IsSet())
		{
			AddError(CommandId + TEXT(":") + Error);
			return false;
		}
		if (!Result->IsOk())
		{
			AddError(CommandId + TEXT(":") + Result->GetErrorJson());
			return false;
		}
		TestEqual(TEXT("terminal state committed: ") + CommandId,
			OutPayload->GetStringField(TEXT("terminal_state")), TEXT("committed"));
		return true;
	};

	TSharedPtr<FJsonObject> Payload;
	{
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(
			TEXT("layer"), Fixture.DataLayer->GetDataLayerFName().ToString());
		Invoke(TEXT("world.data_layer.delete"), Input, Payload);
		TestNull(TEXT("DataLayer independent absence readback"),
			Fixture.WorldDataLayers->GetDataLayerInstance(
				Fixture.DataLayer->GetDataLayerFName()));
	}
	{
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("folder_path"), TEXT("UAI/W08/Delete"));
		Invoke(TEXT("world.level_content.delete_scope"), Input, Payload);
		bool bFound = false;
		for (TActorIterator<AActor> It(Fixture.World); It; ++It)
		{
			bFound |= It->GetFolderPath() == FName(TEXT("UAI/W08/Delete"));
		}
		TestFalse(TEXT("folder actors are absent after independent scan"), bFound);
	}
	{
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("actor_ref"), Fixture.PcgActor->GetPathName());
		Input->SetStringField(
			TEXT("component_name"), Fixture.PcgComponent->GetName());
		Invoke(TEXT("world.pcg.cleanup"), Input, Payload);
		TestFalse(TEXT("PCG component is not generating after cleanup"),
			Fixture.PcgComponent->IsGenerating());
	}
	{
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("actor_ref"), Fixture.MoveActor->GetPathName());
		Input->SetStringField(TEXT("level_package"),
			Fixture.StreamingLevel->GetWorldAssetPackageName());
		Invoke(TEXT("world.level_streaming.move_actors"), Input, Payload);
		AActor* const MovedActor = Fixture.FindMoveActor();
		TestTrue(TEXT("actor relation readback points at target level"),
			MovedActor
				&& MovedActor->GetLevel()
					== Fixture.StreamingLevel->GetLoadedLevel());
	}
	{
		AStaticMeshActor* const MeshActor = Fixture.FindMeshActor();
		UStaticMesh* const Sphere = Fixture.FindSphere();
		if (!TestNotNull(TEXT("mesh actor resolved by stable label"), MeshActor)
			|| !TestNotNull(TEXT("replacement mesh reacquired"), Sphere))
		{
			return false;
		}
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("actor_ref"), Fixture.MeshActorPath);
		Input->SetStringField(TEXT("mesh_ref"), Fixture.SpherePath);
		Invoke(TEXT("modeling.actor.mesh.replace"), Input, Payload);
		AStaticMeshActor* const ReadbackActor = Fixture.FindMeshActor();
		TestTrue(TEXT("actor mesh readback uses replacement"),
			ReadbackActor && ReadbackActor->GetStaticMeshComponent()
				&& ReadbackActor->GetStaticMeshComponent()->GetStaticMesh()
					== Sphere);
	}
	{
		const int32 Before = Fixture.OwnedMeshInitialSlotCount;
		const TSharedRef<FJsonObject> Invalid = Params();
		Invalid->SetStringField(TEXT("target_ref"), Fixture.OwnedMeshPath);
		Invalid->SetNumberField(TEXT("index"), Before + 10);
		TOptional<FUeAgentCommandResult> FailedResult;
		TSharedPtr<FJsonObject> FailedPayload;
		TestTrue(TEXT("invalid material removal reaches formal route"),
			TryInvoke(
				Harness, TEXT("modeling.asset.material_slot.remove"),
				Invalid, FailedResult, FailedPayload, Error));
		TestTrue(TEXT("invalid material removal returns a result"),
			FailedResult.IsSet() && FailedPayload.IsValid());
		if (FailedResult.IsSet() && FailedPayload.IsValid())
		{
			TestFalse(TEXT("invalid material removal fails closed"),
				FailedResult->IsOk());
			TestEqual(TEXT("pre-mutation failure is rolled_back"),
				FailedPayload->GetStringField(TEXT("terminal_state")),
				TEXT("rolled_back"));
		}
		TestEqual(TEXT("failed removal leaves durable asset unchanged"),
			Fixture.ReloadedMaterialSlotCount(), Before);

		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("target_ref"), Fixture.OwnedMeshPath);
		Input->SetNumberField(TEXT("index"), Before - 1);
		Invoke(TEXT("modeling.asset.material_slot.remove"), Input, Payload);
		TestEqual(TEXT("material slot durable independent reload readback"),
			Fixture.ReloadedMaterialSlotCount(), Before - 1);
	}
	{
		UInstancedStaticMeshComponent* const VoxelComponent =
			Fixture.FindVoxelComponent();
		if (!TestNotNull(TEXT("voxel component reacquired"), VoxelComponent))
		{
			return false;
		}
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("actor_ref"), Fixture.VoxelActorPath);
		Input->SetStringField(
			TEXT("component_name"), Fixture.VoxelComponentName);
		Input->SetStringField(TEXT("space"), TEXT("component_local"));
		Input->SetNumberField(TEXT("min_x"), -1);
		Input->SetNumberField(TEXT("min_y"), -1);
		Input->SetNumberField(TEXT("min_z"), -1);
		Input->SetNumberField(TEXT("max_x"), 150);
		Input->SetNumberField(TEXT("max_y"), 1);
		Input->SetNumberField(TEXT("max_z"), 1);
		Invoke(TEXT("modeling.voxel.bounds.remove"), Input, Payload);
		UInstancedStaticMeshComponent* const ReadbackVoxel =
			Fixture.FindVoxelComponent();
		TestEqual(TEXT("component-local voxel absence readback"),
			ReadbackVoxel ? ReadbackVoxel->GetInstanceCount() : INDEX_NONE, 1);
	}
	{
		const FString LevelPackage =
			Fixture.StreamingLevel->GetWorldAssetPackageName();
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("level_package"), LevelPackage);
		Invoke(TEXT("world.level_streaming.remove"), Input, Payload);
		bool bPresent = false;
		for (ULevelStreaming* const Streaming : Fixture.World->GetStreamingLevels())
		{
			bPresent |= Streaming
				&& Streaming->GetWorldAssetPackageName() == LevelPackage;
		}
		TestFalse(TEXT("streaming relation is absent after independent scan"),
			bPresent);
	}

	const TArray<FString> RetiredLegacyIds{
		TEXT("data_layer_delete_instance"),
		TEXT("level_content_delete_scope"),
		TEXT("pcg_cleanup"),
		TEXT("level_streaming_move_actors"),
		TEXT("level_streaming_remove"),
		TEXT("modeling_replace_actor_mesh"),
		TEXT("modeling_remove_material_slot"),
		TEXT("modeling_voxel_grid_remove_bounds"),
	};
	for (const FString& LegacyId : RetiredLegacyIds)
	{
		TestNull(TEXT("retired exact world owner absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("retired prefix world owner absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("retired world ID fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}
	TestTrue(TEXT("formal public composition stops"), Harness.Stop());
	TestEqual(TEXT("public route cleanup"),
		Harness.GetRouteHost().Handlers.Num(), 0);
	return !HasAnyErrors();
}
