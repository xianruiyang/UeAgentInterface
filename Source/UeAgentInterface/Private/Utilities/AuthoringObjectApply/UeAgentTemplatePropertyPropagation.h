#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FProperty;
class UObject;

namespace UeAgentTemplatePropertyPropagation
{
// Keeps only reflected text and object paths, so Blueprint reinstancing and
// persistent package readback do not retain obsolete native objects.
class UEAGENTINTERFACE_API FSnapshot
{
public:
    void Capture(UObject& Object, FProperty& Property);
    bool Restore(UE::UAI::Contract::FDiagnostic& OutError);
private:
    struct FValue { FString ObjectPath; FName PropertyName; FString Type; TArray<FString> Elements; };
    TArray<FValue> Values;
    TMap<FString, bool> PackageDirtyStates;
};

class UEAGENTINTERFACE_API FScope
{
public:
    FScope();
    ~FScope();
    bool Restore(UE::UAI::Contract::FDiagnostic& OutError);
    void Capture(UObject& Object, FProperty& Property);
private:
    FScope* Previous = nullptr;
    FSnapshot Before;
};

// Selection must precede the template write: the native inheritance comparison
// uses the old template value and checks the entire archetype chain.
void Gather(UObject& Template, FProperty& Member, const FString& PropertyPath, FSnapshot& Before,
    TArray<TWeakObjectPtr<UObject>>& OutInstances);
struct FInheritedField
{
    FString PropertyPath;
    TArray<TWeakObjectPtr<UObject>> Instances;
};
bool Apply(UObject& Template, const TArray<FInheritedField>& Fields, UE::UAI::Contract::FDiagnostic& OutError);
}
