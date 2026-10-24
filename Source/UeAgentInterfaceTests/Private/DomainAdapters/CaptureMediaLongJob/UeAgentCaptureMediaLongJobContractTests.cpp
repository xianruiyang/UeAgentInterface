#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobContract.h"
static_assert(
	UAI_CAPTURE_MEDIA_LONG_JOB_CONTRACT_V1 == 1,
	"CaptureMediaLongJob production contract version changed");

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/CaptureMediaLongJob/Fixtures/"
				"capture_media_long_job_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load CaptureMediaLongJob contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse CaptureMediaLongJob contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<FString> ReadStrings(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| Values == nullptr)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	bool HasExactStrings(TArray<FString> Left, TArray<FString> Right)
	{
		Left.Sort();
		Right.Sort();
		return Left == Right;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaLongJobContractTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.E0.Contract."
		"FamiliesArtifactOwnershipAndRenderResourceActions",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaLongJobContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.capture_media_long_job.test_fixture.v1")));
	TestEqual(TEXT("one central operation owner"),
		Fixture->GetStringField(TEXT("operation_owner")),
		FString(TEXT("C7/FLongRunningOperationOwner")));
	TestEqual(TEXT("native handles remain operation scoped"),
		Fixture->GetStringField(TEXT("native_owner")),
		FString(TEXT("C11/operation_scoped_port")));
	TestEqual(TEXT("artifact delivery has a separate owner"),
		Fixture->GetStringField(TEXT("artifact_owner")),
		FString(TEXT("C8F/artifact_publisher")));
	TestEqual(TEXT("business verification remains independent"),
		Fixture->GetStringField(TEXT("verification_owner")),
		FString(TEXT("C9/independent_readback")));
	TestEqual(TEXT("delivery failure cannot rewrite operation outcome"),
		Fixture->GetStringField(TEXT("delivery_failure_semantics")),
		FString(TEXT("no_operation_outcome_rewrite")));
	TestEqual(TEXT("cleanup is exact-owned only"),
		Fixture->GetStringField(TEXT("cleanup_scope")),
		FString(TEXT("exact_owned_native_resource_only")));
	TestTrue(TEXT("all families share one compact request envelope"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("common_request_fields")),
			{TEXT("family_id"), TEXT("subject_ref"),
			 TEXT("policy_id"), TEXT("parameters")}));

	const TArray<FString> Forbidden =
		ReadStrings(Fixture, TEXT("forbidden_request_fields"));
	for (const FString& Field : {
		TEXT("command_id"), TEXT("http_route"),
		TEXT("process_handle"), TEXT("capture_handle"),
		TEXT("task_map"), TEXT("session_registry"),
		TEXT("owner_lease_id"), TEXT("server_session_id"),
		TEXT("raw_image_bytes"), TEXT("raw_audio_bytes"),
		TEXT("raw_video_bytes"), TEXT("base64_payload"),
		TEXT("delivery_status"), TEXT("business_outcome"),
		TEXT("use_current_viewport"), TEXT("use_selection")})
	{
		TestTrue(TEXT("request excludes another owner or raw output: ")
			+ Field, Forbidden.Contains(Field));
	}

	const TArray<FString> Required =
		ReadStrings(Fixture, TEXT("required_scenarios"));
	for (const FString& Scenario : {
		TEXT("accepted_then_run"),
		TEXT("progress_is_non_terminal"),
		TEXT("artifact_ready_is_non_terminal"),
		TEXT("delivery_failure_does_not_rewrite_receipt"),
		TEXT("wrong_session_and_stale_generation_do_no_native_work"),
		TEXT("cancel_waits_for_physical_exit"),
		TEXT("deadline_before_start_does_no_native_work"),
		TEXT("owned_temporary_resource_cleanup"),
		TEXT("non_owned_resource_survives_cleanup"),
		TEXT("deep_valid_parameters"),
		TEXT("deep_forbidden_field_rejected"),
		TEXT("render_target_clear_copy_bake_draw_use_render_resource_readback")})
	{
		TestTrue(TEXT("required scenario is represented: ") + Scenario,
			Required.Contains(Scenario));
	}

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("families"));
	TSet<FString> FamilyIds;
	TMap<FString, TSharedPtr<FJsonObject>> FamilyById;
	for (const TSharedPtr<FJsonValue>& Value : FamilyValues)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId = Family->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("family id unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		FamilyById.Add(FamilyId, Family);
		TestTrue(TEXT("explicit subject kind: ") + FamilyId,
			Family->GetStringField(TEXT("subject_kind")).EndsWith(
				TEXT("_ref")));
		TestTrue(TEXT("internal policy kind: ") + FamilyId,
			Family->GetStringField(TEXT("policy_kind")).EndsWith(
				TEXT("_policy_ref")));
		TestTrue(TEXT("typed progress signal: ") + FamilyId,
			Family->GetStringField(TEXT("signal_type_id")).StartsWith(
				TEXT("uai.capture.")));
	}
	TestTrue(TEXT("required families are present"),
		FamilyIds.Contains(TEXT("image_capture"))
			&& FamilyIds.Contains(TEXT("media_recording"))
			&& FamilyIds.Contains(TEXT("render_resource")));

	const TArray<TSharedPtr<FJsonValue>>& RenderOperationValues =
		Fixture->GetArrayField(TEXT("render_resource_operations"));
	TSet<FString> RenderActionIds;
	for (const TSharedPtr<FJsonValue>& Value : RenderOperationValues)
	{
		const TSharedPtr<FJsonObject> Operation = Value->AsObject();
		const FString ActionId =
			Operation->GetStringField(TEXT("action_id"));
		TestFalse(TEXT("render action id unique: ") + ActionId,
			RenderActionIds.Contains(ActionId));
		RenderActionIds.Add(ActionId);
		TestEqual(TEXT("render action uses render-resource family: ")
			+ ActionId,
			Operation->GetStringField(TEXT("family_id")),
			FString(TEXT("render_resource")));
		TestEqual(TEXT("render action matches family operation: ")
			+ ActionId,
			Operation->GetStringField(TEXT("target_operation")),
			FamilyById[TEXT("render_resource")]->GetStringField(
				TEXT("target_operation")));
		TestFalse(TEXT("render action has independent readback: ")
			+ ActionId,
			Operation->GetStringField(TEXT("readback")).IsEmpty());
	}
	for (const FString& Action : {
		TEXT("clear"), TEXT("copy"), TEXT("bake"), TEXT("draw")})
	{
		TestTrue(TEXT("required render-resource action is represented: ")
			+ Action, RenderActionIds.Contains(Action));
	}
	const TSharedPtr<FJsonObject> Transport =
		Fixture->GetObjectField(TEXT("transport_contract"));
	TestFalse(TEXT("Python owns no capture business logic"),
		Transport->GetBoolField(TEXT("python_business_logic")));
	TestFalse(TEXT("Adapter has no direct HTTP binding"),
		Transport->GetBoolField(TEXT("direct_http_binding")));

	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	TestEqual(TEXT("production contract version"),
		UAI_CAPTURE_MEDIA_LONG_JOB_CONTRACT_V1, 1);
	const auto& ProductionFamilies = GetCaptureMediaLongJobFamilyContracts();
	for (const FUeAgentCaptureMediaLongJobFamilyContract& Contract
		: ProductionFamilies)
	{
		const TSharedPtr<FJsonObject>* Family =
			FamilyById.Find(Contract.GetFamilyId());
		if (!TestTrue(TEXT("production family exists in fixture: ")
				+ Contract.GetFamilyId(), Family != nullptr))
		{
			continue;
		}
		TestEqual(TEXT("production subject kind"),
			Contract.GetSubjectKind(),
			(*Family)->GetStringField(TEXT("subject_kind")));
		TestEqual(TEXT("production policy kind"),
			Contract.GetPolicyKind(),
			(*Family)->GetStringField(TEXT("policy_kind")));
		TestEqual(TEXT("production signal type"),
			Contract.GetSignalTypeId(),
			(*Family)->GetStringField(TEXT("signal_type_id")));
	}
	return !HasAnyErrors();
}
