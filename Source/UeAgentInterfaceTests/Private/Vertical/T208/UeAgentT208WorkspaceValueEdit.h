#pragma once

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Tests::WorkspaceValueEdit
{
inline TSharedPtr<FJsonValue> PreserveValueWrapper(const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& Value)
{
    if (Before && Before->Type == EJson::Object && Before->AsObject()->Values.Num() == 1)
    {
        auto Wrapper = MakeShared<FJsonObject>();
        for (const auto& Field : Before->AsObject()->Values) Wrapper->SetField(Field.Key, Value);
        return MakeShared<FJsonValueObject>(Wrapper);
    }
    return Value;
}
inline int32 ReplaceNamedLeaf(const TSharedPtr<FJsonValue>& Value, const FString& Name, const TSharedPtr<FJsonValue>& Replacement)
{
    if (!Value) return 0;
    int32 Count = 0;
    if (Value->Type == EJson::Array)
        for (const auto& Item : Value->AsArray()) Count += ReplaceNamedLeaf(Item, Name, Replacement);
    if (Value->Type != EJson::Object) return Count;
    const auto Object = Value->AsObject(); FString Path;
    if (Object->TryGetStringField(TEXT("property_path"), Path) && Path == Name && Object->HasField(TEXT("value")))
    { Object->SetField(TEXT("value"), PreserveValueWrapper(Object->TryGetField(TEXT("value")), Replacement)); return 1; }
    for (auto& Field : Object->Values)
    {
        if (Field.Key == Name)
        {
            if (Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value")))
            {
                const auto Row = Field.Value->AsObject();
                Row->SetField(TEXT("value"), PreserveValueWrapper(Row->TryGetField(TEXT("value")), Replacement));
            }
            else Field.Value = PreserveValueWrapper(Field.Value, Replacement);
            ++Count;
        }
        else Count += ReplaceNamedLeaf(Field.Value, Name, Replacement);
    }
    return Count;
}
}
