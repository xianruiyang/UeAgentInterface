#include "QueryOwners/AiBehavior/UeAgentEqsHeadlessSchema.h"

#include "EnvironmentQuery/EnvQueryNode.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_BlueprintBase.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"

void UUeAgentEqsHeadlessSchema::InitializeHeadlessClassCache()
{
	if (ClassCache) return;
	ClassCache = MakeShared<FGraphNodeClassHelper>(UEnvQueryNode::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(UEnvQueryGenerator_BlueprintBase::StaticClass());
	ClassCache->UpdateAvailableBlueprintClasses();
}

void UUeAgentEqsHeadlessSchema::GetGraphContextActions(FGraphContextMenuBuilder& Builder) const
{
	if (!ClassCache || (Builder.FromPin && !Builder.FromPin->GetOwningNode()->IsA<UEnvironmentQueryGraphNode_Root>())) return;
	TArray<FGraphNodeClassData> Classes;
	ClassCache->GatherClasses(UEnvQueryGenerator::StaticClass(), Classes);
	FCategorizedGraphActionListBuilder Generators(TEXT("Generators"));
	for (const auto& Class : Classes)
	{
		auto* Node = NewObject<UEnvironmentQueryGraphNode_Option>(Builder.OwnerOfTemporaries);
		Node->ClassData = Class;
		auto Action = AddNewNodeAction(Generators, Class.GetCategory(),
			FText::FromString(FName::NameToDisplayString(Class.ToString(), false)), FText::GetEmpty());
		Action->NodeTemplate = Node;
	}
	Builder.Append(Generators);
}

void UUeAgentEqsHeadlessSchema::GetSubNodeClasses(int32 Flags, TArray<FGraphNodeClassData>& Classes, UClass*& NodeClass) const
{
	Classes.Reset();
	// The engine Test graph class has no exported C++ API in UE 5.6.
	NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Test"));
	if (ClassCache && NodeClass) ClassCache->GatherClasses(UEnvQueryTest::StaticClass(), Classes);
}
