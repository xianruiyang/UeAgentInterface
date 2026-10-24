// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class UObject;

namespace UeAgentAuthoringOperationApply
{
	using UE::UAI::Contract::FDiagnostic;
	class UEAGENTINTERFACE_API FAssetRemovalStage
	{
	public:
		bool Capture(UObject* InAsset, FDiagnostic& OutError);
		bool Stage(FDiagnostic& OutError);
		bool Rollback(FDiagnostic& OutError);
		void Reset();

		UObject* GetAsset() const { return Asset; }
		bool IsCaptured() const { return Asset != nullptr; }
		bool IsStaged() const { return bStaged; }

	private:
		UObject* Asset = nullptr;
		UObject* OriginalOuter = nullptr;
		FName OriginalName;
		FString OriginalObjectPath;
		FName StagedName;
		bool bStaged = false;
	};
}
