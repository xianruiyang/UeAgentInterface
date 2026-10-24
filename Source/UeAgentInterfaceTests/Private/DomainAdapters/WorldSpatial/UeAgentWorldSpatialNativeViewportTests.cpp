#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeTestSupport.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "InteractiveToolsContext.h"
#include "MeshDescription.h"
#include "PreviewScene.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/GeometrySelector.h"
#include "Selection/StaticMeshSelector.h"
#include "Selections/GeometrySelection.h"
#include "Slate/SceneViewport.h"
#include "StaticMeshAttributes.h"
#include "ToolContextInterfaces.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::Tests::WorldSpatial;
	using namespace UE::UAI::StateSemantic;

	const FUeAgentStateEntry* FindEntry(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role)
	{
		return Snapshot.GetEntries().FindByPredicate(
			[&Role](const FUeAgentStateEntry& Entry)
			{
				return Entry.GetIdentity().GetObjectRole() == Role;
			});
	}

	const FUeAgentCanonicalValueRef* FindField(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Name)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		const TPair<FString, FUeAgentCanonicalValueRef>* Field =
			Object->GetObjectItems().FindByPredicate(
			[&Name](
				const TPair<FString, FUeAgentCanonicalValueRef>& Item)
			{
				return Item.Key == Name;
			});
		return Field ? &Field->Value : nullptr;
	}

	bool HasObjectFields(
		const FUeAgentCanonicalValueRef& Object,
		std::initializer_list<const TCHAR*> Names)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (!FindField(Object, Name))
			{
				return false;
			}
		}
		return true;
	}

	bool IsCameraObject(const FUeAgentCanonicalValueRef& Value)
	{
		return HasObjectFields(
			Value,
			{TEXT("camera_transform"), TEXT("fov_degrees"),
				TEXT("view_mode"), TEXT("observed_at")});
	}

	void TestPixelScaleSnapshot(
		FAutomationTestBase& Test,
		const FUeAgentCanonicalSnapshot& Snapshot)
	{
		const FUeAgentStateEntry* Camera = FindEntry(
			Snapshot, TEXT("viewport_pixel_scale.camera"));
		const FUeAgentStateEntry* Evidence = FindEntry(
			Snapshot, TEXT("viewport_pixel_scale.evidence_ids"));
		Test.TestTrue(
			TEXT("pixel-scale C4 source owns the complete camera state"),
			Camera && IsCameraObject(Camera->GetValue()));
		Test.TestTrue(
			TEXT("pixel-scale C4 source owns non-empty projection evidence"),
			Evidence
				&& Evidence->GetValue()->GetKind() == EUeAgentValueKind::Array
				&& !Evidence->GetValue()->GetArrayItems().IsEmpty());
	}

	void TestViewportCatalogSnapshot(
		FAutomationTestBase& Test,
		const FString& CapabilityId,
		const FUeAgentCanonicalSnapshot& Snapshot)
	{
		if (CapabilityId == TEXT("viewport.warning.list"))
		{
			const FUeAgentStateEntry* Warnings = FindEntry(
				Snapshot, TEXT("viewport_warnings"));
			Test.TestTrue(
				TEXT("warning source preserves the typed warning-object array"),
				Warnings
					&& Warnings->GetType().GetTypeId()
						== TEXT("world_spatial.viewport_warning_array.v1")
					&& Warnings->GetValue()->GetKind()
						== EUeAgentValueKind::Array);
		}
		else if (CapabilityId == TEXT("viewport.view_mode.list"))
		{
			const FUeAgentStateEntry* Modes = FindEntry(
				Snapshot, TEXT("viewport_modes.catalog"));
			const FUeAgentCanonicalValueRef* FirstMode = Modes
				&& Modes->GetValue()->GetKind() == EUeAgentValueKind::Array
				&& !Modes->GetValue()->GetArrayItems().IsEmpty()
					? &Modes->GetValue()->GetArrayItems()[0]
					: nullptr;
			const FUeAgentCanonicalValueRef* FirstId = FirstMode
				? FindField(*FirstMode, TEXT("mode_id")) : nullptr;
			const FUeAgentCanonicalValueRef* FirstLabel = FirstMode
				? FindField(*FirstMode, TEXT("label")) : nullptr;
			Test.TestTrue(
				TEXT("view-mode source preserves mode ids and labels"),
				FirstMode
					&& HasObjectFields(
						*FirstMode, {TEXT("mode_id"), TEXT("label")}));
			Test.TestTrue(
				TEXT("view-mode query uses the shared keyword matcher"),
				FirstId && FirstLabel
					&& FirstId->Get().GetText().Contains(
						TEXT("shader"), ESearchCase::IgnoreCase)
					&& FirstLabel->Get().GetText().Contains(
						TEXT("complexity"), ESearchCase::IgnoreCase));
		}
		else if (CapabilityId == TEXT("viewport.instance.list"))
		{
			const FUeAgentStateEntry* Instances = FindEntry(
				Snapshot, TEXT("viewport_instances.catalog"));
			const bool bHasInstance = Instances
				&& Instances->GetValue()->GetKind() == EUeAgentValueKind::Array
				&& !Instances->GetValue()->GetArrayItems().IsEmpty();
			const FUeAgentCanonicalValueRef* Camera = bHasInstance
				? FindField(
					Instances->GetValue()->GetArrayItems()[0],
					TEXT("camera"))
				: nullptr;
			Test.TestTrue(
				TEXT("viewport-instance source preserves identity, type and camera"),
				bHasInstance
					&& HasObjectFields(
						Instances->GetValue()->GetArrayItems()[0],
						{TEXT("viewport_ref"), TEXT("viewport_type"),
							TEXT("camera")})
					&& Camera && IsCameraObject(*Camera));
			if (Instances)
			{
				Test.TestEqual(
					TEXT("viewport-instance source exposes the stable public selector"),
					Instances->GetIdentity().GetSelector(TEXT("selector")),
					FString(TEXT("explicit_viewports")));
			}
		}
	}

	class FOwnedSelectionTransactions final
		: public IToolsContextTransactionsAPI
	{
	public:
		void DisplayMessage(const FText&, EToolMessageLevel) override {}
		void PostInvalidation() override {}
		void BeginUndoTransaction(const FText&) override {}
		void EndUndoTransaction() override {}
		void AppendChange(
			UObject*,
			TUniquePtr<FToolCommandChange>,
			const FText&) override
		{
		}
		bool RequestSelectionChange(
			const FSelectedObjectsChangeList&) override
		{
			return false;
		}
	};

	class FOwnedSelectionQueries final : public IToolsContextQueriesAPI
	{
	public:
		FOwnedSelectionQueries(
			UWorld* InWorld,
			FEditorViewportClient* InClient,
			FViewport* InViewport)
			: World(InWorld)
			, Client(InClient)
			, Viewport(InViewport)
		{
		}

		void SetToolsContext(UInteractiveToolsContext* InToolsContext)
		{
			ToolsContext = InToolsContext;
		}

		UWorld* GetCurrentEditingWorld() const override
		{
			return World;
		}

		void GetCurrentSelectionState(FToolBuilderState& StateOut) const override
		{
			StateOut = FToolBuilderState();
			StateOut.World = World;
			if (ToolsContext)
			{
				StateOut.ToolManager = ToolsContext->ToolManager;
				StateOut.TargetManager = ToolsContext->TargetManager;
				StateOut.GizmoManager = ToolsContext->GizmoManager;
			}
		}

		void GetCurrentViewState(FViewCameraState& StateOut) const override
		{
			StateOut = FViewCameraState();
			if (!Client || !Viewport)
			{
				return;
			}
			const FIntPoint Size = Viewport->GetSizeXY();
			StateOut.Position = Client->GetViewLocation();
			StateOut.Orientation = Client->GetViewRotation().Quaternion();
			StateOut.HorizontalFOVDegrees = Client->ViewFOV;
			StateOut.OrthoWorldCoordinateWidth = 1.0f;
			StateOut.AspectRatio = Size.Y > 0
				? static_cast<float>(Size.X) / static_cast<float>(Size.Y)
				: 1.0f;
			StateOut.bIsOrthographic = !Client->IsPerspective();
			StateOut.bIsVR = false;
		}

		UMaterialInterface* GetStandardMaterial(
			EStandardToolContextMaterials) const override
		{
			return nullptr;
		}

		FViewport* GetHoveredViewport() const override { return Viewport; }
		FViewport* GetFocusedViewport() const override { return Viewport; }

	private:
		UWorld* World = nullptr;
		FEditorViewportClient* Client = nullptr;
		FViewport* Viewport = nullptr;
		UInteractiveToolsContext* ToolsContext = nullptr;
	};

	class FOwnedSceneViewport final : public FSceneViewport
	{
	public:
		using FSceneViewport::FSceneViewport;

		void RemoveRenderableSurfaceForTest()
		{
			UpdateViewportRHI(
				true,
				0,
				0,
				EWindowMode::Windowed,
				PF_Unknown);
		}
	};

	class FOwnedViewportFixture final : public FOwnedNativeEnvironmentBase
	{
	public:
		FOwnedViewportFixture()
		{
			PreviewScene = MakeUnique<FPreviewScene>(
				FPreviewScene::ConstructionValues());
			World = PreviewScene ? PreviewScene->GetWorld() : nullptr;
			ViewportWidget = SNew(SViewport);
			ViewportWindow = SNew(SWindow)
				.ClientSize(FVector2D(1280.0, 720.0))
				.SizingRule(ESizingRule::FixedSize)
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				[
					ViewportWidget.ToSharedRef()
				];
			if (ViewportWindow.IsValid())
			{
				FSlateApplication::Get().AddWindow(
					ViewportWindow.ToSharedRef(),
					false);
			}
			Client = MakeUnique<FEditorViewportClient>(
				nullptr,
				PreviewScene.Get());
			if (!World || !ViewportWidget.IsValid()
				|| !ViewportWindow.IsValid() || !Client)
			{
				return;
			}
			SceneViewport = MakeShared<FOwnedSceneViewport>(
				Client.Get(),
				ViewportWidget);
			ViewportWidget->SetViewportInterface(SceneViewport.ToSharedRef());
			SceneViewport->SetViewportSize(1280, 720);
			Client->Viewport = SceneViewport.Get();
			Client->SetViewportType(LVT_Perspective);
			Client->SetViewLocation(FVector::ZeroVector);
			Client->SetViewRotation(FRotator::ZeroRotator);
			Client->ViewFOV = 90.0f;
			Client->SetViewMode(VMI_Lit);
			Client->SetRealtime(true);

			Target = World->SpawnActor<AActor>(
				FVector(100.0, 0.0, 0.0),
				FRotator::ZeroRotator);
			if (!Target)
			{
				return;
			}
			Target->Rename(TEXT("WorldSpatialViewportTarget"));
			IgnoredTarget = World->SpawnActor<AActor>(
				FVector(100.0, 500.0, 0.0),
				FRotator::ZeroRotator);
			if (!IgnoredTarget)
			{
				return;
			}
			IgnoredTarget->Rename(TEXT("WorldSpatialViewportIgnoredTarget"));
			Collision = NewObject<UBoxComponent>(
				Target,
				TEXT("WorldSpatialViewportCollision"));
			Collision->InitBoxExtent(FVector(10.0, 40.0, 40.0));
			Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Collision->SetCollisionObjectType(ECC_WorldStatic);
			Collision->SetCollisionResponseToAllChannels(ECR_Block);
			Target->SetRootComponent(Collision);
			Target->AddInstanceComponent(Collision);
			Collision->RegisterComponentWithWorld(World);
			Collision->SetWorldLocation(FVector(100.0, 0.0, 0.0));

			const FString MeshName = FString::Printf(
				TEXT("SM_WorldSpatialViewport_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			MeshPackage = CreatePackage(
				*(TEXT("/Game/UAI/Tests/WorldSpatial/") + MeshName));
			Mesh = MeshPackage
				? NewObject<UStaticMesh>(
					MeshPackage,
					*MeshName,
					RF_Transient)
				: nullptr;
			if (!Mesh)
			{
				return;
			}
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions =
				Attributes.GetVertexPositions();
			const FVertexID V0 = Description.CreateVertex();
			const FVertexID V1 = Description.CreateVertex();
			const FVertexID V2 = Description.CreateVertex();
			Positions[V0] = FVector3f(0.0f, -20.0f, -20.0f);
			Positions[V1] = FVector3f(0.0f, 20.0f, -20.0f);
			Positions[V2] = FVector3f(0.0f, 0.0f, 20.0f);
			const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
			const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
			const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			// Face the owned center ray, which travels from -X toward +X.
			Description.CreatePolygon(Group, {I0, I2, I1});
			Mesh->BuildFromMeshDescriptions({&Description});
			MeshComponent = NewObject<UStaticMeshComponent>(
				Target,
				TEXT("WorldSpatialViewportMesh"));
			MeshComponent->SetStaticMesh(Mesh);
			Target->AddInstanceComponent(MeshComponent);
			MeshComponent->RegisterComponentWithWorld(World);
			MeshComponent->SetWorldLocation(FVector(100.0, 0.0, 0.0));

			Objects.Add(TEXT("actor:target"), Target);
			Objects.Add(TEXT("actor:ignored"), IgnoredTarget);
			Objects.Add(TEXT("mesh:fixture"), Mesh);
			ReverseObjects.Add(Target, TEXT("actor:target"));
			ReverseObjects.Add(IgnoredTarget, TEXT("actor:ignored"));
			ReverseObjects.Add(Collision, TEXT("component:target_collision"));
			ReverseObjects.Add(Mesh, TEXT("mesh:fixture"));
			ReverseObjects.Add(MeshComponent, TEXT("mesh:fixture"));

			SelectionTransactions = MakeUnique<FOwnedSelectionTransactions>();
			SelectionQueries = MakeUnique<FOwnedSelectionQueries>(
				World,
				Client.Get(),
				SceneViewport.Get());
			SelectionToolsContext = NewObject<UInteractiveToolsContext>(
				GetTransientPackage());
			if (!SelectionToolsContext)
			{
				return;
			}
			SelectionToolsContext->AddToRoot();
			SelectionToolsContext->Initialize(
				SelectionQueries.Get(),
				SelectionTransactions.Get());
			SelectionQueries->SetToolsContext(SelectionToolsContext);
			SelectionManager = NewObject<UGeometrySelectionManager>(
				GetTransientPackage());
			if (!SelectionManager)
			{
				return;
			}
			SelectionManager->AddToRoot();
			SelectionManager->Initialize(
				SelectionToolsContext,
				SelectionTransactions.Get());
			SelectionManager->RegisterSelectorFactory(
				MakeUnique<FStaticMeshComponentSelectorFactory>());
			SelectionManager->SetMeshSelectionTypeAndMode(
				UE::Geometry::EGeometryElementType::Face,
				UGeometrySelectionManager::EMeshTopologyMode::Triangle,
				false);
			const TArray<FGeometryIdentifier> DesiredTargets{
				FGeometryIdentifier::PrimitiveComponent(
					MeshComponent,
					FGeometryIdentifier::EObjectType::
						StaticMeshComponent),
			};
			SelectionManager->SynchronizeActiveTargets(
				DesiredTargets,
				[this]()
				{
					if (SelectionManager && SelectionManager->HasSelection())
					{
						SelectionManager->ClearSelection();
					}
				});
			SelectionManager->SetCurrentTargetsLockState(false);
			if (GEditor && GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
				{
					if (AActor* Selected = Cast<AActor>(*It))
					{
						OriginalSelection.Add(Selected);
					}
				}
			}
		}

		~FOwnedViewportFixture()
		{
			if (SelectionManager)
			{
				if (SelectionManager->HasSelection())
				{
					SelectionManager->ClearSelection();
				}
				SelectionManager->SynchronizeActiveTargets(
					{},
					[this]()
					{
						if (SelectionManager && SelectionManager->HasSelection())
						{
							SelectionManager->ClearSelection();
						}
					});
				SelectionManager->Shutdown();
				SelectionManager->RemoveFromRoot();
				SelectionManager->MarkAsGarbage();
				SelectionManager = nullptr;
			}
			if (SelectionToolsContext && !bSelectionToolsContextShutdown)
			{
				SelectionToolsContext->Shutdown();
				bSelectionToolsContextShutdown = true;
			}
			if (SelectionToolsContext)
			{
				SelectionToolsContext->RemoveFromRoot();
				SelectionToolsContext->MarkAsGarbage();
				SelectionToolsContext = nullptr;
			}
			SelectionQueries.Reset();
			SelectionTransactions.Reset();
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
				for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
				{
					if (Actor.IsValid())
					{
						GEditor->SelectActor(
							Actor.Get(),
							true,
							false,
							true);
					}
				}
			}
			if (Client)
			{
				Client->Viewport = nullptr;
			}
			SceneViewport.Reset();
			if (ViewportWindow.IsValid())
			{
				ViewportWindow->SetContent(SNullWidget::NullWidget);
				FSlateApplication::Get().RequestDestroyWindow(
					ViewportWindow.ToSharedRef());
			}
			ViewportWidget.Reset();
			ViewportWindow.Reset();
			Client.Reset();
			PreviewScene.Reset();
			if (Mesh)
			{
				Mesh->MarkAsGarbage();
			}
			if (MeshPackage)
			{
				MeshPackage->MarkAsGarbage();
			}
		}

		bool IsViewportValid() const
		{
			return PreviewScene && World && Client && SceneViewport.IsValid()
				&& ViewportWidget.IsValid() && ViewportWindow.IsValid()
				&& Client->Viewport == SceneViewport.Get()
				&& SceneViewport->GetSizeXY() == FIntPoint(1280, 720)
				&& World->Scene && Target && IgnoredTarget && Collision
				&& Mesh && MeshComponent;
		}

		bool IsMeshSelectionValid() const
		{
			return SelectionManager && SelectionManager->HasActiveTargets()
				&& !SelectionManager->GetAnyCurrentTargetsLocked();
		}

		void ShutdownSelectionToolsContextForTest()
		{
			if (SelectionToolsContext && !bSelectionToolsContextShutdown)
			{
				SelectionToolsContext->Shutdown();
				bSelectionToolsContextShutdown = true;
			}
		}

		bool TryResolveObject(
			const FString& ExplicitRef,
			UObject*& OutObject,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutObject = Objects.FindRef(ExplicitRef);
			if (!OutObject)
			{
				OutError.Code = TEXT("owned_viewport_object_unresolved");
				OutError.SubjectId = ExplicitRef;
				return false;
			}
			return true;
		}

		bool TryGetExplicitObjectRef(
			const UObject* Object,
			FString& OutExplicitRef,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			const FString* Found = ReverseObjects.Find(Object);
			if (!Found)
			{
				OutExplicitRef.Reset();
				OutError.Code = TEXT("owned_viewport_reverse_object_unresolved");
				OutError.SubjectId = Object ? Object->GetPathName() : TEXT("null");
				return false;
			}
			OutExplicitRef = *Found;
			return true;
		}

		bool TryResolveMeshSelectionSession(
			const FString& ViewportIdentity,
			const TArray<FString>& SelectionIdentities,
			UGeometrySelectionManager*& OutManager,
			TArray<FUeAgentWorldSpatialMeshSelectionTarget>& OutTargets,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			if (ViewportIdentity != Identity
				|| SelectionIdentities != TArray<FString>{SelectionIdentity}
				|| !SelectionManager || !MeshComponent)
			{
				OutManager = nullptr;
				OutTargets.Reset();
				OutError.Code = TEXT("owned_mesh_selection_identity_mismatch");
				OutError.SubjectId = ViewportIdentity;
				return false;
			}
			OutManager = SelectionManager;
			OutTargets = {{TEXT("mesh:fixture"), MeshComponent}};
			return true;
		}

		bool TryReadRevision(
			const FString& ExplicitRef,
			FWorldSpatialRevision& OutRevision,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			if (ExplicitRef != Identity)
			{
				OutRevision = {};
				OutError.Code = TEXT("owned_viewport_revision_unresolved");
				OutError.SubjectId = ExplicitRef;
				return false;
			}
			OutRevision = {Identity, TEXT("gen-1"), 7};
			return true;
		}

		bool TryResolveViewport(
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			if (ViewportIdentity != Identity || !Client || !SceneViewport.IsValid())
			{
				OutClient = nullptr;
				OutViewport = nullptr;
				OutError.Code = TEXT("owned_viewport_identity_mismatch");
				OutError.SubjectId = ViewportIdentity;
				return false;
			}
			OutClient = Client.Get();
			OutViewport = SceneViewport.Get();
			return true;
		}

		void GetExplicitViewportIdentities(
			TArray<FString>& OutViewportIdentities) const override
		{
			OutViewportIdentities = {Identity};
		}

		static inline const FString Identity = TEXT("viewport:fixture");
		static inline const FString SelectionIdentity = TEXT("selection:fixture");

		TUniquePtr<FPreviewScene> PreviewScene;
		UWorld* World = nullptr;
		TUniquePtr<FEditorViewportClient> Client;
		TSharedPtr<SViewport> ViewportWidget;
		TSharedPtr<SWindow> ViewportWindow;
		TSharedPtr<FOwnedSceneViewport> SceneViewport;
		AActor* Target = nullptr;
		AActor* IgnoredTarget = nullptr;
		UBoxComponent* Collision = nullptr;
		UStaticMesh* Mesh = nullptr;
		UPackage* MeshPackage = nullptr;
		UStaticMeshComponent* MeshComponent = nullptr;
		TMap<FString, UObject*> Objects;
		TMap<const UObject*, FString> ReverseObjects;
		TArray<TWeakObjectPtr<AActor>> OriginalSelection;
		UGeometrySelectionManager* SelectionManager = nullptr;
		UInteractiveToolsContext* SelectionToolsContext = nullptr;
		bool bSelectionToolsContextShutdown = false;
		TUniquePtr<FOwnedSelectionQueries> SelectionQueries;
		TUniquePtr<FOwnedSelectionTransactions> SelectionTransactions;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialViewportFrameSelectorContractTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.Native.Viewport."
		"FrameSelectorContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialViewportFrameSelectorContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	FWorldSpatialValue Point;
	Point.Subject = TEXT("frame-target");
	Point.Quantity = TEXT("point");
	Point.Frame.FrameId = TEXT("unreal.world");
	Point.Frame.BasisFrameId = TEXT("unreal.world");
	Point.Frame.Dimension = 3;
	Point.Frame.Unit = TEXT("centimeter");
	Point.Frame.BasisEvidenceId = TEXT("native-test");
	Point.Frame.ObservedAt = {
		TEXT("native-test"), TEXT("0"), TEXT("1"), TEXT("1"), TEXT("captured")};
	Point.Components = {TEXT("0"), TEXT("0"), TEXT("0")};
	Point.ConversionEvidenceIds = {TEXT("identity")};

	FViewportCameraFrameRequestInput Input;
	Input.WorldPoint = Point;
	TestTrue(TEXT("world point is one selector"), Input.HasExactlyOneSelector());
	Input.SubjectRefs.Add(TEXT("actor:/Game/Test.Target"));
	TestFalse(TEXT("world point plus subject is ambiguous"),
		Input.HasExactlyOneSelector());
	Input.SubjectRefs.Reset();
	Input.WorldPoint.Reset();
	Input.WorldBoundsMin = Point;
	Input.WorldBoundsMax = Point;
	TestTrue(TEXT("complete world bounds is one selector"),
		Input.HasExactlyOneSelector());
	Input.WorldBoundsMax.Reset();
	TestFalse(TEXT("partial world bounds is rejected"),
		Input.HasExactlyOneSelector());

	FWorldSpatialValue Transform = Point;
	Transform.Quantity = TEXT("transform");
	Transform.Components = {
		TEXT("0"), TEXT("0"), TEXT("0"),
		TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1"),
		TEXT("1"), TEXT("1"), TEXT("1")};
	FWorldSpatialViewportStatePatch Patch;
	Patch.CameraTransform = Transform;
	TestFalse(TEXT("camera transform requires an explicit mode"), Patch.IsValid());
	Patch.CameraTransformMode = TEXT("absolute");
	TestTrue(TEXT("absolute is a valid explicit camera transform mode"),
		Patch.IsValid());
	Patch.CameraTransformMode = TEXT("delta");
	TestTrue(TEXT("delta is a valid explicit camera transform mode"),
		Patch.IsValid());
	Patch.CameraTransformMode = TEXT("relative");
	TestFalse(TEXT("unknown camera transform mode is rejected"),
		Patch.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionViewportProjectionC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.Viewport."
		"ProjectionC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionViewportProjectionC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedViewportFixture Fixture;
	if (!TestTrue(TEXT("production projection fixture is valid"),
			Fixture.IsViewportValid() && Fixture.IsMeshSelectionValid()))
	{
		return false;
	}
	const FString SelectionRef = Fixture.MeshComponent->GetPathName();
	TSharedPtr<FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(
			*this,
			*Fixture.World,
			FOwnedViewportFixture::Identity,
			*Fixture.Client,
			*Fixture.SceneViewport,
			Fixture.SelectionManager,
			{{SelectionRef, Fixture.MeshComponent}},
			Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ViewportIdentity = FOwnedViewportFixture::Identity;
	Bindings.ExplicitRefs = {
		{TEXT("actor:target"), Fixture.Target->GetPathName()},
		{TEXT("actor:ignored"), Fixture.IgnoredTarget->GetPathName()},
		{TEXT("mesh:fixture"), Fixture.Mesh->GetPathName()},
	};
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FOwnedViewportFixture::Identity,
		{SelectionRef},
	};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("viewport_projection_owned_level_viewport"),
		Runtime.ToSharedRef(),
		Context,
		{},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			if (CapabilityId == TEXT("viewport.view_mode.list"))
			{
				RawInput->SetStringField(TEXT("query"), TEXT("shader complexity"));
			}
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("viewport.actor.select"))
			{
				Test.TestTrue(TEXT("production C3 selects the exact actor"),
					GEditor && GEditor->GetSelectedActors()
						&& GEditor->GetSelectedActors()->IsSelected(Fixture.Target));
			}
			else if (CapabilityId == TEXT("mesh.element.select_by_ray"))
			{
				UE::Geometry::FGeometrySelection Selection;
				Test.TestTrue(TEXT("production C3 selects a real mesh triangle"),
					Fixture.SelectionManager->GetSelectionForComponent(
						Fixture.MeshComponent, Selection)
						&& Selection.Selection.Contains(
							UE::Geometry::FGeoSelectionID::MeshTriangle(0).Encoded()));
			}
		},
		{},
		{},
		[](FAutomationTestBase& Test,
			const FString& CapabilityId,
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (CapabilityId == TEXT("viewport.pixel_scale.read"))
			{
				TestPixelScaleSnapshot(Test, Snapshot);
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionViewportSessionC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.Viewport."
		"SessionC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionViewportSessionC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedViewportFixture Fixture;
	if (!TestTrue(TEXT("production viewport session fixture is valid"),
			Fixture.IsViewportValid()))
	{
		return false;
	}
	TSharedPtr<FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(
			*this,
			*Fixture.World,
			FOwnedViewportFixture::Identity,
			*Fixture.Client,
			*Fixture.SceneViewport,
			nullptr,
			{},
			Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ViewportIdentity = FOwnedViewportFixture::Identity;
	Bindings.ExplicitRefs.Add(
		TEXT("actor:target"), Fixture.Target->GetPathName());
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FOwnedViewportFixture::Identity,
		{},
	};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("viewport_session_state_owned_client"),
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
			if (CapabilityId == TEXT("viewport.camera.frame"))
			{
				Test.TestFalse(TEXT("production C3 frames the exact actor"),
					Fixture.Client->GetViewLocation().Equals(FVector::ZeroVector));
			}
			else if (CapabilityId == TEXT("viewport.state.restore"))
			{
				Test.TestTrue(TEXT("production C3 restores the exact viewport"),
					Fixture.Client->GetViewLocation().Equals(
						FVector(0.0, 0.0, 300.0), 0.1));
			}
			else if (CapabilityId == TEXT("viewport.state.update"))
			{
				Test.TestTrue(TEXT("production C3 patches the exact viewport"),
					Fixture.Client->GetViewLocation().Equals(
						FVector(50.0, 0.0, 250.0), 0.1)
						&& FMath::IsNearlyEqual(Fixture.Client->ViewFOV, 75.0f, 0.01f)
						&& !Fixture.Client->IsRealtime()
						&& Fixture.Client->IsInGameView());
			}
		},
		{},
		{},
		[](FAutomationTestBase& Test,
			const FString& CapabilityId,
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			TestViewportCatalogSnapshot(Test, CapabilityId, Snapshot);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialViewportInstanceDiscoveryBootstrapTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production.Viewport."
		"InstanceDiscoveryBootstrap",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialViewportInstanceDiscoveryBootstrapTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedViewportFixture Fixture;
	if (!TestTrue(
			TEXT("viewport discovery fixture is valid"),
			Fixture.IsViewportValid()))
	{
		return false;
	}
	Fixture.SceneViewport->RemoveRenderableSurfaceForTest();
	TestEqual(
		TEXT("viewport discovery does not require a renderable surface"),
		Fixture.SceneViewport->GetSizeXY(),
		FIntPoint::ZeroValue);
	TSharedPtr<FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(
			*this,
			*Fixture.World,
			FOwnedViewportFixture::Identity,
			*Fixture.Client,
			*Fixture.SceneViewport,
			nullptr,
			{},
			Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FString(),
		{},
	};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("viewport_instance_discovery_without_prior_identity"),
		Runtime.ToSharedRef(),
		Context,
		{},
		{},
		{},
		{},
		{},
		[](FAutomationTestBase& Test,
			const FString& CapabilityId,
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			TestViewportCatalogSnapshot(Test, CapabilityId, Snapshot);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialViewportProjectionOwnedLevelViewportTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"ViewportProjectionOwnedLevelViewport",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialViewportProjectionOwnedLevelViewportTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("viewport_projection_owned_level_viewport")))
	{
		return false;
	}
	FOwnedViewportFixture Fixture;
	if (!TestTrue(
			TEXT("owned RHI viewport is valid"),
			Fixture.IsViewportValid())
		|| !TestTrue(
			TEXT("owned real Modeling selection session is valid"),
			Fixture.IsMeshSelectionValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FOwnedViewportFixture::Identity,
		{FOwnedViewportFixture::SelectionIdentity},
	};
	return RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("viewport_projection_owned_level_viewport"),
		MakeShareable(&Fixture, [](FOwnedViewportFixture*) {}),
		Context,
		{},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("viewport.actor.select"))
			{
				Test.TestTrue(
					TEXT("real editor selection owns the picked actor"),
					GEditor && GEditor->GetSelectedActors()
						&& GEditor->GetSelectedActors()->IsSelected(Fixture.Target));
			}
			if (CapabilityId == TEXT("mesh.element.select_by_ray"))
			{
				UE::Geometry::FGeometrySelection Selection;
				Test.TestTrue(
					TEXT("real Modeling selection reads the owned mesh target"),
					Fixture.SelectionManager->GetSelectionForComponent(
						Fixture.MeshComponent,
						Selection));
				Test.TestTrue(
					TEXT("real Modeling selection owns triangle zero"),
					Selection.Selection.Contains(
						UE::Geometry::FGeoSelectionID::MeshTriangle(0).Encoded()));
			}
		},
		{},
		{},
		[](FAutomationTestBase& Test,
			const FString& CapabilityId,
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (CapabilityId == TEXT("viewport.pixel_scale.read"))
			{
				TestPixelScaleSnapshot(Test, Snapshot);
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialMeshSelectionRequiresToolsContextTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Protection."
		"MeshSelectionRequiresToolsContext",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialMeshSelectionRequiresToolsContextTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("mesh_selection_requires_tools_context")))
	{
		return false;
	}
	FOwnedViewportFixture Fixture;
	if (!TestTrue(
			TEXT("owned protected viewport is initially valid"),
			Fixture.IsViewportValid() && Fixture.IsMeshSelectionValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FOwnedViewportFixture::Identity,
		{FOwnedViewportFixture::SelectionIdentity},
	};
	const TMap<FString, FString> ExpectedFailures{
		{
			TEXT("mesh.element.select_by_ray"),
			TEXT("world_spatial_mesh_selection_tools_context_invalid"),
		},
	};
	return RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("viewport_projection_owned_level_viewport"),
		MakeShareable(&Fixture, [](FOwnedViewportFixture*) {}),
		Context,
		{},
		{},
		[&Fixture](const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("mesh.element.select_by_ray"))
			{
				Fixture.ShutdownSelectionToolsContextForTest();
			}
		},
		ExpectedFailures);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialViewportSessionStateOwnedClientTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"ViewportSessionStateOwnedClient",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialViewportSessionStateOwnedClientTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("viewport_session_state_owned_client")))
	{
		return false;
	}
	FOwnedViewportFixture Fixture;
	if (!TestTrue(
			TEXT("owned viewport client is valid"),
			Fixture.IsViewportValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		Fixture.World->GetPathName(),
		FString(),
		FOwnedViewportFixture::Identity,
		{},
	};
	return RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("viewport_session_state_owned_client"),
		MakeShareable(&Fixture, [](FOwnedViewportFixture*) {}),
		Context,
		{},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("viewport.camera.frame"))
			{
				Test.TestFalse(
					TEXT("same viewport camera is framed around the owned target"),
					Fixture.Client->GetViewLocation().Equals(FVector::ZeroVector));
			}
			if (CapabilityId == TEXT("viewport.state.restore"))
			{
				Test.TestTrue(
					TEXT("same viewport restores captured camera location"),
					Fixture.Client->GetViewLocation().Equals(
						FVector(0.0, 0.0, 300.0),
						0.1));
				Test.TestTrue(
					TEXT("same viewport restores captured session flags"),
					Fixture.Client->IsRealtime()
						&& !Fixture.Client->IsInGameView());
			}
			if (CapabilityId == TEXT("viewport.state.update"))
			{
				Test.TestTrue(
					TEXT("same viewport client reads patched camera location"),
					Fixture.Client->GetViewLocation().Equals(
						FVector(50.0, 0.0, 250.0),
						0.1));
				Test.TestTrue(
					TEXT("same viewport client reads patched FOV"),
					FMath::IsNearlyEqual(Fixture.Client->ViewFOV, 75.0f, 0.01f));
				Test.TestEqual(
					TEXT("same viewport client reads patched view mode"),
					Fixture.Client->GetViewMode(),
					VMI_BrushWireframe);
				Test.TestTrue(
					TEXT("same viewport reads patched session flags"),
					!Fixture.Client->IsRealtime()
						&& Fixture.Client->IsInGameView());
				const int32 Lighting =
					FEngineShowFlags::FindIndexByName(TEXT("Lighting"));
				const int32 StaticMeshes =
					FEngineShowFlags::FindIndexByName(TEXT("StaticMeshes"));
				Test.TestTrue(
					TEXT("same viewport reads the exact curated show-flag patch"),
					Lighting >= 0 && StaticMeshes >= 0
						&& !Fixture.Client->EngineShowFlags.GetSingleFlag(Lighting)
						&& Fixture.Client->EngineShowFlags.GetSingleFlag(StaticMeshes));
			}
		},
		{},
		{},
		[](FAutomationTestBase& Test,
			const FString& CapabilityId,
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			TestViewportCatalogSnapshot(Test, CapabilityId, Snapshot);
		});
}
