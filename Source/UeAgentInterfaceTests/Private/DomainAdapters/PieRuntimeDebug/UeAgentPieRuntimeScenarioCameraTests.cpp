#include "Misc/AutomationTest.h"

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 8},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 4096},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 4 * 1024 * 1024},
		}));
	}

	FUeAgentCanonicalValueRef String(const FString& Text, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef Number(const FString& Text, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryDecimal(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef Integer(const FString& Text, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryInteger(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef Vector(
		const TArray<FString>& Values, FUeAgentResourceTracker& Resources)
	{
		TArray<FUeAgentCanonicalValueRef> Items;
		for (const FString& Value : Values) Items.Add(Number(Value, Resources));
		FUeAgentCanonicalValuePtr Result; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(Items, Resources, Result, Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalValueRef Transform(
		FUeAgentResourceTracker& Resources,
		const bool bLocation,
		const bool bRotation)
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields = {
			{TEXT("frame"), String(TEXT("ue_world"), Resources)},
			{TEXT("dimension"), Integer(TEXT("3"), Resources)},
			{TEXT("unit"), String(TEXT("centimeter"), Resources)},
			{TEXT("rotation_format"), String(TEXT("pitch_yaw_roll_degrees"), Resources)}};
		if (bLocation) Fields.Add({TEXT("location"), Vector({TEXT("1"), TEXT("2"), TEXT("3")}, Resources)});
		if (bRotation) Fields.Add({TEXT("rotation"), Vector({TEXT("10"), TEXT("20"), TEXT("30")}, Resources)});
		FUeAgentCanonicalValuePtr Result; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Result, Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalValueRef Capture(
		const FString& Source,
		FUeAgentResourceTracker& Resources,
		const bool bCameraRef = false,
		const FUeAgentCanonicalValueRef* ViewTransform = nullptr,
		const bool bFov = false,
		const FString& Fov = TEXT("90"))
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields = {
			{TEXT("type"), String(TEXT("capture"), Resources)},
			{TEXT("at_seconds"), Number(TEXT("0"), Resources)},
			{TEXT("source"), String(Source, Resources)},
			{TEXT("format"), String(TEXT("png"), Resources)}};
		if (bCameraRef) Fields.Add({TEXT("camera_ref"), String(TEXT("/Game/Maps/UEDPIE_0_Test.Test:PersistentLevel.CameraActor_0"), Resources)});
		if (ViewTransform) Fields.Add({TEXT("view_transform"), *ViewTransform});
		if (bFov) Fields.Add({TEXT("fov_degrees"), Number(Fov, Resources)});
		FUeAgentCanonicalValuePtr Result; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Result, Error));
		return Result.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPieRuntimeScenarioCameraTest,
	"UeAgentInterface.DomainAdapters.PieRuntimeDebug.CameraCaptureParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPieRuntimeScenarioCameraTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::PieRuntimeDebug;
	auto Resources = MakeResources();
	FUeAgentPieCaptureStepSpec Spec;
	FString Error;
	TestTrue(TEXT("output surface parses"), TryParsePieCaptureStepSpec(
		Capture(TEXT("pie_output_surface"), Resources), Spec, Error));
	TestEqual(TEXT("output source"), Spec.Source, EUeAgentPieCaptureSource::OutputSurface);
	TestTrue(TEXT("player camera parses"), TryParsePieCaptureStepSpec(
		Capture(TEXT("pie_player_camera"), Resources), Spec, Error));
	TestEqual(TEXT("player source"), Spec.Source, EUeAgentPieCaptureSource::PlayerCamera);
	TestFalse(TEXT("player forbids camera_ref"), TryParsePieCaptureStepSpec(
		Capture(TEXT("pie_player_camera"), Resources, true), Spec, Error));
	const FUeAgentCanonicalValueRef Location = Transform(Resources, true, false);
	TestTrue(TEXT("scene camera location override parses"), TryParsePieCaptureStepSpec(
		Capture(TEXT("scene_camera"), Resources, true, &Location), Spec, Error));
	TestTrue(TEXT("location selected"), Spec.OverrideLocation.IsSet());
	TestFalse(TEXT("scene camera requires camera_ref"), TryParsePieCaptureStepSpec(
		Capture(TEXT("scene_camera"), Resources), Spec, Error));
	TestFalse(TEXT("output surface forbids view transform"), TryParsePieCaptureStepSpec(
		Capture(TEXT("pie_output_surface"), Resources, false, &Location), Spec, Error));
	const FUeAgentCanonicalValueRef Empty = Transform(Resources, false, false);
	TestFalse(TEXT("view transform requires location or rotation"), TryParsePieCaptureStepSpec(
		Capture(TEXT("scene_camera"), Resources, true, &Empty), Spec, Error));
	const FUeAgentCanonicalValueRef Rotation = Transform(Resources, false, true);
	TestTrue(TEXT("scene camera rotation override parses"), TryParsePieCaptureStepSpec(
		Capture(TEXT("scene_camera"), Resources, true, &Rotation), Spec, Error));
	TestTrue(TEXT("rotation selected"), Spec.OverrideRotation.IsSet());
	const FUeAgentCanonicalValueRef Complete = Transform(Resources, true, true);
	TestTrue(TEXT("transient scene camera accepts complete pose and fov"),
		TryParsePieCaptureStepSpec(
			Capture(TEXT("scene_camera"), Resources, false, &Complete, true), Spec, Error));
	TestTrue(TEXT("transient fov selected"), Spec.OverrideFovDegrees.IsSet());
	TestFalse(TEXT("transient scene camera rejects missing fov"),
		TryParsePieCaptureStepSpec(
			Capture(TEXT("scene_camera"), Resources, false, &Complete), Spec, Error));
	TestFalse(TEXT("scene camera rejects out of range fov"),
		TryParsePieCaptureStepSpec(
			Capture(TEXT("scene_camera"), Resources, true, nullptr, true, TEXT("180")), Spec, Error));
	TestEqual(TEXT("player source identity is namespaced"),
		MakePieCameraSourceIdentity(EUeAgentPieCameraReadSource::PlayerCamera, TEXT("Manager_0")),
		FString(TEXT("pie-player-camera:Manager_0")));
	TestEqual(TEXT("scene source identity is namespaced"),
		MakePieCameraSourceIdentity(EUeAgentPieCameraReadSource::SceneCamera, TEXT("CameraActor_0")),
		FString(TEXT("scene-camera:CameraActor_0")));
	TestEqual(TEXT("transient source identity has one prefix"),
		MakePieTransientCameraSourceIdentity(TEXT("operation-7")),
		FString(TEXT("scene-camera-transient:operation-7")));
	TestEqual(TEXT("exact registry error survives terminal projection"),
		ResolvePieCaptureFailureCode(TEXT("pie_runtime_scene_camera_identity_mismatch"),
			TEXT("pie_runtime_capture_read_failed")),
		FString(TEXT("pie_runtime_scene_camera_identity_mismatch")));
	TestEqual(TEXT("missing registry error uses stable fallback"),
		ResolvePieCaptureFailureCode(FString(), TEXT("pie_runtime_capture_read_failed")),
		FString(TEXT("pie_runtime_capture_read_failed")));
	return !HasAnyErrors();
}
