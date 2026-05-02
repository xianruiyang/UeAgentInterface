// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Dom/JsonObject.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UeAgentProjectSettingsOps
{
	static constexpr const TCHAR* ProjectSettingsProfile = TEXT("ue_agent_interface.project_settings.v1");
	static constexpr const TCHAR* ConfigSectionProfile = TEXT("ue_agent_interface.config_section.v1");
	static constexpr const TCHAR* ProjectSettingsIndexSchema = TEXT("ue_agent.project_settings.index.v1");
	static constexpr const TCHAR* ProjectSettingsManifestSchema = TEXT("ue_agent.project_settings.manifest.v1");
	static constexpr const TCHAR* ProjectSettingsPageSchema = TEXT("ue_agent.project_settings.page.v1");
	static constexpr const TCHAR* ProjectSettingsApplySchema = TEXT("ue_agent.project_settings.apply_result.v1");
	static constexpr const TCHAR* ProjectSettingsValidateSchema = TEXT("ue_agent.project_settings.validate_result.v1");
	static constexpr const TCHAR* ProjectSettingsDiffSchema = TEXT("ue_agent.project_settings.diff_result.v1");
	static constexpr const TCHAR* ConfigSectionSchema = TEXT("ue_agent.config.section.v1");
	static constexpr const TCHAR* ConfigApplySchema = TEXT("ue_agent.config.apply_result.v1");

	struct FPageRef
	{
		FName ContainerName;
		FName CategoryName;
		FName SectionName;
		FString SectionId;
		TSharedPtr<ISettingsContainer> Container;
		TSharedPtr<ISettingsCategory> Category;
		TSharedPtr<ISettingsSection> Section;
		UObject* SettingsObject = nullptr;
	};

	static FString NowIso()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	static void AddProtocolFields(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Schema)
	{
		if (!Obj.IsValid())
		{
			return;
		}
		Obj->SetStringField(TEXT("schema"), Schema);
		Obj->SetStringField(TEXT("generated_at"), NowIso());
	}

	static FString NameToString(const FName& Name)
	{
		return Name.IsNone() ? FString() : Name.ToString();
	}

	static FString BuildSectionId(const FName& Container, const FName& Category, const FName& Section)
	{
		return FString::Printf(TEXT("%s.%s.%s"), *NameToString(Container), *NameToString(Category), *NameToString(Section));
	}

	static FString GetPageSupportLevel(const FPageRef& Page)
	{
		if (Page.SettingsObject)
		{
			return TEXT("reflected_uobject");
		}
		if (Page.Section.IsValid() && Page.Section->HasDefaultSettingsObject())
		{
			return TEXT("settings_object_missing");
		}
		return TEXT("unsupported_custom_settings_ui");
	}

	static FString GetUnsupportedPageError(const FPageRef& Page)
	{
		return GetPageSupportLevel(Page).Equals(TEXT("settings_object_missing"), ESearchCase::IgnoreCase)
			? TEXT("settings_object_missing")
			: TEXT("unsupported_custom_settings_ui");
	}

	static FString ToSlug(const FString& Input)
	{
		FString Out;
		for (const TCHAR Ch : Input.ToLower())
		{
			if ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('_') || Ch == TEXT('-'))
			{
				Out.AppendChar(Ch);
			}
			else if (Ch == TEXT('.') || Ch == TEXT(' ') || Ch == TEXT('/'))
			{
				Out.AppendChar(TEXT('_'));
			}
		}
		while (Out.Contains(TEXT("__")))
		{
			Out.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("unnamed") : Out;
	}

	static FString GetDefaultOutputRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"), TEXT("ProjectSettings")));
	}

	static FString ResolveOutputRoot(const TSharedPtr<FJsonObject>& Params)
	{
		FString OutputRoot;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("output_directory"), OutputRoot);
			if (OutputRoot.IsEmpty())
			{
				Params->TryGetStringField(TEXT("output_dir"), OutputRoot);
			}
			if (OutputRoot.IsEmpty())
			{
				FString OutputPath;
				Params->TryGetStringField(TEXT("output_path"), OutputPath);
				OutputPath.TrimStartAndEndInline();
				if (!OutputPath.IsEmpty())
				{
					OutputRoot = FPaths::GetPath(OutputPath);
				}
			}
		}
		OutputRoot.TrimStartAndEndInline();
		if (OutputRoot.IsEmpty())
		{
			OutputRoot = GetDefaultOutputRoot();
		}
		return FPaths::ConvertRelativePathToFull(OutputRoot);
	}

	static FString ResolveOutputFileParam(const TSharedPtr<FJsonObject>& Params)
	{
		FString OutputFile;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("output_file"), OutputFile);
			if (OutputFile.IsEmpty())
			{
				Params->TryGetStringField(TEXT("output_path"), OutputFile);
			}
		}
		OutputFile.TrimStartAndEndInline();
		return OutputFile.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(OutputFile);
	}

	static void AddProjectIdentityFields(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}
		Obj->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Obj->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	}

	static FString BuildPageFolder(const FString& OutputRoot, const FPageRef& Page)
	{
		return FPaths::Combine(OutputRoot, ToSlug(NameToString(Page.ContainerName)), ToSlug(NameToString(Page.CategoryName)), ToSlug(NameToString(Page.SectionName)));
	}

	static FString HashString(const FString& Text)
	{
		return FMD5::HashAnsiString(*Text);
	}

	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Obj, FString& OutText, bool bPretty = true)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		if (bPretty)
		{
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutText);
			return FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		}
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
		return FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	}

	static bool SerializeJsonValue(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			OutText = TEXT("null");
			return true;
		}
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
		return FJsonSerializer::Serialize(Value, TEXT(""), Writer);
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutSavedPath, FString& OutError)
	{
		FString JsonText;
		if (!SerializeJsonObject(Json, JsonText, true))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}

		const FString ResolvedPath = FPaths::ConvertRelativePathToFull(FilePath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResolvedPath), true);
		if (!FFileHelper::SaveStringToFile(JsonText, *ResolvedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *ResolvedPath);
			return false;
		}
		OutSavedPath = ResolvedPath;
		return true;
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutResolvedPath, FString& OutError)
	{
		OutResolvedPath = FPaths::ConvertRelativePathToFull(FilePath);
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *OutResolvedPath))
		{
			OutError = FString::Printf(TEXT("json_file_not_found:%s"), *OutResolvedPath);
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
		{
			OutError = FString::Printf(TEXT("json_parse_failed:%s"), *OutResolvedPath);
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		SerializeJsonValue(Value, Text);
		TSharedPtr<FJsonValue> Cloned;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (FJsonSerializer::Deserialize(Reader, Cloned) && Cloned.IsValid())
		{
			return Cloned;
		}
		return MakeShared<FJsonValueNull>();
	}

	static FString ValueTypeToString(FConfigValue::EValueType Type)
	{
		switch (Type)
		{
		case FConfigValue::EValueType::Set: return TEXT("set");
		case FConfigValue::EValueType::ArrayAdd: return TEXT("array_add");
		case FConfigValue::EValueType::ArrayAddUnique: return TEXT("array_add_unique");
		case FConfigValue::EValueType::Remove: return TEXT("remove");
		case FConfigValue::EValueType::Clear: return TEXT("clear");
		case FConfigValue::EValueType::InitializeToEmpty: return TEXT("initialize_to_empty");
		case FConfigValue::EValueType::ArrayOfStructKey: return TEXT("array_of_struct_key");
		case FConfigValue::EValueType::POCArrayOfStructKey: return TEXT("per_object_config_array_of_struct_key");
		case FConfigValue::EValueType::Combined: return TEXT("combined");
		case FConfigValue::EValueType::ArrayCombined: return TEXT("array_combined");
		default: return TEXT("unknown");
		}
	}

	static TSharedPtr<FJsonObject> MakeErrorResult(const FString& FieldName, const FString& Code, const FString& Message = FString())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("field"), FieldName);
		Obj->SetStringField(TEXT("status"), Code);
		if (!Message.IsEmpty())
		{
			Obj->SetStringField(TEXT("message"), Message);
		}
		return Obj;
	}

	static bool GetSettingsModule(ISettingsModule*& OutSettingsModule, FString& OutError)
	{
		OutSettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings"));
		if (!OutSettingsModule)
		{
			OutError = TEXT("settings_module_unavailable");
			return false;
		}
		return true;
	}

	static bool IsPluginEnabledByName(const FString& PluginName);

	static void EnumeratePages(TArray<FPageRef>& OutPages, FString& OutError)
	{
		ISettingsModule* SettingsModule = nullptr;
		if (!GetSettingsModule(SettingsModule, OutError))
		{
			return;
		}

		TArray<FName> ContainerNames;
		SettingsModule->GetContainerNames(ContainerNames);
		ContainerNames.Sort([](const FName& A, const FName& B)
		{
			return A.LexicalLess(B);
		});
		for (const FName& ContainerName : ContainerNames)
		{
			TSharedPtr<ISettingsContainer> Container = SettingsModule->GetContainer(ContainerName);
			if (!Container.IsValid())
			{
				continue;
			}

			TArray<TSharedPtr<ISettingsCategory>> Categories;
			Container->GetCategories(Categories);
			Categories.Sort([](const TSharedPtr<ISettingsCategory>& A, const TSharedPtr<ISettingsCategory>& B)
			{
				return A.IsValid() && B.IsValid() ? A->GetName().LexicalLess(B->GetName()) : A.IsValid();
			});
			for (const TSharedPtr<ISettingsCategory>& Category : Categories)
			{
				if (!Category.IsValid())
				{
					continue;
				}

				TArray<TSharedPtr<ISettingsSection>> Sections;
				Category->GetSections(Sections, true);
				Sections.Sort([](const TSharedPtr<ISettingsSection>& A, const TSharedPtr<ISettingsSection>& B)
				{
					return A.IsValid() && B.IsValid() ? A->GetName().LexicalLess(B->GetName()) : A.IsValid();
				});
				for (const TSharedPtr<ISettingsSection>& Section : Sections)
				{
					if (!Section.IsValid())
					{
						continue;
					}

					FPageRef Page;
					Page.ContainerName = ContainerName;
					Page.CategoryName = Category->GetName();
					Page.SectionName = Section->GetName();
					Page.SectionId = BuildSectionId(Page.ContainerName, Page.CategoryName, Page.SectionName);
					Page.Container = Container;
					Page.Category = Category;
					Page.Section = Section;
					Page.SettingsObject = Section->GetSettingsObject().Get();
					OutPages.Add(Page);
				}
			}
		}
	}

	static bool FindPage(const TSharedPtr<FJsonObject>& Params, FPageRef& OutPage, FString& OutError)
	{
		FString SectionId;
		FString ContainerName;
		FString CategoryName;
		FString SectionName;
		FString RequiredPlugin;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("section_id"), SectionId);
			Params->TryGetStringField(TEXT("container"), ContainerName);
			Params->TryGetStringField(TEXT("category"), CategoryName);
			Params->TryGetStringField(TEXT("section"), SectionName);
			Params->TryGetStringField(TEXT("requires_plugin"), RequiredPlugin);
		}
		SectionId.TrimStartAndEndInline();
		ContainerName.TrimStartAndEndInline();
		CategoryName.TrimStartAndEndInline();
		SectionName.TrimStartAndEndInline();
		RequiredPlugin.TrimStartAndEndInline();
		if (SectionId.IsEmpty() && (ContainerName.IsEmpty() || CategoryName.IsEmpty() || SectionName.IsEmpty()))
		{
			FString JsonFile;
			if (Params.IsValid())
			{
				Params->TryGetStringField(TEXT("json_file"), JsonFile);
			}
			JsonFile.TrimStartAndEndInline();
			if (!JsonFile.IsEmpty())
			{
				TSharedPtr<FJsonObject> JsonRoot;
				FString ResolvedJsonFile;
				if (!LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
				{
					return false;
				}
				if (JsonRoot.IsValid())
				{
					JsonRoot->TryGetStringField(TEXT("section_id"), SectionId);
					JsonRoot->TryGetStringField(TEXT("container"), ContainerName);
					JsonRoot->TryGetStringField(TEXT("category"), CategoryName);
					JsonRoot->TryGetStringField(TEXT("section"), SectionName);
					JsonRoot->TryGetStringField(TEXT("requires_plugin"), RequiredPlugin);
					SectionId.TrimStartAndEndInline();
					ContainerName.TrimStartAndEndInline();
					CategoryName.TrimStartAndEndInline();
					SectionName.TrimStartAndEndInline();
					RequiredPlugin.TrimStartAndEndInline();
				}
			}
		}
		if (SectionId.IsEmpty() && (ContainerName.IsEmpty() || CategoryName.IsEmpty() || SectionName.IsEmpty()))
		{
			OutError = TEXT("missing_section_id_or_container_category_section");
			return false;
		}

		TArray<FPageRef> Pages;
		EnumeratePages(Pages, OutError);
		if (!OutError.IsEmpty())
		{
			return false;
		}

		for (const FPageRef& Page : Pages)
		{
			if (!SectionId.IsEmpty())
			{
				if (Page.SectionId.Equals(SectionId, ESearchCase::IgnoreCase))
				{
					OutPage = Page;
					return true;
				}
			}
			else if (NameToString(Page.ContainerName).Equals(ContainerName, ESearchCase::IgnoreCase)
				&& NameToString(Page.CategoryName).Equals(CategoryName, ESearchCase::IgnoreCase)
				&& NameToString(Page.SectionName).Equals(SectionName, ESearchCase::IgnoreCase))
			{
				OutPage = Page;
				return true;
			}
		}

		if (!RequiredPlugin.IsEmpty() && !IsPluginEnabledByName(RequiredPlugin))
		{
			OutError = TEXT("requires_plugin_not_enabled");
			return false;
		}

		OutError = TEXT("settings_section_not_found");
		return false;
	}

	static FString BuildSettingsRegistryHash()
	{
		TArray<FPageRef> Pages;
		FString Error;
		EnumeratePages(Pages, Error);
		if (!Error.IsEmpty())
		{
			return FString();
		}

		TSharedPtr<FJsonObject> RegistryObj = MakeShared<FJsonObject>();
		for (const FPageRef& Page : Pages)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("container"), NameToString(Page.ContainerName));
			Entry->SetStringField(TEXT("category"), NameToString(Page.CategoryName));
			Entry->SetStringField(TEXT("section"), NameToString(Page.SectionName));
			Entry->SetStringField(TEXT("settings_class"), Page.SettingsObject ? Page.SettingsObject->GetClass()->GetPathName() : FString());
			Entry->SetStringField(TEXT("support_level"), GetPageSupportLevel(Page));
			RegistryObj->SetObjectField(Page.SectionId, Entry);
		}

		FString RegistryText;
		SerializeJsonObject(RegistryObj, RegistryText, false);
		return HashString(RegistryText);
	}

	static FString GetPropertyDisplayName(FProperty* Property)
	{
		return Property ? Property->GetDisplayNameText().ToString() : FString();
	}

	static FString GetPropertyTooltip(FProperty* Property)
	{
		return Property ? Property->GetToolTipText().ToString() : FString();
	}

	static bool PropertyMetadataIsTruthy(FProperty* Property, const TCHAR* Key)
	{
		if (!Property)
		{
			return false;
		}
		FString Value = Property->GetMetaData(Key);
		Value.TrimStartAndEndInline();
		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("1"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
	}

	static bool PropertyRequiresRestart(FProperty* Property)
	{
		return PropertyMetadataIsTruthy(Property, TEXT("ConfigRestartRequired"))
			|| PropertyMetadataIsTruthy(Property, TEXT("RequiresRestart"))
			|| PropertyMetadataIsTruthy(Property, TEXT("RestartRequired"));
	}

	static bool IsEditableSettingProperty(FProperty* Property)
	{
		return Property
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient);
	}

	static FString GetJsonTypeForProperty(FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("null");
		}
		if (CastField<FBoolProperty>(Property))
		{
			return TEXT("boolean");
		}
		if (CastField<FNumericProperty>(Property))
		{
			return TEXT("number");
		}
		if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property) || CastField<FTextProperty>(Property))
		{
			return TEXT("string");
		}
		if (CastField<FEnumProperty>(Property))
		{
			return TEXT("enum");
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? TEXT("enum") : TEXT("number");
		}
		if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property))
		{
			return TEXT("array");
		}
		if (CastField<FMapProperty>(Property))
		{
			return TEXT("map");
		}
		if (CastField<FStructProperty>(Property) || CastField<FObjectPropertyBase>(Property))
		{
			return TEXT("object");
		}
		return TEXT("string");
	}

	static TArray<TSharedPtr<FJsonValue>> BuildCategoryPathArray(FProperty* Property)
	{
		TArray<TSharedPtr<FJsonValue>> CategoryPath;
		const FString Category = Property ? Property->GetMetaData(TEXT("Category")) : FString();
		if (!Category.IsEmpty())
		{
			TArray<FString> Parts;
			Category.ParseIntoArray(Parts, TEXT("|"), true);
			if (Parts.Num() == 0)
			{
				Parts.Add(Category);
			}
			for (FString Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					CategoryPath.Add(MakeShared<FJsonValueString>(Part));
				}
			}
		}
		return CategoryPath;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildPropertyFlagsArray(FProperty* Property)
	{
		TArray<TSharedPtr<FJsonValue>> Flags;
		if (!Property)
		{
			return Flags;
		}
		auto AddFlag = [&Flags, Property](EPropertyFlags Flag, const TCHAR* Name)
		{
			if (Property->HasAnyPropertyFlags(Flag))
			{
				Flags.Add(MakeShared<FJsonValueString>(Name));
			}
		};
		AddFlag(CPF_Edit, TEXT("Edit"));
		AddFlag(CPF_EditConst, TEXT("EditConst"));
		AddFlag(CPF_Config, TEXT("Config"));
		AddFlag(CPF_GlobalConfig, TEXT("GlobalConfig"));
		AddFlag(CPF_Transient, TEXT("Transient"));
		AddFlag(CPF_Deprecated, TEXT("Deprecated"));
		AddFlag(CPF_BlueprintVisible, TEXT("BlueprintVisible"));
		AddFlag(CPF_BlueprintReadOnly, TEXT("BlueprintReadOnly"));
		AddFlag(CPF_SaveGame, TEXT("SaveGame"));
		return Flags;
	}

	static void AddCommonPropertySchema(FProperty* Property, TSharedPtr<FJsonObject>& FieldObj)
	{
		FieldObj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
		FieldObj->SetStringField(TEXT("json_type"), GetJsonTypeForProperty(Property));
		FieldObj->SetStringField(TEXT("property_class"), Property ? Property->GetClass()->GetName() : FString());
		FieldObj->SetBoolField(TEXT("editable"), IsEditableSettingProperty(Property));
		FieldObj->SetBoolField(TEXT("visible"), Property && !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient));
		FieldObj->SetBoolField(TEXT("is_config"), Property && Property->HasAnyPropertyFlags(CPF_Config));
		FieldObj->SetBoolField(TEXT("is_global_config"), Property && Property->HasAnyPropertyFlags(CPF_GlobalConfig));
		FieldObj->SetBoolField(TEXT("is_transient"), Property && Property->HasAnyPropertyFlags(CPF_Transient));
		FieldObj->SetBoolField(TEXT("is_deprecated"), Property && Property->HasAnyPropertyFlags(CPF_Deprecated));
		FieldObj->SetStringField(TEXT("display_name"), GetPropertyDisplayName(Property));
		FieldObj->SetStringField(TEXT("tooltip"), GetPropertyTooltip(Property));
		FieldObj->SetStringField(TEXT("description"), GetPropertyTooltip(Property));
		FieldObj->SetStringField(TEXT("category"), Property ? Property->GetMetaData(TEXT("Category")) : FString());
		FieldObj->SetArrayField(TEXT("category_path"), BuildCategoryPathArray(Property));
		FieldObj->SetStringField(TEXT("config_key"), Property ? Property->GetName() : FString());
		FieldObj->SetArrayField(TEXT("property_flags"), BuildPropertyFlagsArray(Property));
		const bool bRequiresRestart = PropertyRequiresRestart(Property);
		const bool bRequiresConfigReload = Property && Property->HasAnyPropertyFlags(CPF_Config);
		FieldObj->SetBoolField(TEXT("requires_restart"), bRequiresRestart);
		FieldObj->SetBoolField(TEXT("requires_config_reload"), bRequiresConfigReload);
		FieldObj->SetBoolField(TEXT("requires_editor_reopen"), false);
		FieldObj->SetBoolField(TEXT("requires_rebuild"), false);
		FieldObj->SetBoolField(TEXT("requires_asset_rescan"), false);
		FieldObj->SetBoolField(TEXT("requires_navigation_rebuild"), false);
		FieldObj->SetBoolField(TEXT("requires_shader_recompile"), false);
		FieldObj->SetBoolField(TEXT("requires_plugin_enabled"), false);
		FieldObj->SetBoolField(TEXT("applied_immediately"), IsEditableSettingProperty(Property) && !bRequiresRestart && !bRequiresConfigReload);

		TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
		if (Property)
		{
			const TArray<const TCHAR*> Keys = {
				TEXT("ClampMin"), TEXT("ClampMax"), TEXT("UIMin"), TEXT("UIMax"), TEXT("Units"),
				TEXT("EditCondition"), TEXT("ConfigRestartRequired"), TEXT("AllowedClasses"),
				TEXT("DisallowedClasses"), TEXT("RelativeToGameContentDir"), TEXT("FilePathFilter")
			};
			for (const TCHAR* Key : Keys)
			{
				const FString Value = Property->GetMetaData(Key);
				if (!Value.IsEmpty())
				{
					MetaObj->SetStringField(Key, Value);
				}
			}
		}
		FieldObj->SetObjectField(TEXT("metadata"), MetaObj);
	}

	static void AddPropertyImpactFields(FProperty* Property, TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}
		const bool bRequiresRestart = PropertyRequiresRestart(Property);
		const bool bRequiresConfigReload = Property && Property->HasAnyPropertyFlags(CPF_Config);
		Obj->SetBoolField(TEXT("requires_restart"), bRequiresRestart);
		Obj->SetBoolField(TEXT("requires_config_reload"), bRequiresConfigReload);
		Obj->SetBoolField(TEXT("requires_editor_reopen"), false);
		Obj->SetBoolField(TEXT("requires_rebuild"), false);
		Obj->SetBoolField(TEXT("requires_asset_rescan"), false);
		Obj->SetBoolField(TEXT("requires_navigation_rebuild"), false);
		Obj->SetBoolField(TEXT("requires_shader_recompile"), false);
		Obj->SetBoolField(TEXT("requires_plugin_enabled"), false);
		Obj->SetBoolField(TEXT("applied_immediately"), IsEditableSettingProperty(Property) && !bRequiresRestart && !bRequiresConfigReload);
	}

	static void AccumulatePropertyImpact(
		FProperty* Property,
		bool& bOutRequiresRestart,
		bool& bOutRequiresConfigReload,
		bool& bOutRequiresEditorReopen,
		bool& bOutRequiresRebuild,
		bool& bOutRequiresAssetRescan,
		bool& bOutRequiresNavigationRebuild,
		bool& bOutRequiresShaderRecompile,
		bool& bOutRequiresPluginEnabled)
	{
		bOutRequiresRestart |= PropertyRequiresRestart(Property);
		bOutRequiresConfigReload |= Property && Property->HasAnyPropertyFlags(CPF_Config);
		bOutRequiresEditorReopen |= false;
		bOutRequiresRebuild |= false;
		bOutRequiresAssetRescan |= false;
		bOutRequiresNavigationRebuild |= false;
		bOutRequiresShaderRecompile |= false;
		bOutRequiresPluginEnabled |= false;
	}

	static TSharedPtr<FJsonValue> ExportSinglePropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner);
	static bool ImportSinglePropertyValue(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, UObject* Owner, FString& OutError);

	static TSharedPtr<FJsonValue> ExportNumericValue(FNumericProperty* NumericProperty, const void* ValuePtr)
	{
		if (NumericProperty->IsFloatingPoint())
		{
			return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}
		if (NumericProperty->IsInteger())
		{
			if (CastField<FUInt16Property>(NumericProperty) || CastField<FUInt32Property>(NumericProperty) || CastField<FUInt64Property>(NumericProperty))
			{
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetUnsignedIntPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
		}
		return MakeShared<FJsonValueNull>();
	}

	static TSharedPtr<FJsonValue> ExportArrayDimAwareValue(FProperty* Property, const void* ValuePtr, UObject* Owner)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}
		if (Property->ArrayDim <= 1)
		{
			return ExportSinglePropertyValue(Property, ValuePtr, Owner);
		}

		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			const void* ElementPtr = static_cast<const uint8*>(ValuePtr) + Property->GetElementSize() * Index;
			Values.Add(ExportSinglePropertyValue(Property, ElementPtr, Owner));
		}
		return MakeShared<FJsonValueArray>(Values);
	}

	static TSharedPtr<FJsonValue> ExportStructValue(FStructProperty* StructProperty, const void* ValuePtr, UObject* Owner)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!StructProperty || !StructProperty->Struct || !ValuePtr)
		{
			return MakeShared<FJsonValueObject>(Obj);
		}
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* InnerProperty = *It;
			if (!InnerProperty || InnerProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			const void* InnerPtr = InnerProperty->ContainerPtrToValuePtr<void>(ValuePtr);
			Obj->SetField(InnerProperty->GetName(), ExportArrayDimAwareValue(InnerProperty, InnerPtr, Owner));
		}
		return MakeShared<FJsonValueObject>(Obj);
	}

	static TSharedPtr<FJsonValue> ExportSinglePropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			if (UEnum* Enum = EnumProperty->GetEnum())
			{
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Value));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(Value));
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProperty->Enum)
			{
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(ByteProperty->GetPropertyValue(ValuePtr));
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return ExportNumericValue(NumericProperty, ValuePtr);
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (ObjectValue)
			{
				return MakeShared<FJsonValueString>(ObjectValue->GetPathName());
			}
			return MakeShared<FJsonValueNull>();
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return ExportStructValue(StructProperty, ValuePtr, Owner);
		}
		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> ArrayValues;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				ArrayValues.Add(ExportArrayDimAwareValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Owner));
			}
			return MakeShared<FJsonValueArray>(ArrayValues);
		}
		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> SetValues;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (FScriptSetHelper::FIterator It = Helper.CreateIterator(); It; ++It)
			{
				SetValues.Add(ExportArrayDimAwareValue(SetProperty->ElementProp, Helper.GetElementPtr(It), Owner));
			}
			return MakeShared<FJsonValueArray>(SetValues);
		}
		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (FScriptMapHelper::FIterator It = Helper.CreateIterator(); It; ++It)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("key"), ExportArrayDimAwareValue(MapProperty->KeyProp, Helper.GetKeyPtr(It), Owner));
				Entry->SetField(TEXT("value"), ExportArrayDimAwareValue(MapProperty->ValueProp, Helper.GetValuePtr(It), Owner));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return MakeShared<FJsonValueArray>(Entries);
		}

		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, Owner, PPF_None);
		TSharedPtr<FJsonObject> FallbackObj = MakeShared<FJsonObject>();
		FallbackObj->SetStringField(TEXT("value_text"), ValueText);
		FallbackObj->SetStringField(TEXT("encoding"), TEXT("ue_import_text"));
		return MakeShared<FJsonValueObject>(FallbackObj);
	}

	static bool JsonValueToString(const TSharedPtr<FJsonValue>& JsonValue, FString& OutValue)
	{
		if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
		{
			OutValue.Reset();
			return true;
		}
		if (JsonValue->Type == EJson::String)
		{
			OutValue = JsonValue->AsString();
			return true;
		}
		if (JsonValue->Type == EJson::Boolean)
		{
			OutValue = JsonValue->AsBool() ? TEXT("True") : TEXT("False");
			return true;
		}
		if (JsonValue->Type == EJson::Number)
		{
			OutValue = LexToString(JsonValue->AsNumber());
			return true;
		}
		return SerializeJsonValue(JsonValue, OutValue);
	}

	static bool ImportFromText(FProperty* Property, void* ValuePtr, UObject* Owner, const FString& ValueText, FString& OutError)
	{
		if (!Property || !ValuePtr)
		{
			OutError = TEXT("invalid_property");
			return false;
		}
		if (Property->ImportText_Direct(*ValueText, ValuePtr, Owner, PPF_None, GLog) == nullptr)
		{
			OutError = FString::Printf(TEXT("import_text_failed:%s"), *ValueText);
			return false;
		}
		return true;
	}

	static bool ExtractWrappedValue(const TSharedPtr<FJsonValue>& InValue, TSharedPtr<FJsonValue>& OutValue)
	{
		OutValue = InValue;
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!InValue.IsValid() || !InValue->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		const bool bLooksLikeExportedField = Obj->HasField(TEXT("name"))
			|| Obj->HasField(TEXT("cpp_type"))
			|| Obj->HasField(TEXT("property_class"))
			|| Obj->HasField(TEXT("display_name"))
			|| Obj->HasField(TEXT("field_schema_hash"))
			|| Obj->HasField(TEXT("config_key"))
			|| Obj->HasField(TEXT("value_text"));
		const TSharedPtr<FJsonValue>* WrappedValue = Obj->Values.Find(TEXT("value"));
		if (bLooksLikeExportedField && WrappedValue && WrappedValue->IsValid())
		{
			OutValue = *WrappedValue;
			return true;
		}
		if (bLooksLikeExportedField && Obj->HasField(TEXT("value_text")))
		{
			FString ValueText;
			Obj->TryGetStringField(TEXT("value_text"), ValueText);
			OutValue = MakeShared<FJsonValueString>(ValueText);
			return true;
		}
		return true;
	}

	static bool ImportNumericValue(FNumericProperty* NumericProperty, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!NumericProperty || !JsonValue.IsValid())
		{
			OutError = TEXT("invalid_numeric_value");
			return false;
		}
		if (NumericProperty->IsFloatingPoint())
		{
			double NumberValue = 0.0;
			if (JsonValue->Type == EJson::Number)
			{
				NumberValue = JsonValue->AsNumber();
			}
			else if (JsonValue->Type == EJson::String && LexTryParseString(NumberValue, *JsonValue->AsString()))
			{
			}
			else
			{
				OutError = TEXT("expected_number");
				return false;
			}
			NumericProperty->SetFloatingPointPropertyValue(ValuePtr, NumberValue);
			return true;
		}

		int64 IntValue = 0;
		if (JsonValue->Type == EJson::Number)
		{
			IntValue = static_cast<int64>(JsonValue->AsNumber());
		}
		else if (JsonValue->Type == EJson::String && LexTryParseString(IntValue, *JsonValue->AsString()))
		{
		}
		else
		{
			OutError = TEXT("expected_integer");
			return false;
		}
		NumericProperty->SetIntPropertyValue(ValuePtr, IntValue);
		return true;
	}

	static bool ImportEnumValue(UEnum* Enum, FNumericProperty* UnderlyingProperty, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!Enum || !UnderlyingProperty || !JsonValue.IsValid())
		{
			OutError = TEXT("invalid_enum_value");
			return false;
		}
		int64 EnumValue = INDEX_NONE;
		if (JsonValue->Type == EJson::String)
		{
			const FString Name = JsonValue->AsString();
			EnumValue = Enum->GetValueByNameString(Name, EGetByNameFlags::CheckAuthoredName);
			if (EnumValue == INDEX_NONE)
			{
				EnumValue = Enum->GetValueByNameString(Name);
			}
		}
		else if (JsonValue->Type == EJson::Number)
		{
			EnumValue = static_cast<int64>(JsonValue->AsNumber());
		}
		if (EnumValue == INDEX_NONE)
		{
			OutError = TEXT("unknown_enum_value");
			return false;
		}
		UnderlyingProperty->SetIntPropertyValue(ValuePtr, EnumValue);
		return true;
	}

	static bool ImportStructValue(FStructProperty* StructProperty, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, UObject* Owner, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (JsonValue.IsValid() && JsonValue->TryGetObject(ObjPtr) && ObjPtr && ObjPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
			if (Obj->HasField(TEXT("value_text")))
			{
				FString ValueText;
				Obj->TryGetStringField(TEXT("value_text"), ValueText);
				return ImportFromText(StructProperty, ValuePtr, Owner, ValueText, OutError);
			}
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				FProperty* InnerProperty = *It;
				const TSharedPtr<FJsonValue>* InnerValue = Obj->Values.Find(InnerProperty->GetName());
				if (!InnerValue || !InnerValue->IsValid())
				{
					continue;
				}
				void* InnerPtr = InnerProperty->ContainerPtrToValuePtr<void>(ValuePtr);
				if (!ImportSinglePropertyValue(InnerProperty, InnerPtr, *InnerValue, Owner, OutError))
				{
					OutError = FString::Printf(TEXT("%s.%s"), *InnerProperty->GetName(), *OutError);
					return false;
				}
			}
			return true;
		}

		FString ValueText;
		if (!JsonValueToString(JsonValue, ValueText))
		{
			OutError = TEXT("invalid_struct_json");
			return false;
		}
		return ImportFromText(StructProperty, ValuePtr, Owner, ValueText, OutError);
	}

	static bool ImportArrayDimAwareValue(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, UObject* Owner, FString& OutError)
	{
		if (!Property || !ValuePtr)
		{
			OutError = TEXT("invalid_property");
			return false;
		}
		TSharedPtr<FJsonValue> EffectiveValue;
		ExtractWrappedValue(JsonValue, EffectiveValue);
		if (Property->ArrayDim <= 1)
		{
			return ImportSinglePropertyValue(Property, ValuePtr, EffectiveValue, Owner, OutError);
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!EffectiveValue.IsValid() || !EffectiveValue->TryGetArray(Values) || !Values || Values->Num() != Property->ArrayDim)
		{
			OutError = FString::Printf(TEXT("expected_fixed_array:%d"), Property->ArrayDim);
			return false;
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			void* ElementPtr = static_cast<uint8*>(ValuePtr) + Property->GetElementSize() * Index;
			if (!ImportSinglePropertyValue(Property, ElementPtr, (*Values)[Index], Owner, OutError))
			{
				OutError = FString::Printf(TEXT("[%d].%s"), Index, *OutError);
				return false;
			}
		}
		return true;
	}

	static bool ImportSinglePropertyValue(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, UObject* Owner, FString& OutError)
	{
		if (!Property || !ValuePtr)
		{
			OutError = TEXT("invalid_property");
			return false;
		}

		TSharedPtr<FJsonValue> EffectiveValue;
		ExtractWrappedValue(JsonValue, EffectiveValue);
		if (EffectiveValue.IsValid() && EffectiveValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (EffectiveValue->TryGetObject(ObjPtr) && ObjPtr && ObjPtr->IsValid() && (*ObjPtr)->HasField(TEXT("value_text")))
			{
				FString ValueText;
				(*ObjPtr)->TryGetStringField(TEXT("value_text"), ValueText);
				return ImportFromText(Property, ValuePtr, Owner, ValueText, OutError);
			}
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			if (!EffectiveValue.IsValid())
			{
				OutError = TEXT("expected_bool");
				return false;
			}
			bool bValue = false;
			if (EffectiveValue->Type == EJson::Boolean)
			{
				bValue = EffectiveValue->AsBool();
			}
			else if (EffectiveValue->Type == EJson::String)
			{
				const FString Text = EffectiveValue->AsString();
				if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("1")))
				{
					bValue = true;
				}
				else if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("0")))
				{
					bValue = false;
				}
				else
				{
					OutError = TEXT("expected_bool");
					return false;
				}
			}
			else
			{
				OutError = TEXT("expected_bool");
				return false;
			}
			BoolProperty->SetPropertyValue(ValuePtr, bValue);
			return true;
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return ImportEnumValue(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty(), ValuePtr, EffectiveValue, OutError);
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum)
			{
				return ImportEnumValue(ByteProperty->Enum, ByteProperty, ValuePtr, EffectiveValue, OutError);
			}
			return ImportNumericValue(ByteProperty, ValuePtr, EffectiveValue, OutError);
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return ImportNumericValue(NumericProperty, ValuePtr, EffectiveValue, OutError);
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString TextValue;
			if (!JsonValueToString(EffectiveValue, TextValue))
			{
				OutError = TEXT("expected_string");
				return false;
			}
			StrProperty->SetPropertyValue(ValuePtr, TextValue);
			return true;
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString TextValue;
			if (!JsonValueToString(EffectiveValue, TextValue))
			{
				OutError = TEXT("expected_name");
				return false;
			}
			NameProperty->SetPropertyValue(ValuePtr, FName(*TextValue));
			return true;
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString TextValue;
			if (!JsonValueToString(EffectiveValue, TextValue))
			{
				OutError = TEXT("expected_text");
				return false;
			}
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(TextValue));
			return true;
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* NewObject = nullptr;
			if (EffectiveValue.IsValid() && EffectiveValue->Type != EJson::Null)
			{
				FString ObjectPath;
				if (!JsonValueToString(EffectiveValue, ObjectPath))
				{
					OutError = TEXT("expected_object_path_string");
					return false;
				}
				ObjectPath.TrimStartAndEndInline();
				if (!ObjectPath.IsEmpty())
				{
					NewObject = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *ObjectPath);
					if (!NewObject)
					{
						OutError = FString::Printf(TEXT("object_not_found:%s"), *ObjectPath);
						return false;
					}
				}
			}
			ObjectProperty->SetObjectPropertyValue(ValuePtr, NewObject);
			return true;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return ImportStructValue(StructProperty, ValuePtr, EffectiveValue, Owner, OutError);
		}
		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!EffectiveValue.IsValid() || !EffectiveValue->TryGetArray(Values) || !Values)
			{
				OutError = TEXT("expected_array");
				return false;
			}
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			Helper.EmptyAndAddValues(Values->Num());
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				if (!ImportArrayDimAwareValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), (*Values)[Index], Owner, OutError))
				{
					OutError = FString::Printf(TEXT("[%d].%s"), Index, *OutError);
					return false;
				}
			}
			return true;
		}
		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!EffectiveValue.IsValid() || !EffectiveValue->TryGetArray(Values) || !Values)
			{
				OutError = TEXT("expected_set_array");
				return false;
			}
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			Helper.EmptyElements(Values->Num());
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const int32 SetIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
				if (!ImportArrayDimAwareValue(SetProperty->ElementProp, Helper.GetElementPtr(SetIndex), (*Values)[Index], Owner, OutError))
				{
					OutError = FString::Printf(TEXT("[%d].%s"), Index, *OutError);
					return false;
				}
			}
			Helper.Rehash();
			return true;
		}
		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!EffectiveValue.IsValid() || !EffectiveValue->TryGetArray(Entries) || !Entries)
			{
				OutError = TEXT("expected_map_entry_array");
				return false;
			}
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			Helper.EmptyValues(Entries->Num());
			for (int32 Index = 0; Index < Entries->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
				if (!(*Entries)[Index].IsValid() || !(*Entries)[Index]->TryGetObject(EntryObjPtr) || !EntryObjPtr || !EntryObjPtr->IsValid())
				{
					OutError = FString::Printf(TEXT("[%d].expected_map_entry_object"), Index);
					return false;
				}
				const TSharedPtr<FJsonValue>* KeyValue = (*EntryObjPtr)->Values.Find(TEXT("key"));
				const TSharedPtr<FJsonValue>* EntryValue = (*EntryObjPtr)->Values.Find(TEXT("value"));
				if (!KeyValue || !EntryValue)
				{
					OutError = FString::Printf(TEXT("[%d].missing_key_or_value"), Index);
					return false;
				}
				const int32 MapIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
				if (!ImportArrayDimAwareValue(MapProperty->KeyProp, Helper.GetKeyPtr(MapIndex), *KeyValue, Owner, OutError))
				{
					OutError = FString::Printf(TEXT("[%d].key.%s"), Index, *OutError);
					return false;
				}
				if (!ImportArrayDimAwareValue(MapProperty->ValueProp, Helper.GetValuePtr(MapIndex), *EntryValue, Owner, OutError))
				{
					OutError = FString::Printf(TEXT("[%d].value.%s"), Index, *OutError);
					return false;
				}
			}
			Helper.Rehash();
			return true;
		}

		FString ValueText;
		if (!JsonValueToString(EffectiveValue, ValueText))
		{
			OutError = TEXT("unsupported_property_json_value");
			return false;
		}
		return ImportFromText(Property, ValuePtr, Owner, ValueText, OutError);
	}

	static void FillEnumOptions(FProperty* Property, TSharedPtr<FJsonObject>& FieldObj)
	{
		UEnum* Enum = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
		}
		if (!Enum)
		{
			return;
		}

		FieldObj->SetStringField(TEXT("enum_path"), Enum->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Options;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();
			Option->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
			Option->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
			Option->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
			Options.Add(MakeShared<FJsonValueObject>(Option));
		}
		FieldObj->SetArrayField(TEXT("enum_options"), Options);
	}

	static TSharedPtr<FJsonObject> BuildFieldObject(FProperty* Property, void* ValuePtr, UObject* Owner)
	{
		TSharedPtr<FJsonObject> FieldObj = MakeShared<FJsonObject>();
		FieldObj->SetStringField(TEXT("name"), Property->GetName());
		AddCommonPropertySchema(Property, FieldObj);
		FillEnumOptions(Property, FieldObj);

		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, Owner, PPF_None);
		FieldObj->SetStringField(TEXT("value_text"), ValueText);
		FieldObj->SetField(TEXT("value"), ExportArrayDimAwareValue(Property, ValuePtr, Owner));
		FieldObj->SetStringField(TEXT("config_source"), Owner ? Owner->GetDefaultConfigFilename() : FString());
		if (Owner && Owner->GetClass())
		{
			if (UObject* DefaultObject = Owner->GetClass()->GetDefaultObject(false))
			{
				const void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultObject);
				FieldObj->SetField(TEXT("default_value"), ExportArrayDimAwareValue(Property, DefaultValuePtr, DefaultObject));
			}
		}
		if (!FieldObj->HasField(TEXT("default_value")))
		{
			FieldObj->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
		}

		FString SchemaText;
		TSharedPtr<FJsonObject> SchemaOnly = MakeShared<FJsonObject>();
		AddCommonPropertySchema(Property, SchemaOnly);
		FillEnumOptions(Property, SchemaOnly);
		SerializeJsonObject(SchemaOnly, SchemaText, false);
		FieldObj->SetStringField(TEXT("field_schema_hash"), HashString(SchemaText));
		FieldObj->SetStringField(TEXT("apply_support"), IsEditableSettingProperty(Property) ? TEXT("json_or_import_text") : TEXT("read_only"));
		return FieldObj;
	}

	static FString GetSettingsSourceModule(UObject* SettingsObject)
	{
		if (!SettingsObject || !SettingsObject->GetClass())
		{
			return FString();
		}
		FString PackageName = SettingsObject->GetClass()->GetOutermost() ? SettingsObject->GetClass()->GetOutermost()->GetName() : FString();
		if (PackageName.RemoveFromStart(TEXT("/Script/")))
		{
			return PackageName;
		}
		return PackageName;
	}

	static FString FindPluginNameForModule(const FString& ModuleName)
	{
		if (ModuleName.IsEmpty())
		{
			return FString();
		}
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
			{
				if (Module.Name.ToString().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					return Plugin->GetName();
				}
			}
		}
		return FString();
	}

	static bool IsPluginEnabledByName(const FString& PluginName)
	{
		if (PluginName.IsEmpty())
		{
			return false;
		}
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			if (Plugin->GetName().Equals(PluginName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsHighRiskSetting(const FPageRef& Page, FProperty* Property)
	{
		FString Text = Page.SectionId;
		Text += TEXT(" ");
		Text += NameToString(Page.CategoryName);
		Text += TEXT(" ");
		Text += NameToString(Page.SectionName);
		if (Property)
		{
			Text += TEXT(" ");
			Text += Property->GetName();
			Text += TEXT(" ");
			Text += Property->GetMetaData(TEXT("Category"));
		}
		const TCHAR* Tokens[] = {
			TEXT("RHI"), TEXT("Rendering"), TEXT("TargetHardware"), TEXT("Packaging"), TEXT("Signing"),
			TEXT("Encryption"), TEXT("Collision"), TEXT("Input"), TEXT("Navigation"), TEXT("WorldPartition"),
			TEXT("Streaming"), TEXT("AssetManager"), TEXT("Plugins"), TEXT("Platforms")
		};
		for (const TCHAR* Token : Tokens)
		{
			if (Text.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static FString GetSettingsConfigSection(UObject* SettingsObject)
	{
		return SettingsObject && SettingsObject->GetClass() ? SettingsObject->GetClass()->GetPathName() : FString();
	}

	static TSharedPtr<FJsonObject> BuildSettingsRawConfigJson(UObject* SettingsObject)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString ConfigFilename = SettingsObject ? SettingsObject->GetDefaultConfigFilename() : FString();
		const FString ConfigSection = GetSettingsConfigSection(SettingsObject);
		Root->SetStringField(TEXT("file"), ConfigFilename);
		Root->SetStringField(TEXT("section"), ConfigSection);

		if (GConfig && !ConfigFilename.IsEmpty())
		{
			GConfig->LoadFile(ConfigFilename);
		}

		TArray<TSharedPtr<FJsonValue>> Entries;
		TMap<FString, TArray<FString>> ValuesByKey;
		const FConfigSection* Section = GConfig && !ConfigFilename.IsEmpty() && !ConfigSection.IsEmpty()
			? GConfig->GetSection(*ConfigSection, false, ConfigFilename)
			: nullptr;
		if (Section)
		{
			for (FConfigSection::TConstIterator It(*Section); It; ++It)
			{
				const FString Key = It.Key().ToString();
				const FString Value = It.Value().GetSavedValue();
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("key"), Key);
				Entry->SetStringField(TEXT("value"), Value);
				Entry->SetStringField(TEXT("value_type"), ValueTypeToString(It.Value().ValueType));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
				ValuesByKey.FindOrAdd(Key).Add(Value);
			}
		}

		TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		for (const TPair<FString, TArray<FString>>& Pair : ValuesByKey)
		{
			if (Pair.Value.Num() == 1)
			{
				ValuesObj->SetStringField(Pair.Key, Pair.Value[0]);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> ArrayValues;
				for (const FString& Value : Pair.Value)
				{
					ArrayValues.Add(MakeShared<FJsonValueString>(Value));
				}
				ValuesObj->SetArrayField(Pair.Key, ArrayValues);
			}
		}
		Root->SetArrayField(TEXT("entries"), Entries);
		Root->SetObjectField(TEXT("values"), ValuesObj);
		Root->SetNumberField(TEXT("entry_count"), Entries.Num());
		FString ValueText;
		SerializeJsonObject(ValuesObj, ValueText, false);
		Root->SetStringField(TEXT("section_value_hash"), HashString(ValueText));
		return Root;
	}

	struct FPageFieldSummary
	{
		int32 FieldCount = 0;
		int32 WritableFieldCount = 0;
		FString FieldSchemaHash;
		FString PageValueHash;
		bool bRequiresRestartHint = false;
		bool bRequiresConfigReloadHint = false;
	};

	static TSharedPtr<FJsonObject> BuildPluginFingerprint();

	static FPageFieldSummary BuildPageFieldSummary(const FPageRef& Page)
	{
		FPageFieldSummary Summary;
		TSharedPtr<FJsonObject> SchemaHashSource = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		if (Page.SettingsObject)
		{
			for (TFieldIterator<FProperty> It(Page.SettingsObject->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
				{
					continue;
				}
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
				TSharedPtr<FJsonObject> FieldObj = BuildFieldObject(Property, ValuePtr, Page.SettingsObject);
				++Summary.FieldCount;
				if (IsEditableSettingProperty(Property))
				{
					++Summary.WritableFieldCount;
				}
				Summary.bRequiresRestartHint |= PropertyRequiresRestart(Property);
				Summary.bRequiresConfigReloadHint |= Property->HasAnyPropertyFlags(CPF_Config);
				SchemaHashSource->SetStringField(Property->GetName(), FieldObj->GetStringField(TEXT("field_schema_hash")));
				ValuesObj->SetField(Property->GetName(), FieldObj->Values.FindRef(TEXT("value")));
			}
		}
		FString SchemaText;
		SerializeJsonObject(SchemaHashSource, SchemaText, false);
		Summary.FieldSchemaHash = HashString(SchemaText);
		FString ValueText;
		SerializeJsonObject(ValuesObj, ValueText, false);
		Summary.PageValueHash = HashString(ValueText);
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildPageJson(
		const FPageRef& Page,
		const FString& OutputRoot,
		bool bIncludeNonEditable,
		bool bIncludeRawConfig = true,
		bool bIncludeDefaults = true,
		bool bIncludeMetadata = true)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("profile"), ProjectSettingsProfile);
		Root->SetStringField(TEXT("generated_at_utc"), NowIso());
		AddProtocolFields(Root, ProjectSettingsPageSchema);
		AddProjectIdentityFields(Root);
		Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Root->SetStringField(TEXT("section_id"), Page.SectionId);
		Root->SetStringField(TEXT("container"), NameToString(Page.ContainerName));
		Root->SetStringField(TEXT("category"), NameToString(Page.CategoryName));
		Root->SetStringField(TEXT("section"), NameToString(Page.SectionName));
		Root->SetStringField(TEXT("container_slug"), ToSlug(NameToString(Page.ContainerName)));
		Root->SetStringField(TEXT("category_slug"), ToSlug(NameToString(Page.CategoryName)));
		Root->SetStringField(TEXT("section_slug"), ToSlug(NameToString(Page.SectionName)));
		Root->SetStringField(TEXT("folder_path"), FPaths::Combine(ToSlug(NameToString(Page.ContainerName)), ToSlug(NameToString(Page.CategoryName)), ToSlug(NameToString(Page.SectionName))));
		Root->SetStringField(TEXT("display_name"), Page.Section.IsValid() ? Page.Section->GetDisplayName().ToString() : FString());
		Root->SetStringField(TEXT("description"), Page.Section.IsValid() ? Page.Section->GetDescription().ToString() : FString());
		Root->SetBoolField(TEXT("supported"), Page.SettingsObject != nullptr);
		Root->SetStringField(TEXT("support_level"), GetPageSupportLevel(Page));

		TSharedPtr<FJsonObject> SectionInfo = MakeShared<FJsonObject>();
		SectionInfo->SetStringField(TEXT("id"), Page.SectionId);
		SectionInfo->SetStringField(TEXT("section_id"), Page.SectionId);
		SectionInfo->SetStringField(TEXT("container"), NameToString(Page.ContainerName));
		SectionInfo->SetStringField(TEXT("category"), NameToString(Page.CategoryName));
		SectionInfo->SetStringField(TEXT("section"), NameToString(Page.SectionName));
		SectionInfo->SetStringField(TEXT("container_slug"), ToSlug(NameToString(Page.ContainerName)));
		SectionInfo->SetStringField(TEXT("category_slug"), ToSlug(NameToString(Page.CategoryName)));
		SectionInfo->SetStringField(TEXT("section_slug"), ToSlug(NameToString(Page.SectionName)));
		SectionInfo->SetStringField(TEXT("folder_path"), FPaths::Combine(ToSlug(NameToString(Page.ContainerName)), ToSlug(NameToString(Page.CategoryName)), ToSlug(NameToString(Page.SectionName))));
		SectionInfo->SetStringField(TEXT("display_name"), Page.Section.IsValid() ? Page.Section->GetDisplayName().ToString() : FString());
		SectionInfo->SetStringField(TEXT("description"), Page.Section.IsValid() ? Page.Section->GetDescription().ToString() : FString());
		SectionInfo->SetStringField(TEXT("settings_class"), Page.SettingsObject && Page.SettingsObject->GetClass() ? Page.SettingsObject->GetClass()->GetPathName() : FString());
		SectionInfo->SetStringField(TEXT("settings_object_path"), Page.SettingsObject ? Page.SettingsObject->GetPathName() : FString());
		SectionInfo->SetStringField(TEXT("config_file"), Page.SettingsObject ? Page.SettingsObject->GetDefaultConfigFilename() : FString());
		SectionInfo->SetStringField(TEXT("config_section"), GetSettingsConfigSection(Page.SettingsObject));
		SectionInfo->SetStringField(TEXT("support_level"), GetPageSupportLevel(Page));
		Root->SetObjectField(TEXT("section_info"), SectionInfo);

		const FString SourceModule = GetSettingsSourceModule(Page.SettingsObject);
		const FString SourcePlugin = FindPluginNameForModule(SourceModule);
		Root->SetStringField(TEXT("source_module"), SourceModule);
		if (SourcePlugin.IsEmpty())
		{
			Root->SetField(TEXT("source_plugin"), MakeShared<FJsonValueNull>());
			Root->SetField(TEXT("requires_plugin"), MakeShared<FJsonValueNull>());
			Root->SetBoolField(TEXT("plugin_enabled"), false);
		}
		else
		{
			Root->SetStringField(TEXT("source_plugin"), SourcePlugin);
			Root->SetStringField(TEXT("requires_plugin"), SourcePlugin);
			Root->SetBoolField(TEXT("plugin_enabled"), true);
		}

		TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetBoolField(TEXT("can_edit"), Page.Section.IsValid() && Page.Section->CanEdit());
		Capabilities->SetBoolField(TEXT("can_save"), Page.Section.IsValid() && Page.Section->CanSave());
		Capabilities->SetBoolField(TEXT("can_save_defaults"), Page.Section.IsValid() && Page.Section->CanSaveDefaults());
		Capabilities->SetBoolField(TEXT("can_export"), Page.Section.IsValid() && Page.Section->CanExport());
		Capabilities->SetBoolField(TEXT("can_import"), Page.Section.IsValid() && Page.Section->CanImport());
		Capabilities->SetBoolField(TEXT("has_default_settings_object"), Page.Section.IsValid() && Page.Section->HasDefaultSettingsObject());
		Root->SetObjectField(TEXT("capabilities"), Capabilities);

		TSharedPtr<FJsonObject> SettingsObjectJson = MakeShared<FJsonObject>();
		SettingsObjectJson->SetStringField(TEXT("object_path"), Page.SettingsObject ? Page.SettingsObject->GetPathName() : FString());
		SettingsObjectJson->SetStringField(TEXT("class_path"), Page.SettingsObject && Page.SettingsObject->GetClass() ? Page.SettingsObject->GetClass()->GetPathName() : FString());
		SettingsObjectJson->SetStringField(TEXT("default_config_filename"), Page.SettingsObject ? Page.SettingsObject->GetDefaultConfigFilename() : FString());
		SettingsObjectJson->SetStringField(TEXT("config_section"), GetSettingsConfigSection(Page.SettingsObject));
		Root->SetObjectField(TEXT("settings_object"), SettingsObjectJson);
		Root->SetStringField(TEXT("config_file"), Page.SettingsObject ? Page.SettingsObject->GetDefaultConfigFilename() : FString());
		Root->SetStringField(TEXT("config_section"), GetSettingsConfigSection(Page.SettingsObject));

		TSharedPtr<FJsonObject> FieldsObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> FieldsArray;
		if (Page.SettingsObject)
		{
			for (TFieldIterator<FProperty> It(Page.SettingsObject->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
				{
					continue;
				}
				if (!bIncludeNonEditable && !Property->HasAnyPropertyFlags(CPF_Edit | CPF_Config))
				{
					continue;
				}
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
				TSharedPtr<FJsonObject> FieldObj = BuildFieldObject(Property, ValuePtr, Page.SettingsObject);
				if (!bIncludeDefaults)
				{
					FieldObj->RemoveField(TEXT("default_value"));
				}
				if (!bIncludeMetadata)
				{
					FieldObj->RemoveField(TEXT("metadata"));
				}
				FieldsObj->SetObjectField(Property->GetName(), FieldObj);
				ValuesObj->SetField(Property->GetName(), FieldObj->Values.FindRef(TEXT("value")));
				FieldsArray.Add(MakeShared<FJsonValueObject>(FieldObj));
			}
		}
		Root->SetObjectField(TEXT("fields"), FieldsObj);
		Root->SetObjectField(TEXT("values"), ValuesObj);
		Root->SetArrayField(TEXT("fields_array"), FieldsArray);
		Root->SetNumberField(TEXT("field_count"), FieldsArray.Num());

		FString SchemaText;
		TSharedPtr<FJsonObject> SchemaHashSource = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FieldsObj->Values)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}
			SchemaHashSource->SetStringField(Pair.Key, (*FieldObj)->GetStringField(TEXT("field_schema_hash")));
		}
		SerializeJsonObject(SchemaHashSource, SchemaText, false);
		Root->SetStringField(TEXT("field_schema_hash"), HashString(SchemaText));

		FString ValueText;
		SerializeJsonObject(ValuesObj, ValueText, false);
		Root->SetStringField(TEXT("page_value_hash"), HashString(ValueText));
		Root->SetStringField(TEXT("settings_registry_hash"), BuildSettingsRegistryHash());
		Root->SetStringField(TEXT("default_output_file"), FPaths::Combine(BuildPageFolder(OutputRoot, Page), TEXT("settings.json")));
		TSharedPtr<FJsonObject> PluginFingerprint = BuildPluginFingerprint();
		Root->SetObjectField(TEXT("plugin_fingerprint"), PluginFingerprint);
		Root->SetStringField(TEXT("plugin_fingerprint_hash"), PluginFingerprint->GetStringField(TEXT("enabled_plugins_hash")));
		if (bIncludeRawConfig)
		{
			Root->SetObjectField(TEXT("raw_config"), BuildSettingsRawConfigJson(Page.SettingsObject));
		}
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildSectionIndexObject(const FPageRef& Page, const FString& OutputRoot, bool bIncludeFieldCounts = true)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("section_id"), Page.SectionId);
		Obj->SetStringField(TEXT("id"), Page.SectionId);
		Obj->SetStringField(TEXT("container"), NameToString(Page.ContainerName));
		Obj->SetStringField(TEXT("category"), NameToString(Page.CategoryName));
		Obj->SetStringField(TEXT("section"), NameToString(Page.SectionName));
		Obj->SetStringField(TEXT("container_slug"), ToSlug(NameToString(Page.ContainerName)));
		Obj->SetStringField(TEXT("category_slug"), ToSlug(NameToString(Page.CategoryName)));
		Obj->SetStringField(TEXT("section_slug"), ToSlug(NameToString(Page.SectionName)));
		Obj->SetStringField(TEXT("folder_path"), FPaths::Combine(ToSlug(NameToString(Page.ContainerName)), ToSlug(NameToString(Page.CategoryName)), ToSlug(NameToString(Page.SectionName))));
		Obj->SetStringField(TEXT("display_name"), Page.Section.IsValid() ? Page.Section->GetDisplayName().ToString() : FString());
		Obj->SetStringField(TEXT("description"), Page.Section.IsValid() ? Page.Section->GetDescription().ToString() : FString());
		Obj->SetBoolField(TEXT("supported"), Page.SettingsObject != nullptr);
		Obj->SetStringField(TEXT("support_level"), GetPageSupportLevel(Page));
		Obj->SetStringField(TEXT("settings_class"), Page.SettingsObject && Page.SettingsObject->GetClass() ? Page.SettingsObject->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("settings_object_class"), Page.SettingsObject && Page.SettingsObject->GetClass() ? Page.SettingsObject->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("settings_object_path"), Page.SettingsObject ? Page.SettingsObject->GetPathName() : FString());
		Obj->SetStringField(TEXT("config_file"), Page.SettingsObject ? Page.SettingsObject->GetDefaultConfigFilename() : FString());
		Obj->SetStringField(TEXT("config_section"), GetSettingsConfigSection(Page.SettingsObject));
		const FString SourceModule = GetSettingsSourceModule(Page.SettingsObject);
		const FString SourcePlugin = FindPluginNameForModule(SourceModule);
		Obj->SetStringField(TEXT("source_module"), SourceModule);
		if (SourcePlugin.IsEmpty())
		{
			Obj->SetField(TEXT("source_plugin"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("requires_plugin"), MakeShared<FJsonValueNull>());
			Obj->SetBoolField(TEXT("plugin_enabled"), false);
		}
		else
		{
			Obj->SetStringField(TEXT("source_plugin"), SourcePlugin);
			Obj->SetStringField(TEXT("requires_plugin"), SourcePlugin);
			Obj->SetBoolField(TEXT("plugin_enabled"), true);
		}
		Obj->SetStringField(TEXT("relative_file"), FPaths::Combine(ToSlug(NameToString(Page.ContainerName)), ToSlug(NameToString(Page.CategoryName)), ToSlug(NameToString(Page.SectionName)), TEXT("settings.json")));
		Obj->SetStringField(TEXT("output_file"), FPaths::Combine(BuildPageFolder(OutputRoot, Page), TEXT("settings.json")));
		Obj->SetBoolField(TEXT("can_edit"), Page.Section.IsValid() && Page.Section->CanEdit());
		Obj->SetBoolField(TEXT("can_save"), Page.Section.IsValid() && Page.Section->CanSave());
		const FPageFieldSummary Summary = BuildPageFieldSummary(Page);
		if (bIncludeFieldCounts)
		{
			Obj->SetNumberField(TEXT("field_count"), Summary.FieldCount);
			Obj->SetNumberField(TEXT("writable_field_count"), Summary.WritableFieldCount);
		}
		Obj->SetStringField(TEXT("field_schema_hash"), Summary.FieldSchemaHash);
		Obj->SetStringField(TEXT("page_value_hash"), Summary.PageValueHash);
		Obj->SetBoolField(TEXT("requires_restart_hint"), Summary.bRequiresRestartHint);
		Obj->SetBoolField(TEXT("requires_config_reload_hint"), Summary.bRequiresConfigReloadHint);
		return Obj;
	}

	static bool IsPluginSettingsPage(const FPageRef& Page)
	{
		const FString SourceModule = GetSettingsSourceModule(Page.SettingsObject);
		return !FindPluginNameForModule(SourceModule).IsEmpty()
			|| NameToString(Page.CategoryName).Equals(TEXT("Plugins"), ESearchCase::IgnoreCase);
	}

	static bool IsDeveloperSettingsPage(const FPageRef& Page)
	{
		return NameToString(Page.ContainerName).Contains(TEXT("Developer"), ESearchCase::IgnoreCase)
			|| NameToString(Page.CategoryName).Contains(TEXT("Developer"), ESearchCase::IgnoreCase)
			|| NameToString(Page.SectionName).Contains(TEXT("Developer"), ESearchCase::IgnoreCase);
	}

	static bool IsReadOnlySettingsPage(const FPageRef& Page)
	{
		return !Page.Section.IsValid() || !Page.Section->CanEdit();
	}

	static TSharedPtr<FJsonObject> BuildPluginFingerprint()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Plugins;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
			PluginObj->SetStringField(TEXT("name"), Plugin->GetName());
			PluginObj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
			PluginObj->SetNumberField(TEXT("version"), Plugin->GetDescriptor().Version);
			Plugins.Add(MakeShared<FJsonValueObject>(PluginObj));
		}
		Plugins.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			const FString AName = A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid() ? (*AObj)->GetStringField(TEXT("name")) : FString();
			const FString BName = B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid() ? (*BObj)->GetStringField(TEXT("name")) : FString();
			return AName < BName;
		});
		Root->SetArrayField(TEXT("enabled_plugins"), Plugins);
		FString Text;
		SerializeJsonObject(Root, Text, false);
		Root->SetStringField(TEXT("enabled_plugins_hash"), HashString(Text));
		return Root;
	}

	static bool TryNormalizeFieldsArray(
		const TSharedPtr<FJsonObject>& Source,
		TSharedPtr<FJsonObject>& OutFields,
		FString& OutError)
	{
		OutFields.Reset();
		if (!Source.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* FieldArray = nullptr;
		if (!Source->TryGetArrayField(TEXT("fields"), FieldArray) || !FieldArray)
		{
			return false;
		}

		TSharedPtr<FJsonObject> FieldsByName = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& FieldValue : *FieldArray)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				OutError = TEXT("invalid_fields_array_item");
				return false;
			}
			FString FieldName;
			(*FieldObj)->TryGetStringField(TEXT("name"), FieldName);
			FieldName.TrimStartAndEndInline();
			if (FieldName.IsEmpty())
			{
				OutError = TEXT("invalid_fields_array_missing_name");
				return false;
			}
			FieldsByName->SetObjectField(FieldName, *FieldObj);
		}

		Source->SetObjectField(TEXT("fields_by_name"), FieldsByName);
		OutFields = FieldsByName;
		return true;
	}

	static bool ExtractPatchObject(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutPatch,
		FString& OutJsonFile,
		FString& OutResolvedJsonFile,
		TSharedPtr<FJsonObject>& OutJsonRoot,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> JsonRoot;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("json_file"), OutJsonFile);
		}
		OutJsonFile.TrimStartAndEndInline();
		if (!OutJsonFile.IsEmpty())
		{
			if (!LoadJsonFile(OutJsonFile, JsonRoot, OutResolvedJsonFile, OutError))
			{
				return false;
			}
		}
		OutJsonRoot = JsonRoot;

		const TSharedPtr<FJsonObject>* PatchObj = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("patch"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("values"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("fields"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (TryNormalizeFieldsArray(Params, OutPatch, OutError))
		{
			return true;
		}
		if (!OutError.IsEmpty())
		{
			return false;
		}
		if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("values"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("patch"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("fields"), PatchObj) && PatchObj && PatchObj->IsValid())
		{
			OutPatch = *PatchObj;
			return true;
		}
		if (TryNormalizeFieldsArray(JsonRoot, OutPatch, OutError))
		{
			return true;
		}
		if (!OutError.IsEmpty())
		{
			return false;
		}

		OutError = TEXT("missing_patch_or_values");
		return false;
	}

	static FString GetOptionalStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(FieldName, Value);
		}
		Value.TrimStartAndEndInline();
		return Value;
	}

	static const TSharedPtr<FJsonObject>* GetOptionalObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* OutObj = nullptr;
		if (Obj.IsValid())
		{
			Obj->TryGetObjectField(FieldName, OutObj);
		}
		return OutObj;
	}

	static bool TryGetOptionBool(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& JsonRoot,
		const TCHAR* FieldName,
		bool& InOutValue)
	{
		auto TryFromObject = [FieldName, &InOutValue](const TSharedPtr<FJsonObject>& Obj) -> bool
		{
			return Obj.IsValid() && Obj->HasField(FieldName) && Obj->TryGetBoolField(FieldName, InOutValue);
		};

		if (TryFromObject(Params))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* ParamsOptions = GetOptionalObjectField(Params, TEXT("options"));
		if (ParamsOptions && TryFromObject(*ParamsOptions))
		{
			return true;
		}
		if (TryFromObject(JsonRoot))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* JsonOptions = GetOptionalObjectField(JsonRoot, TEXT("options"));
		if (JsonOptions && TryFromObject(*JsonOptions))
		{
			return true;
		}
		return false;
	}

	static bool TryGetOptionString(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& JsonRoot,
		const TCHAR* FieldName,
		FString& OutValue)
	{
		auto TryFromObject = [FieldName, &OutValue](const TSharedPtr<FJsonObject>& Obj) -> bool
		{
			if (!Obj.IsValid() || !Obj->HasField(FieldName))
			{
				return false;
			}
			if (!Obj->TryGetStringField(FieldName, OutValue))
			{
				return false;
			}
			OutValue.TrimStartAndEndInline();
			return true;
		};

		if (TryFromObject(Params))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* ParamsOptions = GetOptionalObjectField(Params, TEXT("options"));
		if (ParamsOptions && TryFromObject(*ParamsOptions))
		{
			return true;
		}
		if (TryFromObject(JsonRoot))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* JsonOptions = GetOptionalObjectField(JsonRoot, TEXT("options"));
		if (JsonOptions && TryFromObject(*JsonOptions))
		{
			return true;
		}
		return false;
	}

	static const TSharedPtr<FJsonObject>* GetFieldsObjectForSchemaMatching(
		const TSharedPtr<FJsonObject>& JsonRoot,
		const TSharedPtr<FJsonObject>& Params)
	{
		const TSharedPtr<FJsonObject>* FieldsObj = GetOptionalObjectField(JsonRoot, TEXT("fields"));
		if (!FieldsObj || !FieldsObj->IsValid())
		{
			FieldsObj = GetOptionalObjectField(JsonRoot, TEXT("fields_by_name"));
		}
		if (!FieldsObj || !FieldsObj->IsValid())
		{
			FieldsObj = GetOptionalObjectField(Params, TEXT("fields"));
		}
		if (!FieldsObj || !FieldsObj->IsValid())
		{
			FieldsObj = GetOptionalObjectField(Params, TEXT("fields_by_name"));
		}
		return FieldsObj;
	}

	static void FillSchemaMismatchDetails(
		const TSharedPtr<FJsonObject>& OutData,
		const TSharedPtr<FJsonObject>& CurrentPageJson,
		const TSharedPtr<FJsonObject>& InputJsonRoot,
		const FString& InputFieldSchemaHash,
		const FString& CurrentFieldSchemaHash,
		const FString& SchemaPolicy)
	{
		OutData->SetBoolField(TEXT("schema_mismatch"), true);
		OutData->SetStringField(TEXT("schema_mismatch_policy"), SchemaPolicy);
		OutData->SetStringField(TEXT("error"), TEXT("field_schema_changed"));
		OutData->SetStringField(TEXT("base_field_schema_hash"), InputFieldSchemaHash);
		OutData->SetStringField(TEXT("current_field_schema_hash"), CurrentFieldSchemaHash);
		OutData->SetStringField(TEXT("recommendation"), TEXT("Re-run project_settings_export_page and re-apply changes to the new JSON."));

		TArray<TSharedPtr<FJsonValue>> MissingFields;
		TArray<TSharedPtr<FJsonValue>> NewFields;
		TArray<TSharedPtr<FJsonValue>> ChangedFields;
		const TSharedPtr<FJsonObject>* InputFields = GetOptionalObjectField(InputJsonRoot, TEXT("fields"));
		const TSharedPtr<FJsonObject>* CurrentFields = GetOptionalObjectField(CurrentPageJson, TEXT("fields"));
		if (InputFields && InputFields->IsValid() && CurrentFields && CurrentFields->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*InputFields)->Values)
			{
				if (!(*CurrentFields)->HasField(Pair.Key))
				{
					MissingFields.Add(MakeShared<FJsonValueString>(Pair.Key));
					continue;
				}
				const TSharedPtr<FJsonObject>* InputFieldObj = nullptr;
				const TSharedPtr<FJsonObject>* CurrentFieldObj = nullptr;
				if (Pair.Value.IsValid()
					&& Pair.Value->TryGetObject(InputFieldObj)
					&& InputFieldObj
					&& InputFieldObj->IsValid()
					&& (*CurrentFields)->TryGetObjectField(Pair.Key, CurrentFieldObj)
					&& CurrentFieldObj
					&& CurrentFieldObj->IsValid())
				{
					const FString InputFieldHash = GetOptionalStringField(*InputFieldObj, TEXT("field_schema_hash"));
					const FString CurrentFieldHash = GetOptionalStringField(*CurrentFieldObj, TEXT("field_schema_hash"));
					if (!InputFieldHash.IsEmpty() && !CurrentFieldHash.IsEmpty() && !InputFieldHash.Equals(CurrentFieldHash, ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
						Changed->SetStringField(TEXT("field"), Pair.Key);
						Changed->SetStringField(TEXT("base_field_schema_hash"), InputFieldHash);
						Changed->SetStringField(TEXT("current_field_schema_hash"), CurrentFieldHash);
						ChangedFields.Add(MakeShared<FJsonValueObject>(Changed));
					}
				}
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CurrentFields)->Values)
			{
				if (!(*InputFields)->HasField(Pair.Key))
				{
					NewFields.Add(MakeShared<FJsonValueString>(Pair.Key));
				}
			}
		}
		OutData->SetArrayField(TEXT("missing_fields"), MissingFields);
		OutData->SetArrayField(TEXT("new_fields"), NewFields);
		OutData->SetArrayField(TEXT("changed_fields"), ChangedFields);
	}

	static bool ApplyOrValidatePage(
		const FPageRef& Page,
		const TSharedPtr<FJsonObject>& Params,
		bool bApply,
		bool bDiffOnly,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError,
		bool* OutReadBackRequested = nullptr)
	{
		AddProtocolFields(OutData, bDiffOnly ? ProjectSettingsDiffSchema : (bApply ? ProjectSettingsApplySchema : ProjectSettingsValidateSchema));
		AddProjectIdentityFields(OutData);
		OutData->SetStringField(TEXT("profile"), ProjectSettingsProfile);
		if (!Page.SettingsObject)
		{
			OutError = GetUnsupportedPageError(Page);
			return false;
		}
		if (bApply && Page.Section.IsValid() && !Page.Section->CanEdit())
		{
			OutError = TEXT("settings_section_cannot_edit");
			return false;
		}

		TSharedPtr<FJsonObject> PatchObj;
		FString JsonFile;
		FString ResolvedJsonFile;
		TSharedPtr<FJsonObject> JsonRoot;
		if (!ExtractPatchObject(Params, PatchObj, JsonFile, ResolvedJsonFile, JsonRoot, OutError))
		{
			return false;
		}

		FString SchemaPolicy = TEXT("fail");
		TryGetOptionString(Params, JsonRoot, TEXT("schema_mismatch_policy"), SchemaPolicy);
		SchemaPolicy.TrimStartAndEndInline();
		if (SchemaPolicy.IsEmpty())
		{
			SchemaPolicy = TEXT("fail");
		}
		if (!SchemaPolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase)
			&& !SchemaPolicy.Equals(TEXT("apply_matching_fields"), ESearchCase::IgnoreCase)
			&& !SchemaPolicy.Equals(TEXT("dry_run_only"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("invalid_schema_mismatch_policy");
			return false;
		}

		TSharedPtr<FJsonObject> CurrentPageJson = BuildPageJson(Page, ResolveOutputRoot(Params), true);
		const FString CurrentFieldSchemaHash = GetOptionalStringField(CurrentPageJson, TEXT("field_schema_hash"));
		FString InputFieldSchemaHash = GetOptionalStringField(Params, TEXT("base_field_schema_hash"));
		if (InputFieldSchemaHash.IsEmpty())
		{
			InputFieldSchemaHash = GetOptionalStringField(Params, TEXT("field_schema_hash"));
		}
		if (InputFieldSchemaHash.IsEmpty())
		{
			InputFieldSchemaHash = GetOptionalStringField(JsonRoot, TEXT("base_field_schema_hash"));
		}
		if (InputFieldSchemaHash.IsEmpty())
		{
			InputFieldSchemaHash = GetOptionalStringField(JsonRoot, TEXT("field_schema_hash"));
		}
		const bool bSchemaMismatch = !InputFieldSchemaHash.IsEmpty()
			&& !CurrentFieldSchemaHash.IsEmpty()
			&& !InputFieldSchemaHash.Equals(CurrentFieldSchemaHash, ESearchCase::IgnoreCase);
		if (bSchemaMismatch)
		{
			FillSchemaMismatchDetails(OutData, CurrentPageJson, JsonRoot, InputFieldSchemaHash, CurrentFieldSchemaHash, SchemaPolicy);
			if (SchemaPolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
			{
					OutData->SetBoolField(TEXT("dry_run"), true);
					OutData->SetBoolField(TEXT("valid"), false);
					OutData->SetBoolField(TEXT("success"), false);
					OutData->SetBoolField(TEXT("changed"), false);
					OutData->SetNumberField(TEXT("error_count"), 1);
					OutData->SetNumberField(TEXT("applied_field_count"), 0);
					OutData->SetArrayField(TEXT("field_results"), TArray<TSharedPtr<FJsonValue>>());
					TArray<TSharedPtr<FJsonValue>> Errors;
					TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
					ErrorObj->SetStringField(TEXT("error"), TEXT("field_schema_changed"));
					ErrorObj->SetStringField(TEXT("message"), TEXT("Input field schema hash does not match current reflected schema."));
					Errors.Add(MakeShared<FJsonValueObject>(ErrorObj));
					OutData->SetArrayField(TEXT("errors"), Errors);
					OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
					if (bApply)
					{
						OutError = TEXT("field_schema_changed");
						return false;
				}
				return true;
			}
			if (SchemaPolicy.Equals(TEXT("apply_matching_fields"), ESearchCase::IgnoreCase))
			{
				const TSharedPtr<FJsonObject>* InputFields = GetFieldsObjectForSchemaMatching(JsonRoot, Params);
				if (!InputFields || !InputFields->IsValid())
				{
					OutData->SetBoolField(TEXT("dry_run"), true);
					OutData->SetBoolField(TEXT("valid"), false);
					OutData->SetBoolField(TEXT("success"), false);
					OutData->SetBoolField(TEXT("changed"), false);
					OutData->SetStringField(TEXT("error"), TEXT("field_schema_matching_requires_exported_fields"));
					OutData->SetNumberField(TEXT("error_count"), 1);
					OutData->SetNumberField(TEXT("applied_field_count"), 0);
					OutData->SetArrayField(TEXT("field_results"), TArray<TSharedPtr<FJsonValue>>());
					TArray<TSharedPtr<FJsonValue>> Errors;
					TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
					ErrorObj->SetStringField(TEXT("error"), TEXT("field_schema_matching_requires_exported_fields"));
					ErrorObj->SetStringField(TEXT("message"), TEXT("apply_matching_fields requires exported fields with per-field schema hashes."));
					Errors.Add(MakeShared<FJsonValueObject>(ErrorObj));
					OutData->SetArrayField(TEXT("errors"), Errors);
					OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
					if (bApply)
					{
						OutError = TEXT("field_schema_matching_requires_exported_fields");
						return false;
					}
					return true;
				}
			}
		}

		bool bDryRun = !bApply || bDiffOnly;
		if (bApply && !bDiffOnly)
		{
			bDryRun = true;
			TryGetOptionBool(Params, JsonRoot, TEXT("dry_run"), bDryRun);
		}
		if (bSchemaMismatch && SchemaPolicy.Equals(TEXT("dry_run_only"), ESearchCase::IgnoreCase))
		{
			bDryRun = true;
		}
		bool bBackup = true;
		TryGetOptionBool(Params, JsonRoot, TEXT("backup"), bBackup);

		bool bReadBack = true;
		TryGetOptionBool(Params, JsonRoot, TEXT("read_back"), bReadBack);
		if (OutReadBackRequested)
		{
			*OutReadBackRequested = bReadBack;
		}
		bool bAllowUnknownFields = false;
		TryGetOptionBool(Params, JsonRoot, TEXT("allow_unknown_fields"), bAllowUnknownFields);
		bool bPreserveUnknownFields = false;
		TryGetOptionBool(Params, JsonRoot, TEXT("preserve_unknown_fields"), bPreserveUnknownFields);
		bAllowUnknownFields |= bPreserveUnknownFields;
		bool bFailOnUnsupportedField = true;
		TryGetOptionBool(Params, JsonRoot, TEXT("fail_on_unsupported_field"), bFailOnUnsupportedField);
		bool bFailOnRequiresRestart = false;
		TryGetOptionBool(Params, JsonRoot, TEXT("fail_on_requires_restart"), bFailOnRequiresRestart);
		bool bAllowHighRisk = false;
		TryGetOptionBool(Params, JsonRoot, TEXT("allow_high_risk"), bAllowHighRisk);

		TArray<TSharedPtr<FJsonValue>> Results;
		TArray<TSharedPtr<FJsonValue>> Diffs;
		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		int32 AppliedCount = 0;
		int32 ErrorCount = 0;
		bool bAnyChange = false;
		bool bRequiresRestart = false;
		bool bRequiresConfigReload = false;
		bool bRequiresEditorReopen = false;
		bool bRequiresRebuild = false;
		bool bRequiresAssetRescan = false;
		bool bRequiresNavigationRebuild = false;
		bool bRequiresShaderRecompile = false;
		bool bRequiresPluginEnabled = false;
		struct FPendingProjectSettingApply
		{
			FProperty* Property = nullptr;
			TArray<uint8> NewStorage;
			TArray<uint8> OldStorage;
			TSharedPtr<FJsonObject> ResultObj;
			FString ExpectedValueText;
			bool bApplied = false;
		};
		TArray<FPendingProjectSettingApply> PendingApplies;
		auto DestroyPendingApplyStorage = [](TArray<FPendingProjectSettingApply>& Items)
		{
			for (FPendingProjectSettingApply& Item : Items)
			{
				if (Item.Property)
				{
					if (Item.NewStorage.Num() > 0)
					{
						Item.Property->DestroyValue(Item.NewStorage.GetData());
					}
					if (Item.OldStorage.Num() > 0)
					{
						Item.Property->DestroyValue(Item.OldStorage.GetData());
					}
				}
			}
			Items.Empty();
		};
		auto CopySettingPropertyWithNotify = [](UObject* SettingsObject, FProperty* Property, void* DestinationPtr, const void* SourcePtr)
		{
			if (!SettingsObject || !Property || !DestinationPtr || !SourcePtr)
			{
				return;
			}
			SettingsObject->Modify();
			SettingsObject->PreEditChange(Property);
			Property->CopyCompleteValue(DestinationPtr, SourcePtr);
			FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
			SettingsObject->PostEditChangeProperty(ChangeEvent);
		};

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PatchObj->Values)
		{
			const FString FieldName = Pair.Key;
			if (bSchemaMismatch && SchemaPolicy.Equals(TEXT("apply_matching_fields"), ESearchCase::IgnoreCase))
			{
				const TSharedPtr<FJsonObject>* InputFields = GetFieldsObjectForSchemaMatching(JsonRoot, Params);
				const TSharedPtr<FJsonObject>* CurrentFields = GetOptionalObjectField(CurrentPageJson, TEXT("fields"));
				const TSharedPtr<FJsonObject>* InputFieldObj = nullptr;
				const TSharedPtr<FJsonObject>* CurrentFieldObj = nullptr;
				const bool bHasComparableFieldSchema = InputFields
					&& InputFields->IsValid()
					&& CurrentFields
					&& CurrentFields->IsValid()
					&& (*InputFields)->TryGetObjectField(FieldName, InputFieldObj)
					&& InputFieldObj
					&& InputFieldObj->IsValid()
					&& (*CurrentFields)->TryGetObjectField(FieldName, CurrentFieldObj)
					&& CurrentFieldObj
					&& CurrentFieldObj->IsValid();
				if (bHasComparableFieldSchema)
				{
					const FString InputFieldHash = GetOptionalStringField(*InputFieldObj, TEXT("field_schema_hash"));
					const FString CurrentFieldHash = GetOptionalStringField(*CurrentFieldObj, TEXT("field_schema_hash"));
					if (InputFieldHash.IsEmpty() || CurrentFieldHash.IsEmpty() || !InputFieldHash.Equals(CurrentFieldHash, ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
						ResultObj->SetStringField(TEXT("field"), FieldName);
						ResultObj->SetStringField(TEXT("status"), TEXT("skipped_schema_mismatch"));
						ResultObj->SetStringField(TEXT("base_field_schema_hash"), InputFieldHash);
						ResultObj->SetStringField(TEXT("current_field_schema_hash"), CurrentFieldHash);
						Results.Add(MakeShared<FJsonValueObject>(ResultObj));
						continue;
					}
				}
				else
				{
					TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetStringField(TEXT("field"), FieldName);
					ResultObj->SetStringField(TEXT("status"), TEXT("skipped_schema_mismatch"));
					ResultObj->SetStringField(TEXT("error"), TEXT("field_schema_unavailable"));
					Results.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
			}
			FProperty* Property = FindFProperty<FProperty>(Page.SettingsObject->GetClass(), *FieldName);
			if (!Property)
			{
				TSharedPtr<FJsonObject> ResultObj = MakeErrorResult(FieldName, bAllowUnknownFields ? TEXT("skipped_unknown_field") : TEXT("field_not_found"));
				if (bAllowUnknownFields)
				{
					ResultObj->SetStringField(TEXT("message"), TEXT("Unknown field preserved by allow_unknown_fields/preserve_unknown_fields."));
				}
				else
				{
					ResultObj->SetStringField(TEXT("error"), TEXT("field_not_found"));
					++ErrorCount;
					Errors.Add(MakeShared<FJsonValueObject>(ResultObj));
				}
				Results.Add(MakeShared<FJsonValueObject>(ResultObj));
				if (SchemaPolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				continue;
			}
			if (!IsEditableSettingProperty(Property))
			{
				TSharedPtr<FJsonObject> ResultObj = MakeErrorResult(FieldName, bFailOnUnsupportedField ? TEXT("field_not_editable") : TEXT("skipped_unsupported_field"));
				ResultObj->SetStringField(TEXT("config_key"), FieldName);
				ResultObj->SetStringField(TEXT("config_file"), Page.SettingsObject->GetDefaultConfigFilename());
				if (bFailOnUnsupportedField)
				{
					ResultObj->SetStringField(TEXT("error"), TEXT("field_not_editable"));
					++ErrorCount;
					Errors.Add(MakeShared<FJsonValueObject>(ResultObj));
				}
				Results.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
			FString OldValueText;
			Property->ExportTextItem_Direct(OldValueText, ValuePtr, nullptr, Page.SettingsObject, PPF_None);
			TSharedPtr<FJsonValue> OldValueJson = ExportArrayDimAwareValue(Property, ValuePtr, Page.SettingsObject);

			TArray<uint8> TempStorage;
			TempStorage.SetNumZeroed(Property->GetElementSize() * Property->ArrayDim);
			void* TempPtr = TempStorage.GetData();
			Property->InitializeValue(TempPtr);
			Property->CopyCompleteValue(TempPtr, ValuePtr);

			FString ImportError;
			TSharedPtr<FJsonValue> RequestedValue;
			ExtractWrappedValue(Pair.Value, RequestedValue);
			const bool bCanImport = ImportArrayDimAwareValue(Property, TempPtr, RequestedValue, Page.SettingsObject, ImportError);
			FString NewValueText;
			if (bCanImport)
			{
				Property->ExportTextItem_Direct(NewValueText, TempPtr, nullptr, Page.SettingsObject, PPF_None);
			}

			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("field"), FieldName);
			ResultObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			ResultObj->SetStringField(TEXT("config_key"), FieldName);
			ResultObj->SetStringField(TEXT("config_file"), Page.SettingsObject->GetDefaultConfigFilename());
			ResultObj->SetBoolField(TEXT("high_risk"), IsHighRiskSetting(Page, Property));
			ResultObj->SetStringField(TEXT("old_value_text"), OldValueText);
			AddPropertyImpactFields(Property, ResultObj);
			ResultObj->SetStringField(TEXT("requested_value_text"), [&RequestedValue]()
			{
				FString Text;
				JsonValueToString(RequestedValue, Text);
				return Text;
			}());
			ResultObj->SetField(TEXT("requested_value"), CloneJsonValue(RequestedValue));
			if (!bCanImport)
			{
				ResultObj->SetStringField(TEXT("status"), TEXT("import_failed"));
				const FString ErrorCode = ImportError.Equals(TEXT("unsupported_property_json_value"), ESearchCase::IgnoreCase)
					? TEXT("unsupported_property_type")
					: TEXT("invalid_json_value_type");
				ResultObj->SetStringField(TEXT("error"), ErrorCode);
				ResultObj->SetStringField(TEXT("error_detail"), ImportError);
				Results.Add(MakeShared<FJsonValueObject>(ResultObj));
				Errors.Add(MakeShared<FJsonValueObject>(ResultObj));
				Property->DestroyValue(TempPtr);
				++ErrorCount;
				continue;
			}

			const bool bChanged = OldValueText != NewValueText;
			bAnyChange |= bChanged;
			if (bChanged)
			{
				AccumulatePropertyImpact(
					Property,
					bRequiresRestart,
					bRequiresConfigReload,
					bRequiresEditorReopen,
					bRequiresRebuild,
					bRequiresAssetRescan,
					bRequiresNavigationRebuild,
					bRequiresShaderRecompile,
					bRequiresPluginEnabled);
			}
			ResultObj->SetBoolField(TEXT("changed"), bChanged);
			ResultObj->SetStringField(TEXT("new_value_text"), NewValueText);
			ResultObj->SetField(TEXT("old_value"), OldValueJson);
			ResultObj->SetField(TEXT("new_value"), ExportArrayDimAwareValue(Property, TempPtr, Page.SettingsObject));
			ResultObj->SetField(TEXT("applied_value"), ExportArrayDimAwareValue(Property, TempPtr, Page.SettingsObject));
			ResultObj->SetStringField(TEXT("status"), bDryRun ? TEXT("validated") : TEXT("pending_apply"));

			if (bChanged && bFailOnRequiresRestart && PropertyRequiresRestart(Property))
			{
				ResultObj->SetStringField(TEXT("status"), TEXT("requires_restart_blocked"));
				ResultObj->SetStringField(TEXT("error"), TEXT("requires_restart_blocked"));
				ResultObj->SetStringField(TEXT("message"), TEXT("Field requires restart and fail_on_requires_restart=true."));
				Results.Add(MakeShared<FJsonValueObject>(ResultObj));
				Errors.Add(MakeShared<FJsonValueObject>(ResultObj));
				Property->DestroyValue(TempPtr);
				++ErrorCount;
				continue;
			}
			if (bChanged && !bDryRun && !bAllowHighRisk && IsHighRiskSetting(Page, Property))
			{
				ResultObj->SetStringField(TEXT("status"), TEXT("high_risk_blocked"));
				ResultObj->SetStringField(TEXT("error"), TEXT("high_risk_blocked"));
				ResultObj->SetStringField(TEXT("message"), TEXT("High risk Project Settings writes require allow_high_risk=true."));
				Results.Add(MakeShared<FJsonValueObject>(ResultObj));
				Errors.Add(MakeShared<FJsonValueObject>(ResultObj));
				Property->DestroyValue(TempPtr);
				++ErrorCount;
				continue;
			}

			bool bQueuedForApply = false;
			if (!bDryRun)
			{
				FPendingProjectSettingApply& Pending = PendingApplies.Emplace_GetRef();
				Pending.Property = Property;
				Pending.NewStorage = MoveTemp(TempStorage);
				Pending.OldStorage.SetNumZeroed(Property->GetElementSize() * Property->ArrayDim);
				Property->InitializeValue(Pending.OldStorage.GetData());
				Property->CopyCompleteValue(Pending.OldStorage.GetData(), ValuePtr);
				Pending.ResultObj = ResultObj;
				Pending.ExpectedValueText = NewValueText;
				bQueuedForApply = true;
			}

			if (bDiffOnly || bChanged)
			{
				Diffs.Add(MakeShared<FJsonValueObject>(ResultObj));
			}
			Results.Add(MakeShared<FJsonValueObject>(ResultObj));
			if (!bQueuedForApply)
			{
				Property->DestroyValue(TempPtr);
			}
		}

		FString BackupFile;
		if (bApply && !bDryRun && ErrorCount > 0)
		{
			for (FPendingProjectSettingApply& Pending : PendingApplies)
			{
				if (Pending.ResultObj.IsValid())
				{
					Pending.ResultObj->SetStringField(TEXT("status"), TEXT("skipped_due_to_errors"));
					Pending.ResultObj->SetStringField(TEXT("message"), TEXT("No field was written because apply preflight found errors."));
				}
			}
			DestroyPendingApplyStorage(PendingApplies);
		}
		if (bApply && !bDryRun && ErrorCount == 0 && PendingApplies.Num() > 0)
		{
			if (bBackup)
			{
				const FString ConfigFilename = Page.SettingsObject->GetDefaultConfigFilename();
				if (!ConfigFilename.IsEmpty() && FPaths::FileExists(ConfigFilename))
				{
					const FString BackupRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"), TEXT("Backups"), TEXT("ProjectSettings"));
					IFileManager::Get().MakeDirectory(*BackupRoot, true);
					BackupFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						BackupRoot,
						FPaths::GetBaseFilename(ConfigFilename) + TEXT("_") + FDateTime::Now().ToString(TEXT("%Y%m%dT%H%M%S")) + TEXT(".bak")));
					if (IFileManager::Get().Copy(*BackupFile, *ConfigFilename) != 0)
					{
						DestroyPendingApplyStorage(PendingApplies);
						OutError = TEXT("config_backup_failed");
						return false;
					}
				}
			}

			for (FPendingProjectSettingApply& Pending : PendingApplies)
			{
				if (!Pending.Property || !Pending.ResultObj.IsValid())
				{
					continue;
				}
				void* ValuePtr = Pending.Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
				CopySettingPropertyWithNotify(Page.SettingsObject, Pending.Property, ValuePtr, Pending.NewStorage.GetData());
				Pending.bApplied = true;
				FString AppliedValueText;
				Pending.Property->ExportTextItem_Direct(AppliedValueText, ValuePtr, nullptr, Page.SettingsObject, PPF_None);
				Pending.ResultObj->SetStringField(TEXT("applied_value_text"), AppliedValueText);
				Pending.ResultObj->SetField(TEXT("value_read_back"), ExportArrayDimAwareValue(Pending.Property, ValuePtr, Page.SettingsObject));
				Pending.ResultObj->SetField(TEXT("read_back_value"), ExportArrayDimAwareValue(Pending.Property, ValuePtr, Page.SettingsObject));
				if (!AppliedValueText.Equals(Pending.ExpectedValueText, ESearchCase::CaseSensitive))
				{
					Pending.ResultObj->SetStringField(TEXT("status"), TEXT("read_back_mismatch"));
					Pending.ResultObj->SetStringField(TEXT("error"), TEXT("read_back_mismatch"));
					++ErrorCount;
					Errors.Add(MakeShared<FJsonValueObject>(Pending.ResultObj));
				}
				else
				{
					Pending.ResultObj->SetStringField(TEXT("status"), TEXT("applied"));
					Pending.ResultObj->SetStringField(TEXT("read_back_status"), TEXT("applied_and_read_back"));
					++AppliedCount;
				}
			}

			if (ErrorCount > 0)
			{
				for (FPendingProjectSettingApply& Pending : PendingApplies)
				{
					if (Pending.bApplied && Pending.Property)
					{
						void* ValuePtr = Pending.Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
						CopySettingPropertyWithNotify(Page.SettingsObject, Pending.Property, ValuePtr, Pending.OldStorage.GetData());
					}
				}
				AppliedCount = 0;
			}
			else
			{
				const bool bSaved = Page.Section.IsValid() ? Page.Section->Save() : Page.SettingsObject->TryUpdateDefaultConfigFile();
				if (!bSaved)
				{
					for (FPendingProjectSettingApply& Pending : PendingApplies)
					{
						if (Pending.bApplied && Pending.Property)
						{
							void* ValuePtr = Pending.Property->ContainerPtrToValuePtr<void>(Page.SettingsObject);
							CopySettingPropertyWithNotify(Page.SettingsObject, Pending.Property, ValuePtr, Pending.OldStorage.GetData());
						}
					}
					DestroyPendingApplyStorage(PendingApplies);
					OutError = TEXT("config_save_failed");
					return false;
				}
			}
			DestroyPendingApplyStorage(PendingApplies);
		}

		OutData->SetStringField(TEXT("section_id"), Page.SectionId);
		OutData->SetStringField(TEXT("object_path"), Page.SettingsObject->GetPathName());
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("valid"), ErrorCount == 0);
		OutData->SetBoolField(TEXT("success"), ErrorCount == 0);
		OutData->SetBoolField(TEXT("changed"), bAnyChange);
		OutData->SetBoolField(TEXT("requires_restart"), bRequiresRestart);
		OutData->SetBoolField(TEXT("requires_config_reload"), bRequiresConfigReload);
		OutData->SetBoolField(TEXT("requires_editor_reopen"), bRequiresEditorReopen);
		OutData->SetBoolField(TEXT("requires_rebuild"), bRequiresRebuild);
		OutData->SetBoolField(TEXT("requires_asset_rescan"), bRequiresAssetRescan);
		OutData->SetBoolField(TEXT("requires_navigation_rebuild"), bRequiresNavigationRebuild);
		OutData->SetBoolField(TEXT("requires_shader_recompile"), bRequiresShaderRecompile);
		OutData->SetBoolField(TEXT("requires_plugin_enabled"), bRequiresPluginEnabled);
		OutData->SetBoolField(TEXT("applied_immediately"), bAnyChange
			&& !bRequiresRestart
			&& !bRequiresConfigReload
			&& !bRequiresEditorReopen
			&& !bRequiresRebuild
			&& !bRequiresAssetRescan
			&& !bRequiresNavigationRebuild
			&& !bRequiresShaderRecompile
			&& !bRequiresPluginEnabled);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("applied_field_count"), AppliedCount);
		if (bSchemaMismatch)
		{
			OutData->SetBoolField(TEXT("schema_mismatch"), true);
			OutData->SetStringField(TEXT("schema_mismatch_policy"), SchemaPolicy);
			OutData->SetStringField(TEXT("base_field_schema_hash"), InputFieldSchemaHash);
			OutData->SetStringField(TEXT("current_field_schema_hash"), CurrentFieldSchemaHash);
		}
		OutData->SetArrayField(TEXT("field_results"), Results);
		OutData->SetArrayField(TEXT("changes"), Diffs);
		OutData->SetArrayField(TEXT("errors"), Errors);
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		if (bDiffOnly)
		{
			OutData->SetArrayField(TEXT("field_diffs"), Diffs);
		}
		if (!ResolvedJsonFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
		}
		if (!BackupFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("backup_file"), BackupFile);
			OutData->SetStringField(TEXT("backup_path"), BackupFile);
		}
		if (ErrorCount > 0 && bApply)
		{
			OutError = TEXT("project_settings_apply_failed");
			return false;
		}
		return true;
	}

	static FString ResolveConfigFile(const FString& InConfigFile, FString& OutError)
	{
		FString ConfigFile = InConfigFile;
		ConfigFile.TrimStartAndEndInline();
		if (ConfigFile.IsEmpty())
		{
			OutError = TEXT("missing_config_file");
			return FString();
		}

		static const TSet<FString> AllowedNames = {
			TEXT("DefaultGame.ini"),
			TEXT("DefaultEngine.ini"),
			TEXT("DefaultInput.ini"),
			TEXT("DefaultEditor.ini"),
			TEXT("DefaultEditorPerProjectUserSettings.ini")
		};

		if (!ConfigFile.EndsWith(TEXT(".ini"), ESearchCase::IgnoreCase) && !ConfigFile.Contains(TEXT("/")) && !ConfigFile.Contains(TEXT("\\")))
		{
			ConfigFile = FString::Printf(TEXT("Default%s.ini"), *ConfigFile);
		}

		FString Resolved = FPaths::IsRelative(ConfigFile)
			? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), ConfigFile))
			: FPaths::ConvertRelativePathToFull(ConfigFile);
		FPaths::NormalizeFilename(Resolved);

		const FString ProjectConfigDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir());
		FString NormalizedProjectConfigDir = ProjectConfigDir;
		FPaths::NormalizeFilename(NormalizedProjectConfigDir);
		if (!Resolved.StartsWith(NormalizedProjectConfigDir))
		{
			OutError = TEXT("config_file_outside_project_config_dir");
			return FString();
		}

		if (!AllowedNames.Contains(FPaths::GetCleanFilename(Resolved)))
		{
			OutError = TEXT("config_file_not_allowed");
			return FString();
		}
		return Resolved;
	}

	static TSharedPtr<FJsonObject> BuildConfigSectionJson(const FString& ConfigFilename, const FString& SectionName)
	{
		if (GConfig)
		{
			GConfig->LoadFile(ConfigFilename);
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("profile"), ConfigSectionProfile);
		Root->SetStringField(TEXT("generated_at_utc"), NowIso());
		AddProtocolFields(Root, ConfigSectionSchema);
		AddProjectIdentityFields(Root);
		Root->SetStringField(TEXT("config_file"), ConfigFilename);
		Root->SetStringField(TEXT("section"), SectionName);
		Root->SetStringField(TEXT("source"), TEXT("config_file_fallback"));
		Root->SetStringField(TEXT("source_path"), ConfigFilename);

		TArray<TSharedPtr<FJsonValue>> Entries;
		TMap<FString, TArray<FString>> ValuesByKey;
		const FConfigSection* Section = GConfig ? GConfig->GetSection(*SectionName, false, ConfigFilename) : nullptr;
		if (Section)
		{
			for (FConfigSection::TConstIterator It(*Section); It; ++It)
			{
				const FString Key = It.Key().ToString();
				const FString Value = It.Value().GetSavedValue();
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("key"), Key);
				Entry->SetStringField(TEXT("value"), Value);
				Entry->SetStringField(TEXT("value_type"), ValueTypeToString(It.Value().ValueType));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
				ValuesByKey.FindOrAdd(Key).Add(Value);
			}
		}

		TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		for (const TPair<FString, TArray<FString>>& Pair : ValuesByKey)
		{
			if (Pair.Value.Num() == 1)
			{
				ValuesObj->SetStringField(Pair.Key, Pair.Value[0]);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> ArrayValues;
				for (const FString& Value : Pair.Value)
				{
					ArrayValues.Add(MakeShared<FJsonValueString>(Value));
				}
				ValuesObj->SetArrayField(Pair.Key, ArrayValues);
			}
		}
		Root->SetArrayField(TEXT("entries"), Entries);
		Root->SetObjectField(TEXT("values"), ValuesObj);
		Root->SetNumberField(TEXT("entry_count"), Entries.Num());
		FString ValueText;
		SerializeJsonObject(ValuesObj, ValueText, false);
		Root->SetStringField(TEXT("section_value_hash"), HashString(ValueText));
		return Root;
	}

	static bool ConfigJsonValueToString(const TSharedPtr<FJsonValue>& JsonValue, FString& OutText)
	{
		TSharedPtr<FJsonValue> EffectiveValue;
		ExtractWrappedValue(JsonValue, EffectiveValue);
		return JsonValueToString(EffectiveValue, OutText);
	}
}

using namespace UeAgentProjectSettingsOps;

bool FUeAgentHttpServer::CmdProjectSettingsExportIndex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<FPageRef> Pages;
	EnumeratePages(Pages, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}

	const FString OutputRoot = ResolveOutputRoot(Ctx.Params);
	const FString ExplicitIndexFile = ResolveOutputFileParam(Ctx.Params);
	bool bWriteFiles = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("write_files")))
	{
		Ctx.Params->TryGetBoolField(TEXT("write_files"), bWriteFiles);
	}
	FString ContainerFilter;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("container"), ContainerFilter);
		ContainerFilter.TrimStartAndEndInline();
	}
	bool bIncludeUnsupportedSections = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_unsupported_sections")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_unsupported_sections"), bIncludeUnsupportedSections);
	}
	bool bIncludePluginSections = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_plugin_sections")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_plugin_sections"), bIncludePluginSections);
	}
	bool bIncludeDeveloperSections = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_developer_sections")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_developer_sections"), bIncludeDeveloperSections);
	}
	bool bIncludeReadonlySections = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_readonly_sections")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_readonly_sections"), bIncludeReadonlySections);
	}
	bool bIncludeFieldCounts = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_field_counts")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_field_counts"), bIncludeFieldCounts);
	}

	TArray<TSharedPtr<FJsonValue>> SectionValues;
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetNumberField(TEXT("format_version"), 1);
	Manifest->SetStringField(TEXT("profile"), ProjectSettingsProfile);
	Manifest->SetStringField(TEXT("generated_at_utc"), NowIso());
	AddProtocolFields(Manifest, ProjectSettingsManifestSchema);
	AddProjectIdentityFields(Manifest);
	TSharedPtr<FJsonObject> ManifestSections = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ManifestEntries;
	int32 SupportedCount = 0;
	int32 UnsupportedCount = 0;

	for (const FPageRef& Page : Pages)
	{
		if (!ContainerFilter.IsEmpty() && !NameToString(Page.ContainerName).Equals(ContainerFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!bIncludeUnsupportedSections && !Page.SettingsObject)
		{
			continue;
		}
		if (!bIncludePluginSections && IsPluginSettingsPage(Page))
		{
			continue;
		}
		if (!bIncludeDeveloperSections && IsDeveloperSettingsPage(Page))
		{
			continue;
		}
		if (!bIncludeReadonlySections && IsReadOnlySettingsPage(Page))
		{
			continue;
		}
		TSharedPtr<FJsonObject> SectionObj = BuildSectionIndexObject(Page, OutputRoot, bIncludeFieldCounts);
		SectionValues.Add(MakeShared<FJsonValueObject>(SectionObj));
		if (Page.SettingsObject)
		{
			++SupportedCount;
		}
		else
		{
			++UnsupportedCount;
		}
		TSharedPtr<FJsonObject> ManifestEntry = MakeShared<FJsonObject>();
		ManifestEntry->SetStringField(TEXT("section_id"), Page.SectionId);
		ManifestEntry->SetStringField(TEXT("id"), Page.SectionId);
		ManifestEntry->SetStringField(TEXT("path"), SectionObj->GetStringField(TEXT("relative_file")));
		ManifestEntry->SetStringField(TEXT("container"), NameToString(Page.ContainerName));
		ManifestEntry->SetStringField(TEXT("category"), NameToString(Page.CategoryName));
		ManifestEntry->SetStringField(TEXT("section"), NameToString(Page.SectionName));
		ManifestEntry->SetStringField(TEXT("display_name"), SectionObj->GetStringField(TEXT("display_name")));
		ManifestEntry->SetStringField(TEXT("settings_class"), SectionObj->GetStringField(TEXT("settings_class")));
		ManifestEntry->SetStringField(TEXT("relative_file"), SectionObj->GetStringField(TEXT("relative_file")));
		ManifestEntry->SetStringField(TEXT("output_file"), SectionObj->GetStringField(TEXT("output_file")));
		ManifestEntry->SetStringField(TEXT("support_level"), SectionObj->GetStringField(TEXT("support_level")));
		ManifestEntry->SetStringField(TEXT("field_schema_hash"), SectionObj->GetStringField(TEXT("field_schema_hash")));
		ManifestEntry->SetStringField(TEXT("page_value_hash"), SectionObj->GetStringField(TEXT("page_value_hash")));
		ManifestSections->SetObjectField(Page.SectionId, ManifestEntry);
		ManifestEntries.Add(MakeShared<FJsonValueObject>(ManifestEntry));
	}
	Manifest->SetObjectField(TEXT("sections"), ManifestSections);
	Manifest->SetArrayField(TEXT("entries"), ManifestEntries);

	OutData = MakeShared<FJsonObject>();
	OutData->SetNumberField(TEXT("format_version"), 1);
	OutData->SetStringField(TEXT("profile"), ProjectSettingsProfile);
	OutData->SetStringField(TEXT("generated_at_utc"), NowIso());
	AddProtocolFields(OutData, ProjectSettingsIndexSchema);
	AddProjectIdentityFields(OutData);
	OutData->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	OutData->SetStringField(TEXT("output_directory"), OutputRoot);
	if (!ContainerFilter.IsEmpty())
	{
		OutData->SetStringField(TEXT("container"), ContainerFilter);
	}
	OutData->SetArrayField(TEXT("sections"), SectionValues);
	OutData->SetNumberField(TEXT("section_count"), SectionValues.Num());
	TSharedPtr<FJsonObject> Filters = MakeShared<FJsonObject>();
	Filters->SetBoolField(TEXT("include_unsupported_sections"), bIncludeUnsupportedSections);
	Filters->SetBoolField(TEXT("include_plugin_sections"), bIncludePluginSections);
	Filters->SetBoolField(TEXT("include_developer_sections"), bIncludeDeveloperSections);
	Filters->SetBoolField(TEXT("include_readonly_sections"), bIncludeReadonlySections);
	Filters->SetBoolField(TEXT("include_field_counts"), bIncludeFieldCounts);
	OutData->SetObjectField(TEXT("filters"), Filters);
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("section_count"), SectionValues.Num());
	Summary->SetNumberField(TEXT("supported_count"), SupportedCount);
	Summary->SetNumberField(TEXT("unsupported_count"), UnsupportedCount);
	OutData->SetObjectField(TEXT("summary"), Summary);
	TSharedPtr<FJsonObject> PluginFingerprint = BuildPluginFingerprint();
	OutData->SetObjectField(TEXT("plugin_fingerprint"), PluginFingerprint);
	OutData->SetStringField(TEXT("plugin_fingerprint_hash"), PluginFingerprint->GetStringField(TEXT("enabled_plugins_hash")));
	OutData->SetObjectField(TEXT("manifest"), Manifest);

	FString RegistryText;
	SerializeJsonObject(ManifestSections, RegistryText, false);
	OutData->SetStringField(TEXT("settings_registry_hash"), HashString(RegistryText));

	if (bWriteFiles)
	{
		FString SavedIndex;
		const FString IndexFile = ExplicitIndexFile.IsEmpty() ? FPaths::Combine(OutputRoot, TEXT("index.json")) : ExplicitIndexFile;
		if (!SaveJsonFile(IndexFile, OutData, SavedIndex, OutError))
		{
			return false;
		}
		FString SavedManifest;
		if (!SaveJsonFile(FPaths::Combine(FPaths::GetPath(SavedIndex), TEXT("manifest.json")), Manifest, SavedManifest, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("index_file"), SavedIndex);
		OutData->SetStringField(TEXT("manifest_file"), SavedManifest);
	}
	return true;
}

bool FUeAgentHttpServer::CmdProjectSettingsExportPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FPageRef Page;
	if (!FindPage(Ctx.Params, Page, OutError))
	{
		return false;
	}
	if (!Page.SettingsObject)
	{
		OutError = GetUnsupportedPageError(Page);
		OutData = BuildSectionIndexObject(Page, ResolveOutputRoot(Ctx.Params));
		return false;
	}

	bool bIncludeNonEditable = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_non_editable")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_non_editable"), bIncludeNonEditable);
	}
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_hidden_properties")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_hidden_properties"), bIncludeNonEditable);
	}
	bool bIncludeRawConfig = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_raw_config")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_raw_config"), bIncludeRawConfig);
	}
	bool bIncludeDefaults = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_defaults")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);
	}
	bool bIncludeMetadata = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("include_metadata")))
	{
		Ctx.Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	}

	const FString OutputRoot = ResolveOutputRoot(Ctx.Params);
	OutData = BuildPageJson(Page, OutputRoot, bIncludeNonEditable, bIncludeRawConfig, bIncludeDefaults, bIncludeMetadata);

	bool bWriteFile = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("write_file")))
	{
		Ctx.Params->TryGetBoolField(TEXT("write_file"), bWriteFile);
	}
	FString OutputFile = ResolveOutputFileParam(Ctx.Params);
	if (OutputFile.IsEmpty())
	{
		OutputFile = FPaths::Combine(BuildPageFolder(OutputRoot, Page), TEXT("settings.json"));
	}
	if (bWriteFile)
	{
		FString SavedFile;
		if (!SaveJsonFile(OutputFile, OutData, SavedFile, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedFile);
		if (bIncludeRawConfig)
		{
			const TSharedPtr<FJsonObject>* RawConfig = nullptr;
			if (OutData->TryGetObjectField(TEXT("raw_config"), RawConfig) && RawConfig && RawConfig->IsValid())
			{
				FString SavedRawConfig;
				if (!SaveJsonFile(FPaths::Combine(FPaths::GetPath(SavedFile), TEXT("raw_config.json")), *RawConfig, SavedRawConfig, OutError))
				{
					return false;
				}
				OutData->SetStringField(TEXT("raw_config_file"), SavedRawConfig);
			}
		}
	}
	return true;
}

bool FUeAgentHttpServer::CmdProjectSettingsValidatePage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FPageRef Page;
	if (!FindPage(Ctx.Params, Page, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FString LocalError;
	if (!ApplyOrValidatePage(Page, Ctx.Params, false, false, OutData, LocalError))
	{
		OutData->SetBoolField(TEXT("valid"), false);
		OutData->SetStringField(TEXT("error"), LocalError);
		return true;
	}
	return true;
}

bool FUeAgentHttpServer::CmdProjectSettingsApplyPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FPageRef Page;
	if (!FindPage(Ctx.Params, Page, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	bool bReadBack = true;
	if (!ApplyOrValidatePage(Page, Ctx.Params, true, false, OutData, OutError, &bReadBack))
	{
		return false;
	}

	if (bReadBack)
	{
		OutData->SetObjectField(TEXT("read_back"), BuildPageJson(Page, ResolveOutputRoot(Ctx.Params), true));
	}
	return true;
}

bool FUeAgentHttpServer::CmdProjectSettingsDiffPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FPageRef Page;
	if (!FindPage(Ctx.Params, Page, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	return ApplyOrValidatePage(Page, Ctx.Params, false, true, OutData, OutError);
}

bool FUeAgentHttpServer::CmdConfigExportSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ConfigFileInput;
	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("config_file"), ConfigFileInput) || ConfigFileInput.IsEmpty())
	{
		OutError = TEXT("missing_config_file");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("section"), SectionName) || SectionName.IsEmpty())
	{
		OutError = TEXT("missing_section");
		return false;
	}

	const FString ConfigFilename = ResolveConfigFile(ConfigFileInput, OutError);
	if (ConfigFilename.IsEmpty())
	{
		return false;
	}
	OutData = BuildConfigSectionJson(ConfigFilename, SectionName);

	FString OutputFile = ResolveOutputFileParam(Ctx.Params);
	if (!OutputFile.IsEmpty())
	{
		FString SavedFile;
		if (!SaveJsonFile(OutputFile, OutData, SavedFile, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdConfigApplySection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	TSharedPtr<FJsonObject> JsonRoot;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	FString ConfigFileInput;
	FString SectionName;
	JsonTryGetString(Ctx.Params, TEXT("config_file"), ConfigFileInput);
	JsonTryGetString(Ctx.Params, TEXT("section"), SectionName);
	if (ConfigFileInput.IsEmpty() && JsonRoot.IsValid())
	{
		JsonRoot->TryGetStringField(TEXT("config_file"), ConfigFileInput);
	}
	if (SectionName.IsEmpty() && JsonRoot.IsValid())
	{
		JsonRoot->TryGetStringField(TEXT("section"), SectionName);
	}
	if (ConfigFileInput.IsEmpty())
	{
		OutError = TEXT("missing_config_file");
		return false;
	}
	if (SectionName.IsEmpty())
	{
		OutError = TEXT("missing_section");
		return false;
	}

	const FString ConfigFilename = ResolveConfigFile(ConfigFileInput, OutError);
	if (ConfigFilename.IsEmpty())
	{
		return false;
	}

	bool bDryRun = true;
	TryGetOptionBool(Ctx.Params, JsonRoot, TEXT("dry_run"), bDryRun);
	bool bBackup = true;
	TryGetOptionBool(Ctx.Params, JsonRoot, TEXT("backup"), bBackup);

	const TSharedPtr<FJsonObject>* SetObj = nullptr;
	if (!Ctx.Params->TryGetObjectField(TEXT("set"), SetObj) || !SetObj || !SetObj->IsValid())
	{
		if (JsonRoot.IsValid())
		{
			JsonRoot->TryGetObjectField(TEXT("set"), SetObj);
		}
	}
	if ((!SetObj || !SetObj->IsValid()) && JsonRoot.IsValid())
	{
		JsonRoot->TryGetObjectField(TEXT("values"), SetObj);
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("remove"), RemoveArray) || !RemoveArray)
	{
		if (JsonRoot.IsValid())
		{
			JsonRoot->TryGetArrayField(TEXT("remove"), RemoveArray);
		}
	}

	if ((!SetObj || !SetObj->IsValid()) && (!RemoveArray || RemoveArray->Num() == 0))
	{
		OutError = TEXT("missing_set_or_remove");
		return false;
	}

	if (GConfig)
	{
		GConfig->LoadFile(ConfigFilename);
	}

	struct FConfigPendingOperation
	{
		FString Key;
		FString Operation;
		TArray<FString> Values;
		bool bArray = false;
		TSharedPtr<FJsonObject> ResultObj;
	};

	TArray<FConfigPendingOperation> PendingOperations;
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	int32 ErrorCount = 0;

	auto AddConfigError = [&Results, &Errors, &ErrorCount](const FString& Key, const FString& Operation, const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("key"), Key);
		Result->SetStringField(TEXT("operation"), Operation);
		Result->SetStringField(TEXT("status"), Code);
		Result->SetStringField(TEXT("error"), Code);
		if (!Message.IsEmpty())
		{
			Result->SetStringField(TEXT("message"), Message);
		}
		Results.Add(MakeShared<FJsonValueObject>(Result));
		Errors.Add(MakeShared<FJsonValueObject>(Result));
		++ErrorCount;
	};

	if (SetObj && SetObj->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SetObj)->Values)
		{
			const FString Key = Pair.Key;
			if (Key.TrimStartAndEnd().IsEmpty())
			{
				AddConfigError(Key, TEXT("set"), TEXT("invalid_config_key"), TEXT("Config set key must not be empty."));
				continue;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("key"), Key);
			Result->SetStringField(TEXT("operation"), TEXT("set"));

			FConfigPendingOperation Pending;
			Pending.Key = Key;
			Pending.Operation = TEXT("set");
			Pending.ResultObj = Result;

			bool bValueOk = true;
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
			{
				Pending.bArray = true;
				const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
				Pair.Value->TryGetArray(ArrayValues);
				Result->SetNumberField(TEXT("value_count"), ArrayValues ? ArrayValues->Num() : 0);
				TArray<TSharedPtr<FJsonValue>> ResultValues;
				if (ArrayValues)
				{
					for (int32 Index = 0; Index < ArrayValues->Num(); ++Index)
					{
						FString TextValue;
						if (!ConfigJsonValueToString((*ArrayValues)[Index], TextValue))
						{
							Result->SetStringField(TEXT("status"), TEXT("invalid_config_value"));
							Result->SetStringField(TEXT("error"), TEXT("invalid_config_value"));
							Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Config array value at index %d cannot be converted to a string."), Index));
							Results.Add(MakeShared<FJsonValueObject>(Result));
							Errors.Add(MakeShared<FJsonValueObject>(Result));
							++ErrorCount;
							bValueOk = false;
							break;
						}
						Pending.Values.Add(TextValue);
						ResultValues.Add(MakeShared<FJsonValueString>(TextValue));
					}
				}
				Result->SetArrayField(TEXT("values"), ResultValues);
			}
			else
			{
				FString TextValue;
				if (!ConfigJsonValueToString(Pair.Value, TextValue))
				{
					Result->SetStringField(TEXT("status"), TEXT("invalid_config_value"));
					Result->SetStringField(TEXT("error"), TEXT("invalid_config_value"));
					Result->SetStringField(TEXT("message"), TEXT("Config value cannot be converted to a string."));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					Errors.Add(MakeShared<FJsonValueObject>(Result));
					++ErrorCount;
					bValueOk = false;
				}
				else
				{
					Pending.Values.Add(TextValue);
					Result->SetStringField(TEXT("value"), TextValue);
				}
			}

			if (bValueOk)
			{
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("validated") : TEXT("pending_apply"));
				Results.Add(MakeShared<FJsonValueObject>(Result));
				PendingOperations.Add(MoveTemp(Pending));
			}
		}
	}

	if (RemoveArray)
	{
		for (int32 Index = 0; Index < RemoveArray->Num(); ++Index)
		{
			FString Key;
			if (!ConfigJsonValueToString((*RemoveArray)[Index], Key) || Key.TrimStartAndEnd().IsEmpty())
			{
				AddConfigError(FString::Printf(TEXT("remove[%d]"), Index), TEXT("remove"), TEXT("invalid_config_key"), TEXT("Config remove entries must be non-empty key strings."));
				continue;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("key"), Key);
			Result->SetStringField(TEXT("operation"), TEXT("remove"));
			Result->SetStringField(TEXT("status"), bDryRun ? TEXT("validated") : TEXT("pending_apply"));
			Results.Add(MakeShared<FJsonValueObject>(Result));

			FConfigPendingOperation Pending;
			Pending.Key = Key;
			Pending.Operation = TEXT("remove");
			Pending.ResultObj = Result;
			PendingOperations.Add(MoveTemp(Pending));
		}
	}

	auto BuildApplyResult = [&]()
	{
		OutData = MakeShared<FJsonObject>();
		AddProtocolFields(OutData, ConfigApplySchema);
		AddProjectIdentityFields(OutData);
		OutData->SetStringField(TEXT("profile"), ConfigSectionProfile);
		OutData->SetStringField(TEXT("config_file"), ConfigFilename);
		OutData->SetStringField(TEXT("section"), SectionName);
		OutData->SetStringField(TEXT("source"), TEXT("config_file_fallback"));
		OutData->SetStringField(TEXT("source_path"), ConfigFilename);
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("success"), ErrorCount == 0);
		OutData->SetArrayField(TEXT("results"), Results);
		OutData->SetArrayField(TEXT("changes"), Results);
		OutData->SetArrayField(TEXT("errors"), Errors);
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		OutData->SetNumberField(TEXT("operation_count"), Results.Num());
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		if (!ResolvedJsonFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
		}
	};

	if (ErrorCount > 0)
	{
		if (!bDryRun)
		{
			for (FConfigPendingOperation& Pending : PendingOperations)
			{
				if (Pending.ResultObj.IsValid())
				{
					Pending.ResultObj->SetStringField(TEXT("status"), TEXT("skipped_due_to_errors"));
					Pending.ResultObj->SetStringField(TEXT("message"), TEXT("No config key was written because apply preflight found errors."));
				}
			}
		}
		BuildApplyResult();
		OutError = TEXT("config_apply_failed");
		return false;
	}

	FString BackupFile;
	if (!bDryRun)
	{
		if (!GConfig)
		{
			OutError = TEXT("config_cache_unavailable");
			return false;
		}
		if (bBackup && FPaths::FileExists(ConfigFilename))
		{
			const FString BackupRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"), TEXT("Backups"), TEXT("Config"));
			IFileManager::Get().MakeDirectory(*BackupRoot, true);
			BackupFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				BackupRoot,
				FPaths::GetBaseFilename(ConfigFilename) + TEXT("_") + FDateTime::Now().ToString(TEXT("%Y%m%dT%H%M%S")) + TEXT(".bak")));
			if (IFileManager::Get().Copy(*BackupFile, *ConfigFilename) != 0)
			{
				OutError = TEXT("config_backup_failed");
				return false;
			}
		}

		for (FConfigPendingOperation& Pending : PendingOperations)
		{
			if (Pending.Operation.Equals(TEXT("set"), ESearchCase::IgnoreCase))
			{
				if (Pending.bArray)
				{
					GConfig->RemoveKey(*SectionName, *Pending.Key, ConfigFilename);
					for (const FString& TextValue : Pending.Values)
					{
						GConfig->AddToSection(*SectionName, FName(*Pending.Key), TextValue, ConfigFilename);
					}
				}
				else
				{
					const FString TextValue = Pending.Values.Num() > 0 ? Pending.Values[0] : FString();
					GConfig->SetString(*SectionName, *Pending.Key, *TextValue, ConfigFilename);
				}
			}
			else if (Pending.Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
			{
				GConfig->RemoveKey(*SectionName, *Pending.Key, ConfigFilename);
			}
			if (Pending.ResultObj.IsValid())
			{
				Pending.ResultObj->SetStringField(TEXT("status"), TEXT("applied"));
			}
		}
		GConfig->Flush(false, ConfigFilename);
	}

	BuildApplyResult();
	if (!BackupFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("backup_file"), BackupFile);
		OutData->SetStringField(TEXT("backup_path"), BackupFile);
	}
	bool bReadBack = true;
	TryGetOptionBool(Ctx.Params, JsonRoot, TEXT("read_back"), bReadBack);
	if (bReadBack)
	{
		OutData->SetObjectField(TEXT("read_back"), BuildConfigSectionJson(ConfigFilename, SectionName));
	}
	return true;
}
