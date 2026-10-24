#pragma once

#include "CoreMinimal.h"

class UClass;
class UFactory;

namespace UE::UAI::DomainAdapters::ExternalImport::Factory
{
    TSet<FString> NativeImportExtensions(const UClass* ExpectedClass);
    UFactory* SelectNativeImportFactory(const UClass* ExpectedClass, const FString& Source);
    TArray<UObject*> ImportNativeAsset(UFactory* Factory, const FString& Source, const FString& DestinationPackage);
}
