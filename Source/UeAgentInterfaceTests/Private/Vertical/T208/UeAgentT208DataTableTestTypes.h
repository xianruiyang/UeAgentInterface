#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "UeAgentT208DataTableTestTypes.generated.h"

UCLASS(EditInlineNew, DefaultToInstanced)
class UUeAgentT208DataTableOwnedValue : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere)
	int32 Amount = 1;
};

USTRUCT()
struct FUeAgentT208DataTableOwnedRow : public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere)
	int32 Count = 1;
	UPROPERTY(EditAnywhere, Instanced)
	TObjectPtr<UUeAgentT208DataTableOwnedValue> Detail;
	UPROPERTY(VisibleAnywhere)
	int32 ReadOnly = 7;
};
