#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentAuthoringDiagnostics
{
	enum class ESeverity : uint8
	{
		Info,
		Warning,
		Error,
	};

	struct UEAGENTINTERFACE_API FSourceLocation
	{
		FString WorkspaceSource;
		FString SourceFile;
		FString SourceUnit;
		FString JsonPath;
		int32 Line = 0;
		int32 Column = 0;
	};

	struct UEAGENTINTERFACE_API FDiagnostic
	{
		FString Code;
		FString Stage;
		ESeverity Severity = ESeverity::Error;
		bool bRetrySafe = false;
		FString Message;
		FString SuggestedFix;
		FString TargetPath;
		FString PropertyPath;
		FString SemanticId;
		FSourceLocation Source;

		TSharedRef<FJsonObject> ToJson() const;
	};

	UEAGENTINTERFACE_API FString SeverityToString(ESeverity Severity);
	UEAGENTINTERFACE_API FString GetActionableFailureMessage(
		const FString& ErrorCode,
		const FString& ErrorDetail,
		const FString& Fallback);
}
