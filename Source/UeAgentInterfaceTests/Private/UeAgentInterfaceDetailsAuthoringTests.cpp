#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "Components/PointLightComponent.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonObject> MakeField(const FString& PropertyPath, const FString& Category, const FString& ValueText)
	{
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("property_path"), PropertyPath);
		Field->SetStringField(TEXT("display_name"), PropertyPath);
		Field->SetArrayField(TEXT("category_path"), { MakeShared<FJsonValueString>(Category) });
		Field->SetStringField(TEXT("control_type"), TEXT("float"));
		Field->SetStringField(TEXT("value_kind"), TEXT("number"));
		Field->SetNumberField(TEXT("value"), FCString::Atod(*ValueText));
		Field->SetStringField(TEXT("value_text"), ValueText);
		Field->SetStringField(TEXT("tooltip"), TEXT("authoring tooltip"));
		TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
		Availability->SetBoolField(TEXT("editable"), true);
		Field->SetObjectField(TEXT("availability"), Availability);
		Field->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueString>(TEXT("set")) });
		Field->SetArrayField(TEXT("candidates"), {
			MakeShared<FJsonValueString>(TEXT("A")),
			MakeShared<FJsonValueString>(TEXT("B"))
		});
		return Field;
	}

	bool LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool SaveJsonObject(const FString& Path, const TSharedPtr<FJsonObject>& Object)
	{
		return FUeAgentStructuredJsonFormatter::SaveObjectToFile(Path, Object);
	}

	FString MakeAutomationRoot(const FString& Leaf)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Leaf);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return Root;
	}

	UeAgentDetailsAuthoring::FOwnerInfo MakeTestOwnerInfo()
	{
		UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
		OwnerInfo.OwnerId = TEXT("owner");
		OwnerInfo.OwnerKind = TEXT("component");
		OwnerInfo.OwnerName = TEXT("Owner");
		OwnerInfo.OwnerPath = TEXT("/Temp/Owner");
		OwnerInfo.OwnerClassPath = TEXT("/Script/Engine.PointLightComponent");
		OwnerInfo.SourceCommand = TEXT("details_authoring_test");
		return OwnerInfo;
	}

	TSharedPtr<FJsonObject> MakePropertiesJson(const TArray<TSharedPtr<FJsonObject>>& Fields)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TSharedPtr<FJsonObject>& Field : Fields)
		{
			Values.Add(MakeShared<FJsonValueObject>(Field));
		}

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
		Properties->SetStringField(TEXT("profile"), TEXT("authoring"));
		Properties->SetArrayField(TEXT("properties"), MoveTemp(Values));
		return Properties;
	}

	bool FindGroupedField(
		const FString& DetailsRoot,
		const FString& PropertyPath,
		FString& OutGroupFilePath,
		TSharedPtr<FJsonObject>& OutGroup,
		TSharedPtr<FJsonObject>& OutField)
	{
		TSharedPtr<FJsonObject> Index;
		if (!LoadJsonObject(DetailsRoot / TEXT("index.json"), Index))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GroupValue : *Groups)
		{
			const TSharedPtr<FJsonObject> GroupRef = GroupValue.IsValid() ? GroupValue->AsObject() : nullptr;
			if (!GroupRef.IsValid())
			{
				continue;
			}

			FString GroupFile;
			if (!GroupRef->TryGetStringField(TEXT("file"), GroupFile) || GroupFile.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> Group;
			const FString GroupPath = DetailsRoot / GroupFile;
			if (!LoadJsonObject(GroupPath, Group))
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (!Group->TryGetArrayField(TEXT("fields"), Fields) || !Fields)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
			{
				const TSharedPtr<FJsonObject> Field = FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
				if (Field.IsValid() && Field->GetStringField(TEXT("property_path")) == PropertyPath)
				{
					OutGroupFilePath = GroupPath;
					OutGroup = Group;
					OutField = Field;
					return true;
				}
			}
		}
		return false;
	}

	bool HasArrayObjectWithField(const TSharedPtr<FJsonObject>& Object, const FString& ArrayField, const FString& FieldName, const FString& FieldValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(ArrayField, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Entry.IsValid() && Entry->GetStringField(FieldName) == FieldValue)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDetailsAuthoringRoundTripTest,
	"UeAgentInterface.Json.DetailsAuthoring.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDetailsAuthoringRoundTripTest::RunTest(const FString& Parameters)
{
	const FString DetailsRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_DetailsAuthoringRoundTrip") / TEXT("details"));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(DetailsRoot), false, true);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
	Properties->SetStringField(TEXT("profile"), TEXT("authoring"));
	Properties->SetArrayField(TEXT("properties"), {
		MakeShared<FJsonValueObject>(MakeField(TEXT("Intensity"), TEXT("Light"), TEXT("7.5"))),
		MakeShared<FJsonValueObject>(MakeField(TEXT("CastShadows"), TEXT("Rendering"), TEXT("1"))),
		MakeShared<FJsonValueObject>(MakeField(TEXT("Mobility"), TEXT("Transform"), TEXT("2")))
	});

	UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
	OwnerInfo.OwnerId = TEXT("owner");
	OwnerInfo.OwnerKind = TEXT("actor");
	OwnerInfo.OwnerName = TEXT("Owner");
	OwnerInfo.OwnerPath = TEXT("/Game/Map.Owner");
	OwnerInfo.OwnerClassPath = TEXT("/Script/Engine.Actor");
	OwnerInfo.SourceCommand = TEXT("test");

	TSharedPtr<FJsonObject> DetailsSummary;
	FString Error;
	TestTrue(TEXT("write grouped details"), UeAgentDetailsAuthoring::WriteGroupedDetails(DetailsRoot, OwnerInfo, Properties, DetailsSummary, Error));
	TestTrue(TEXT("details summary valid"), DetailsSummary.IsValid());
	TestTrue(TEXT("details index written"), IFileManager::Get().FileExists(*(DetailsRoot / TEXT("index.json"))));
	TestEqual(TEXT("group count"), static_cast<int32>(DetailsSummary->GetNumberField(TEXT("group_count"))), 3);
	TestEqual(TEXT("field count"), static_cast<int32>(DetailsSummary->GetNumberField(TEXT("field_count"))), 3);

	TSharedPtr<FJsonObject> LightGroup;
	TestTrue(TEXT("light group readable"), LoadJsonObject(DetailsRoot / TEXT("Light.json"), LightGroup));
	if (LightGroup.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		TestTrue(TEXT("light group fields"), LightGroup->TryGetArrayField(TEXT("fields"), Fields) && Fields && Fields->Num() == 1);
		if (Fields && Fields->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Field = (*Fields)[0].IsValid() ? (*Fields)[0]->AsObject() : nullptr;
			TestTrue(TEXT("tooltip retained"), Field.IsValid() && Field->HasField(TEXT("tooltip")));
			TestTrue(TEXT("candidates retained"), Field.IsValid() && Field->HasField(TEXT("candidates")));
			TestTrue(TEXT("value_text retained for direct authoring"), Field.IsValid() && Field->HasField(TEXT("value_text")));
		}
	}

	TSharedPtr<FJsonObject> LoadedProperties;
	TestTrue(TEXT("load grouped details"), UeAgentDetailsAuthoring::LoadGroupedDetails(DetailsRoot, LoadedProperties, Error));
	TestTrue(TEXT("loaded properties valid"), LoadedProperties.IsValid());
	if (LoadedProperties.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* LoadedFields = nullptr;
		TestTrue(TEXT("loaded properties array"), LoadedProperties->TryGetArrayField(TEXT("properties"), LoadedFields) && LoadedFields);
		TestEqual(TEXT("loaded property count"), LoadedFields ? LoadedFields->Num() : 0, 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDetailsAuthoringInlineTreeTest,
	"UeAgentInterface.Json.DetailsAuthoring.InlineTreeExternalizeRehydrate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDetailsAuthoringInlineTreeTest::RunTest(const FString& Parameters)
{
	const FString EntryFilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_DetailsInlineTree") / TEXT("widget_tree.json"));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(EntryFilePath), false, true);

	TSharedPtr<FJsonObject> Widget = MakeShared<FJsonObject>();
	Widget->SetStringField(TEXT("id"), TEXT("SkyLightPanel"));
	Widget->SetStringField(TEXT("name"), TEXT("SkyLightPanel"));
	Widget->SetStringField(TEXT("class"), TEXT("/Script/UMG.Border"));
	Widget->SetArrayField(TEXT("properties"), {
		MakeShared<FJsonValueObject>(MakeField(TEXT("RenderOpacity"), TEXT("Render"), TEXT("0.75"))),
		MakeShared<FJsonValueObject>(MakeField(TEXT("ToolTipText"), TEXT("Behavior"), TEXT("1")))
	});
	Widget->SetArrayField(TEXT("settings_properties"), {
		MakeShared<FJsonValueObject>(MakeField(TEXT("Seed"), TEXT("Settings"), TEXT("42")))
	});

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("test.widget_tree"));
	Root->SetArrayField(TEXT("widgets"), { MakeShared<FJsonValueObject>(Widget) });

	FString Error;
	TestTrue(TEXT("externalize inline details tree"), UeAgentDetailsAuthoring::ExternalizeInlineDetailsInObjectTree(EntryFilePath, Root, TEXT("test_inline_tree"), Error));
	TestFalse(TEXT("widget properties removed"), Widget->HasField(TEXT("properties")));
	TestTrue(TEXT("widget details ref written"), Widget->HasField(TEXT("details")));
	TestFalse(TEXT("widget settings properties removed"), Widget->HasField(TEXT("settings_properties")));
	TestTrue(TEXT("widget settings details ref written"), Widget->HasField(TEXT("settings_details")));

	const TSharedPtr<FJsonObject>* Details = nullptr;
	TestTrue(TEXT("widget details object"), Widget->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid());
	if (Details && Details->IsValid())
	{
		FString IndexFile;
		TestTrue(TEXT("details index_file"), (*Details)->TryGetStringField(TEXT("index_file"), IndexFile) && !IndexFile.IsEmpty());
		TestTrue(TEXT("nested details index exists"), IFileManager::Get().FileExists(*(FPaths::GetPath(EntryFilePath) / IndexFile)));
	}

	TestTrue(TEXT("rehydrate inline details tree"), UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(EntryFilePath, Root, Error));
	const TArray<TSharedPtr<FJsonValue>>* RehydratedProperties = nullptr;
	TestTrue(TEXT("widget properties restored"), Widget->TryGetArrayField(TEXT("properties"), RehydratedProperties) && RehydratedProperties);
	TestEqual(TEXT("rehydrated property count"), RehydratedProperties ? RehydratedProperties->Num() : 0, 2);
	const TArray<TSharedPtr<FJsonValue>>* RehydratedSettingsProperties = nullptr;
	TestTrue(TEXT("widget settings properties restored"), Widget->TryGetArrayField(TEXT("settings_properties"), RehydratedSettingsProperties) && RehydratedSettingsProperties);
	TestEqual(TEXT("rehydrated settings property count"), RehydratedSettingsProperties ? RehydratedSettingsProperties->Num() : 0, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDetailsAuthoringOwnerApiRoundTripTest,
	"UeAgentInterface.Json.DetailsAuthoring.OwnerApiRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDetailsAuthoringOwnerApiRoundTripTest::RunTest(const FString& Parameters)
{
	const FString DetailsRoot = MakeAutomationRoot(TEXT("UAI_DetailsOwnerApiRoundTrip")) / TEXT("details");

	UPointLightComponent* Component = NewObject<UPointLightComponent>(GetTransientPackage());
	TestNotNull(TEXT("point light component created"), Component);
	if (!Component)
	{
		return false;
	}
	Component->SetIntensity(1234.0f);

	UeAgentDetailsAuthoring::FOwnerDetailsExportRequest ExportRequest =
		UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(Component, TEXT("component"), TEXT("point_light"), DetailsRoot, TEXT("test_export_folder"));
	TSharedPtr<FJsonObject> DetailsSummary;
	FString Error;
	TestTrue(TEXT("export owner details"), UeAgentDetailsAuthoring::ExportOwnerDetails(ExportRequest, DetailsSummary, Error));
	TestTrue(TEXT("details summary valid"), DetailsSummary.IsValid());
	if (DetailsSummary.IsValid())
	{
		TestEqual(TEXT("details ref schema"), DetailsSummary->GetStringField(TEXT("schema")), FString(TEXT("uai.details_ref.v1")));
		TestEqual(TEXT("details ref index file"), DetailsSummary->GetStringField(TEXT("index_file")), FString(TEXT("details/index.json")));
		TestEqual(TEXT("details ref field"), DetailsSummary->GetStringField(TEXT("ref_field")), FString(TEXT("details")));
		TestEqual(TEXT("owner kind"), DetailsSummary->GetStringField(TEXT("owner_kind")), FString(TEXT("component")));
		TestEqual(TEXT("owner id"), DetailsSummary->GetStringField(TEXT("owner_id")), FString(TEXT("point_light")));
		TestTrue(TEXT("details status present"), DetailsSummary->HasField(TEXT("status")));
		TestTrue(TEXT("group count present"), DetailsSummary->HasField(TEXT("group_count")));
		TestTrue(TEXT("field count present"), DetailsSummary->HasField(TEXT("field_count")));
		TestTrue(TEXT("editable field count present"), DetailsSummary->HasField(TEXT("editable_field_count")));
		TestTrue(TEXT("inlined field count present"), DetailsSummary->HasField(TEXT("field_details_inlined")));
	}

	TSharedPtr<FJsonObject> LoadedProperties;
	TestTrue(TEXT("grouped details readable"), UeAgentDetailsAuthoring::LoadGroupedDetails(DetailsRoot, LoadedProperties, Error));
	TestTrue(TEXT("loaded properties valid"), LoadedProperties.IsValid());

	FString GroupFilePath;
	TSharedPtr<FJsonObject> Group;
	TSharedPtr<FJsonObject> IntensityField;
	TestTrue(TEXT("intensity grouped field found"), FindGroupedField(DetailsRoot, TEXT("Intensity"), GroupFilePath, Group, IntensityField));
	if (IntensityField.IsValid())
	{
		IntensityField->SetNumberField(TEXT("value"), 7777.0);
		IntensityField->SetStringField(TEXT("value_text"), TEXT("7777.0"));
		TestTrue(TEXT("modified group saved"), SaveJsonObject(GroupFilePath, Group));
	}

	UeAgentDetailsAuthoring::FOwnerDetailsApplyRequest ApplyRequest;
	ApplyRequest.Object = Component;
	ApplyRequest.OwnerInfo = ExportRequest.OwnerInfo;
	ApplyRequest.DetailsRoot = DetailsRoot;
	TSharedPtr<FJsonObject> ApplySummary;
	TestTrue(TEXT("apply owner details"), UeAgentDetailsAuthoring::ApplyOwnerDetails(ApplyRequest, ApplySummary, Error));
	TestTrue(TEXT("apply summary valid"), ApplySummary.IsValid());
	if (ApplySummary.IsValid())
	{
		TestEqual(TEXT("apply schema"), ApplySummary->GetStringField(TEXT("schema")), FString(TEXT("uai.grouped_details_apply.v1")));
		TestTrue(TEXT("apply ok"), ApplySummary->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("applied at least one property"), ApplySummary->GetNumberField(TEXT("properties_applied")) >= 1.0);
	}
	TestEqual(TEXT("intensity applied from grouped details"), Component->Intensity, 7777.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDetailsAuthoringValidateDiffApiTest,
	"UeAgentInterface.Json.DetailsAuthoring.ValidateDiffApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDetailsAuthoringValidateDiffApiTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeAutomationRoot(TEXT("UAI_DetailsValidateDiffApi"));
	const FString OldDetailsRoot = Root / TEXT("old_details");
	const FString NewDetailsRoot = Root / TEXT("new_details");
	const UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo = MakeTestOwnerInfo();

	FString Error;
	TSharedPtr<FJsonObject> Summary;
	TestTrue(TEXT("write old details"), UeAgentDetailsAuthoring::WriteGroupedDetails(
		OldDetailsRoot,
		OwnerInfo,
		MakePropertiesJson({
			MakeField(TEXT("Intensity"), TEXT("Light"), TEXT("100")),
			MakeField(TEXT("CastShadows"), TEXT("Rendering"), TEXT("1"))
		}),
		Summary,
		Error));
	TestTrue(TEXT("write new details"), UeAgentDetailsAuthoring::WriteGroupedDetails(
		NewDetailsRoot,
		OwnerInfo,
		MakePropertiesJson({
			MakeField(TEXT("Intensity"), TEXT("Light"), TEXT("200")),
			MakeField(TEXT("LightColor"), TEXT("Light"), TEXT("3"))
		}),
		Summary,
		Error));

	UeAgentDetailsAuthoring::FOwnerDetailsValidateRequest ValidateRequest;
	ValidateRequest.OwnerInfo = OwnerInfo;
	ValidateRequest.DetailsRoot = NewDetailsRoot;
	TSharedPtr<FJsonObject> ValidateSummary;
	TestTrue(TEXT("validate grouped details"), UeAgentDetailsAuthoring::ValidateOwnerDetails(ValidateRequest, ValidateSummary, Error));
	TestTrue(TEXT("validate summary valid"), ValidateSummary.IsValid());
	if (ValidateSummary.IsValid())
	{
		TestEqual(TEXT("validate schema"), ValidateSummary->GetStringField(TEXT("schema")), FString(TEXT("uai.grouped_details_validate.v1")));
		TestTrue(TEXT("validate ok"), ValidateSummary->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("validate status"), ValidateSummary->GetStringField(TEXT("status")), FString(TEXT("passed")));
	}

	UeAgentDetailsAuthoring::FOwnerDetailsDiffRequest DiffRequest;
	DiffRequest.OwnerInfo = OwnerInfo;
	DiffRequest.OldDetailsRoot = OldDetailsRoot;
	DiffRequest.NewDetailsRoot = NewDetailsRoot;
	TSharedPtr<FJsonObject> DiffSummary;
	TestTrue(TEXT("diff grouped details"), UeAgentDetailsAuthoring::DiffOwnerDetails(DiffRequest, DiffSummary, Error));
	TestTrue(TEXT("diff summary valid"), DiffSummary.IsValid());
	if (DiffSummary.IsValid())
	{
		TestEqual(TEXT("diff schema"), DiffSummary->GetStringField(TEXT("schema")), FString(TEXT("uai.grouped_details_diff.v1")));
		TestTrue(TEXT("diff ok"), DiffSummary->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("field changed"), HasArrayObjectWithField(DiffSummary, TEXT("field_changes"), TEXT("change_type"), TEXT("changed")));
		TestTrue(TEXT("field added"), HasArrayObjectWithField(DiffSummary, TEXT("field_changes"), TEXT("change_type"), TEXT("added")));
		TestTrue(TEXT("field removed"), HasArrayObjectWithField(DiffSummary, TEXT("field_changes"), TEXT("change_type"), TEXT("removed")));
		TestTrue(TEXT("domain changes separated"), DiffSummary->HasField(TEXT("domain_changes")));
	}

	UeAgentDetailsAuthoring::FOwnerDetailsValidateRequest CoverageValidateRequest = ValidateRequest;
	CoverageValidateRequest.CoverageGaps.Add(TEXT("missing_validate_command"));
	TSharedPtr<FJsonObject> CoverageValidateSummary;
	TestTrue(TEXT("validate coverage gap returns summary"), UeAgentDetailsAuthoring::ValidateOwnerDetails(CoverageValidateRequest, CoverageValidateSummary, Error));
	TestEqual(TEXT("validate coverage gap status"), CoverageValidateSummary->GetStringField(TEXT("status")), FString(TEXT("coverage_gap")));
	TestFalse(TEXT("validate coverage gap not ok"), CoverageValidateSummary->GetBoolField(TEXT("ok")));

	UeAgentDetailsAuthoring::FOwnerDetailsDiffRequest CoverageDiffRequest = DiffRequest;
	CoverageDiffRequest.CoverageGaps.Add(TEXT("missing_diff_command"));
	TSharedPtr<FJsonObject> CoverageDiffSummary;
	TestTrue(TEXT("diff coverage gap returns summary"), UeAgentDetailsAuthoring::DiffOwnerDetails(CoverageDiffRequest, CoverageDiffSummary, Error));
	TestEqual(TEXT("diff coverage gap status"), CoverageDiffSummary->GetStringField(TEXT("status")), FString(TEXT("coverage_gap")));
	TestFalse(TEXT("diff coverage gap not ok"), CoverageDiffSummary->GetBoolField(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDetailsAuthoringLegacyDiagnosticsApiTest,
	"UeAgentInterface.Json.DetailsAuthoring.LegacyDiagnosticsApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDetailsAuthoringLegacyDiagnosticsApiTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeAutomationRoot(TEXT("UAI_DetailsLegacyDiagnosticsApi"));
	const UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo = MakeTestOwnerInfo();

	TSharedPtr<FJsonObject> LegacyInline = MakeShared<FJsonObject>();
	LegacyInline->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(MakeField(TEXT("Intensity"), TEXT("Light"), TEXT("100")) ) });
	TSharedPtr<FJsonObject> Rejection;
	TestTrue(TEXT("inline legacy properties rejected"), UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(
		LegacyInline,
		OwnerInfo,
		TEXT("details/index.json"),
		Rejection));
	TestTrue(TEXT("legacy rejection valid"), Rejection.IsValid());
	if (Rejection.IsValid())
	{
		TestEqual(TEXT("legacy rejection schema"), Rejection->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
		TestFalse(TEXT("legacy rejection ok false"), Rejection->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("legacy error code present"), Rejection->HasField(TEXT("error_code")));
		TestEqual(TEXT("legacy surface"), Rejection->GetStringField(TEXT("legacy_surface")), FString(TEXT("properties[]")));
		TestEqual(TEXT("legacy owner kind"), Rejection->GetStringField(TEXT("owner_kind")), OwnerInfo.OwnerKind);
		TestEqual(TEXT("legacy owner id"), Rejection->GetStringField(TEXT("owner_id")), OwnerInfo.OwnerId);
		TestTrue(TEXT("legacy json path present"), Rejection->HasField(TEXT("json_path")));
		TestTrue(TEXT("legacy migration hint present"), Rejection->HasField(TEXT("migration_hint")));
		TestEqual(TEXT("legacy allowed target"), Rejection->GetStringField(TEXT("allowed_target")), FString(TEXT("details/index.json")));
		TestFalse(TEXT("legacy not silently dropped"), Rejection->GetBoolField(TEXT("dropped")));
	}

	TSharedPtr<FJsonObject> IncludeRaw = MakeShared<FJsonObject>();
	IncludeRaw->SetBoolField(TEXT("include_raw_properties"), true);
	TestTrue(TEXT("include_raw_properties rejected"), UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(
		IncludeRaw,
		OwnerInfo,
		TEXT("details/index.json"),
		Rejection));
	TestEqual(TEXT("include_raw error code"), Rejection->GetStringField(TEXT("error_code")), FString(TEXT("include_raw_properties_command_param_only")));

	TSharedPtr<FJsonObject> RawSuffix = MakeShared<FJsonObject>();
	RawSuffix->SetObjectField(TEXT("movie_scene_raw_properties"), MakeShared<FJsonObject>());
	TestTrue(TEXT("raw properties suffix rejected"), UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(
		RawSuffix,
		OwnerInfo,
		TEXT("details/index.json"),
		Rejection));
	TestEqual(TEXT("raw suffix surface"), Rejection->GetStringField(TEXT("legacy_surface")), FString(TEXT("movie_scene_raw_properties")));

	TestTrue(TEXT("raw sidecar saved"), FFileHelper::SaveStringToFile(TEXT("{}"), *(Root / TEXT("raw_properties.json"))));
	TestTrue(TEXT("raw sidecar rejected"), UeAgentDetailsAuthoring::BuildLegacyPropertiesFileRejectionIfPresent(
		Root,
		OwnerInfo,
		TEXT("details/index.json"),
		Rejection));
	TestEqual(TEXT("raw sidecar surface"), Rejection->GetStringField(TEXT("legacy_surface")), FString(TEXT("raw_properties.json")));

	TSharedPtr<FJsonObject> DiagnosticEntry = MakeShared<FJsonObject>();
	DiagnosticEntry->SetBoolField(TEXT("include_raw_properties"), true);
	TSharedPtr<FJsonObject> RawProperties = MakeShared<FJsonObject>();
	RawProperties->SetBoolField(TEXT("apply"), true);
	DiagnosticEntry->SetObjectField(TEXT("raw_properties"), RawProperties);

	UeAgentDetailsAuthoring::FDiagnosticsIsolationRequest DiagnosticsRequest;
	DiagnosticsRequest.OwnerInfo = OwnerInfo;
	DiagnosticsRequest.FolderRoot = Root;
	DiagnosticsRequest.EntryObject = DiagnosticEntry;
	TSharedPtr<FJsonObject> DiagnosticsReport;
	FString Error;
	TestFalse(TEXT("diagnostics report rejects authoring misuse"), UeAgentDetailsAuthoring::BuildDiagnosticsIsolationReport(DiagnosticsRequest, DiagnosticsReport, Error));
	TestTrue(TEXT("diagnostics report valid"), DiagnosticsReport.IsValid());
	if (DiagnosticsReport.IsValid())
	{
		TestEqual(TEXT("diagnostics schema"), DiagnosticsReport->GetStringField(TEXT("schema")), FString(TEXT("uai.details_diagnostics_policy.v1")));
		TestFalse(TEXT("diagnostics not apply consumable"), DiagnosticsReport->GetBoolField(TEXT("apply_consumable")));
		TestFalse(TEXT("diagnostics not authoring input"), DiagnosticsReport->GetBoolField(TEXT("authoring_input_allowed")));
		TestTrue(TEXT("diagnostic write intent issue"), HasArrayObjectWithField(DiagnosticsReport, TEXT("issues"), TEXT("code"), TEXT("diagnostic_write_intent_not_authoring")));
		TestTrue(TEXT("include_raw policy issue"), HasArrayObjectWithField(DiagnosticsReport, TEXT("issues"), TEXT("code"), TEXT("include_raw_properties_command_param_only")));
		TestTrue(TEXT("raw sidecar policy issue"), HasArrayObjectWithField(DiagnosticsReport, TEXT("issues"), TEXT("path"), TEXT("raw_properties.json")));
	}
	return true;
}
