#pragma once

#include "AIGraphTypes.h"
#include "AIGraphSchema.h"
#include "UeAgentEqsHeadlessSchema.generated.h"

/** EQS binds its native class cache to the UI module. Supply the same class families
 * without opening an asset editor; mutation remains in the native AI schema actions. */
UCLASS()
class UUeAgentEqsHeadlessSchema final : public UAIGraphSchema
{
	GENERATED_BODY()
public:
	void InitializeHeadlessClassCache();
	virtual void GetGraphContextActions(FGraphContextMenuBuilder& Builder) const override;
	virtual void GetSubNodeClasses(int32 Flags, TArray<FGraphNodeClassData>& Classes, UClass*& NodeClass) const override;
private:
	TSharedPtr<FGraphNodeClassHelper> ClassCache;
};
