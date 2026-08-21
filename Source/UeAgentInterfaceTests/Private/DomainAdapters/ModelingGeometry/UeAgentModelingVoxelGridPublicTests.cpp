#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

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
		explicit FScopedVoxelGridFixture(FString InGridId)
			: GridId(MoveTemp(InGridId))
			, World(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			, WorldPackage(World ? World->GetOutermost() : nullptr)
			, bWorldPackageWasDirty(WorldPackage && WorldPackage->IsDirty())
		{
		}

		~FScopedVoxelGridFixture()
		{
			Cleanup();
		}

		UWorld* GetWorld() const { return World; }

		bool Cleanup()
		{
			ResetProcessSessionsForAutomation();
			bool bActorRemoved = true;
			if (World)
			{
				const FName GridTag(*(TEXT("UAIEditableVoxelGrid:") + GridId));
				TArray<ADynamicMeshActor*> Actors;
				for (TActorIterator<ADynamicMeshActor> It(World); It; ++It)
				{
					if (It->Tags.Contains(GridTag)) Actors.Add(*It);
				}
				for (ADynamicMeshActor* Actor : Actors)
				{
					bActorRemoved = World->DestroyActor(Actor) && bActorRemoved;
				}
			}
			const FString Sidecar = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UeAgentInterface"),
				TEXT("VoxelGrids"),
				GridId + TEXT(".json"));
			bool bSidecarRemoved = true;
			if (IFileManager::Get().FileExists(*Sidecar))
			{
				bSidecarRemoved = IFileManager::Get().Delete(*Sidecar, false, true);
			}
			if (WorldPackage)
			{
				WorldPackage->SetDirtyFlag(bWorldPackageWasDirty);
			}
			return bActorRemoved
				&& bSidecarRemoved
				&& !IFileManager::Get().FileExists(*Sidecar);
		}

	private:
		FString GridId;
		UWorld* World = nullptr;
		UPackage* WorldPackage = nullptr;
		bool bWorldPackageWasDirty = false;
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
	FScopedVoxelGridFixture Fixture(GridId);
	if (!TestNotNull(TEXT("editor world exists"), Fixture.GetWorld()))
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
	double ExposedFaces = -1.0;
	double CollisionBoxes = -1.0;
	bool bEquivalent = false;
	FString SidecarPath;
	TestTrue(
		TEXT("apply verifies mesh, exact-cover collision, and sidecar"),
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
			&& ApplyPayload->TryGetStringField(TEXT("sidecar_path"), SidecarPath)
			&& IFileManager::Get().FileExists(*SidecarPath));

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
			TEXT("edit session reloads from the committed sidecar"),
			SubmitPublic(Registry, Dispatch, GetSessionCommandId(), Edit),
			EditPayload))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* EditReadback = nullptr;
	Revision = -1.0;
	OccupiedCells = -1.0;
	TestTrue(
		TEXT("sidecar reload restores revision and occupied cells"),
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

	TestTrue(TEXT("owned actor and sidecar are fully cleaned"), Fixture.Cleanup());
	return !HasAnyErrors();
}
