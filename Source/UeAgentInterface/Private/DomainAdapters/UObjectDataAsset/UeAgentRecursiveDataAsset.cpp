#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"

UUeAgentRecursiveLeafObject::UUeAgentRecursiveLeafObject()
{
}

UUeAgentRecursiveOwnedObject::UUeAgentRecursiveOwnedObject(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NativeDefaultSubobject = ObjectInitializer.CreateDefaultSubobject<
		UUeAgentRecursiveLeafObject>(this, TEXT("NativeDefaultSubobject"));
}

UUeAgentRecursiveDataAsset::UUeAgentRecursiveDataAsset(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NativeDefaultSubobject = ObjectInitializer.CreateDefaultSubobject<
		UUeAgentRecursiveLeafObject>(this, TEXT("NativeDefaultSubobject"));
}
