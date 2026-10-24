// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "MaterialShared.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

class FJsonObject;
class UMaterialExpression;
struct FExpressionInput;
struct FMaterialExpressionCollection;
namespace UE::UAI::HjsonWorkspace { struct FGraphAuthoringConnectionContext; }

namespace UeAgentMaterialAuthoring
{
	enum class EGraphApplyPolicy : uint8 { Replace, PreserveExisting };
	struct FGraphExpressionBinding
	{
		FGuid Guid;
		FString ClassPath;
		FString FunctionPath;
	};
	struct FGraphApplyContext
	{
		EGraphApplyPolicy Policy = EGraphApplyPolicy::Replace;
		TMap<FString, FGraphExpressionBinding> ExpressionsByAlias;
		TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> Connections;
	};
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
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FMaterialExpressionCollection* GetExpressionCollection(UObject& Owner);
	UEAGENTINTERFACE_API const FMaterialExpressionCollection* GetExpressionCollection(const UObject& Owner);
	// Both native collections contain graph nodes. Keep discovery, selection and
	// mutation on the same enumeration, including comments in materials/functions.
	UEAGENTINTERFACE_API TArray<UMaterialExpression*> GetGraphExpressions(const FMaterialExpressionCollection& Collection);
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
	UEAGENTINTERFACE_API UE::UAI::Contract::FDiagnostic DescribeExpressionPinFailure(UMaterialExpression& Expression,
		const FString& Name, bool bInput, const FString& Field);
	UEAGENTINTERFACE_API bool ConnectExpressionToProperty(
		UMaterialExpression& Expression,
		const FString& OutputName,
		EMaterialProperty Property,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API void RefreshGraphOwner(UObject& Owner);
	UEAGENTINTERFACE_API bool ConnectExpressions(
		UMaterialExpression& FromExpression,
		const FString& FromOutputName,
		UMaterialExpression& ToExpression,
		const FString& ToInputName,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildExpressionPinInterface(
		UMaterialExpression& Expression);

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
			UE::UAI::Contract::FDiagnostic& OutError,
			const FGraphApplyContext& Context = {});
		bool CaptureRollback(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError);
		bool ApplyTo(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError);
		bool VerifyIntent(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError) const;
		bool AppendPropertyReadRequests(
			UObject& Owner,
			const FString& AssetPath,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const;
		bool Rollback(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError);
		void Commit();
		void ReleaseForPersistentReadback();

		bool IsReady() const;
		bool PreservesExisting() const;
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
		UE::UAI::Contract::FDiagnostic& OutError);
}
