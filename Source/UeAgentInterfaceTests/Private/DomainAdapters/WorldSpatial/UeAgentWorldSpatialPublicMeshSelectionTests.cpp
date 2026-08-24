#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ModelingToolsEditorMode.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/GeometrySelector.h"
#include "Selections/GeometrySelection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "StaticMeshAttributes.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"

namespace UE::UAI::Tests::WorldSpatialPublicMeshSelection
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;

	TSharedRef<FJsonObject> TemporalPoint()
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("editor"));
		Result->SetStringField(TEXT("tick"), TEXT("1"));
		Result->SetStringField(TEXT("rate_numerator"), TEXT("60"));
		Result->SetStringField(TEXT("rate_denominator"), TEXT("1"));
		Result->SetStringField(TEXT("state"), TEXT("frozen"));
		return Result;
	}

	TSharedRef<FJsonObject> ScreenPoint(
		const FString& ViewportIdentity,
		const FIntPoint& Size)
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("viewport.screen.pixel"));
		Frame->SetStringField(TEXT("basis_frame_id"), ViewportIdentity);
		Frame->SetNumberField(TEXT("dimension"), 2);
		Frame->SetStringField(TEXT("unit"), TEXT("pixel"));
		Frame->SetStringField(
			TEXT("basis_evidence_id"),
			TEXT("t50-public-mesh-selection-screen"));
		Frame->SetObjectField(TEXT("observed_at"), TemporalPoint());

		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), ViewportIdentity);
		Result->SetStringField(TEXT("quantity"), TEXT("point"));
		Result->SetObjectField(TEXT("frame"), Frame);
		Result->SetArrayField(TEXT("components"), {
			MakeShared<FJsonValueString>(FString::FromInt(Size.X / 2)),
			MakeShared<FJsonValueString>(FString::FromInt(Size.Y / 2)),
		});
		Result->SetArrayField(TEXT("conversion_evidence_ids"), {});
		return Result;
	}

	TSharedRef<FJsonObject> SelectionInput(
		const FString& ViewportIdentity,
		const FIntPoint& Size)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("screen_point"),
			ScreenPoint(ViewportIdentity, Size));
		Result->SetStringField(TEXT("element_kind"), TEXT("triangle"));
		Result->SetStringField(TEXT("selection_mode"), TEXT("replace"));
		return Result;
	}

	TSharedRef<FJsonObject> PublicParams(
		const FString& ViewportIdentity,
		const TArray<FString>& SelectionIdentities,
		const TSharedRef<FJsonObject>& Input)
	{
		TArray<TSharedPtr<FJsonValue>> SelectionValues;
		for (const FString& Identity : SelectionIdentities)
		{
			SelectionValues.Add(MakeShared<FJsonValueString>(Identity));
		}
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		Context->SetStringField(TEXT("viewport_identity"), ViewportIdentity);
		Context->SetArrayField(
			TEXT("selection_identities"),
			MoveTemp(SelectionValues));
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), ViewportIdentity);
		Result->SetObjectField(TEXT("input"), Input);
		Result->SetObjectField(TEXT("context"), Context);
		return Result;
	}

	TSharedRef<FJsonObject> ModelingSessionParams(const FString& Action)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("action"), Action);
		Result->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());
		return Result;
	}

	bool IsVerifiedMeshSelectionOutcome(const FString& PayloadJson)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(PayloadJson);
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EvidenceIds = nullptr;
		FString OperationId;
		FString Schema;
		FString Type;
		FString Status;
		FString EffectOutcome;
		FString VerificationId;
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid()
			|| !Root->TryGetStringField(TEXT("operation_id"), OperationId)
			|| !Root->TryGetObjectField(TEXT("projection"), Projection)
			|| !Projection
			|| !Projection->IsValid()
			|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload
			|| !Payload->IsValid()
			|| !(*Payload)->TryGetStringField(TEXT("$schema"), Schema)
			|| !(*Payload)->TryGetStringField(TEXT("$type"), Type)
			|| !(*Payload)->TryGetStringField(TEXT("status"), Status)
			|| !(*Payload)->TryGetStringField(
				TEXT("effect_outcome"),
				EffectOutcome)
			|| !(*Payload)->TryGetStringField(
				TEXT("verification_id"),
				VerificationId)
			|| !(*Payload)->TryGetArrayField(
				TEXT("evidence_ids"),
				EvidenceIds)
			|| !EvidenceIds)
		{
			return false;
		}
		const bool bHasIndependentReadback = EvidenceIds->ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->Type == EJson::String
					&& Value->AsString()
						== TEXT("same-explicit-viewport-independent-selection-readback");
			});
		return OperationId.StartsWith(TEXT("world-spatial:"))
			&& Schema == TEXT("uai.c9.canonical_outcome_view.v1")
			&& Type == TEXT("verification_outcome")
			&& Status == TEXT("verified")
			&& EffectOutcome == TEXT("complete")
			&& VerificationId
				== TEXT("world-spatial-verify:mesh.element.select_by_ray")
			&& bHasIndependentReadback;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialPublicMeshElementSelectionTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E3.PublicProduct."
		"MeshElementSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialPublicMeshElementSelectionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::WorldSpatialPublicMeshSelection;
	(void)Parameters;

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

	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	FLevelEditorViewportClient* Client = nullptr;
	int32 ViewportIndex = INDEX_NONE;
	if (GEditor)
	{
		const TArray<FLevelEditorViewportClient*>& Clients =
			GEditor->GetLevelViewportClients();
		for (int32 Index = 0; Index < Clients.Num(); ++Index)
		{
			if (Clients[Index]
				&& Clients[Index]->Viewport
				&& Clients[Index]->GetWorld() == World)
			{
				Client = Clients[Index];
				ViewportIndex = Index;
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World)
		|| !TestNotNull(TEXT("the owned worker has a level viewport"), Client))
	{
		return false;
	}

	const FString ViewportIdentity = FString::Printf(
		TEXT("level:%d"),
		ViewportIndex);
	FSceneViewport* const SceneViewport =
		static_cast<FSceneViewport*>(Client->Viewport);
	const FIntPoint OriginalSize = Client->Viewport->GetSizeXY();
	const FVector OriginalViewLocation = Client->GetViewLocation();
	const FRotator OriginalViewRotation = Client->GetViewRotation();
	if (OriginalSize.X <= 0 || OriginalSize.Y <= 0)
	{
		SceneViewport->SetViewportSize(1280, 720);
	}
	Client->SetViewLocation(FVector::ZeroVector);
	Client->SetViewRotation(FRotator::ZeroRotator);
	Client->Invalidate();
	const FIntPoint ActiveSize = Client->Viewport->GetSizeXY();
	if (!TestTrue(
			TEXT("the owned level viewport has an RHI surface"),
			ActiveSize.X > 0 && ActiveSize.Y > 0))
	{
		return false;
	}

	TArray<TWeakObjectPtr<AActor>> OriginalSelection;
	if (GEditor && GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* const Selected = Cast<AActor>(*It))
			{
				OriginalSelection.Add(Selected);
			}
		}
	}
	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	const bool bModelingModeWasActive = ModeTools.IsModeActive(
		UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
	if (!TestFalse(
			TEXT("the isolated worker starts outside Modeling mode"),
			bModelingModeWasActive))
	{
		return false;
	}

	const FString MeshName = FString::Printf(
		TEXT("SM_T50PublicMeshSelection_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* const MeshPackage = CreatePackage(
		*(TEXT("/Game/UAI/Tests/WorldSpatial/") + MeshName));
	UStaticMesh* const Mesh = MeshPackage
		? NewObject<UStaticMesh>(
			MeshPackage,
			*MeshName,
			RF_Transient)
		: nullptr;
	if (Mesh)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		const FVertexID V0 = Description.CreateVertex();
		const FVertexID V1 = Description.CreateVertex();
		const FVertexID V2 = Description.CreateVertex();
		Positions[V0] = FVector3f(0.0f, -100.0f, -100.0f);
		Positions[V1] = FVector3f(0.0f, 100.0f, -100.0f);
		Positions[V2] = FVector3f(0.0f, 0.0f, 100.0f);
		const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
		const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
		const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Description.CreatePolygon(Group, {I0, I2, I1});
		Mesh->BuildFromMeshDescriptions({&Description});
	}
	FActorSpawnParameters Spawn;
	Spawn.Name = MakeUniqueObjectName(
		World,
		AStaticMeshActor::StaticClass(),
		FName(TEXT("T50PublicMeshSelection")));
	Spawn.ObjectFlags = RF_Transient | RF_TextExportTransient;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* const Target = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		FTransform(FVector(500.0, 0.0, 0.0)),
		Spawn);
	if (!TestNotNull(TEXT("the owned selectable mesh exists"), Mesh)
		|| !TestNotNull(TEXT("the owned mesh target exists"), Target))
	{
		if (Target)
		{
			Target->Destroy();
		}
		return false;
	}
	UStaticMeshComponent* const Component = Target->GetStaticMeshComponent();
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	Component->UpdateBounds();

	TWeakObjectPtr<UModelingToolsEditorMode> ModelingMode;
	ON_SCOPE_EXIT
	{
		if (ModelingMode.IsValid())
		{
			if (UGeometrySelectionManager* const Manager =
				ModelingMode->GetSelectionManager())
			{
				if (Manager->HasSelection())
				{
					Manager->ClearSelection();
				}
				Manager->SynchronizeActiveTargets(
					{},
					[]() {});
			}
		}
		if (!bModelingModeWasActive
			&& ModeTools.IsModeActive(
				UModelingToolsEditorMode::EM_ModelingToolsEditorModeId))
		{
			ModeTools.DeactivateMode(
				UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
		}
		if (GEditor)
		{
			GEditor->SelectNone(false, true, false);
			for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
			{
				if (Actor.IsValid())
				{
					GEditor->SelectActor(Actor.Get(), true, false, true);
				}
			}
		}
		if (Target && IsValid(Target))
		{
			Target->Destroy();
		}
		if (Mesh)
		{
			Mesh->MarkAsGarbage();
		}
		if (MeshPackage)
		{
			MeshPackage->MarkAsGarbage();
		}
		Client->SetViewLocation(OriginalViewLocation);
		Client->SetViewRotation(OriginalViewRotation);
		if (OriginalSize.X <= 0 || OriginalSize.Y <= 0)
		{
			SceneViewport->SetViewportSize(OriginalSize.X, OriginalSize.Y);
		}
	};

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Target, true, false, true);
	const FUeAgentCommandResult Activated =
		UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
			Registry,
			Dispatch,
			TEXT("mesh.modeling_session"),
			ModelingSessionParams(TEXT("activate")),
			TEXT("t50-world-spatial-public-selection"));
	if (!TestTrue(TEXT("the public modeling session activates"), Activated.IsOk()))
	{
		AddError(Activated.GetErrorJson());
		return false;
	}
	ModelingMode = Cast<UModelingToolsEditorMode>(
		ModeTools.GetActiveScriptableMode(
			UModelingToolsEditorMode::EM_ModelingToolsEditorModeId));
	UGeometrySelectionManager* const Manager = ModelingMode.IsValid()
		? ModelingMode->GetSelectionManager()
		: nullptr;
	if (!TestNotNull(TEXT("the live Modeling mode is active"), ModelingMode.Get())
		|| !TestNotNull(TEXT("the live selection manager exists"), Manager))
	{
		return false;
	}
	Manager->SynchronizeActiveTargets(
		{FGeometryIdentifier::PrimitiveComponent(
			Component,
			FGeometryIdentifier::EObjectType::StaticMeshComponent)},
		[]() {});
	Manager->SetCurrentTargetsLockState(false);
	Manager->SetMeshSelectionTypeAndMode(
		UE::Geometry::EGeometryElementType::Face,
		UGeometrySelectionManager::EMeshTopologyMode::Triangle,
		false);
	if (!TestTrue(
			TEXT("the public fixture has one unlocked live mesh target"),
			Manager->HasActiveTargets()
				&& !Manager->GetAnyCurrentTargetsLocked()))
	{
		return false;
	}
	UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialNativeDependencies DirectDependencies;
	UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialExecutionError DirectError;
	if (!TestTrue(
			TEXT("production dependencies compose against the owned world"),
			UE::UAI::DomainAdapters::WorldSpatial::
				TryCreateWorldSpatialProductionDependencies(
					*World,
					TEXT("t50-public-mesh-selection-generation"),
					TEXT("t50-public-mesh-selection-session"),
					DirectDependencies,
					DirectError)))
	{
		AddError(DirectError.Code + TEXT(":") + DirectError.Detail);
		return false;
	}
	UGeometrySelectionManager* DirectManager = nullptr;
	TArray<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialMeshSelectionTarget> DirectTargets;
	if (!TestTrue(
			TEXT("production dependencies resolve the live exact mesh target"),
			DirectDependencies.Environment->TryResolveMeshSelectionSession(
				ViewportIdentity,
				{Component->GetPathName()},
				DirectManager,
				DirectTargets,
				DirectError)))
	{
		AddError(DirectError.Code + TEXT(":") + DirectError.Detail);
		return false;
	}
	TestEqual(
		TEXT("production dependencies reuse the live selection manager"),
		DirectManager,
		Manager);
	TestTrue(
		TEXT("production dependencies preserve the exact component identity"),
		DirectTargets.Num() == 1
			&& DirectTargets[0].Component == Component);

	const TSharedRef<FJsonObject> Input = SelectionInput(
		ViewportIdentity,
		ActiveSize);
	const FUeAgentCommandResult Selected =
		UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
			Registry,
			Dispatch,
			TEXT("mesh.element.select_by_ray"),
			PublicParams(
				ViewportIdentity,
				{Component->GetPathName()},
				Input),
			TEXT("t50-world-spatial-public-selection"));
	if (!TestTrue(TEXT("the public mesh-element selection succeeds"), Selected.IsOk()))
	{
		AddError(Selected.GetErrorJson());
		return false;
	}
	UE::Geometry::FGeometrySelection Readback;
	TestTrue(
		TEXT("the live selection manager independently reads the selected triangle"),
		Manager->GetSelectionForComponent(Component, Readback)
			&& !Readback.Selection.IsEmpty());
	if (!TestTrue(
			TEXT("the public result exposes its verified independent-readback outcome"),
			IsVerifiedMeshSelectionOutcome(Selected.GetPayloadJson())))
	{
		AddError(Selected.GetPayloadJson());
	}

	const FUeAgentCommandResult Invalid =
		UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
			Registry,
			Dispatch,
			TEXT("mesh.element.select_by_ray"),
			PublicParams(
				ViewportIdentity,
				{Target->GetPathName()},
				Input),
			TEXT("t50-world-spatial-public-selection"));
	TestFalse(TEXT("an Actor identity cannot masquerade as a mesh target"), Invalid.IsOk());
	UE::Geometry::FGeometrySelection SelectionAfterInvalidTarget;
	TestTrue(
		TEXT("the invalid target fails before changing mesh selection"),
		Manager->GetSelectionForComponent(
			Component,
			SelectionAfterInvalidTarget)
			&& SelectionAfterInvalidTarget.Selection.Num()
				== Readback.Selection.Num()
			&& SelectionAfterInvalidTarget.Selection.Includes(
				Readback.Selection));
	return !HasAnyErrors();
}

#endif
