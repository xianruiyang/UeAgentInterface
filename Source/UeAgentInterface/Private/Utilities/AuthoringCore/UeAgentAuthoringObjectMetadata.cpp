#include "Utilities/AuthoringCore/UeAgentAuthoringObjectMetadata.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/MetaData.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UeAgentAuthoringObjectMetadata {
namespace {
bool IsAuthoringKey(FName Key) {
  // This existing namespace belongs to the external-input provenance owner.
  return !Key.ToString().StartsWith(TEXT("UAI.FactoryExternalImport."));
}
}

TSharedPtr<FJsonObject> Read(const UObject& Object) {
  auto Result = MakeShared<FJsonObject>();
  if (const auto* Values = FMetaData::GetMapForObject(&Object))
    for (const auto& Pair : *Values)
      if (IsAuthoringKey(Pair.Key)) Result->SetStringField(Pair.Key.ToString(), Pair.Value);
  return Result;
}

bool ApplyDesired(UObject& Object, const FJsonObject& Desired, UE::UAI::Contract::FDiagnostic& Error) {
  TMap<FName, FString> Values;
  for (const auto& Pair : Desired.Values) {
    const FName Key(*Pair.Key);
    FString Value;
    if (Key.IsNone() || !IsAuthoringKey(Key) || Values.Contains(Key) ||
        !Pair.Value || !Pair.Value->TryGetString(Value)) {
      const TCHAR* Reason = Key.IsNone() ? TEXT("A metadata name must resolve to a nonempty Unreal name.")
          : !IsAuthoringKey(Key) ? TEXT("This metadata name is reserved for external import provenance.")
          : Values.Contains(Key) ? TEXT("Metadata names must be unique under Unreal's case-insensitive name comparison.")
          : TEXT("A metadata value must be a string.");
      Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("object_metadata_name_or_string_invalid"), Reason, TEXT("object_metadata"));
      Error.CandidateName = Pair.Key;
      Error.SemanticPointer = TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
      Error.PropertyPath = Error.SemanticPointer;
      if (!Key.IsNone() && IsAuthoringKey(Key) && !Values.Contains(Key)) {
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
        const TCHAR* Kind = !Pair.Value ? TEXT("missing") : Pair.Value->Type == EJson::Null ? TEXT("null")
            : Pair.Value->Type == EJson::Boolean ? TEXT("boolean") : Pair.Value->Type == EJson::Number ? TEXT("number")
            : Pair.Value->Type == EJson::Array ? TEXT("array") : Pair.Value->Type == EJson::Object ? TEXT("object") : TEXT("none");
        Error.ActualValue = MakeShared<FJsonValueString>(Kind);
        Error.ActualValuePresent = Pair.Value.IsValid();
      }
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Desired, Pair.Key);
      return false;
    }
    Values.Add(Key, MoveTemp(Value));
  }
  if (const auto* Existing = FMetaData::GetMapForObject(&Object)) {
    for (const auto& Pair : *Existing)
      if (!IsAuthoringKey(Pair.Key)) Values.Add(Pair.Key, Pair.Value);
    if (Existing->OrderIndependentCompareEqual(Values)) return true;
  } else if (Values.IsEmpty()) return true;
  Object.Modify();
  Object.GetPackage()->GetMetaData().SetObjectValues(&Object, MoveTemp(Values));
  Object.MarkPackageDirty();
  return true;
}

void ApplyChanges(UObject& Object, const TMap<FName, TOptional<FString>>& Changes) {
  if (Changes.IsEmpty()) return;
  Object.Modify();
  auto& Metadata = Object.GetPackage()->GetMetaData();
  for (const auto& Pair : Changes) {
    if (Pair.Value.IsSet()) Metadata.SetValue(&Object, Pair.Key, *Pair.Value.GetValue());
    else Metadata.RemoveValue(&Object, Pair.Key);
  }
  Object.MarkPackageDirty();
}
}
