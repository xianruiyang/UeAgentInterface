#include "Misc/AutomationTest.h"

#include "LifecycleHost/UeAgentLifecycleHost.h"

#if __has_include("LifecycleHost/UeAgentAutomationHostContract.h")
#define UAI_T33_AUTOMATION_HOST_CONTRACT_AVAILABLE 1
#include "LifecycleHost/UeAgentAutomationHostContract.h"
#else
#define UAI_T33_AUTOMATION_HOST_CONTRACT_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T33OwnedHostModesTest,
	"UAI.StrictV2.T33.EDITOR.AUTOMATION.OwnedHostModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T33OwnedHostModesTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T33_AUTOMATION_HOST_CONTRACT_AVAILABLE
	TestTrue(TEXT("T33 automation host contract is implemented"), false);
	return false;
#else
	using namespace UE::UAI::LifecycleHost;
	FUeAgentAutomationHostContract NullRhi;
	TestTrue(
		TEXT("non-rendering automation resolves to an owned NullRHI worker"),
		TryResolveAutomationHostContract(
			FUeAgentAutomationRequirement{},
			false,
			NullRhi));
	TestEqual(
		TEXT("non-rendering worker mode is exact"),
		NullRhi.ExecutorKind,
		EUeAgentAutomationExecutorKind::HeadlessNullRhi);
	TestTrue(TEXT("worker owns its process"), NullRhi.bOwnedProcessOnly);
	TestTrue(TEXT("worker owns an isolated UserDir"), NullRhi.bIsolatedUserDir);
	TestTrue(TEXT("worker owns isolated ports"), NullRhi.bIsolatedPorts);
	TestTrue(TEXT("main Editor is preserved"), NullRhi.bPreserveMainEditor);
	TestTrue(TEXT("NullRHI is explicit"), NullRhi.LaunchArguments.Contains(TEXT("-NullRHI")));

	FUeAgentAutomationRequirement Rendering;
	Rendering.bRequiresRhi = true;
	Rendering.MinimumFps = 10.0;
	FUeAgentAutomationHostContract Rhi;
	TestTrue(
		TEXT("rendering automation resolves to an owned RHI worker"),
		TryResolveAutomationHostContract(Rendering, false, Rhi));
	TestEqual(
		TEXT("rendering worker mode is exact"),
		Rhi.ExecutorKind,
		EUeAgentAutomationExecutorKind::HeadlessRhi);
	TestFalse(TEXT("rendering worker does not force NullRHI"), Rhi.LaunchArguments.Contains(TEXT("-NullRHI")));
	TestFalse(TEXT("contract never disables background throttling"), Rhi.bMayDisableBackgroundThrottling);
	TestFalse(TEXT("contract never forces foreground activation"), Rhi.bMayForceForeground);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T33FpsReceiptTest,
	"UAI.StrictV2.T33.EDITOR.AUTOMATION.FpsReceiptDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T33FpsReceiptTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T33_AUTOMATION_HOST_CONTRACT_AVAILABLE
	TestTrue(TEXT("T33 automation host contract is implemented"), false);
	return false;
#else
	using namespace UE::UAI::LifecycleHost;
	FUeAgentHostIdentity Worker{
		TEXT("worker:t33"),
		TEXT("session:t33"),
		EUeAgentHostKind::OwnedWorker,
		FUeAgentProcessIdentity{33, TEXT("2026-08-07T00:00:00Z"), TEXT("UnrealEditor-Cmd.exe")}};
	FUeAgentAutomationRequirement Requirement;
	Requirement.bRequiresRhi = true;
	Requirement.MinimumFps = 10.0;
	FUeAgentFpsGateDecision Decision;
	FUeAgentLifecycleDiagnostic Diagnostic;
	TestTrue(
		TEXT("observed FPS above the exact gate is ready"),
		EvaluateFpsGate(
			Worker,
			EUeAgentRenderMode::Rhi,
			Requirement,
			FUeAgentFpsObservation{10.0, 12.0},
			Decision,
			Diagnostic));
	TestEqual(TEXT("ready decision is receipt-safe"), Decision.State, EUeAgentFpsGateState::Ready);
	TestTrue(
		TEXT("low FPS is evaluated without hiding the blocked decision"),
		EvaluateFpsGate(
			Worker,
			EUeAgentRenderMode::Rhi,
			Requirement,
			FUeAgentFpsObservation{10.0, 5.0},
			Decision,
			Diagnostic));
	TestEqual(TEXT("low FPS is blocked"), Decision.State, EUeAgentFpsGateState::Blocked);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T33InteractiveBoundaryTest,
	"UAI.StrictV2.T33.EDITOR.AUTOMATION.InteractiveBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T33InteractiveBoundaryTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T33_AUTOMATION_HOST_CONTRACT_AVAILABLE
	TestTrue(TEXT("T33 automation host contract is implemented"), false);
	return false;
#else
	using namespace UE::UAI::LifecycleHost;
	FUeAgentAutomationRequirement Interactive;
	Interactive.bRequiresSlate = true;
	Interactive.bRequiresForeground = true;
	FUeAgentAutomationHostContract Contract;
	TestFalse(
		TEXT("foreground work is explicitly unsupported without an owned interactive runner"),
		TryResolveAutomationHostContract(Interactive, false, Contract));
	TestEqual(
		TEXT("unsupported boundary is typed"),
		Contract.FailureCode,
		FString(TEXT("interactive_requires_owned_interactive_runner")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T33MainEditorIsolationTest,
	"UAI.StrictV2.T33.EDITOR.AUTOMATION.MainEditorIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T33MainEditorIsolationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::LifecycleHost;
	const FUeAgentHostIdentity MainEditor{
		TEXT("main-editor:t33"),
		TEXT("session:t33"),
		EUeAgentHostKind::MainEditor,
		FUeAgentProcessIdentity{34, TEXT("2026-08-07T00:00:00Z"), TEXT("UnrealEditor.exe")}};
	FUeAgentLifecycleDiagnostic Diagnostic;
	TestFalse(
		TEXT("automation cannot target the user main Editor"),
		RequireOwnedAutomationTarget(MainEditor, Diagnostic));
	TestEqual(
		TEXT("main Editor rejection is typed"),
		Diagnostic.Code,
		FString(TEXT("automation_requires_owned_worker")));
	return true;
}
