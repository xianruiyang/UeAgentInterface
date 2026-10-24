#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
		"FamiliesAndCurrentResponsibilityRoutes",
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
	for (const FString& Expected : ExpectedFamilies)
	{
		TestTrue(
			TEXT("required family is present: ") + Expected,
			FamilyIds.Contains(Expected));
	}

	const TArray<TSharedPtr<FJsonValue>>& ResponsibilityValues =
		Fixture->GetArrayField(TEXT("responsibility_routes"));
	TSet<FString> ResponsibilityCaseIds;
	TSet<FString> StartFamilies;
	TSet<FString> Responsibilities;
	for (const TSharedPtr<FJsonValue>& Value : ResponsibilityValues)
	{
		const TSharedPtr<FJsonObject> Route = Value->AsObject();
		const FString CaseId = Route->GetStringField(TEXT("case_id"));
		const FString Responsibility =
			Route->GetStringField(TEXT("responsibility"));
		TestFalse(TEXT("responsibility case id is unique: ") + CaseId,
			ResponsibilityCaseIds.Contains(CaseId));
		ResponsibilityCaseIds.Add(CaseId);
		Responsibilities.Add(Responsibility);
		if (Responsibility == TEXT("start"))
		{
			const FString FamilyId =
				Route->GetStringField(TEXT("family_id"));
			TestTrue(TEXT("start route uses a real family: ") + CaseId,
				FamilyIds.Contains(FamilyId));
			StartFamilies.Add(FamilyId);
		}
		else
		{
			TestFalse(TEXT("non-start route has an explicit owner: ")
				+ CaseId,
				Route->GetStringField(TEXT("owner")).IsEmpty());
			TestFalse(TEXT("non-start route does not claim a family: ")
				+ CaseId,
				Route->HasField(TEXT("family_id")));
		}
	}
	for (const FString& FamilyId : FamilyIds)
	{
		TestTrue(TEXT("current family has a start route: ") + FamilyId,
			StartFamilies.Contains(FamilyId));
	}
	for (const FString& Responsibility : {
		TEXT("start"), TEXT("typed_observation"),
		TEXT("lifecycle_observation"), TEXT("cancellation"),
		TEXT("preflight"), TEXT("artifact_cleanup"),
		TEXT("verification")})
	{
		TestTrue(TEXT("required responsibility is represented: ")
			+ Responsibility,
			Responsibilities.Contains(Responsibility));
	}

	TMap<FString, FString> PcgFamilyByCase;
	for (const TSharedPtr<FJsonValue>& Value :
		Fixture->GetArrayField(TEXT("pcg_operation_cases")))
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CaseId = Case->GetStringField(TEXT("case_id"));
		const FString FamilyId = Case->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("PCG operation case id is unique: ") + CaseId,
			PcgFamilyByCase.Contains(CaseId));
		TestTrue(TEXT("PCG operation uses a real family: ") + CaseId,
			FamilyIds.Contains(FamilyId));
		PcgFamilyByCase.Add(CaseId, FamilyId);
	}
	TestEqual(TEXT("PCG graph compilation is asset compilation"),
		PcgFamilyByCase.FindRef(TEXT("pcg_graph_asset_compile")),
		FString(TEXT("asset_compile")));
	TestEqual(TEXT("PCG world generation is a world build"),
		PcgFamilyByCase.FindRef(TEXT("pcg_world_generation")),
		FString(TEXT("world_build")));
	TestEqual(TEXT("PCG world regeneration is a world build"),
		PcgFamilyByCase.FindRef(TEXT("pcg_world_regeneration")),
		FString(TEXT("world_build")));

	using namespace UE::UAI::DomainAdapters::CompileBuildJob;
	TestEqual(
		TEXT("production contract version"),
		UAI_COMPILE_BUILD_JOB_CONTRACT_V1,
		1);
	const auto& ProductionFamilies = GetCompileBuildJobFamilyContracts();
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
