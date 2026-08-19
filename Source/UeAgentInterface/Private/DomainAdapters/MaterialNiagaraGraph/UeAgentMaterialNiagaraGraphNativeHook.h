#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphAdapter.h"
#include "MaterialShared.h"
#include "NiagaraCommon.h"
#include "NiagaraTypes.h"

class UClass;
class UMaterialExpression;
class UMaterialInstanceConstant;
class UTexture;
class UNiagaraNodeFunctionCall;
class UNiagaraEmitter;
class UNiagaraScript;
class UNiagaraSystem;
class UObject;

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	class FMaterialNiagaraGraphEffectiveParameter final
	{
	public:
		FMaterialNiagaraGraphEffectiveParameter(
			FMaterialNiagaraGraphParameterIdentity InIdentity,
			bool bInOverridden,
			float InScalarValue,
			FLinearColor InVectorValue,
			FString InTextureRef,
			bool bInBooleanValue);

		const FMaterialNiagaraGraphParameterIdentity& GetIdentity() const;
		bool IsOverridden() const;
		float GetScalarValue() const;
		const FLinearColor& GetVectorValue() const;
		const FString& GetTextureRef() const;
		bool GetBooleanValue() const;
		bool IsValid() const;

	private:
		FMaterialNiagaraGraphParameterIdentity Identity;
		bool bOverridden = false;
		float ScalarValue = 0.0f;
		FLinearColor VectorValue = FLinearColor::Black;
		FString TextureRef;
		bool bBooleanValue = false;
		bool bValid = false;
	};

	class FMaterialNiagaraGraphNativeMutationReceipt final
	{
	public:
		const FString& GetEvidenceId() const { return EvidenceId; }
		void SetEvidenceId(FString InEvidenceId)
		{
			EvidenceId = MoveTemp(InEvidenceId);
		}

	private:
		FString EvidenceId;
	};

	class FMaterialNiagaraGraphCompileSettleReceipt final
	{
	public:
		bool IsCompilationComplete() const { return bCompilationComplete; }
		int32 GetCompileErrorCount() const { return CompileErrorCount; }
		int32 GetStackIssueCount() const { return StackIssueCount; }
		const FString& GetCompileEvidenceId() const
		{
			return CompileEvidenceId;
		}
		const FString& GetSettleEvidenceId() const
		{
			return SettleEvidenceId;
		}

	private:
		friend class FMaterialNiagaraGraphNativeHook;
		bool bCompilationComplete = false;
		int32 CompileErrorCount = 0;
		int32 StackIssueCount = 0;
		FString CompileEvidenceId;
		FString SettleEvidenceId;
	};

	class UEAGENTINTERFACE_API FMaterialNiagaraGraphNativeHook final
	{
	public:
		bool TryAddMaterialExpression(
			UObject& Owner,
			UClass& ExpressionClass,
			const FGuid& ExpressionGuid,
			int32 PositionX,
			int32 PositionY,
			UMaterialExpression*& OutExpression,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryConnectMaterialExpressionToProperty(
			UMaterialExpression& Expression,
			const FString& OutputName,
			EMaterialProperty Property,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryConnectMaterialExpressionToPropertyByIndex(
			UMaterialExpression& Expression,
			int32 OutputIndex,
			EMaterialProperty Property,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryConnectMaterialExpressionsByIndex(
			UMaterialExpression& FromExpression,
			int32 FromOutputIndex,
			UMaterialExpression& ToExpression,
			const FString& ToInputName,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialExpressionLinearColorProperty(
			UMaterialExpression& Expression,
			const FString& PropertyPath,
			const FLinearColor& Value,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialExpressionNameProperty(
			UMaterialExpression& Expression,
			const FString& PropertyPath,
			const FName& Value,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCaptureMaterialGraph(
			UObject& Owner,
			const FString& TargetIdentity,
			FMaterialNiagaraGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialInstanceScalarParameter(
			UMaterialInstanceConstant& MaterialInstance,
			const FMaterialParameterInfo& ParameterInfo,
			float Value,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialInstanceVectorParameter(
			UMaterialInstanceConstant& MaterialInstance,
			const FMaterialParameterInfo& ParameterInfo,
			const FLinearColor& Value,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialInstanceTextureParameter(
			UMaterialInstanceConstant& MaterialInstance,
			const FMaterialParameterInfo& ParameterInfo,
			UTexture& Value,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetMaterialInstanceStaticSwitchParameter(
			UMaterialInstanceConstant& MaterialInstance,
			const FMaterialParameterInfo& ParameterInfo,
			bool bValue,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCaptureMaterialInstanceEffectiveParameters(
			UMaterialInstanceConstant& MaterialInstance,
			TArray<FMaterialNiagaraGraphEffectiveParameter>& OutParameters,
			FString& OutRevision,
			FString& OutEvidenceId,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCaptureMaterialInstanceParameters(
			UMaterialInstanceConstant& MaterialInstance,
			TArray<FMaterialNiagaraGraphParameterIdentity>& OutParameters,
			FString& OutRevision,
			FString& OutEvidenceId,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCompileAndSettleMaterial(
			UObject& Owner,
			FMaterialNiagaraGraphCompileSettleReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryAddNiagaraAssignmentModule(
			UObject& Asset,
			const FGuid& EmitterHandleId,
			ENiagaraScriptUsage Usage,
			const FGuid& UsageId,
			const TArray<FNiagaraVariable>& AssignmentTargets,
			int32 TargetIndex,
			UNiagaraNodeFunctionCall*& OutModule,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryMoveNiagaraModule(
			UObject& Asset,
			const FGuid& EmitterHandleId,
			ENiagaraScriptUsage Usage,
			const FGuid& UsageId,
			UNiagaraNodeFunctionCall& Module,
			int32 TargetIndex,
			int32& OutNewIndex,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCaptureNiagaraStack(
			UObject& Asset,
			const FGuid& EmitterHandleId,
			ENiagaraScriptUsage Usage,
			const FGuid& UsageId,
			FMaterialNiagaraGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetNiagaraSystemWarmupTime(
			UNiagaraSystem& System,
			float WarmupTime,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetNiagaraEmitterLocalSpace(
			UNiagaraEmitter& Emitter,
			bool bLocalSpace,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TrySetNiagaraScriptDescription(
			UNiagaraScript& Script,
			const FText& Description,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;

		bool TryCompileAndSettleNiagara(
			UObject& Asset,
			FMaterialNiagaraGraphCompileSettleReceipt& OutReceipt,
			FMaterialNiagaraGraphAdapterError& OutError) const;
	};
}
