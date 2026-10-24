#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationContract.h"
static_assert(
	UAI_EDITOR_SESSION_OPERATION_CONTRACT_V1 == 1,
	"EditorSessionOperation production contract version changed");

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/EditorSessionOperation/Fixtures/"
				"editor_session_operation_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load EditorSessionOperation contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse EditorSessionOperation contract fixture"),
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
	FUeAgentEditorSessionOperationContractTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.E0.Contract."
		"TypedFamiliesAndCurrentOperationBindings",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionOperationContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.editor_session_operation.test_fixture.v1")));
	TestEqual(
		TEXT("one central operation owner"),
		Fixture->GetStringField(TEXT("operation_owner")),
		FString(TEXT("C7/FLongRunningOperationOwner")));
	TestEqual(
		TEXT("physical handles remain operation-scoped"),
		Fixture->GetStringField(TEXT("native_owner")),
		FString(TEXT("C11/operation_scoped_port")));
	TestEqual(
		TEXT("business completion remains independent readback"),
		Fixture->GetStringField(TEXT("verification_owner")),
		FString(TEXT("C9/independent_readback")));
	TestEqual(
		TEXT("typed signals reuse canonical values"),
		Fixture->GetStringField(TEXT("signal_payload_contract")),
		FString(TEXT("C4/FUeAgentCanonicalValueRef")));
	TestEqual(
		TEXT("disconnect has no business state transition"),
		Fixture->GetStringField(TEXT("disconnect_semantics")),
		FString(TEXT("no_state_transition")));
	TestTrue(
		TEXT("all families share one compact typed envelope"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("common_request_fields")),
			{TEXT("family_id"), TEXT("subject_ref"),
			 TEXT("policy_id"), TEXT("parameters")}));
	TestTrue(
		TEXT("C7 request owns lease/session/expiry"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("operation_context_fields")),
			{TEXT("operation_id"), TEXT("owner_lease_id"),
			 TEXT("server_session_id"), TEXT("deadline")}));
	TestTrue(
		TEXT("continuation is generation and session bound"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("continuation_fields")),
			{TEXT("operation_id"), TEXT("owner_lease_id"),
			 TEXT("server_session_id"),
			 TEXT("expected_generation")}));
	TestTrue(
		TEXT("reattach returns current generation and signal revision"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("reattach_observation_fields")),
			{TEXT("generation"), TEXT("latest_signal.revision")}));

	const TArray<FString> Forbidden =
		ReadStrings(Fixture, TEXT("forbidden_request_fields"));
	for (const FString& Field : {
		TEXT("command_id"), TEXT("http_route"), TEXT("batch"),
		TEXT("phase_index"), TEXT("resume"),
		TEXT("timeout_seconds"), TEXT("process_handle"),
		TEXT("owner_lease_id"), TEXT("server_session_id"),
		TEXT("force_exit"), TEXT("use_current_level"),
		TEXT("use_active_tool"), TEXT("use_selection")})
	{
		TestTrue(
			TEXT("domain params exclude another owner or implicit context: ")
				+ Field,
			Forbidden.Contains(Field));
	}

	const TArray<FString> RequiredScenarios =
		ReadStrings(Fixture, TEXT("required_scenarios"));
	for (const FString& Scenario : {
		TEXT("accepted_then_run"),
		TEXT("same_session_reattach_with_revision"),
		TEXT("disconnect_without_outcome"),
		TEXT("pie_session"),
		TEXT("modeling_tool_session"),
		TEXT("current_level"),
		TEXT("generation_bound_health"),
		TEXT("cancel_or_expiry_waits_for_physical_exit"),
		TEXT("safe_exit_scheduled_is_not_closed")})
	{
		TestTrue(
			TEXT("required scenario is represented: ") + Scenario,
			RequiredScenarios.Contains(Scenario));
	}

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("families"));
	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_editor"),
		TEXT("runtime_preview"),
		TEXT("world_context"),
		TEXT("modeling_tool"),
		TEXT("host_lifecycle"),
	};
	TSet<FString> FamilyIds;
	TMap<FString, TSharedPtr<FJsonObject>> FamilyById;
	for (const TSharedPtr<FJsonValue>& Value : FamilyValues)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId = Family->GetStringField(TEXT("family_id"));
		TestFalse(
			TEXT("family id is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		FamilyById.Add(FamilyId, Family);
		TestTrue(
			TEXT("family has explicit semantic subject: ") + FamilyId,
			Family->GetStringField(TEXT("subject_kind")).EndsWith(
				TEXT("_ref")));
		TestTrue(
			TEXT("family has explicit internal policy: ") + FamilyId,
			Family->GetStringField(TEXT("policy_kind")).EndsWith(
				TEXT("_policy_ref")));
		TestTrue(
			TEXT("family has stable typed signal: ") + FamilyId,
			Family->GetStringField(TEXT("signal_type_id")).StartsWith(
				TEXT("uai.editor_session.")));
	}
	for (const FString& Expected : ExpectedFamilies)
	{
		TestTrue(
			TEXT("required family is present: ") + Expected,
			FamilyIds.Contains(Expected));
	}

	const TArray<TSharedPtr<FJsonValue>>& OperationCases =
		Fixture->GetArrayField(TEXT("session_operation_cases"));
	TSet<FString> CaseIds;
	TSet<FString> SessionFamilies;
	bool bHostLifecycleSafeExit = false;
	bool bWorldContextPolicy = false;
	for (const TSharedPtr<FJsonValue>& Value : OperationCases)
	{
		const TSharedPtr<FJsonObject> OperationCase = Value->AsObject();
		const FString CaseId =
			OperationCase->GetStringField(TEXT("case_id"));
		const FString FamilyId =
			OperationCase->GetStringField(TEXT("family_id"));
		const FString CommandId =
			OperationCase->GetStringField(TEXT("command_id"));
		TestFalse(TEXT("operation case id is unique: ") + CaseId,
			CaseIds.Contains(CaseId));
		CaseIds.Add(CaseId);
		TestTrue(TEXT("operation case uses a real family: ") + CaseId,
			FamilyIds.Contains(FamilyId));
		if (FamilyId == TEXT("host_lifecycle"))
		{
			bHostLifecycleSafeExit = CommandId == TEXT("editor.safe_exit");
		}
		else
		{
			TestEqual(TEXT("session family uses editor.session: ") + CaseId,
				CommandId, FString(TEXT("editor.session")));
			SessionFamilies.Add(FamilyId);
		}
		if (FamilyId == TEXT("world_context"))
		{
			bWorldContextPolicy =
				OperationCase->GetStringField(TEXT("policy_id"))
					== TEXT("policy:world-context:set-current/v1");
		}
	}
	for (const FString& FamilyId : {
		TEXT("asset_editor"), TEXT("runtime_preview"),
		TEXT("world_context"), TEXT("modeling_tool")})
	{
		TestTrue(TEXT("session family has a current binding: ") + FamilyId,
			SessionFamilies.Contains(FamilyId));
	}
	TestTrue(TEXT("host lifecycle uses editor.safe_exit"),
		bHostLifecycleSafeExit);
	TestTrue(TEXT("world context uses explicit set-current policy"),
		bWorldContextPolicy);

	const TSharedPtr<FJsonObject> OperationControl =
		Fixture->GetObjectField(TEXT("operation_control_contract"));
	TestEqual(TEXT("status control command"),
		OperationControl->GetStringField(TEXT("status_command_id")),
		FString(TEXT("operation.status")));
	TestEqual(TEXT("resume control command"),
		OperationControl->GetStringField(TEXT("resume_command_id")),
		FString(TEXT("operation.resume")));
	TestEqual(TEXT("cancel control command"),
		OperationControl->GetStringField(TEXT("cancel_command_id")),
		FString(TEXT("operation.cancel")));
	TestEqual(TEXT("plan control command"),
		OperationControl->GetStringField(TEXT("plan_command_id")),
		FString(TEXT("operation.plan.execute")));
	TestFalse(TEXT("Python never owns process close logic"),
		OperationControl->GetBoolField(TEXT("python_process_close_logic")));

	using namespace UE::UAI::DomainAdapters::EditorSessionOperation;
	TestEqual(
		TEXT("production contract version"),
		UAI_EDITOR_SESSION_OPERATION_CONTRACT_V1,
		1);
	const auto& ProductionFamilies =
		GetEditorSessionOperationFamilyContracts();
	for (const FUeAgentEditorSessionOperationFamilyContract& Contract
		: ProductionFamilies)
	{
		const TSharedPtr<FJsonObject>* FixtureFamily =
			FamilyById.Find(Contract.GetFamilyId());
		if (!TestTrue(
				TEXT("production family exists in fixture: ")
					+ Contract.GetFamilyId(),
				FixtureFamily != nullptr))
		{
			continue;
		}
		TestEqual(TEXT("production subject kind"),
			Contract.GetSubjectKind(),
			(*FixtureFamily)->GetStringField(TEXT("subject_kind")));
		TestEqual(TEXT("production policy kind"),
			Contract.GetPolicyKind(),
			(*FixtureFamily)->GetStringField(TEXT("policy_kind")));
		TestEqual(TEXT("production signal type"),
			Contract.GetSignalTypeId(),
			(*FixtureFamily)->GetStringField(TEXT("signal_type_id")));
	}

	return !HasAnyErrors();
}
