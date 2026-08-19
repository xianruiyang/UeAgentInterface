#include "Misc/AutomationTest.h"

#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T39
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("editor_project.config_section.update"),
			TEXT("editor_project.device_profile.update"),
			TEXT("editor_project.localization_target.update"),
			TEXT("editor_project.platform_config.update"),
			TEXT("editor_project.project_settings.update"),
			TEXT("editor_project.scalability_group.update"),
			TEXT("editor_project.string_table.update"),
		};
		return Commands;
	}

	const TArray<TPair<FString, FString>>& HistoricalQueryRoutes()
	{
		static const TArray<TPair<FString, FString>> Routes = {
			{TEXT("asset_project.read.project_validation"), TEXT("device_profile.diff")},
			{TEXT("asset_project.read.config_section"), TEXT("device_profile.export")},
			{TEXT("asset_project.read.config_section"), TEXT("device_profile.query")},
			{TEXT("asset_project.read.config_resolved_value"), TEXT("device_profile.resolve_cvars")},
			{TEXT("asset_project.read.project_validation"), TEXT("device_profile.validate")},
			{TEXT("asset_project.read.project_platform"), TEXT("platform_config.export")},
			{TEXT("asset_project.read.project_validation"), TEXT("platform_config.validate")},
			{TEXT("asset_project.read.project_validation"), TEXT("platform_profile.validate_packaging")},
			{TEXT("asset_project.read.project_validation"), TEXT("project_settings.diff")},
			{TEXT("asset_project.read.project_settings"), TEXT("project_settings.index")},
			{TEXT("asset_project.read.project_settings"), TEXT("project_settings.page")},
			{TEXT("asset_project.read.project_validation"), TEXT("project_settings.validate")},
			{TEXT("asset_project.read.config_section"), TEXT("scalability.export")},
			{TEXT("asset_project.read.project_validation"), TEXT("scalability.validate")},
			{TEXT("asset_project.read.project_platform"), TEXT("target_platform.query")},
		};
		return Routes;
	}

	const TArray<FString>& HistoricalCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("config_apply_section"),
			TEXT("device_profile_apply_json"),
			TEXT("device_profile_diff_json"),
			TEXT("device_profile_export_json"),
			TEXT("device_profile_query"),
			TEXT("device_profile_resolve_cvars"),
			TEXT("device_profile_validate_json"),
			TEXT("platform_config_apply_json"),
			TEXT("platform_config_export_json"),
			TEXT("platform_config_validate_json"),
			TEXT("platform_profile_validate_packaging"),
			TEXT("project_settings_apply_page"),
			TEXT("project_settings_diff_page"),
			TEXT("project_settings_export_index"),
			TEXT("project_settings_export_page"),
			TEXT("project_settings_validate_page"),
			TEXT("scalability_apply_json"),
			TEXT("scalability_export_json"),
			TEXT("scalability_validate_json"),
			TEXT("target_platform_query"),
		};
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T39EditorProjectInventoryTest,
	"UAI.StrictV2.T39.EDITOR.PROJECT.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T39EditorProjectInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T39;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T39 owns exactly 7 inventory commands"),
		ExpectedCommands().Num(), 7);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T39 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("editor-project-operation-owner")));
		}
	}
	TestEqual(TEXT("T39 freezes exactly twenty historical identities"),
		HistoricalCommandIds().Num(), 20);
	for (const FString& HistoricalId : HistoricalCommandIds())
	{
		TestNull(
			*FString::Printf(TEXT("%s has no active exact or alias entry"),
				*HistoricalId),
			Registry->FindExact(HistoricalId));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T39HistoricalQueryRouteTest,
	"UAI.StrictV2.T39.EDITOR.PROJECT.OPERATION.HistoricalQueryRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T39HistoricalQueryRouteTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::EditorProject;
	using namespace UE::UAI::Tests::StrictV2::T39;
	TestEqual(TEXT("T39 owns fifteen closed historical query actions"),
		HistoricalQueryRoutes().Num(), 15);
	for (const TPair<FString, FString>& Route : HistoricalQueryRoutes())
	{
		TestTrue(
			*FString::Printf(TEXT("%s accepts %s"), *Route.Key, *Route.Value),
			IsEditorProjectQueryRoute(Route.Key, Route.Value));
	}
	for (const FString& Provider : TArray<FString>{
		TEXT("asset_project.read.config_section"),
		TEXT("asset_project.read.config_resolved_value"),
		TEXT("asset_project.read.project_settings"),
		TEXT("asset_project.read.project_platform"),
		TEXT("asset_project.read.project_validation")})
	{
		TestTrue(
			*FString::Printf(TEXT("%s supports target.read"), *Provider),
			IsEditorProjectQueryRoute(Provider, TEXT("target.read")));
	}
	TestFalse(
		TEXT("a capability action cannot enter a different Query provider"),
		IsEditorProjectQueryRoute(
			TEXT("asset_project.read.project_settings"),
			TEXT("target_platform.query")));
	TestFalse(
		TEXT("target.read cannot enter an unknown provider"),
		IsEditorProjectQueryRoute(
			TEXT("asset_project.read.unknown"), TEXT("target.read")));
	const TSharedRef<const IEditorProjectQueryMechanicsPort> Port =
		CreateEditorProjectQueryMechanicsPort();
	TestTrue(TEXT("the injected native query mechanics port exists"),
		&Port.Get() != nullptr);
	TSharedPtr<FJsonObject> Observation;
	FString Error;
	TestTrue(
		TEXT("default packaging validation returns a structured result without crashing"),
		Port->Execute(
			TEXT("platform_profile.validate_packaging"),
			{TEXT("t39-default-packaging"), MakeShared<FJsonObject>()},
			Observation,
			Error));
	TestTrue(TEXT("default packaging validation returns an observation"),
		Observation.IsValid());
	TestTrue(TEXT("default packaging validation reports no transport error"),
		Error.IsEmpty());
	return !HasAnyErrors();
}
