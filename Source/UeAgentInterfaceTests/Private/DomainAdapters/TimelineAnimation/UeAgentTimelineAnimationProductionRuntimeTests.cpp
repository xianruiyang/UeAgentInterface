#include "Misc/AutomationTest.h"

// TS-010A production runtime is intentionally discovered through a production
// header seam; keep this translation unit rebuilding when that seam is added.

#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineAnimationProductionRuntimeCompositionTest,
	"UeAgentInterface.DomainAdapters.TimelineAnimation.E2.ProductionRuntime.Composition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineAnimationProductionRuntimeCompositionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;

	FUeAgentTimelineAnimationProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("ts010a-project");
	Config.EditorInstanceId = TEXT("ts010a-editor");
	Config.ServerSessionId = TEXT("ts010a-session");
	Config.RegistryEpoch = 1;

	TSharedPtr<FUeAgentTimelineAnimationProductionRuntime> Runtime;
	FUeAgentTimelineAnimationExecutionError Error;
	const bool bCreated = TestTrue(
		TEXT("production runtime composes without implicit Editor state"),
		FUeAgentTimelineAnimationProductionRuntime::TryCreate(
			Config, Runtime, Error));
	if (!bCreated || !Runtime.IsValid())
	{
		AddError(
			TEXT("timeline_animation_production_runtime_unavailable:")
			+ Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	TSet<FString> CapabilityIds;
	for (const FUeAgentTimelineAnimationExecutionChainContract& Contract
		: GetTimelineAnimationExecutionChainContracts())
	{
		TestFalse(
			TEXT("production capability identity is unique"),
			CapabilityIds.Contains(Contract.GetCapabilityId()));
		CapabilityIds.Add(Contract.GetCapabilityId());
		if (Contract.GetC3Lane() == TEXT("c3q"))
		{
			TestTrue(
				TEXT("query has an exact production provider: ")
					+ Contract.GetCapabilityId(),
				Runtime->GetQueryProviderRegistry()->FindExact(
					Contract.GetBindingId()).IsValid());
		}
		else if (Contract.GetC3Lane() == TEXT("c3o"))
		{
			TestTrue(
				TEXT("operation has an exact production chain: ")
					+ Contract.GetCapabilityId(),
				Runtime->GetOperationChains().ContainsByPredicate(
					[&Contract](
						const TSharedRef<FUeAgentTimelineAnimationOperationChain>& Chain)
					{
						return Chain->GetContract().GetCapabilityId()
							== Contract.GetCapabilityId();
					}));
		}
		else
		{
			AddError(
				TEXT("timeline_animation_c3_lane_invalid:")
				+ Contract.GetCapabilityId() + TEXT(":")
				+ Contract.GetC3Lane());
		}
	}
	TArray<FString> SubjectIds;
	Runtime->GetExplicitSubjectIdentities(SubjectIds);
	TestTrue(
		TEXT("runtime never discovers an implicit current animation subject"),
		SubjectIds.IsEmpty());
	TArray<FString> RigCandidateIds;
	Runtime->GetExplicitRigCandidateIdentities(RigCandidateIds);
	TestTrue(
		TEXT("runtime never discovers an implicit current rig candidate"),
		RigCandidateIds.IsEmpty());
	return true;
}
