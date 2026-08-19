// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UeAgentAuthoringOperationApply
{
	class UEAGENTINTERFACE_API FAssetRemovalStage
	{
	public:
		bool Capture(UObject* InAsset, FString& OutError);
		bool Stage(FString& OutError);
		bool Rollback(FString& OutError);
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
