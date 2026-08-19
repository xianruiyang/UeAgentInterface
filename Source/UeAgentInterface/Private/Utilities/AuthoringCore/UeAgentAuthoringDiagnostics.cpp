#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"

namespace UeAgentAuthoringDiagnostics
{
	FString GetActionableFailureMessage(
		const FString& ErrorCode,
		const FString& ErrorDetail,
		const FString& Fallback)
	{
		const FString Detail = ErrorDetail.TrimStartAndEnd();
		if (!Detail.IsEmpty()) return Detail;
		const FString Code = ErrorCode.TrimStartAndEnd();
		return Code.IsEmpty() ? Fallback : Code;
	}

	FString SeverityToString(const ESeverity Severity)
	{
		switch (Severity)
		{
		case ESeverity::Info:
			return TEXT("info");
		case ESeverity::Warning:
			return TEXT("warning");
		case ESeverity::Error:
		default:
			return TEXT("error");
		}
	}

	TSharedRef<FJsonObject> FDiagnostic::ToJson() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("code"), Code);
		Object->SetStringField(TEXT("stage"), Stage);
		Object->SetStringField(TEXT("severity"), SeverityToString(Severity));
		Object->SetBoolField(TEXT("retry_safe"), bRetrySafe);
		Object->SetStringField(TEXT("message"), Message);
		if (!SuggestedFix.IsEmpty())
		{
			Object->SetStringField(TEXT("suggested_fix"), SuggestedFix);
		}
		if (!TargetPath.IsEmpty())
		{
			Object->SetStringField(TEXT("target_path"), TargetPath);
		}
		if (!PropertyPath.IsEmpty())
		{
			Object->SetStringField(TEXT("property_path"), PropertyPath);
		}
		if (!SemanticId.IsEmpty())
		{
			Object->SetStringField(TEXT("semantic_id"), SemanticId);
		}

		TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
		SourceObject->SetStringField(TEXT("workspace_source"), Source.WorkspaceSource);
		SourceObject->SetStringField(TEXT("source_file"), Source.SourceFile);
		SourceObject->SetStringField(TEXT("source_unit"), Source.SourceUnit);
		SourceObject->SetStringField(TEXT("json_path"), Source.JsonPath);
		SourceObject->SetNumberField(TEXT("line"), Source.Line);
		SourceObject->SetNumberField(TEXT("column"), Source.Column);
		Object->SetObjectField(TEXT("source"), SourceObject);
		return Object;
	}
}
