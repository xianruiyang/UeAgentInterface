#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
	using namespace UE::UAI;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition::SimpleAuthoring::OperationHost;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	struct FOwnedWorld final
	{
		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		bool bHasWorldContext = false;
		FString PackageName;
		FString Filename;
		TArray<FString> AdditionalPackageNames;
		TArray<FString> AdditionalPackageFiles;
	};

	class FOwnedWorldPair final
	{
	public:
		FOwnedWorldPair()
		{
			const FString Unique = FGuid::NewGuid().ToString(
				EGuidFormats::Digits);
			Create(
				TEXT("/Game/UAI_Automation/T47A/RuntimeWorldRebindA_")
					+ Unique,
				First);
			Create(
				TEXT("/Game/UAI_Automation/T47A/RuntimeWorldRebindB_")
					+ Unique,
				Second);
		}

		~FOwnedWorldPair()
		{
			SetAutomationWorldOverride(nullptr);
			Destroy(Second);
			Destroy(First);
			CollectGarbage(RF_NoFlags, true);
			DeletePackageFile(Second);
			DeletePackageFile(First);
		}

		bool IsValid() const
		{
			return First.Package
				&& First.World
				&& First.World->PersistentLevel
				&& First.bHasWorldContext
				&& Second.Package
				&& Second.World
				&& Second.World->PersistentLevel
				&& Second.bHasWorldContext;
		}

		FOwnedWorld First;
		FOwnedWorld Second;

		void TrackPackageFiles(
			FOwnedWorld& Owned,
			const FString& PackageName)
		{
			Owned.AdditionalPackageNames.AddUnique(PackageName);
			FString Header;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					PackageName,
					Header,
					FPackageName::GetAssetPackageExtension()))
			{
				return;
			}
			Header = FPaths::ConvertRelativePathToFull(Header);
			Owned.AdditionalPackageFiles.AddUnique(Header);
			for (const TCHAR* Extension : {
				TEXT("uexp"),
				TEXT("ubulk"),
				TEXT("upayload"),
			})
			{
				Owned.AdditionalPackageFiles.AddUnique(
					FPaths::ChangeExtension(Header, Extension));
			}
		}

		bool Reload(
			FOwnedWorld& Owned,
			FString& OutError)
		{
			OutError.Reset();
			if (!Owned.Package
				|| !Owned.World
				|| Owned.PackageName.IsEmpty())
			{
				OutError = TEXT("owned_world_reload_source_missing");
				return false;
			}
			const TWeakObjectPtr<UWorld> OriginalWorld(Owned.World);
			UPackage* const PackageToUnload = Owned.Package;
			TArray<UPackage*> PackagesToUnload{PackageToUnload};
			for (AActor* Actor : Owned.World->PersistentLevel->Actors)
			{
				if (::IsValid(Actor) && Actor->GetExternalPackage())
				{
					PackagesToUnload.AddUnique(
						Actor->GetExternalPackage());
				}
			}
			if (Owned.bHasWorldContext && GEngine)
			{
				GEngine->DestroyWorldContext(Owned.World);
				Owned.bHasWorldContext = false;
			}
			SetAutomationWorldOverride(nullptr);
			Owned.World->DestroyWorld(false);
			Owned.World->MarkAsGarbage();
			for (UPackage* Package : PackagesToUnload)
			{
				Package->SetDirtyFlag(false);
			}
			Owned.World = nullptr;
			Owned.Package = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages(
					PackagesToUnload,
					UnloadError,
					true))
			{
				OutError = TEXT("owned_world_unload_failed:")
					+ UnloadError.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (OriginalWorld.IsValid())
			{
				OutError = TEXT("owned_world_reload_source_still_live");
				return false;
			}
			UPackage* const LoadedPackage = LoadPackage(
				nullptr,
				*Owned.PackageName,
				LOAD_None);
			UWorld* const LoadedWorld = LoadedPackage
				? UWorld::FindWorldInPackage(LoadedPackage)
				: nullptr;
			if (!LoadedPackage
				|| !LoadedWorld
				|| !LoadedWorld->PersistentLevel)
			{
				OutError = TEXT("owned_world_reload_failed:")
					+ Owned.PackageName;
				return false;
			}
			Owned.Package = LoadedPackage;
			Owned.World = LoadedWorld;
			for (const FString& AdditionalPackageName
				: Owned.AdditionalPackageNames)
			{
				UPackage* const AdditionalPackage = LoadPackage(
					nullptr,
					*AdditionalPackageName,
					LOAD_None);
				if (!AdditionalPackage)
				{
					OutError =
						TEXT("owned_world_external_reload_failed:")
						+ AdditionalPackageName;
					return false;
				}
				ForEachObjectWithPackage(
					AdditionalPackage,
					[LoadedWorld](UObject* Object)
					{
						AActor* const Actor = Cast<AActor>(Object);
						if (Actor
							&& !Actor->IsTemplate()
							&& Actor->GetLevel()
								== LoadedWorld->PersistentLevel)
						{
							LoadedWorld->PersistentLevel
								->Actors.AddUnique(Actor);
						}
						return true;
					});
			}
			Owned.World->SetCurrentLevel(
				Owned.World->PersistentLevel);
			if (GEngine)
			{
				FWorldContext& Context = GEngine->CreateNewWorldContext(
					EWorldType::EditorPreview);
				Context.SetCurrentWorld(Owned.World);
				Owned.bHasWorldContext = true;
			}
			SetAutomationWorldOverride(Owned.World);
			return true;
		}

	private:
		static void Create(
			const FString& PackageName,
			FOwnedWorld& OutOwned)
		{
			OutOwned.PackageName = PackageName;
			OutOwned.Package = CreatePackage(*PackageName);
			OutOwned.Filename = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetMapPackageExtension());
			if (!OutOwned.Package)
			{
				return;
			}
			OutOwned.World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(*FPackageName::GetLongPackageAssetName(PackageName)),
				OutOwned.Package);
			if (!OutOwned.World || !OutOwned.World->PersistentLevel)
			{
				return;
			}
			OutOwned.World->SetFlags(RF_Public | RF_Standalone);
			OutOwned.World->PersistentLevel->SetUseExternalActors(false);
			OutOwned.World->SetCurrentLevel(
				OutOwned.World->PersistentLevel);
			if (GEngine)
			{
				FWorldContext& Context = GEngine->CreateNewWorldContext(
					EWorldType::EditorPreview);
				Context.SetCurrentWorld(OutOwned.World);
				OutOwned.bHasWorldContext = true;
			}
			OutOwned.Package->SetDirtyFlag(false);
		}

		static void Destroy(FOwnedWorld& Owned)
		{
			if (Owned.World && Owned.bHasWorldContext && GEngine)
			{
				GEngine->DestroyWorldContext(Owned.World);
				Owned.bHasWorldContext = false;
			}
			if (Owned.Package)
			{
				Owned.Package->SetDirtyFlag(false);
			}
			if (Owned.World)
			{
				Owned.World->DestroyWorld(false);
				Owned.World->MarkAsGarbage();
				Owned.World = nullptr;
			}
			if (Owned.Package)
			{
				Owned.Package->MarkAsGarbage();
				Owned.Package = nullptr;
			}
			for (const FString& AdditionalPackageName
				: Owned.AdditionalPackageNames)
			{
				if (UPackage* AdditionalPackage = FindPackage(
						nullptr,
						*AdditionalPackageName))
				{
					AdditionalPackage->SetDirtyFlag(false);
					AdditionalPackage->MarkAsGarbage();
				}
			}
		}

		static void DeletePackageFile(const FOwnedWorld& Owned)
		{
			if (!Owned.Filename.IsEmpty()
				&& IFileManager::Get().FileExists(*Owned.Filename))
			{
				IFileManager::Get().Delete(
					*Owned.Filename,
					false,
					true);
			}
			for (const FString& Filename : Owned.AdditionalPackageFiles)
			{
				if (IFileManager::Get().FileExists(*Filename))
				{
					IFileManager::Get().Delete(
						*Filename,
						false,
						true);
				}
			}
		}
	};

	FUeAgentCommandResult SubmitPublic(
		const TSharedRef<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedRef<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FUeAgentFrozenCommandContract* CommandContract =
			Registry->FindExact(CommandId);
		if (!CommandContract)
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-runtime-world-rebind"),
				TEXT("req-runtime-world-rebind-missing"),
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("runtime_world_rebind_contract_missing"),
				CommandId);
		}
		const FString RequestId = TEXT("req-runtime-world-rebind-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-runtime-world-rebind\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*CommandContract->GetContractHash(),
			*ParamsJson);
		const FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes,
				Request,
				Diagnostic)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-runtime-world-rebind"),
				RequestId,
				CommandId,
				CommandContract->GetContractHash(),
				Diagnostic.Code,
				Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("simple-authoring-runtime-world-rebind"),
				{TEXT("uai:operate"), TEXT("uai:query")},
				TEXT("corr-runtime-world-rebind")),
			MakeShared<FNeverCancelled>());
	}

	bool QueryContainsDirtyResource(
		const FUeAgentCommandResult& Result,
		const FString& ResourceRef,
		const FString& ResourceKind)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetPayloadJson());
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* QueryResult = nullptr;
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TSharedPtr<FJsonObject>* Snapshot = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Root->TryGetObjectField(TEXT("query_result"), QueryResult)
			|| !QueryResult || !QueryResult->IsValid()
			|| !(*QueryResult)->TryGetObjectField(
				TEXT("projection"), Projection)
			|| !Projection || !Projection->IsValid()
			|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload || !Payload->IsValid()
			|| !(*Payload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
			|| !Snapshot || !Snapshot->IsValid()
			|| !(*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
			|| !Entries)
		{
			return false;
		}
		bool bFoundRef = false;
		bool bFoundKind = false;
		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
			const TSharedPtr<FJsonObject>* Identity = nullptr;
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FString OwnerId;
			FString Selector;
			FString Text;
			if (!Entry.IsValid()
				|| !Entry->TryGetObjectField(TEXT("identity"), Identity)
				|| !Identity || !Identity->IsValid()
				|| !(*Identity)->TryGetStringField(TEXT("owner_id"), OwnerId)
				|| OwnerId != ResourceRef
				|| !(*Identity)->TryGetStringField(TEXT("selector"), Selector)
				|| !Entry->TryGetObjectField(TEXT("value"), Value)
				|| !Value || !Value->IsValid()
				|| !(*Value)->TryGetStringField(TEXT("text"), Text))
			{
				continue;
			}
			bFoundRef |= Selector == TEXT("resource_ref")
				&& Text == ResourceRef;
			bFoundKind |= Selector == TEXT("resource_kind")
				&& Text == ResourceKind;
		}
		return bFoundRef && bFoundKind;
	}

	AStaticMeshActor* FindActorWithLabel(
		UWorld& World,
		const FString& Label)
	{
		for (TActorIterator<AStaticMeshActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				return *It;
			}
		}
		return nullptr;
	}

	ANavMeshBoundsVolume* FindNavBoundsVolumeWithLabel(
		UWorld& World,
		const FString& Label)
	{
		for (TActorIterator<ANavMeshBoundsVolume> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				return *It;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeWorldRebindTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Runtime.WorldRebind",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeWorldRebindTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorldPair Owner;
	if (!TestTrue(TEXT("two owned Editor worlds are valid"), Owner.IsValid()))
	{
		return false;
	}

	SetAutomationWorldOverride(Owner.First.World);
	TSharedPtr<IUeAgentCommandHandler> FirstHandler;
	FString Error;
	if (!TestTrue(
			TEXT("the first world composes one exact actor handler"),
			TryResolveHandler(
				TEXT("world.actor.create"),
				FirstHandler,
				Error))
		|| !FirstHandler.IsValid())
	{
		AddError(Error);
		return false;
	}

	SetAutomationWorldOverride(Owner.Second.World, false);
	TSharedPtr<IUeAgentCommandHandler> SecondHandler;
	Error.Reset();
	if (!TestTrue(
			TEXT("the changed world resolves one exact actor handler"),
			TryResolveHandler(
				TEXT("world.actor.create"),
				SecondHandler,
				Error))
		|| !SecondHandler.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("the process host rebuilds its runtime for the changed world"),
		FirstHandler.Get() != SecondHandler.Get());

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	Error.Reset();
	if (!TestTrue(
			TEXT("the public product registry composes"),
			Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString ActorLabel = TEXT("T47ARuntimeWorldRebindActor");
	const FString ParamsJson = FString::Printf(
		TEXT("{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\",")
		TEXT("\"folder_path\":\"UAI/T47A\",\"label\":\"%s\",")
		TEXT("\"placement\":{\"basis_evidence\":\"owned runtime rebind fixture\",")
		TEXT("\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":[0,0,0,1],")
		TEXT("\"scale\":[1,1,1],\"translation\":[25,50,75],")
		TEXT("\"unit\":\"centimeter\"},")
		TEXT("\"tags\":[\"T47ARuntimeWorldRebind\"]}"),
		*ActorLabel);
	const FUeAgentCommandResult Result = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("world.actor.create"),
		ParamsJson);
	if (!Result.IsOk())
	{
		AddInfo(TEXT("public_result_error=") + Result.GetErrorJson());
		AddInfo(TEXT("public_result_payload=") + Result.GetPayloadJson());
	}
	TestTrue(
		TEXT("public actor create succeeds after the world change"),
		Result.IsOk());
	TestNull(
		TEXT("the changed command does not mutate the stale world"),
		FindActorWithLabel(*Owner.First.World, ActorLabel));
	TestNotNull(
		TEXT("the changed command mutates the current world"),
		FindActorWithLabel(*Owner.Second.World, ActorLabel));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeVolumeActorCreateTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Runtime.VolumeActorCreate",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeVolumeActorCreateTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorldPair Owner;
	if (!TestTrue(TEXT("the owned Editor world is valid"), Owner.IsValid()))
	{
		return false;
	}

	SetAutomationWorldOverride(Owner.First.World);
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the public product registry composes"),
			Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString VolumeLabel = TEXT("T47ARuntimeNavBoundsVolume");
	const FString ParamsJson = FString::Printf(
		TEXT("{\"actor_class_ref\":\"/Script/NavigationSystem.NavMeshBoundsVolume\",")
		TEXT("\"folder_path\":\"UAI/T47A\",\"label\":\"%s\",")
		TEXT("\"placement\":{\"basis_evidence\":\"owned volume actor fixture\",")
		TEXT("\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":[0,0,0,1],")
		TEXT("\"scale\":[30,30,8],\"translation\":[0,0,0],")
		TEXT("\"unit\":\"centimeter\"},")
		TEXT("\"tags\":[\"T47AVolumeActorCreate\"]}"),
		*VolumeLabel);
	const FUeAgentCommandResult Result = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("world.actor.create"),
		ParamsJson);
	if (!Result.IsOk())
	{
		AddInfo(TEXT("public_result_error=") + Result.GetErrorJson());
		AddInfo(TEXT("public_result_payload=") + Result.GetPayloadJson());
	}
	TestTrue(
		TEXT("public actor create accepts a supported Volume class"),
		Result.IsOk());
	ANavMeshBoundsVolume* Volume = FindNavBoundsVolumeWithLabel(
		*Owner.First.World,
		VolumeLabel);
	TestNotNull(
		TEXT("public actor create produces the exact Nav bounds actor"),
		Volume);
	if (Volume)
	{
		const FBox Bounds = Volume->GetComponentsBoundingBox(true);
		TestTrue(
			TEXT("the created Volume owns valid non-empty brush bounds"),
			Bounds.IsValid
				&& Bounds.GetExtent().GetMin() > UE_KINDA_SMALL_NUMBER);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeExternalActorDirtyReadbackTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Runtime.ExternalActorDirtyResourceReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeExternalActorDirtyReadbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorldPair Owner;
	if (!TestTrue(TEXT("the owned Editor world is valid"), Owner.IsValid()))
	{
		return false;
	}
	Owner.First.World->PersistentLevel->SetUseExternalActors(true);
	Owner.First.Package->SetDirtyFlag(false);
	SetAutomationWorldOverride(Owner.First.World);

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the public product registry composes"),
			Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.OverrideLevel = Owner.First.World->PersistentLevel;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = Cast<AStaticMeshActor>(
		Owner.First.World->SpawnActor<AActor>(
			AStaticMeshActor::StaticClass(),
			FTransform::Identity,
			SpawnParameters));
	if (!TestNotNull(TEXT("the created actor exists"), Actor))
	{
		return false;
	}
	Actor->SetActorLabel(TEXT("T47AExternalDirtyActor"));
	UPackage* ActorPackage = Actor->GetExternalPackage();
	if (!TestNotNull(
			TEXT("external-actor levels create an actor package"),
			ActorPackage))
	{
		return false;
	}
	ActorPackage->SetDirtyFlag(true);
	TestTrue(
		TEXT("the unsaved external actor package is dirty"),
		ActorPackage->IsDirty());
	TestFalse(
		TEXT("the clean world package remains clean for an external actor edit"),
		Owner.First.Package->IsDirty());

	const FUeAgentCommandResult DirtyResult = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("editor.dirty_resources"),
		TEXT("{\"input\":{\"resource_kinds\":[\"asset\",\"map\"]}}"));
	if (!DirtyResult.IsOk())
	{
		AddInfo(TEXT("dirty_result_error=") + DirtyResult.GetErrorJson());
		AddInfo(TEXT("dirty_result_payload=") + DirtyResult.GetPayloadJson());
	}
	TestTrue(
		TEXT("public dirty-resource readback succeeds"),
		DirtyResult.IsOk());
	TestTrue(
		TEXT("dirty-resource readback reports the external actor package as map state"),
		QueryContainsDirtyResource(
			DirtyResult,
			ActorPackage->GetName(),
			TEXT("map")));
	ActorPackage->SetDirtyFlag(false);
	Owner.First.Package->SetDirtyFlag(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeExternalActorDurablePersistenceTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Runtime.ExternalActorDurablePersistence",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeExternalActorDurablePersistenceTest::
RunTest(const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorldPair Owner;
	if (!TestTrue(TEXT("the owned Editor world is valid"), Owner.IsValid()))
	{
		return false;
	}
	Owner.First.World->PersistentLevel->SetUseExternalActors(true);
	Owner.First.Package->SetDirtyFlag(false);
	SetAutomationWorldOverride(Owner.First.World);

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
		TEXT("the public product registry composes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString ActorLabel = TEXT("T47BExternalDurableActor");
	const FString ParamsJson = FString::Printf(
		TEXT("{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\",")
		TEXT("\"folder_path\":\"UAI/T47B\",\"label\":\"%s\",")
		TEXT("\"placement\":{\"basis_evidence\":\"owned external actor fixture\",")
		TEXT("\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":[0,0,0,1],")
		TEXT("\"scale\":[1,1,1],\"translation\":[50,75,100],")
		TEXT("\"unit\":\"centimeter\"},")
		TEXT("\"tags\":[\"T47BExternalDurableActor\"]}"),
		*ActorLabel);
	const FUeAgentCommandResult CreateResult = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("world.actor.create"),
		ParamsJson);
	if (!CreateResult.IsOk())
	{
		AddInfo(TEXT("create_result_error=") + CreateResult.GetErrorJson());
		AddInfo(TEXT("create_result_payload=") + CreateResult.GetPayloadJson());
	}
	if (!TestTrue(
		TEXT("public actor create persists through the shared lifecycle"),
		CreateResult.IsOk()))
	{
		return false;
	}
	AStaticMeshActor* Actor = FindActorWithLabel(
		*Owner.First.World,
		ActorLabel);
	if (!TestNotNull(TEXT("the created external actor exists"), Actor))
	{
		return false;
	}
	UPackage* const ExternalPackage = Actor->GetExternalPackage();
	if (!TestNotNull(
		TEXT("the actor is owned by an external actor package"),
		ExternalPackage))
	{
		return false;
	}
	const FString ExternalPackageName = ExternalPackage->GetName();
	Owner.TrackPackageFiles(Owner.First, ExternalPackageName);
	FString ExternalHeader;
	TestTrue(
		TEXT("the external package resolves to a durable artifact path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			ExternalPackageName,
			ExternalHeader,
			FPackageName::GetAssetPackageExtension()));
	ExternalHeader = FPaths::ConvertRelativePathToFull(ExternalHeader);
	TestTrue(
		TEXT("the external actor package header was published"),
		IFileManager::Get().FileExists(*ExternalHeader));
	TestFalse(
		TEXT("the published external actor package is clean"),
		ExternalPackage->IsDirty());
	TestFalse(
		TEXT("the world package is clean after the same transaction"),
		Owner.First.Package->IsDirty());

	const FUeAgentCommandResult DirtyResult = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("editor.dirty_resources"),
		TEXT("{\"input\":{\"resource_kinds\":[\"asset\",\"map\"]}}"));
	TestTrue(
		TEXT("public dirty-resource readback succeeds"),
		DirtyResult.IsOk());
	TestFalse(
		TEXT("dirty-resource readback excludes the persisted external package"),
		QueryContainsDirtyResource(
			DirtyResult,
			ExternalPackageName,
			TEXT("map")));

	const TWeakObjectPtr<AStaticMeshActor> OriginalActor(Actor);
	FString ReloadError;
	if (!TestTrue(
		TEXT("the owned map unloads and reloads from its durable artifacts"),
		Owner.Reload(Owner.First, ReloadError)))
	{
		AddError(ReloadError);
		return false;
	}
	TestFalse(
		TEXT("reload does not reuse the live actor instance"),
		OriginalActor.IsValid());
	AStaticMeshActor* const ReloadedActor = FindActorWithLabel(
		*Owner.First.World,
		ActorLabel);
	if (!TestNotNull(
		TEXT("the actor is independently readable after reload"),
		ReloadedActor))
	{
		return false;
	}
	UPackage* const ReloadedExternalPackage =
		ReloadedActor->GetExternalPackage();
	if (!TestNotNull(
		TEXT("the reloaded actor still has its external package"),
		ReloadedExternalPackage))
	{
		return false;
	}
	TestEqual(
		TEXT("reload preserves the exact durable package identity"),
		ReloadedExternalPackage->GetName(),
		ExternalPackageName);
	TestFalse(
		TEXT("the independently reloaded external package is clean"),
		ReloadedExternalPackage->IsDirty());
	Owner.First.Package->SetDirtyFlag(false);
	ReloadedExternalPackage->SetDirtyFlag(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeInternalActorDurablePersistenceTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Runtime.InternalActorDurablePersistence",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeInternalActorDurablePersistenceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedWorldPair Owner;
	if (!TestTrue(TEXT("the owned Editor world is valid"), Owner.IsValid()))
	{
		return false;
	}
	Owner.First.World->PersistentLevel->SetUseExternalActors(false);
	Owner.First.Package->SetDirtyFlag(false);
	SetAutomationWorldOverride(Owner.First.World);

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the public product registry composes"),
			Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString ActorLabel = TEXT("T47AInternalDirtyActor");
	const FString ParamsJson = FString::Printf(
		TEXT("{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\",")
		TEXT("\"folder_path\":\"UAI/T47A\",\"label\":\"%s\",")
		TEXT("\"placement\":{\"basis_evidence\":\"owned internal actor fixture\",")
		TEXT("\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":[0,0,0,1],")
		TEXT("\"scale\":[1,1,1],\"translation\":[0,0,0],")
		TEXT("\"unit\":\"centimeter\"},")
		TEXT("\"tags\":[\"T47AInternalDirtyActor\"]}"),
		*ActorLabel);
	const FUeAgentCommandResult CreateResult = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("world.actor.create"),
		ParamsJson);
	if (!CreateResult.IsOk())
	{
		AddInfo(TEXT("create_result_error=") + CreateResult.GetErrorJson());
		AddInfo(TEXT("create_result_payload=") + CreateResult.GetPayloadJson());
	}
	TestTrue(
		TEXT("public actor create succeeds through the durable authoring path"),
		CreateResult.IsOk());
	AStaticMeshActor* Actor = FindActorWithLabel(
		*Owner.First.World,
		ActorLabel);
	if (!TestNotNull(TEXT("the created actor exists"), Actor))
	{
		return false;
	}
	TestNull(
		TEXT("the internal actor does not own an external package"),
		Actor->GetExternalPackage());
	TestFalse(
		TEXT("durable authoring leaves the persisted world package clean"),
		Owner.First.Package->IsDirty());

	const FUeAgentCommandResult DirtyResult = SubmitPublic(
		Registry.ToSharedRef(),
		Dispatch.ToSharedRef(),
		TEXT("editor.dirty_resources"),
		TEXT("{\"input\":{\"resource_kinds\":[\"asset\",\"map\"]}}"));
	if (!DirtyResult.IsOk())
	{
		AddInfo(TEXT("dirty_result_error=") + DirtyResult.GetErrorJson());
		AddInfo(TEXT("dirty_result_payload=") + DirtyResult.GetPayloadJson());
	}
	TestTrue(
		TEXT("public dirty-resource readback succeeds"),
		DirtyResult.IsOk());
	TestFalse(
		TEXT("dirty-resource readback excludes the durably persisted world package"),
		QueryContainsDirtyResource(
			DirtyResult,
			Owner.First.Package->GetName(),
			TEXT("map")));
	Owner.First.Package->SetDirtyFlag(false);
	return !HasAnyErrors();
}
