#pragma once

#include "AIGraphTypes.h"
#include "EdGraphSchema_BehaviorTree.h"

#include "UeAgentBehaviorTreeHeadlessSchema.generated.h"

/**
 * Reuses the engine BehaviorTree schema menu without creating an asset editor.
 * UE 5.6 initializes the schema's class cache only from the UI editor entry;
 * this adapter supplies the same cache initialization while leaving the menu
 * implementation and returned native actions owned by the engine schema.
 */
UCLASS()
class UUeAgentBehaviorTreeHeadlessSchema final
	: public UEdGraphSchema_BehaviorTree
{
	GENERATED_BODY()

public:
	void InitializeHeadlessClassCache();
	FGraphNodeClassHelper& GetHeadlessClassCache() const;

protected:
	virtual FGraphNodeClassHelper& GetClassCache() const override;

private:
	TSharedPtr<FGraphNodeClassHelper> HeadlessClassCache;
};
