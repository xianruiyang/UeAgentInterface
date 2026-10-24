// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

class FJsonValue;
class UObject;
class UStruct;

namespace UeAgentAuthoringDirectApply
{
	struct UEAGENTINTERFACE_API FResult
	{
		FString Status;
		FString Phase;
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
		int32 PropertiesSeen = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;
		TArray<UeAgentAuthoringObjectApply::FPropertyIntent> PropertyIntents;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool ApplyObjectPatch(
		UObject* Object,
		const TArray<TSharedPtr<FJsonValue>>& PropertyDescriptors,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		FResult& OutResult);

	UEAGENTINTERFACE_API bool ApplyStructPatch(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TArray<TSharedPtr<FJsonValue>>& PropertyDescriptors,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		FResult& OutResult);
}
