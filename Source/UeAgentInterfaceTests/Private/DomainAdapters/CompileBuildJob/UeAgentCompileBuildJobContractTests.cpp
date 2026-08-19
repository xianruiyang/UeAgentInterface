#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The production header is intentionally absent in LS-002. LS-003 must force
// this translation unit to rebuild after adding it because a missing optional
// include cannot form a UBT dependency.
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobContract.h"
static_assert(
	UAI_COMPILE_BUILD_JOB_CONTRACT_V1 == 1,
	"CompileBuildJob production contract version changed");

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/CompileBuildJob/Fixtures/"
				"compile_build_job_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load CompileBuildJob contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse CompileBuildJob contract fixture"),
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
	FUeAgentCompileBuildJobContractTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.E0.Contract."
		"TypedFamiliesAndLegacyResponsibilities",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobContractTest::RunTest(
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
			"uai.domain_adapter.compile_build_job.test_fixture.v1")));
	TestEqual(
		TEXT("one central operation owner"),
		Fixture->GetStringField(TEXT("operation_owner")),
		FString(TEXT("C7/FLongRunningOperationOwner")));
	TestEqual(
		TEXT("physical resources remain C11-owned"),
		Fixture->GetStringField(TEXT("native_owner")),
		FString(TEXT("C11/operation_scoped_port")));
	TestEqual(
		TEXT("completion remains independent C9 readback"),
		Fixture->GetStringField(TEXT("verification_owner")),
		FString(TEXT("C9/independent_readback")));
	TestEqual(
		TEXT("typed signals reuse C4 canonical values"),
		Fixture->GetStringField(TEXT("signal_payload_contract")),
		FString(TEXT("C4/FUeAgentCanonicalValueRef")));

	const TArray<FString> CommonFields =
		ReadStrings(Fixture, TEXT("common_request_fields"));
	TestTrue(
		TEXT("all families share the same compact request envelope"),
		HasExactStrings(
			CommonFields,
			{TEXT("family_id"), TEXT("subject_ref"),
			 TEXT("policy_id"), TEXT("parameters")}));
	const TArray<FString> ForbiddenFields =
		ReadStrings(Fixture, TEXT("forbidden_request_fields"));
	for (const FString& Forbidden : {
		TEXT("command_id"), TEXT("executable"), TEXT("arguments"),
		TEXT("command_line"), TEXT("timeout_seconds"), TEXT("http_route"),
		TEXT("task_directory"), TEXT("process_handle")})
	{
		TestTrue(
			TEXT("domain request excludes owner/transport/native detail: ")
				+ Forbidden,
			ForbiddenFields.Contains(Forbidden));
	}

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("families"));
	TestEqual(
		TEXT("exact four reusable request families"),
		FamilyValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(TEXT("family_count"))));
	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_compile"),
		TEXT("world_build"),
		TEXT("external_process"),
		TEXT("world_partition_resave"),
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
		TestFalse(
			TEXT("family is not an old command id: ") + FamilyId,
			FamilyId.Contains(TEXT("packaging_"))
				|| FamilyId.Contains(TEXT("blueprint_")));
		TestTrue(
			TEXT("family has semantic subject: ") + FamilyId,
			!Family->GetStringField(TEXT("subject_kind")).IsEmpty());
		TestTrue(
			TEXT("family selects an internal policy, not executable args: ")
				+ FamilyId,
			Family->GetStringField(TEXT("policy_kind")).EndsWith(
				TEXT("_policy_ref")));
		TestTrue(
			TEXT("family has stable typed signal: ") + FamilyId,
			Family->GetStringField(TEXT("signal_type_id")).StartsWith(
				TEXT("uai.compile_build.")));
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
		TEXT("all P0 candidates plus SA-003 resave are adjudicated"),
		MappingValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));
	TSet<FString> LegacyIds;
	TMap<FString, TSharedPtr<FJsonObject>> MappingById;
	TMap<FString, int32> ResponsibilityCounts;
	TMap<FString, int32> StartFamilyCounts;
	for (const TSharedPtr<FJsonValue>& Value : MappingValues)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId = Mapping->GetStringField(TEXT("legacy_id"));
		const FString Responsibility =
			Mapping->GetStringField(TEXT("responsibility"));
		TestFalse(
			TEXT("legacy evidence id is unique: ") + LegacyId,
			LegacyIds.Contains(LegacyId));
		LegacyIds.Add(LegacyId);
		MappingById.Add(LegacyId, Mapping);
		++ResponsibilityCounts.FindOrAdd(Responsibility);
		if (Responsibility == TEXT("start"))
		{
			const FString FamilyId =
				Mapping->GetStringField(TEXT("family_id"));
			TestTrue(
				TEXT("start evidence maps to one real family: ") + LegacyId,
				FamilyIds.Contains(FamilyId));
			++StartFamilyCounts.FindOrAdd(FamilyId);
		}
		else
		{
			TestTrue(
				TEXT("non-start evidence has an explicit public owner: ")
					+ LegacyId,
				!Mapping->GetStringField(TEXT("owner")).IsEmpty());
			TestFalse(
				TEXT("non-start evidence cannot invent a job family: ")
					+ LegacyId,
				Mapping->HasField(TEXT("family_id")));
		}
	}
	TestEqual(TEXT("nineteen real starts"),
		ResponsibilityCounts.FindRef(TEXT("start")), 19);
	TestEqual(TEXT("seven log reads become typed observation"),
		ResponsibilityCounts.FindRef(TEXT("typed_observation")), 7);
	TestEqual(TEXT("one lifecycle status read"),
		ResponsibilityCounts.FindRef(TEXT("lifecycle_observation")), 1);
	TestEqual(TEXT("one C7 cancellation route"),
		ResponsibilityCounts.FindRef(TEXT("cancellation")), 1);
	TestEqual(TEXT("one C6 preflight route"),
		ResponsibilityCounts.FindRef(TEXT("preflight")), 1);
	TestEqual(TEXT("one scoped artifact cleanup route"),
		ResponsibilityCounts.FindRef(TEXT("artifact_cleanup")), 1);
	TestEqual(TEXT("one C9 verification route"),
		ResponsibilityCounts.FindRef(TEXT("verification")), 1);
	TestEqual(TEXT("nine asset compile starts"),
		StartFamilyCounts.FindRef(TEXT("asset_compile")), 9);
	TestEqual(TEXT("five persisted world build starts"),
		StartFamilyCounts.FindRef(TEXT("world_build")), 5);
	TestEqual(TEXT("PCG generation is not disguised as asset compile"),
		MappingById[TEXT("pcg_generate")]
			->GetStringField(TEXT("family_id")),
		FString(TEXT("world_build")));
	TestEqual(TEXT("PCG regeneration is not disguised as asset compile"),
		MappingById[TEXT("pcg_regenerate")]
			->GetStringField(TEXT("family_id")),
		FString(TEXT("world_build")));
	TestEqual(TEXT("four owned external process starts"),
		StartFamilyCounts.FindRef(TEXT("external_process")), 4);
	TestEqual(TEXT("one World Partition resave start"),
		StartFamilyCounts.FindRef(TEXT("world_partition_resave")), 1);
	TestEqual(
		TEXT("World Partition resave comes from SA-003"),
		MappingById[TEXT("world_partition_resave_actors")]
			->GetStringField(TEXT("source")),
		FString(TEXT("SA-003")));
	TestEqual(
		TEXT("packaging status reads C7 snapshot"),
		MappingById[TEXT("packaging_task_status")]
			->GetStringField(TEXT("owner")),
		FString(TEXT("C7_snapshot")));
	TestEqual(
		TEXT("HLOD validation remains C9 readback"),
		MappingById[TEXT("hlod_validate_build")]
			->GetStringField(TEXT("owner")),
		FString(TEXT("C9_independent_readback")));

	using namespace UE::UAI::DomainAdapters::CompileBuildJob;
	TestEqual(
		TEXT("production contract version"),
		UAI_COMPILE_BUILD_JOB_CONTRACT_V1,
		1);
	const auto& ProductionFamilies = GetCompileBuildJobFamilyContracts();
	TestEqual(
		TEXT("production exposes four family contracts"),
		ProductionFamilies.Num(),
		FamilyValues.Num());
	for (const FUeAgentCompileBuildJobFamilyContract& Contract
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
