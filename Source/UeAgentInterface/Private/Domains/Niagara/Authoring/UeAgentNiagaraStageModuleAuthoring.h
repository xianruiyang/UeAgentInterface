// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "NiagaraTypes.h"

class UEdGraphPin;
class FJsonObject;
class UNiagaraEmitter;
class UNiagaraGraph;
class UNiagaraNode;
class UNiagaraNodeFunctionCall;
class UNiagaraNodeInput;
class UNiagaraNodeOutput;
class UNiagaraDataInterface;
class UNiagaraScript;
class UNiagaraScriptSource;
class UNiagaraSystem;
class UObject;
struct FAssetData;
struct FVersionedNiagaraEmitterData;

namespace UeAgentNiagaraStageModuleAuthoring
{
	struct FStageTarget
	{
		FString AssetPath;
		FString EmitterName;
		FGuid EmitterHandleId;
		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		bool bEmitterHandleIdExplicit = false;
		bool bUsageExplicit = false;
		bool bUsageIdExplicit = false;
	};

	struct FResolvedStage
	{
		UObject* Asset = nullptr;
		UNiagaraSystem* System = nullptr;
		UNiagaraEmitter* Emitter = nullptr;
		FGuid EmitterVersion;
		FGuid EmitterHandleId;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
		UNiagaraNodeOutput* OutputNode = nullptr;
		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		FString EmitterName;
		FString StageKey;
	};

	struct FEmitterOwnerTarget
	{
		FString AssetPath;
		FString EmitterName;
		FGuid EmitterHandleId;
		bool bEmitterHandleIdExplicit = false;
	};

	struct FResolvedEmitterOwner
	{
		UObject* Asset = nullptr;
		UNiagaraSystem* System = nullptr;
		UNiagaraEmitter* Emitter = nullptr;
		FVersionedNiagaraEmitterData* EmitterData = nullptr;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
		FGuid EmitterVersion;
		FGuid EmitterHandleId;
		FString EmitterName;
	};

	struct FModuleSelector
	{
		FGuid NodeGuid;
		FString ScriptPath;
		FString Name;
		int32 Occurrence = 0;
		bool bOccurrenceExplicit = false;

		bool IsEmpty() const;
	};

	struct FDynamicInputSelector
	{
		FGuid NodeGuid;
		FModuleSelector Module;
		FString InputName;
	};

	struct FResolvedModuleInput
	{
		FNiagaraVariable InputVariable;
		FString FullInputName;
		FString ShortInputName;
		FGuid InputScriptVariableGuid;
		UEdGraphPin* VisiblePin = nullptr;
	};

	UEAGENTINTERFACE_API FString ScriptUsageToString(ENiagaraScriptUsage Usage);
	UEAGENTINTERFACE_API bool TryParseScriptUsage(const FString& UsageText, ENiagaraScriptUsage& OutUsage);
	UEAGENTINTERFACE_API FString MakeStageKey(ENiagaraScriptUsage Usage, const FGuid& UsageId);
	UEAGENTINTERFACE_API FString GetStageOwnerId(const FResolvedStage& Stage);
	UEAGENTINTERFACE_API TMap<FString, FString> BuildStageIdentitySelectors(
		const FResolvedStage& Stage, const FString& CanonicalAssetPath);

	UEAGENTINTERFACE_API bool ResolveStage(
		const FStageTarget& Target,
		FResolvedStage& OutStage,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool EnumerateStages(
		UObject& Asset,
		TArray<FResolvedStage>& OutStages,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool EnumerateEmitterOwners(
		UObject& Asset,
		TArray<FResolvedEmitterOwner>& OutOwners,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ResolveEmitterOwner(
		const FEmitterOwnerTarget& Target,
		FResolvedEmitterOwner& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError,
		UObject* RootObject = nullptr);
	UEAGENTINTERFACE_API UNiagaraNodeOutput* EnsureStageBridge(
		UNiagaraGraph& Graph,
		ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RemoveStageGraph(
		UNiagaraGraph& Graph,
		ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool GetParameterMapChainNodes(
		UNiagaraNodeOutput& OutputNode,
		TArray<UNiagaraNode*>& OutNodes,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateGraphInputDependenciesAcyclic(
		UNiagaraGraph& Graph,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetModules(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules, UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResolveModule(
		UNiagaraNodeOutput& OutputNode,
		const FModuleSelector& Selector,
		UNiagaraNodeFunctionCall*& OutModuleNode,
		int32& OutModuleIndex,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API UNiagaraNodeFunctionCall* AddModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraScript& ModuleScript,
		const FString& SuggestedName,
		int32 TargetIndex,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UNiagaraNodeFunctionCall* AddAssignmentModule(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		int32 TargetIndex,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool MoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		int32 TargetIndex,
		int32& OutNewIndex,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool RemoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API FString GetModuleScriptPath(const UNiagaraNodeFunctionCall& ModuleNode);
	UEAGENTINTERFACE_API bool ResolveNiagaraTypeDefinition(
		const FString& TypeText,
		FNiagaraTypeDefinition& OutTypeDefinition,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ResolveAssignmentTargetVariable(
		const FString& VariableName,
		const FString& OptionalTypeText,
		FNiagaraVariable& OutVariable,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString NormalizeAssignmentTargetName(const FString& VariableName);
	UEAGENTINTERFACE_API FString GetNiagaraTypeToken(const FNiagaraTypeDefinition& TypeDefinition);
	UEAGENTINTERFACE_API bool BuildAssignmentTargets(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutTargets,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API void BuildResolvedModuleInputs(
		UNiagaraNodeFunctionCall& ModuleNode,
		TArray<FResolvedModuleInput>& OutInputs);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildModuleInputSchema(
		UNiagaraNodeFunctionCall& ModuleNode, const UObject* OwningAsset = nullptr);
	UEAGENTINTERFACE_API UEdGraphPin* FindResolvedInputValuePin(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FResolvedModuleInput& Input);
	UEAGENTINTERFACE_API const FResolvedModuleInput* ResolveModuleInputByName(
		const TArray<FResolvedModuleInput>& ModuleInputs,
		const FString& InputName,
		FString& OutResolvedInputName);
	UEAGENTINTERFACE_API UE::UAI::Contract::FDiagnostic BuildInputNameNotFoundError(
		const FString& InputName,
		const FString& OwnerScriptPath,
		const TArray<FString>& AvailableInputNames);
	UEAGENTINTERFACE_API bool BuildLiteralInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildInputValueForPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		TSharedPtr<FJsonObject>& OutPayload,
		bool& bOutHasValue,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool DoesInputValueMatchPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutMatches,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool DoesInputValueMatch(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutMatches,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetLiteralInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetLinkedInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const FString& LinkedParameterName,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetLiteralInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetLinkedInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const FString& LinkedParameterName,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UNiagaraNodeFunctionCall* FindDynamicInputForModuleInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		FString& OutResolvedInputName, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UNiagaraNodeFunctionCall* FindDynamicInputForPin(const UEdGraphPin& InputPin);
	UEAGENTINTERFACE_API bool ClearInputValueNodesForPin(UEdGraphPin& InputPin, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ResolveDynamicInput(
		FResolvedStage& Stage,
		const FDynamicInputSelector& Selector,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		FString& OutResolvedInputName,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetDynamicInputForPin(
		UEdGraphPin& InputPin,
		UNiagaraScript& DynamicInputScript,
		const FString& SuggestedName,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ClearDynamicInputForPin(
		UEdGraphPin& InputPin,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SetDynamicInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		UNiagaraScript& DynamicInputScript,
		const FString& SuggestedName,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		FString& OutResolvedInputName,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool InitializeDynamicInputDataInterfaces(
		UNiagaraNodeFunctionCall& DynamicInputNode,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ResolveDynamicInputDataInterface(
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const FString& InputName,
		UNiagaraNodeInput*& OutInputNode,
		UNiagaraDataInterface*& OutDataInterface,
		FString& OutResolvedInputName,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ClearDynamicInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		FString& OutResolvedInputName,
		UE::UAI::Contract::FDiagnostic& OutError);
}
