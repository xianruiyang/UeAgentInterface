#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UeAgentRecursiveDataAsset.generated.h"

UCLASS(EditInlineNew, DefaultToInstanced)
class UEAGENTINTERFACE_API UUeAgentRecursiveLeafObject final : public UObject
{
	GENERATED_BODY()

public:
	UUeAgentRecursiveLeafObject();

	UPROPERTY(EditAnywhere, Category = "UAI")
	int32 NativeDefaultValue = 29;
};

UCLASS(EditInlineNew, DefaultToInstanced)
class UEAGENTINTERFACE_API UUeAgentRecursiveOwnedObject final : public UObject
{
	GENERATED_BODY()

public:
	UUeAgentRecursiveOwnedObject(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, Category = "UAI")
	FString NodeId;

	UPROPERTY(EditAnywhere, Category = "UAI")
	FString Label = TEXT("native-child");

	UPROPERTY(EditAnywhere, Category = "UAI")
	int32 NativeCounter = 17;

	UPROPERTY(VisibleAnywhere, Instanced, Category = "UAI")
	TObjectPtr<UUeAgentRecursiveLeafObject> NativeDefaultSubobject;

	UPROPERTY(EditAnywhere, Instanced, Category = "UAI")
	TArray<TObjectPtr<UUeAgentRecursiveOwnedObject>> Children;

	UPROPERTY(EditAnywhere, Category = "UAI")
	TObjectPtr<UUeAgentRecursiveOwnedObject> Peer;

	UPROPERTY(EditAnywhere, Category = "UAI")
	TObjectPtr<UUeAgentRecursiveOwnedObject> Shared;
};

UCLASS(BlueprintType)
class UEAGENTINTERFACE_API UUeAgentRecursiveDataAsset final : public UDataAsset
{
	GENERATED_BODY()

public:
	UUeAgentRecursiveDataAsset(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, Category = "UAI")
	FString Label = TEXT("native-root");

	UPROPERTY(EditAnywhere, Category = "UAI")
	int32 NativeCounter = 11;

	UPROPERTY(EditAnywhere, Category = "UAI")
	int32 ExplicitNumber = 0;

	UPROPERTY(EditAnywhere, Category = "UAI")
	TArray<int32> Numbers;

	UPROPERTY(EditAnywhere, Category = "UAI")
	TMap<FString, int32> NamedNumbers;

	UPROPERTY(VisibleAnywhere, Instanced, Category = "UAI")
	TObjectPtr<UUeAgentRecursiveLeafObject> NativeDefaultSubobject;

	UPROPERTY(EditAnywhere, Instanced, Category = "UAI")
	TArray<TObjectPtr<UUeAgentRecursiveOwnedObject>> OwnedObjects;
};

namespace UE::UAI::DomainAdapters::UObjectDataAsset
{
	using ::UUeAgentRecursiveDataAsset;
	using ::UUeAgentRecursiveLeafObject;
	using ::UUeAgentRecursiveOwnedObject;
}
