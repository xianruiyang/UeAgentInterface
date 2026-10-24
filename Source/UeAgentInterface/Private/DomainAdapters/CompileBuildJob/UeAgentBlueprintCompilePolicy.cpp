#include "DomainAdapters/CompileBuildJob/UeAgentBlueprintCompilePolicy.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            OutError = MoveTemp(Diagnostic);
            return false;
        }

		FString FieldPath(FString Field)
		{
			Field.ReplaceInline(TEXT("~"), TEXT("~0"));
			Field.ReplaceInline(TEXT("/"), TEXT("~1"));
			return TEXT("/parameters/") + Field;
		}

		TSharedPtr<FJsonValue> PolicyChoices(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		Contract::FDiagnostic PolicyFact(const TCHAR* Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const TCHAR* SourceKind = TEXT("command_input"))
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("compile_build"));
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.Source.JsonPath = Path;
			Error.SourceKind = SourceKind;
			Error.TargetPath = Target;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		Contract::FDiagnostic PolicyInput(const TCHAR* Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue> Expected)
		{
			TSharedPtr<FJsonValue> Actual = Value;
			if (Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array))
				Actual = MakeShared<FJsonValueString>(Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			auto Error = PolicyFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected));
			Error.ActualValuePresent = Value.IsValid();
			Error.ExpectedValuePresent = true;
			if (Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array))
				Error.ActualEvidence = Value;
			return Error;
		}

		TSharedPtr<FJsonValue> SeverityChoices()
		{
			return PolicyChoices({TEXT("all"), TEXT("error"), TEXT("warning"), TEXT("info"), TEXT("warning_or_error")});
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequest = {};
		OutError.Reset();
		if (!Parameters.IsValid())
		{
			return Fail(OutError, PolicyInput(TEXT("blueprint_compile_parameters_invalid"),
				TEXT("Blueprint compile parameters must be an object."), TEXT("/parameters"), nullptr,
				MakeShared<FJsonValueString>(TEXT("object"))));
		}
		static const TSet<FString> Allowed = {
			TEXT("asset_path"), TEXT("include_messages"),
			TEXT("save_after_compile"), TEXT("severity_filter"),
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
			: Parameters->Values)
		{
			if (!Allowed.Contains(Field.Key))
			{
				auto Error = PolicyFact(TEXT("blueprint_compile_parameters_invalid"),
					FString::Printf(TEXT("Unknown Blueprint compile parameter '%s'."), *Field.Key), FieldPath(Field.Key),
					MakeShared<FJsonValueString>(Field.Key),
					PolicyChoices({TEXT("asset_path"), TEXT("include_messages"), TEXT("save_after_compile"), TEXT("severity_filter")}));
				Error.ActualEvidence = Field.Value;
				return Fail(OutError, MoveTemp(Error));
			}
		}
		if (!Parameters->TryGetStringField(TEXT("asset_path"), OutRequest.AssetPath)
			|| OutRequest.AssetPath.TrimStartAndEnd().IsEmpty())
		{
			return Fail(OutError, PolicyInput(TEXT("blueprint_compile_asset_path_invalid"),
				TEXT("Blueprint compile requires a non-whitespace asset_path string."), TEXT("/parameters/asset_path"),
				Parameters->TryGetField(TEXT("asset_path")), MakeShared<FJsonValueString>(TEXT("non-whitespace asset path"))));
		}
		OutRequest.AssetPath.TrimStartAndEndInline();
		if (Parameters->HasField(TEXT("include_messages"))
			&& !Parameters->TryGetBoolField(
				TEXT("include_messages"), OutRequest.bIncludeMessages))
		{
			return Fail(OutError, PolicyInput(TEXT("blueprint_compile_parameters_invalid"),
				TEXT("The include_messages parameter must be boolean."), TEXT("/parameters/include_messages"),
				Parameters->TryGetField(TEXT("include_messages")), MakeShared<FJsonValueString>(TEXT("boolean"))));
		}
		if (Parameters->HasField(TEXT("save_after_compile"))
			&& !Parameters->TryGetBoolField(
				TEXT("save_after_compile"), OutRequest.bSaveAfterCompile))
		{
			return Fail(OutError, PolicyInput(TEXT("blueprint_compile_parameters_invalid"),
				TEXT("The save_after_compile parameter must be boolean."), TEXT("/parameters/save_after_compile"),
				Parameters->TryGetField(TEXT("save_after_compile")), MakeShared<FJsonValueString>(TEXT("boolean"))));
		}
		if (Parameters->HasField(TEXT("severity_filter"))
			&& !Parameters->TryGetStringField(
				TEXT("severity_filter"), OutRequest.SeverityFilter))
		{
			return Fail(OutError, PolicyInput(TEXT("blueprint_compile_severity_filter_invalid"),
				TEXT("The severity_filter parameter must be a string."), TEXT("/parameters/severity_filter"),
				Parameters->TryGetField(TEXT("severity_filter")), MakeShared<FJsonValueString>(TEXT("string"))));
		}
		ECompileMessageFilter Ignored;
		FString Canonical;
		if (!TryFilter(OutRequest.SeverityFilter, Ignored, Canonical))
		{
			return Fail(OutError, PolicyFact(TEXT("blueprint_compile_severity_filter_invalid"),
				TEXT("Unknown Blueprint compile message severity filter."), TEXT("/parameters/severity_filter"),
				MakeShared<FJsonValueString>(OutRequest.SeverityFilter), SeverityChoices()));
		}
		OutRequest.SeverityFilter = MoveTemp(Canonical);
		return true;
	}

	bool TryExecuteBlueprintCompilePolicy(
		UBlueprint& Blueprint,
		const FBlueprintCompilePolicyRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutReport.Reset();
		OutError.Reset();
		const FString ObjectPath = Blueprint.GetPathName();
		const FString PackageName = Blueprint.GetOutermost()->GetName();
		if (Request.AssetPath != ObjectPath && Request.AssetPath != PackageName)
		{
			return Fail(OutError, PolicyFact(TEXT("blueprint_compile_asset_path_mismatch"),
				TEXT("The requested asset_path does not identify the Blueprint being compiled."), TEXT("/parameters/asset_path"),
				MakeShared<FJsonValueString>(Request.AssetPath), PolicyChoices({ObjectPath, PackageName}), ObjectPath));
		}
		ECompileMessageFilter Filter;
		FString CanonicalFilter;
		if (!TryFilter(Request.SeverityFilter, Filter, CanonicalFilter))
		{
			return Fail(OutError, PolicyFact(TEXT("blueprint_compile_severity_filter_invalid"),
				TEXT("Unknown Blueprint compile message severity filter."), TEXT("/parameters/severity_filter"),
				MakeShared<FJsonValueString>(Request.SeverityFilter), SeverityChoices(), ObjectPath));
		}

		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		bool bSaved = false;
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (Request.bSaveAfterCompile)
		{
			if (!UE::UAI::EditorResources::SavePackages(
					{Blueprint.GetOutermost()}, false, OutError))
			{
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("blueprint_compile_save_failed"), OutError, TEXT("compile_build")));
			}
			bSaved = !Blueprint.GetOutermost()->IsDirty()
				&& IFileManager::Get().FileExists(*PackageFilename);
			if (!bSaved)
			{
				const bool bPackageDirty = Blueprint.GetOutermost()->IsDirty();
				auto Error = PolicyFact(TEXT("blueprint_compile_save_readback_failed"), bPackageDirty
					? TEXT("Saving returned, but the Blueprint package is still dirty.")
					: TEXT("Saving returned, but the Blueprint package file does not exist."),
					bPackageDirty ? TEXT("package.dirty") : TEXT("package.file_exists"),
					MakeShared<FJsonValueBoolean>(bPackageDirty), MakeShared<FJsonValueBoolean>(!bPackageDirty),
					ObjectPath, TEXT("native_result"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(PackageFilename);
				return Fail(OutError, MoveTemp(Error));
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
			if (!Request.bIncludeMessages)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("severity"), SeverityText(Severity));
			Item->SetStringField(TEXT("message"), Message->ToText().ToString());
			UeAgentAuthoringDiagnostics::AppendCompilerMessageSources(*Message, &Blueprint, *Item);
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
