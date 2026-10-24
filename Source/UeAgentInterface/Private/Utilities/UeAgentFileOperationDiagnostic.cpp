#include "Utilities/UeAgentFileOperationDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"

namespace UE::UAI::FileDiagnostics
{
	Contract::FDiagnostic MakeFileOperationFailure(
		const TCHAR* Code, const FString& PublicReason, const FString& Stage,
		const FString& InternalFilename)
	{
		const uint32 ErrorCode = FPlatformMisc::GetLastError();
		Contract::FDiagnostic Error = Contract::FDiagnostic::Failure(Code, PublicReason, Stage);
		Error.SourceKind = TEXT("native_result");
		Error.InternalContext = InternalFilename;
		if (ErrorCode != 0)
		{
			TCHAR Message[1024] = {};
			FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), ErrorCode);
			const FString SystemReason = FString(Message).TrimStartAndEnd();
			// High-level file APIs do not guarantee that this thread-local value belongs to their failure.
			TSharedRef<FJsonObject> ObservedSystemError = MakeShared<FJsonObject>();
			ObservedSystemError->SetNumberField(TEXT("observed_system_error"), ErrorCode);
			ObservedSystemError->SetStringField(TEXT("observed_system_message"), SystemReason);
			Error.ActualEvidence = MakeShared<FJsonValueObject>(ObservedSystemError);
		}
		return Error;
	}
}
