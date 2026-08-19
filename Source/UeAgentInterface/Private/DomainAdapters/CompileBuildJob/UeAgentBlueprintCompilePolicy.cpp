#include "DomainAdapters/CompileBuildJob/UeAgentBlueprintCompilePolicy.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	namespace
	{
		enum class ECompileMessageFilter : uint8
		{
			All,
			Error,
			Warning,
			Info,
			WarningOrError,
		};

		bool Fail(
			FString& OutCode,
			FString& OutDetail,
			FString Code,
			FString Detail = FString())
		{
			OutCode = MoveTemp(Code);
			OutDetail = MoveTemp(Detail);
			return false;
		}

		FString StatusText(const EBlueprintStatus Status)
		{
			switch (Status)
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

		FString SeverityText(const EMessageSeverity::Type Severity)
		{
			switch (Severity)
			{
			case EMessageSeverity::Error: return TEXT("Error");
			case EMessageSeverity::Warning: return TEXT("Warning");
			case EMessageSeverity::PerformanceWarning:
				return TEXT("PerformanceWarning");
			case EMessageSeverity::Info: return TEXT("Info");
			default: return TEXT("Unknown");
			}
		}

		bool TryFilter(
			const FString& Input,
			ECompileMessageFilter& OutFilter,
			FString& OutCanonical)
		{
			FString Normalized = Input.TrimStartAndEnd().ToLower();
			Normalized.ReplaceInline(TEXT("_"), TEXT(""));
			Normalized.ReplaceInline(TEXT("-"), TEXT(""));
			Normalized.ReplaceInline(TEXT(" "), TEXT(""));
			if (Normalized.IsEmpty() || Normalized == TEXT("all"))
			{
				OutFilter = ECompileMessageFilter::All;
				OutCanonical = TEXT("all");
				return true;
			}
			if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
			{
				OutFilter = ECompileMessageFilter::Error;
				OutCanonical = TEXT("error");
				return true;
			}
			if (Normalized == TEXT("warning") || Normalized == TEXT("warnings")
				|| Normalized == TEXT("warn"))
			{
				OutFilter = ECompileMessageFilter::Warning;
				OutCanonical = TEXT("warning");
				return true;
			}
			if (Normalized == TEXT("info") || Normalized == TEXT("note")
				|| Normalized == TEXT("notes"))
			{
				OutFilter = ECompileMessageFilter::Info;
				OutCanonical = TEXT("info");
				return true;
			}
			if (Normalized == TEXT("warningorerror")
				|| Normalized == TEXT("warnorerror")
				|| Normalized == TEXT("errorsandwarnings"))
			{
				OutFilter = ECompileMessageFilter::WarningOrError;
				OutCanonical = TEXT("warning_or_error");
				return true;
			}
			return false;
		}

		bool Includes(
			const EMessageSeverity::Type Severity,
			const ECompileMessageFilter Filter)
		{
			switch (Filter)
			{
			case ECompileMessageFilter::All: return true;
			case ECompileMessageFilter::Error:
				return Severity == EMessageSeverity::Error;
			case ECompileMessageFilter::Warning:
				return Severity == EMessageSeverity::Warning
					|| Severity == EMessageSeverity::PerformanceWarning;
			case ECompileMessageFilter::Info:
				return Severity == EMessageSeverity::Info;
			case ECompileMessageFilter::WarningOrError:
				return Severity == EMessageSeverity::Error
					|| Severity == EMessageSeverity::Warning
					|| Severity == EMessageSeverity::PerformanceWarning;
			default: return false;
			}
		}
	}

	bool TryDecodeBlueprintCompilePolicyRequest(
		const TSharedPtr<FJsonObject>& Parameters,
		FBlueprintCompilePolicyRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutRequest = {};
		OutErrorCode.Reset();
		OutErrorDetail.Reset();
		if (!Parameters.IsValid())
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_parameters_invalid"));
		}
		static const TSet<FString> Allowed = {
			TEXT("asset_path"), TEXT("include_messages"), TEXT("max_messages"),
			TEXT("save_after_compile"), TEXT("severity_filter"),
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
			: Parameters->Values)
		{
			if (!Allowed.Contains(Field.Key))
			{
				return Fail(OutErrorCode, OutErrorDetail,
					TEXT("blueprint_compile_parameters_invalid"), Field.Key);
			}
		}
		if (!Parameters->TryGetStringField(TEXT("asset_path"), OutRequest.AssetPath)
			|| OutRequest.AssetPath.TrimStartAndEnd().IsEmpty())
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_asset_path_invalid"));
		}
		OutRequest.AssetPath.TrimStartAndEndInline();
		if (Parameters->HasField(TEXT("include_messages"))
			&& !Parameters->TryGetBoolField(
				TEXT("include_messages"), OutRequest.bIncludeMessages))
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_parameters_invalid"),
				TEXT("include_messages"));
		}
		if (Parameters->HasField(TEXT("save_after_compile"))
			&& !Parameters->TryGetBoolField(
				TEXT("save_after_compile"), OutRequest.bSaveAfterCompile))
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_parameters_invalid"),
				TEXT("save_after_compile"));
		}
		if (Parameters->HasField(TEXT("max_messages")))
		{
			double Value = 0.0;
			if (!Parameters->TryGetNumberField(TEXT("max_messages"), Value)
				|| Value < 1.0 || Value > 2000.0
				|| !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
			{
				return Fail(OutErrorCode, OutErrorDetail,
					TEXT("blueprint_compile_max_messages_invalid"));
			}
			OutRequest.MaxMessages = FMath::RoundToInt(Value);
		}
		if (Parameters->HasField(TEXT("severity_filter"))
			&& !Parameters->TryGetStringField(
				TEXT("severity_filter"), OutRequest.SeverityFilter))
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_severity_filter_invalid"));
		}
		ECompileMessageFilter Ignored;
		FString Canonical;
		if (!TryFilter(OutRequest.SeverityFilter, Ignored, Canonical))
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_severity_filter_invalid"),
				OutRequest.SeverityFilter);
		}
		OutRequest.SeverityFilter = MoveTemp(Canonical);
		return true;
	}

	bool TryExecuteBlueprintCompilePolicy(
		UBlueprint& Blueprint,
		const FBlueprintCompilePolicyRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutReport.Reset();
		OutErrorCode.Reset();
		OutErrorDetail.Reset();
		const FString ObjectPath = Blueprint.GetPathName();
		const FString PackageName = Blueprint.GetOutermost()->GetName();
		if (Request.AssetPath != ObjectPath && Request.AssetPath != PackageName)
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_asset_path_mismatch"), Request.AssetPath);
		}
		ECompileMessageFilter Filter;
		FString CanonicalFilter;
		if (!TryFilter(Request.SeverityFilter, Filter, CanonicalFilter))
		{
			return Fail(OutErrorCode, OutErrorDetail,
				TEXT("blueprint_compile_severity_filter_invalid"));
		}

		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		bool bSaved = false;
		FString PackageFilename;
		if (Request.bSaveAfterCompile)
		{
			if (!UEditorLoadingAndSavingUtils::SavePackages(
					{Blueprint.GetOutermost()}, false))
			{
				return Fail(OutErrorCode, OutErrorDetail,
					TEXT("blueprint_compile_save_failed"), PackageName);
			}
			PackageFilename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			bSaved = !Blueprint.GetOutermost()->IsDirty()
				&& IFileManager::Get().FileExists(*PackageFilename);
			if (!bSaved)
			{
				return Fail(OutErrorCode, OutErrorDetail,
					TEXT("blueprint_compile_save_readback_failed"),
					PackageFilename);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 FilteredErrorCount = 0;
		int32 FilteredWarningCount = 0;
		int32 FilteredInfoCount = 0;
		for (const TSharedRef<FTokenizedMessage>& Message : CompilerLog.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (!Includes(Severity, Filter))
			{
				continue;
			}
			if (Severity == EMessageSeverity::Error)
			{
				++FilteredErrorCount;
			}
			else if (Severity == EMessageSeverity::Warning
				|| Severity == EMessageSeverity::PerformanceWarning)
			{
				++FilteredWarningCount;
			}
			else if (Severity == EMessageSeverity::Info)
			{
				++FilteredInfoCount;
			}
			if (!Request.bIncludeMessages
				|| Messages.Num() >= Request.MaxMessages)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("severity"), SeverityText(Severity));
			Item->SetStringField(TEXT("message"), Message->ToText().ToString());
			Item->SetStringField(
				TEXT("identifier"), Message->GetIdentifier().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutReport = MakeShared<FJsonObject>();
		OutReport->SetStringField(
			TEXT("schema"), TEXT("uai.blueprint_compile.report.v1"));
		OutReport->SetStringField(TEXT("asset_path"), PackageName);
		OutReport->SetStringField(TEXT("object_path"), ObjectPath);
		OutReport->SetStringField(TEXT("status"), StatusText(Blueprint.Status));
		OutReport->SetStringField(
			TEXT("status_readback"), StatusText(Blueprint.Status));
		OutReport->SetBoolField(TEXT("saved"), bSaved);
		OutReport->SetBoolField(
			TEXT("package_dirty_after_compile"),
			Blueprint.GetOutermost()->IsDirty());
		OutReport->SetBoolField(
			TEXT("package_file_exists"),
			!PackageFilename.IsEmpty()
				&& IFileManager::Get().FileExists(*PackageFilename));
		OutReport->SetStringField(TEXT("severity_filter"), CanonicalFilter);
		OutReport->SetNumberField(TEXT("max_messages"), Request.MaxMessages);
		OutReport->SetBoolField(
			TEXT("messages_included"), Request.bIncludeMessages);
		OutReport->SetNumberField(
			TEXT("message_total_count"), CompilerLog.Messages.Num());
		OutReport->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
		OutReport->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
		OutReport->SetNumberField(
			TEXT("filtered_error_count"), FilteredErrorCount);
		OutReport->SetNumberField(
			TEXT("filtered_warning_count"), FilteredWarningCount);
		OutReport->SetNumberField(
			TEXT("filtered_info_count"), FilteredInfoCount);
		OutReport->SetNumberField(
			TEXT("filtered_message_count"),
			FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
		OutReport->SetBoolField(
			TEXT("has_error"),
			CompilerLog.NumErrors > 0 || Blueprint.Status == BS_Error);
		OutReport->SetArrayField(TEXT("messages"), MoveTemp(Messages));
		return true;
	}
}
