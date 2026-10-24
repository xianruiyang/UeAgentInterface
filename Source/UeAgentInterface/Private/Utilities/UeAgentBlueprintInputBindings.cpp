#include "Utilities/UeAgentBlueprintInputBindings.h"

#include "InputCoreTypes.h"
#include "K2Node.h"
#include "K2Node_GetInputAxisValue.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "UObject/UnrealType.h"

void UeAgentBlueprintInputBindings::Capture(
	const UK2Node& Node, TMap<FString, FString>& OutFields)
{
	// FKey also covers inherited axis nodes and plugin-private debug nodes.
	// A native key name is stable across editor cultures; menu text is not.
	TSet<FName> BoundKeys;
	for (TFieldIterator<FStructProperty> Property(Node.GetClass()); Property; ++Property)
	{
		if (Property->Struct != FKey::StaticStruct()) continue;
		const FKey* Key = Property->ContainerPtrToValuePtr<FKey>(&Node);
		if (Key && Key->IsValid()) BoundKeys.Add(Key->GetFName());
	}
	if (BoundKeys.Num() == 1)
	{
		OutFields.Add(TEXT("input_key"), BoundKeys.CreateConstIterator()->ToString());
	}

	if (const auto* Action = Cast<UK2Node_InputAction>(&Node))
	{
		if (!Action->InputActionName.IsNone())
			OutFields.Add(TEXT("input_action"), Action->InputActionName.ToString());
	}
	FName AxisName;
	if (const auto* AxisEvent = Cast<UK2Node_InputAxisEvent>(&Node))
	{
		AxisName = AxisEvent->InputAxisName;
	}
	else if (const auto* AxisValue = Cast<UK2Node_GetInputAxisValue>(&Node))
	{
		AxisName = AxisValue->InputAxisName;
	}
	if (!AxisName.IsNone()) OutFields.Add(TEXT("input_axis"), AxisName.ToString());
}
