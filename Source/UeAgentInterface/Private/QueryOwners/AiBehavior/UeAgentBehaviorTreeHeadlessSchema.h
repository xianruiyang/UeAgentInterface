#pragma once

#include "AIGraphTypes.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraphSchema_BehaviorTreeDecorator.h"

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

/** The owned decorator graph consumes the same class cache and native schema menu. */
UCLASS()
class UUeAgentBehaviorTreeDecoratorHeadlessSchema final : public UEdGraphSchema_BehaviorTreeDecorator
{
	GENERATED_BODY()

public:
	void SetClassCacheOwner(UUeAgentBehaviorTreeHeadlessSchema& Owner);

protected:
	virtual FGraphNodeClassHelper& GetClassCache() const override;

private:
	UPROPERTY()
	TObjectPtr<UUeAgentBehaviorTreeHeadlessSchema> ClassCacheOwner;
};
