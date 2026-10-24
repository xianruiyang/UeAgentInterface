#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::FileDiagnostics
{
	// Capture immediately after a failed filesystem operation, before cleanup I/O.
	// The declared operation failure is public; the filename and observed OS error are internal.
	UEAGENTINTERFACE_API Contract::FDiagnostic MakeFileOperationFailure(
		const TCHAR* Code, const FString& PublicReason, const FString& Stage,
		const FString& InternalFilename);
}
