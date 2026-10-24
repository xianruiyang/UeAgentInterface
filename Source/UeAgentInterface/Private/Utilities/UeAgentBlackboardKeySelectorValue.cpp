// Copyright Epic Games, Inc. All Rights Reserved.
#include "Utilities/UeAgentBlackboardKeySelectorValue.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardAssetProvider.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "UObject/UnrealType.h"

namespace UeAgentBlackboardKeySelectorValue
{
namespace
{
UBlackboardData* FindBlackboard(const UObject& Owner)
{
    for (const UObject* Object = &Owner; Object; Object = Object->GetOuter())
        if (const auto* Provider = Cast<IBlackboardAssetProvider>(Object))
            return Provider->GetBlackboardAsset();
    return nullptr;
}

bool ContainsSelector(const FProperty* Property, TSet<const UStruct*>& Visiting)
{
    if (const auto* Struct = CastField<FStructProperty>(Property))
    {
        if (Struct->Struct == FBlackboardKeySelector::StaticStruct()) return true;
        if (Visiting.Contains(Struct->Struct)) return false;
        Visiting.Add(Struct->Struct);
        bool bFound = false;
        for (TFieldIterator<FProperty> Field(Struct->Struct); Field && !bFound; ++Field)
            bFound = ContainsSelector(*Field, Visiting);
        Visiting.Remove(Struct->Struct);
        return bFound;
    }
    if (const auto* Array = CastField<FArrayProperty>(Property)) return ContainsSelector(Array->Inner, Visiting);
    if (const auto* Set = CastField<FSetProperty>(Property)) return ContainsSelector(Set->ElementProp, Visiting);
    if (const auto* Map = CastField<FMapProperty>(Property)) return ContainsSelector(Map->ValueProp, Visiting);
    if (const auto* Optional = CastField<FOptionalProperty>(Property)) return ContainsSelector(Optional->GetValueProperty(), Visiting);
    return false;
}

void ResolveValue(FProperty* Property, void* Value, UBlackboardData& Blackboard,
    TArray<FProperty*>& Chain, TArray<TArray<FProperty*>>& Notifications, bool bInitializeContext)
{
    Chain.Add(Property);
    if (const auto* Struct = CastField<FStructProperty>(Property))
    {
        if (Struct->Struct == FBlackboardKeySelector::StaticStruct())
        {
            auto& Selector = *static_cast<FBlackboardKeySelector*>(Value);
            const auto BeforeType = Selector.SelectedKeyType;
            const auto BeforeID = Selector.GetSelectedKeyID();
            const auto BeforeName = Selector.SelectedKeyName;
            // ResolveSelectedKey intentionally skips an allowed None key. Clear
            // the previous cache first so changing to None cannot retain it.
            Selector.InvalidateResolvedKey();
            Selector.SelectedKeyType = nullptr;
            Selector.ResolveSelectedKey(Blackboard);
            const auto Notify = [&](FName Member)
            {
                auto Changed = Chain;
                FProperty* Field = FindFProperty<FProperty>(Struct->Struct, Member);
                check(Field);
                Changed.Add(Field);
                Notifications.AddUnique(MoveTemp(Changed));
            };
            // Match native Details ordering: type and ID precede key-name and
            // dependent operator/value notifications. UE computes derived state.
            if (bInitializeContext || BeforeType != Selector.SelectedKeyType)
                Notify(GET_MEMBER_NAME_CHECKED(FBlackboardKeySelector, SelectedKeyType));
            if (BeforeID != Selector.GetSelectedKeyID()) Notify(TEXT("SelectedKeyID"));
            if (BeforeName != Selector.SelectedKeyName)
                Notify(GET_MEMBER_NAME_CHECKED(FBlackboardKeySelector, SelectedKeyName));
        }
        else
            for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
                for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
                    ResolveValue(*Field, Field->ContainerPtrToValuePtr<void>(Value, Index), Blackboard,
                        Chain, Notifications, bInitializeContext);
    }
    else if (auto* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Values(Array, Value);
        for (int32 Index = 0; Index < Values.Num(); ++Index)
            ResolveValue(Array->Inner, Values.GetRawPtr(Index), Blackboard, Chain, Notifications, bInitializeContext);
    }
    else if (auto* Set = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Values(Set, Value);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
            if (Values.IsValidIndex(Index))
                ResolveValue(Set->ElementProp, Values.GetElementPtr(Index), Blackboard, Chain, Notifications, bInitializeContext);
    }
    else if (auto* Map = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Values(Map, Value);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
            if (Values.IsValidIndex(Index))
                ResolveValue(Map->ValueProp, Values.GetValuePtr(Index), Blackboard, Chain, Notifications, bInitializeContext);
    }
    else if (auto* Optional = CastField<FOptionalProperty>(Property); Optional && Optional->IsSet(Value))
        ResolveValue(Optional->GetValueProperty(), Optional->GetValuePointerForReadOrReplace(Value),
            Blackboard, Chain, Notifications, bInitializeContext);
    Chain.Pop();
}
}

TSet<FName> RootProperties(const UObject& Owner)
{
    TSet<FName> Result;
    if (!FindBlackboard(Owner)) return Result;
    TSet<const UStruct*> Visiting;
    for (TFieldIterator<FProperty> Field(Owner.GetClass()); Field; ++Field)
        if (ContainsSelector(*Field, Visiting)) Result.Add(Field->GetFName());
    return Result;
}

void Resolve(UObject& Owner, TArray<TArray<FProperty*>>& OutNotifications, bool bInitializeContext)
{
    UBlackboardData* Blackboard = FindBlackboard(Owner);
    if (!Blackboard) return;
    TArray<FProperty*> Chain;
    TSet<const UStruct*> Visiting;
    for (TFieldIterator<FProperty> Field(Owner.GetClass()); Field; ++Field)
        if (ContainsSelector(*Field, Visiting))
            for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
                ResolveValue(*Field, Field->ContainerPtrToValuePtr<void>(&Owner, Index),
                    *Blackboard, Chain, OutNotifications, bInitializeContext);
}
}
