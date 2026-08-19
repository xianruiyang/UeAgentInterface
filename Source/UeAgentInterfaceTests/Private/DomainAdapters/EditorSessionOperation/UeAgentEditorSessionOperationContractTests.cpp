#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// LS-004 intentionally landed before the production contract. This LS-005
// marker forces a rebuild because the formerly missing optional include could
// not form a UBT dependency.
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
		"TypedFamiliesOwnershipAndLegacyDisposition",
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
	TestEqual(
		TEXT("raw batch is retired rather than made a family"),
		Fixture->GetStringField(TEXT("batch_disposition")),
		FString(TEXT("retired_no_adapter_family")));

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
		TEXT("voxel_session"),
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
	TestEqual(
		TEXT("exact reusable family count"),
		FamilyValues.Num(),
		static_cast<int32>(
			Fixture->GetNumberField(TEXT("family_count"))));
	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_editor"),
		TEXT("runtime_preview"),
		TEXT("world_context"),
		TEXT("modeling_tool"),
		TEXT("voxel_grid"),
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
	bool bExactFamilies = FamilyIds.Num() == ExpectedFamilies.Num();
	for (const FString& Expected : ExpectedFamilies)
	{
		bExactFamilies = bExactFamilies && FamilyIds.Contains(Expected);
	}
	TestTrue(TEXT("family set is exact"), bExactFamilies);

	const TArray<TSharedPtr<FJsonValue>>& MappingValues =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(
		TEXT("all P0 and SA-003 candidates are adjudicated"),
		MappingValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));
	TSet<FString> LegacyIds;
	TMap<FString, TSharedPtr<FJsonObject>> MappingById;
	TSet<FString> ReallocatedIds;
	for (const TSharedPtr<FJsonValue>& Value : MappingValues)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId =
			Mapping->GetStringField(TEXT("legacy_id"));
		TestFalse(
			TEXT("legacy evidence id is unique: ") + LegacyId,
			LegacyIds.Contains(LegacyId));
		LegacyIds.Add(LegacyId);
		MappingById.Add(LegacyId, Mapping);
		TestTrue(
			TEXT("legacy candidate has explicit terminal disposition: ")
				+ LegacyId,
			!Mapping->GetStringField(TEXT("disposition")).IsEmpty());
		TestTrue(
			TEXT("legacy responsibility has one explicit owner: ")
				+ LegacyId,
			!Mapping->GetStringField(TEXT("owner")).IsEmpty());
		if (Mapping->HasField(TEXT("family_id")))
		{
			TestTrue(
				TEXT("mapped family is a real reusable family: ")
					+ LegacyId,
				FamilyIds.Contains(
					Mapping->GetStringField(TEXT("family_id"))));
		}
		if (Mapping->GetStringField(TEXT("source")) == TEXT("SA-003"))
		{
			ReallocatedIds.Add(LegacyId);
		}
	}
	const TSet<FString> ExpectedReallocatedIds = {
		TEXT("modeling_invoke_active_tool_action"),
		TEXT("modeling_set_active_tool_property"),
		TEXT("modeling_voxel_grid_begin"),
		TEXT("modeling_voxel_grid_add_bounds"),
		TEXT("modeling_voxel_grid_apply"),
		TEXT("editor.current_level.set"),
	};
	bool bExactReallocated =
		ReallocatedIds.Num() == ExpectedReallocatedIds.Num();
	for (const FString& Expected : ExpectedReallocatedIds)
	{
		bExactReallocated =
			bExactReallocated && ReallocatedIds.Contains(Expected);
	}
	TestTrue(
		TEXT("five modeling-session operations plus current-level are exact"),
		bExactReallocated);
	TestEqual(
		TEXT("raw exec_batch remains retired"),
		MappingById[TEXT("exec_batch")]->GetStringField(
			TEXT("disposition")),
		FString(TEXT("retire")));
	TestFalse(
		TEXT("raw exec_batch cannot invent an Adapter family"),
		MappingById[TEXT("exec_batch")]->HasField(TEXT("family_id")));
	TestEqual(
		TEXT("level_open is replaced by explicit current-level operation"),
		MappingById[TEXT("level_open")]->GetStringField(TEXT("owner")),
		FString(TEXT("editor.current_level.set")));
	TestEqual(
		TEXT("safe exit replaces editor_close compatibility surface"),
		MappingById[TEXT("editor_close")]->GetStringField(TEXT("owner")),
		FString(TEXT("LS-013_public_safe_exit")));

	const TSharedPtr<FJsonObject> Activation =
		Fixture->GetObjectField(TEXT("activation_contract"));
	TestEqual(
		TEXT("public activation is deferred to LS-013"),
		Activation->GetStringField(TEXT("owner_task")),
		FString(TEXT("LS-013")));
	TestEqual(
		TEXT("manifest remains inactive until LS-013"),
		Activation->GetStringField(TEXT("manifest_status")),
		FString(TEXT("planned_inactive_until_ls013")));
	TestEqual(
		TEXT("safe exit has one future public command"),
		Activation->GetStringField(TEXT("safe_exit_command_id")),
		FString(TEXT("editor.safe_exit")));
	TestFalse(
		TEXT("Python never owns process close logic"),
		Activation->GetBoolField(TEXT("python_process_close_logic")));
	TestTrue(
		TEXT("manifest and handler must share the C++ contract"),
		Activation->GetBoolField(
			TEXT("same_cpp_contract_handler_binding")));

	using namespace UE::UAI::DomainAdapters::EditorSessionOperation;
	TestEqual(
		TEXT("production contract version"),
		UAI_EDITOR_SESSION_OPERATION_CONTRACT_V1,
		1);
	const auto& ProductionFamilies =
		GetEditorSessionOperationFamilyContracts();
	TestEqual(
		TEXT("production exposes exactly the fixture families"),
		ProductionFamilies.Num(),
		FamilyValues.Num());
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
