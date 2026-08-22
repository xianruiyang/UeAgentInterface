#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGridState.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid;

	class FNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

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

	TSharedRef<FJsonObject> Vector(
		const double X,
		const double Y,
		const double Z)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("x"), X);
		Result->SetNumberField(TEXT("y"), Y);
		Result->SetNumberField(TEXT("z"), Z);
		return Result;
	}

	TSharedRef<FJsonObject> Rotator(
		const double Pitch,
		const double Yaw,
		const double Roll)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("pitch"), Pitch);
		Result->SetNumberField(TEXT("yaw"), Yaw);
		Result->SetNumberField(TEXT("roll"), Roll);
		return Result;
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* const Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		const FString RequestId = TEXT("voxel-grid-public-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("voxel-grid-public"),
				RequestId,
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("voxel_grid_public_contract_missing"),
				CommandId);
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"voxel-grid-public\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Contract->GetContractHash(),
			*Serialize(Params));
		const FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("voxel-grid-public"),
				RequestId,
				CommandId,
				Contract->GetContractHash(),
				Diagnostic.Code,
				Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			UE::UAI::Contract::FUeAgentCallerContext(
				TEXT("voxel-grid-public"),
				{
					TEXT("uai:query"),
					TEXT("uai:operate"),
					TEXT("uai:modeling-geometry:write"),
				},
				TEXT("voxel-grid-public")),
			MakeShared<FNeverCancelled>());
	}

	bool ParsePayload(
		FAutomationTestBase& Test,
		const TCHAR* Label,
		const FUeAgentCommandResult& Result,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		OutPayload.Reset();
		if (!Test.TestTrue(Label, Result.IsOk()))
		{
			Test.AddError(Result.GetErrorJson());
			return false;
		}
		if (!Test.TestTrue(
				*(FString(Label) + TEXT(" payload parses")),
				FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Result.GetPayloadJson()),
					OutPayload)
					&& OutPayload.IsValid()))
		{
			Test.AddError(Result.GetPayloadJson());
			return false;
		}
		return true;
	}

	class FScopedVoxelGridFixture final
	{
	public:
		explicit FScopedVoxelGridFixture(const FString& Suffix)
			: PackageName(
				TEXT("/Game/UAI_Automation/VoxelGrid/Public_") + Suffix)
			, Filename(FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetMapPackageExtension()))
		{
			Package = CreatePackage(*PackageName);
			World = Package
				? UWorld::CreateWorld(
					EWorldType::EditorPreview,
					false,
					FName(*FPackageName::GetLongPackageAssetName(PackageName)),
					Package,
					false)
				: nullptr;
			if (!World || !World->PersistentLevel) return;
			World->SetFlags(RF_Public | RF_Standalone);
			World->PersistentLevel->SetUseExternalActors(false);
			World->SetCurrentLevel(World->PersistentLevel);
			FWorldContext& Context =
				GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
			Context.SetCurrentWorld(World);
			bWorldContextCreated = true;
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			bInitialSaveSucceeded = UPackage::SavePackage(
				Package, World, *Filename, Args);
			Package->SetDirtyFlag(false);
			SetAutomationWorldOverride(World);
		}

		~FScopedVoxelGridFixture()
		{
			Cleanup();
		}

		bool IsValid() const
		{
			return Package && World && World->PersistentLevel
				&& bWorldContextCreated && bInitialSaveSucceeded
				&& IFileManager::Get().FileExists(*Filename);
		}

		UWorld* GetWorld() const { return World; }
		UPackage* GetPackage() const { return Package; }
		const FString& GetFilename() const { return Filename; }

		FString LegacySidecarPath(const FString& GridId) const
		{
			return FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/VoxelGrids")
				/ (GridId + TEXT(".json"));
		}

		bool Save()
		{
			if (!Package || !World) return false;
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const bool bSaved = UPackage::SavePackage(
				Package, World, *Filename, Args);
			Package->SetDirtyFlag(false);
			return bSaved;
		}

		bool Cleanup()
		{
			if (bCleaned) return bCleanupSucceeded;
			bCleaned = true;
			ResetProcessSessionsForAutomation();
			SetAutomationWorldOverride(nullptr);
			if (Package) Package->SetDirtyFlag(false);
			if (World && bWorldContextCreated)
			{
				GEngine->DestroyWorldContext(World);
				bWorldContextCreated = false;
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package) Package->MarkAsGarbage();
			if (IFileManager::Get().FileExists(*Filename))
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
			if (Package)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages({Package}, UnloadError, true);
			}
			CollectGarbage(RF_NoFlags, true);
			if (IFileManager::Get().FileExists(*Filename))
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
			bCleanupSucceeded =
				!IFileManager::Get().FileExists(*Filename)
				&& !FindPackage(nullptr, *PackageName);
			World = nullptr;
			Package = nullptr;
			return bCleanupSucceeded;
		}

	private:
		FString PackageName;
		FString Filename;
		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		bool bWorldContextCreated = false;
		bool bInitialSaveSucceeded = false;
		bool bCleaned = false;
		bool bCleanupSucceeded = false;
	};

	class FScopedLegacySidecar final
	{
	public:
		explicit FScopedLegacySidecar(FString InGridId)
			: GridId(MoveTemp(InGridId))
			, SourcePath(FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/VoxelGrids")
				/ (GridId + TEXT(".json")))
		{
		}

		~FScopedLegacySidecar()
		{
			IFileManager::Get().Delete(*SourcePath, false, true);
			if (!TombstoneDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(
					*TombstoneDirectory, false, true);
			}
		}

		bool Write() const
		{
			const FString Parent = FPaths::GetPath(SourcePath);
			if (!IFileManager::Get().MakeDirectory(*Parent, true)
				&& !IFileManager::Get().DirectoryExists(*Parent))
			{
				return false;
			}
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(
				TEXT("schema"), TEXT("uai.modeling.voxel_grid.v2"));
			Root->SetStringField(TEXT("grid_id"), GridId);
			Root->SetNumberField(TEXT("cell_size"), 75.0);
			Root->SetObjectField(TEXT("grid_origin"), Vector(300.0, 40.0, 20.0));
			Root->SetObjectField(TEXT("grid_rotation"), Rotator(0.0, 15.0, 0.0));
			Root->SetObjectField(TEXT("anchor"), Vector(0.0, 0.0, 37.5));
			Root->SetNumberField(TEXT("revision"), 4);
			Root->SetArrayField(TEXT("cells"), {
				MakeShared<FJsonValueObject>(Vector(0.0, 0.0, 0.0)),
				MakeShared<FJsonValueObject>(Vector(0.0, 0.0, 1.0)),
			});
			Root->SetStringField(TEXT("collision_generation"), FString());
			Root->SetStringField(TEXT("collision_mode"), TEXT("none"));
			return FFileHelper::SaveStringToFile(
				Serialize(Root),
				*SourcePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void BindReceipt(const FString& ReceiptId)
		{
			TombstoneDirectory = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/VoxelGridMigrationTombstones")
				/ ReceiptId;
		}

		const FString& GetSourcePath() const { return SourcePath; }

		FString GetTombstonePath() const
		{
			return TombstoneDirectory.IsEmpty()
				? FString()
				: TombstoneDirectory / FPaths::GetCleanFilename(SourcePath);
		}

	private:
		FString GridId;
		FString SourcePath;
		FString TombstoneDirectory;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingVoxelGridPublicVerticalTest,
	"UeAgentInterface.ModelingGeometry.VoxelGrid.PublicVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingVoxelGridPublicVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16);
	const FString GridId = TEXT("VG_TEST_") + Suffix;
	const FString SessionId = TEXT("VGS_TEST_") + Suffix;
	FScopedVoxelGridFixture Fixture(Suffix);
	if (!TestTrue(TEXT("owned persisted editor world exists"), Fixture.IsValid()))
	{
		return false;
	}
	ResetProcessSessionsForAutomation();

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("formal public composition starts"),
			UE::UAI::Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TSharedRef<FJsonObject> Begin = MakeShared<FJsonObject>();
	Begin->SetStringField(TEXT("action"), TEXT("begin"));
	Begin->SetStringField(TEXT("mode"), TEXT("create"));
	Begin->SetStringField(TEXT("session_id"), SessionId);
	Begin->SetStringField(TEXT("grid_id"), GridId);
	Begin->SetStringField(TEXT("label"), TEXT("UAI Voxel Grid Public Test"));
	Begin->SetNumberField(TEXT("cell_size"), 100.0);
	Begin->SetObjectField(TEXT("origin"), Vector(1200.0, -300.0, 80.0));
	Begin->SetObjectField(TEXT("rotation"), Rotator(0.0, 30.0, 0.0));
	Begin->SetObjectField(TEXT("anchor"), Vector(50.0, 50.0, 50.0));
	TSharedPtr<FJsonObject> BeginPayload;
	if (!ParsePayload(
			*this,
			TEXT("public begin succeeds"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Begin),
			BeginPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* BeginReadback = nullptr;
	FString ActorRef;
	double Revision = -1.0;
	if (!TestTrue(
			TEXT("begin returns actor and zero revision"),
			BeginPayload->TryGetObjectField(TEXT("readback"), BeginReadback)
				&& BeginReadback && BeginReadback->IsValid()
				&& (*BeginReadback)->TryGetStringField(TEXT("actor_ref"), ActorRef)
				&& (*BeginReadback)->TryGetNumberField(TEXT("revision"), Revision)
				&& Revision == 0.0))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("action"), TEXT("add_bounds"));
	Add->SetStringField(TEXT("session_id"), SessionId);
	Add->SetNumberField(TEXT("expected_revision"), 0);
	const TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
	Bounds->SetStringField(TEXT("space"), TEXT("grid_local"));
	Bounds->SetObjectField(TEXT("min"), Vector(0.0, 0.0, 0.0));
	Bounds->SetObjectField(TEXT("max"), Vector(200.0, 100.0, 100.0));
	Add->SetObjectField(TEXT("bounds"), Bounds);
	TSharedPtr<FJsonObject> AddPayload;
	if (!ParsePayload(
			*this,
			TEXT("public add_bounds succeeds"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Add),
			AddPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* AddReadback = nullptr;
	double OccupiedCells = -1.0;
	Revision = -1.0;
	TestTrue(
		TEXT("add_bounds publishes two cells at revision one"),
		AddPayload->TryGetObjectField(TEXT("readback"), AddReadback)
			&& AddReadback && AddReadback->IsValid()
			&& (*AddReadback)->TryGetNumberField(
				TEXT("occupied_cell_count"), OccupiedCells)
			&& (*AddReadback)->TryGetNumberField(TEXT("revision"), Revision)
			&& OccupiedCells == 2.0
			&& Revision == 1.0);

	const TSharedRef<FJsonObject> StaleRemove = MakeShared<FJsonObject>();
	StaleRemove->SetStringField(TEXT("action"), TEXT("remove_bounds"));
	StaleRemove->SetStringField(TEXT("session_id"), SessionId);
	StaleRemove->SetNumberField(TEXT("expected_revision"), 0);
	StaleRemove->SetObjectField(TEXT("bounds"), Bounds);
	const FUeAgentCommandResult StaleResult = SubmitPublic(
		Registry, Dispatch, GetSessionCommandId(), StaleRemove);
	TestFalse(TEXT("stale mutation is rejected"), StaleResult.IsOk());
	TestTrue(
		TEXT("stale mutation exposes the stable revision error"),
		StaleResult.GetErrorJson().Contains(TEXT("voxel_grid_revision_mismatch")));

	const TSharedRef<FJsonObject> Apply = MakeShared<FJsonObject>();
	Apply->SetStringField(TEXT("session_id"), SessionId);
	Apply->SetNumberField(TEXT("expected_revision"), 1);
	Apply->SetStringField(TEXT("mesh_build"), TEXT("exposed_faces"));
	Apply->SetBoolField(TEXT("keep_session"), true);
	const TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
	Collision->SetStringField(TEXT("mode"), TEXT("exact_box_components"));
	Collision->SetStringField(TEXT("profile_name"), TEXT("BlockAll"));
	Collision->SetStringField(TEXT("enabled"), TEXT("query_and_physics"));
	Collision->SetNumberField(TEXT("max_boxes"), 8);
	Apply->SetObjectField(TEXT("collision"), Collision);
	TSharedPtr<FJsonObject> ApplyPayload;
	if (!ParsePayload(
			*this,
			TEXT("public apply succeeds"),
			SubmitPublic(Registry, Dispatch, GetApplyCommandId(), Apply),
			ApplyPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Mesh = nullptr;
	const TSharedPtr<FJsonObject>* CollisionReadback = nullptr;
	const TSharedPtr<FJsonObject>* IndependentReadback = nullptr;
	const TSharedPtr<FJsonObject>* AuthoringState = nullptr;
	const TSharedPtr<FJsonObject>* Durability = nullptr;
	double ExposedFaces = -1.0;
	double CollisionBoxes = -1.0;
	bool bEquivalent = false;
	bool bManagedIdentityEquivalent = false;
	bool bPersistedReadbackVerified = false;
	FString ReceiptId;
	TestTrue(
		TEXT("apply verifies UE-owned state and durable persisted readback"),
		ApplyPayload->TryGetObjectField(TEXT("mesh"), Mesh)
			&& Mesh && Mesh->IsValid()
			&& (*Mesh)->TryGetNumberField(TEXT("exposed_face_count"), ExposedFaces)
			&& ExposedFaces == 10.0
			&& ApplyPayload->TryGetObjectField(TEXT("collision"), CollisionReadback)
			&& CollisionReadback && CollisionReadback->IsValid()
			&& (*CollisionReadback)->TryGetNumberField(
				TEXT("box_count"), CollisionBoxes)
			&& CollisionBoxes == 1.0
			&& ApplyPayload->TryGetObjectField(
				TEXT("readback"), IndependentReadback)
			&& IndependentReadback && IndependentReadback->IsValid()
			&& (*IndependentReadback)->TryGetBoolField(
				TEXT("equivalent"), bEquivalent)
			&& bEquivalent
			&& (*IndependentReadback)->TryGetBoolField(
				TEXT("managed_identity_equivalent"),
				bManagedIdentityEquivalent)
			&& bManagedIdentityEquivalent
			&& (*IndependentReadback)->TryGetBoolField(
				TEXT("persisted_readback_verified"),
				bPersistedReadbackVerified)
			&& bPersistedReadbackVerified
			&& ApplyPayload->TryGetObjectField(
				TEXT("authoring_state"), AuthoringState)
			&& AuthoringState && AuthoringState->IsValid()
			&& ApplyPayload->TryGetObjectField(TEXT("durability"), Durability)
			&& Durability && Durability->IsValid()
			&& (*Durability)->TryGetStringField(TEXT("receipt_id"), ReceiptId)
			&& !ReceiptId.IsEmpty()
			&& !Fixture.GetPackage()->IsDirty()
			&& !IFileManager::Get().FileExists(
				*Fixture.LegacySidecarPath(GridId)));

	const TSharedRef<FJsonObject> Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("action"), TEXT("read"));
	Read->SetStringField(TEXT("session_id"), SessionId);
	Read->SetNumberField(TEXT("expected_revision"), 1);
	const TSharedRef<FJsonObject> Projection = MakeShared<FJsonObject>();
	Projection->SetBoolField(TEXT("include_cells"), true);
	Projection->SetNumberField(TEXT("max_cells"), 8);
	Read->SetObjectField(TEXT("projection"), Projection);
	TSharedPtr<FJsonObject> ReadPayload;
	if (!ParsePayload(
			*this,
			TEXT("clean session read succeeds"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Read),
			ReadPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Readback = nullptr;
	bool bDirty = true;
	bool bCommitted = false;
	TestTrue(
		TEXT("read observes the committed clean session"),
		ReadPayload->TryGetObjectField(TEXT("readback"), Readback)
			&& Readback && Readback->IsValid()
			&& (*Readback)->TryGetBoolField(TEXT("dirty"), bDirty)
			&& (*Readback)->TryGetBoolField(TEXT("committed"), bCommitted)
			&& !bDirty
			&& bCommitted);

	const TSharedRef<FJsonObject> Close = MakeShared<FJsonObject>();
	Close->SetStringField(TEXT("action"), TEXT("close"));
	Close->SetStringField(TEXT("session_id"), SessionId);
	Close->SetNumberField(TEXT("expected_revision"), 1);
	TSharedPtr<FJsonObject> ClosePayload;
	if (!ParsePayload(
			*this,
			TEXT("clean session closes"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Close),
			ClosePayload))
	{
		return false;
	}

	ResetProcessSessionsForAutomation();
	Fixture.GetPackage()->SetDirtyFlag(true);
	const TSharedRef<FJsonObject> DirtyEdit = MakeShared<FJsonObject>();
	DirtyEdit->SetStringField(TEXT("action"), TEXT("begin"));
	DirtyEdit->SetStringField(TEXT("mode"), TEXT("edit"));
	DirtyEdit->SetStringField(TEXT("session_id"), TEXT("VGS_DIRTY_") + Suffix);
	DirtyEdit->SetStringField(TEXT("grid_id"), GridId);
	DirtyEdit->SetStringField(TEXT("actor_ref"), ActorRef);
	const FUeAgentCommandResult DirtyEditResult = SubmitPublic(
		Registry, Dispatch, GetSessionCommandId(), DirtyEdit);
	TestFalse(
		TEXT("pre-existing dirty target package is rejected"),
		DirtyEditResult.IsOk());
	TestTrue(
		TEXT("dirty target rejection exposes the stable C8D error"),
		DirtyEditResult.GetErrorJson().Contains(
			TEXT("voxel_grid_target_package_dirty")));
	Fixture.GetPackage()->SetDirtyFlag(false);

	const FString EditSessionId = TEXT("VGS_EDIT_") + Suffix;
	const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
	Edit->SetStringField(TEXT("action"), TEXT("begin"));
	Edit->SetStringField(TEXT("mode"), TEXT("edit"));
	Edit->SetStringField(TEXT("session_id"), EditSessionId);
	Edit->SetStringField(TEXT("grid_id"), GridId);
	Edit->SetStringField(TEXT("actor_ref"), ActorRef);
	Edit->SetObjectField(TEXT("projection"), Projection);
	TSharedPtr<FJsonObject> EditPayload;
	if (!ParsePayload(
			*this,
			TEXT("edit session reloads from the UE-owned authoring component"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Edit),
			EditPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* EditReadback = nullptr;
	Revision = -1.0;
	OccupiedCells = -1.0;
	TestTrue(
		TEXT("component reload restores revision and occupied cells"),
		EditPayload->TryGetObjectField(TEXT("readback"), EditReadback)
			&& EditReadback && EditReadback->IsValid()
			&& (*EditReadback)->TryGetNumberField(TEXT("revision"), Revision)
			&& (*EditReadback)->TryGetNumberField(
				TEXT("occupied_cell_count"), OccupiedCells)
			&& Revision == 1.0
			&& OccupiedCells == 2.0);

	const TSharedRef<FJsonObject> EditClose = MakeShared<FJsonObject>();
	EditClose->SetStringField(TEXT("action"), TEXT("close"));
	EditClose->SetStringField(TEXT("session_id"), EditSessionId);
	EditClose->SetNumberField(TEXT("expected_revision"), 1);
	TSharedPtr<FJsonObject> EditClosePayload;
	if (!ParsePayload(
			*this,
			TEXT("reloaded clean session closes"),
			SubmitPublic(
				Registry, Dispatch, GetSessionCommandId(), EditClose),
			EditClosePayload))
	{
		return false;
	}

	const FString CancelGridId = TEXT("VG_CANCEL_") + Suffix;
	const FString CancelSessionId = TEXT("VGS_CANCEL_") + Suffix;
	const TSharedRef<FJsonObject> CancelBegin = MakeShared<FJsonObject>();
	CancelBegin->SetStringField(TEXT("action"), TEXT("begin"));
	CancelBegin->SetStringField(TEXT("mode"), TEXT("create"));
	CancelBegin->SetStringField(TEXT("session_id"), CancelSessionId);
	CancelBegin->SetStringField(TEXT("grid_id"), CancelGridId);
	TSharedPtr<FJsonObject> CancelBeginPayload;
	if (!ParsePayload(
			*this,
			TEXT("second create session begins for cancellation"),
			SubmitPublic(
				Registry, Dispatch, GetSessionCommandId(), CancelBegin),
			CancelBeginPayload))
	{
		return false;
	}
	const TSharedRef<FJsonObject> Cancel = MakeShared<FJsonObject>();
	Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
	Cancel->SetStringField(TEXT("session_id"), CancelSessionId);
	Cancel->SetNumberField(TEXT("expected_revision"), 0);
	TSharedPtr<FJsonObject> CancelPayload;
	if (!ParsePayload(
			*this,
			TEXT("uncommitted create session cancels with durable rollback"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Cancel),
			CancelPayload))
	{
		return false;
	}
	bool bCancelledActorPresent = false;
	const FName CancelTag(*(TEXT("UAIEditableVoxelGrid:") + CancelGridId));
	for (TActorIterator<ADynamicMeshActor> It(Fixture.GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(CancelTag))
		{
			bCancelledActorPresent = true;
			break;
		}
	}
	TestFalse(
		TEXT("cancel removes the uncommitted actor"), bCancelledActorPresent);
	TestFalse(
		TEXT("cancel leaves the owned map package clean"),
		Fixture.GetPackage()->IsDirty());

	TestTrue(TEXT("owned persisted map fixture is fully cleaned"), Fixture.Cleanup());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingVoxelGridLegacyMigrationTest,
	"UeAgentInterface.ModelingGeometry.VoxelGrid.LegacySidecarMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingVoxelGridLegacyMigrationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16);
	const FString GridId = TEXT("VG_LEGACY_") + Suffix;
	const FString SessionId = TEXT("VGS_LEGACY_") + Suffix;
	FScopedVoxelGridFixture Fixture(TEXT("Legacy_") + Suffix);
	FScopedLegacySidecar Legacy(GridId);
	if (!TestTrue(TEXT("legacy migration owns a persisted map"), Fixture.IsValid()))
	{
		return false;
	}

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transactional;
	ADynamicMeshActor* const Actor = Fixture.GetWorld()->SpawnActor<ADynamicMeshActor>(
		ADynamicMeshActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		Spawn);
	if (!TestNotNull(TEXT("legacy fixture actor exists"), Actor))
	{
		return false;
	}
	Actor->Tags.AddUnique(FName(*(TEXT("UAIEditableVoxelGrid:") + GridId)));
	if (!TestTrue(TEXT("legacy fixture map persists"), Fixture.Save())
		|| !TestTrue(TEXT("legacy sidecar is written"), Legacy.Write()))
	{
		return false;
	}

	ResetProcessSessionsForAutomation();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("legacy migration public composition starts"),
			UE::UAI::Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TSharedRef<FJsonObject> Begin = MakeShared<FJsonObject>();
	Begin->SetStringField(TEXT("action"), TEXT("begin"));
	Begin->SetStringField(TEXT("mode"), TEXT("edit"));
	Begin->SetStringField(TEXT("session_id"), SessionId);
	Begin->SetStringField(TEXT("grid_id"), GridId);
	Begin->SetStringField(TEXT("actor_ref"), Actor->GetPathName());
	TSharedPtr<FJsonObject> BeginPayload;
	if (!ParsePayload(
			*this,
			TEXT("legacy edit imports the one-time sidecar"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Begin),
			BeginPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* BeginReadback = nullptr;
	const TSharedPtr<FJsonObject>* StateOwner = nullptr;
	bool bLegacyImported = false;
	double Revision = -1.0;
	TestTrue(
		TEXT("legacy import is explicit and preserves revision"),
		BeginPayload->TryGetObjectField(TEXT("readback"), BeginReadback)
			&& BeginReadback && BeginReadback->IsValid()
			&& (*BeginReadback)->TryGetNumberField(TEXT("revision"), Revision)
			&& Revision == 4.0
			&& (*BeginReadback)->TryGetObjectField(
				TEXT("authoring_state"), StateOwner)
			&& StateOwner && StateOwner->IsValid()
			&& (*StateOwner)->TryGetBoolField(
				TEXT("legacy_sidecar_imported"), bLegacyImported)
			&& bLegacyImported);

	const TSharedRef<FJsonObject> Apply = MakeShared<FJsonObject>();
	Apply->SetStringField(TEXT("session_id"), SessionId);
	Apply->SetNumberField(TEXT("expected_revision"), 4);
	const TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
	Collision->SetStringField(TEXT("mode"), TEXT("none"));
	Apply->SetObjectField(TEXT("collision"), Collision);
	TSharedPtr<FJsonObject> ApplyPayload;
	if (!ParsePayload(
			*this,
			TEXT("legacy import persists through the canonical UE owner"),
			SubmitPublic(Registry, Dispatch, GetApplyCommandId(), Apply),
			ApplyPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Durability = nullptr;
	FString ReceiptId;
	if (!TestTrue(
			TEXT("legacy migration returns the durable receipt"),
			ApplyPayload->TryGetObjectField(TEXT("durability"), Durability)
				&& Durability && Durability->IsValid()
				&& (*Durability)->TryGetStringField(
					TEXT("receipt_id"), ReceiptId)
				&& !ReceiptId.IsEmpty()))
	{
		return false;
	}
	Legacy.BindReceipt(ReceiptId);
	UUeAgentModelingVoxelGridStateComponent* Component = nullptr;
	FVoxelGridAuthoringState State;
	FString StateError;
	TestTrue(
		TEXT("legacy state now has one UE-owned serialized component"),
		TryFindAuthoringStateComponent(*Actor, Component, StateError)
			&& Component
			&& TryReadAuthoringState(*Component, 16, State, StateError)
			&& State.GridId == GridId
			&& State.Revision == 4
			&& State.Cells.Num() == 2);
	TestFalse(
		TEXT("legacy active sidecar exits after C9"),
		IFileManager::Get().FileExists(*Legacy.GetSourcePath()));
	TestTrue(
		TEXT("legacy source is retained only as a recoverable tombstone"),
		IFileManager::Get().FileExists(*Legacy.GetTombstonePath()));
	TestTrue(TEXT("legacy migration fixture is fully cleaned"), Fixture.Cleanup());
	return !HasAnyErrors();
}
