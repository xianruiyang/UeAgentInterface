// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_UMG.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgOps
{
	enum class ECompilerMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
	{
		return Cast<UWidgetBlueprint>(LoadAssetObject(InPath));
	}

	static UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassPath = ClassPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	static FString BlueprintStatusToString(const EBlueprintStatus InStatus)
	{
		switch (InStatus)
		{
		case BS_Unknown: return TEXT("Unknown");
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_BeingCreated: return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		default: return TEXT("Unknown");
		}
	}

	static FString CompilerSeverityToString(const EMessageSeverity::Type InSeverity)
	{
		switch (InSeverity)
		{
		case EMessageSeverity::Error:
			return TEXT("Error");
		case EMessageSeverity::Warning:
			return TEXT("Warning");
		case EMessageSeverity::PerformanceWarning:
			return TEXT("PerformanceWarning");
		case EMessageSeverity::Info:
			return TEXT("Info");
		default:
			return TEXT("Unknown");
		}
	}

	static bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter)
	{
		FString Normalized = InFilterText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ECompilerMessageSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Error;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Warning;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("note") || Normalized == TEXT("notes"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Info;
			return true;
		}
		if (Normalized == TEXT("warningorerror") || Normalized == TEXT("warnorerror") || Normalized == TEXT("errorsandwarnings"))
		{
			OutFilter = ECompilerMessageSeverityFilter::WarningOrError;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompilerMessage(const EMessageSeverity::Type InSeverity, const ECompilerMessageSeverityFilter InFilter)
	{
		switch (InFilter)
		{
		case ECompilerMessageSeverityFilter::All:
			return true;
		case ECompilerMessageSeverityFilter::Error:
			return InSeverity == EMessageSeverity::Error;
		case ECompilerMessageSeverityFilter::Warning:
			return InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		case ECompilerMessageSeverityFilter::Info:
			return InSeverity == EMessageSeverity::Info;
		case ECompilerMessageSeverityFilter::WarningOrError:
			return InSeverity == EMessageSeverity::Error || InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		default:
			return true;
		}
	}

	static void BuildCompilerLogJson(
		const FCompilerResultsLog& InCompilerLog,
		const ECompilerMessageSeverityFilter InFilter,
		const int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutFilteredErrorCount,
		int32& OutFilteredWarningCount,
		int32& OutFilteredInfoCount)
	{
		OutFilteredErrorCount = 0;
		OutFilteredWarningCount = 0;
		OutFilteredInfoCount = 0;
		OutMessages.Reset();

		const int32 MaxMessages = FMath::Clamp(InMaxMessages, 1, 2000);
		for (const TSharedRef<FTokenizedMessage>& Message : InCompilerLog.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (!ShouldIncludeCompilerMessage(Severity, InFilter))
			{
				continue;
			}

			if (Severity == EMessageSeverity::Error)
			{
				++OutFilteredErrorCount;
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				++OutFilteredWarningCount;
			}
			else if (Severity == EMessageSeverity::Info)
			{
				++OutFilteredInfoCount;
			}

			if (OutMessages.Num() >= MaxMessages)
			{
				continue;
			}

			TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
			MessageObj->SetStringField(TEXT("severity"), CompilerSeverityToString(Severity));
			MessageObj->SetStringField(TEXT("message"), Message->ToText().ToString());
			MessageObj->SetStringField(TEXT("identifier"), Message->GetIdentifier().ToString());
			OutMessages.Add(MakeShared<FJsonValueObject>(MessageObj));
		}
	}

	static bool SaveWidgetBlueprintPackage(UWidgetBlueprint* WidgetBlueprint, FString& OutError)
	{
		if (!WidgetBlueprint)
		{
			OutError = TEXT("widget_blueprint_not_found");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(WidgetBlueprint->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}

		return false;
	}

	static UWidget* FindWidgetByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || WidgetName.IsEmpty())
		{
			return nullptr;
		}

		const FString NameTrim = WidgetName.TrimStartAndEnd();
		if (UWidget* Exact = WidgetBlueprint->WidgetTree->FindWidget(FName(*NameTrim)))
		{
			return Exact;
		}

		TArray<UWidget*> Widgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (Widget && Widget->GetName().Equals(NameTrim, ESearchCase::IgnoreCase))
			{
				return Widget;
			}
		}
		return nullptr;
	}

	static void BuildParentMap(UWidget* ParentWidget, TMap<UWidget*, UWidget*>& OutParentMap)
	{
		UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
		if (!Panel)
		{
			return;
		}

		for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
		{
			UWidget* Child = Panel->GetChildAt(ChildIndex);
			if (!Child)
			{
				continue;
			}
			OutParentMap.Add(Child, ParentWidget);
			BuildParentMap(Child, OutParentMap);
		}
	}
}

bool FUeAgentHttpServer::ExecuteUmgCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("umg_create_widget_blueprint")) return CmdUmgCreateWidgetBlueprint(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_compile")) return CmdUmgCompile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_get_compile_log")) return CmdUmgGetCompileLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_get_info")) return CmdUmgGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_add_widget")) return CmdUmgAddWidget(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_remove_widget")) return CmdUmgRemoveWidget(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_set_widget_property")) return CmdUmgSetWidgetProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_set_slot_property")) return CmdUmgSetSlotProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_rename_widget")) return CmdUmgRenameWidget(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_bind_widget_property_to_variable")) return CmdUmgBindWidgetPropertyToVariable(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_export_folder")) return CmdUmgExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("umg_apply_folder")) return CmdUmgApplyFolder(Ctx, OutData, OutError);

	OutError = TEXT("unknown_umg_command");
	return false;
}

bool FUeAgentHttpServer::CmdUmgCreateWidgetBlueprint(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentUmgOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentUmgOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	FString ParentClassPath = TEXT("/Script/UMG.UserWidget");
	JsonTryGetString(Ctx.Params, TEXT("parent_class"), ParentClassPath);
	UClass* ParentClass = UeAgentUmgOps::ResolveClassByPath(ParentClassPath);
	if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		OutError = TEXT("invalid_parent_class");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UBlueprint* CreatedBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		*AssetName,
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("UeAgentInterface")));
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(CreatedBlueprint);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("create_widget_blueprint_failed");
		return false;
	}

	bool bCreateDefaultRoot = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_default_root"), bCreateDefaultRoot);
	if (bCreateDefaultRoot && WidgetBlueprint->WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName(TEXT("RootCanvas")));
		if (RootCanvas)
		{
			WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		}
	}

	FAssetRegistryModule::AssetCreated(WidgetBlueprint);
	WidgetBlueprint->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(WidgetBlueprint);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("object_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("status"), UeAgentUmgOps::BlueprintStatusToString(WidgetBlueprint->Status));
	OutData->SetStringField(TEXT("root_widget"), WidgetBlueprint->WidgetTree->RootWidget ? WidgetBlueprint->WidgetTree->RootWidget->GetName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdUmgCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::None, &CompilerLog);

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentUmgOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentUmgOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentUmgOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bIncludeMessages = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_messages"), bIncludeMessages);

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	if (bSaveAfterCompile && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentUmgOps::BlueprintStatusToString(WidgetBlueprint->Status));
	OutData->SetStringField(TEXT("generated_class"), WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || WidgetBlueprint->Status == BS_Error);

	if (bIncludeMessages)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 FilteredErrorCount = 0;
		int32 FilteredWarningCount = 0;
		int32 FilteredInfoCount = 0;
		UeAgentUmgOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

		OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
		OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
		OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
		OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
		OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
		OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
		OutData->SetArrayField(TEXT("messages"), Messages);
	}
	return true;
}

bool FUeAgentHttpServer::CmdUmgGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentUmgOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentUmgOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentUmgOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::None, &CompilerLog);

	if (bSaveAfterCompile && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	int32 FilteredErrorCount = 0;
	int32 FilteredWarningCount = 0;
	int32 FilteredInfoCount = 0;
	UeAgentUmgOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentUmgOps::BlueprintStatusToString(WidgetBlueprint->Status));
	OutData->SetStringField(TEXT("generated_class"), WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
	OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
	OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
	OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || WidgetBlueprint->Status == BS_Error);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetArrayField(TEXT("messages"), Messages);
	return true;
}

bool FUeAgentHttpServer::CmdUmgGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	TMap<UWidget*, UWidget*> ParentMap;
	UeAgentUmgOps::BuildParentMap(WidgetBlueprint->WidgetTree->RootWidget, ParentMap);

	TArray<UWidget*> Widgets;
	WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
	TArray<TSharedPtr<FJsonValue>> WidgetItems;
	for (UWidget* Widget : Widgets)
	{
		if (!Widget)
		{
			continue;
		}

		TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
		WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObj->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : TEXT(""));
		WidgetObj->SetStringField(TEXT("slot_class"), Widget->Slot ? Widget->Slot->GetClass()->GetPathName() : TEXT(""));
		WidgetObj->SetStringField(TEXT("parent"), ParentMap.Contains(Widget) && ParentMap[Widget] ? ParentMap[Widget]->GetName() : TEXT(""));
		WidgetObj->SetBoolField(TEXT("is_panel"), Cast<UPanelWidget>(Widget) != nullptr);

		TArray<TSharedPtr<FJsonValue>> Children;
		if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
		{
			for (int32 ChildIndex = 0; ChildIndex < PanelWidget->GetChildrenCount(); ++ChildIndex)
			{
				if (UWidget* Child = PanelWidget->GetChildAt(ChildIndex))
				{
					Children.Add(MakeShared<FJsonValueString>(Child->GetName()));
				}
			}
		}
		WidgetObj->SetArrayField(TEXT("children"), Children);
		WidgetItems.Add(MakeShared<FJsonValueObject>(WidgetObj));
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("generated_class"), WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("status"), UeAgentUmgOps::BlueprintStatusToString(WidgetBlueprint->Status));
	OutData->SetStringField(TEXT("root_widget"), WidgetBlueprint->WidgetTree->RootWidget ? WidgetBlueprint->WidgetTree->RootWidget->GetName() : TEXT(""));
	OutData->SetNumberField(TEXT("widget_count"), WidgetItems.Num());
	OutData->SetArrayField(TEXT("widgets"), WidgetItems);
	return true;
}

bool FUeAgentHttpServer::CmdUmgAddWidget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_class"), WidgetClassPath) || WidgetClassPath.IsEmpty())
	{
		OutError = TEXT("missing_widget_class");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UClass* WidgetClass = UeAgentUmgOps::ResolveClassByPath(WidgetClassPath);
	if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		OutError = TEXT("invalid_widget_class");
		return false;
	}

	FString WidgetName;
	JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName);
	WidgetName.TrimStartAndEndInline();
	if (WidgetName.IsEmpty())
	{
		WidgetName = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		WidgetName = FString::Printf(TEXT("%s_%s"), *WidgetClass->GetName(), *WidgetName);
	}
	if (UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName))
	{
		OutError = TEXT("widget_name_already_exists");
		return false;
	}

	UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!NewWidget)
	{
		OutError = TEXT("create_widget_failed");
		return false;
	}

	bool bMakeVariable = true;
	JsonTryGetBool(Ctx.Params, TEXT("make_variable"), bMakeVariable);
	NewWidget->bIsVariable = bMakeVariable;

	FString ParentWidgetName;
	JsonTryGetString(Ctx.Params, TEXT("parent_widget"), ParentWidgetName);

	UPanelWidget* ParentPanel = nullptr;
	if (!ParentWidgetName.IsEmpty())
	{
		UWidget* ParentWidget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, ParentWidgetName);
		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			OutError = TEXT("parent_widget_not_found_or_not_panel");
			return false;
		}
	}
	else if (WidgetBlueprint->WidgetTree->RootWidget)
	{
		ParentPanel = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->RootWidget);
		if (!ParentPanel)
		{
			OutError = TEXT("root_widget_is_not_panel_require_parent_widget");
			return false;
		}
	}

	WidgetBlueprint->Modify();
	WidgetBlueprint->WidgetTree->Modify();
	NewWidget->Modify();

	UPanelSlot* Slot = nullptr;
	if (!WidgetBlueprint->WidgetTree->RootWidget)
	{
		WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		double InsertIndexNumber = -1.0;
		JsonTryGetNumber(Ctx.Params, TEXT("insert_index"), InsertIndexNumber);
		const int32 InsertIndex = (int32)InsertIndexNumber;
		if (InsertIndex >= 0)
		{
			Slot = ParentPanel->InsertChildAt(InsertIndex, NewWidget);
		}
		else
		{
			Slot = ParentPanel->AddChild(NewWidget);
		}
		if (!Slot)
		{
			OutError = TEXT("add_child_failed");
			return false;
		}
	}
	else
	{
		OutError = TEXT("missing_parent_widget");
		return false;
	}

	if (bMakeVariable)
	{
		WidgetBlueprint->OnVariableAdded(NewWidget->GetFName());
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	if (bCompileAfterAdd)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (bSaveAfterAdd && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	OutData->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
	OutData->SetStringField(TEXT("parent_widget"), ParentPanel ? ParentPanel->GetName() : TEXT(""));
	OutData->SetStringField(TEXT("slot_class"), Slot ? Slot->GetClass()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdUmgRemoveWidget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	const FString RemovedName = Widget->GetName();
	const FName RemovedWidgetFName = Widget->GetFName();
	const bool bWasVariable = Widget->bIsVariable;
	WidgetBlueprint->Modify();
	WidgetBlueprint->WidgetTree->Modify();

	bool bRemoved = false;
	if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
	{
		WidgetBlueprint->WidgetTree->RootWidget = nullptr;
		bRemoved = true;
	}
	else
	{
		bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
	}
	if (!bRemoved)
	{
		OutError = TEXT("remove_widget_failed");
		return false;
	}

	// Rename the removed widget out of the WidgetTree hierarchy before structural refresh,
	// otherwise the compiler can still discover it during source-widget traversal.
	Widget->Rename(nullptr, GetTransientPackage());

	const bool bHasWidgetWithSameName = WidgetBlueprint->GetAllSourceWidgets().ContainsByPredicate([RemovedWidgetFName](const UWidget* ExistingWidget)
	{
		return ExistingWidget && ExistingWidget->GetFName() == RemovedWidgetFName;
	});
	if (bWasVariable && !bHasWidgetWithSameName)
	{
		WidgetBlueprint->OnVariableRemoved(RemovedWidgetFName);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	if (bCompileAfterRemove)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (bSaveAfterRemove && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_widget"), RemovedName);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdUmgSetWidgetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentUmgOps::ResolvePropertyPath(Widget, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	WidgetBlueprint->Modify();
	Widget->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Widget, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	bool bCompileAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, Widget, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdUmgSetSlotProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}
	if (!Widget->Slot)
	{
		OutError = TEXT("slot_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentUmgOps::ResolvePropertyPath(Widget->Slot, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	WidgetBlueprint->Modify();
	Widget->Slot->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Widget->Slot, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	bool bCompileAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, Widget->Slot, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutData->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdUmgRenameWidget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}
	FString NewWidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_widget_name"), NewWidgetName) || NewWidgetName.IsEmpty())
	{
		OutError = TEXT("missing_new_widget_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	NewWidgetName.TrimStartAndEndInline();
	if (Widget->GetName().Equals(NewWidgetName, ESearchCase::IgnoreCase))
	{
		OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
		OutData->SetStringField(TEXT("new_widget_name"), NewWidgetName);
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}
	if (UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, NewWidgetName))
	{
		OutError = TEXT("new_widget_name_already_exists");
		return false;
	}

	const FName OldWidgetFName = Widget->GetFName();
	const bool bWasVariable = Widget->bIsVariable;
	WidgetBlueprint->Modify();
	WidgetBlueprint->WidgetTree->Modify();
	Widget->Modify();
	if (!Widget->Rename(*NewWidgetName, WidgetBlueprint->WidgetTree, REN_DontCreateRedirectors))
	{
		OutError = TEXT("rename_widget_failed");
		return false;
	}
	if (bWasVariable)
	{
		WidgetBlueprint->OnVariableRenamed(OldWidgetFName, Widget->GetFName());
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterRename = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_rename"), bCompileAfterRename);
	if (bCompileAfterRename)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (bSaveAfterRename && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("widget_name"), WidgetName);
	OutData->SetStringField(TEXT("new_widget_name"), Widget->GetName());
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdUmgBindWidgetPropertyToVariable(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString SourceVariableName;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_variable_name"), SourceVariableName) || SourceVariableName.IsEmpty())
	{
		OutError = TEXT("missing_source_variable_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}
	if (!WidgetBlueprint->SkeletonGeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	if (!WidgetBlueprint->SkeletonGeneratedClass)
	{
		OutError = TEXT("widget_blueprint_skeleton_not_found");
		return false;
	}

	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	const FName SourceVariableFName(*SourceVariableName);
	FProperty* SourceProperty = FindFProperty<FProperty>(WidgetBlueprint->SkeletonGeneratedClass, SourceVariableFName);
	if (!SourceProperty)
	{
		OutError = TEXT("source_variable_not_found");
		return false;
	}

	const FString DelegateName = PropertyName + TEXT("Delegate");
	if (!FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*DelegateName)))
	{
		OutError = TEXT("target_property_not_bindable");
		return false;
	}

	FDelegateEditorBinding NewBinding;
	NewBinding.ObjectName = Widget->GetName();
	NewBinding.PropertyName = FName(*PropertyName);
	NewBinding.FunctionName = SourceVariableFName;
	NewBinding.SourceProperty = SourceVariableFName;
	NewBinding.SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{ FFieldVariant(SourceProperty) });
	NewBinding.Kind = EBindingKind::Property;

	bool bReplaced = false;
	for (FDelegateEditorBinding& Existing : WidgetBlueprint->Bindings)
	{
		if (Existing.ObjectName.Equals(NewBinding.ObjectName, ESearchCase::IgnoreCase) &&
			Existing.PropertyName == NewBinding.PropertyName)
		{
			Existing = NewBinding;
			bReplaced = true;
			break;
		}
	}
	if (!bReplaced)
	{
		WidgetBlueprint->Bindings.Add(NewBinding);
	}

	WidgetBlueprint->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	bool bCompileAfterBind = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_bind"), bCompileAfterBind);
	if (bCompileAfterBind)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	bool bSaveAfterBind = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_bind"), bSaveAfterBind);
	if (bSaveAfterBind && !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("source_variable_name"), SourceVariableName);
	OutData->SetBoolField(TEXT("replaced_existing"), bReplaced);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterBind);
	return true;
}

#include "UeAgentHttpServer_UMG_FolderFormat.inl"
