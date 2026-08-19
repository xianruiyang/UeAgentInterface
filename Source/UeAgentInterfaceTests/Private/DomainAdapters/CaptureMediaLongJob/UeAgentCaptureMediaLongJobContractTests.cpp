#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// LS-007 production marker: force a rebuild after the formerly missing
// optional contract header becomes available to UBT.
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
		"FamiliesArtifactOwnershipAndLegacyDisposition",
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
	TestEqual(TEXT("exact family count"), FamilyValues.Num(), 3);
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
	TestTrue(TEXT("family set is exact"),
		FamilyIds.Num() == 3
			&& FamilyIds.Contains(TEXT("image_capture"))
			&& FamilyIds.Contains(TEXT("media_recording"))
			&& FamilyIds.Contains(TEXT("render_resource")));

	const TArray<TSharedPtr<FJsonValue>>& MappingValues =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(TEXT("23 P0 plus 4 SA-004 candidates are adjudicated"),
		MappingValues.Num(), 27);
	TSet<FString> LegacyIds;
	TSet<FString> Sa004Ids;
	TMap<FString, TSharedPtr<FJsonObject>> MappingById;
	for (const TSharedPtr<FJsonValue>& Value : MappingValues)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId =
			Mapping->GetStringField(TEXT("legacy_id"));
		TestFalse(TEXT("legacy id unique: ") + LegacyId,
			LegacyIds.Contains(LegacyId));
		LegacyIds.Add(LegacyId);
		MappingById.Add(LegacyId, Mapping);
		TestEqual(TEXT("one Adapter owner: ") + LegacyId,
			Mapping->GetStringField(TEXT("owner")),
			FString(TEXT("D/CaptureMediaLongJob")));
		const FString FamilyId =
			Mapping->GetStringField(TEXT("family_id"));
		TestTrue(TEXT("mapping uses a real family: ") + LegacyId,
			FamilyIds.Contains(FamilyId));
		TestEqual(TEXT("mapping operation matches family: ") + LegacyId,
			Mapping->GetStringField(TEXT("target_operation")),
			FamilyById[FamilyId]->GetStringField(TEXT("target_operation")));
		if (Mapping->GetStringField(TEXT("source")) == TEXT("SA-004"))
		{
			Sa004Ids.Add(LegacyId);
			TestEqual(TEXT("SA-004 operation uses render family"),
				FamilyId, FString(TEXT("render_resource")));
		}
	}
	TestEqual(TEXT("legacy mapping ids are exact"), LegacyIds.Num(), 27);
	TestTrue(TEXT("SA-004 GPU operation set is exact"),
		Sa004Ids.Num() == 4
			&& Sa004Ids.Contains(TEXT("render_target_clear"))
			&& Sa004Ids.Contains(TEXT("render_target_copy_texture"))
			&& Sa004Ids.Contains(TEXT("render_target_create_static_texture"))
			&& Sa004Ids.Contains(TEXT("render_target_draw_material")));
	TestEqual(TEXT("RenderTarget clear maps to GPU action"),
		MappingById[TEXT("render_target_clear")]->GetStringField(
			TEXT("render_action")), FString(TEXT("clear")));
	TestEqual(TEXT("RenderTarget copy maps to GPU action"),
		MappingById[TEXT("render_target_copy_texture")]->GetStringField(
			TEXT("render_action")), FString(TEXT("copy")));
	TestEqual(TEXT("RenderTarget static texture maps to bake action"),
		MappingById[TEXT("render_target_create_static_texture")]
			->GetStringField(TEXT("render_action")), FString(TEXT("bake")));
	TestEqual(TEXT("RenderTarget draw maps to GPU action"),
		MappingById[TEXT("render_target_draw_material")]->GetStringField(
			TEXT("render_action")), FString(TEXT("draw")));

	const TSharedPtr<FJsonObject> Readback =
		Fixture->GetObjectField(TEXT("render_resource_readback"));
	for (const FString& Action : {
		TEXT("clear"), TEXT("copy"), TEXT("bake"), TEXT("draw")})
	{
		TestTrue(TEXT("GPU action has real render-resource readback: ")
			+ Action, !Readback->GetStringField(Action).IsEmpty());
	}
	const TSharedPtr<FJsonObject> Activation =
		Fixture->GetObjectField(TEXT("activation_contract"));
	TestEqual(TEXT("public activation remains LS-008"),
		Activation->GetStringField(TEXT("owner_task")),
		FString(TEXT("LS-008")));
	TestFalse(TEXT("LS-006 activates no public command"),
		Activation->GetBoolField(TEXT("public_commands_active_in_ls006")));
	TestFalse(TEXT("Python owns no capture business logic"),
		Activation->GetBoolField(TEXT("python_business_logic")));
	TestFalse(TEXT("Adapter has no direct HTTP binding"),
		Activation->GetBoolField(TEXT("direct_http_binding")));

	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	TestEqual(TEXT("production contract version"),
		UAI_CAPTURE_MEDIA_LONG_JOB_CONTRACT_V1, 1);
	const auto& ProductionFamilies = GetCaptureMediaLongJobFamilyContracts();
	TestEqual(TEXT("production family count matches fixture"),
		ProductionFamilies.Num(), FamilyValues.Num());
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
