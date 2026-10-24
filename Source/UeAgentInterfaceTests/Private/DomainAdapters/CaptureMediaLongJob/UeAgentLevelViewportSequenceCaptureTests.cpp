#include "Misc/AutomationTest.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentLevelViewportSequenceCapture.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelViewportSequencePolicyContractTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Sequence.E0."
		"LevelViewportPolicyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelViewportSequencePolicyContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	const FUeAgentCaptureMediaImagePolicyDescriptor* const Policy =
		FindCaptureMediaImagePolicy(
			TEXT("policy:image-capture:viewport-sequence/v1"));
	if (!TestNotNull(TEXT("the viewport sequence policy is registered"), Policy))
	{
		return false;
	}
	TestTrue(TEXT("the viewport sequence discovery contract is complete"),
		Policy->IsValid() && Policy->HasDiscoveryContract());
	TestEqual(TEXT("the viewport sequence mechanics owner is stable"),
		Policy->GetMechanicsId(),
		FString(TEXT("mechanics:capture:level-viewport-sequence/v1")));
	const FString& Schema = Policy->GetParameterSchemaJson();
	TestTrue(TEXT("the subject is fixed to the active viewport"),
		Policy->GetSubjectSchemaJson().Contains(
			TEXT("capture-subject:viewport:active")));
	for (const TCHAR* Field : {
		TEXT("frame_count"), TEXT("interval_frames"),
		TEXT("format"), TEXT("quality"),
		TEXT("max_size"), TEXT("warmup_frames"),
		TEXT("frame_sheet_padding")})
	{
		TestTrue(FString::Printf(TEXT("schema contains %s"), Field),
			Schema.Contains(Field));
	}
	TestFalse(TEXT("automatic sheet layout exposes no column override"),
		Schema.Contains(TEXT("frame_sheet_columns")));
	for (const TCHAR* PolicyId : {
		TEXT("policy:image-capture:viewport/v1"),
		TEXT("policy:image-capture:viewport-sequence/v1"),
		TEXT("policy:image-capture:transform-candidates/v1")})
	{
		const FUeAgentCaptureMediaImagePolicyDescriptor* const PublicPolicy =
			FindCaptureMediaImagePolicy(PolicyId);
		if (!TestNotNull(
				TEXT("public capture policy is registered"), PublicPolicy))
		{
			continue;
		}
		const FString& PublicSchema = PublicPolicy->GetParameterSchemaJson();
		TestTrue(
			FString::Printf(TEXT("%s rejects undeclared parameters"), PolicyId),
			PublicSchema.Contains(TEXT("\"additionalProperties\":false")));
		for (const TCHAR* RetiredField : {
			TEXT("tick_editor_world"),
			TEXT("tick_editor_world_during_warmup"),
			TEXT("editor_world_tick_frames"),
			TEXT("editor_world_tick_delta_seconds")})
		{
			TestFalse(
				FString::Printf(
					TEXT("%s does not expose %s"), PolicyId, RetiredField),
				PublicSchema.Contains(RetiredField));
		}
	}
	return !HasAnyErrors();
}
