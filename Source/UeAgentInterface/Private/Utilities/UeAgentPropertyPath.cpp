#include "Utilities/UeAgentPropertyPath.h"
#include "Dom/JsonObject.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace UeAgentPropertyPath
{
    bool Resolve(FProperty* Root, void* Address, const TArray<TSharedPtr<FJsonValue>>& Path,
        UObject* Owner, const UeAgentPropertyValueCodec::FOptions& Options,
        FResolved& Out, FDiagnostic& Error, bool bTypeOnly)
    {
        Error.Reset();
        Out = {};
        Out.Property = Root;
        Out.Address = Address;
        Out.bReadOnly = Root && Root->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
        for (int32 Segment = 0; Segment < Path.Num(); ++Segment)
        {
            const auto Fail = [&](const TCHAR* Code, const TCHAR* Reason,
                const TSharedPtr<FJsonValue>& Actual, const FString& Expected, const FString& Selector = FString(), bool bNative = false)
            {
                Error = FDiagnostic::Failure(TEXT("property_path_") + FString(Code), Reason, TEXT("property_path"));
                Error.Source.JsonPath = FString::Printf(TEXT("/path/%d"), Segment) + Selector;
                Error.ActualValue = Actual;
                Error.ActualValuePresent = Actual.IsValid();
                Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
                Error.ExpectedValuePresent = true;
                if (bNative) { Error.SourceKind = TEXT("native_result"); Error.Source = {}; }
                return false;
            };
            if (!Out.Property || (!bTypeOnly && !Out.Address) || !Path[Segment].IsValid() || Path[Segment]->Type != EJson::Object)
            {
                const bool bNative = !Out.Property || (!bTypeOnly && !Out.Address);
                return Fail(TEXT("invalid_segment"),
                    !Out.Property ? TEXT("The native property for this path segment is unavailable.")
                        : (!bTypeOnly && !Out.Address) ? TEXT("The native value address for this path segment is unavailable.")
                        : TEXT("Each property path segment must be an object."),
                    bNative ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : Path[Segment],
                    bNative ? TEXT("available native property and value address") : TEXT("object selector"), FString(), bNative);
            }
            const TSharedPtr<FJsonObject> Item = Path[Segment]->AsObject();
            if (Item->Values.Num() != 1) return Fail(TEXT("one_selector_required"),
                TEXT("A property path segment must contain exactly one selector."), Path[Segment], TEXT("exactly one selector field"));
            FString Field;
            if (Item->TryGetStringField(TEXT("field"), Field))
            {
                FStructProperty* Struct = CastField<FStructProperty>(Out.Property);
                if (!Struct || (Struct->ArrayDim > 1 && !Out.bSingleFixedElement)) return Fail(TEXT("not_struct_value"),
                    TEXT("A field selector requires a single struct value."), MakeShared<FJsonValueString>(Out.Property->GetCPPType()),
                    TEXT("single struct value"), TEXT("/field"));
                FProperty* Child = FindFProperty<FProperty>(Struct->Struct, *Field);
                if (!Child) return Fail(TEXT("field_not_found"), TEXT("The requested field does not exist in this struct."),
                    MakeShared<FJsonValueString>(Field), TEXT("field of ") + Struct->Struct->GetPathName(), TEXT("/field"));
                if (!bTypeOnly) Out.Address = Child->ContainerPtrToValuePtr<void>(Out.Address);
                Out.Property = Child;
                Out.bSingleFixedElement = false;
            }
            else if (const auto IndexValue = Item->TryGetField(TEXT("index")))
            {
                int64 IndexNumber = 0;
                FDiagnostic IndexError;
                if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(
                    IndexValue, 0, MAX_int32, IndexNumber, IndexError))
                {
                    Fail(TEXT("invalid_index"), TEXT("The array index must be a nonnegative int32."),
                        IndexValue, TEXT("integer in [0, 2147483647]"), TEXT("/index"));
                    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(IndexError, Error.Source.JsonPath);
                    Error.PublicCause = MakeShared<FDiagnostic>(MoveTemp(IndexError));
                    return false;
                }
                const int32 Index = static_cast<int32>(IndexNumber);
                if (Out.Property->ArrayDim > 1 && !Out.bSingleFixedElement)
                {
                    if (Index >= Out.Property->ArrayDim) return Fail(TEXT("index_out_of_range"),
                        TEXT("The index is outside the fixed array."), IndexValue,
                        FString::Printf(TEXT("index in [0, %d)"), Out.Property->ArrayDim), TEXT("/index"));
                    if (!bTypeOnly) Out.Address = static_cast<uint8*>(Out.Address) + Index * Out.Property->ElementSize;
                    Out.bSingleFixedElement = true;
                }
                else if (FArrayProperty* Array = CastField<FArrayProperty>(Out.Property))
                {
                    if (!bTypeOnly)
                    {
                        Out.ArrayDependencies.Emplace(Array, Out.Address);
                        FScriptArrayHelper Helper(Array, Out.Address);
                        if (Index >= Helper.Num()) return Fail(TEXT("index_out_of_range"),
                            TEXT("The index is outside the runtime array."), IndexValue,
                            FString::Printf(TEXT("index in [0, %d)"), Helper.Num()), TEXT("/index"));
                        Out.Address = Helper.GetRawPtr(Index);
                    }
                    Out.Property = Array->Inner;
                    Out.bSingleFixedElement = false;
                }
                else return Fail(TEXT("not_array"), TEXT("An index selector requires an array value."),
                    MakeShared<FJsonValueString>(Out.Property->GetCPPType()), TEXT("array"), TEXT("/index"));
            }
            else
            {
                if (Out.Property->ArrayDim > 1 && !Out.bSingleFixedElement) return Fail(TEXT("fixed_array_index_required"),
                    TEXT("A fixed array element must be selected before applying a container selector."),
                    Path[Segment], FString::Printf(TEXT("index selector in [0, %d)"), Out.Property->ArrayDim));
                FMapProperty* Map = CastField<FMapProperty>(Out.Property);
                FSetProperty* Set = CastField<FSetProperty>(Out.Property);
                const TSharedPtr<FJsonValue> Match = Item->TryGetField(Map ? TEXT("map_key") : TEXT("set_value"));
                if ((!Map && !Set) || !Match.IsValid()) return Fail(TEXT("invalid_container_selector"),
                    TEXT("The selector does not match this map or set value."), Path[Segment],
                    Map ? TEXT("map_key selector") : Set ? TEXT("set_value selector") : TEXT("map or set property"));
                FProperty* Key = Map ? Map->KeyProp : Set->ElementProp;
                void* KeyValue = FMemory::Malloc(Key->GetSize(), Key->GetMinAlignment());
                Key->InitializeValue(KeyValue);
                FDiagnostic DecodeError;
                const bool bDecoded = UeAgentPropertyValueCodec::Decode(Key, Match, KeyValue, Owner, Options, DecodeError);
                void* Found = nullptr;
                if (bDecoded && !bTypeOnly && Map)
                {
                    FScriptMapHelper Helper(Map, Out.Address);
                    for (int32 I = 0; I < Helper.GetMaxIndex(); ++I)
                        if (Helper.IsValidIndex(I) && Key->Identical(KeyValue, Helper.GetKeyPtr(I))) { Found = Helper.GetValuePtr(I); break; }
                }
                else if (bDecoded && !bTypeOnly)
                {
                    FScriptSetHelper Helper(Set, Out.Address);
                    for (int32 I = 0; I < Helper.GetMaxIndex(); ++I)
                        if (Helper.IsValidIndex(I) && Key->Identical(KeyValue, Helper.GetElementPtr(I))) { Found = Helper.GetElementPtr(I); break; }
                    if (Found)
                    {
                        Out.Sets.Emplace(Set, Out.Address);
                        Out.SetSelections.Add({Segment, Key, Found});
                    }
                }
                Key->DestroyValue(KeyValue);
                FMemory::Free(KeyValue);
                if (!bDecoded) { Error = MoveTemp(DecodeError);
                    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, FString::Printf(TEXT("/path/%d/%s"), Segment, Map ? TEXT("map_key") : TEXT("set_value"))); return false; }
                if (!bTypeOnly && !Found) return Fail(TEXT("element_not_found"),
                    Map ? TEXT("No map entry matches the requested key.") : TEXT("No set element matches the requested value."),
                    Match, Map ? TEXT("existing map key") : TEXT("existing set element"), Map ? TEXT("/map_key") : TEXT("/set_value"));
                Out.Address = Found;
                Out.Property = Map ? Map->ValueProp : Set->ElementProp;
                Out.bSingleFixedElement = false;
            }
            Out.bReadOnly |= Out.Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
        }
        return Out.Property && (bTypeOnly || Out.Address);
    }

    bool FinishWrite(const FResolved& Target, FDiagnostic& Error)
    {
        for (int32 S = Target.Sets.Num() - 1; S >= 0; --S)
        {
            const auto& Set = Target.Sets[S];
            FScriptSetHelper Helper(Set.Key, Set.Value);
            for (int32 I = 0; I < Helper.GetMaxIndex(); ++I)
                if (Helper.IsValidIndex(I))
                    for (int32 J = I + 1; J < Helper.GetMaxIndex(); ++J)
                        if (Helper.IsValidIndex(J) && Set.Key->ElementProp->Identical(Helper.GetElementPtr(I), Helper.GetElementPtr(J)))
                        {
                            Error = FDiagnostic::Failure(TEXT("property_path_set_duplicate_element"), TEXT("Writing the property produced two equal elements in a set."), TEXT("property_path"));
                            Error.SourceKind = TEXT("native_result");
                            Error.PropertyPath = Set.Key->GetName();
                            const auto Duplicates = MakeShared<FJsonObject>();
                            Duplicates->SetNumberField(TEXT("first_index"), I);
                            Duplicates->SetNumberField(TEXT("second_index"), J);
                            Error.ActualValue = MakeShared<FJsonValueObject>(Duplicates);
                            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique set elements"));
                            return false;
                        }
            Helper.Rehash();
        }
        return true;
    }

    bool CaptureWritePath(const FResolved& Target,
        const TArray<TSharedPtr<FJsonValue>>& Path, UObject* Owner,
        const UeAgentPropertyValueCodec::FOptions& Options,
        TArray<TSharedPtr<FJsonValue>>& OutPath, FDiagnostic& Error)
    {
        OutPath = Path;
        for (const auto& Selection : Target.SetSelections)
        {
            const auto Value = UeAgentPropertyValueCodec::Encode(
                Selection.ElementProperty, Selection.ElementAddress, Owner, Options);
            if (!OutPath.IsValidIndex(Selection.PathIndex))
            {
                Error = FDiagnostic::Failure(TEXT("property_path_set_readback_selector_invalid"), TEXT("The set selector references a segment outside the property path."), TEXT("property_path"));
                Error.SourceKind = TEXT("native_result");
                Error.TargetPath = Owner ? Owner->GetPathName() : FString();
                Error.PropertyPath = Selection.ElementProperty ? Selection.ElementProperty->GetName() : FString();
                Error.ActualValue = MakeShared<FJsonValueNumber>(Selection.PathIndex);
                Error.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("path index in [0, %d)"), OutPath.Num()));
                return false;
            }
            if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(Selection.ElementProperty, Value))
            {
                Error = FDiagnostic::Failure(TEXT("property_path_set_readback_selector_invalid"), TEXT("The written set element cannot be represented by a reliable canonical selector."), TEXT("property_path"));
                Error.SourceKind = TEXT("native_result");
                Error.TargetPath = Owner ? Owner->GetPathName() : FString();
                Error.PropertyPath = FString::Printf(TEXT("path[%d]"), Selection.PathIndex);
                Error.CandidateName = Selection.ElementProperty ? Selection.ElementProperty->GetName() : FString();
                Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("reliable canonical set element"));
                Error.ActualValue = Value;
                Error.ActualValuePresent = Value.IsValid();
                return false;
            }
            const auto Segment = MakeShared<FJsonObject>();
            Segment->SetField(TEXT("set_value"), Value);
            OutPath[Selection.PathIndex] = MakeShared<FJsonValueObject>(Segment);
        }
        return true;
    }
}
