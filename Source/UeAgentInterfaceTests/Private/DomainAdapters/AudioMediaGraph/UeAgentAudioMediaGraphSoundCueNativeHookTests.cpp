#include "Misc/AutomationTest.h"

// GC-011 production-header discovery is compiled into this test source.

#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"

#include "Sound/SoundCue.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAudioMediaGraphSoundCueNativeHookTest,
	"UeAgentInterface.DomainAdapters.AudioMediaGraph.E2.Native.SoundCue."
		"OwnedNodeRelationCompileAndIndependentReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAudioMediaGraphSoundCueNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;

	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/AudioMediaGraph/SoundCue/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned SoundCue package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);
	TStrongObjectPtr<USoundCue> Cue(NewObject<USoundCue>(
		Package, TEXT("SC_GC010"), RF_Transactional));
	if (!TestNotNull(TEXT("owned SoundCue exists"), Cue.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}

	FAudioMediaGraphSoundCueNativeHook Hook;
	FAudioMediaGraphSoundCueScenarioResult Result;
	FAudioMediaGraphError Error;
	TestTrue(TEXT("SoundCue mutates through native node API"),
		Hook.TryMutateCompileAndCaptureOwnedCue(*Cue, Result, Error));
	TestTrue(TEXT("SoundCue apply evidence exists"),
		Result.HasAppliedMutation());
	TestTrue(TEXT("SoundCue graph sync/compile settles"),
		Result.HasCompileBarrier() && Result.HasSettled());
	TestTrue(TEXT("SoundCue readback is independently captured"),
		Result.HasIndependentCapture());
	TestTrue(TEXT("SoundCue root is a captured native node"),
		Result.GetRootObjectPath().StartsWith(Cue->GetPathName()));
	TestTrue(TEXT("SoundCue captures at least two native nodes"),
		Result.GetNodeObjectPaths().Num() >= 2);
	TestTrue(TEXT("SoundCue captures ordered child relation"),
		Result.GetOrderedChildRelations().Num() >= 1);
	TestEqual(TEXT("SoundCue first child slot is native order zero"),
		Result.GetOrderedChildRelations()[0].GetOrder(), 0);
	TestFalse(TEXT("SoundCue node identity is not a display label"),
		Result.GetOrderedChildRelations()[0].GetParentObjectPath().IsEmpty());
	TestFalse(TEXT("apply evidence differs from compile evidence"),
		Result.GetApplyEvidenceId() == Result.GetBarrierEvidenceId());
	TestFalse(TEXT("compile evidence differs from readback evidence"),
		Result.GetBarrierEvidenceId() == Result.GetReadbackEvidenceId());
	TestFalse(TEXT("revision changes after mutation"),
		Result.GetBeforeRevision() == Result.GetReadbackRevision());

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
