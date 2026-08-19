// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UPCGGraph;

namespace UeAgentPCGAuthoring
{
	struct UEAGENTINTERFACE_API FGraphApplyResult
	{
		int32 NodeCount = 0;
		int32 EdgeCount = 0;
		int32 NodePropertiesApplied = 0;
	};

	UEAGENTINTERFACE_API FString MakeEdgeKey(
		const FString& FromNode,
		const FString& FromPin,
		const FString& ToNode,
		const FString& ToPin);
	UEAGENTINTERFACE_API bool BuildGraphDocument(
		UPCGGraph& Graph,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildBackendGraphDocument(
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		TSharedPtr<FJsonObject>& OutBackendDocument,
		FString& OutError);
	UEAGENTINTERFACE_API bool ApplyGraphDocument(
		UPCGGraph& Graph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		FString& OutError);
	UEAGENTINTERFACE_API bool PreflightGraphDocument(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		FString& OutError);
}

