#if WITH_DEV_AUTOMATION_TESTS

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentInterfaceSmoke, Log, All);

#include "UeAgentInterfaceSmokeTestCommon.inl"

namespace
{
	static FString JsonString(const FString& Text)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(MakeShared<FJsonValueString>(Text), TEXT(""), Writer);
		return Out;
	}

	static bool ExecProjectSettingsCommand(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		double TimeoutSeconds = 45.0)
	{
		OutData = MakeShared<FJsonObject>();
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}
		if (!Test->TestEqual(Command + TEXT(" http status"), Result.StatusCode, 200))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		if (!Test->TestTrue(Command + TEXT(" ok=true"), Root->GetBoolField(TEXT("ok"))))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* Data = nullptr;
		OutData = (Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()) ? *Data : MakeShared<FJsonObject>();
		return true;
	}

	static bool ExecProjectSettingsCommandExpectFailure(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedToken)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" failure request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 45.0)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" failure response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		const bool bMatches = Error.Contains(ExpectedToken);
		if (!Test->TestTrue(Command + TEXT(" expected error token ") + ExpectedToken, bMatches))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		return true;
	}

	static bool FindGeneralProjectSettingsSection(FAutomationTestBase* Test, const TSharedPtr<FJsonObject>& IndexData, FString& OutSectionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!Test->TestTrue(TEXT("index has sections"), IndexData.IsValid() && IndexData->TryGetArrayField(TEXT("sections"), Sections) && Sections))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				continue;
			}
			const FString ClassPath = (*SectionObj)->GetStringField(TEXT("settings_object_class"));
			if (ClassPath.Contains(TEXT("GeneralProjectSettings")))
			{
				OutSectionId = (*SectionObj)->GetStringField(TEXT("section_id"));
				return !OutSectionId.IsEmpty();
			}
		}
		Test->AddError(TEXT("GeneralProjectSettings section not found in reflected settings index."));
		return false;
	}

	static bool FindSectionIdContaining(const TSharedPtr<FJsonObject>& IndexData, const FString& Token, FString& OutSectionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!IndexData.IsValid() || !IndexData->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				continue;
			}
			FString SectionId;
			(*SectionObj)->TryGetStringField(TEXT("section_id"), SectionId);
			if (SectionId.Contains(Token, ESearchCase::IgnoreCase))
			{
				OutSectionId = SectionId;
				return !OutSectionId.IsEmpty();
			}
		}
		return false;
	}

	static bool ValidateFilteredIndex(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& IndexData,
		const FString& Label)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		bool bOk = Test->TestTrue(Label + TEXT(" has sections"), IndexData.IsValid() && IndexData->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
		if (!bOk || !Sections)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				continue;
			}
			FString SectionId;
			FString Category;
			FString SourcePlugin;
			FString RequiredPlugin;
			(*SectionObj)->TryGetStringField(TEXT("section_id"), SectionId);
			(*SectionObj)->TryGetStringField(TEXT("category"), Category);
			(*SectionObj)->TryGetStringField(TEXT("source_plugin"), SourcePlugin);
			(*SectionObj)->TryGetStringField(TEXT("requires_plugin"), RequiredPlugin);
			const bool bPluginSection = Category.Equals(TEXT("Plugins"), ESearchCase::IgnoreCase) || !SourcePlugin.IsEmpty() || !RequiredPlugin.IsEmpty();
			const bool bDeveloperSection = SectionId.Contains(TEXT("Developer"), ESearchCase::IgnoreCase);
			const bool bCanEdit = (*SectionObj)->GetBoolField(TEXT("can_edit"));
			bOk &= Test->TestFalse(Label + TEXT(" excludes plugin section ") + SectionId, bPluginSection);
			bOk &= Test->TestFalse(Label + TEXT(" excludes developer section ") + SectionId, bDeveloperSection);
			bOk &= Test->TestTrue(Label + TEXT(" excludes readonly section ") + SectionId, bCanEdit);
			bOk &= Test->TestFalse(Label + TEXT(" omits field_count ") + SectionId, (*SectionObj)->HasField(TEXT("field_count")));
			bOk &= Test->TestFalse(Label + TEXT(" omits writable_field_count ") + SectionId, (*SectionObj)->HasField(TEXT("writable_field_count")));
			bOk &= Test->TestTrue(Label + TEXT(" keeps field_schema_hash ") + SectionId, !(*SectionObj)->GetStringField(TEXT("field_schema_hash")).IsEmpty());
			bOk &= Test->TestTrue(Label + TEXT(" keeps page_value_hash ") + SectionId, !(*SectionObj)->GetStringField(TEXT("page_value_hash")).IsEmpty());
		}
		return bOk;
	}

	static bool GetExportedFieldStringValue(const TSharedPtr<FJsonObject>& PageData, const FString& FieldName, FString& OutValue)
	{
		const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
		if (PageData.IsValid() && PageData->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid())
		{
			return (*ValuesObj)->TryGetStringField(FieldName, OutValue);
		}
		return false;
	}

	static bool GetExportedFieldBoolValue(const TSharedPtr<FJsonObject>& PageData, const FString& FieldName, bool& OutValue)
	{
		const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
		if (PageData.IsValid() && PageData->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid())
		{
			return (*ValuesObj)->TryGetBoolField(FieldName, OutValue);
		}
		return false;
	}

	static bool GetExportedFieldSchemaHash(const TSharedPtr<FJsonObject>& PageData, const FString& FieldName, FString& OutHash)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
		const TSharedPtr<FJsonObject>* FieldObj = nullptr;
		return PageData.IsValid()
			&& PageData->TryGetObjectField(TEXT("fields"), FieldsObj)
			&& FieldsObj
			&& FieldsObj->IsValid()
			&& (*FieldsObj)->TryGetObjectField(FieldName, FieldObj)
			&& FieldObj
			&& FieldObj->IsValid()
			&& (*FieldObj)->TryGetStringField(TEXT("field_schema_hash"), OutHash)
			&& !OutHash.IsEmpty();
	}

	static const TSharedPtr<FJsonObject>* GetExportedFieldObject(const TSharedPtr<FJsonObject>& PageData, const FString& FieldName)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
		const TSharedPtr<FJsonObject>* FieldObj = nullptr;
		if (PageData.IsValid()
			&& PageData->TryGetObjectField(TEXT("fields"), FieldsObj)
			&& FieldsObj
			&& FieldsObj->IsValid()
			&& (*FieldsObj)->TryGetObjectField(FieldName, FieldObj)
			&& FieldObj
			&& FieldObj->IsValid())
		{
			return FieldObj;
		}
		return nullptr;
	}

	static const TSharedPtr<FJsonObject>* FindFieldResult(const TSharedPtr<FJsonObject>& Data, const FString& FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* FieldResults = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("field_results"), FieldResults) || !FieldResults)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *FieldResults)
		{
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ResultObj) || !ResultObj || !ResultObj->IsValid())
			{
				continue;
			}
			FString ResultField;
			(*ResultObj)->TryGetStringField(TEXT("field"), ResultField);
			if (ResultField.Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return ResultObj;
			}
		}
		return nullptr;
	}

	static const TSharedPtr<FJsonObject>* FindConfigResult(const TSharedPtr<FJsonObject>& Data, const FString& Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("results"), Results) || !Results)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Results)
		{
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ResultObj) || !ResultObj || !ResultObj->IsValid())
			{
				continue;
			}
			FString ResultKey;
			(*ResultObj)->TryGetStringField(TEXT("key"), ResultKey);
			if (ResultKey.Equals(Key, ESearchCase::IgnoreCase))
			{
				return ResultObj;
			}
		}
		return nullptr;
	}

	static bool ExecProjectSettingsCommandExpectFailureFieldStatus(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedErrorToken,
		const FString& FieldName,
		const FString& ExpectedStatus)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" failure request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 45.0)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" failure response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		if (!Test->TestTrue(Command + TEXT(" expected error token ") + ExpectedErrorToken, Error.Contains(ExpectedErrorToken)))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Test->TestTrue(Command + TEXT(" failure data exists"), Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* FieldResult = FindFieldResult(*Data, FieldName);
		if (!Test->TestTrue(Command + TEXT(" failure field result exists"), FieldResult && FieldResult->IsValid()))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		if (FieldResult && FieldResult->IsValid())
		{
			Test->TestEqual(Command + TEXT(" failure field status"), (*FieldResult)->GetStringField(TEXT("status")), ExpectedStatus);
			Test->TestEqual(Command + TEXT(" failure field error"), (*FieldResult)->GetStringField(TEXT("error")), ExpectedStatus);
		}
		return true;
	}

	static bool ExecProjectSettingsCommandExpectFailureConfigResult(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedErrorToken,
		const FString& ResultKey,
		const FString& ExpectedStatus)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" failure request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 45.0)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" failure response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		if (!Test->TestTrue(Command + TEXT(" expected error token ") + ExpectedErrorToken, Error.Contains(ExpectedErrorToken)))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Test->TestTrue(Command + TEXT(" failure data exists"), Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* ConfigResult = FindConfigResult(*Data, ResultKey);
		if (!Test->TestTrue(Command + TEXT(" failure config result exists"), ConfigResult && ConfigResult->IsValid()))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		if (ConfigResult && ConfigResult->IsValid())
		{
			Test->TestEqual(Command + TEXT(" failure config status"), (*ConfigResult)->GetStringField(TEXT("status")), ExpectedStatus);
			Test->TestEqual(Command + TEXT(" failure config error"), (*ConfigResult)->GetStringField(TEXT("error")), ExpectedStatus);
		}
		return true;
	}

	static bool FindFirstEditableBoolField(const TSharedPtr<FJsonObject>& PageData, FString& OutFieldName)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
		if (!PageData.IsValid() || !PageData->TryGetObjectField(TEXT("fields"), FieldsObj) || !FieldsObj || !FieldsObj->IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*FieldsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}
			bool bEditable = false;
			FString CppType;
			(*FieldObj)->TryGetBoolField(TEXT("editable"), bEditable);
			(*FieldObj)->TryGetStringField(TEXT("cpp_type"), CppType);
			if (bEditable && CppType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				OutFieldName = Pair.Key;
				return true;
			}
		}
		return false;
	}

	static bool FindFirstEditableRestartBoolField(const TSharedPtr<FJsonObject>& PageData, FString& OutFieldName)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
		if (!PageData.IsValid() || !PageData->TryGetObjectField(TEXT("fields"), FieldsObj) || !FieldsObj || !FieldsObj->IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*FieldsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}
			bool bEditable = false;
			bool bRequiresRestart = false;
			FString CppType;
			(*FieldObj)->TryGetBoolField(TEXT("editable"), bEditable);
			(*FieldObj)->TryGetBoolField(TEXT("requires_restart"), bRequiresRestart);
			(*FieldObj)->TryGetStringField(TEXT("cpp_type"), CppType);
			if (bEditable && bRequiresRestart && CppType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				OutFieldName = Pair.Key;
				return true;
			}
		}
		return false;
	}

	static bool FindFirstNonEditableField(const TSharedPtr<FJsonObject>& PageData, FString& OutFieldName)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
		if (!PageData.IsValid() || !PageData->TryGetObjectField(TEXT("fields"), FieldsObj) || !FieldsObj || !FieldsObj->IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*FieldsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}
			bool bEditable = true;
			(*FieldObj)->TryGetBoolField(TEXT("editable"), bEditable);
			if (!bEditable)
			{
				OutFieldName = Pair.Key;
				return true;
			}
		}
		return false;
	}

	static bool ExpectProjectDescriptionValues(
		FAutomationTestBase* Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& PageData,
		const FString& ExpectedProjectName,
		const FString& ExpectedProjectVersion,
		const FString& ExpectedCompanyName)
	{
		FString ActualProjectName;
		FString ActualProjectVersion;
		FString ActualCompanyName;
		bool bOk = true;
		bOk &= Test->TestTrue(Label + TEXT(" ProjectName reads"), GetExportedFieldStringValue(PageData, TEXT("ProjectName"), ActualProjectName));
		bOk &= Test->TestTrue(Label + TEXT(" ProjectVersion reads"), GetExportedFieldStringValue(PageData, TEXT("ProjectVersion"), ActualProjectVersion));
		bOk &= Test->TestTrue(Label + TEXT(" CompanyName reads"), GetExportedFieldStringValue(PageData, TEXT("CompanyName"), ActualCompanyName));
		if (bOk)
		{
			bOk &= Test->TestEqual(Label + TEXT(" ProjectName matches"), ActualProjectName, ExpectedProjectName);
			bOk &= Test->TestEqual(Label + TEXT(" ProjectVersion matches"), ActualProjectVersion, ExpectedProjectVersion);
			bOk &= Test->TestEqual(Label + TEXT(" CompanyName matches"), ActualCompanyName, ExpectedCompanyName);
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceProjectSettingsJsonSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ProjectSettingsJsonWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceProjectSettingsJsonSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString OriginalDefaultGameIni = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
	FString OriginalDefaultGameText;
	FFileHelper::LoadFileToString(OriginalDefaultGameText, *OriginalDefaultGameIni);
	auto RestoreDefaultGameIni = [&]()
	{
		if (!OriginalDefaultGameText.IsEmpty())
		{
			FFileHelper::SaveStringToFile(OriginalDefaultGameText, *OriginalDefaultGameIni, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			if (GConfig)
			{
				GConfig->LoadFile(OriginalDefaultGameIni);
			}
		}
	};

	bool bOk = true;
	const FString RootId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterfaceProjectSettingsSmoke"), RootId));

	TSharedPtr<FJsonObject> IndexData;
	const bool bIndexOk = ExecProjectSettingsCommand(
		this,
		ServerScope,
		TEXT("smoke-project-settings-index-") + RootId,
		TEXT("project_settings_export_index"),
		FString::Printf(TEXT("{\"output_directory\":%s,\"write_files\":true}"), *JsonString(OutputRoot)),
		IndexData);
	bOk &= bIndexOk;
	if (bIndexOk)
	{
		TestEqual(TEXT("index schema"), IndexData->GetStringField(TEXT("schema")), FString(TEXT("ue_agent.project_settings.index.v1")));
		TestTrue(TEXT("index generated_at"), !IndexData->GetStringField(TEXT("generated_at")).IsEmpty());
		TestTrue(TEXT("index project name"), !IndexData->GetStringField(TEXT("project_name")).IsEmpty());
		TestTrue(TEXT("index plugin fingerprint hash"), !IndexData->GetStringField(TEXT("plugin_fingerprint_hash")).IsEmpty());
		TestTrue(TEXT("index section_count > 0"), IndexData->GetIntegerField(TEXT("section_count")) > 0);
		TestTrue(TEXT("index contains registry hash"), !IndexData->GetStringField(TEXT("settings_registry_hash")).IsEmpty());
		TestTrue(TEXT("index file saved"), FPaths::FileExists(IndexData->GetStringField(TEXT("index_file"))));
		TestTrue(TEXT("manifest file saved"), FPaths::FileExists(IndexData->GetStringField(TEXT("manifest_file"))));
		const TSharedPtr<FJsonObject>* ManifestObj = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ManifestEntries = nullptr;
		TestTrue(TEXT("manifest has entries"), IndexData->TryGetObjectField(TEXT("manifest"), ManifestObj) && ManifestObj && ManifestObj->IsValid() && (*ManifestObj)->TryGetArrayField(TEXT("entries"), ManifestEntries) && ManifestEntries && ManifestEntries->Num() > 0);
	}

	TSharedPtr<FJsonObject> AliasIndexData;
	if (bIndexOk)
	{
		const FString AliasIndexFile = FPaths::Combine(OutputRoot, TEXT("alias_index.json"));
		const bool bAliasIndexOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-index-output-path-") + RootId,
			TEXT("project_settings_export_index"),
			FString::Printf(TEXT("{\"container\":\"Project\",\"output_path\":%s,\"write_files\":true,\"include_unsupported_sections\":true}"), *JsonString(AliasIndexFile)),
			AliasIndexData);
		bOk &= bAliasIndexOk;
		if (bAliasIndexOk)
		{
			TestEqual(TEXT("index output_path alias saved"), AliasIndexData->GetStringField(TEXT("index_file")), FPaths::ConvertRelativePathToFull(AliasIndexFile));
			TestEqual(TEXT("index container filter echoed"), AliasIndexData->GetStringField(TEXT("container")), FString(TEXT("Project")));
			TestTrue(TEXT("index output_path manifest saved beside index"), FPaths::FileExists(FPaths::Combine(FPaths::GetPath(AliasIndexData->GetStringField(TEXT("index_file"))), TEXT("manifest.json"))));
		}
	}

	TSharedPtr<FJsonObject> FilteredIndexData;
	if (bIndexOk)
	{
		const bool bFilteredIndexOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-index-filters-") + RootId,
			TEXT("project_settings_export_index"),
			FString::Printf(TEXT("{\"container\":\"Project\",\"write_files\":false,\"include_plugin_sections\":false,\"include_developer_sections\":false,\"include_readonly_sections\":false,\"include_field_counts\":false,\"include_unsupported_sections\":true,\"output_directory\":%s}"), *JsonString(OutputRoot)),
			FilteredIndexData);
		bOk &= bFilteredIndexOk;
		if (bFilteredIndexOk)
		{
			const TSharedPtr<FJsonObject>* Filters = nullptr;
			TestTrue(TEXT("index filters object exists"), FilteredIndexData->TryGetObjectField(TEXT("filters"), Filters) && Filters && Filters->IsValid());
			if (Filters && Filters->IsValid())
			{
				TestFalse(TEXT("index include_plugin_sections=false echoed"), (*Filters)->GetBoolField(TEXT("include_plugin_sections")));
				TestFalse(TEXT("index include_developer_sections=false echoed"), (*Filters)->GetBoolField(TEXT("include_developer_sections")));
				TestFalse(TEXT("index include_readonly_sections=false echoed"), (*Filters)->GetBoolField(TEXT("include_readonly_sections")));
				TestFalse(TEXT("index include_field_counts=false echoed"), (*Filters)->GetBoolField(TEXT("include_field_counts")));
			}
			bOk &= ValidateFilteredIndex(this, FilteredIndexData, TEXT("filtered index"));
		}
	}

	FString SectionId;
	bOk &= FindGeneralProjectSettingsSection(this, IndexData, SectionId);

	TSharedPtr<FJsonObject> PageData;
	if (bOk)
	{
		const bool bPageOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-page-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"output_directory\":%s,\"write_file\":true}"), *JsonString(SectionId), *JsonString(OutputRoot)),
			PageData);
		bOk &= bPageOk;
	}

	FString OriginalProjectName;
	FString OriginalProjectVersion;
	FString OriginalCompanyName;
	FString ProjectNameFieldSchemaHash;
	FString ProjectVersionFieldSchemaHash;
	FString CompanyNameFieldSchemaHash;
	FString InvalidTypeBoolFieldName;
	FString NonEditableFieldName;
	if (bOk)
	{
		TestEqual(TEXT("page schema"), PageData->GetStringField(TEXT("schema")), FString(TEXT("ue_agent.project_settings.page.v1")));
		TestTrue(TEXT("page generated_at"), !PageData->GetStringField(TEXT("generated_at")).IsEmpty());
		TestEqual(TEXT("page section_id matches"), PageData->GetStringField(TEXT("section_id")), SectionId);
		TestTrue(TEXT("page contains field_schema_hash"), !PageData->GetStringField(TEXT("field_schema_hash")).IsEmpty());
		TestTrue(TEXT("page contains page_value_hash"), !PageData->GetStringField(TEXT("page_value_hash")).IsEmpty());
		TestTrue(TEXT("page contains settings_registry_hash"), !PageData->GetStringField(TEXT("settings_registry_hash")).IsEmpty());
		TestTrue(TEXT("page contains plugin fingerprint hash"), !PageData->GetStringField(TEXT("plugin_fingerprint_hash")).IsEmpty());
		const TSharedPtr<FJsonObject>* SectionInfoObj = nullptr;
		TestTrue(TEXT("page contains section_info"), PageData->TryGetObjectField(TEXT("section_info"), SectionInfoObj) && SectionInfoObj && SectionInfoObj->IsValid());
		TestTrue(TEXT("page file saved"), FPaths::FileExists(PageData->GetStringField(TEXT("output_file"))));
		TestTrue(TEXT("page raw_config file saved"), FPaths::FileExists(PageData->GetStringField(TEXT("raw_config_file"))));
		TestTrue(TEXT("page has ProjectName"), GetExportedFieldStringValue(PageData, TEXT("ProjectName"), OriginalProjectName));
		TestTrue(TEXT("page has ProjectName field schema hash"), GetExportedFieldSchemaHash(PageData, TEXT("ProjectName"), ProjectNameFieldSchemaHash));
		TestTrue(TEXT("page has ProjectVersion"), GetExportedFieldStringValue(PageData, TEXT("ProjectVersion"), OriginalProjectVersion));
		TestTrue(TEXT("page has ProjectVersion field schema hash"), GetExportedFieldSchemaHash(PageData, TEXT("ProjectVersion"), ProjectVersionFieldSchemaHash));
		TestTrue(TEXT("page has CompanyName"), GetExportedFieldStringValue(PageData, TEXT("CompanyName"), OriginalCompanyName));
		TestTrue(TEXT("page has CompanyName field schema hash"), GetExportedFieldSchemaHash(PageData, TEXT("CompanyName"), CompanyNameFieldSchemaHash));
		TestTrue(TEXT("page has raw_config"), PageData->HasField(TEXT("raw_config")));
		TestTrue(TEXT("page has editable bool field for invalid type coverage"), FindFirstEditableBoolField(PageData, InvalidTypeBoolFieldName));
		FindFirstNonEditableField(PageData, NonEditableFieldName);
		const TSharedPtr<FJsonObject>* ProjectNameField = GetExportedFieldObject(PageData, TEXT("ProjectName"));
		TestTrue(TEXT("ProjectName field metadata exists"), ProjectNameField && ProjectNameField->IsValid());
		if (ProjectNameField && ProjectNameField->IsValid())
		{
			TestEqual(TEXT("ProjectName json_type"), (*ProjectNameField)->GetStringField(TEXT("json_type")), FString(TEXT("string")));
			TestEqual(TEXT("ProjectName config_key"), (*ProjectNameField)->GetStringField(TEXT("config_key")), FString(TEXT("ProjectName")));
			TestTrue(TEXT("ProjectName has config_source"), !(*ProjectNameField)->GetStringField(TEXT("config_source")).IsEmpty());
			TestTrue(TEXT("ProjectName has property_flags"), (*ProjectNameField)->HasField(TEXT("property_flags")));
			TestTrue(TEXT("ProjectName has default_value"), (*ProjectNameField)->HasField(TEXT("default_value")));
			TestTrue(TEXT("ProjectName has visible"), (*ProjectNameField)->HasField(TEXT("visible")));
		}
	}

	TSharedPtr<FJsonObject> PageNoRawConfigData;
	if (bOk)
	{
		const FString AliasPageFile = FPaths::Combine(OutputRoot, TEXT("alias_page_settings.json"));
		const bool bPageNoRawConfigOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-page-output-path-no-raw-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"output_path\":%s,\"write_file\":true,\"include_raw_config\":false,\"include_hidden_properties\":false}"), *JsonString(SectionId), *JsonString(AliasPageFile)),
			PageNoRawConfigData);
		bOk &= bPageNoRawConfigOk;
		if (bPageNoRawConfigOk)
		{
			TestEqual(TEXT("page output_path alias saved"), PageNoRawConfigData->GetStringField(TEXT("output_file")), FPaths::ConvertRelativePathToFull(AliasPageFile));
			TestFalse(TEXT("page include_raw_config=false omits raw_config"), PageNoRawConfigData->HasField(TEXT("raw_config")));
		}
	}

	TSharedPtr<FJsonObject> PageNoDefaultsMetadataData;
	if (bOk)
	{
		const bool bPageNoDefaultsMetadataOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-page-no-defaults-metadata-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false,\"include_defaults\":false,\"include_metadata\":false}"), *JsonString(SectionId)),
			PageNoDefaultsMetadataData);
		bOk &= bPageNoDefaultsMetadataOk;
		if (bPageNoDefaultsMetadataOk)
		{
			const TSharedPtr<FJsonObject>* ProjectNameField = GetExportedFieldObject(PageNoDefaultsMetadataData, TEXT("ProjectName"));
			TestTrue(TEXT("page include flags ProjectName field exists"), ProjectNameField && ProjectNameField->IsValid());
			if (ProjectNameField && ProjectNameField->IsValid())
			{
				TestFalse(TEXT("page include_defaults=false omits default_value"), (*ProjectNameField)->HasField(TEXT("default_value")));
				TestFalse(TEXT("page include_metadata=false omits metadata"), (*ProjectNameField)->HasField(TEXT("metadata")));
				TestTrue(TEXT("page include flags keep value"), (*ProjectNameField)->HasField(TEXT("value")));
				TestTrue(TEXT("page include flags keep field_schema_hash"), (*ProjectNameField)->HasField(TEXT("field_schema_hash")));
			}
		}
	}

	FString HighRiskSectionId;
	if (bOk)
	{
		bOk &= TestTrue(TEXT("Rendering settings section found for high risk guard coverage"), FindSectionIdContaining(IndexData, TEXT("Rendering"), HighRiskSectionId));
	}
	if (bOk)
	{
		TSharedPtr<FJsonObject> HighRiskPageData;
		const bool bHighRiskPageOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-high-risk-page-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(HighRiskSectionId)),
			HighRiskPageData);
		bOk &= bHighRiskPageOk;
		if (bHighRiskPageOk)
		{
			FString HighRiskBoolField;
			bOk &= TestTrue(TEXT("high risk page has editable bool field"), FindFirstEditableBoolField(HighRiskPageData, HighRiskBoolField));
			if (bOk)
			{
				bool CurrentValue = false;
				bOk &= TestTrue(TEXT("high risk bool value reads"), GetExportedFieldBoolValue(HighRiskPageData, HighRiskBoolField, CurrentValue));
				if (bOk)
				{
					bOk &= ExecProjectSettingsCommandExpectFailureFieldStatus(
						this,
						ServerScope,
						TEXT("smoke-project-settings-high-risk-blocked-") + RootId,
						TEXT("project_settings_apply_page"),
						FString::Printf(
							TEXT("{\"section_id\":%s,\"dry_run\":false,\"backup\":false,\"read_back\":false,\"values\":{%s:%s}}"),
							*JsonString(HighRiskSectionId),
							*JsonString(HighRiskBoolField),
							CurrentValue ? TEXT("false") : TEXT("true")),
						TEXT("project_settings_apply_failed"),
						HighRiskBoolField,
						TEXT("high_risk_blocked"));
				}
			}

			FString RestartBoolField;
			bOk &= TestTrue(TEXT("high risk page has editable restart bool field"), FindFirstEditableRestartBoolField(HighRiskPageData, RestartBoolField));
			if (bOk)
			{
				bool CurrentValue = false;
				bOk &= TestTrue(TEXT("restart bool value reads"), GetExportedFieldBoolValue(HighRiskPageData, RestartBoolField, CurrentValue));
				if (bOk)
				{
					TSharedPtr<FJsonObject> RestartValidateData;
					const bool bRestartValidateOk = ExecProjectSettingsCommand(
						this,
						ServerScope,
						TEXT("smoke-project-settings-restart-blocked-validate-") + RootId,
						TEXT("project_settings_validate_page"),
						FString::Printf(
							TEXT("{\"section_id\":%s,\"fail_on_requires_restart\":true,\"allow_high_risk\":true,\"values\":{%s:%s}}"),
							*JsonString(HighRiskSectionId),
							*JsonString(RestartBoolField),
							CurrentValue ? TEXT("false") : TEXT("true")),
						RestartValidateData);
					bOk &= bRestartValidateOk;
					if (bRestartValidateOk)
					{
						TestFalse(TEXT("requires restart validate success=false"), RestartValidateData->GetBoolField(TEXT("success")));
						const TSharedPtr<FJsonObject>* RestartResult = FindFieldResult(RestartValidateData, RestartBoolField);
						TestTrue(TEXT("requires restart result exists"), RestartResult && RestartResult->IsValid());
						if (RestartResult && RestartResult->IsValid())
						{
							TestEqual(TEXT("requires restart status"), (*RestartResult)->GetStringField(TEXT("status")), FString(TEXT("requires_restart_blocked")));
						}
					}
				}
			}
		}
	}

	const FString DryRunProjectName = TEXT("UAI_DryRun_Name_") + RootId.Left(8);
	const FString DryRunVersion = TEXT("UAI_Smoke_DryRun_") + RootId.Left(8);
	const FString DryRunCompanyName = TEXT("UAI_DryRun_Co_") + RootId.Left(8);
	const FString DefaultDryRunVersion = TEXT("UAI_Smoke_DefaultDryRun_") + RootId.Left(8);
	TSharedPtr<FJsonObject> DefaultDryRunData;
	if (bOk)
	{
		const bool bDefaultDryRunOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-default-dry-run-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"read_back\":true,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(DefaultDryRunVersion)),
			DefaultDryRunData);
		bOk &= bDefaultDryRunOk;
		if (bDefaultDryRunOk)
		{
			TestTrue(TEXT("default apply is dry_run"), DefaultDryRunData->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("default dry_run applies no fields"), DefaultDryRunData->GetIntegerField(TEXT("applied_field_count")), 0);
		}
	}

	TSharedPtr<FJsonObject> AfterDefaultDryRunPage;
	if (bOk)
	{
		const bool bAfterDefaultDryRunOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-default-dry-run-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterDefaultDryRunPage);
		bOk &= bAfterDefaultDryRunOk;
		if (bAfterDefaultDryRunOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("default dry_run did not change"),
				AfterDefaultDryRunPage,
				OriginalProjectName,
				OriginalProjectVersion,
				OriginalCompanyName);
		}
	}

	TSharedPtr<FJsonObject> DryRunData;
	if (bOk)
	{
		const bool bDryRunOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-dry-run-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":true,\"read_back\":true,\"values\":{\"ProjectName\":%s,\"ProjectVersion\":%s,\"CompanyName\":%s}}"),
				*JsonString(SectionId),
				*JsonString(DryRunProjectName),
				*JsonString(DryRunVersion),
				*JsonString(DryRunCompanyName)),
			DryRunData);
		bOk &= bDryRunOk;
		if (bDryRunOk)
		{
			TestTrue(TEXT("dry_run reports true"), DryRunData->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("dry_run applies no fields"), DryRunData->GetIntegerField(TEXT("applied_field_count")), 0);
		}
	}

	TSharedPtr<FJsonObject> AfterDryRunPage;
	if (bOk)
	{
		const bool bAfterDryRunOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-dry-run-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterDryRunPage);
		bOk &= bAfterDryRunOk;
		if (bAfterDryRunOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("dry_run did not change"),
				AfterDryRunPage,
				OriginalProjectName,
				OriginalProjectVersion,
				OriginalCompanyName);
		}
	}

	TSharedPtr<FJsonObject> DiffData;
	if (bOk)
	{
		const bool bDiffOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-diff-") + RootId,
			TEXT("project_settings_diff_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(DryRunVersion)),
			DiffData);
		bOk &= bDiffOk;
		if (bDiffOk)
		{
			TestTrue(TEXT("diff reports changed"), DiffData->GetBoolField(TEXT("changed")));
			const TArray<TSharedPtr<FJsonValue>>* FieldDiffs = nullptr;
			TestTrue(TEXT("diff returns field_diffs"), DiffData->TryGetArrayField(TEXT("field_diffs"), FieldDiffs) && FieldDiffs && FieldDiffs->Num() > 0);
		}
	}

	const FString BadSchemaHash = TEXT("bad_schema_hash_") + RootId.Left(8);
	TSharedPtr<FJsonObject> SchemaMismatchValidateData;
	if (bOk)
	{
		const bool bSchemaMismatchValidateOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-schema-mismatch-validate-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"base_field_schema_hash\":%s,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(BadSchemaHash),
				*JsonString(DryRunVersion)),
			SchemaMismatchValidateData);
		bOk &= bSchemaMismatchValidateOk;
		if (bSchemaMismatchValidateOk)
		{
			TestFalse(TEXT("schema mismatch validate returns valid=false"), SchemaMismatchValidateData->GetBoolField(TEXT("valid")));
			TestEqual(TEXT("schema mismatch validate error"), SchemaMismatchValidateData->GetStringField(TEXT("error")), FString(TEXT("field_schema_changed")));
		}
	}

	if (bOk)
	{
		bOk &= ExecProjectSettingsCommandExpectFailure(
			this,
			ServerScope,
			TEXT("smoke-project-settings-schema-mismatch-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"base_field_schema_hash\":%s,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(BadSchemaHash),
				*JsonString(DryRunVersion)),
			TEXT("field_schema_changed"));
	}

	TSharedPtr<FJsonObject> SchemaMismatchDryRunOnlyData;
	if (bOk)
	{
		const bool bSchemaMismatchDryRunOnlyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-schema-mismatch-dry-run-only-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"schema_mismatch_policy\":\"dry_run_only\",\"base_field_schema_hash\":%s,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(BadSchemaHash),
				*JsonString(DryRunVersion)),
			SchemaMismatchDryRunOnlyData);
		bOk &= bSchemaMismatchDryRunOnlyOk;
		if (bSchemaMismatchDryRunOnlyOk)
		{
			TestTrue(TEXT("schema mismatch dry_run_only reports mismatch"), SchemaMismatchDryRunOnlyData->GetBoolField(TEXT("schema_mismatch")));
			TestTrue(TEXT("schema mismatch dry_run_only forces dry_run"), SchemaMismatchDryRunOnlyData->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("schema mismatch dry_run_only applies no fields"), SchemaMismatchDryRunOnlyData->GetIntegerField(TEXT("applied_field_count")), 0);
		}
	}

	TSharedPtr<FJsonObject> AfterSchemaDryRunOnlyPage;
	if (bOk)
	{
		const bool bAfterSchemaDryRunOnlyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-schema-dry-run-only-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterSchemaDryRunOnlyPage);
		bOk &= bAfterSchemaDryRunOnlyOk;
		if (bAfterSchemaDryRunOnlyOk)
		{
			FString AfterSchemaDryRunOnlyVersion;
			TestTrue(TEXT("after schema dry_run_only ProjectVersion reads"), GetExportedFieldStringValue(AfterSchemaDryRunOnlyPage, TEXT("ProjectVersion"), AfterSchemaDryRunOnlyVersion));
			TestEqual(TEXT("schema dry_run_only did not change ProjectVersion"), AfterSchemaDryRunOnlyVersion, OriginalProjectVersion);
		}
	}

	const FString JsonFileProjectName = TEXT("UAI_JsonFile_Name_") + RootId.Left(8);
	const FString JsonFileVersion = TEXT("UAI_Smoke_JsonFile_") + RootId.Left(8);
	const FString JsonFileCompanyName = TEXT("UAI_JsonFile_Co_") + RootId.Left(8);
	const FString JsonFilePatch = FPaths::Combine(OutputRoot, TEXT("project_settings_json_file_apply.json"));
	if (bOk)
	{
		const FString JsonFilePatchText = FString::Printf(
			TEXT("{\"section_id\":%s,\"base_field_schema_hash\":%s,\"fields\":[{\"name\":\"ProjectName\",\"value\":%s,\"field_schema_hash\":%s},{\"name\":\"ProjectVersion\",\"value\":%s,\"field_schema_hash\":%s},{\"name\":\"CompanyName\",\"value\":%s,\"field_schema_hash\":%s}],\"options\":{\"dry_run\":false,\"read_back\":true,\"backup\":false}}"),
			*JsonString(SectionId),
			*JsonString(PageData->GetStringField(TEXT("field_schema_hash"))),
			*JsonString(JsonFileProjectName),
			*JsonString(ProjectNameFieldSchemaHash),
			*JsonString(JsonFileVersion),
			*JsonString(ProjectVersionFieldSchemaHash),
			*JsonString(JsonFileCompanyName),
			*JsonString(CompanyNameFieldSchemaHash));
		bOk &= TestTrue(
			TEXT("json_file patch saved"),
			FFileHelper::SaveStringToFile(JsonFilePatchText, *JsonFilePatch, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	TSharedPtr<FJsonObject> JsonFileValidateData;
	if (bOk)
	{
		const bool bJsonFileValidateOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-json-file-validate-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(JsonFilePatch)),
			JsonFileValidateData);
		bOk &= bJsonFileValidateOk;
		if (bJsonFileValidateOk)
		{
			TestTrue(TEXT("json_file validate returns valid=true"), JsonFileValidateData->GetBoolField(TEXT("valid")));
			TestEqual(TEXT("json_file validate applies no fields"), JsonFileValidateData->GetIntegerField(TEXT("applied_field_count")), 0);
		}
	}

	TSharedPtr<FJsonObject> JsonFileApplyData;
	if (bOk)
	{
		const bool bJsonFileApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-json-file-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(JsonFilePatch)),
			JsonFileApplyData);
		bOk &= bJsonFileApplyOk;
		if (bJsonFileApplyOk)
		{
			TestEqual(TEXT("json_file apply applied three fields"), JsonFileApplyData->GetIntegerField(TEXT("applied_field_count")), 3);
		}
	}

	TSharedPtr<FJsonObject> AfterJsonFileApplyPage;
	if (bOk)
	{
		const bool bAfterJsonFileApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-json-file-apply-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterJsonFileApplyPage);
		bOk &= bAfterJsonFileApplyOk;
		if (bAfterJsonFileApplyOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("json_file apply changed"),
				AfterJsonFileApplyPage,
				JsonFileProjectName,
				JsonFileVersion,
				JsonFileCompanyName);
		}
	}

	if (bOk)
	{
		bOk &= ExecProjectSettingsCommandExpectFailure(
			this,
			ServerScope,
			TEXT("smoke-project-settings-schema-mismatch-matching-without-fields-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"schema_mismatch_policy\":\"apply_matching_fields\",\"base_field_schema_hash\":%s,\"values\":{\"ProjectVersion\":%s}}"),
				*JsonString(SectionId),
				*JsonString(BadSchemaHash),
				*JsonString(DryRunVersion)),
			TEXT("field_schema_matching_requires_exported_fields"));
	}

	const FString MatchingProjectName = TEXT("UAI_Matching_Name_") + RootId.Left(8);
	const FString MatchingVersion = TEXT("UAI_Smoke_Matching_") + RootId.Left(8);
	const FString MatchingCompanyName = TEXT("UAI_Matching_Co_") + RootId.Left(8);
	TSharedPtr<FJsonObject> SchemaMatchingApplyData;
	if (bOk)
	{
		const bool bSchemaMatchingApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-schema-matching-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"read_back\":true,\"backup\":false,\"schema_mismatch_policy\":\"apply_matching_fields\",\"base_field_schema_hash\":%s,\"fields\":{\"ProjectName\":{\"value\":%s,\"field_schema_hash\":%s},\"ProjectVersion\":{\"value\":%s,\"field_schema_hash\":%s},\"CompanyName\":{\"value\":%s,\"field_schema_hash\":%s}}}"),
				*JsonString(SectionId),
				*JsonString(BadSchemaHash),
				*JsonString(MatchingProjectName),
				*JsonString(ProjectNameFieldSchemaHash),
				*JsonString(MatchingVersion),
				*JsonString(ProjectVersionFieldSchemaHash),
				*JsonString(MatchingCompanyName),
				*JsonString(CompanyNameFieldSchemaHash)),
			SchemaMatchingApplyData);
		bOk &= bSchemaMatchingApplyOk;
		if (bSchemaMatchingApplyOk)
		{
			TestTrue(TEXT("apply_matching_fields reports mismatch"), SchemaMatchingApplyData->GetBoolField(TEXT("schema_mismatch")));
			TestFalse(TEXT("apply_matching_fields does not force dry_run"), SchemaMatchingApplyData->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("apply_matching_fields applied three fields"), SchemaMatchingApplyData->GetIntegerField(TEXT("applied_field_count")), 3);
		}
	}

	TSharedPtr<FJsonObject> AfterSchemaMatchingApplyPage;
	if (bOk)
	{
		const bool bAfterSchemaMatchingApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-schema-matching-apply-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterSchemaMatchingApplyPage);
		bOk &= bAfterSchemaMatchingApplyOk;
		if (bAfterSchemaMatchingApplyOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("apply_matching_fields changed"),
				AfterSchemaMatchingApplyPage,
				MatchingProjectName,
				MatchingVersion,
				MatchingCompanyName);
		}
	}

	const FString AppliedProjectName = TEXT("UAI_Apply_Name_") + RootId.Left(8);
	const FString AppliedVersion = TEXT("UAI_Smoke_") + RootId.Left(8);
	const FString AppliedCompanyName = TEXT("UAI_Apply_Co_") + RootId.Left(8);
	TSharedPtr<FJsonObject> ApplyData;
	if (bOk)
	{
		const bool bApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"read_back\":true,\"backup\":true,\"values\":{\"ProjectName\":%s,\"ProjectVersion\":%s,\"CompanyName\":%s}}"),
				*JsonString(SectionId),
				*JsonString(AppliedProjectName),
				*JsonString(AppliedVersion),
				*JsonString(AppliedCompanyName)),
			ApplyData);
		bOk &= bApplyOk;
		if (bApplyOk)
		{
			TestEqual(TEXT("apply schema"), ApplyData->GetStringField(TEXT("schema")), FString(TEXT("ue_agent.project_settings.apply_result.v1")));
			TestTrue(TEXT("apply success"), ApplyData->GetBoolField(TEXT("success")));
			TestEqual(TEXT("apply applied three fields"), ApplyData->GetIntegerField(TEXT("applied_field_count")), 3);
			TestTrue(TEXT("apply has backup file"), ApplyData->HasField(TEXT("backup_file")));
			TestTrue(TEXT("apply has backup_path alias"), ApplyData->HasField(TEXT("backup_path")));
			const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			TestTrue(TEXT("apply has changes array"), ApplyData->TryGetArrayField(TEXT("changes"), Changes) && Changes && Changes->Num() >= 3);
			TestTrue(TEXT("apply has warnings array"), ApplyData->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);
			TestTrue(TEXT("apply has errors array"), ApplyData->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 0);
			TestTrue(TEXT("apply returns requires_config_reload"), ApplyData->HasField(TEXT("requires_config_reload")));
			TestTrue(TEXT("apply returns applied_immediately"), ApplyData->HasField(TEXT("applied_immediately")));
			const TSharedPtr<FJsonObject>* ProjectNameResult = FindFieldResult(ApplyData, TEXT("ProjectName"));
			TestTrue(TEXT("apply ProjectName result exists"), ProjectNameResult && ProjectNameResult->IsValid());
			if (ProjectNameResult && ProjectNameResult->IsValid())
			{
				TestEqual(TEXT("apply ProjectName status"), (*ProjectNameResult)->GetStringField(TEXT("status")), FString(TEXT("applied")));
				TestEqual(TEXT("apply ProjectName config_key"), (*ProjectNameResult)->GetStringField(TEXT("config_key")), FString(TEXT("ProjectName")));
				TestTrue(TEXT("apply ProjectName requested_value"), (*ProjectNameResult)->HasField(TEXT("requested_value")));
				TestTrue(TEXT("apply ProjectName applied_value"), (*ProjectNameResult)->HasField(TEXT("applied_value")));
				TestTrue(TEXT("apply ProjectName read_back_value"), (*ProjectNameResult)->HasField(TEXT("read_back_value")));
			}
		}
	}

	TSharedPtr<FJsonObject> AfterApplyPage;
	if (bOk)
	{
		const bool bAfterApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-apply-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterApplyPage);
		bOk &= bAfterApplyOk;
		if (bAfterApplyOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("apply changed"),
				AfterApplyPage,
				AppliedProjectName,
				AppliedVersion,
				AppliedCompanyName);
		}
	}

	TSharedPtr<FJsonObject> RestoreData;
	if (!SectionId.IsEmpty())
	{
		ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-restore-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"read_back\":true,\"backup\":false,\"values\":{\"ProjectName\":%s,\"ProjectVersion\":%s,\"CompanyName\":%s}}"),
				*JsonString(SectionId),
				*JsonString(OriginalProjectName),
				*JsonString(OriginalProjectVersion),
				*JsonString(OriginalCompanyName)),
			RestoreData);
	}

	const FString MixedInvalidVersion = TEXT("UAI_Should_Not_Persist_") + RootId.Left(8);
	if (bOk)
	{
		bOk &= ExecProjectSettingsCommandExpectFailureFieldStatus(
			this,
			ServerScope,
			TEXT("smoke-project-settings-mixed-invalid-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(
				TEXT("{\"section_id\":%s,\"dry_run\":false,\"backup\":true,\"read_back\":true,\"values\":{\"ProjectVersion\":%s,\"DefinitelyNotAProjectSetting\":true}}"),
				*JsonString(SectionId),
				*JsonString(MixedInvalidVersion)),
			TEXT("project_settings_apply_failed"),
			TEXT("DefinitelyNotAProjectSetting"),
			TEXT("field_not_found"));
	}

	TSharedPtr<FJsonObject> AfterMixedInvalidApplyPage;
	if (bOk)
	{
		const bool bAfterMixedInvalidApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-after-mixed-invalid-apply-") + RootId,
			TEXT("project_settings_export_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"write_file\":false}"), *JsonString(SectionId)),
			AfterMixedInvalidApplyPage);
		bOk &= bAfterMixedInvalidApplyOk;
		if (bAfterMixedInvalidApplyOk)
		{
			bOk &= ExpectProjectDescriptionValues(
				this,
				TEXT("mixed invalid apply did not partially write"),
				AfterMixedInvalidApplyPage,
				OriginalProjectName,
				OriginalProjectVersion,
				OriginalCompanyName);
		}
	}

	TSharedPtr<FJsonObject> AllowUnknownValidateData;
	if (bOk)
	{
		const bool bAllowUnknownValidateOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-allow-unknown-validate-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"allow_unknown_fields\":true,\"values\":{\"DefinitelyNotAProjectSetting\":true}}"), *JsonString(SectionId)),
			AllowUnknownValidateData);
		bOk &= bAllowUnknownValidateOk;
		if (bAllowUnknownValidateOk)
		{
			TestTrue(TEXT("allow unknown validate success"), AllowUnknownValidateData->GetBoolField(TEXT("success")));
			TestEqual(TEXT("allow unknown validate error_count"), AllowUnknownValidateData->GetIntegerField(TEXT("error_count")), 0);
			const TSharedPtr<FJsonObject>* UnknownResult = FindFieldResult(AllowUnknownValidateData, TEXT("DefinitelyNotAProjectSetting"));
			TestTrue(TEXT("allow unknown result exists"), UnknownResult && UnknownResult->IsValid());
			if (UnknownResult && UnknownResult->IsValid())
			{
				TestEqual(TEXT("allow unknown status"), (*UnknownResult)->GetStringField(TEXT("status")), FString(TEXT("skipped_unknown_field")));
			}
		}
	}

	if (bOk && !NonEditableFieldName.IsEmpty())
	{
		TSharedPtr<FJsonObject> UnsupportedFieldValidateData;
		const bool bUnsupportedFieldValidateOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-fail-on-unsupported-false-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"fail_on_unsupported_field\":false,\"values\":{%s:true}}"), *JsonString(SectionId), *JsonString(NonEditableFieldName)),
			UnsupportedFieldValidateData);
		bOk &= bUnsupportedFieldValidateOk;
		if (bUnsupportedFieldValidateOk)
		{
			TestTrue(TEXT("unsupported field skip validate success"), UnsupportedFieldValidateData->GetBoolField(TEXT("success")));
			const TSharedPtr<FJsonObject>* UnsupportedResult = FindFieldResult(UnsupportedFieldValidateData, NonEditableFieldName);
			TestTrue(TEXT("unsupported field skip result exists"), UnsupportedResult && UnsupportedResult->IsValid());
			if (UnsupportedResult && UnsupportedResult->IsValid())
			{
				TestEqual(TEXT("unsupported field skip status"), (*UnsupportedResult)->GetStringField(TEXT("status")), FString(TEXT("skipped_unsupported_field")));
			}
		}
	}

	TSharedPtr<FJsonObject> ValidateInvalidData;
	if (bOk)
	{
		const bool bValidateInvalidOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-invalid-validate-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"values\":{\"DefinitelyNotAProjectSetting\":true}}"), *JsonString(SectionId)),
			ValidateInvalidData);
		bOk &= bValidateInvalidOk;
		if (bValidateInvalidOk)
		{
			TestFalse(TEXT("invalid validate returns valid=false"), ValidateInvalidData->GetBoolField(TEXT("valid")));
			TestTrue(TEXT("invalid validate reports errors"), ValidateInvalidData->GetIntegerField(TEXT("error_count")) > 0);
			const TSharedPtr<FJsonObject>* InvalidFieldResult = FindFieldResult(ValidateInvalidData, TEXT("DefinitelyNotAProjectSetting"));
			TestTrue(TEXT("field_not_found result exists"), InvalidFieldResult && InvalidFieldResult->IsValid());
			if (InvalidFieldResult && InvalidFieldResult->IsValid())
			{
				TestEqual(TEXT("invalid field error code"), (*InvalidFieldResult)->GetStringField(TEXT("error")), FString(TEXT("field_not_found")));
			}
		}

		TSharedPtr<FJsonObject> ValidateInvalidTypeData;
		const bool bValidateInvalidTypeOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-project-settings-invalid-type-validate-") + RootId,
			TEXT("project_settings_validate_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"values\":{%s:{\"unexpected\":true}}}"), *JsonString(SectionId), *JsonString(InvalidTypeBoolFieldName)),
			ValidateInvalidTypeData);
		bOk &= bValidateInvalidTypeOk;
		if (bValidateInvalidTypeOk)
		{
			TestFalse(TEXT("invalid type validate returns valid=false"), ValidateInvalidTypeData->GetBoolField(TEXT("valid")));
			const TArray<TSharedPtr<FJsonValue>>* FieldResults = nullptr;
			TestTrue(TEXT("invalid type returns field_results"), ValidateInvalidTypeData->TryGetArrayField(TEXT("field_results"), FieldResults) && FieldResults && FieldResults->Num() > 0);
			if (FieldResults && FieldResults->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* FirstResult = nullptr;
				if ((*FieldResults)[0].IsValid() && (*FieldResults)[0]->TryGetObject(FirstResult) && FirstResult && FirstResult->IsValid())
				{
					TestEqual(TEXT("invalid type error code"), (*FirstResult)->GetStringField(TEXT("error")), FString(TEXT("invalid_json_value_type")));
					TestTrue(TEXT("invalid type preserves error_detail"), (*FirstResult)->HasField(TEXT("error_detail")));
				}
			}
		}

		bOk &= ExecProjectSettingsCommandExpectFailure(
			this,
			ServerScope,
			TEXT("smoke-project-settings-invalid-apply-") + RootId,
			TEXT("project_settings_apply_page"),
			FString::Printf(TEXT("{\"section_id\":%s,\"values\":{\"DefinitelyNotAProjectSetting\":true}}"), *JsonString(SectionId)),
			TEXT("project_settings_apply_failed"));
	}

	const FString SmokeSection = TEXT("UeAgentInterface.ProjectSettingsSmoke.") + RootId;
	TSharedPtr<FJsonObject> ConfigDefaultDryRunData;
	if (bOk)
	{
		const bool bConfigDefaultDryRunOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-config-default-dry-run-") + RootId,
			TEXT("config_apply_section"),
			FString::Printf(
				TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s,\"read_back\":true,\"set\":{\"SmokeKey\":%s}}"),
				*JsonString(SmokeSection),
				*JsonString(RootId)),
			ConfigDefaultDryRunData);
		bOk &= bConfigDefaultDryRunOk;
		if (bConfigDefaultDryRunOk)
		{
			TestTrue(TEXT("config default apply is dry_run"), ConfigDefaultDryRunData->GetBoolField(TEXT("dry_run")));
		}
	}

	TSharedPtr<FJsonObject> ConfigApplyData;
	if (bOk)
	{
		const bool bConfigApplyOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-config-apply-") + RootId,
			TEXT("config_apply_section"),
			FString::Printf(
				TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s,\"options\":{\"dry_run\":false,\"backup\":true,\"read_back\":true},\"set\":{\"SmokeKey\":%s,\"SmokeArray\":[\"A\",\"B\"]}}"),
				*JsonString(SmokeSection),
				*JsonString(RootId)),
			ConfigApplyData);
		bOk &= bConfigApplyOk;
		if (bConfigApplyOk)
		{
			TestEqual(TEXT("config apply schema"), ConfigApplyData->GetStringField(TEXT("schema")), FString(TEXT("ue_agent.config.apply_result.v1")));
			TestTrue(TEXT("config apply success"), ConfigApplyData->GetBoolField(TEXT("success")));
			TestEqual(TEXT("config apply source"), ConfigApplyData->GetStringField(TEXT("source")), FString(TEXT("config_file_fallback")));
			TestTrue(TEXT("config apply has source_path"), !ConfigApplyData->GetStringField(TEXT("source_path")).IsEmpty());
			TestTrue(TEXT("config apply has backup_path"), ConfigApplyData->HasField(TEXT("backup_path")));
			TestEqual(TEXT("config apply operation count"), ConfigApplyData->GetIntegerField(TEXT("operation_count")), 2);
		}
	}

	TSharedPtr<FJsonObject> ConfigExportData;
	if (bOk)
	{
		const bool bConfigExportOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-config-export-") + RootId,
			TEXT("config_export_section"),
			FString::Printf(TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s}"), *JsonString(SmokeSection)),
			ConfigExportData);
		bOk &= bConfigExportOk;
		if (bConfigExportOk)
		{
			TestEqual(TEXT("config export schema"), ConfigExportData->GetStringField(TEXT("schema")), FString(TEXT("ue_agent.config.section.v1")));
			TestEqual(TEXT("config export source"), ConfigExportData->GetStringField(TEXT("source")), FString(TEXT("config_file_fallback")));
			TestTrue(TEXT("config export has source_path"), !ConfigExportData->GetStringField(TEXT("source_path")).IsEmpty());
			const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
			TestTrue(TEXT("config export has values"), ConfigExportData->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid());
			if (ValuesObj && ValuesObj->IsValid())
			{
				TestEqual(TEXT("config SmokeKey roundtrip"), (*ValuesObj)->GetStringField(TEXT("SmokeKey")), RootId);
				const TArray<TSharedPtr<FJsonValue>>* SmokeArray = nullptr;
				TestTrue(TEXT("config SmokeArray roundtrip"), (*ValuesObj)->TryGetArrayField(TEXT("SmokeArray"), SmokeArray) && SmokeArray && SmokeArray->Num() == 2);
			}
		}
	}

	const FString MixedInvalidConfigKey = TEXT("SmokeMixedInvalidKey");
	if (bOk)
	{
		bOk &= ExecProjectSettingsCommandExpectFailureConfigResult(
			this,
			ServerScope,
			TEXT("smoke-config-mixed-invalid-apply-") + RootId,
			TEXT("config_apply_section"),
			FString::Printf(
				TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s,\"dry_run\":false,\"backup\":true,\"read_back\":true,\"set\":{%s:%s},\"remove\":[null]}"),
				*JsonString(SmokeSection),
				*JsonString(MixedInvalidConfigKey),
				*JsonString(RootId)),
			TEXT("config_apply_failed"),
			TEXT("remove[0]"),
			TEXT("invalid_config_key"));
	}

	TSharedPtr<FJsonObject> ConfigAfterMixedInvalidData;
	if (bOk)
	{
		const bool bConfigAfterMixedInvalidOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-config-after-mixed-invalid-apply-") + RootId,
			TEXT("config_export_section"),
			FString::Printf(TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s}"), *JsonString(SmokeSection)),
			ConfigAfterMixedInvalidData);
		bOk &= bConfigAfterMixedInvalidOk;
		if (bConfigAfterMixedInvalidOk)
		{
			const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
			TestTrue(TEXT("config after mixed invalid export has values"), ConfigAfterMixedInvalidData->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid());
			if (ValuesObj && ValuesObj->IsValid())
			{
				TestFalse(TEXT("config mixed invalid apply did not partially write valid set key"), (*ValuesObj)->HasField(MixedInvalidConfigKey));
			}
		}
	}

	TSharedPtr<FJsonObject> ConfigRemoveData;
	if (bOk)
	{
		const bool bConfigRemoveOk = ExecProjectSettingsCommand(
			this,
			ServerScope,
			TEXT("smoke-config-remove-") + RootId,
			TEXT("config_apply_section"),
			FString::Printf(TEXT("{\"config_file\":\"DefaultGame.ini\",\"section\":%s,\"dry_run\":false,\"backup\":false,\"read_back\":true,\"remove\":[\"SmokeKey\",\"SmokeArray\"]}"), *JsonString(SmokeSection)),
			ConfigRemoveData);
		bOk &= bConfigRemoveOk;
		if (bConfigRemoveOk)
		{
			TestEqual(TEXT("config remove operation count"), ConfigRemoveData->GetIntegerField(TEXT("operation_count")), 2);
		}
	}

	RestoreDefaultGameIni();
	return bOk && !HasAnyErrors();
}

#endif
