#if WITH_DEV_AUTOMATION_TESTS

#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	static TSharedPtr<FJsonObject> FindPropertyDescriptor(const TSharedPtr<FJsonObject>& Exported, const FString& PropertyPath)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Exported.IsValid() || !Exported->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Obj.IsValid() && Obj->GetStringField(TEXT("property_path")) == PropertyPath)
			{
				return Obj;
			}
		}
		return nullptr;
	}

	static FString GetValueKind(const TSharedPtr<FJsonObject>& Descriptor)
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Descriptor.IsValid() && Descriptor->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid())
		{
			return (*ValueObj)->GetStringField(TEXT("kind"));
		}
		return FString();
	}

	static TSharedPtr<FJsonObject> GetValueObject(const TSharedPtr<FJsonObject>& Descriptor)
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Descriptor.IsValid() && Descriptor->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid())
		{
			return *ValueObj;
		}
		return nullptr;
	}

	static bool HasEnumCandidate(const TSharedPtr<FJsonObject>& Descriptor, const FString& CandidateName)
	{
		const TSharedPtr<FJsonObject> ValueObj = GetValueObject(Descriptor);
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!ValueObj.IsValid() || !ValueObj->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CandidateValue : *Candidates)
		{
			const TSharedPtr<FJsonObject> Candidate = CandidateValue.IsValid() ? CandidateValue->AsObject() : nullptr;
			if (Candidate.IsValid() && Candidate->GetStringField(TEXT("name")) == CandidateName)
			{
				return true;
			}
		}
		return false;
	}

	static int32 GetPropertyCount(const TSharedPtr<FJsonObject>& Exported)
	{
		return Exported.IsValid() ? static_cast<int32>(Exported->GetNumberField(TEXT("property_count"))) : 0;
	}

	static UeAgentDetailsAuthoring::FOwnerInfo MakeLegacyRejectionTestOwner()
	{
		return UeAgentDetailsAuthoring::MakeOwnerInfoForObject(
			nullptr,
			TEXT("legacy_rejection_smoke"),
			TEXT("legacy_rejection_smoke_owner"),
			TEXT("uobject_properties_legacy_rejection_smoke"));
	}

	static TSharedPtr<FJsonObject> MakeObjectWithArrayField(const FString& FieldName)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueString>(TEXT("legacy")));
		Object->SetArrayField(FieldName, Values);
		return Object;
	}

	static bool HasIssueCode(const TSharedPtr<FJsonObject>& Report, const FString& ExpectedCode)
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			if (Issue.IsValid() && Issue->GetStringField(TEXT("code")) == ExpectedCode)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesLegacyRejectionTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.LegacyRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesLegacyRejectionTest::RunTest(const FString& Parameters)
{
	const UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo = MakeLegacyRejectionTestOwner();
	const FString AllowedTarget = TEXT("details/index.json");

	struct FInlineCase
	{
		const TCHAR* FieldName;
		const TCHAR* ErrorCode;
		const TCHAR* LegacySurface;
	};

	const FInlineCase InlineCases[] = {
		{ TEXT("properties"), TEXT("legacy_inline_properties_not_supported"), TEXT("properties[]") },
		{ TEXT("settings_properties"), TEXT("legacy_settings_properties_not_supported"), TEXT("settings_properties[]") },
		{ TEXT("generator_properties"), TEXT("legacy_inline_properties_not_supported"), TEXT("generator_properties[]") }
	};

	for (const FInlineCase& TestCase : InlineCases)
	{
		TSharedPtr<FJsonObject> Rejection;
		const bool bRejected = UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(
			MakeObjectWithArrayField(TestCase.FieldName),
			OwnerInfo,
			AllowedTarget,
			Rejection);
		TestTrue(FString::Printf(TEXT("%s rejected"), TestCase.FieldName), bRejected);
		TestTrue(FString::Printf(TEXT("%s rejection object valid"), TestCase.FieldName), Rejection.IsValid());
		if (Rejection.IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s rejection schema"), TestCase.FieldName), Rejection->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
			TestEqual(FString::Printf(TEXT("%s rejection error code"), TestCase.FieldName), Rejection->GetStringField(TEXT("error_code")), FString(TestCase.ErrorCode));
			TestEqual(FString::Printf(TEXT("%s rejection surface"), TestCase.FieldName), Rejection->GetStringField(TEXT("legacy_surface")), FString(TestCase.LegacySurface));
			TestFalse(FString::Printf(TEXT("%s rejection not dropped"), TestCase.FieldName), Rejection->GetBoolField(TEXT("dropped")));
			TestTrue(FString::Printf(TEXT("%s rejection has migration hint"), TestCase.FieldName), !Rejection->GetStringField(TEXT("migration_hint")).IsEmpty());
		}
	}

	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UeAgentInterfaceTests"),
		TEXT("LegacyRejection"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	TestTrue(TEXT("legacy rejection temp dir created"), IFileManager::Get().MakeDirectory(*Root, true));

	struct FFileCase
	{
		const TCHAR* FileName;
		const TCHAR* ErrorCode;
		const TCHAR* LegacySurface;
	};

	const FFileCase FileCases[] = {
		{ TEXT("properties.json"), TEXT("legacy_properties_file_not_supported"), TEXT("properties.json") },
		{ TEXT("raw_properties.json"), TEXT("legacy_raw_properties_not_supported"), TEXT("raw_properties.json") },
		{ TEXT("readonly_properties.json"), TEXT("readonly_properties_not_authoring"), TEXT("readonly_properties.json") }
	};

	for (const FFileCase& TestCase : FileCases)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		TestTrue(FString::Printf(TEXT("%s temp dir recreated"), TestCase.FileName), IFileManager::Get().MakeDirectory(*Root, true));
		const FString FilePath = FPaths::Combine(Root, TestCase.FileName);
		TestTrue(FString::Printf(TEXT("%s written"), TestCase.FileName), FFileHelper::SaveStringToFile(TEXT("{}"), *FilePath));

		TSharedPtr<FJsonObject> Rejection;
		FString Error;
		const bool bRejected = UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
			Root,
			OwnerInfo,
			AllowedTarget,
			Rejection,
			Error);
		TestTrue(FString::Printf(TEXT("%s rejected"), TestCase.FileName), bRejected);
		TestTrue(FString::Printf(TEXT("%s rejection object valid"), TestCase.FileName), Rejection.IsValid());
		if (Rejection.IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s rejection schema"), TestCase.FileName), Rejection->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
			TestEqual(FString::Printf(TEXT("%s rejection error code"), TestCase.FileName), Rejection->GetStringField(TEXT("error_code")), FString(TestCase.ErrorCode));
			TestEqual(FString::Printf(TEXT("%s rejection surface"), TestCase.FileName), Rejection->GetStringField(TEXT("legacy_surface")), FString(TestCase.LegacySurface));
			TestEqual(FString::Printf(TEXT("%s out error"), TestCase.FileName), Error, FString(TestCase.ErrorCode));
			TestFalse(FString::Printf(TEXT("%s rejection not dropped"), TestCase.FileName), Rejection->GetBoolField(TEXT("dropped")));
			TestTrue(FString::Printf(TEXT("%s rejection has migration hint"), TestCase.FileName), !Rejection->GetStringField(TEXT("migration_hint")).IsEmpty());
		}
	}

	TSharedPtr<FJsonObject> DiagnosticsObject = MakeShared<FJsonObject>();
	DiagnosticsObject->SetArrayField(TEXT("raw_properties"), {});
	DiagnosticsObject->SetArrayField(TEXT("readonly_properties"), {});
	DiagnosticsObject->SetArrayField(TEXT("missing_properties"), {});
	DiagnosticsObject->SetArrayField(TEXT("unsupported_properties"), {});

	UeAgentDetailsAuthoring::FDiagnosticsIsolationRequest DiagnosticsRequest;
	DiagnosticsRequest.OwnerInfo = OwnerInfo;
	DiagnosticsRequest.AllowedTarget = AllowedTarget;
	DiagnosticsRequest.EntryObject = DiagnosticsObject;

	TSharedPtr<FJsonObject> DiagnosticsReport;
	FString DiagnosticsError;
	const bool bDiagnosticsOk = UeAgentDetailsAuthoring::BuildDiagnosticsIsolationReport(DiagnosticsRequest, DiagnosticsReport, DiagnosticsError);
	TestFalse(TEXT("diagnostic properties are not authoring"), bDiagnosticsOk);
	TestTrue(TEXT("diagnostics report valid"), DiagnosticsReport.IsValid());
	if (DiagnosticsReport.IsValid())
	{
		TestEqual(TEXT("diagnostics report schema"), DiagnosticsReport->GetStringField(TEXT("schema")), FString(TEXT("uai.details_diagnostics_policy.v1")));
		TestTrue(TEXT("diagnostics raw_properties rejected"), HasIssueCode(DiagnosticsReport, TEXT("legacy_raw_properties_not_supported")));
		TestTrue(TEXT("diagnostics readonly_properties rejected"), HasIssueCode(DiagnosticsReport, TEXT("readonly_properties_not_authoring")));
		TestTrue(TEXT("diagnostics missing/unsupported rejected"), HasIssueCode(DiagnosticsReport, TEXT("diagnostic_properties_not_authoring")));
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesEnumerationCoreTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.EnumerationCore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesEnumerationCoreTest::RunTest(const FString& Parameters)
{
	UPointLightComponent* Component = NewObject<UPointLightComponent>(GetTransientPackage());
	TestNotNull(TEXT("component created"), Component);
	if (!Component)
	{
		return false;
	}

	Component->SetIntensity(54321.0f);

	UeAgentUObjectProperties::FExportOptions Options;
	TSharedPtr<FJsonObject> Exported = UeAgentUObjectProperties::ExportObjectProperties(Component, Options);
	TestTrue(TEXT("export object valid"), Exported.IsValid());
	TestEqual(TEXT("schema"), Exported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));

	const TSharedPtr<FJsonObject>* Identity = nullptr;
	TestTrue(TEXT("object identity exists"), Exported->TryGetObjectField(TEXT("object"), Identity) && Identity && Identity->IsValid());
	if (Identity && Identity->IsValid())
	{
		TestTrue(TEXT("class path contains PointLightComponent"), (*Identity)->GetStringField(TEXT("class_path")).Contains(TEXT("PointLightComponent")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties array exists"), Exported->TryGetArrayField(TEXT("properties"), Properties) && Properties);

	bool bFoundIntensity = false;
	if (Properties)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid() || Obj->GetStringField(TEXT("property_path")) != TEXT("Intensity"))
			{
				continue;
			}

			bFoundIntensity = true;
			TestTrue(TEXT("Intensity has value"), Obj->HasField(TEXT("value")));
			TestTrue(TEXT("Intensity has value_text"), Obj->HasField(TEXT("value_text")));
			TestTrue(TEXT("Intensity keeps tooltip"), Obj->HasField(TEXT("tooltip")));
			TestFalse(TEXT("authoring omits legacy property_name"), Obj->HasField(TEXT("property_name")));
			TestFalse(TEXT("authoring omits owner class"), Obj->HasField(TEXT("owner_class_path")));
			TestFalse(TEXT("authoring omits CDO comparison"), Obj->HasField(TEXT("has_cdo_default")));
			TestFalse(TEXT("authoring omits cpp type"), Obj->HasField(TEXT("cpp_type")));
			break;
		}
	}

	TestTrue(TEXT("Intensity property found"), bFoundIntensity);
	TestTrue(TEXT("transient/deprecated skipped are tracked or empty"), Exported->HasField(TEXT("skipped_count")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesValueSerializationTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.ValueSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesValueSerializationTest::RunTest(const FString& Parameters)
{
	UPointLightComponent* LightComponent = NewObject<UPointLightComponent>(GetTransientPackage());
	TestNotNull(TEXT("light component created"), LightComponent);
	if (!LightComponent)
	{
		return false;
	}

	LightComponent->SetIntensity(1234.0f);
	LightComponent->SetLightColor(FLinearColor::Red);
	LightComponent->ComponentTags.Add(TEXT("UAI_P2_002_Tag"));
	LightComponent->SetMobility(EComponentMobility::Static);

	UeAgentUObjectProperties::FExportOptions Options;
	TSharedPtr<FJsonObject> LightExported = UeAgentUObjectProperties::ExportObjectProperties(LightComponent, Options);
	TestTrue(TEXT("light export valid"), LightExported.IsValid());

	TSharedPtr<FJsonObject> Intensity = FindPropertyDescriptor(LightExported, TEXT("Intensity"));
	TestTrue(TEXT("Intensity descriptor exists"), Intensity.IsValid());
	if (Intensity.IsValid())
	{
		TestTrue(TEXT("Intensity has value"), Intensity->HasField(TEXT("value")));
		TestTrue(TEXT("Intensity has value_text"), Intensity->HasField(TEXT("value_text")));
		TestTrue(TEXT("Intensity keeps tooltip"), Intensity->HasField(TEXT("tooltip")));
		TestFalse(TEXT("authoring omits default_value"), Intensity->HasField(TEXT("default_value")));
		TestFalse(TEXT("authoring omits differs_from_cdo"), Intensity->HasField(TEXT("differs_from_cdo")));
	}

	TSharedPtr<FJsonObject> Mobility = FindPropertyDescriptor(LightExported, TEXT("Mobility"));
	TestTrue(TEXT("Mobility enum descriptor exists"), Mobility.IsValid());
	if (Mobility.IsValid())
	{
		TestEqual(TEXT("Mobility value kind"), GetValueKind(Mobility), FString(TEXT("enum")));
		TestTrue(TEXT("Mobility enum candidates include Static"), HasEnumCandidate(Mobility, TEXT("Static")));
		TestTrue(TEXT("Mobility enum candidates include Movable"), HasEnumCandidate(Mobility, TEXT("Movable")));
	}

	TSharedPtr<FJsonObject> LightColor = FindPropertyDescriptor(LightExported, TEXT("LightColor"));
	TestTrue(TEXT("LightColor struct descriptor exists"), LightColor.IsValid());
	if (LightColor.IsValid())
	{
		TestEqual(TEXT("LightColor value kind"), GetValueKind(LightColor), FString(TEXT("struct")));
	}

	TSharedPtr<FJsonObject> ComponentTags = FindPropertyDescriptor(LightExported, TEXT("ComponentTags"));
	TestTrue(TEXT("ComponentTags array descriptor exists"), ComponentTags.IsValid());
	if (ComponentTags.IsValid())
	{
		TestEqual(TEXT("ComponentTags value kind"), GetValueKind(ComponentTags), FString(TEXT("array")));
	}

	UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
	TestNotNull(TEXT("mesh component created"), MeshComponent);
	if (!MeshComponent)
	{
		return false;
	}
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestNotNull(TEXT("cube mesh loaded"), CubeMesh);
	MeshComponent->SetStaticMesh(CubeMesh);

	TSharedPtr<FJsonObject> MeshExported = UeAgentUObjectProperties::ExportObjectProperties(MeshComponent, Options);
	TSharedPtr<FJsonObject> StaticMesh = FindPropertyDescriptor(MeshExported, TEXT("StaticMesh"));
	TestTrue(TEXT("StaticMesh object reference descriptor exists"), StaticMesh.IsValid());
	if (StaticMesh.IsValid())
	{
		TestEqual(TEXT("StaticMesh value kind"), GetValueKind(StaticMesh), FString(TEXT("object_reference")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesLegacyApplyAliasTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.LegacyApplyAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesLegacyApplyAliasTest::RunTest(const FString& Parameters)
{
	UPointLightComponent* LightComponent = NewObject<UPointLightComponent>(GetTransientPackage());
	TestNotNull(TEXT("light component created"), LightComponent);
	if (!LightComponent)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Properties;
	TSharedPtr<FJsonObject> LegacyDescriptor = MakeShared<FJsonObject>();
	LegacyDescriptor->SetStringField(TEXT("property_name"), TEXT("Intensity"));
	LegacyDescriptor->SetStringField(TEXT("value_text"), TEXT("4321.0"));
	Properties.Add(MakeShared<FJsonValueObject>(LegacyDescriptor));
	Payload->SetArrayField(TEXT("properties"), Properties);

	UeAgentUObjectProperties::FApplyOptions Options;
	TSharedPtr<FJsonObject> Summary = UeAgentUObjectProperties::ApplyObjectProperties(LightComponent, Payload, Options);
	TestTrue(TEXT("apply summary valid"), Summary.IsValid());
	if (Summary.IsValid())
	{
		TestEqual(TEXT("legacy alias apply status"), Summary->GetStringField(TEXT("status")), FString(TEXT("applied")));
		TestEqual(TEXT("legacy alias applied count"), Summary->GetIntegerField(TEXT("properties_applied")), 1);
	}
	TestEqual(TEXT("legacy alias applied intensity"), LightComponent->Intensity, 4321.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesActorComponentBridgeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.ActorComponentBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesActorComponentBridgeTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage(), AActor::StaticClass());
	TestNotNull(TEXT("actor created"), Actor);
	if (!Actor)
	{
		return false;
	}

	UPointLightComponent* LightComponent = NewObject<UPointLightComponent>(Actor, TEXT("UAI_TestPointLightComponent"));
	TestNotNull(TEXT("component created"), LightComponent);
	if (!LightComponent)
	{
		return false;
	}
	LightComponent->SetIntensity(2468.0f);
	Actor->AddInstanceComponent(LightComponent);

	UeAgentUObjectProperties::FExportOptions Options;
	Options.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;

	TSharedPtr<FJsonObject> ActorProperties = UeAgentUObjectProperties::ExportActorProperties(Actor, Options);
	TestTrue(TEXT("actor properties valid"), ActorProperties.IsValid());
	if (ActorProperties.IsValid())
	{
		TestEqual(TEXT("actor schema"), ActorProperties->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
		TestEqual(TEXT("actor profile"), ActorProperties->GetStringField(TEXT("profile")), FString(TEXT("authoring")));
		TestFalse(TEXT("actor export has no actor-only schema"), ActorProperties->HasField(TEXT("actor_schema")));
	}

	TSharedPtr<FJsonObject> ComponentProperties = UeAgentUObjectProperties::ExportComponentProperties(LightComponent, Options);
	TestTrue(TEXT("component properties valid"), ComponentProperties.IsValid());
	if (ComponentProperties.IsValid())
	{
		TestEqual(TEXT("component schema"), ComponentProperties->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
		TestEqual(TEXT("component profile"), ComponentProperties->GetStringField(TEXT("profile")), FString(TEXT("authoring")));
		TestFalse(TEXT("component export has no component-only schema"), ComponentProperties->HasField(TEXT("component_schema")));
		TestTrue(TEXT("component intensity descriptor exists"), FindPropertyDescriptor(ComponentProperties, TEXT("Intensity")).IsValid());
	}

	TArray<TSharedPtr<FJsonValue>> ComponentEntries = UeAgentUObjectProperties::ExportActorComponentProperties(Actor, Options);
	TestEqual(TEXT("component entry count"), ComponentEntries.Num(), 1);
	if (ComponentEntries.Num() == 1)
	{
		const TSharedPtr<FJsonObject> Entry = ComponentEntries[0].IsValid() ? ComponentEntries[0]->AsObject() : nullptr;
		TestTrue(TEXT("component entry valid"), Entry.IsValid());
		if (Entry.IsValid())
		{
			TestEqual(TEXT("entry component schema"), Entry->GetStringField(TEXT("component_schema")), FString(TEXT("uai.uobject_properties.v1")));
			const TSharedPtr<FJsonObject>* NestedProperties = nullptr;
			TestTrue(TEXT("entry properties object"), Entry->TryGetObjectField(TEXT("properties"), NestedProperties) && NestedProperties && NestedProperties->IsValid());
			if (NestedProperties && NestedProperties->IsValid())
			{
				TestEqual(TEXT("entry nested schema"), (*NestedProperties)->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesProfileFilteringTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.ProfileFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesProfileFilteringTest::RunTest(const FString& Parameters)
{
	UPointLightComponent* Component = NewObject<UPointLightComponent>(GetTransientPackage());
	TestNotNull(TEXT("component created"), Component);
	if (!Component)
	{
		return false;
	}

	Component->SetIntensity(9876.0f);

	UeAgentUObjectProperties::FExportOptions IndexOptions;
	IndexOptions.Profile = UeAgentUObjectProperties::EExportProfile::Index;
	TSharedPtr<FJsonObject> IndexExported = UeAgentUObjectProperties::ExportObjectProperties(Component, IndexOptions);
	TestTrue(TEXT("index export valid"), IndexExported.IsValid());
	TestEqual(TEXT("index profile"), IndexExported->GetStringField(TEXT("profile")), FString(TEXT("index")));

	TSharedPtr<FJsonObject> IndexIntensity = FindPropertyDescriptor(IndexExported, TEXT("Intensity"));
	TestTrue(TEXT("index intensity descriptor exists"), IndexIntensity.IsValid());
	if (IndexIntensity.IsValid())
	{
		TestFalse(TEXT("index omits value"), IndexIntensity->HasField(TEXT("value")));
		TestFalse(TEXT("index omits tooltip"), IndexIntensity->HasField(TEXT("tooltip")));
	}

	UeAgentUObjectProperties::FExportOptions AuthoringOptions;
	AuthoringOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
	TSharedPtr<FJsonObject> AuthoringExported = UeAgentUObjectProperties::ExportObjectProperties(Component, AuthoringOptions);
	TestTrue(TEXT("authoring export valid"), AuthoringExported.IsValid());
	TestEqual(TEXT("authoring profile"), AuthoringExported->GetStringField(TEXT("profile")), FString(TEXT("authoring")));
	TestFalse(TEXT("authoring omits skipped details"), AuthoringExported->HasField(TEXT("skipped")));

	TSharedPtr<FJsonObject> AuthoringIntensity = FindPropertyDescriptor(AuthoringExported, TEXT("Intensity"));
	TestTrue(TEXT("authoring intensity descriptor exists"), AuthoringIntensity.IsValid());
	if (AuthoringIntensity.IsValid())
	{
		TestTrue(TEXT("authoring includes value"), AuthoringIntensity->HasField(TEXT("value")));
		TestTrue(TEXT("authoring includes tooltip"), AuthoringIntensity->HasField(TEXT("tooltip")));
		TestTrue(TEXT("authoring includes display name"), AuthoringIntensity->HasField(TEXT("display_name")));
		TestTrue(TEXT("authoring includes category path"), AuthoringIntensity->HasField(TEXT("category_path")));
		TestTrue(TEXT("authoring includes control type"), AuthoringIntensity->HasField(TEXT("control_type")));
		TestTrue(TEXT("authoring includes value kind"), AuthoringIntensity->HasField(TEXT("value_kind")));
		TestTrue(TEXT("authoring includes availability"), AuthoringIntensity->HasField(TEXT("availability")));
		TestTrue(TEXT("authoring includes operations"), AuthoringIntensity->HasField(TEXT("operations")));
		TestTrue(TEXT("authoring includes constraints"), AuthoringIntensity->HasField(TEXT("constraints")));
		TestFalse(TEXT("authoring omits default_value"), AuthoringIntensity->HasField(TEXT("default_value")));
		TestFalse(TEXT("authoring omits metadata"), AuthoringIntensity->HasField(TEXT("owner_class_path")));
	}
	TestTrue(TEXT("authoring keeps unchanged editable properties"), FindPropertyDescriptor(AuthoringExported, TEXT("AttenuationRadius")).IsValid());

	UeAgentUObjectProperties::FExportOptions FullOptions;
	FullOptions.Profile = UeAgentUObjectProperties::EExportProfile::Full;
	TSharedPtr<FJsonObject> FullExported = UeAgentUObjectProperties::ExportObjectProperties(Component, FullOptions);
	TestTrue(TEXT("full export valid"), FullExported.IsValid());
	TestEqual(TEXT("full profile"), FullExported->GetStringField(TEXT("profile")), FString(TEXT("full")));
	TestTrue(TEXT("full includes at least authoring properties"), GetPropertyCount(FullExported) >= GetPropertyCount(AuthoringExported));
	TSharedPtr<FJsonObject> FullIntensity = FindPropertyDescriptor(FullExported, TEXT("Intensity"));
	TestTrue(TEXT("full intensity descriptor exists"), FullIntensity.IsValid());
	if (FullIntensity.IsValid())
	{
		TestTrue(TEXT("full includes default_value"), FullIntensity->HasField(TEXT("default_value")));
		TestTrue(TEXT("full includes owner class"), FullIntensity->HasField(TEXT("owner_class_path")));
		TestTrue(TEXT("full includes CDO comparison"), FullIntensity->HasField(TEXT("differs_from_cdo")));
	}

	UeAgentUObjectProperties::FExportOptions DebugOptions;
	DebugOptions.Profile = UeAgentUObjectProperties::EExportProfile::Debug;
	TSharedPtr<FJsonObject> DebugExported = UeAgentUObjectProperties::ExportObjectProperties(Component, DebugOptions);
	TestTrue(TEXT("debug export valid"), DebugExported.IsValid());
	TestEqual(TEXT("debug profile"), DebugExported->GetStringField(TEXT("profile")), FString(TEXT("debug")));
	TestTrue(TEXT("debug includes skipped details"), DebugExported->HasField(TEXT("skipped")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentUObjectPropertiesRealObjectMatrixTest,
	"GptProjectTest.UeAgentInterface.Smoke.UObjectProperties.RealObjectMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUObjectPropertiesRealObjectMatrixTest::RunTest(const FString& Parameters)
{
	UeAgentUObjectProperties::FExportOptions Options;
	Options.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;

	UCameraComponent* CameraComponent = NewObject<UCameraComponent>(GetTransientPackage(), TEXT("UAI_TestCameraComponent"));
	TestNotNull(TEXT("camera component created"), CameraComponent);
	if (CameraComponent)
	{
		CameraComponent->SetFieldOfView(72.0f);
		TSharedPtr<FJsonObject> CameraExported = UeAgentUObjectProperties::ExportComponentProperties(CameraComponent, Options);
		TestTrue(TEXT("camera export valid"), CameraExported.IsValid());
		if (CameraExported.IsValid())
		{
			TestEqual(TEXT("camera schema"), CameraExported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			TestTrue(TEXT("camera FieldOfView exists"), FindPropertyDescriptor(CameraExported, TEXT("FieldOfView")).IsValid());
			TSharedPtr<FJsonObject> RelativeLocation = FindPropertyDescriptor(CameraExported, TEXT("RelativeLocation"));
			TestTrue(TEXT("camera RelativeLocation exists"), RelativeLocation.IsValid());
			if (RelativeLocation.IsValid())
			{
				TestTrue(TEXT("camera RelativeLocation has space contract"), RelativeLocation->HasField(TEXT("space_contract")));
			}
		}
	}

	UTextRenderComponent* TextComponent = NewObject<UTextRenderComponent>(GetTransientPackage(), TEXT("UAI_TestTextRenderComponent"));
	TestNotNull(TEXT("text render component created"), TextComponent);
	if (TextComponent)
	{
		TextComponent->SetText(FText::FromString(TEXT("UAI Matrix")));
		TSharedPtr<FJsonObject> TextExported = UeAgentUObjectProperties::ExportComponentProperties(TextComponent, Options);
		TestTrue(TEXT("text export valid"), TextExported.IsValid());
		if (TextExported.IsValid())
		{
			TestEqual(TEXT("text schema"), TextExported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			TestTrue(TEXT("text Text exists"), FindPropertyDescriptor(TextExported, TEXT("Text")).IsValid());
		}
	}

	UStaticMeshComponent* StaticMeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), TEXT("UAI_TestStaticMeshComponent"));
	TestNotNull(TEXT("static mesh component created"), StaticMeshComponent);
	if (StaticMeshComponent)
	{
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		TestNotNull(TEXT("cube mesh loaded for matrix"), CubeMesh);
		StaticMeshComponent->SetStaticMesh(CubeMesh);
		TSharedPtr<FJsonObject> StaticMeshExported = UeAgentUObjectProperties::ExportComponentProperties(StaticMeshComponent, Options);
		TestTrue(TEXT("static mesh export valid"), StaticMeshExported.IsValid());
		if (StaticMeshExported.IsValid())
		{
			TestEqual(TEXT("static mesh schema"), StaticMeshExported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			TestTrue(TEXT("static mesh property exists"), FindPropertyDescriptor(StaticMeshExported, TEXT("StaticMesh")).IsValid());
		}
	}

	UBoxComponent* BoxComponent = NewObject<UBoxComponent>(GetTransientPackage(), TEXT("UAI_TestBoxComponent"));
	TestNotNull(TEXT("box component created"), BoxComponent);
	if (BoxComponent)
	{
		BoxComponent->SetBoxExtent(FVector(12.0, 24.0, 36.0));
		TSharedPtr<FJsonObject> BoxExported = UeAgentUObjectProperties::ExportComponentProperties(BoxComponent, Options);
		TestTrue(TEXT("box export valid"), BoxExported.IsValid());
		if (BoxExported.IsValid())
		{
			TestEqual(TEXT("box schema"), BoxExported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			TestTrue(TEXT("box extent exists"), FindPropertyDescriptor(BoxExported, TEXT("BoxExtent")).IsValid());
		}

		UeAgentUObjectProperties::FExportOptions FullOptions;
		FullOptions.Profile = UeAgentUObjectProperties::EExportProfile::Full;
		TSharedPtr<FJsonObject> BoxFullExported = UeAgentUObjectProperties::ExportComponentProperties(BoxComponent, FullOptions);
		TestTrue(TEXT("box full export valid"), BoxFullExported.IsValid());
		if (BoxFullExported.IsValid())
		{
			TSharedPtr<FJsonObject> BodyInstance = FindPropertyDescriptor(BoxFullExported, TEXT("BodyInstance"));
			TestTrue(TEXT("box primitive collision body exists"), BodyInstance.IsValid());
			if (BodyInstance.IsValid())
			{
				TestEqual(TEXT("box primitive collision body kind"), GetValueKind(BodyInstance), FString(TEXT("struct")));
			}
		}
	}

	AActor* Actor = NewObject<AActor>(GetTransientPackage(), AActor::StaticClass(), TEXT("UAI_TestMatrixActor"));
	TestNotNull(TEXT("matrix actor created"), Actor);
	if (Actor)
	{
		Actor->SetActorLabel(TEXT("UAI Matrix Actor"));
		Actor->SetCanBeDamaged(true);
		TSharedPtr<FJsonObject> ActorExported = UeAgentUObjectProperties::ExportActorProperties(Actor, Options);
		TestTrue(TEXT("matrix actor export valid"), ActorExported.IsValid());
		if (ActorExported.IsValid())
		{
			TestEqual(TEXT("matrix actor schema"), ActorExported->GetStringField(TEXT("schema")), FString(TEXT("uai.uobject_properties.v1")));
			TestTrue(TEXT("matrix actor has properties"), ActorExported->GetNumberField(TEXT("property_count")) > 0.0);
		}
	}

	return true;
}

#endif
