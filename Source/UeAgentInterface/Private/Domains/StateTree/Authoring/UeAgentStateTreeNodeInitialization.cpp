#include "Domains/StateTree/Authoring/UeAgentStateTreeNodeInitialization.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTreeEditorNode.h"

namespace UeAgentStateTreeAuthoring
{
	bool InitializeNode(FStateTreeEditorNode& Node, UObject& Outer, const UStruct* Type, UE::UAI::Contract::FDiagnostic& Error)
	{
		Node.Reset(); Error.Reset();
		if (const auto* Struct = Cast<UScriptStruct>(Type); Struct && Struct->IsChildOf(FStateTreeNodeBase::StaticStruct()))
			Node.Node.InitializeAs(Struct);
		else if (auto* Class = const_cast<UClass*>(Cast<UClass>(Type)))
		{
			if (Class->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()))
			{ Node.Node.InitializeAs<FStateTreeBlueprintTaskWrapper>(); Node.Node.GetMutable<FStateTreeBlueprintTaskWrapper>().TaskClass = Class; }
			else if (Class->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()))
			{ Node.Node.InitializeAs<FStateTreeBlueprintConditionWrapper>(); Node.Node.GetMutable<FStateTreeBlueprintConditionWrapper>().ConditionClass = Class; }
			else if (Class->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()))
			{ Node.Node.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>(); Node.Node.GetMutable<FStateTreeBlueprintEvaluatorWrapper>().EvaluatorClass = Class; }
			else if (Class->IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass()))
			{ Node.Node.InitializeAs<FStateTreeBlueprintConsiderationWrapper>(); Node.Node.GetMutable<FStateTreeBlueprintConsiderationWrapper>().ConsiderationClass = Class; }
		}
		if (!Node.Node.IsValid()) {
			const auto* Struct = Cast<UScriptStruct>(Type);
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_node_type_invalid"),
				!Type ? TEXT("A StateTree node type is required.")
					: Struct && !Struct->IsChildOf(FStateTreeNodeBase::StaticStruct()) ? TEXT("The selected structure does not derive from StateTreeNodeBase.")
					: Cast<UClass>(Type) ? TEXT("The class is not a supported StateTree task, condition, evaluator, or consideration Blueprint type.")
					: TEXT("Unreal could not initialize the selected StateTree node structure."),
				TEXT("state_tree_node_initialize"));
			Error.PropertyPath = TEXT("node_type");
			Error.ActualValuePresent = Type != nullptr;
			if (Type) Error.ActualValue = MakeShared<FJsonValueString>(Type->GetPathName());
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("StateTreeNodeBase structure or StateTree task/condition/evaluator/consideration Blueprint subclass"));
			return false;
		}
		const UStruct* InstanceType = Node.Node.Get<FStateTreeNodeBase>().GetInstanceDataType();
		if (const auto* Struct = Cast<UScriptStruct>(InstanceType)) Node.Instance.InitializeAs(Struct);
		else if (auto* Class = const_cast<UClass*>(Cast<UClass>(InstanceType)))
			Node.InstanceObject = NewObject<UObject>(&Outer, Class, NAME_None, RF_Transactional);
		Node.ID = FGuid::NewGuid();
		return true;
	}
}
