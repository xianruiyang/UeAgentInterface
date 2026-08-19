#include "Misc/AutomationTest.h"

#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"

#include <type_traits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineAnimationHjsonSingleResolutionContractTest,
	"UeAgentInterface.DomainAdapters.TimelineAnimation.E3."
		"HjsonSingleResolutionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineAnimationHjsonSingleResolutionContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::HjsonImport;
	using FWorkspaceAccessor =
		const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspacePtr& (
			FUeAgentHjsonTargetImportRequest::*)() const;
	static_assert(std::is_same_v<
		decltype(&FUeAgentHjsonTargetImportRequest::GetWorkspace),
		FWorkspaceAccessor>);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineAnimationHjsonExactRegistryTest,
	"UeAgentInterface.DomainAdapters.TimelineAnimation.E3.HjsonExactRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineAnimationHjsonExactRegistryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::HjsonImport;
	const FUeAgentHjsonImportTargetRegistry& Registry =
		GetHjsonImportTargetRegistry();
	int32 TimelineTargetCount = 0;
	for (const FUeAgentHjsonImportTargetBinding& Binding
		: Registry.GetBindings())
	{
		if (Binding.CompositionKind == TEXT("ue.animation_sequence.asset")
			|| Binding.CompositionKind == TEXT("ue.level_sequence.asset"))
		{
			++TimelineTargetCount;
		}
	}
	TestEqual(TEXT("Timeline exact target count"), TimelineTargetCount, 2);
	TestNotNull(
		TEXT("AnimationSequence target exists"),
		Registry.FindExact(TEXT("ue.animation_sequence.asset")));
	TestNotNull(
		TEXT("LevelSequence target exists"),
		Registry.FindExact(TEXT("ue.level_sequence.asset")));
	TestNull(
		TEXT("prefix is not a target selector"),
		Registry.FindExact(TEXT("ue.animation_sequence.asset.child")));
	TestNull(
		TEXT("empty kind is not inferred"),
		Registry.FindExact(TEXT("")));
	return !HasAnyErrors();
}
