// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MaterialShared.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

class FJsonObject;
class UMaterialExpression;
struct FExpressionInput;
struct FMaterialExpressionCollection;

namespace UeAgentMaterialAuthoring
{
	struct UEAGENTINTERFACE_API FExpressionSelector
	{
		FGuid ExpressionGuid;
		FString ExpressionName;
		int32 ExpressionIndex = INDEX_NONE;

		bool HasAny() const
		{
			return ExpressionGuid.IsValid() || !ExpressionName.IsEmpty() || ExpressionIndex != INDEX_NONE;
		}
	};

	UEAGENTINTERFACE_API FString NormalizeExpressionPinName(const FString& PinName);
	UEAGENTINTERFACE_API bool ParseMaterialProperty(const FString& PropertyName, EMaterialProperty& OutProperty);
	UEAGENTINTERFACE_API FString MaterialPropertyToString(EMaterialProperty Property);
	UEAGENTINTERFACE_API TArray<EMaterialProperty> GetSupportedMaterialProperties();
	UEAGENTINTERFACE_API UMaterialExpression* ResolveExpressionUnique(
		UObject& Owner,
		const FExpressionSelector& Selector,
		FString& OutError);
	UEAGENTINTERFACE_API FMaterialExpressionCollection* GetExpressionCollection(UObject& Owner);
	UEAGENTINTERFACE_API const FMaterialExpressionCollection* GetExpressionCollection(const UObject& Owner);
	UEAGENTINTERFACE_API UMaterialExpression* CreateExpression(
		UObject& Owner,
		UClass* ExpressionClass,
		int32 X,
		int32 Y);
	UEAGENTINTERFACE_API bool ResolveExpressionInput(
		UMaterialExpression& Expression,
		const FString& InputName,
		FExpressionInput*& OutInput);
	UEAGENTINTERFACE_API bool ResolveExpressionOutputIndex(
		UMaterialExpression& Expression,
		const FString& OutputName,
		int32& OutIndex);
	UEAGENTINTERFACE_API bool ConnectExpressionToProperty(
		UMaterialExpression& Expression,
		const FString& OutputName,
		EMaterialProperty Property,
		FString& OutError);
	UEAGENTINTERFACE_API void RefreshGraphOwner(UObject& Owner);
	UEAGENTINTERFACE_API bool ConnectExpressions(
		UMaterialExpression& FromExpression,
		const FString& FromOutputName,
		UMaterialExpression& ToExpression,
		const FString& ToInputName,
		FString& OutError);

	class UEAGENTINTERFACE_API FPreparedGraph
	{
	public:
		FPreparedGraph();
		~FPreparedGraph();
		FPreparedGraph(FPreparedGraph&&) noexcept;
		FPreparedGraph& operator=(FPreparedGraph&&) noexcept;
		FPreparedGraph(const FPreparedGraph&) = delete;
		FPreparedGraph& operator=(const FPreparedGraph&) = delete;

		bool Prepare(
			UObject& ValidationOwner,
			const TSharedPtr<const FJsonObject>& GraphDocument,
			const FString& ContextAssetPath,
			const FString& ContextGraphName,
			FString& OutError);
		bool CaptureRollback(UObject& Owner, FString& OutError);
		bool ApplyTo(UObject& Owner, FString& OutError);
		bool AppendPropertyReadRequests(
			UObject& Owner,
			const FString& AssetPath,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const;
		bool Rollback(UObject& Owner, FString& OutError);
		void Commit();

		bool IsReady() const;
		int32 GetExpressionCount() const;
		int32 GetEdgeCount() const;
		int32 GetRootInputCount() const;

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	UEAGENTINTERFACE_API bool BuildGraphState(
		const UObject& Owner,
		TSharedPtr<FJsonObject>& OutState,
		FString& OutError);
}
