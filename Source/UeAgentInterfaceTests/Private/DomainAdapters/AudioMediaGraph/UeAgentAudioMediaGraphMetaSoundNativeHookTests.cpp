#include "Misc/AutomationTest.h"

// GC-011 production-header discovery is compiled into this test source.

#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphMetaSoundNativeHook.h"

#include "MetasoundSource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAudioMediaGraphMetaSoundNativeHookTest,
	"UeAgentInterface.DomainAdapters.AudioMediaGraph.E2.Native.MetaSound."
		"OwnedFrontendMutationCompileAndIndependentReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAudioMediaGraphMetaSoundNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;

	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/AudioMediaGraph/MetaSound/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned MetaSound package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);
	TStrongObjectPtr<UMetaSoundSource> Source(NewObject<UMetaSoundSource>(
		Package, TEXT("MS_GC010"), RF_Transactional));
	if (!TestNotNull(TEXT("owned MetaSound source exists"), Source.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}

	FAudioMediaGraphMetaSoundNativeHook Hook;
	FAudioMediaGraphMetaSoundScenarioResult Result;
	FAudioMediaGraphError Error;
	const bool bMutated = Hook.TryMutateCompileAndCaptureOwnedSource(
		*Source, Result, Error);
	if (!bMutated)
	{
		AddError(TEXT("MetaSound native hook failed: ") + Error.GetCode());
	}
	TestTrue(TEXT("MetaSound mutates through native frontend API"), bMutated);
	TestTrue(TEXT("MetaSound apply evidence exists"),
		Result.HasAppliedMutation());
	TestTrue(TEXT("MetaSound frontend register/compile settles"),
		Result.HasCompileBarrier() && Result.HasSettled());
	TestTrue(TEXT("MetaSound readback is independently captured"),
		Result.HasIndependentCapture());
	TestTrue(TEXT("MetaSound native node GUID captured"),
		Result.GetNodeGuid().IsValid());
	TestTrue(TEXT("MetaSound native vertex GUID captured"),
		Result.GetVertexGuid().IsValid());
	TestFalse(TEXT("MetaSound pin direction is explicit"),
		Result.GetPinDirection().IsEmpty());
	TestFalse(TEXT("MetaSound pin type is explicit"),
		Result.GetPinType().IsNone());
	TestTrue(TEXT("MetaSound graph input is read back from frontend"),
		Result.GetGraphInputNames().Contains(TEXT("GC010_Gain")));
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
