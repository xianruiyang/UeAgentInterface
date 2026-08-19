#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "DomainConformance/UeAgentDomainConformanceHarness.h"
#include "FileHelpers.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditorViewport.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"
#include "PackageTools.h"
#include "Slate/SceneViewport.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"

namespace UE::UAI::Tests::StrictV2::T45
{
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	FString CanonicalJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FString Error;
		return CommandContract::Json::TryCanonicalize(Object, Result, Error)
			? Result : FString(TEXT("{}"));
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t45-product"), TEXT("req-t45-product"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t45_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t45-product-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t45-product\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(),
			*CanonicalJson(Params));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t45-product"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t45-product"),
				{TEXT("uai:query"), TEXT("uai:operate")},
				TEXT("corr-t45-product")),
			MakeShared<FNeverCancelled>());
	}

	TSharedRef<FJsonObject> PublicParams(
		const FString& ViewportIdentity,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("viewport_identity"), ViewportIdentity);
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), ViewportIdentity);
		Params->SetObjectField(TEXT("input"), Input);
		Params->SetObjectField(TEXT("context"), Context);
		return Params;
	}

	TSharedRef<FJsonObject> GraphPublicParams(
		const FString& GraphIdentity,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("asset_editor_identity"), GraphIdentity);
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), GraphIdentity);
		Params->SetObjectField(TEXT("input"), Input);
		Params->SetObjectField(TEXT("context"), Context);
		return Params;
	}

	TSharedRef<FJsonObject> TemporalPoint(const FString& Tick)
	{
		const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("clock_id"), TEXT("editor"));
		Point->SetStringField(TEXT("tick"), Tick);
		Point->SetStringField(TEXT("rate_numerator"), TEXT("60"));
		Point->SetStringField(TEXT("rate_denominator"), TEXT("1"));
		Point->SetStringField(TEXT("state"), TEXT("captured"));
		return Point;
	}

	FString WidgetModeId(const UE::Widget::EWidgetMode Mode)
	{
		switch (Mode)
		{
		case UE::Widget::WM_Translate: return TEXT("translate");
		case UE::Widget::WM_Rotate: return TEXT("rotate");
		case UE::Widget::WM_Scale: return TEXT("scale");
		default: return TEXT("none");
		}
	}

	FString ViewModeId(FLevelEditorViewportClient& Client)
	{
		switch (Client.GetViewMode())
		{
		case VMI_Unlit: return TEXT("unlit");
		case VMI_BrushWireframe:
		case VMI_Wireframe: return TEXT("wireframe");
		default: return TEXT("lit");
		}
	}

	TSharedRef<FJsonObject> CameraTransform(
		const FString& ViewportIdentity,
		const FVector& Location,
		const FRotator& Rotation)
	{
		const FQuat Quat = Rotation.Quaternion();
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("unreal.world"));
		Frame->SetStringField(
			TEXT("basis_frame_id"), TEXT("captured.viewport.session"));
		Frame->SetNumberField(TEXT("dimension"), 3);
		Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Frame->SetStringField(TEXT("basis_evidence_id"), TEXT("t45-public-camera"));
		Frame->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("1")));
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const double Value : {
			Location.X, Location.Y, Location.Z,
			Quat.X, Quat.Y, Quat.Z, Quat.W,
			1.0, 1.0, 1.0})
		{
			Components.Add(MakeShared<FJsonValueString>(
				FString::SanitizeFloat(Value, 9)));
		}
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetStringField(
			TEXT("subject"), ViewportIdentity + TEXT(".camera"));
		Transform->SetStringField(TEXT("quantity"), TEXT("transform"));
		Transform->SetObjectField(TEXT("frame"), Frame);
		Transform->SetArrayField(TEXT("components"), MoveTemp(Components));
		Transform->SetArrayField(
			TEXT("conversion_evidence_ids"),
			{MakeShared<FJsonValueString>(TEXT("t45-public-identity"))});
		return Transform;
	}

	TSharedRef<FJsonObject> CaptureState(
		const FString& ViewportIdentity,
		FLevelEditorViewportClient& Client)
	{
		const TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
		Camera->SetObjectField(TEXT("camera_transform"), CameraTransform(
			ViewportIdentity, Client.GetViewLocation(), Client.GetViewRotation()));
		Camera->SetNumberField(TEXT("fov_degrees"), Client.ViewFOV);
		Camera->SetStringField(TEXT("view_mode"), ViewModeId(Client));
		Camera->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("1")));

		TArray<TSharedPtr<FJsonValue>> ShowFlags;
		if (Client.EngineShowFlags.Lighting)
		{
			ShowFlags.Add(MakeShared<FJsonValueString>(TEXT("Lighting")));
		}
		if (Client.EngineShowFlags.StaticMeshes)
		{
			ShowFlags.Add(MakeShared<FJsonValueString>(TEXT("StaticMeshes")));
		}
		const TSharedRef<FJsonObject> Interaction = MakeShared<FJsonObject>();
		Interaction->SetBoolField(
			TEXT("mode_widgets"), Client.EngineShowFlags.ModeWidgets != 0);
		Interaction->SetBoolField(
			TEXT("selection"), Client.EngineShowFlags.Selection != 0);
		Interaction->SetBoolField(
			TEXT("pivot"), Client.EngineShowFlags.Pivot != 0);
		Interaction->SetBoolField(
			TEXT("editor_primitives"), Client.EngineShowFlags.Editor != 0);
		Interaction->SetBoolField(
			TEXT("game_primitives"), Client.EngineShowFlags.Game != 0);
		Interaction->SetStringField(
			TEXT("widget_mode"), WidgetModeId(Client.GetWidgetMode()));

		const TSharedRef<FJsonObject> Revision = MakeShared<FJsonObject>();
		Revision->SetStringField(TEXT("source_id"), ViewportIdentity);
		Revision->SetStringField(TEXT("generation"), TEXT("t45-public-captured"));
		Revision->SetNumberField(TEXT("sequence"), 1);
		const TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("viewport_ref"), ViewportIdentity);
		Snapshot->SetObjectField(TEXT("camera"), Camera);
		Snapshot->SetBoolField(TEXT("realtime"), Client.IsRealtime());
		Snapshot->SetBoolField(TEXT("game_view"), Client.IsInGameView());
		Snapshot->SetArrayField(TEXT("show_flags"), MoveTemp(ShowFlags));
		Snapshot->SetObjectField(TEXT("interaction"), Interaction);
		Snapshot->SetObjectField(TEXT("observed_revision"), Revision);
		return Snapshot;
	}

	TSharedRef<FJsonObject> FramePolicy()
	{
		const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
		Policy->SetNumberField(TEXT("padding_ratio"), 0.15);
		Policy->SetNumberField(TEXT("minimum_distance_cm"), 100);
		Policy->SetNumberField(TEXT("maximum_distance_cm"), 10000);
		Policy->SetBoolField(TEXT("preserve_rotation"), false);
		Policy->SetBoolField(TEXT("collision_aware"), true);
		Policy->SetBoolField(TEXT("look_at"), true);
		Policy->SetBoolField(TEXT("auto_fallback"), true);
		return Policy;
	}

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("graph.view.read"),
			TEXT("graph.view.update"),
			TEXT("viewport.actor.pick"),
			TEXT("viewport.actor.select"),
			TEXT("viewport.camera.frame"),
			TEXT("viewport.instance.list"),
			TEXT("viewport.pixel_scale.read"),
			TEXT("viewport.ray.deproject"),
			TEXT("viewport.ray.trace"),
			TEXT("viewport.state.read"),
			TEXT("viewport.state.restore"),
			TEXT("viewport.state.update"),
			TEXT("viewport.view_mode.list"),
			TEXT("viewport.warning.list"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedOperations()
	{
		static const TArray<FString> Commands = {
			TEXT("graph.view.update"),
			TEXT("viewport.actor.select"),
			TEXT("viewport.camera.frame"),
			TEXT("viewport.state.restore"),
			TEXT("viewport.state.update"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T45ViewportGraphInventoryTest,
	"UAI.StrictV2.T45.VIEWPORT.GRAPH.CONTROL.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T45ViewportGraphInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::DomainConformance;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T45;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T45 owns exactly 14 commands"), ExpectedCommands().Num(), 14);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T45 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("viewport-graph-control-owner")));
		}
	}

	TSharedPtr<const FFrozenDomainCapabilitySet> FrozenCapabilities;
	if (!TestTrue(TEXT("the process domain capability set freezes"),
		TryFreezeProcessDomainCapabilities(FrozenCapabilities, Error))
		|| !FrozenCapabilities.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* ViewportGraph =
		FrozenCapabilities->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("viewport-graph-control");
			});
	TestNotNull(TEXT("T45 owns one typed domain contribution"), ViewportGraph);
	if (!ViewportGraph)
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*ViewportGraph)->GetDeclaration();
	TestEqual(TEXT("T45 contributes one public command owner"),
		Declaration.Command.Contributions.Num(), 1);
	TestEqual(TEXT("T45 declares exactly five transient operations"),
		Declaration.Operation.Operations.Num(), ExpectedOperations().Num());
	for (const FString& OperationId : ExpectedOperations())
	{
		const FDomainOperationEntry* Operation =
			Declaration.Operation.Operations.FindByPredicate(
				[&OperationId](const FDomainOperationEntry& Item)
				{
					return Item.OperationId == OperationId;
				});
		TestNotNull(*FString::Printf(TEXT("%s has one typed operation"),
			*OperationId), Operation);
		if (!Operation)
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s is not persisted"), *OperationId),
			Operation->bPersists);
		TestTrue(*FString::Printf(TEXT("%s has independent readback"),
			*OperationId), Operation->bIndependentReadback);
		TestEqual(*FString::Printf(TEXT("%s is immediate"), *OperationId),
			Operation->ExecutionPolicy,
			EDomainOperationExecutionPolicy::Immediate);
		TestEqual(*FString::Printf(TEXT("%s is recoverable transient state"),
			*OperationId), Operation->TransientStatePolicy,
			EDomainTransientStatePolicy::RecoverableSession);
		TestTrue(*FString::Printf(TEXT("%s has a request-scoped factory"),
			*OperationId), Operation->InvocationFactory.IsValid());
		TestTrue(*FString::Printf(TEXT("%s shares the owner factory"),
			*OperationId),
			Operation->InvocationFactory.IsValid()
				&& OperationOwners::ViewportGraph::
					FindOperationInvocationFactory(OperationId).Get()
					== Operation->InvocationFactory.Get());
	}
	TestEqual(TEXT("T45 owns one migration identity"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T45-VIEWPORT-GRAPH-CONTROL")));
	TestEqual(TEXT("T45 does not register a persistent layout provider"),
		Declaration.Layout.Support, EDomainCapabilitySupport::Unsupported);

	FDomainConformanceFixture Fixture;
	Fixture.FixtureId = TEXT("t45-viewport-graph-control");
	Fixture.DomainId = TEXT("viewport-graph-control");
	Fixture.TargetIdentity = TEXT("viewport:owned-level-viewport");
	Fixture.BaseRevision = TEXT("viewport-transient-state-current");
	Fixture.MechanicsFamily = TEXT("viewport-graph-transient-control");
	Fixture.PublicRouteId = TEXT("viewport.state.update");
	Fixture.PublicAutomationTestId =
		TEXT("UAI.StrictV2.T45.VIEWPORT.GRAPH.CONTROL.PublicProductLifecycle");
	Fixture.bPublicRoute = true;
	Fixture.bRealMechanics = true;
	Fixture.bIndependentReadback = true;
	Fixture.bPersistReload = false;
	Fixture.bFailureCleanup = true;
	Fixture.bLegacyExit = true;
	FDomainConformancePlan Plan;
	TestTrue(TEXT("T45 consumes the shared conformance planner"),
		TryBuildDomainConformancePlan(*ViewportGraph, Fixture, Plan, Error));
	TestTrue(TEXT("T45 plans process authority adoption"),
		Plan.Contains(EDomainConformanceScenario::ProcessAuthorityAdoption));
	TestTrue(TEXT("T45 plans request-factory isolation"),
		Plan.Contains(EDomainConformanceScenario::OperationRequestFactory));
	TestTrue(TEXT("T45 plans failure cleanup"),
		Plan.Contains(EDomainConformanceScenario::FailureCleanup));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T45ViewportGraphPublicProductTest,
	"UAI.StrictV2.T45.VIEWPORT.GRAPH.CONTROL.PublicProductLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T45ViewportGraphPublicProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T45;
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the public product composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World() : nullptr;
	FLevelEditorViewportClient* Client = nullptr;
	int32 ViewportIndex = INDEX_NONE;
	if (GEditor)
	{
		const TArray<FLevelEditorViewportClient*>& Clients =
			GEditor->GetLevelViewportClients();
		for (int32 Index = 0; Index < Clients.Num(); ++Index)
		{
			if (Clients[Index] && Clients[Index]->Viewport
				&& Clients[Index]->GetWorld() == World)
			{
				Client = Clients[Index];
				ViewportIndex = Index;
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("one real level viewport is available"), Client)
		|| !TestNotNull(TEXT("the real editor world is available"), World))
	{
		return false;
	}
	const FString ViewportIdentity = FString::Printf(
		TEXT("level:%d"), ViewportIndex);
	FSceneViewport* const SceneViewport =
		static_cast<FSceneViewport*>(Client->Viewport);
	const FIntPoint OriginalViewportSize = Client->Viewport->GetSizeXY();
	if (OriginalViewportSize.X <= 0 || OriginalViewportSize.Y <= 0)
	{
		SceneViewport->SetViewportSize(1280, 720);
		Client->Invalidate();
	}
	if (!TestTrue(TEXT("the owned headless level viewport has an RHI size"),
		Client->Viewport->GetSizeXY().X > 0
			&& Client->Viewport->GetSizeXY().Y > 0))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (OriginalViewportSize.X <= 0 || OriginalViewportSize.Y <= 0)
		{
			SceneViewport->SetViewportSize(
				OriginalViewportSize.X, OriginalViewportSize.Y);
		}
	};
	const TSharedRef<FJsonObject> Baseline = CaptureState(
		ViewportIdentity, *Client);
	const FVector BaselineLocation = Client->GetViewLocation();
	const FRotator BaselineRotation = Client->GetViewRotation();
	const float BaselineFov = Client->ViewFOV;
	const bool bBaselineRealtime = Client->IsRealtime();
	const bool bBaselineGameView = Client->IsInGameView();
	const UE::Widget::EWidgetMode BaselineWidgetMode = Client->GetWidgetMode();

	TArray<TWeakObjectPtr<AActor>> OriginalSelection;
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
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FName Folder(*FString(TEXT("UAI/T45/") + Suffix));
	UStaticMesh* const Cube = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient | RF_TextExportTransient;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* First = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FTransform::Identity, Spawn);
	AStaticMeshActor* Second = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("first real actor fixture exists"), First)
		|| !TestNotNull(TEXT("second real actor fixture exists"), Second)
		|| !TestNotNull(TEXT("the real cube mesh exists"), Cube))
	{
		if (First) First->Destroy();
		if (Second) Second->Destroy();
		return false;
	}
	First->SetActorLabel(TEXT("T45ViewportFirst_") + Suffix);
	Second->SetActorLabel(TEXT("T45ViewportSecond_") + Suffix);
	First->SetFolderPath(Folder);
	Second->SetFolderPath(Folder);
	First->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Second->GetStaticMeshComponent()->SetStaticMesh(Cube);
	const FVector Forward = BaselineRotation.Vector().GetSafeNormal();
	const FVector Right = FRotationMatrix(BaselineRotation).GetUnitAxis(EAxis::Y);
	First->SetActorLocation(BaselineLocation + Forward * 500.0);
	Second->SetActorLocation(BaselineLocation + Forward * 500.0 + Right * 150.0);
	First->GetStaticMeshComponent()->UpdateBounds();
	Second->GetStaticMeshComponent()->UpdateBounds();

	auto RestoreSelection = [&]()
	{
		if (!GEditor) return;
		GEditor->SelectNone(false, true, false);
		for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
		{
			if (Actor.IsValid())
			{
				GEditor->SelectActor(Actor.Get(), true, false, true);
			}
		}
	};
	ON_SCOPE_EXIT
	{
		const TSharedRef<FJsonObject> RestoreInput = MakeShared<FJsonObject>();
		RestoreInput->SetObjectField(TEXT("snapshot"), Baseline);
		SubmitPublic(
			Registry, Dispatch, TEXT("viewport.state.restore"),
			PublicParams(ViewportIdentity, RestoreInput));
		RestoreSelection();
		if (First && IsValid(First)) First->Destroy();
		if (Second && IsValid(Second)) Second->Destroy();
	};

	auto RequireOk = [this](
		const FString& Stage, const FUeAgentCommandResult& Result)
	{
		if (!Result.IsOk())
		{
			AddError(Stage + TEXT(": ") + Result.GetErrorJson());
			return false;
		}
		return true;
	};
	auto Call = [&](const FString& CommandId,
		const TSharedRef<FJsonObject>& Input)
	{
		return SubmitPublic(
			Registry, Dispatch, CommandId,
			PublicParams(ViewportIdentity, Input));
	};

	const FUeAgentCommandResult Instances = Call(
		TEXT("viewport.instance.list"), MakeShared<FJsonObject>());
	TestTrue(TEXT("public viewport catalog uses real mechanics"),
		RequireOk(TEXT("viewport.instance.list"), Instances)
			&& Instances.GetPayloadJson().Contains(ViewportIdentity));
	const FUeAgentCommandResult Modes = Call(
		TEXT("viewport.view_mode.list"), MakeShared<FJsonObject>());
	TestTrue(TEXT("public view-mode catalog is non-synthetic"),
		RequireOk(TEXT("viewport.view_mode.list"), Modes)
			&& Modes.GetPayloadJson().Contains(TEXT("viewport_modes.catalog"))
			&& Modes.GetPayloadJson().Contains(TEXT("mode_id")));
	const TSharedRef<FJsonObject> WarningInput = MakeShared<FJsonObject>();
	WarningInput->SetStringField(TEXT("viewport_ref"), ViewportIdentity);
	WarningInput->SetBoolField(TEXT("include_suppressed"), true);
	const FUeAgentCommandResult Warnings = Call(
		TEXT("viewport.warning.list"), WarningInput);
	TestTrue(TEXT("public warning query uses the real warning probe"),
		RequireOk(TEXT("viewport.warning.list"), Warnings)
			&& Warnings.GetPayloadJson().Contains(TEXT("viewport_warnings")));

	const TSharedRef<FJsonObject> ReadInput = MakeShared<FJsonObject>();
	const FUeAgentCommandResult BeforeRead = Call(
		TEXT("viewport.state.read"), ReadInput);
	TestTrue(TEXT("public state read captures camera and interaction"),
		RequireOk(TEXT("viewport.state.read before"), BeforeRead)
			&& BeforeRead.GetPayloadJson().Contains(
				TEXT("viewport_state.camera_transform"))
			&& BeforeRead.GetPayloadJson().Contains(
				TEXT("viewport_state.interaction.widget_mode")));

	const TArray<TSharedPtr<FJsonValue>> ActorRefs = {
		MakeShared<FJsonValueString>(First->GetPathName()),
		MakeShared<FJsonValueString>(Second->GetPathName()),
	};
	const TSharedRef<FJsonObject> DirectSelection = MakeShared<FJsonObject>();
	DirectSelection->SetArrayField(TEXT("actor_refs"), ActorRefs);
	DirectSelection->SetStringField(TEXT("selection_mode"), TEXT("replace"));
	const FUeAgentCommandResult Selected = Call(
		TEXT("viewport.actor.select"), DirectSelection);
	TestTrue(TEXT("public explicit actor identity set succeeds"),
		RequireOk(TEXT("viewport.actor.select direct"), Selected));
	TestTrue(TEXT("public selection owns the exact two-actor set"),
		GEditor && GEditor->GetSelectedActors()
			&& GEditor->GetSelectedActors()->Num() == 2
			&& GEditor->GetSelectedActors()->IsSelected(First)
			&& GEditor->GetSelectedActors()->IsSelected(Second));

	for (const FString Selector : {TEXT("subjects"), TEXT("folder"), TEXT("selection")})
	{
		const TSharedRef<FJsonObject> FrameInput = MakeShared<FJsonObject>();
		if (Selector == TEXT("subjects"))
		{
			FrameInput->SetArrayField(TEXT("subject_refs"), ActorRefs);
		}
		else if (Selector == TEXT("folder"))
		{
			FrameInput->SetStringField(TEXT("folder_path"), Folder.ToString());
			FrameInput->SetBoolField(TEXT("include_child_folders"), true);
		}
		else
		{
			FrameInput->SetBoolField(TEXT("use_current_selection"), true);
		}
		FrameInput->SetObjectField(TEXT("frame_policy"), FramePolicy());
		const FUeAgentCommandResult Framed = Call(
			TEXT("viewport.camera.frame"), FrameInput);
		TestTrue(TEXT("public camera frame selector succeeds: ") + Selector,
			RequireOk(TEXT("viewport.camera.frame ") + Selector, Framed));
	}

	const FIntPoint Size = Client->Viewport->GetSizeXY();
	auto ScreenPoint = [&]()
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("viewport.screen.pixel"));
		Frame->SetStringField(TEXT("basis_frame_id"), ViewportIdentity);
		Frame->SetNumberField(TEXT("dimension"), 2);
		Frame->SetStringField(TEXT("unit"), TEXT("pixel"));
		Frame->SetStringField(TEXT("basis_evidence_id"), TEXT("t45-public-screen"));
		Frame->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("2")));
		const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("subject"), ViewportIdentity);
		Point->SetStringField(TEXT("quantity"), TEXT("point"));
		Point->SetObjectField(TEXT("frame"), Frame);
		Point->SetArrayField(TEXT("components"), {
			MakeShared<FJsonValueString>(FString::FromInt(Size.X / 2)),
			MakeShared<FJsonValueString>(FString::FromInt(Size.Y / 2)),
		});
		Point->SetArrayField(TEXT("conversion_evidence_ids"), {});
		return Point;
	};
	auto TraceQuery = [&]()
	{
		const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetNumberField(TEXT("max_distance_cm"), 10000);
		Query->SetStringField(TEXT("trace_channel"), TEXT("visibility"));
		Query->SetBoolField(TEXT("trace_complex"), true);
		Query->SetArrayField(TEXT("ignore_target_refs"), {});
		return Query;
	};
	for (const FString CommandId : {
		TEXT("viewport.ray.deproject"), TEXT("viewport.ray.trace"),
		TEXT("viewport.actor.pick")})
	{
		const TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetObjectField(TEXT("screen_point"), ScreenPoint());
		if (CommandId != TEXT("viewport.ray.deproject"))
		{
			Input->SetObjectField(TEXT("trace_query"), TraceQuery());
			if (CommandId == TEXT("viewport.actor.pick"))
			{
				Input->SetBoolField(TEXT("allow_no_hit"), true);
			}
		}
		TestTrue(TEXT("public projection command succeeds: ") + CommandId,
			RequireOk(CommandId, Call(CommandId, Input)));
	}
	const TSharedRef<FJsonObject> PixelInput = MakeShared<FJsonObject>();
	PixelInput->SetObjectField(TEXT("sample_point"), CameraTransform(
		First->GetPathName(), First->GetActorLocation(), FRotator::ZeroRotator));
	// Pixel scale requires a point, not the transform wrapper used for cameras.
	TSharedPtr<FJsonObject> PixelPoint = PixelInput->GetObjectField(
		TEXT("sample_point"));
	PixelPoint->SetStringField(TEXT("quantity"), TEXT("point"));
	PixelPoint->SetArrayField(TEXT("components"), {
		MakeShared<FJsonValueString>(FString::SanitizeFloat(First->GetActorLocation().X)),
		MakeShared<FJsonValueString>(FString::SanitizeFloat(First->GetActorLocation().Y)),
		MakeShared<FJsonValueString>(FString::SanitizeFloat(First->GetActorLocation().Z)),
	});
	TestTrue(TEXT("public pixel-scale query succeeds"), RequireOk(
		TEXT("viewport.pixel_scale.read"),
		Call(TEXT("viewport.pixel_scale.read"), PixelInput)));

	const TSharedRef<FJsonObject> Interaction = MakeShared<FJsonObject>();
	Interaction->SetBoolField(TEXT("mode_widgets"), true);
	Interaction->SetBoolField(TEXT("selection"), true);
	Interaction->SetBoolField(TEXT("pivot"), true);
	Interaction->SetBoolField(TEXT("editor_primitives"), false);
	Interaction->SetBoolField(TEXT("game_primitives"), true);
	Interaction->SetStringField(TEXT("widget_mode"), TEXT("rotate"));
	const TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetNumberField(TEXT("fov_degrees"), 67.0);
	Patch->SetStringField(TEXT("view_mode"), TEXT("wireframe"));
	Patch->SetBoolField(TEXT("realtime"), !bBaselineRealtime);
	Patch->SetBoolField(TEXT("game_view"), !bBaselineGameView);
	Patch->SetArrayField(TEXT("show_flags"), {
		MakeShared<FJsonValueString>(TEXT("StaticMeshes"))});
	Patch->SetObjectField(TEXT("interaction"), Interaction);
	const TSharedRef<FJsonObject> UpdateInput = MakeShared<FJsonObject>();
	UpdateInput->SetObjectField(TEXT("state_patch"), Patch);
	const FUeAgentCommandResult Updated = Call(
		TEXT("viewport.state.update"), UpdateInput);
	if (!Updated.IsOk())
	{
		AddInfo(FString::Printf(
			TEXT("T45 state-update actual fov=%.9g mode=%s realtime=%d game_view=%d flags=%d/%d interaction=%d/%d/%d/%d/%d/%s"),
			Client->ViewFOV,
			*ViewModeId(*Client),
			Client->IsRealtime(),
			Client->IsInGameView(),
			Client->EngineShowFlags.Lighting != 0,
			Client->EngineShowFlags.StaticMeshes != 0,
			Client->EngineShowFlags.ModeWidgets != 0,
			Client->EngineShowFlags.Selection != 0,
			Client->EngineShowFlags.Pivot != 0,
			Client->EngineShowFlags.Editor != 0,
			Client->EngineShowFlags.Game != 0,
			*WidgetModeId(Client->GetWidgetMode())));
	}
	TestTrue(TEXT("public state update succeeds"),
		RequireOk(TEXT("viewport.state.update"), Updated));
	const FUeAgentCommandResult UpdatedRead = Call(
		TEXT("viewport.state.read"), MakeShared<FJsonObject>());
	TestTrue(TEXT("independent public read observes the update"),
		RequireOk(TEXT("viewport.state.read updated"), UpdatedRead)
			&& UpdatedRead.GetPayloadJson().Contains(TEXT("67"))
			&& UpdatedRead.GetPayloadJson().Contains(TEXT("wireframe"))
			&& UpdatedRead.GetPayloadJson().Contains(TEXT("rotate")));

	const TSharedRef<FJsonObject> InvalidFrame = MakeShared<FJsonObject>();
	InvalidFrame->SetArrayField(TEXT("subject_refs"), ActorRefs);
	InvalidFrame->SetBoolField(TEXT("use_current_selection"), true);
	InvalidFrame->SetObjectField(TEXT("frame_policy"), FramePolicy());
	const FVector BeforeInvalid = Client->GetViewLocation();
	const FUeAgentCommandResult Invalid = Call(
		TEXT("viewport.camera.frame"), InvalidFrame);
	TestFalse(TEXT("ambiguous frame selector fails before success"), Invalid.IsOk());
	TestTrue(TEXT("ambiguous frame selector leaves camera unchanged"),
		Client->GetViewLocation().Equals(BeforeInvalid, 0.01));

	const TSharedRef<FJsonObject> RestoreInput = MakeShared<FJsonObject>();
	RestoreInput->SetObjectField(TEXT("snapshot"), Baseline);
	const FUeAgentCommandResult Restored = Call(
		TEXT("viewport.state.restore"), RestoreInput);
	TestTrue(TEXT("public state restore succeeds"),
		RequireOk(TEXT("viewport.state.restore"), Restored));
	const FUeAgentCommandResult RestoredRead = Call(
		TEXT("viewport.state.read"), MakeShared<FJsonObject>());
	TestTrue(TEXT("independent public read observes the restored state"),
		RequireOk(TEXT("viewport.state.read restored"), RestoredRead)
			&& Client->GetViewLocation().Equals(BaselineLocation, 0.01)
			&& Client->GetViewRotation().Equals(BaselineRotation, 0.01)
			&& FMath::IsNearlyEqual(Client->ViewFOV, BaselineFov, 0.01f)
			&& Client->IsRealtime() == bBaselineRealtime
			&& Client->IsInGameView() == bBaselineGameView
			&& Client->GetWidgetMode() == BaselineWidgetMode);

	// Exercise the active public registry against a real SGraphEditor without
	// opening a window, including Blueprint LastEditedDocuments persistence.
	const FString GraphPackageName = TEXT("/Game/UAI/Tests/T47/GraphView_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const GraphPackage = CreatePackage(*GraphPackageName);
	UBlueprint* const GraphBlueprint = GraphPackage
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GraphPackage, TEXT("BP_T45Graph"),
			BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None)
		: nullptr;
	UEdGraph* const Graph = GraphBlueprint
		&& !GraphBlueprint->UbergraphPages.IsEmpty()
		? GraphBlueprint->UbergraphPages[0]
		: nullptr;
	if (GraphBlueprint)
	{
		FAssetRegistryModule::AssetCreated(GraphBlueprint);
		FKismetEditorUtilities::CompileBlueprint(GraphBlueprint);
		GraphBlueprint->MarkPackageDirty();
	}
	TSharedPtr<SGraphEditor> GraphEditor;
	if (Graph)
	{
		GraphEditor = SNew(SGraphEditor).GraphToEdit(Graph);
	}
	if (!TestNotNull(TEXT("owned public graph package exists"), GraphPackage)
		|| !TestNotNull(TEXT("owned public graph asset exists"), GraphBlueprint)
		|| !TestNotNull(TEXT("owned public graph exists"), Graph)
		|| !TestTrue(TEXT("owned Blueprint saves before view mutation"),
			UEditorLoadingAndSavingUtils::SavePackages({GraphPackage}, false))
		|| !TestTrue(TEXT("a real graph editor is registered without a window"),
			GraphEditor.IsValid()
				&& SGraphEditor::FindGraphEditorForGraph(Graph).IsValid()))
	{
		if (GraphPackage)
		{
			GraphPackage->SetDirtyFlag(false);
			GraphPackage->MarkAsGarbage();
		}
		return false;
	}
	GraphEditor->SetViewLocation(FVector2f(12.0f, 24.0f), 0.75f);
	GraphPackage->SetDirtyFlag(false);
	const FString GraphIdentity = Graph->GetPathName();
	const FString GraphBlueprintObjectPath = GraphBlueprint->GetPathName();
	auto GraphCall = [&](const FString& CommandId,
		const TSharedRef<FJsonObject>& Input)
	{
		return SubmitPublic(
			Registry, Dispatch, CommandId,
			GraphPublicParams(GraphIdentity, Input));
	};

	const FUeAgentCommandResult GraphBaselineRead = GraphCall(
		TEXT("graph.view.read"), MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> GraphBaselinePayload;
	FString GraphParseError;
	const TSharedPtr<FJsonObject>* Projection = nullptr;
	const TSharedPtr<FJsonObject>* ProjectedPayload = nullptr;
	const TSharedPtr<FJsonObject>* Snapshot = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	FString GraphRevisionSource;
	FString GraphRevisionGeneration;
	FString GraphRevisionSequenceText;
	int64 GraphRevisionSequence = 0;
	auto TryReadGraphEntry = [&](const FString& Role,
		const FString& ScalarField, FString& OutValue)
	{
		if (!Entries)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid()
				? EntryValue->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* Identity = nullptr;
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FString ActualRole;
			if (Entry.IsValid()
				&& Entry->TryGetObjectField(TEXT("identity"), Identity)
				&& Identity && Identity->IsValid()
				&& (*Identity)->TryGetStringField(TEXT("object_role"), ActualRole)
				&& ActualRole == Role
				&& Entry->TryGetObjectField(TEXT("value"), Value)
				&& Value && Value->IsValid())
			{
				return (*Value)->TryGetStringField(ScalarField, OutValue);
			}
		}
		return false;
	};
	const bool bGraphBaselineValid =
		RequireOk(TEXT("graph.view.read baseline"), GraphBaselineRead)
		&& CommandContract::Json::TryParseObject(
			GraphBaselineRead.GetPayloadJson(), GraphBaselinePayload, GraphParseError)
		&& GraphBaselinePayload.IsValid()
		&& GraphBaselinePayload->TryGetObjectField(TEXT("projection"), Projection)
		&& Projection && Projection->IsValid()
		&& (*Projection)->TryGetObjectField(TEXT("payload"), ProjectedPayload)
		&& ProjectedPayload && ProjectedPayload->IsValid()
		&& (*ProjectedPayload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
		&& Snapshot && Snapshot->IsValid()
		&& (*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
		&& TryReadGraphEntry(
			TEXT("graph_view.revision.source_id"), TEXT("text"), GraphRevisionSource)
		&& TryReadGraphEntry(
			TEXT("graph_view.revision.generation"), TEXT("text"), GraphRevisionGeneration)
		&& TryReadGraphEntry(
			TEXT("graph_view.revision.sequence"), TEXT("lexeme"), GraphRevisionSequenceText)
		&& LexTryParseString(GraphRevisionSequence, *GraphRevisionSequenceText)
		&& GraphRevisionSource == GraphIdentity
		&& !GraphRevisionGeneration.IsEmpty()
		&& GraphBaselineRead.GetPayloadJson().Contains(TEXT("graph_view.location"))
		&& GraphBaselineRead.GetPayloadJson().Contains(TEXT("graph_view.zoom"));
	TestTrue(TEXT("public graph read exposes an exact restorable view state"),
		bGraphBaselineValid);

	TSharedRef<FJsonObject> GraphRevision = MakeShared<FJsonObject>();
	GraphRevision->SetStringField(TEXT("source_id"), GraphRevisionSource);
	GraphRevision->SetStringField(TEXT("generation"), GraphRevisionGeneration);
	GraphRevision->SetNumberField(TEXT("sequence"), GraphRevisionSequence);
	const TSharedRef<FJsonObject> GraphFrame = MakeShared<FJsonObject>();
	GraphFrame->SetStringField(TEXT("frame_id"), TEXT("graph.editor.view.local"));
	GraphFrame->SetStringField(TEXT("basis_frame_id"), TEXT("graph.editor.view.local"));
	GraphFrame->SetNumberField(TEXT("dimension"), 2);
	GraphFrame->SetStringField(TEXT("unit"), TEXT("graph_unit"));
	GraphFrame->SetStringField(TEXT("basis_evidence_id"), TEXT("t45-public-graph-basis"));
	GraphFrame->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("3")));
	auto MakeGraphState = [&](const FString& X, const FString& Y,
		const double Zoom, const TSharedRef<FJsonObject>& Revision)
	{
		const TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetStringField(TEXT("subject"), GraphIdentity);
		Location->SetStringField(TEXT("quantity"), TEXT("point"));
		Location->SetObjectField(TEXT("frame"), GraphFrame);
		Location->SetArrayField(TEXT("components"), {
			MakeShared<FJsonValueString>(X),
			MakeShared<FJsonValueString>(Y),
		});
		Location->SetArrayField(TEXT("conversion_evidence_ids"), {
			MakeShared<FJsonValueString>(TEXT("t45-public-graph-identity")),
		});
		const TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetObjectField(TEXT("location"), Location);
		State->SetNumberField(TEXT("zoom"), Zoom);
		State->SetObjectField(TEXT("graph_revision"), Revision);
		return State;
	};
	const TSharedRef<FJsonObject> GraphBaselineState = MakeGraphState(
		TEXT("12"), TEXT("24"), 0.75, GraphRevision);
	const TSharedRef<FJsonObject> GraphUpdateState = MakeGraphState(
		TEXT("320.3"), TEXT("180.3"), 1.25, GraphRevision);
	const TSharedRef<FJsonObject> GraphUpdateInput = MakeShared<FJsonObject>();
	GraphUpdateInput->SetObjectField(TEXT("view_state"), GraphUpdateState);
	GraphUpdateInput->SetBoolField(TEXT("update_document_state"), true);
	GraphUpdateInput->SetBoolField(TEXT("save_after_update"), true);
	const FUeAgentCommandResult GraphUpdated = GraphCall(
		TEXT("graph.view.update"), GraphUpdateInput);
	FVector2f UpdatedGraphLocation = FVector2f::ZeroVector;
	float UpdatedGraphZoom = 0.0f;
	GraphEditor->GetViewLocation(UpdatedGraphLocation, UpdatedGraphZoom);
	TestTrue(TEXT("public graph update writes the real graph editor view"),
		bGraphBaselineValid
			&& RequireOk(TEXT("graph.view.update"), GraphUpdated)
			&& UpdatedGraphLocation.Equals(FVector2f(320.3f, 180.3f), 0.01f)
			&& FMath::IsNearlyEqual(UpdatedGraphZoom, 1.25f, 0.001f));
	const FUeAgentCommandResult GraphUpdatedRead = GraphCall(
		TEXT("graph.view.read"), MakeShared<FJsonObject>());
	TestTrue(TEXT("independent public graph read observes the update"),
		RequireOk(TEXT("graph.view.read updated"), GraphUpdatedRead)
			&& GraphUpdatedRead.GetPayloadJson().Contains(TEXT("320"))
			&& GraphUpdatedRead.GetPayloadJson().Contains(TEXT("180"))
			&& GraphUpdatedRead.GetPayloadJson().Contains(TEXT("1.25"))
			&& GraphUpdatedRead.GetPayloadJson().Contains(
				TEXT("graph_view.document.location"))
			&& !GraphPackage->IsDirty());
	const TSharedRef<FJsonObject> InvalidGraphRevision = MakeShared<FJsonObject>();
	InvalidGraphRevision->SetStringField(TEXT("source_id"), GraphRevisionSource);
	InvalidGraphRevision->SetStringField(TEXT("generation"), GraphRevisionGeneration);
	InvalidGraphRevision->SetNumberField(
		TEXT("sequence"), GraphRevisionSequence + 1);
	const TSharedRef<FJsonObject> InvalidGraphInput = MakeShared<FJsonObject>();
	InvalidGraphInput->SetObjectField(TEXT("view_state"), MakeGraphState(
		TEXT("900"), TEXT("900"), 0.5, InvalidGraphRevision));
	const FUeAgentCommandResult InvalidGraphUpdate = GraphCall(
		TEXT("graph.view.update"), InvalidGraphInput);
	FVector2f AfterInvalidGraphLocation = FVector2f::ZeroVector;
	float AfterInvalidGraphZoom = 0.0f;
	GraphEditor->GetViewLocation(AfterInvalidGraphLocation, AfterInvalidGraphZoom);
	TestFalse(TEXT("stale graph revision is rejected before mutation"),
		InvalidGraphUpdate.IsOk());
	TestTrue(TEXT("stale graph revision leaves the graph view unchanged"),
		AfterInvalidGraphLocation.Equals(FVector2f(320.3f, 180.3f), 0.01f)
			&& FMath::IsNearlyEqual(AfterInvalidGraphZoom, 1.25f, 0.001f));

	if (bGraphBaselineValid)
	{
		const TSharedRef<FJsonObject> GraphRestoreInput = MakeShared<FJsonObject>();
		GraphRestoreInput->SetObjectField(
			TEXT("view_state"), GraphBaselineState);
		const FUeAgentCommandResult GraphRestored = GraphCall(
			TEXT("graph.view.update"), GraphRestoreInput);
		FVector2f RestoredGraphLocation = FVector2f::ZeroVector;
		float RestoredGraphZoom = 0.0f;
		GraphEditor->GetViewLocation(RestoredGraphLocation, RestoredGraphZoom);
		TestTrue(TEXT("public graph update restores its exact captured state"),
			RequireOk(TEXT("graph.view.update restore"), GraphRestored)
				&& RestoredGraphLocation.Equals(FVector2f(12.0f, 24.0f), 0.01f)
				&& FMath::IsNearlyEqual(RestoredGraphZoom, 0.75f, 0.001f));
		const FUeAgentCommandResult GraphRestoredRead = GraphCall(
			TEXT("graph.view.read"), MakeShared<FJsonObject>());
		TestTrue(TEXT("independent public graph read observes restoration"),
			RequireOk(TEXT("graph.view.read restored"), GraphRestoredRead)
				&& GraphRestoredRead.GetPayloadJson().Contains(TEXT("12"))
				&& GraphRestoredRead.GetPayloadJson().Contains(TEXT("24"))
				&& GraphRestoredRead.GetPayloadJson().Contains(TEXT("0.75")));
	}
	GraphEditor.Reset();
	GraphPackage->SetDirtyFlag(false);
	FText UnloadError;
	TestTrue(TEXT("saved graph package unloads for independent readback"),
		UPackageTools::UnloadPackages({GraphPackage}, UnloadError, true));
	CollectGarbage(RF_NoFlags, true);
	UBlueprint* const ReloadedBlueprint =
		LoadObject<UBlueprint>(nullptr, *GraphBlueprintObjectPath);
	UEdGraph* const ReloadedGraph = ReloadedBlueprint
		&& !ReloadedBlueprint->UbergraphPages.IsEmpty()
		? ReloadedBlueprint->UbergraphPages[0]
		: nullptr;
	FVector2f PersistedLocation = FVector2f::ZeroVector;
	float PersistedZoom = 0.0f;
	bool bPersistedDocumentFound = false;
	if (ReloadedBlueprint && ReloadedGraph)
	{
		const FSoftObjectPath GraphPath(ReloadedGraph);
		for (const FEditedDocumentInfo& Info
			: ReloadedBlueprint->LastEditedDocuments)
		{
			if (Info.EditedObjectPath == GraphPath)
			{
				PersistedLocation = Info.SavedViewOffset;
				PersistedZoom = Info.SavedZoomAmount;
				bPersistedDocumentFound = true;
				break;
			}
		}
	}
	TestTrue(TEXT("unload/reload preserves pixel-snapped document state"),
		bPersistedDocumentFound
			&& PersistedLocation.Equals(FVector2f(320.0f, 180.0f), 0.01f)
			&& FMath::IsNearlyEqual(PersistedZoom, 1.25f, 0.001f));
	if (UPackage* ReloadedPackage = FindPackage(nullptr, *GraphPackageName))
	{
		ReloadedPackage->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({ReloadedPackage}, UnloadError, true);
		CollectGarbage(RF_NoFlags, true);
	}
	const FString GraphFilename = FPackageName::LongPackageNameToFilename(
		GraphPackageName, FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*GraphFilename))
	{
		IFileManager::Get().Delete(*GraphFilename, false, true);
	}
	TestFalse(TEXT("owned graph-view fixture is removed"),
		IFileManager::Get().FileExists(*GraphFilename));
	return !HasAnyErrors();
}
