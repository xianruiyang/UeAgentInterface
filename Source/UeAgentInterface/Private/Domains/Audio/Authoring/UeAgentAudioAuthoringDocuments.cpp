// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"
#include "Domains/Audio/Authoring/UeAgentAudioGraphIntent.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

#include "Domains/Audio/Authoring/UeAgentAudioSoundCueAuthoring.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Metasound.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendLiteral.h"
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#include "MetasoundSource.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundSubmix.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

namespace UeAgentAudioAuthoring {
namespace {

using UE::UAI::Contract::FDiagnostic;

FDiagnostic DocumentFailure(const FString& Code, const FString& Reason,
    const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
    const FJsonObject* Input = nullptr, const FString& InputField = FString(),
    const FString& Candidate = FString(), const FString& InputPointer = FString()) {
  auto Error = FDiagnostic::Failure(Code, Reason, TEXT("authoring"));
  Error.PropertyPath = Field;
  Error.CandidateName = Candidate;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValuePresent = Expected.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  Error.Source.JsonPath = InputPointer;
  if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, InputField);
  else Error.SourceKind = TEXT("native_result");
  return Error;
}

FDiagnostic MetaOperationFailure(const FString& Code, const FString& Reason,
    EMetaSoundBuilderResult Result, const FJsonObject* Input = nullptr,
    const FString& InputField = FString(), const FString& Candidate = FString(),
    TSharedPtr<FJsonValue> Proposed = nullptr) {
  const FString ResultText = Result == EMetaSoundBuilderResult::Succeeded ? TEXT("succeeded")
      : Result == EMetaSoundBuilderResult::Failed ? TEXT("failed")
      : FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Result));
  auto Error = DocumentFailure(Code, Reason, TEXT("native_operation_result"),
      MakeShared<FJsonValueString>(ResultText), MakeShared<FJsonValueString>(TEXT("succeeded")),
      Input, InputField, Candidate);
  Error.ProposedValue = MoveTemp(Proposed);
  return Error;
}
// This document owns the default graph/page. A cached editor builder may be on
// another page; bind it to the represented graph and restore native scope.
class FMetaSoundDefaultPageScope {
public:
  explicit FMetaSoundDefaultPageScope(UMetaSoundBuilderBase* InBuilder) : Builder(InBuilder) {
    if (Builder) {
      Before = Builder->GetBuilder().GetBuildPageID();
      bReady = Builder->GetBuilder().SetBuildPageID(Metasound::Frontend::DefaultPageID);
    }
  }
  ~FMetaSoundDefaultPageScope() {
    // This owner never removes pages, so the original page remains valid.
    if (Builder && bReady && Before != Metasound::Frontend::DefaultPageID)
      Builder->GetBuilder().SetBuildPageID(Before);
  }
  bool IsReady() const { return bReady; }
private:
  UMetaSoundBuilderBase* Builder = nullptr;
  FGuid Before;
  bool bReady = true;
};

bool RequireCurrentValue(const TSharedPtr<const FJsonObject> &Requested,
                         const TCHAR *Field,
                         const TSharedPtr<FJsonObject> &Current,
                         UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Requested.IsValid() || !Requested->HasField(Field))
    return true;
  const TSharedPtr<FJsonValue> RequestedValue = Requested->TryGetField(Field);
  const TSharedPtr<FJsonValue> CurrentValue =
      Current.IsValid() ? Current->TryGetField(Field) : nullptr;
  const auto Compared = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(CurrentValue, RequestedValue);
  if (Compared.bEquivalent) return true;
  OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_readonly_field_mismatch"), FString::Printf(TEXT("Read-only field '%s' differs from the current value."), Field));
  OutError.PropertyPath = Field;
  Compared.WithPrefix(FString(TEXT("/")) + Field).ApplyTo(OutError);
  UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Requested, TEXT("audio_readonly_verify"), FString(), Field);
  return false;
}

bool ValidateFields(const TSharedPtr<const FJsonObject> &Object,
                    const TSet<FString> &Allowed, const FString &Prefix,
                    UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Object.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field : Object->Values) {
    if (!Allowed.Contains(Field.Key)) {
      TArray<FString> Names = Allowed.Array();
      Names.Sort();
      TArray<TSharedPtr<FJsonValue>> Choices;
      for (const FString& Name : Names) Choices.Add(MakeShared<FJsonValueString>(Name));
      OutError = DocumentFailure(Prefix + TEXT("_unknown_field"),
          TEXT("The audio document contains an unsupported field."), TEXT("field_name"),
          MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueArray>(MoveTemp(Choices)),
          Object.Get(), Field.Key);
      return false;
    }
  }
  return true;
}

bool BuildEditableProperties(UObject &Object, const TSet<FName> &Excluded,
    TArray<TSharedPtr<FJsonValue>>& OutProperties, UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.ExcludedProperties = Excluded;
  UeAgentAuthoringState::FStateSnapshot Snapshot;
  TSharedPtr<FJsonObject> Document;
  if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
          &Object, Policy, Snapshot, Document, OutError)) {
    return false;
  }
  return UeAgentDetailsAuthoringService::DecodePropertiesDocument(
      Document, OutProperties, OutError);
}

bool ApplyEditableProperties(UObject &Object,
                             const TSharedPtr<const FJsonObject> &Owner,
                             FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                             TArray<FNodePropertyIntent>& OutIntents) {
  TSharedPtr<FJsonObject> Properties;
  if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Owner, TEXT("properties"), Properties, OutError)) return false;
  if (!Properties) return true;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = true;
  UeAgentAuthoringDirectApply::FResult Result;
  if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(&Object, Properties, Options, Result)) {
    OutError = Result.Diagnostic;
    return false;
  }
  OutStats.Settings += Result.PropertiesSeen;
  for (auto& Intent : Result.PropertyIntents)
    OutIntents.Add({Object.GetFName(), MoveTemp(Intent), Owner});
  return true;
}

FString LiteralTypeName(const EMetasoundFrontendLiteralType Type) {
  switch (Type) {
  case EMetasoundFrontendLiteralType::None:
    return TEXT("none");
  case EMetasoundFrontendLiteralType::Boolean:
    return TEXT("bool");
  case EMetasoundFrontendLiteralType::Integer:
    return TEXT("int");
  case EMetasoundFrontendLiteralType::Float:
    return TEXT("float");
  case EMetasoundFrontendLiteralType::String:
    return TEXT("string");
  case EMetasoundFrontendLiteralType::UObject:
    return TEXT("object");
  case EMetasoundFrontendLiteralType::NoneArray:
    return TEXT("none_array");
  case EMetasoundFrontendLiteralType::BooleanArray:
    return TEXT("bool_array");
  case EMetasoundFrontendLiteralType::IntegerArray:
    return TEXT("int_array");
  case EMetasoundFrontendLiteralType::FloatArray:
    return TEXT("float_array");
  case EMetasoundFrontendLiteralType::StringArray:
    return TEXT("string_array");
  case EMetasoundFrontendLiteralType::UObjectArray:
    return TEXT("object_array");
  default:
    return TEXT("invalid");
  }
}

struct FStorage {
  const TCHAR* Name;
  const TCHAR* Field;
  EMetasoundFrontendLiteralType Scalar;
  EMetasoundFrontendLiteralType Array;
};
static const FStorage StorageTypes[] = {
    {TEXT("bool"), TEXT("AsBoolean"), EMetasoundFrontendLiteralType::Boolean, EMetasoundFrontendLiteralType::BooleanArray},
    {TEXT("int"), TEXT("AsInteger"), EMetasoundFrontendLiteralType::Integer, EMetasoundFrontendLiteralType::IntegerArray},
    {TEXT("float"), TEXT("AsFloat"), EMetasoundFrontendLiteralType::Float, EMetasoundFrontendLiteralType::FloatArray},
    {TEXT("string"), TEXT("AsString"), EMetasoundFrontendLiteralType::String, EMetasoundFrontendLiteralType::StringArray},
    {TEXT("object"), TEXT("AsUObject"), EMetasoundFrontendLiteralType::UObject, EMetasoundFrontendLiteralType::UObjectArray}};

TSharedPtr<FJsonObject>
EncodeLiteral(const FMetasoundFrontendLiteral &Literal) {
  if (!Literal.IsValid())
    return nullptr;
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("type"), LiteralTypeName(Literal.GetType()));
  bool BoolValue = false;
  int32 IntValue = 0;
  float FloatValue = 0.0f;
  FString StringValue;
  UObject *ObjectValue = nullptr;
  TArray<bool> BoolArray;
  TArray<int32> IntArray;
  TArray<float> FloatArray;
  TArray<FString> StringArray;
  TArray<UObject *> ObjectArray;
  if (Literal.TryGet(BoolValue)) {
    Result->SetBoolField(TEXT("value"), BoolValue);
  } else if (Literal.TryGet(IntValue)) {
    Result->SetNumberField(TEXT("value"), IntValue);
  } else if (Literal.TryGet(FloatValue)) {
    Result->SetNumberField(TEXT("value"), FloatValue);
  } else if (Literal.TryGet(StringValue)) {
    Result->SetStringField(TEXT("value"), StringValue);
  } else if (Literal.TryGet(ObjectValue)) {
    Result->SetStringField(
        TEXT("value"), ObjectValue ? ObjectValue->GetPathName() : FString());
  } else if (Literal.TryGet(BoolArray)) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const bool Value : BoolArray)
      Values.Add(MakeShared<FJsonValueBoolean>(Value));
    Result->SetArrayField(TEXT("value"), Values);
  } else if (Literal.TryGet(IntArray)) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const int32 Value : IntArray)
      Values.Add(MakeShared<FJsonValueNumber>(Value));
    Result->SetArrayField(TEXT("value"), Values);
  } else if (Literal.TryGet(FloatArray)) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const float Value : FloatArray)
      Values.Add(MakeShared<FJsonValueNumber>(Value));
    Result->SetArrayField(TEXT("value"), Values);
  } else if (Literal.TryGet(StringArray)) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString &Value : StringArray)
      Values.Add(MakeShared<FJsonValueString>(Value));
    Result->SetArrayField(TEXT("value"), Values);
  } else if (Literal.TryGet(ObjectArray)) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const UObject *Value : ObjectArray)
      Values.Add(MakeShared<FJsonValueString>(Value ? Value->GetPathName()
                                                    : FString()));
    Result->SetArrayField(TEXT("value"), Values);
  } else if (Literal.GetType() == EMetasoundFrontendLiteralType::NoneArray) {
    Result->SetNumberField(TEXT("value"), Literal.GetArrayNum());
  }
  for (const FStorage& Storage : StorageTypes) {
    const bool bArray = Literal.GetType() == Storage.Array;
    if (!bArray && Literal.GetType() != Storage.Scalar) continue;
    const auto* Property = FindFProperty<FArrayProperty>(FMetasoundFrontendLiteral::StaticStruct(), Storage.Field);
    if (Property) UeAgentAuthoringEncodedTypes::FScope::Record(Result->TryGetField(TEXT("value")),
        UeAgentAuthoringTypeDescriptors::Describe(bArray ? static_cast<const FProperty*>(Property) : Property->Inner), nullptr);
    break;
  }
  return Result;
}

FDiagnostic LiteralMismatch(const FString& Code, const FString& Reason,
    const FMetasoundFrontendLiteral& Expected, const FMetasoundFrontendLiteral* Actual,
    const FJsonObject& Input, const FString& Field, const FString& Candidate) {
  const auto ExpectedObject = EncodeLiteral(Expected);
  const auto ActualObject = Actual ? EncodeLiteral(*Actual) : nullptr;
  TSharedPtr<FJsonValue> ExpectedValue, ActualValue;
  if (ExpectedObject) ExpectedValue = MakeShared<FJsonValueObject>(ExpectedObject);
  if (ActualObject) ActualValue = MakeShared<FJsonValueObject>(ActualObject);
  auto Error = FDiagnostic::Failure(Code, Reason, TEXT("authoring"));
  Error.CandidateName = Candidate;
  const auto Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(ExpectedValue, ActualValue);
  if (!Difference.bEquivalent) {
    Difference.WithPrefix(Field.IsEmpty() ? FString() : TEXT("/") + Field).ApplyTo(Error);
  } else {
    // Native object identity can differ while its public asset path is equal.
    Error.PropertyPath = Actual ? TEXT("native_value_matches_requested") : TEXT("default_present");
    Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
    Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    Error.ActualValuePresent = Error.ExpectedValuePresent = true;
    Error.ProposedValue = ExpectedValue;
    if (Actual) Error.PublicMessage += TEXT(" The public values encode identically, but the native literals do not match.");
  }
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
  return Error;
}

bool DecodeLiteral(const TSharedPtr<const FJsonObject> &Object,
                   FMetasoundFrontendLiteral &OutLiteral, UE::UAI::Contract::FDiagnostic& OutError) {
  const auto Invalid = [&](const FString& Code, const FString& Field, const FString& Expected) {
    auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code,
        Field.IsEmpty() ? FString() : TEXT("/") + Field,
        MakeShared<FJsonValueString>(Expected), Object.IsValid() ? Object->TryGetField(Field) : nullptr);
    if (Object.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Object, Field);
    OutError = Diagnostic;
    return false;
  };
  if (!Object.IsValid()) {
    return Invalid(TEXT("metasound_literal_invalid"), FString(), TEXT("literal object"));
  }
  FString Type;
  if (!Object->TryGetStringField(TEXT("type"), Type)) {
    return Invalid(TEXT("metasound_literal_type_required"), TEXT("type"), TEXT("literal type string"));
  }
  if (Type == TEXT("none")) {
    OutLiteral.Set(FMetasoundFrontendLiteral::FDefault());
    return true;
  }
  if (Type == TEXT("none_array")) {
    int32 Num = 0;
    if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Object, TEXT("value"), Num, OutError)) return false;
    if (Num < 0) return Invalid(TEXT("metasound_literal_count_negative"), TEXT("value"), TEXT("non-negative integer"));
    OutLiteral.Set(FMetasoundFrontendLiteral::FDefaultArray{Num});
    return true;
  }
  const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("value"));
  if (!Value.IsValid()) {
    return Invalid(TEXT("metasound_literal_value_invalid"), TEXT("value"), Type);
  }
  // The domain selects the native variant. Its reflected storage supplies the
  // scalar/array/reference types to the same recursive codec as other consumers.
  for (const FStorage& Storage : StorageTypes) {
    const bool bArray = Type == FString(Storage.Name) + TEXT("_array");
    if (!bArray && Type != Storage.Name) continue;
    auto* Property = FindFProperty<FArrayProperty>(FMetasoundFrontendLiteral::StaticStruct(), Storage.Field);
    auto* Tag = FindFProperty<FEnumProperty>(FMetasoundFrontendLiteral::StaticStruct(), TEXT("Type"));
    if (!Property || !Tag) {
      OutError = DocumentFailure(TEXT("metasound_literal_native_storage_missing"),
          !Property ? TEXT("The requested literal type has no reflected native storage array.")
                    : TEXT("The native literal type discriminator is unavailable."),
          TEXT("native_storage_available"), MakeShared<FJsonValueBoolean>(false),
          MakeShared<FJsonValueBoolean>(true), Object.Get(), TEXT("type"), Type);
      OutError.InternalContext = !Property ? FString(Storage.Field) : TEXT("FMetasoundFrontendLiteral.Type");
      return false;
    }
    FMetasoundFrontendLiteral Decoded;
    // UE 5.6 SetType switches on the old Type instead of InType. Initialize the
    // reflected variant tag together with its storage on this isolated value.
    Tag->GetUnderlyingProperty()->SetIntPropertyValue(Tag->ContainerPtrToValuePtr<void>(&Decoded),
        static_cast<uint64>(bArray ? Storage.Array : Storage.Scalar));
    void* Address = Property->ContainerPtrToValuePtr<void>(&Decoded);
    FScriptArrayHelper Items(Property, Address);
    if (!bArray) Items.Resize(1);
    if (!UeAgentPropertyValueCodec::Decode(bArray ? static_cast<FProperty*>(Property) : Property->Inner,
        Value, bArray ? Address : Items.GetRawPtr(0), nullptr, {}, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value"));
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object, TEXT("metasound_literal_decode"));
      return false;
    }
    OutLiteral = MoveTemp(Decoded);
    return true;
  }
  TArray<TSharedPtr<FJsonValue>> Choices = {MakeShared<FJsonValueString>(TEXT("none")), MakeShared<FJsonValueString>(TEXT("none_array"))};
  for (const FStorage& Storage : StorageTypes) {
    Choices.Add(MakeShared<FJsonValueString>(Storage.Name));
    Choices.Add(MakeShared<FJsonValueString>(FString(Storage.Name) + TEXT("_array")));
  }
  auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("metasound_literal_type_unsupported"),
      TEXT("/type"), MakeShared<FJsonValueArray>(Choices), Object->TryGetField(TEXT("type")));
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Object, TEXT("type"));
  OutError = Diagnostic;
  return false;
}

FString MetaSoundNodeKind(const EMetasoundFrontendClassType Type) {
  switch (Type) {
  case EMetasoundFrontendClassType::Input:
    return TEXT("graph_input");
  case EMetasoundFrontendClassType::Output:
    return TEXT("graph_output");
  case EMetasoundFrontendClassType::External:
    return TEXT("external");
  case EMetasoundFrontendClassType::Template:
    return TEXT("template");
  default:
    return TEXT("unsupported");
  }
}

struct FAudioPinRecord final {
  FString SourceId;
  FString Name;
  FString Type;
  UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection Direction =
      UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input;
  int32 Occurrence = 0;
};

bool AddCommonPinInterface(
    const TArray<FAudioPinRecord> &Pins,
    TSharedRef<FJsonObject> Node,
    UE::UAI::Contract::FDiagnostic& OutError) {
  TArray<UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasInput> Inputs;
  for (const FAudioPinRecord &Pin : Pins)
    Inputs.Add({Pin.SourceId, Pin.Name, Pin.Occurrence, Pin.Direction});
  UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasProjection Projection;
  UE::UAI::HjsonWorkspace::FWorkspaceError Error;
  if (!UE::UAI::HjsonWorkspace::TryBuildGraphAuthoringPinAliasProjection(
          Inputs, Projection, Error)) {
    OutError = Error.GetDiagnostic();
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> VisiblePins;
  for (const FAudioPinRecord &Pin : Pins) {
    const TSharedRef<FJsonObject> VisiblePin = MakeShared<FJsonObject>();
    VisiblePin->SetStringField(TEXT("alias"),
        Projection.AliasBySourceId.FindRef(Pin.SourceId));
    VisiblePin->SetStringField(TEXT("name"), Pin.Name);
    if (Pin.Occurrence > 0)
      VisiblePin->SetNumberField(TEXT("occurrence"), Pin.Occurrence);
    VisiblePin->SetStringField(TEXT("type"), Pin.Type);
    VisiblePin->SetStringField(TEXT("direction"),
        Pin.Direction == UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input
            ? TEXT("input") : TEXT("output"));
    VisiblePins.Add(MakeShared<FJsonValueObject>(VisiblePin));
  }
  const TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
  Interface->SetArrayField(TEXT("pins"), MoveTemp(VisiblePins));
  Node->SetObjectField(TEXT("pin_interface"), Interface);
  return true;
}

// The ordinary document represents the default page. A pin with no instance
// override still has its class default; reading it must not create an override.
const FMetasoundFrontendLiteral* FindMetaSoundEffectiveInputDefault(
    UMetaSoundBuilderBase& Builder, const FMetaSoundBuilderNodeInputHandle& Pin) {
  return UeAgentAudioAuthoring::FindMetaSoundEffectiveInputDefault(Builder, Pin.NodeID, Pin.VertexID);
}

TSharedPtr<FJsonObject> BuildMetaSoundDocument(UObject &Asset,
                                               UE::UAI::Contract::FDiagnostic& OutError) {
  IMetaSoundDocumentInterface *DocumentInterface =
      Cast<IMetaSoundDocumentInterface>(&Asset);
  if (!DocumentInterface) {
    OutError = DocumentFailure(TEXT("metasound_document_interface_missing"),
        TEXT("The audio asset does not implement the MetaSound document interface."),
        TEXT("asset_class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
        MakeShared<FJsonValueString>(TEXT("a class implementing MetaSoundDocumentInterface")));
    return nullptr;
  }
  const FMetasoundFrontendDocument &Document =
      DocumentInterface->GetConstDocument();
  const FMetasoundFrontendGraph &Graph =
      Document.RootGraph.GetConstDefaultGraph();
  const FMetasoundFrontendClassInterface &Interface =
      Document.RootGraph.GetDefaultInterface();

  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("asset_type"), Asset.IsA<UMetaSoundSource>()
                                                   ? TEXT("source")
                                                   : TEXT("patch"));
  Result->SetObjectField(TEXT("settings"), Settings);

  TArray<TSharedPtr<FJsonValue>> Interfaces;
  for (const FMetasoundFrontendVersion &Version : Document.Interfaces) {
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Version.Name.ToString());
    Item->SetNumberField(TEXT("major"), Version.Number.Major);
    Item->SetNumberField(TEXT("minor"), Version.Number.Minor);
    Interfaces.Add(MakeShared<FJsonValueObject>(Item));
  }
  Interfaces.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return UeAgentAuthoringState::MakeStableCanonicalValueKey(A) <
               UeAgentAuthoringState::MakeStableCanonicalValueKey(B);
      });
  Result->SetArrayField(TEXT("interfaces"), Interfaces);

  TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Inputs;
  for (const FMetasoundFrontendClassInput &Input : Interface.Inputs) {
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Input.Name.ToString());
    Item->SetStringField(TEXT("type"), Input.TypeName.ToString());
    if (const FMetasoundFrontendLiteral *Default =
            Input.FindConstDefault(Metasound::Frontend::DefaultPageID)) {
      if (TSharedPtr<FJsonObject> Encoded = EncodeLiteral(*Default))
        Item->SetObjectField(TEXT("default"), Encoded);
    }
    Inputs.Add(MakeShared<FJsonValueObject>(Item));
  }
  TArray<TSharedPtr<FJsonValue>> Outputs;
  for (const FMetasoundFrontendClassOutput &Output : Interface.Outputs) {
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Output.Name.ToString());
    Item->SetStringField(TEXT("type"), Output.TypeName.ToString());
    Outputs.Add(MakeShared<FJsonValueObject>(Item));
  }
  Members->SetArrayField(TEXT("inputs"), Inputs);
  Members->SetArrayField(TEXT("outputs"), Outputs);
  Result->SetObjectField(TEXT("members"), Members);

  struct FNodeRecord {
    const FMetasoundFrontendNode *Node = nullptr;
    const FMetasoundFrontendClass *Dependency = nullptr;
    FString Kind;
    FString ClassName;
    FString CandidateName;
    FString StableBase;
  };
  TArray<FNodeRecord> Records;
  UE::UAI::ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
  for (const FMetasoundFrontendNode &Node : Graph.Nodes) {
    FNodeRecord Record;
    Record.Node = &Node;
    if (Interface.Inputs.ContainsByPredicate(
            [&Node](const FMetasoundFrontendClassInput &Input) {
              return Input.NodeID == Node.GetID();
            })) {
      Record.Kind = TEXT("graph_input");
      Record.ClassName = Node.Name.ToString();
    } else if (Interface.Outputs.ContainsByPredicate(
                   [&Node](const FMetasoundFrontendClassOutput &Output) {
                     return Output.NodeID == Node.GetID();
                   })) {
      Record.Kind = TEXT("graph_output");
      Record.ClassName = Node.Name.ToString();
    } else {
      const FMetasoundFrontendClass *Dependency =
          Document.Dependencies.FindByPredicate(
              [&Node](const FMetasoundFrontendClass &Item) {
                return Item.ID == Node.ClassID;
              });
      if (!Dependency)
        continue;
      Record.Dependency = Dependency;
      Record.Kind = MetaSoundNodeKind(Dependency->Metadata.GetType());
      if (Record.Kind != TEXT("external"))
        continue;
      Record.ClassName = Dependency->Metadata.GetClassName().ToString();
    }
    if (Record.Kind == TEXT("external")) {
      const FMetasoundFrontendClass *Dependency = Record.Dependency;
      check(Dependency);
      using namespace UE::UAI::QueryOwners::AudioGraph;
      FAudioGraphCandidate Candidate;
      FAudioGraphCandidateQueryError QueryError;
      if (!TryResolveAudioGraphNativeCandidate(
              Asset, Record.ClassName, Dependency->Metadata.GetVersion().Major,
              Dependency->Metadata.GetVersion().Minor, Candidate, QueryError, &CandidateBatch)) {
        OutError = MoveTemp(QueryError);
        if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
        return nullptr;
      }
      Record.CandidateName = Candidate.Name;
    }
    Record.StableBase = Record.Kind + TEXT("|") + Node.Name.ToString() +
                        TEXT("|") + (Record.CandidateName.IsEmpty()
                                           ? Record.ClassName
                                           : Record.CandidateName);
    Records.Add(MoveTemp(Record));
  }
  Records.Sort([](const FNodeRecord &A, const FNodeRecord &B) {
    return A.StableBase < B.StableBase;
  });

  TArray<UE::UAI::HjsonWorkspace::FGraphAuthoringNodeAliasInput> AliasInputs;
  for (const FNodeRecord &Record : Records)
  {
    AliasInputs.Add({
      Record.Node->GetID().ToString(EGuidFormats::DigitsWithHyphensLower),
      (Record.Kind == TEXT("graph_input") || Record.Kind == TEXT("graph_output"))
        ? Record.Node->Name.ToString() : Record.CandidateName});
  }
  UE::UAI::HjsonWorkspace::FGraphAuthoringAliasProjection AliasProjection;
  UE::UAI::HjsonWorkspace::FWorkspaceError AliasError;
  if (!UE::UAI::HjsonWorkspace::TryBuildGraphAuthoringAliasProjection(
      AliasInputs, AliasProjection, AliasError))
  {
    OutError = AliasError.GetDiagnostic();
    return nullptr;
  }

  TMap<FGuid, FString> NodeIds;
  TMap<FGuid, TMap<FGuid, FString>> InputNames;
  TMap<FGuid, TMap<FGuid, FString>> OutputNames;
  TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
  UMetaSoundBuilderBase *Builder = nullptr;
  if (GEditor) {
    if (UMetaSoundEditorSubsystem *Subsystem =
            GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()) {
      TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface;
      ScriptInterface.SetObject(&Asset);
      ScriptInterface.SetInterface(DocumentInterface);
      EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
      Builder = Subsystem->FindOrBeginBuilding(ScriptInterface, BuilderResult);
    }
  }
  FMetaSoundDefaultPageScope PageScope(Builder);
  if (!PageScope.IsReady()) {
    OutError = DocumentFailure(TEXT("metasound_default_graph_missing"),
        TEXT("The MetaSound builder could not select the asset's default graph page for readback."),
        TEXT("default_page_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return nullptr;
  }
  for (const FNodeRecord &Record : Records) {
    const FString SourceId = Record.Node->GetID().ToString(
        EGuidFormats::DigitsWithHyphensLower);
    const FString& StableId = AliasProjection.AliasBySourceId.FindChecked(SourceId);
    NodeIds.Add(Record.Node->GetID(), StableId);
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    // The captured state keeps the native id for resolver bookkeeping.  The
    // common graph projection removes it and exposes StableId as the key.
    Item->SetStringField(TEXT("id"), SourceId);
    Item->SetStringField(TEXT("kind"), Record.Kind);
    if (Record.Kind == TEXT("external")) {
      Item->SetStringField(TEXT("name"), Record.CandidateName);
      TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
      Candidate->SetStringField(TEXT("name"), Record.CandidateName);
      Item->SetObjectField(TEXT("candidate"), Candidate);
    }
#if WITH_EDITORONLY_DATA
    if (const FVector2D *Location = Record.Node->Style.Display.Locations.Find(
            Record.Node->GetID())) {
      TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
      Position->SetNumberField(TEXT("x"), Location->X);
      Position->SetNumberField(TEXT("y"), Location->Y);
      Item->SetObjectField(TEXT("position"), Position);
    }
#endif
    if (Record.Kind == TEXT("graph_input") ||
        Record.Kind == TEXT("graph_output"))
      Item->SetStringField(TEXT("member_name"), Record.Node->Name.ToString());
    TSharedRef<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
    for (const FMetasoundFrontendVertex &Input :
         Record.Node->Interface.Inputs) {
      InputNames.FindOrAdd(Record.Node->GetID())
          .Add(Input.VertexID, Input.Name.ToString());
      if (Builder && Record.Kind != TEXT("graph_input")) {
        if (const auto* Literal = FindMetaSoundEffectiveInputDefault(*Builder,
            FMetaSoundBuilderNodeInputHandle(Record.Node->GetID(), Input.VertexID))) {
          if (TSharedPtr<FJsonObject> Encoded = EncodeLiteral(*Literal)) {
            auto Default = MakeShared<FJsonObject>();
            Default->SetStringField(TEXT("pin_name"), Input.Name.ToString());
            Default->SetNumberField(TEXT("pin_occurrence"), 0);
            Default->SetObjectField(TEXT("default_value"), Encoded);
            PinDefaults->SetObjectField(Input.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower), Default);
          }
        }
      }
    }
    for (const FMetasoundFrontendVertex &Output :
         Record.Node->Interface.Outputs) {
      OutputNames.FindOrAdd(Record.Node->GetID())
          .Add(Output.VertexID, Output.Name.ToString());
    }
    if (!PinDefaults->Values.IsEmpty()) Item->SetObjectField(TEXT("pin_defaults"), PinDefaults);
    TArray<FAudioPinRecord> PinRecords;
    for (const FMetasoundFrontendVertex &Input : Record.Node->Interface.Inputs)
      PinRecords.Add({Input.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower),
                      Input.Name.ToString(), Input.TypeName.ToString(),
                      UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input});
    for (const FMetasoundFrontendVertex &Output : Record.Node->Interface.Outputs)
      PinRecords.Add({Output.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower),
                      Output.Name.ToString(), Output.TypeName.ToString(),
                      UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output});
    if (!AddCommonPinInterface(PinRecords, Item, OutError))
      return nullptr;
    NodesObject->SetObjectField(StableId, Item);
  }

  TSharedRef<FJsonObject> EdgesObject = MakeShared<FJsonObject>();
  int32 EdgeIndex = 0;
  for (const FMetasoundFrontendEdge &Edge : Graph.Edges) {
    const FString FromNode = NodeIds.FindRef(Edge.FromNodeID);
    const FString ToNode = NodeIds.FindRef(Edge.ToNodeID);
    const FString FromPin =
        OutputNames.FindRef(Edge.FromNodeID).FindRef(Edge.FromVertexID);
    const FString ToPin =
        InputNames.FindRef(Edge.ToNodeID).FindRef(Edge.ToVertexID);
    if (FromNode.IsEmpty() || ToNode.IsEmpty() || FromPin.IsEmpty() ||
        ToPin.IsEmpty())
      continue;
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> FromEndpoint = MakeShared<FJsonObject>();
    FromEndpoint->SetStringField(TEXT("node"),
        Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphensLower));
    FromEndpoint->SetStringField(TEXT("pin"), FromPin);
    TSharedRef<FJsonObject> ToEndpoint = MakeShared<FJsonObject>();
    ToEndpoint->SetStringField(TEXT("node"),
        Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphensLower));
    ToEndpoint->SetStringField(TEXT("pin"), ToPin);
    Item->SetObjectField(TEXT("from"), FromEndpoint);
    Item->SetObjectField(TEXT("to"), ToEndpoint);
    EdgesObject->SetObjectField(FString::Printf(TEXT("edge_%d"), EdgeIndex++), Item);
  }
  TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
  GraphJson->SetStringField(TEXT("semantic"), TEXT("metasound_graph"));
  Result->SetObjectField(TEXT("nodes"), NodesObject);
  Result->SetObjectField(TEXT("edges"), EdgesObject);
  Result->SetObjectField(TEXT("graph"), GraphJson);
  return Result;
}

TSharedPtr<FJsonObject> BuildSoundCueDocument(USoundCue &Cue, UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<FJsonObject> Info =
      UeAgentAudioAuthoring::BuildSoundCueInfo(&Cue, true, false, false);
  const TArray<TSharedPtr<FJsonValue>> *SourceNodes = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *SourceEdges = nullptr;
  Info->TryGetArrayField(TEXT("nodes"), SourceNodes);
  Info->TryGetArrayField(TEXT("edges"), SourceEdges);
  struct FNodeRecord {
    USoundNode *Node = nullptr;
    FString OldId;
    FString Type;
  };
  TArray<FNodeRecord> Records;
  TMap<FString, FString> NativeIdByLegacyId;
  int32 SourceIndex = 0;
  for (int32 Index = 0; Index < Cue.AllNodes.Num(); ++Index) {
    USoundNode *Node = Cue.AllNodes[Index];
    if (!Node)
      continue;
    const TSharedPtr<FJsonObject> Source =
        SourceNodes && SourceNodes->IsValidIndex(SourceIndex)
            ? (*SourceNodes)[SourceIndex]->AsObject()
            : nullptr;
    ++SourceIndex;
    FString OldId;
    FString Type;
    if (Source.IsValid()) {
      Source->TryGetStringField(TEXT("id"), OldId);
      Source->TryGetStringField(TEXT("type"), Type);
    }
    NativeIdByLegacyId.Add(OldId, Node->GetName());
    OldId = Node->GetName();
    if (Type.IsEmpty())
      Type = Node->GetClass()->GetName();
    Records.Add({Node, OldId, Type});
  }
  Records.Sort([](const FNodeRecord &A, const FNodeRecord &B) {
    const FString AKey = A.Type + TEXT("|") + A.OldId;
    const FString BKey = B.Type + TEXT("|") + B.OldId;
    return AKey < BKey;
  });
  TArray<UE::UAI::HjsonWorkspace::FGraphAuthoringNodeAliasInput> AliasInputs;
  for (const FNodeRecord &Record : Records)
    AliasInputs.Add({Record.OldId, Record.Type});
  const auto* OutputNode = FindSoundCueOutputNode(Cue);
  if (OutputNode) AliasInputs.Add({TEXT("sound_cue_root"), TEXT("SoundCueOutput")});
  UE::UAI::HjsonWorkspace::FGraphAuthoringAliasProjection AliasProjection;
  UE::UAI::HjsonWorkspace::FWorkspaceError AliasError;
  if (!UE::UAI::HjsonWorkspace::TryBuildGraphAuthoringAliasProjection(
      AliasInputs, AliasProjection, AliasError)) {
    OutError = AliasError.GetDiagnostic();
    return nullptr;
  }
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
  if (OutputNode) {
    auto Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("id"), TEXT("sound_cue_root"));
    Item->SetStringField(TEXT("type"), TEXT("SoundCueOutput"));
    Item->SetStringField(TEXT("class_path"), OutputNode->GetClass()->GetPathName());
    auto Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), OutputNode->NodePosX);
    Position->SetNumberField(TEXT("y"), OutputNode->NodePosY);
    Item->SetObjectField(TEXT("pos"), Position);
    Nodes->SetObjectField(AliasProjection.AliasBySourceId.FindRef(TEXT("sound_cue_root")), Item);
  }
  for (const FNodeRecord &Record : Records) {
    const FString Id = AliasProjection.AliasBySourceId.FindRef(Record.OldId);
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("id"), Record.OldId);
    Item->SetStringField(TEXT("type"), Record.Type);
    // Captured native class metadata validates retained node identity. The
    // common graph projection hides class_path from ordinary authoring.
    Item->SetStringField(TEXT("class_path"),
                         Record.Node->GetClass()->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Properties;
    if (!BuildEditableProperties(*Record.Node, SoundNodeStructuralProperties(), Properties, OutError)) return nullptr;
    Item->SetArrayField(TEXT("properties"), MoveTemp(Properties));
    if (!Record.Node->GetGraphNode()) {
      OutError = DocumentFailure(TEXT("sound_cue_graph_node_missing"),
          TEXT("The native SoundCue node has no editor graph node to export."), TEXT("graph_node_present"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), nullptr, FString(), Id);
      return nullptr;
    }
    if (const UEdGraphNode *GraphNode = Record.Node->GetGraphNode()) {
      TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
      Position->SetNumberField(TEXT("x"), GraphNode->NodePosX);
      Position->SetNumberField(TEXT("y"), GraphNode->NodePosY);
      Item->SetObjectField(TEXT("pos"), Position);
      TArray<FAudioPinRecord> PinRecords;
      TMap<FString, int32> Occurrences;
      for (int32 PinIndex = 0; PinIndex < GraphNode->Pins.Num(); ++PinIndex) {
        const UEdGraphPin *Pin = GraphNode->Pins[PinIndex];
        if (!Pin)
          continue;
        const FString OccurrenceKey = FString::FromInt(Pin->Direction) + TEXT(":") + Pin->PinName.ToString();
        const int32 Occurrence = Occurrences.FindOrAdd(OccurrenceKey)++;
        PinRecords.Add({
            FString::Printf(TEXT("%s:%d"), *Id, PinIndex),
            Pin->PinName.ToString(), Pin->PinType.PinCategory.ToString(),
            Pin->Direction == EGPD_Output
                ? UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output
                : UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input,
            Occurrence});
      }
      UE::UAI::Contract::FDiagnostic PinError;
      if (!AddCommonPinInterface(PinRecords, Item, PinError)) {
        OutError = PinError;
        return nullptr;
      }
    }
    Nodes->SetObjectField(Id, Item);
  }
  TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
  int32 EdgeIndex = 0;
  if (SourceEdges) {
    for (const TSharedPtr<FJsonValue> &Value : *SourceEdges) {
      const TSharedPtr<FJsonObject> Edge =
          Value.IsValid() ? Value->AsObject() : nullptr;
      if (!Edge.IsValid())
        continue;
      FString From;
      FString To;
      Edge->TryGetStringField(TEXT("from"), From);
      Edge->TryGetStringField(TEXT("to"), To);
      From = NativeIdByLegacyId.FindRef(From);
      To = NativeIdByLegacyId.FindRef(To);
      int32 ChildIndex = 0;
      Edge->TryGetNumberField(TEXT("child_index"), ChildIndex);
      const FNodeRecord *Parent = Records.FindByPredicate(
          [&From](const FNodeRecord &Record) { return Record.OldId == From; });
      const FNodeRecord *Child = Records.FindByPredicate(
          [&To](const FNodeRecord &Record) { return Record.OldId == To; });
      const UEdGraphNode *ParentGraph = Parent && Parent->Node
                                           ? Parent->Node->GetGraphNode()
                                           : nullptr;
      const UEdGraphNode *ChildGraph = Child && Child->Node
                                          ? Child->Node->GetGraphNode()
                                          : nullptr;
      TArray<const UEdGraphPin *> ParentInputs;
      const UEdGraphPin *ChildOutput = nullptr;
      if (ParentGraph) {
        for (const UEdGraphPin *Pin : ParentGraph->Pins)
          if (Pin && Pin->Direction == EGPD_Input)
            ParentInputs.Add(Pin);
      }
      if (ChildGraph) {
        for (const UEdGraphPin *Pin : ChildGraph->Pins) {
          if (Pin && Pin->Direction == EGPD_Output) {
            ChildOutput = Pin;
            break;
          }
        }
      }
      if (!ParentInputs.IsValidIndex(ChildIndex) || !ChildOutput) {
        OutError = DocumentFailure(TEXT("sound_cue_edge_pin_missing"),
            !ParentInputs.IsValidIndex(ChildIndex) ? TEXT("The SoundCue child relationship has no corresponding parent input pin.")
                                                 : TEXT("The SoundCue child relationship has no child output pin."),
            !ParentInputs.IsValidIndex(ChildIndex) ? TEXT("parent_input_index") : TEXT("child_output_present"),
            !ParentInputs.IsValidIndex(ChildIndex) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(ChildIndex)) : MakeShared<FJsonValueBoolean>(false),
            !ParentInputs.IsValidIndex(ChildIndex) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), ParentInputs.Num()))) : MakeShared<FJsonValueBoolean>(true),
            nullptr, FString(), From);
        return nullptr;
      }
      int32 ParentOccurrence = 0;
      for (int32 Index = 0; Index < ChildIndex; ++Index)
        if (ParentInputs[Index]->PinName == ParentInputs[ChildIndex]->PinName)
          ++ParentOccurrence;
      TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
      TSharedRef<FJsonObject> FromEndpoint = MakeShared<FJsonObject>();
      FromEndpoint->SetStringField(TEXT("node"), To);
      FromEndpoint->SetStringField(TEXT("pin"), ChildOutput->PinName.ToString());
      FromEndpoint->SetNumberField(TEXT("pin_occurrence"), 0);
      TSharedRef<FJsonObject> ToEndpoint = MakeShared<FJsonObject>();
      ToEndpoint->SetStringField(TEXT("node"), From);
      ToEndpoint->SetStringField(
          TEXT("pin"), ParentInputs[ChildIndex]->PinName.ToString());
      ToEndpoint->SetNumberField(TEXT("pin_occurrence"), ParentOccurrence);
      Item->SetObjectField(TEXT("from"), FromEndpoint);
      Item->SetObjectField(TEXT("to"), ToEndpoint);
      Edges->SetObjectField(
          FString::Printf(TEXT("edge_%d"), EdgeIndex++), Item);
    }
  }
  TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> NodeReferences = MakeShared<FJsonObject>();
  NodeReferences->SetStringField(TEXT("root"),
      Cue.FirstNode ? Cue.FirstNode->GetName() : FString());
  Graph->SetObjectField(TEXT("node_references"), NodeReferences);
  Graph->SetStringField(TEXT("semantic"), TEXT("sound_cue_graph"));
  Result->SetObjectField(TEXT("nodes"), Nodes);
  Result->SetObjectField(TEXT("edges"), Edges);
  Result->SetObjectField(TEXT("graph"), Graph);
  return Result;
}

TSharedPtr<FJsonObject> BuildSoundSubmixDocument(USoundSubmixBase &Submix) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("submix_class"),
                           Submix.GetClass()->GetPathName());
  if (const USoundSubmixWithParentBase *WithParent =
          Cast<USoundSubmixWithParentBase>(&Submix)) {
    Settings->SetStringField(TEXT("parent"),
                             WithParent->ParentSubmix
                                 ? WithParent->ParentSubmix->GetPathName()
                                 : FString());
  }
  TArray<TSharedPtr<FJsonValue>> Effects;
  if (const USoundSubmix *Concrete = Cast<USoundSubmix>(&Submix)) {
    for (const USoundEffectSubmixPreset *Preset : Concrete->SubmixEffectChain)
      Effects.Add(MakeShared<FJsonValueString>(Preset ? Preset->GetPathName()
                                                      : FString()));
  }
  Settings->SetArrayField(TEXT("effect_chain"), Effects);
  Result->SetObjectField(TEXT("settings"), Settings);
  TArray<TSharedPtr<FJsonValue>> Children;
  for (const USoundSubmixBase *Child : Submix.ChildSubmixes)
    Children.Add(
        MakeShared<FJsonValueString>(Child ? Child->GetPathName() : FString()));
  Result->SetArrayField(TEXT("children"), Children);
  return Result;
}

bool GetMetaSoundBuilder(UObject &Asset, UMetaSoundBuilderBase *&OutBuilder,
                         UE::UAI::Contract::FDiagnostic& OutError) {
  OutBuilder = nullptr;
  IMetaSoundDocumentInterface *Interface =
      Cast<IMetaSoundDocumentInterface>(&Asset);
  UMetaSoundEditorSubsystem *Subsystem =
      GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
              : nullptr;
  if (!Interface || !Subsystem) {
    OutError = DocumentFailure(TEXT("metasound_builder_unavailable"),
        !Interface ? TEXT("The asset does not implement the MetaSound document interface.")
                   : !GEditor ? TEXT("No editor is available to create the MetaSound builder.")
                              : TEXT("The editor has no MetaSound subsystem."),
        !Interface ? TEXT("document_interface_available") : !GEditor ? TEXT("editor_available") : TEXT("subsystem_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = Asset.GetClass()->GetPathName();
    return false;
  }
  TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface;
  ScriptInterface.SetObject(&Asset);
  ScriptInterface.SetInterface(Interface);
  EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
  OutBuilder = Subsystem->FindOrBeginBuilding(ScriptInterface, Result);
  if (!OutBuilder || Result != EMetaSoundBuilderResult::Succeeded) {
    OutError = !OutBuilder
        ? DocumentFailure(TEXT("metasound_builder_unavailable"),
            TEXT("The editor did not return a MetaSound builder for the asset."),
            TEXT("builder_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
        : MetaOperationFailure(TEXT("metasound_builder_unavailable"),
            TEXT("The editor failed to begin building the MetaSound asset."), Result);
    OutError.CandidateName = Asset.GetClass()->GetPathName();
    return false;
  }
  return true;
}

struct FMetaSoundMemberIntent {
  FName Name;
  FName Type;
  bool bInput = false;
  TOptional<FMetasoundFrontendLiteral> Default;
  TSharedPtr<FJsonObject> Source;
};

bool ReadMetaSoundMemberIntents(const TSharedPtr<const FJsonObject>& Members,
    TArray<FMetaSoundMemberIntent>& OutIntents, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Members.IsValid())
    return true;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Members,
      {{TEXT("inputs"), EJson::Array}, {TEXT("outputs"), EJson::Array}}, OutError, TEXT("members"))) return false;
  for (const TPair<FString, bool> &Field :
       {TPair<FString, bool>(TEXT("inputs"), true),
        TPair<FString, bool>(TEXT("outputs"), false)}) {
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!Members->TryGetArrayField(Field.Key, Items) || !Items)
      continue;
    TSet<FName> Names;
    for (int32 MemberIndex = 0; MemberIndex < Items->Num(); ++MemberIndex) {
      const TSharedPtr<FJsonValue>& Value = (*Items)[MemberIndex];
      const TSharedPtr<FJsonObject> Item =
          Value.IsValid() ? Value->AsObject() : nullptr;
      FString Name;
      FString Type;
      if (!Item.IsValid() || !Item->TryGetStringField(TEXT("name"), Name) ||
          !Item->TryGetStringField(TEXT("type"), Type) || Name.IsEmpty() ||
          Type.IsEmpty()) {
        const FString InvalidField = !Item ? FString() : Name.IsEmpty() ? TEXT("name") : TEXT("type");
        OutError = DocumentFailure(TEXT("metasound_member_invalid"),
            !Item ? TEXT("Each MetaSound member must be an object.")
                  : TEXT("A MetaSound member requires nonempty string name and type fields."),
            InvalidField, Item ? Item->TryGetField(InvalidField) : Value,
            MakeShared<FJsonValueString>(Item ? TEXT("a nonempty string") : TEXT("a member object")),
            Item ? Item.Get() : Members.Get(), InvalidField, FString(),
            Item ? FString() : FString::Printf(TEXT("/%s/%d"), *Field.Key, MemberIndex));
        return false;
      }
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Item,
          {{TEXT("default"), EJson::Object}}, OutError, Name)) return false;
      FMetaSoundMemberIntent Intent;
      Intent.Name = *Name;
      Intent.Type = *Type;
      Intent.bInput = Field.Value;
      Intent.Source = Item;
      if (Names.Contains(Intent.Name)) {
        OutError = DocumentFailure(TEXT("metasound_duplicate_member"),
            TEXT("A MetaSound member name occurs more than once in the same collection."),
            TEXT("name"), MakeShared<FJsonValueString>(Name),
            MakeShared<FJsonValueString>(TEXT("a unique name within ") + Field.Key),
            Item.Get(), TEXT("name"), Name);
        return false;
      }
      Names.Add(Intent.Name);
      FMetasoundFrontendLiteral Default;
      const TSharedPtr<FJsonObject>* DefaultObject = nullptr;
      if (Item->TryGetObjectField(TEXT("default"), DefaultObject)) {
        if (!DecodeLiteral(*DefaultObject, Default, OutError)) return false;
        Intent.Default = Default;
      }
      OutIntents.Add(MoveTemp(Intent));
    }
  }
  return true;
}

bool ApplyMetaSoundMembers(UMetaSoundBuilderBase& Builder,
    const TArray<FMetaSoundMemberIntent>& Intents,
    TMap<FString, FMetaSoundNodeHandle>& OutNodeHandles,
    FMutationStats& OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
    for (const auto& Intent : Intents) {
      const FString Name = Intent.Name.ToString();
      const FString Type = Intent.Type.ToString();
      const bool bInput = Intent.bInput;
      const auto& Item = Intent.Source;
      const FMetasoundFrontendLiteral Default = Intent.Default.IsSet() ? Intent.Default.GetValue() : FMetasoundFrontendLiteral();
      const auto* Existing = bInput ? Builder.GetBuilder().FindGraphInputNode(*Name)
          : Builder.GetBuilder().FindGraphOutputNode(*Name);
      FMetaSoundNodeHandle Handle;
      if (!Existing) {
        EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
        if (bInput) {
          const FMetaSoundBuilderNodeOutputHandle Output =
              Builder.AddGraphInputNode(*Name, *Type, Default, AddResult);
          Handle = FMetaSoundNodeHandle(Output.NodeID);
        } else {
          const FMetaSoundBuilderNodeInputHandle Input =
              Builder.AddGraphOutputNode(*Name, *Type, Default, AddResult);
          Handle = FMetaSoundNodeHandle(Input.NodeID);
        }
        if (AddResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = MetaOperationFailure(bInput ? TEXT("metasound_add_input_failed") : TEXT("metasound_add_output_failed"),
              bInput ? TEXT("The MetaSound builder could not add the requested graph input.")
                     : TEXT("The MetaSound builder could not add the requested graph output."),
              AddResult, Item.Get(), TEXT("type"), Name, MakeShared<FJsonValueString>(Type));
          return false;
        }
        ++OutStats.Members;
      } else {
        Handle = FMetaSoundNodeHandle(Existing->GetID());
        EMetaSoundBuilderResult TypeResult = EMetaSoundBuilderResult::Failed;
        if (bInput) Builder.SetGraphInputDataType(*Name, *Type, TypeResult);
        else Builder.SetGraphOutputDataType(*Name, *Type, TypeResult);
        if (TypeResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = MetaOperationFailure(TEXT("metasound_set_member_type_failed"),
              TEXT("The MetaSound builder could not set the member's requested data type."),
              TypeResult, Item.Get(), TEXT("type"), Name, MakeShared<FJsonValueString>(Type));
          return false;
        }
        // Native type changes can reconstruct a member's template node.
        Existing = bInput ? Builder.GetBuilder().FindGraphInputNode(*Name)
            : Builder.GetBuilder().FindGraphOutputNode(*Name);
        if (!Existing) {
          OutError = DocumentFailure(TEXT("metasound_member_node_missing"),
              TEXT("The member's native node is missing after changing its data type."),
              TEXT("member_node_available"), MakeShared<FJsonValueBoolean>(false),
              MakeShared<FJsonValueBoolean>(true), Item.Get(), TEXT("type"), Name);
          OutError.ProposedValue = MakeShared<FJsonValueString>(Type);
          return false;
        }
        Handle = FMetaSoundNodeHandle(Existing->GetID());
      }
      OutNodeHandles.Add(
          (bInput ? TEXT("graph_input:") : TEXT("graph_output:")) + Name,
          Handle);
      if (Intent.Default.IsSet()) {
          if (bInput && !Builder.GetBuilder().SetGraphInputDefault(*Name, Intent.Default.GetValue(), &Metasound::Frontend::DefaultPageID)) {
            OutError = DocumentFailure(TEXT("metasound_set_input_default_failed"),
                TEXT("The MetaSound builder rejected the graph input's requested default."),
                TEXT("default_write_succeeded"), MakeShared<FJsonValueBoolean>(false),
                MakeShared<FJsonValueBoolean>(true), Item.Get(), TEXT("default"), Name);
            OutError.ProposedValue = MakeShared<FJsonValueObject>(EncodeLiteral(Intent.Default.GetValue()));
            return false;
          }
          if (!bInput) {
            EMetaSoundBuilderResult FindResult, SetResult;
            const auto InputHandle = Builder.FindNodeInputByName(Handle, Intent.Name, FindResult);
            if (FindResult != EMetaSoundBuilderResult::Succeeded) {
              OutError = MetaOperationFailure(TEXT("metasound_output_input_missing"),
                  TEXT("The graph output's input pin could not be found to set its default."),
                  FindResult, Item.Get(), TEXT("default"), Name);
              return false;
            }
            Builder.SetNodeInputDefault(InputHandle, Intent.Default.GetValue(), SetResult);
            if (SetResult != EMetaSoundBuilderResult::Succeeded) {
              OutError = MetaOperationFailure(TEXT("metasound_set_output_default_failed"),
                  TEXT("The MetaSound builder rejected the graph output's requested default."),
                  SetResult, Item.Get(), TEXT("default"), Name,
                  MakeShared<FJsonValueObject>(EncodeLiteral(Intent.Default.GetValue())));
              return false;
            }
          }
      }
  }
  return true;
}

// Arrays are complete only after ordinary composition restores them. Partial
// internal documents and omitted collections keep their existing overlay meaning.
bool PlanMetaSoundMemberRemovals(UMetaSoundBuilderBase& Builder,
    const FJsonObject& Members, const TArray<FMetaSoundMemberIntent>& Intents,
    TArray<TPair<FName, bool>>& OutRemovals, UE::UAI::Contract::FDiagnostic& OutError) {
  TArray<const Metasound::Frontend::IInterfaceRegistryEntry*> Declared;
  Builder.GetBuilder().FindDeclaredInterfaces(Declared);
  const auto& Interface = Builder.GetBuilder().GetConstDocumentChecked().RootGraph.GetDefaultInterface();
  auto Plan = [&](const auto& ExistingMembers, bool bInput, const TCHAR* Field) {
    if (!Members.HasField(Field)) return true;
    for (const auto& Existing : ExistingMembers) {
      if (Intents.ContainsByPredicate([&](const auto& Intent) { return Intent.bInput == bInput && Intent.Name == Existing.Name; })) continue;
      for (const auto* Entry : Declared) {
        const auto& NativeInterface = Entry->GetInterface();
        const bool bDeclared = bInput
          ? NativeInterface.Inputs.ContainsByPredicate([&](const auto& Item) { return Item.Name == Existing.Name; })
          : NativeInterface.Outputs.ContainsByPredicate([&](const auto& Item) { return Item.Name == Existing.Name; });
        if (bDeclared) {
          OutError = DocumentFailure(TEXT("metasound_interface_member_delete_forbidden"),
              TEXT("The complete member collection omits a member required by a declared MetaSound interface."),
              TEXT("required_member_present"), MakeShared<FJsonValueBoolean>(false),
              MakeShared<FJsonValueBoolean>(true), &Members, Field, Existing.Name.ToString());
          return false;
        }
      }
      OutRemovals.Emplace(Existing.Name, bInput);
    }
    return true;
  };
  return Plan(Interface.Inputs, true, TEXT("inputs")) && Plan(Interface.Outputs, false, TEXT("outputs"));
}

bool VerifyMetaSoundMemberIntents(UMetaSoundBuilderBase& Builder,
    const TSharedPtr<const FJsonObject>& Members, const TArray<FMetaSoundMemberIntent>& Intents,
    bool bCompleteDocument, UE::UAI::Contract::FDiagnostic& OutError) {
  for (const auto& Intent : Intents) {
    const auto* Input = Builder.GetBuilder().FindGraphInput(Intent.Name);
    const auto* Output = Builder.GetBuilder().FindGraphOutput(Intent.Name);
    const auto* Default = Input ? Input->FindConstDefault(Metasound::Frontend::DefaultPageID) : nullptr;
    bool bDefaultMatches = true;
    TOptional<FMetasoundFrontendLiteral> ActualOutputDefault;
    EMetaSoundBuilderResult DefaultFindResult = EMetaSoundBuilderResult::Succeeded;
    EMetaSoundBuilderResult DefaultReadResult = EMetaSoundBuilderResult::Succeeded;
    if (Intent.Default.IsSet()) {
      if (Intent.bInput) bDefaultMatches = Default && Default->IsEqual(Intent.Default.GetValue());
      else {
        EMetaSoundBuilderResult FindResult, ReadResult;
        const auto Node = Builder.FindGraphOutputNode(Intent.Name, FindResult);
        const auto Pin = Builder.FindNodeInputByName(Node, Intent.Name, FindResult);
        const auto Actual = Builder.GetNodeInputDefault(Pin, ReadResult);
        DefaultFindResult = FindResult;
        DefaultReadResult = ReadResult;
        if (FindResult == EMetaSoundBuilderResult::Succeeded && ReadResult == EMetaSoundBuilderResult::Succeeded)
          ActualOutputDefault = Actual;
        bDefaultMatches = FindResult == EMetaSoundBuilderResult::Succeeded && ReadResult == EMetaSoundBuilderResult::Succeeded && Actual.IsEqual(Intent.Default.GetValue());
      }
    }
    if ((Intent.bInput ? !Input || Input->TypeName != Intent.Type : !Output || Output->TypeName != Intent.Type) || !bDefaultMatches) {
      const FString Name = Intent.Name.ToString();
      const bool bPresent = Intent.bInput ? Input != nullptr : Output != nullptr;
      const FName ActualType = Intent.bInput ? (Input ? Input->TypeName : NAME_None) : (Output ? Output->TypeName : NAME_None);
      if (!bPresent) {
        OutError = DocumentFailure(TEXT("metasound_member_intent_not_retained"),
            TEXT("The requested MetaSound member is missing after apply."), TEXT("member_present"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Intent.Source.Get(), TEXT("name"), Name);
      } else if (ActualType != Intent.Type) {
        OutError = DocumentFailure(TEXT("metasound_member_intent_not_retained"),
            TEXT("The MetaSound member retained a different data type."), TEXT("type"),
            MakeShared<FJsonValueString>(ActualType.ToString()), MakeShared<FJsonValueString>(Intent.Type.ToString()),
            Intent.Source.Get(), TEXT("type"), Name);
      } else if (!Intent.bInput && (DefaultFindResult != EMetaSoundBuilderResult::Succeeded || DefaultReadResult != EMetaSoundBuilderResult::Succeeded)) {
        OutError = MetaOperationFailure(TEXT("metasound_member_intent_not_retained"),
            DefaultFindResult != EMetaSoundBuilderResult::Succeeded
                ? TEXT("The output member's default pin could not be found during verification.")
                : TEXT("The output member's default could not be read during verification."),
            DefaultFindResult != EMetaSoundBuilderResult::Succeeded ? DefaultFindResult : DefaultReadResult,
            Intent.Source.Get(), TEXT("default"), Name);
      } else {
        OutError = LiteralMismatch(TEXT("metasound_member_intent_not_retained"),
            TEXT("The MetaSound member did not retain its requested default."), Intent.Default.GetValue(),
            Intent.bInput ? Default : ActualOutputDefault.IsSet() ? &ActualOutputDefault.GetValue() : nullptr,
            *Intent.Source, TEXT("default"), Name);
      }
      return false;
    }
  }
  if (bCompleteDocument && Members) {
    TArray<TPair<FName, bool>> ExtraMembers;
    if (!PlanMetaSoundMemberRemovals(Builder, *Members, Intents, ExtraMembers, OutError)) return false;
    if (!ExtraMembers.IsEmpty()) {
      OutError = DocumentFailure(TEXT("metasound_member_removal_not_retained"),
          TEXT("A MetaSound member omitted from the complete collection still exists after apply."),
          TEXT("member_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
          Members.Get(), ExtraMembers[0].Value ? TEXT("inputs") : TEXT("outputs"), ExtraMembers[0].Key.ToString());
      return false;
    }
  }
  return true;
}

bool ApplyMetaSoundNodeDefaults(UMetaSoundBuilderBase& Builder,
    const FMetaSoundNodeHandle& Node, const FJsonObject& Item,
    TArray<FMetaSoundDefaultIntent>& OutIntents, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(Item,
      {{TEXT("inputs"), EJson::Array}, {TEXT("pin_defaults"), EJson::Object}}, OutError)) return false;
  struct FDefault { FMetasoundFrontendLiteral Value; TSharedPtr<FJsonObject> Source; };
  TMap<FName, FDefault> Defaults;
  auto Add = [&](const FString& Name, const TSharedPtr<FJsonObject>& Literal,
      const FJsonObject& NameSource, const FString& NameField) {
    FDefault Default;
    if (Name.IsEmpty() || !Literal) {
      OutError = DocumentFailure(TEXT("metasound_node_default_invalid"),
          Name.IsEmpty() ? TEXT("A node default requires a nonempty input pin name.")
                         : TEXT("A node default requires a literal object."),
          Name.IsEmpty() ? TEXT("pin_name") : TEXT("literal_available"),
          Name.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Name)) : MakeShared<FJsonValueBoolean>(false),
          Name.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a nonempty pin name"))) : MakeShared<FJsonValueBoolean>(true),
          &NameSource, NameField, Name);
      return false;
    }
    if (!DecodeLiteral(Literal, Default.Value, OutError)) return false;
    Default.Source = Literal;
    if (const auto* Existing = Defaults.Find(*Name); Existing && !Existing->Value.IsEqual(Default.Value)) {
      OutError = LiteralMismatch(TEXT("metasound_node_default_conflict"),
          TEXT("Two defaults for the same MetaSound input pin disagree."),
          Existing->Value, &Default.Value, *Literal, FString(), Name);
      return false;
    }
    Defaults.Add(*Name, MoveTemp(Default));
    return true;
  };
  // Older internal graph documents normalize here into the same native writer.
  // Ordinary export has only pin_defaults; pin_interface owns interface metadata.
  const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
  if (Item.TryGetArrayField(TEXT("inputs"), Inputs)) {
    for (int32 InputIndex = 0; InputIndex < Inputs->Num(); ++InputIndex) {
      const auto& Value = (*Inputs)[InputIndex];
      const auto Input = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
      FString Name;
      const TSharedPtr<FJsonObject>* Literal = nullptr;
      if (!Input || !Input->TryGetStringField(TEXT("name"), Name)) {
        OutError = DocumentFailure(TEXT("metasound_node_input_invalid"),
            !Input ? TEXT("Each legacy node input must be an object.") : TEXT("A node input requires a string name."),
            TEXT("name"), Input ? Input->TryGetField(TEXT("name")) : Value,
            MakeShared<FJsonValueString>(Input ? TEXT("a string") : TEXT("an input object")),
            Input ? Input.Get() : &Item, Input ? TEXT("name") : FString(), FString(),
            Input ? FString() : FString::Printf(TEXT("/inputs/%d"), InputIndex));
        return false;
      }
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Input, {{TEXT("default"), EJson::Object}}, OutError)) return false;
      if (Input->TryGetObjectField(TEXT("default"), Literal) && !Add(Name, *Literal, *Input, TEXT("name"))) return false;
    }
  }
  const TSharedPtr<FJsonObject>* PinDefaults = nullptr;
  if (Item.TryGetObjectField(TEXT("pin_defaults"), PinDefaults)) {
    for (const auto& Pair : (*PinDefaults)->Values) {
      const auto Default = Pair.Value && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
      const TSharedPtr<FJsonObject>* Literal = nullptr;
      FString Name = Pair.Key;
      int32 Occurrence = 0;
      if (!Default || !Default->TryGetObjectField(TEXT("default_value"), Literal)) {
        OutError = DocumentFailure(TEXT("metasound_pin_default_invalid"),
            !Default ? TEXT("Each pin default must be an object.") : TEXT("The pin default requires a default_value literal object."),
            TEXT("default_value"), Default ? Default->TryGetField(TEXT("default_value")) : Pair.Value,
            MakeShared<FJsonValueString>(TEXT("a literal object")),
            Default ? Default.Get() : PinDefaults->Get(), Default ? TEXT("default_value") : Pair.Key, Pair.Key);
        return false;
      }
      Default->TryGetStringField(TEXT("pin_name"), Name);
      if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Default, TEXT("pin_occurrence"), Occurrence, OutError)) return false;
      // Native MetaSound vertex names are unique within one node interface.
      if (Occurrence != 0) {
        OutError = DocumentFailure(TEXT("metasound_pin_occurrence_not_found"),
            TEXT("MetaSound vertex names are unique within a node; only occurrence zero exists."),
            TEXT("pin_occurrence"), MakeShared<FJsonValueNumber>(Occurrence), MakeShared<FJsonValueNumber>(0),
            Default.Get(), TEXT("pin_occurrence"), Name);
        return false;
      }
      if (!Add(Name, *Literal, *Default, TEXT("pin_name"))) return false;
    }
  }
  for (const auto& Pair : Defaults) {
    EMetaSoundBuilderResult FindResult, SetResult;
    const auto Pin = Builder.FindNodeInputByName(Node, Pair.Key, FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded) {
      OutError = MetaOperationFailure(TEXT("metasound_node_input_not_found"),
          TEXT("The MetaSound node has no resolvable input pin for the requested default."),
          FindResult, Pair.Value.Source.Get(), FString(), Pair.Key.ToString());
      return false;
    }
    const auto* Current = FindMetaSoundEffectiveInputDefault(Builder, Pin);
    if (!Current || !Current->IsEqual(Pair.Value.Value)) {
      Builder.SetNodeInputDefault(Pin, Pair.Value.Value, SetResult);
      if (SetResult != EMetaSoundBuilderResult::Succeeded) {
        OutError = MetaOperationFailure(TEXT("metasound_node_default_failed"),
            TEXT("The MetaSound builder rejected the requested node input default."),
            SetResult, Pair.Value.Source.Get(), FString(), Pair.Key.ToString(),
            MakeShared<FJsonValueObject>(EncodeLiteral(Pair.Value.Value)));
        return false;
      }
    }
    // Freeze decoded request semantics, including stable object paths; neither
    // mutable JSON children nor UObject literal pointers survive as expectations.
    OutIntents.Add({Pin.NodeID, Pin.VertexID, EncodeLiteral(Pair.Value.Value), Pair.Value.Source});
  }
  return true;
}

bool ApplyMetaSoundGraph(UMetaSoundBuilderBase &Builder,
                         const TSharedPtr<const FJsonObject> &Graph,
                         TMap<FString, FMetaSoundNodeHandle> &NodeHandles,
                         const TArray<FString> &NodePointers,
                         const TArray<FString> &EdgePointers,
                         FAudioGraphIntent& Intent,
                         FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                         const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* Connections) {
  if (!Graph.IsValid())
    return true;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Graph,
      {{TEXT("nodes"), EJson::Array}, {TEXT("edges"), EJson::Array}}, OutError, TEXT("graph"))) return false;
  const TArray<TSharedPtr<FJsonValue>> *Nodes = nullptr;
  if (!Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) {
    OutError = DocumentFailure(TEXT("metasound_graph_nodes_required"),
        TEXT("The MetaSound graph document requires a nodes array."), TEXT("nodes"),
        Graph->TryGetField(TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("an array")), Graph.Get(), TEXT("nodes"));
    return false;
  }
  TArray<FGuid> ExistingExternal;
  for (const auto& Node : Builder.GetBuilder().FindConstBuildGraphChecked().Nodes) {
    const auto* Dependency = Builder.GetBuilder().FindDependency(Node.ClassID);
    if (Dependency && Dependency->Metadata.GetType() == EMetasoundFrontendClassType::External)
      ExistingExternal.Add(Node.GetID());
  }
  TSet<FGuid> DesiredExternal;
  auto ApplyPosition = [&](const TSharedPtr<FJsonObject>& Item, const FMetaSoundNodeHandle& Handle, FMetaSoundNodeIntent& NodeIntent) {
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Item,
        {{TEXT("pos"), EJson::Object}, {TEXT("position"), EJson::Object}}, OutError)) return false;
    const TSharedPtr<FJsonObject>* Position = nullptr;
    if (!Item->TryGetObjectField(TEXT("pos"), Position) && !Item->TryGetObjectField(TEXT("position"), Position)) return true;
    FVector2D Location = FVector2D::ZeroVector;
    if (const auto* Node = Builder.GetBuilder().FindNode(Handle.NodeID))
      if (const auto* Current = Node->Style.Display.Locations.Find(Handle.NodeID)) Location = *Current;
    if (!UeAgentAuthoringJsonFields::ReadFloat64Field(**Position, TEXT("x"), Location.X, OutError)
        || !UeAgentAuthoringJsonFields::ReadFloat64Field(**Position, TEXT("y"), Location.Y, OutError)) return false;
    NodeIntent.Position = Location;
    NodeIntent.PositionSourcePointer = NodeIntent.SourcePointer
        + (Item->HasField(TEXT("pos")) ? TEXT("/pos") : TEXT("/position"));
    if (!Builder.GetBuilder().SetNodeLocation(Handle.NodeID, Location, &Handle.NodeID)) {
      OutError = DocumentFailure(TEXT("metasound_node_position_failed"),
          TEXT("The MetaSound builder rejected the requested node position."), TEXT("position_write_succeeded"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Position->Get());
      auto RequestedPosition = MakeShared<FJsonObject>();
      RequestedPosition->SetNumberField(TEXT("x"), Location.X);
      RequestedPosition->SetNumberField(TEXT("y"), Location.Y);
      OutError.ProposedValue = MakeShared<FJsonValueObject>(RequestedPosition);
      return false;
    }
    return true;
  };
  for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex) {
    const TSharedPtr<FJsonValue>& Value = (*Nodes)[NodeIndex];
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString Id;
    FString Kind;
    FString ClassNameText;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id) ||
        !Item->TryGetStringField(TEXT("kind"), Kind) || Id.IsEmpty()) {
      const FString Field = !Item ? FString() : Id.IsEmpty() ? TEXT("id") : TEXT("kind");
      OutError = DocumentFailure(TEXT("metasound_graph_node_invalid"),
          !Item ? TEXT("Each MetaSound graph node must be an object.")
                : TEXT("A MetaSound graph node requires a nonempty id and a string kind."),
          Field, Item ? Item->TryGetField(Field) : Value,
          MakeShared<FJsonValueString>(!Item ? TEXT("a node object") : Field == TEXT("id") ? TEXT("a nonempty string") : TEXT("a string")),
          Item ? Item.Get() : Graph.Get(), Field, FString(),
          Item ? FString() : FString::Printf(TEXT("/nodes/%d"), NodeIndex));
      return false;
    }
    if (Kind == TEXT("graph_input") || Kind == TEXT("graph_output")) {
      {
        FString MemberName;
        Item->TryGetStringField(TEXT("member_name"), MemberName);
        EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
        FMetaSoundNodeHandle Handle =
            Kind == TEXT("graph_input")
                ? Builder.FindGraphInputNode(*MemberName, FindResult)
                : Builder.FindGraphOutputNode(*MemberName, FindResult);
        if (FindResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = MetaOperationFailure(TEXT("metasound_graph_member_node_missing"),
              TEXT("The requested graph member has no resolvable native node."),
              FindResult, Item.Get(), TEXT("member_name"), MemberName);
          return false;
        }
        NodeHandles.Add(Id, Handle);
      }
      auto& NodeIntent = Intent.MetaNodes.AddDefaulted_GetRef();
      NodeIntent.Id = NodeHandles.FindChecked(Id).NodeID;
      Item->TryGetStringField(TEXT("member_name"), NodeIntent.PublicName);
      NodeIntent.SourcePointer = NodePointers.IsValidIndex(NodeIndex) ? NodePointers[NodeIndex]
          : FString::Printf(TEXT("/graph/nodes/%d"), NodeIndex);
      if (!ApplyPosition(Item, NodeHandles.FindChecked(Id), NodeIntent)) return false;
      if (!ApplyMetaSoundNodeDefaults(Builder, NodeHandles.FindChecked(Id), *Item, Intent.MetaDefaults, OutError)) return false;
      continue;
    }
    if (Kind != TEXT("external")) {
      OutError = DocumentFailure(TEXT("metasound_node_kind_unsupported"),
          TEXT("The requested MetaSound node kind is not supported by this authoring surface."),
          TEXT("kind"), MakeShared<FJsonValueString>(Kind),
          MakeShared<FJsonValueString>(TEXT("graph_input, graph_output, or external")), Item.Get(), TEXT("kind"));
      return false;
    }
    if (!Item->TryGetStringField(TEXT("class_name"), ClassNameText)) {
      OutError = DocumentFailure(TEXT("metasound_node_class_name_required"),
          TEXT("The external node has no resolved MetaSound class name."), TEXT("class_name"),
          Item->TryGetField(TEXT("class_name")), MakeShared<FJsonValueString>(TEXT("a resolved class name")), Item.Get(), TEXT("class_name"));
      return false;
    }
    FMetasoundFrontendClassName ClassName;
    if (!FMetasoundFrontendClassName::Parse(ClassNameText, ClassName)) {
      OutError = DocumentFailure(TEXT("metasound_node_class_name_invalid"),
          TEXT("The MetaSound class name could not be parsed."), TEXT("class_name"),
          MakeShared<FJsonValueString>(ClassNameText), MakeShared<FJsonValueString>(TEXT("a valid MetaSound frontend class name")), Item.Get(), TEXT("class_name"));
      return false;
    }
    int32 MajorVersion = 1;
    if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Item, TEXT("major_version"), MajorVersion, OutError)
        || !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Item,
            {{TEXT("inputs"), EJson::Array}}, OutError, Id)) return false;
    FMetaSoundNodeHandle Handle = NodeHandles.FindRef(Id);
    if (const auto* Existing = Builder.GetBuilder().FindNode(Handle.NodeID)) {
      const auto* Dependency = Builder.GetBuilder().FindDependency(Existing->ClassID);
      if (!Dependency || Dependency->Metadata.GetClassName() != ClassName
          || Dependency->Metadata.GetVersion().Major != MajorVersion) {
        EMetaSoundBuilderResult RemoveResult;
        Builder.RemoveNode(Handle, RemoveResult);
        if (RemoveResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = MetaOperationFailure(TEXT("metasound_replace_node_failed"),
              TEXT("The existing MetaSound node could not be removed before replacing its class or major version."),
              RemoveResult, Item.Get(), TEXT("class_name"), ClassNameText);
          return false;
        }
        Handle = FMetaSoundNodeHandle();
      }
    }
    if (!Handle.IsSet()) {
      EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
      Handle = Builder.AddNodeByClassName(ClassName, AddResult, MajorVersion);
      if (AddResult != EMetaSoundBuilderResult::Succeeded || !Handle.IsSet()) {
        OutError = AddResult != EMetaSoundBuilderResult::Succeeded
            ? MetaOperationFailure(TEXT("metasound_add_node_failed"),
                TEXT("The MetaSound builder could not add the requested class and major version."),
                AddResult, Item.Get(), TEXT("class_name"), ClassNameText, MakeShared<FJsonValueNumber>(MajorVersion))
            : DocumentFailure(TEXT("metasound_add_node_failed"),
                TEXT("The MetaSound builder reported success but returned no node handle."),
                TEXT("node_handle_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
                Item.Get(), TEXT("class_name"), ClassNameText);
        return false;
      }
      ++OutStats.Nodes;
    }
    DesiredExternal.Add(Handle.NodeID);
    NodeHandles.Add(Id, Handle);
    auto& NodeIntent = Intent.MetaNodes.AddDefaulted_GetRef();
    NodeIntent.Id = Handle.NodeID;
    NodeIntent.ClassName = ClassNameText;
    NodeIntent.PublicName = ClassNameText;
    NodeIntent.Major = MajorVersion;
    // The native add API selects the highest minor in this major, so only
    // class + major are authoring selectors here (unlike the exact typed hook).
    NodeIntent.SourcePointer = NodePointers.IsValidIndex(NodeIndex) ? NodePointers[NodeIndex]
        : FString::Printf(TEXT("/graph/nodes/%d"), NodeIndex);
    if (!ApplyPosition(Item, Handle, NodeIntent)) return false;
    if (!ApplyMetaSoundNodeDefaults(Builder, Handle, *Item, Intent.MetaDefaults, OutError)) return false;
  }
  // The complete ordinary graph is already restored by the common collection
  // owner. Retain native identities and state; remove only absent external nodes.
  for (const FGuid& Id : ExistingExternal) {
    if (DesiredExternal.Contains(Id) || !Builder.GetBuilder().FindNode(Id)) continue;
    EMetaSoundBuilderResult RemoveResult;
    Builder.RemoveNode(FMetaSoundNodeHandle(Id), RemoveResult);
    if (RemoveResult != EMetaSoundBuilderResult::Succeeded) {
      const auto* Existing = Builder.GetBuilder().FindNode(Id);
      OutError = MetaOperationFailure(TEXT("metasound_remove_existing_node_failed"),
          TEXT("A native external node omitted from the complete graph could not be removed."),
          RemoveResult, Graph.Get(), TEXT("nodes"), Existing ? Existing->Name.ToString() : FString());
      OutError.InternalContext = Id.ToString();
      return false;
    }
    ++OutStats.Nodes;
  }
  Intent.CompleteMetaExternalNodes = DesiredExternal;
  const TArray<TSharedPtr<FJsonValue>> *Edges = nullptr;
  if (!Graph->TryGetArrayField(TEXT("edges"), Edges) || !Edges)
    return true;
  for (int32 EdgeIndex = 0; EdgeIndex < Edges->Num(); ++EdgeIndex) {
    const TSharedPtr<FJsonValue> &Value = (*Edges)[EdgeIndex];
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString FromNode;
    FString FromPin;
    FString ToNode;
    FString ToPin;
    if (!Item.IsValid() ||
        !Item->TryGetStringField(TEXT("from_node"), FromNode) ||
        !Item->TryGetStringField(TEXT("from_pin"), FromPin) ||
        !Item->TryGetStringField(TEXT("to_node"), ToNode) ||
        !Item->TryGetStringField(TEXT("to_pin"), ToPin) ||
        !NodeHandles.Contains(FromNode) || !NodeHandles.Contains(ToNode)) {
      FString Field;
      FString Required = TEXT("an edge object");
      if (Item) {
        for (const TCHAR* Key : {TEXT("from_node"), TEXT("from_pin"), TEXT("to_node"), TEXT("to_pin")}) {
          FString Text;
          if (!Item->TryGetStringField(Key, Text)) { Field = Key; Required = TEXT("a string"); break; }
        }
        if (Field.IsEmpty()) { Field = !NodeHandles.Contains(FromNode) ? TEXT("from_node") : TEXT("to_node"); Required = TEXT("an existing graph node alias"); }
      }
      OutError = DocumentFailure(TEXT("metasound_edge_invalid"),
          TEXT("The MetaSound edge requires string endpoints that reference existing graph nodes."),
          Field, Item ? Item->TryGetField(Field) : Value, MakeShared<FJsonValueString>(Required),
          Item ? Item.Get() : Graph.Get(), Field, FString(),
          Item ? FString() : FString::Printf(TEXT("/edges/%d"), EdgeIndex));
      return false;
    }
    if (Connections && Connections->IsUnchanged(FromNode, FromPin, ToNode, ToPin)) continue;
    FDiagnostic PinResolutionError;
    auto ResolvePinName = [&](const FMetaSoundNodeHandle& Handle, const FString& Alias,
                              UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection Direction) -> FName {
      if (!Connections || !Connections->bHasResolvedIntent) return FName(*Alias);
      const auto* Native = Builder.GetBuilder().FindNode(Handle.NodeID);
      if (!Native) return NAME_None;
      TArray<UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasInput> Pins;
      for (const auto& Pin : Native->Interface.Inputs)
        Pins.Add({Pin.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower), Pin.Name.ToString(), 0,
            UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input});
      for (const auto& Pin : Native->Interface.Outputs)
        Pins.Add({Pin.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower), Pin.Name.ToString(), 0,
            UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output});
      UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasProjection Projection;
      UE::UAI::HjsonWorkspace::FWorkspaceError Error;
      if (!UE::UAI::HjsonWorkspace::TryBuildGraphAuthoringPinAliasProjection(Pins, Projection, Error)) {
        if (PinResolutionError.IsEmpty()) PinResolutionError = Error.GetDiagnostic();
        return NAME_None;
      }
      for (const auto& Pin : Pins)
        if (Pin.Direction == Direction && Projection.AliasBySourceId.FindRef(Pin.SourceId).Equals(Alias, ESearchCase::IgnoreCase))
          return FName(*Pin.SemanticName);
      return NAME_None;
    };
    // Unchanged links above retain native vertices. New public links resolve
    // aliases against the reconstructed node using the same export owner.
    EMetaSoundBuilderResult FromResult = EMetaSoundBuilderResult::Failed;
    EMetaSoundBuilderResult ToResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeOutputHandle Output =
        Builder.FindNodeOutputByName(NodeHandles.FindChecked(FromNode),
                                     ResolvePinName(NodeHandles.FindChecked(FromNode), FromPin,
                                         UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output), FromResult);
    const FMetaSoundBuilderNodeInputHandle Input = Builder.FindNodeInputByName(
        NodeHandles.FindChecked(ToNode), ResolvePinName(NodeHandles.FindChecked(ToNode), ToPin,
            UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input), ToResult);
    if (FromResult != EMetaSoundBuilderResult::Succeeded ||
        ToResult != EMetaSoundBuilderResult::Succeeded) {
      if (!PinResolutionError.IsEmpty()) OutError = MoveTemp(PinResolutionError);
      else {
        const bool bFromFailed = FromResult != EMetaSoundBuilderResult::Succeeded;
        OutError = MetaOperationFailure(TEXT("metasound_edge_pin_not_found"),
            bFromFailed ? TEXT("The source output pin of the MetaSound connection could not be resolved.")
                        : TEXT("The destination input pin of the MetaSound connection could not be resolved."),
            bFromFailed ? FromResult : ToResult, Item.Get(), bFromFailed ? TEXT("from_pin") : TEXT("to_pin"),
            (bFromFailed ? FromNode : ToNode) + TEXT(":") + (bFromFailed ? FromPin : ToPin));
      }
      return false;
    }
    EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
    Builder.ConnectNodes(Output, Input, ConnectResult);
    if (ConnectResult != EMetaSoundBuilderResult::Succeeded) {
      OutError = MetaOperationFailure(TEXT("metasound_connect_nodes_failed"),
          TEXT("The MetaSound builder rejected the requested connection."), ConnectResult, Item.Get(), FString(),
          FromNode + TEXT(":") + FromPin + TEXT(" -> ") + ToNode + TEXT(":") + ToPin);
      return false;
    }
    Intent.MetaLinks.Add({Output.NodeID, Output.VertexID, Input.NodeID, Input.VertexID, true, EdgePointers.IsValidIndex(EdgeIndex)
        ? EdgePointers[EdgeIndex] : FString::Printf(TEXT("/graph/edges/%d"), EdgeIndex)});
    ++OutStats.Edges;
  }
  // ConnectNodes only upserts. Explicitly remove stale edges among represented
  // nodes; edges touching unsupported native node kinds stay outside this surface.
  if (Connections && Connections->bHasResolvedIntent) return true;
  TSet<FGuid> RepresentedNodes;
  for (const auto& Pair : NodeHandles) RepresentedNodes.Add(Pair.Value.NodeID);
  Intent.CompleteMetaLinkNodes = RepresentedNodes;
  const TArray<FMetasoundFrontendEdge> ExistingEdges = Builder.GetBuilder().FindConstBuildGraphChecked().Edges;
  for (const auto& Edge : ExistingEdges) {
    if (!RepresentedNodes.Contains(Edge.FromNodeID) || !RepresentedNodes.Contains(Edge.ToNodeID)) continue;
    if (Intent.MetaLinks.ContainsByPredicate([&](const auto& Link) {
      return Link.FromNode == Edge.FromNodeID && Link.FromPin == Edge.FromVertexID
          && Link.ToNode == Edge.ToNodeID && Link.ToPin == Edge.ToVertexID;
    })) continue;
    if (!Builder.GetBuilder().RemoveEdge(Edge)) {
      OutError = DocumentFailure(TEXT("metasound_remove_edge_failed"),
          TEXT("A native connection omitted from the complete graph could not be removed."),
          TEXT("edge_removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.Get(), TEXT("edges"));
      const auto* FromNode = Builder.GetBuilder().FindNode(Edge.FromNodeID);
      const auto* ToNode = Builder.GetBuilder().FindNode(Edge.ToNodeID);
      const auto* FromPin = FromNode ? FromNode->Interface.Outputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.FromVertexID; }) : nullptr;
      const auto* ToPin = ToNode ? ToNode->Interface.Inputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.ToVertexID; }) : nullptr;
      OutError.CandidateName = (FromNode ? FromNode->Name.ToString() : TEXT("<missing node>")) + FString(TEXT(":"))
          + (FromPin ? FromPin->Name.ToString() : TEXT("<missing pin>")) + TEXT(" -> ")
          + (ToNode ? ToNode->Name.ToString() : TEXT("<missing node>")) + TEXT(":")
          + (ToPin ? ToPin->Name.ToString() : TEXT("<missing pin>"));
      OutError.InternalContext = Edge.FromNodeID.ToString() + TEXT(":") + Edge.FromVertexID.ToString()
          + TEXT(" -> ") + Edge.ToNodeID.ToString() + TEXT(":") + Edge.ToVertexID.ToString();
      return false;
    }
    ++OutStats.Edges;
  }
  return true;
}

bool ApplyMetaSoundDocument(UObject &Asset,
                            const TSharedPtr<const FJsonObject> &Document,
                            FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                            bool bCompleteDocument, FAudioGraphIntent* OutGraphIntent,
                            const FString& CanonicalAssetPath) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("interfaces"), TEXT("members"),
                       TEXT("graph"), TEXT("nodes"), TEXT("connections"),
                       TEXT("edges")},
                      TEXT("metasound_document"), OutError))
    return false;
  if (Document && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("settings"), EJson::Object}, {TEXT("members"), EJson::Object},
       {TEXT("graph"), EJson::Object}, {TEXT("nodes"), EJson::Object},
       {TEXT("connections"), EJson::Array}, {TEXT("edges"), EJson::Object}}, OutError)) return false;
  TSharedPtr<FJsonObject> Current = BuildMetaSoundDocument(Asset, OutError);
  if (!Current.IsValid())
    return false;
  if (!RequireCurrentValue(Document, TEXT("interfaces"), Current, OutError))
    return false;
  const TSharedPtr<FJsonObject> *RequestedSettings = nullptr;
  const TSharedPtr<FJsonObject> *CurrentSettings = nullptr;
  if (Document.IsValid() &&
      Document->TryGetObjectField(TEXT("settings"), RequestedSettings) &&
      RequestedSettings && RequestedSettings->IsValid() &&
      Current->TryGetObjectField(TEXT("settings"), CurrentSettings) &&
      CurrentSettings && CurrentSettings->IsValid()) {
    const auto Compared = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(
        MakeShared<FJsonValueObject>(*CurrentSettings), MakeShared<FJsonValueObject>(*RequestedSettings));
    if (!Compared.bEquivalent) {
      OutError = FDiagnostic::Failure(TEXT("metasound_asset_type_immutable"),
          TEXT("The MetaSound asset settings are read-only and differ from the current asset."), TEXT("authoring"));
      Compared.WithPrefix(TEXT("/settings")).ApplyTo(OutError);
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
      return false;
    }
  }
  const TSharedPtr<FJsonObject> *Members = nullptr;
  const TSharedPtr<FJsonObject> *Graph = nullptr;
  const bool bHasMembers =
      Document.IsValid() &&
      Document->TryGetObjectField(TEXT("members"), Members) && Members &&
      Members->IsValid();
  const bool bHasGraph = Document.IsValid() &&
                         Document->TryGetObjectField(TEXT("graph"), Graph) &&
                         Graph && Graph->IsValid();
  const TSharedPtr<FJsonObject> *PublicNodes = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *PublicConnections = nullptr;
  const TSharedPtr<FJsonObject> *CapturedEdges = nullptr;
  const bool bHasPublicNodes =
      Document.IsValid() &&
      Document->TryGetObjectField(TEXT("nodes"), PublicNodes) &&
      PublicNodes && PublicNodes->IsValid();
  const bool bHasPublicConnections =
      Document.IsValid() &&
      Document->TryGetArrayField(TEXT("connections"), PublicConnections) &&
      PublicConnections;
  const bool bHasCapturedEdges =
      Document.IsValid() &&
      Document->TryGetObjectField(TEXT("edges"), CapturedEdges) &&
      CapturedEdges && CapturedEdges->IsValid();
  const bool bHasPublicGraph =
      bHasPublicNodes && (bHasPublicConnections || bHasCapturedEdges);
  if (!bHasMembers && !bHasGraph && !bHasPublicGraph)
    return true;
  UMetaSoundBuilderBase *Builder = nullptr;
  if (!GetMetaSoundBuilder(Asset, Builder, OutError))
    return false;
  FMetaSoundDefaultPageScope PageScope(Builder);
  if (!PageScope.IsReady()) {
    OutError = DocumentFailure(TEXT("metasound_default_graph_missing"),
        TEXT("The MetaSound builder could not select the asset's default graph page for apply."),
        TEXT("default_page_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }

  TMap<FString, FMetaSoundNodeHandle> NodeHandles;
  FAudioGraphIntent GraphIntent;
  TArray<FMetaSoundMemberIntent> MemberIntents;
  TArray<TPair<FName, bool>> MemberRemovals;
  TSharedPtr<FJsonObject> GraphForApply;
  TMap<FString, UE::UAI::Contract::FSourceLocation> GraphSources;
  TArray<FString> NodePointers;
  TArray<FString> EdgePointers;
  const auto AppendGraphSources = [&](const FString& Prefix, const FJsonObject& Source) {
    for (const auto& Pair : UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(Source))
      GraphSources.Add(Prefix + Pair.Key, Pair.Value);
  };
  UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext Connections;
  Connections.bHasResolvedIntent = bCompleteDocument && bHasCapturedEdges;
  if (bHasMembers) {
    if (!ReadMetaSoundMemberIntents(*Members, MemberIntents, OutError)
        || (bCompleteDocument && !PlanMetaSoundMemberRemovals(*Builder, **Members, MemberIntents, MemberRemovals, OutError))) return false;
    if (bCompleteDocument) for (auto& Intent : MemberIntents) {
      if (!Intent.Default.IsSet()) continue;
      const auto* Input = Intent.bInput ? Builder->GetBuilder().FindGraphInput(Intent.Name) : nullptr;
      const auto* Output = !Intent.bInput ? Builder->GetBuilder().FindGraphOutput(Intent.Name) : nullptr;
      if (Input && Input->TypeName != Intent.Type) {
        const auto* Default = Input->FindConstDefault(Metasound::Frontend::DefaultPageID);
        if (Default && Default->IsEqual(Intent.Default.GetValue())) Intent.Default.Reset();
      } else if (Output && Output->TypeName != Intent.Type) {
        EMetaSoundBuilderResult FindResult, ReadResult;
        const auto Node = Builder->FindGraphOutputNode(Intent.Name, FindResult);
        const auto Pin = Builder->FindNodeInputByName(Node, Intent.Name, FindResult);
        const auto Default = Builder->GetNodeInputDefault(Pin, ReadResult);
        if (FindResult == EMetaSoundBuilderResult::Succeeded && ReadResult == EMetaSoundBuilderResult::Succeeded
            && Default.IsEqual(Intent.Default.GetValue())) Intent.Default.Reset();
      }
    }
  }
  if (bHasGraph || bHasPublicGraph) {
    GraphForApply = MakeShared<FJsonObject>();
    if (bHasGraph) {
      GraphSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(**Graph);
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Field : (*Graph)->Values)
        GraphForApply->SetField(Field.Key, Field.Value);
    }
    if (bHasPublicGraph) {
      for (auto It = GraphSources.CreateIterator(); It; ++It)
        if (It.Key() == TEXT("/nodes") || It.Key().StartsWith(TEXT("/nodes/"))
            || It.Key() == TEXT("/edges") || It.Key().StartsWith(TEXT("/edges/"))) It.RemoveCurrent();
      GraphSources.Add(TEXT("/nodes"), UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(
          UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(**PublicNodes), FString()));
      GraphSources.Add(TEXT("/edges"), UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(
          UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Document),
          bHasCapturedEdges ? TEXT("/edges") : TEXT("/connections")));
      TArray<TSharedPtr<FJsonValue>> Nodes;
      TArray<FString> NodeAliases;
      (*PublicNodes)->Values.GetKeys(NodeAliases);
      NodeAliases.Sort();
      for (const FString &Alias : NodeAliases) {
        const TSharedPtr<FJsonObject> *Node = nullptr;
        if (!(*PublicNodes)->TryGetObjectField(Alias, Node) ||
            !Node || !Node->IsValid()) {
          OutError = DocumentFailure(TEXT("metasound_graph_node_invalid"),
              TEXT("Each named MetaSound node must be an object."), TEXT("node"),
              (*PublicNodes)->TryGetField(Alias), MakeShared<FJsonValueString>(TEXT("a node object")),
              PublicNodes->Get(), Alias, Alias);
          return false;
        }
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(**Node);
        FString Operation;
        Item->TryGetStringField(TEXT("operation"), Operation);
        if (Operation == TEXT("remove")) continue;
        FString NativeId;
        FGuid NativeGuid;
        if (Item->TryGetStringField(TEXT("id"), NativeId) && FGuid::Parse(NativeId, NativeGuid)) {
          if (Builder->GetBuilder().FindNode(NativeGuid)) NodeHandles.Add(Alias, FMetaSoundNodeHandle(NativeGuid));
          else if (Operation == TEXT("update")) {
            OutError = DocumentFailure(TEXT("metasound_graph_node_identity_missing"),
                TEXT("The MetaSound node selected for update no longer exists in the native graph."),
                TEXT("node_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
                Node->Get(), TEXT("operation"), Alias);
            OutError.InternalContext = NativeId;
            return false;
          }
        }
        Item->SetStringField(TEXT("id"), Alias);
        FString Kind;
        Item->TryGetStringField(TEXT("kind"), Kind);
        if (Kind.IsEmpty() && Item->HasField(TEXT("candidate"))) {
          Kind = TEXT("external"); Item->SetStringField(TEXT("kind"), Kind);
        }
        if (Kind == TEXT("external")) {
          if (Item->HasField(TEXT("class_name")) ||
              Item->HasField(TEXT("major_version")) ||
              Item->HasField(TEXT("minor_version"))) {
            const FString Field = Item->HasField(TEXT("class_name")) ? TEXT("class_name")
                : Item->HasField(TEXT("major_version")) ? TEXT("major_version") : TEXT("minor_version");
            OutError = DocumentFailure(TEXT("metasound_model_native_binding_forbidden"),
                TEXT("An authored external node must select a readable candidate instead of supplying native class or version fields."),
                Field, Item->TryGetField(Field), nullptr, Node->Get(), Field, Alias);
            return false;
          }
          const TSharedPtr<FJsonObject> *CandidateObject = nullptr;
          FString CandidateName;
          if (!Item->TryGetObjectField(TEXT("candidate"), CandidateObject) ||
              !CandidateObject || !CandidateObject->IsValid() ||
              !(*CandidateObject)->TryGetStringField(TEXT("name"),
                                                     CandidateName) ||
              CandidateName.IsEmpty()) {
            const bool bHasCandidateObject = CandidateObject && CandidateObject->IsValid();
            OutError = DocumentFailure(TEXT("metasound_readable_candidate_required"),
                TEXT("An external MetaSound node requires a candidate object with a nonempty readable name."),
                bHasCandidateObject ? TEXT("name") : TEXT("candidate"),
                bHasCandidateObject ? (*CandidateObject)->TryGetField(TEXT("name")) : Item->TryGetField(TEXT("candidate")),
                MakeShared<FJsonValueString>(bHasCandidateObject ? TEXT("a nonempty candidate name") : TEXT("a candidate object")),
                bHasCandidateObject ? CandidateObject->Get() : Node->Get(), bHasCandidateObject ? TEXT("name") : TEXT("candidate"), Alias);
            return false;
          }
          UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector CandidateSelector;
          UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
          if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
                  *CandidateObject, TEXT("candidate"), CandidateSelector, SelectorError)) {
            OutError = SelectorError.GetDiagnostic();
            return false;
          }
          UE::UAI::QueryOwners::AudioGraph::FAudioGraphCandidate Resolved;
          UE::UAI::QueryOwners::AudioGraph::FAudioGraphCandidateQueryError
              ResolveError;
          if (!UE::UAI::QueryOwners::AudioGraph::TryResolveAudioGraphCandidate(
                  Asset, CandidateName, Resolved, ResolveError, nullptr, CandidateSelector.Select)) {
            OutError = MoveTemp(ResolveError);
            UeAgentAuthoringDiagnostics::LocateFailure(OutError, **CandidateObject, TEXT("audio_graph_apply"));
            return false;
          }
          if (!Resolved.bSupported || !Resolved.SupportedOperations.Contains(TEXT("create"))) {
            if (!Resolved.SupportDiagnostic.IsEmpty()) OutError = Resolved.SupportDiagnostic;
            else if (!Resolved.bSupported) {
              OutError = DocumentFailure(TEXT("metasound_candidate_resolution_failed"),
                  TEXT("The selected MetaSound candidate is not supported for authoring."), TEXT("candidate_supported"),
                  MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), CandidateObject->Get(), TEXT("name"));
            } else {
              TArray<TSharedPtr<FJsonValue>> Supported;
              for (const FString& OperationName : Resolved.SupportedOperations) Supported.Add(MakeShared<FJsonValueString>(OperationName));
              OutError = DocumentFailure(TEXT("metasound_candidate_resolution_failed"),
                  TEXT("The selected MetaSound candidate does not support node creation."), TEXT("supported_operations"),
                  MakeShared<FJsonValueArray>(MoveTemp(Supported)), MakeShared<FJsonValueString>(TEXT("create")), CandidateObject->Get(), TEXT("name"));
            }
            OutError.CandidateName = CandidateName;
            UeAgentAuthoringDiagnostics::LocateFailure(OutError, **CandidateObject,
                TEXT("audio_graph_apply"), FString());
            return false;
          }
          Item->SetStringField(TEXT("class_name"),
                               Resolved.NativeClassName.IsEmpty()
                                   ? Resolved.Name
                                   : Resolved.NativeClassName);
          Item->SetNumberField(TEXT("major_version"), Resolved.MajorVersion);
          Item->SetNumberField(TEXT("minor_version"), Resolved.MinorVersion);
          Item->RemoveField(TEXT("candidate"));
        }
        const FString NodePrefix = FString::Printf(TEXT("/nodes/%d"), Nodes.Num());
        AppendGraphSources(NodePrefix, **Node);
        if (const auto Candidate = (*Node)->TryGetField(TEXT("candidate")); Candidate && Candidate->Type == EJson::Object) {
          const auto CandidateSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Candidate->AsObject());
          const auto CandidateSource = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(CandidateSources, TEXT("/name"));
          for (const TCHAR* Field : {TEXT("/class_name"), TEXT("/major_version"), TEXT("/minor_version")}) GraphSources.Add(NodePrefix + Field, CandidateSource);
        }
        NodePointers.Add(TEXT("/nodes/") + Alias.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
        Nodes.Add(MakeShared<FJsonValueObject>(Item));
      }
      GraphForApply->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
      TArray<TSharedPtr<FJsonValue>> Edges;
      for (const TSharedPtr<FJsonValue> &Value :
           bHasPublicConnections ? *PublicConnections
                                 : TArray<TSharedPtr<FJsonValue>>{}) {
        const TSharedPtr<FJsonObject> Connection =
            Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Connection.IsValid()) {
          OutError = DocumentFailure(TEXT("metasound_graph_connection_invalid"),
              TEXT("Each MetaSound connection must be an object."), TEXT("connection"), Value,
              MakeShared<FJsonValueString>(TEXT("a connection object")), Document.Get(), FString(), FString(),
              FString::Printf(TEXT("/connections/%d"), Edges.Num()));
          return false;
        }
        FString From;
        FString To;
        if (!Connection->TryGetStringField(TEXT("from"), From) ||
            !Connection->TryGetStringField(TEXT("to"), To)) {
          FString Ignored;
          const FString Field = !Connection->TryGetStringField(TEXT("from"), Ignored) ? TEXT("from") : TEXT("to");
          OutError = DocumentFailure(TEXT("metasound_graph_connection_endpoint_invalid"),
              TEXT("A MetaSound connection endpoint must be a string containing a node and pin alias."),
              Field, Connection->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("a node:pin endpoint string")),
              Connection.Get(), Field);
          return false;
        }
        FString FromNode;
        FString FromPin;
        FString ToNode;
        FString ToPin;
        UE::UAI::HjsonWorkspace::FWorkspaceError EndpointError;
        const bool bFromParsed = UE::UAI::HjsonWorkspace::TryParseGraphAuthoringEndpoint(From, FromNode, FromPin, EndpointError);
        if (!bFromParsed || !UE::UAI::HjsonWorkspace::TryParseGraphAuthoringEndpoint(To, ToNode, ToPin, EndpointError)) {
          OutError = EndpointError.GetDiagnostic();
          UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Connection, bFromParsed ? TEXT("to") : TEXT("from"));
          return false;
        }
        TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
        Edge->SetStringField(TEXT("from_node"), FromNode);
        Edge->SetStringField(TEXT("from_pin"), FromPin);
        Edge->SetStringField(TEXT("to_node"), ToNode);
        Edge->SetStringField(TEXT("to_pin"), ToPin);
        const FString EdgePrefix = FString::Printf(TEXT("/edges/%d"), Edges.Num());
        AppendGraphSources(EdgePrefix, *Connection);
        const auto ConnectionSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Connection);
        for (const TCHAR* Field : {TEXT("from"), TEXT("to")}) {
          const auto Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ConnectionSources, FString(TEXT("/")) + Field);
          GraphSources.Add(EdgePrefix + TEXT("/") + Field + TEXT("_node"), Source);
          GraphSources.Add(EdgePrefix + TEXT("/") + Field + TEXT("_pin"), Source);
        }
        EdgePointers.Add(FString::Printf(TEXT("/connections/%d"), Edges.Num()));
        Edges.Add(MakeShared<FJsonValueObject>(Edge));
      }
      if (bHasCapturedEdges) {
        TMap<FString, FString> AliasByNativeNodeId;
        for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
             (*PublicNodes)->Values) {
          const TSharedPtr<FJsonObject> Node =
              Pair.Value.IsValid() && Pair.Value->Type == EJson::Object
                  ? Pair.Value->AsObject()
                  : nullptr;
          FString NativeId;
          if (Node.IsValid() &&
              Node->TryGetStringField(TEXT("id"), NativeId) &&
              !NativeId.IsEmpty())
            AliasByNativeNodeId.Add(NativeId, Pair.Key);
        }
        TArray<FString> EdgeAliases;
        (*CapturedEdges)->Values.GetKeys(EdgeAliases);
        EdgeAliases.Sort();
        for (const FString &EdgeAlias : EdgeAliases) {
          const TSharedPtr<FJsonObject> *CapturedEdge = nullptr;
          const TSharedPtr<FJsonObject> *From = nullptr;
          const TSharedPtr<FJsonObject> *To = nullptr;
          FString FromNode;
          FString FromPin;
          FString ToNode;
          FString ToPin;
          if (!(*CapturedEdges)->TryGetObjectField(EdgeAlias, CapturedEdge) ||
              !CapturedEdge || !CapturedEdge->IsValid() ||
              !(*CapturedEdge)->TryGetObjectField(TEXT("from"), From) ||
              !From || !From->IsValid() ||
              !(*CapturedEdge)->TryGetObjectField(TEXT("to"), To) || !To ||
              !To->IsValid() ||
              !(*From)->TryGetStringField(TEXT("node"), FromNode) ||
              !(*From)->TryGetStringField(TEXT("pin"), FromPin) ||
              !(*To)->TryGetStringField(TEXT("node"), ToNode) ||
              !(*To)->TryGetStringField(TEXT("pin"), ToPin)) {
            const FJsonObject* Source = CapturedEdges->Get();
            FString Field = EdgeAlias;
            if (CapturedEdge && CapturedEdge->IsValid()) {
              Source = CapturedEdge->Get();
              Field = !From || !From->IsValid() ? TEXT("from") : !To || !To->IsValid() ? TEXT("to") : FString();
              if (Field.IsEmpty()) {
                FString Ignored;
                if (!(*From)->TryGetStringField(TEXT("node"), Ignored)) { Source = From->Get(); Field = TEXT("node"); }
                else if (!(*From)->TryGetStringField(TEXT("pin"), Ignored)) { Source = From->Get(); Field = TEXT("pin"); }
                else { Source = To->Get(); Field = !(*To)->TryGetStringField(TEXT("node"), Ignored) ? TEXT("node") : TEXT("pin"); }
              }
            }
            const bool bStringRequired = Source == (From ? From->Get() : nullptr) || Source == (To ? To->Get() : nullptr);
            OutError = DocumentFailure(TEXT("metasound_captured_edge_invalid"),
                TEXT("The captured MetaSound edge requires from/to objects with string node and pin fields."),
                Field, Source->TryGetField(Field), MakeShared<FJsonValueString>(bStringRequired ? TEXT("a string") : TEXT("an endpoint object")),
                Source, Field, EdgeAlias);
            return false;
          }
          FString Operation;
          if ((*CapturedEdge)->TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("remove")) continue;
          FromNode = AliasByNativeNodeId.FindRef(FromNode).IsEmpty()
                         ? FromNode
                         : AliasByNativeNodeId.FindRef(FromNode);
          ToNode = AliasByNativeNodeId.FindRef(ToNode).IsEmpty()
                       ? ToNode
                       : AliasByNativeNodeId.FindRef(ToNode);
          if (Operation == TEXT("update")) Connections.RecordUnchanged(FromNode, FromPin, ToNode, ToPin);
          TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
          Edge->SetStringField(TEXT("from_node"), FromNode);
          Edge->SetStringField(TEXT("from_pin"), FromPin);
          Edge->SetStringField(TEXT("to_node"), ToNode);
          Edge->SetStringField(TEXT("to_pin"), ToPin);
          const FString EdgePrefix = FString::Printf(TEXT("/edges/%d"), Edges.Num());
          AppendGraphSources(EdgePrefix, **CapturedEdge);
          const auto CapturedSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(**CapturedEdge);
          for (const TCHAR* Direction : {TEXT("from"), TEXT("to")})
            for (const TCHAR* Part : {TEXT("node"), TEXT("pin")})
              GraphSources.Add(EdgePrefix + TEXT("/") + Direction + TEXT("_") + Part,
                  UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(CapturedSources, FString(TEXT("/")) + Direction + TEXT("/") + Part));
          EdgePointers.Add(TEXT("/edges/") + EdgeAlias.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
          Edges.Add(MakeShared<FJsonValueObject>(Edge));
        }
      }
      GraphForApply->SetArrayField(TEXT("edges"), MoveTemp(Edges));
    }
  }
  UeAgentAuthoringDiagnostics::FScopedSourceLocations GraphSourceScope(GraphForApply.Get(),
      [&GraphSources](const FString& Pointer) {
        return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(GraphSources, Pointer);
      });
  if (Connections.bHasResolvedIntent && GraphForApply) {
    // Resolve removals before member types or external nodes reconstruct pins.
    // Unchanged relations stay native; only explicitly requested links are later replayed.
    TMap<FGuid, FString> Aliases;
    for (const auto& Pair : NodeHandles) Aliases.Add(Pair.Value.NodeID, Pair.Key);
    UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext Requested;
    Requested.bHasResolvedIntent = true;
    const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
    if (GraphForApply->TryGetArrayField(TEXT("edges"), Edges))
      for (const auto& Value : *Edges) {
        const auto Edge = Value->AsObject();
        Requested.RecordUnchanged(Edge->GetStringField(TEXT("from_node")), Edge->GetStringField(TEXT("from_pin")),
            Edge->GetStringField(TEXT("to_node")), Edge->GetStringField(TEXT("to_pin")));
      }
    const auto ExistingEdges = Builder->GetBuilder().FindConstBuildGraphChecked().Edges;
    for (const auto& Edge : ExistingEdges) {
      const FString* FromAlias = Aliases.Find(Edge.FromNodeID);
      const FString* ToAlias = Aliases.Find(Edge.ToNodeID);
      if (!FromAlias || !ToAlias) continue;
      const auto* FromNode = Builder->GetBuilder().FindNode(Edge.FromNodeID);
      const auto* ToNode = Builder->GetBuilder().FindNode(Edge.ToNodeID);
      const auto* From = FromNode ? FromNode->Interface.Outputs.FindByPredicate(
          [&](const auto& Pin) { return Pin.VertexID == Edge.FromVertexID; }) : nullptr;
      const auto* To = ToNode ? ToNode->Interface.Inputs.FindByPredicate(
          [&](const auto& Pin) { return Pin.VertexID == Edge.ToVertexID; }) : nullptr;
      if (!From || !To) {
        OutError = DocumentFailure(TEXT("metasound_existing_edge_pin_missing"),
            !From ? TEXT("An existing MetaSound connection refers to a missing source output pin.")
                  : TEXT("An existing MetaSound connection refers to a missing destination input pin."),
            TEXT("pin_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
            nullptr, FString(), !From ? *FromAlias : *ToAlias);
        OutError.InternalContext = !From ? Edge.FromVertexID.ToString() : Edge.ToVertexID.ToString();
        return false;
      }
      if (Connections.IsUnchanged(*FromAlias, From->Name.ToString(), *ToAlias, To->Name.ToString())) continue;
      const bool bStillRequested = Requested.IsUnchanged(*FromAlias, From->Name.ToString(), *ToAlias, To->Name.ToString());
      if (!Builder->GetBuilder().RemoveEdge(Edge)) {
        OutError = DocumentFailure(TEXT("metasound_remove_edge_failed"),
            TEXT("The existing MetaSound connection could not be removed before replaying the requested graph."),
            TEXT("edge_removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
            Document.Get(), TEXT("edges"), *FromAlias + TEXT(":") + From->Name.ToString() + TEXT(" -> ") + *ToAlias + TEXT(":") + To->Name.ToString());
        return false;
      }
      if (!bStillRequested) GraphIntent.MetaLinks.Add({Edge.FromNodeID, Edge.FromVertexID, Edge.ToNodeID, Edge.ToVertexID, false, TEXT("/edges")});
      ++OutStats.Edges;
    }
  }
  if (bHasMembers && !ApplyMetaSoundMembers(*Builder, MemberIntents, NodeHandles, OutStats, OutError)) return false;
  if (GraphForApply && !ApplyMetaSoundGraph(*Builder, GraphForApply, NodeHandles, NodePointers, EdgePointers, GraphIntent, OutStats,
      OutError, &Connections)) return false;
  // A member owns its native node(s) and incident edges on every page. Apply
  // the restored graph first, then let the native member owner remove that
  // dependency closure; untouched captured graph rows cannot recreate it.
  TSet<FGuid> RemovedMemberNodes;
  TMap<FGuid, TPair<FName, bool>> RemovedMemberSources;
  for (const auto& Removal : MemberRemovals) {
    const auto* Node = Removal.Value ? Builder->GetBuilder().FindGraphInputNode(Removal.Key)
                                    : Builder->GetBuilder().FindGraphOutputNode(Removal.Key);
    if (Node) {
      RemovedMemberNodes.Add(Node->GetID());
      RemovedMemberSources.Add(Node->GetID(), Removal);
    }
    const bool bRemoved = Removal.Value ? Builder->GetBuilder().RemoveGraphInput(Removal.Key)
                                       : Builder->GetBuilder().RemoveGraphOutput(Removal.Key);
    if (!bRemoved) {
      OutError = DocumentFailure(TEXT("metasound_remove_member_failed"),
          TEXT("A MetaSound member omitted from the complete collection could not be removed."),
          TEXT("member_removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
          Members ? Members->Get() : Document.Get(), Removal.Value ? TEXT("inputs") : TEXT("outputs"), Removal.Key.ToString());
      return false;
    }
    ++OutStats.Members;
  }
  Builder->InitNodeLocations();
  Builder->ConformObjectToDocument();
  if (UMetaSoundEditorSubsystem *Subsystem =
          GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
                  : nullptr) {
    Subsystem->RegisterGraphWithFrontend(Asset, true);
  }
  Asset.PostEditChange();
  Asset.MarkPackageDirty();
  // Compare the decoded request, not a second capture of the converted document.
  if (!VerifyMetaSoundMemberIntents(*Builder, bHasMembers ? *Members : nullptr, MemberIntents, bCompleteDocument, OutError)) {
    if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalAssetPath;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError);
    OutError.Reset();
  }
  for (const auto& Id : RemovedMemberNodes) {
    auto* NodeIntent = GraphIntent.MetaNodes.FindByPredicate([&](const auto& Item) { return Item.Id == Id; });
    if (!NodeIntent) NodeIntent = &GraphIntent.MetaNodes.AddDefaulted_GetRef();
    NodeIntent->Id = Id;
    NodeIntent->bPresent = false;
    const auto& Removal = RemovedMemberSources.FindChecked(Id);
    NodeIntent->PublicName = Removal.Key.ToString();
    NodeIntent->SourcePointer = Removal.Value ? TEXT("/members/inputs") : TEXT("/members/outputs");
  }
  GraphIntent.MetaDefaults.RemoveAll([&](const auto& Item) { return RemovedMemberNodes.Contains(Item.Node); });
  GraphIntent.MetaLinks.RemoveAll([&](const auto& Item) {
    return RemovedMemberNodes.Contains(Item.FromNode) || RemovedMemberNodes.Contains(Item.ToNode);
  });
  // Retain deleted member nodes in the complete edge scope: a native callback
  // must not resurrect an incident connection after the member owner removed it.
  if (!GraphIntent.Verify(Asset, OutError)) {
    if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalAssetPath;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError);
    OutError.Reset();
  }
  if (OutGraphIntent) *OutGraphIntent = MoveTemp(GraphIntent);
  return true;
}

// Resolve the common endpoint against the actual native pin interface. Existing
// endpoints carry name/occurrence; a newly created node carries the common alias.
bool ResolveSoundCuePin(USoundNode &Node, const FJsonObject &Endpoint,
                        EEdGraphPinDirection Direction, bool bResolveAlias, int32 &OutIndex,
                        UE::UAI::Contract::FDiagnostic& OutError) {
  FString Name;
  int32 Occurrence = 0;
  if (!UeAgentAuthoringJsonFields::ReadInt32Field(Endpoint, TEXT("pin_occurrence"), Occurrence, OutError)) return false;
  if (!Endpoint.TryGetStringField(TEXT("pin"), Name) ||
      Occurrence < 0 || !Node.GetGraphNode()) {
    FString Ignored;
    if (!Endpoint.TryGetStringField(TEXT("pin"), Ignored)) {
      OutError = DocumentFailure(TEXT("sound_cue_endpoint_pin_invalid"),
          TEXT("A SoundCue endpoint requires a string pin name or alias."), TEXT("pin"), Endpoint.TryGetField(TEXT("pin")),
          MakeShared<FJsonValueString>(TEXT("a string")), &Endpoint, TEXT("pin"), Node.GetName());
    } else if (Occurrence < 0) {
      OutError = DocumentFailure(TEXT("sound_cue_endpoint_pin_invalid"),
          TEXT("A SoundCue pin occurrence cannot be negative."), TEXT("pin_occurrence"),
          MakeShared<FJsonValueNumber>(Occurrence), MakeShared<FJsonValueString>(TEXT("an integer greater than or equal to zero")),
          &Endpoint, TEXT("pin_occurrence"), Node.GetName());
    } else {
      OutError = DocumentFailure(TEXT("sound_cue_endpoint_pin_invalid"),
          TEXT("The SoundCue node has no editor graph node from which to resolve pins."), TEXT("graph_node_available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Endpoint, TEXT("pin"), Node.GetName());
    }
    return false;
  }
  using namespace UE::UAI::HjsonWorkspace;
  TArray<FGraphAuthoringPinAliasInput> Pins;
  TMap<FString, int32> Occurrences;
  TMap<FString, int32> IndexBySourceId;
  int32 DirectionIndex = 0;
  OutIndex = INDEX_NONE;
  for (int32 Index = 0; Index < Node.GetGraphNode()->Pins.Num(); ++Index) {
    const UEdGraphPin *Pin = Node.GetGraphNode()->Pins[Index];
    if (!Pin) continue;
    const FString SourceId = FString::FromInt(Index);
    const FString Key = FString::FromInt(Pin->Direction) + TEXT(":") + Pin->PinName.ToString();
    const int32 PinOccurrence = Occurrences.FindOrAdd(Key)++;
    Pins.Add({SourceId, Pin->PinName.ToString(),
        PinOccurrence, Pin->Direction == EGPD_Input
            ? EGraphAuthoringEndpointDirection::Input
            : EGraphAuthoringEndpointDirection::Output});
    if (Pin->Direction != Direction) continue;
    IndexBySourceId.Add(SourceId, DirectionIndex);
    if (Pin->PinName.ToString() == Name && PinOccurrence == Occurrence)
      OutIndex = DirectionIndex;
    ++DirectionIndex;
  }
  const auto Unresolved = [&](const FGraphAuthoringPinAliasProjection* Aliases) {
    auto Requested = MakeShared<FJsonObject>();
    Requested->SetStringField(TEXT("pin"), Name);
    Requested->SetNumberField(TEXT("occurrence"), Occurrence);
    Requested->SetStringField(TEXT("direction"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    TArray<TSharedPtr<FJsonValue>> Choices;
    for (const auto& Pin : Pins) {
      if (!IndexBySourceId.Contains(Pin.SourceId)) continue;
      auto Choice = MakeShared<FJsonObject>();
      Choice->SetStringField(TEXT("pin"), Aliases ? Aliases->AliasBySourceId.FindRef(Pin.SourceId) : Pin.SemanticName);
      Choice->SetNumberField(TEXT("occurrence"), Aliases ? 0 : Pin.Occurrence);
      Choices.Add(MakeShared<FJsonValueObject>(Choice));
    }
    OutError = DocumentFailure(TEXT("sound_cue_endpoint_pin_unresolved"),
        TEXT("No SoundCue pin matches the requested name or alias, occurrence, and direction."),
        TEXT("pin"), MakeShared<FJsonValueObject>(Requested), MakeShared<FJsonValueArray>(MoveTemp(Choices)),
        &Endpoint, Aliases && Occurrence != 0 ? TEXT("pin_occurrence") : TEXT("pin"), Node.GetName());
    return false;
  };
  if (!bResolveAlias) {
    if (OutIndex != INDEX_NONE) return true;
    return Unresolved(nullptr);
  }
  FGraphAuthoringPinAliasProjection Aliases;
  FWorkspaceError Error;
  if (!TryBuildGraphAuthoringPinAliasProjection(Pins, Aliases, Error)) {
    OutError = Error.GetDiagnostic();
    return false;
  }
  if (Occurrence == 0) {
    const FString *Source = Aliases.SourceIdByAlias.Find(Name);
    const int32 *Index = Source ? IndexBySourceId.Find(*Source) : nullptr;
    if (Index) {
      OutIndex = *Index;
      return true;
    }
  }
  return Unresolved(&Aliases);
}

bool ApplySoundCueDocument(USoundCue &Cue,
                           const TSharedPtr<const FJsonObject> &Document,
                           FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                           const FString &CanonicalAssetPath,
                           TArray<FNodePropertyIntent>* OutPropertyIntents,
                           FAudioGraphIntent* OutGraphIntent) {
  using namespace UE::UAI::DomainAdapters::AudioMediaGraph;
  if (!ValidateFields(Document,
      {TEXT("graph"), TEXT("nodes"), TEXT("edges"), TEXT("connections")},
      TEXT("sound_cue_document"), OutError)) return false;
  const TSharedPtr<FJsonObject> *Graph = nullptr;
  const TSharedPtr<FJsonObject> *Nodes = nullptr;
  const TSharedPtr<FJsonObject> *Edges = nullptr;
  if (!Document.IsValid() ||
      !Document->TryGetObjectField(TEXT("graph"), Graph) || !Graph || !Graph->IsValid() ||
      !Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid() ||
      !Document->TryGetObjectField(TEXT("edges"), Edges) || !Edges || !Edges->IsValid()) {
    const FString Field = !Graph || !Graph->IsValid() ? TEXT("graph") : !Nodes || !Nodes->IsValid() ? TEXT("nodes") : TEXT("edges");
    OutError = DocumentFailure(TEXT("sound_cue_graph_shape_invalid"),
        TEXT("A SoundCue document requires graph, nodes, and edges objects."), Field,
        Document ? Document->TryGetField(Field) : nullptr, MakeShared<FJsonValueString>(TEXT("an object")), Document.Get(), Field);
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> CandidateNodes;
  TArray<FString> CandidateSources, DeleteSources;
  TMap<FString, FString> NodeInputPointers;
  TSet<FString> NewNodeIds;
  TMap<FString, TSharedPtr<FJsonObject>> DesiredById;
  TArray<FAudioMediaGraphSoundCueNodeMutation> Creates;
  TArray<FAudioMediaGraphSoundCueNodeMutation> Deletes;
  FAudioGraphIntent GraphIntent;
  GraphIntent.CompleteSoundNodes.Emplace();
  TFunction<void(UE::UAI::Contract::FDiagnostic&, const TArray<FString>&, const FString&)> LocateBatchError;
  LocateBatchError = [&](auto& Diagnostic, const auto& Sources, const FString& Prefix) {
    for (auto& Child : Diagnostic.Diagnostics) LocateBatchError(Child, Sources, Prefix);
    if (Diagnostic.PublicCause) {
      auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
      LocateBatchError(*Cause, Sources, Prefix); Diagnostic.PublicCause = MoveTemp(Cause);
    }
    if (!Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.SourceKind == TEXT("native_result")) return;
    const FString Pointer = Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.Source.JsonPath : Diagnostic.SemanticPointer;
    if (!Pointer.StartsWith(Prefix)) return;
    FString Ordinal = Pointer.Mid(Prefix.Len()), Suffix;
    int32 Slash = INDEX_NONE, Index = INDEX_NONE;
    if (Ordinal.FindChar(TEXT('/'), Slash)) { Suffix = Ordinal.Mid(Slash); Ordinal.LeftInline(Slash); }
    if (!LexTryParseString(Index, *Ordinal) || !Sources.IsValidIndex(Index)) return;
    const bool bCandidateField = Suffix == TEXT("/candidate") || Suffix.StartsWith(TEXT("/candidate/"));
    const FString AuthoredPointer = Sources[Index] + (bCandidateField ? Suffix : FString());
    UE::UAI::Contract::FSourceLocation Source;
    if (UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Document, FString(), Source, AuthoredPointer)) {
      Diagnostic.Source = MoveTemp(Source);
      Diagnostic.SemanticPointer = AuthoredPointer;
      Diagnostic.SourceKind = Diagnostic.Source.SourceFile.IsEmpty() ? TEXT("command_input")
          : bCandidateField ? TEXT("authored_value") : TEXT("related_input");
    }
  };
  const FString NodePrefix = Cue.GetPathName() + TEXT(":");
  for (const auto &Pair : (*Nodes)->Values) {
    const auto Item = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    FString Id, Operation;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty()) {
      OutError = DocumentFailure(TEXT("sound_cue_node_identity_missing"),
          !Item ? TEXT("Each named SoundCue node must be an object.") : TEXT("A SoundCue node requires a nonempty string id."),
          TEXT("id"), Item ? Item->TryGetField(TEXT("id")) : Pair.Value,
          MakeShared<FJsonValueString>(Item ? TEXT("a nonempty string") : TEXT("a node object")),
          Item ? Item.Get() : Nodes->Get(), Item ? TEXT("id") : Pair.Key, Pair.Key);
      return false;
    }
    Item->TryGetStringField(TEXT("operation"), Operation);
    if (Id == TEXT("sound_cue_root")) {
      auto* Output = FindSoundCueOutputNode(Cue);
      if (!Output) {
        OutError = DocumentFailure(TEXT("sound_cue_output_node_missing"),
            TEXT("The SoundCue has no native output node to receive the authored output settings."),
            TEXT("output_node_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Item.Get(), FString(), Pair.Key);
        return false;
      }
      if (Operation == TEXT("remove") && !Output->CanUserDeleteNode()) {
        OutError = DocumentFailure(TEXT("sound_cue_output_node_remove_forbidden"),
            TEXT("The SoundCue output node does not permit user deletion."), TEXT("operation"),
            MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(TEXT("retain the output node")), Item.Get(), TEXT("operation"), Pair.Key);
        return false;
      }
      const TSharedPtr<FJsonObject>* Position = nullptr;
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Item,
          {{TEXT("pos"), EJson::Object}}, OutError, Pair.Key)) return false;
      if (Item->TryGetObjectField(TEXT("pos"), Position)) {
        FIntPoint NativePosition(Output->NodePosX, Output->NodePosY);
        if (!UeAgentAuthoringJsonFields::ReadInt32Field(**Position, TEXT("x"), NativePosition.X, OutError)
            || !UeAgentAuthoringJsonFields::ReadInt32Field(**Position, TEXT("y"), NativePosition.Y, OutError)) return false;
        GraphIntent.SoundOutputPosition = NativePosition;
        GraphIntent.SoundOutputSourcePointer = TEXT("/nodes/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
      }
      continue;
    }
    NodeInputPointers.Add(Id, TEXT("/nodes/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
    if (Operation == TEXT("remove")) continue;
    if (DesiredById.Contains(Id)) {
      OutError = DocumentFailure(TEXT("sound_cue_node_identity_duplicate"),
          TEXT("Two SoundCue node entries use the same id."), TEXT("id"),
          MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(TEXT("a unique node id")), Item.Get(), TEXT("id"), Pair.Key);
      return false;
    }
    DesiredById.Add(Id, Item);
    GraphIntent.CompleteSoundNodes->Add(*Id);
    auto& NodeIntent = GraphIntent.SoundNodes.AddDefaulted_GetRef();
    NodeIntent.Name = *Id;
    NodeIntent.SourcePointer = TEXT("/nodes/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    USoundNode *Existing = FindObject<USoundNode>(&Cue, *Id);
    if (Existing && Cue.AllNodes.Contains(Existing)) {
      if (Item->HasField(TEXT("candidate"))) {
        OutError = DocumentFailure(TEXT("sound_cue_existing_node_candidate_unsupported"),
            TEXT("An existing SoundCue node cannot be reselected through a creation candidate."), TEXT("candidate"),
            Item->TryGetField(TEXT("candidate")), nullptr, Item.Get(), TEXT("candidate"), Pair.Key);
        return false;
      }
      FString ClassPath;
      if (Item->TryGetStringField(TEXT("class_path"), ClassPath) &&
          ClassPath != Existing->GetClass()->GetPathName()) {
        OutError = DocumentFailure(TEXT("sound_cue_node_class_immutable"),
            TEXT("The class of an existing SoundCue node cannot be changed in place."), TEXT("class_path"),
            MakeShared<FJsonValueString>(ClassPath), MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()),
            Item.Get(), TEXT("class_path"), Pair.Key);
        return false;
      }
      NodeIntent.ClassPath = ClassPath.IsEmpty() ? Existing->GetClass()->GetPathName() : ClassPath;
      continue;
    }
    if (!Item->HasField(TEXT("candidate"))) {
      OutError = DocumentFailure(TEXT("sound_cue_candidate_required"),
          TEXT("Creating a SoundCue node requires a readable candidate selector."), TEXT("candidate"), nullptr,
          MakeShared<FJsonValueString>(TEXT("a candidate selector")), Item.Get(), TEXT("candidate"), Pair.Key);
      return false;
    }
    auto CandidateNode = MakeShared<FJsonObject>();
    CandidateNode->SetStringField(TEXT("id"), Id);
    CandidateNode->SetStringField(TEXT("intent"), TEXT("create"));
    CandidateNode->SetField(TEXT("candidate"), Item->TryGetField(TEXT("candidate")));
    CandidateNodes.Add(MakeShared<FJsonValueObject>(CandidateNode));
    CandidateSources.Add(NodeIntent.SourcePointer);
    NewNodeIds.Add(Id);
  }
  // Candidate selection stays in the same owner as typed graph operations.
  if (!CandidateNodes.IsEmpty()) {
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("nodes"), CandidateNodes);
    UE::UAI::HjsonWorkspace::FWorkspaceError Error;
    const FString Target = CanonicalAssetPath.IsEmpty() ? Cue.GetPathName() : CanonicalAssetPath;
    if (!UE::UAI::DomainAdapters::AudioGraph::TryNormalizeAudioGraphCandidateBindings(Target, Params, Error)) {
      OutError = Error.GetDiagnostic();
      LocateBatchError(OutError, CandidateSources, TEXT("/root/operation/params/nodes/"));
      return false;
    }
    for (const auto &Value : CandidateNodes) {
      const auto Item = Value->AsObject();
      Creates.Emplace(NodePrefix + Item->GetStringField(TEXT("id")),
          Item->GetStringField(TEXT("native_class_ref")), EAudioMediaGraphMutationIntent::Create);
      GraphIntent.SoundNodes.FindByPredicate([&](const auto& Intent) {
        return Intent.Name == FName(*Item->GetStringField(TEXT("id")));
      })->ClassPath = Item->GetStringField(TEXT("native_class_ref"));
    }
  }
  for (USoundNode *Node : Cue.AllNodes)
    if (Node && !DesiredById.Contains(Node->GetName())) {
      Deletes.Emplace(NodePrefix + Node->GetName(), FString(), EAudioMediaGraphMutationIntent::Delete);
      const auto* Source = NodeInputPointers.Find(Node->GetName());
      DeleteSources.Add(Source ? *Source : TEXT("/nodes"));
    }

  Cue.Modify();
  if (!Cue.GetGraph()) Cue.CreateGraph();
  FAudioMediaGraphSoundCueNativeHook Hook;
  FAudioMediaGraphError NativeError;
  if (!Creates.IsEmpty() && !Hook.TryApplyNodeMutations(Cue, Creates, NativeError)) {
    OutError = NativeError;
    LocateBatchError(OutError, CandidateSources, TEXT("/nodes/"));
    return false;
  }
  for (const auto &Create : Creates) {
    USoundNode *Node = FindObject<USoundNode>(&Cue, *Create.GetObjectPath().RightChop(NodePrefix.Len()));
    if (!Node || !Node->GetGraphNode()) {
      const FString Id = Create.GetObjectPath().RightChop(NodePrefix.Len());
      const auto Source = DesiredById.FindRef(Id);
      OutError = DocumentFailure(TEXT("sound_cue_created_node_missing"),
          !Node ? TEXT("The SoundCue node creation did not produce the requested native node.")
                : TEXT("The created SoundCue node has no editor graph node."),
          !Node ? TEXT("node_present") : TEXT("graph_node_present"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Source.Get(), TEXT("candidate"), Id);
      return false;
    }
    Node->CreateStartingConnectors();
    Node->GetGraphNode()->ReconstructNode();
  }
  TMap<FString, USoundNode *> NativeById;
  TMap<USoundNode *, TArray<USoundNode *>> DesiredChildren;
  TArray<FNodePropertyIntent> PropertyIntents;
  for (const auto &Pair : DesiredById) {
    USoundNode *Node = FindObject<USoundNode>(&Cue, *Pair.Key);
    if (!Node || !Node->GetGraphNode()) {
      OutError = DocumentFailure(TEXT("sound_cue_node_readback_missing"),
          !Node ? TEXT("The requested SoundCue node is missing during apply readback.")
                : TEXT("The SoundCue node has no editor graph node during apply readback."),
          !Node ? TEXT("node_present") : TEXT("graph_node_present"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Pair.Value.Get(), TEXT("id"), Pair.Key);
      return false;
    }
    NativeById.Add(Pair.Key, Node);
    int32 InputCount = 0;
    for (const UEdGraphPin *Pin : Node->GetGraphNode()->Pins)
      if (Pin && Pin->Direction == EGPD_Input) ++InputCount;
    DesiredChildren.Add(Node).SetNumZeroed(InputCount);
  }
  TMap<USoundNode*, TMap<USoundNode*, const FJsonObject*>> ConnectionSources;
  for (const auto &Pair : (*Edges)->Values) {
    const auto Edge = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    FString Operation;
    if (Edge.IsValid() && Edge->TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("remove")) continue;
    const TSharedPtr<FJsonObject> *From = nullptr, *To = nullptr;
    FString ChildId, ParentId;
    if (!Edge.IsValid() || !Edge->TryGetObjectField(TEXT("from"), From) || !From || !From->IsValid() ||
        !Edge->TryGetObjectField(TEXT("to"), To) || !To || !To->IsValid() ||
        !(*From)->TryGetStringField(TEXT("node"), ChildId) || !(*To)->TryGetStringField(TEXT("node"), ParentId)) {
      FString Ignored;
      const FJsonObject* Source = !Edge ? Edges->Get() : !From || !From->IsValid() || !To || !To->IsValid()
          ? Edge.Get() : !(*From)->TryGetStringField(TEXT("node"), Ignored) ? From->Get() : To->Get();
      const FString Field = !Edge ? Pair.Key : !From || !From->IsValid() ? TEXT("from")
          : !To || !To->IsValid() ? TEXT("to") : TEXT("node");
      OutError = DocumentFailure(TEXT("sound_cue_edge_invalid"),
          TEXT("A SoundCue edge requires from/to objects with string node identities."), Field,
          Source->TryGetField(Field), MakeShared<FJsonValueString>(Field == TEXT("node") ? TEXT("a string") : TEXT("an object")), Source, Field, Pair.Key);
      return false;
    }
    USoundNode *Child = NativeById.FindRef(ChildId), *Parent = NativeById.FindRef(ParentId);
    int32 OutputIndex = INDEX_NONE, InputIndex = INDEX_NONE;
    if (!Child || !Parent) {
      OutError = DocumentFailure(TEXT("sound_cue_edge_node_unresolved"),
          !Child ? TEXT("The SoundCue edge's source node does not exist in the requested graph.")
                 : TEXT("The SoundCue edge's destination node does not exist in the requested graph."),
          TEXT("node"), MakeShared<FJsonValueString>(!Child ? ChildId : ParentId),
          MakeShared<FJsonValueString>(TEXT("an existing node id")), !Child ? From->Get() : To->Get(), TEXT("node"), Pair.Key);
      return false;
    }
    if (!ResolveSoundCuePin(*Child, **From, EGPD_Output, NewNodeIds.Contains(ChildId), OutputIndex, OutError) ||
        !ResolveSoundCuePin(*Parent, **To, EGPD_Input, NewNodeIds.Contains(ParentId), InputIndex, OutError)) return false;
    auto &Children = DesiredChildren.FindChecked(Parent);
    if (!Children.IsValidIndex(InputIndex) || Children[InputIndex]) {
      if (!Children.IsValidIndex(InputIndex)) {
        OutError = DocumentFailure(TEXT("sound_cue_input_connection_conflict"),
            TEXT("The resolved SoundCue input slot is outside the node's available input slots."), TEXT("input_index"),
            MakeShared<FJsonValueNumber>(InputIndex), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Children.Num())),
            To->Get(), TEXT("pin"), ParentId);
      } else {
        OutError = DocumentFailure(TEXT("sound_cue_input_connection_conflict"),
            TEXT("Two SoundCue edges attempt to occupy the same input slot."), TEXT("connected_child"),
            MakeShared<FJsonValueString>(Children[InputIndex]->GetName()), nullptr, To->Get(), TEXT("pin"), ParentId);
        OutError.ProposedValue = MakeShared<FJsonValueString>(ChildId);
      }
      return false;
    }
    Children[InputIndex] = Child;
    ConnectionSources.FindOrAdd(Parent).Add(Child, Edge.Get());
  }
  TSet<USoundNode *> Visiting, Visited;
  TArray<USoundNode*> VisitStack;
  TArray<TSharedPtr<FJsonValue>> Cycle;
  const FJsonObject* CycleSource = nullptr;
  TFunction<bool(USoundNode *)> Visit = [&](USoundNode *Node) {
    if (Visited.Contains(Node)) return true;
    if (Visiting.Contains(Node)) {
      for (int32 Index = VisitStack.IndexOfByKey(Node); Index < VisitStack.Num(); ++Index)
        Cycle.Add(MakeShared<FJsonValueString>(VisitStack[Index]->GetName()));
      Cycle.Add(MakeShared<FJsonValueString>(Node->GetName()));
      if (!VisitStack.IsEmpty())
        if (const auto* Sources = ConnectionSources.Find(VisitStack.Last())) CycleSource = Sources->FindRef(Node);
      return false;
    }
    Visiting.Add(Node);
    VisitStack.Add(Node);
    for (USoundNode *Child : DesiredChildren.FindChecked(Node))
      if (Child && !Visit(Child)) return false;
    Visiting.Remove(Node);
    VisitStack.Pop();
    Visited.Add(Node);
    return true;
  };
  for (const auto &Pair : DesiredChildren) {
    if (!Visit(Pair.Key)) {
      OutError = DocumentFailure(TEXT("sound_cue_connection_cycle"),
          TEXT("The requested SoundCue connections form a cycle."), TEXT("connection_cycle"),
          MakeShared<FJsonValueArray>(MoveTemp(Cycle)), MakeShared<FJsonValueString>(TEXT("an acyclic graph")),
          CycleSource ? CycleSource : Edges->Get());
      return false;
    }
  }
  // Pin slots differ from the typed ordered-child list: null/unconnected slots
  // must not collapse or move an untouched connection to another input.
  for (auto &Pair : DesiredChildren) {
    auto& Expected = GraphIntent.CompleteSoundChildren.Add(Pair.Key->GetFName());
    for (const auto* Child : Pair.Value) Expected.Add(Child ? Child->GetFName() : NAME_None);
    bool bSameChildren = Pair.Key->ChildNodes.Num() == Pair.Value.Num();
    for (int32 Index = 0; bSameChildren && Index < Pair.Value.Num(); ++Index)
      bSameChildren = Pair.Key->ChildNodes[Index] == Pair.Value[Index];
    if (!bSameChildren) {
      Pair.Key->Modify();
      Pair.Key->SetChildNodes(Pair.Value);
      ++OutStats.Edges;
    }
  }
  for (const auto &Delete : Deletes) {
    if (USoundNode *Node = FindObject<USoundNode>(&Cue, *Delete.GetObjectPath().RightChop(NodePrefix.Len())))
      if (UEdGraphNode *GraphNode = Node->GetGraphNode()) GraphNode->DestroyNode();
  }
  if (!Deletes.IsEmpty() && !Hook.TryApplyNodeMutations(Cue, Deletes, NativeError)) {
    OutError = NativeError;
    LocateBatchError(OutError, DeleteSources, TEXT("/nodes/"));
    return false;
  }
  OutStats.Nodes += Creates.Num() + Deletes.Num();
  const TSharedPtr<FJsonObject> *References = nullptr;
  if ((*Graph)->HasField(TEXT("node_references"))) {
    if (!(*Graph)->TryGetObjectField(TEXT("node_references"), References) || !References || !References->IsValid()) {
      OutError = DocumentFailure(TEXT("sound_cue_node_references_invalid"),
          TEXT("SoundCue node_references must be an object."), TEXT("node_references"),
          (*Graph)->TryGetField(TEXT("node_references")), MakeShared<FJsonValueString>(TEXT("an object")), Graph->Get(), TEXT("node_references"));
      return false;
    }
    if (!ValidateFields(*References, {TEXT("root")}, TEXT("sound_cue_node_references"), OutError)) return false;
    FString RootId;
    if (!(*References)->TryGetStringField(TEXT("root"), RootId) ||
        (!RootId.IsEmpty() && !NativeById.Contains(RootId))) {
      OutError = DocumentFailure(TEXT("sound_cue_root_node_unresolved"),
          TEXT("The SoundCue root must be an empty string or the id of a node in the requested graph."), TEXT("root"),
          (*References)->TryGetField(TEXT("root")), MakeShared<FJsonValueString>(TEXT("an existing node id or an empty string")), References->Get(), TEXT("root"));
      return false;
    }
    Cue.FirstNode = RootId.IsEmpty() ? nullptr : NativeById.FindRef(RootId);
    GraphIntent.SoundRoot = FName(*RootId);
  }
  for (const auto &Pair : DesiredById) {
    USoundNode *Node = NativeById.FindChecked(Pair.Key);
    if (!ApplyEditableProperties(*Node, Pair.Value, OutStats, OutError, PropertyIntents)) return false;
    const TSharedPtr<FJsonObject> *Position = nullptr;
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Pair.Value,
        {{TEXT("pos"), EJson::Object}}, OutError, Pair.Key)) return false;
    if (Pair.Value->TryGetObjectField(TEXT("pos"), Position) && Position && Position->IsValid()) {
      UEdGraphNode *GraphNode = Node->GetGraphNode();
      int32 NativeX = GraphNode->NodePosX, NativeY = GraphNode->NodePosY;
      if (!UeAgentAuthoringJsonFields::ReadInt32Field(**Position, TEXT("x"), NativeX, OutError)
          || !UeAgentAuthoringJsonFields::ReadInt32Field(**Position, TEXT("y"), NativeY, OutError)) {
        return false;
      }
      GraphNode->Modify();
      GraphNode->NodePosX = NativeX;
      GraphNode->NodePosY = NativeY;
      GraphIntent.SoundNodes.FindByPredicate([&](const auto& Intent) { return Intent.Name == Node->GetFName(); })->Position = FIntPoint(NativeX, NativeY);
    }
  }
  if (GraphIntent.SoundOutputPosition.IsSet()) {
    auto* Output = FindSoundCueOutputNode(Cue);
    Output->Modify();
    Output->NodePosX = GraphIntent.SoundOutputPosition->X;
    Output->NodePosY = GraphIntent.SoundOutputPosition->Y;
  }
  Cue.LinkGraphNodesFromSoundNodes();
  Cue.PostEditChange();
  Cue.MarkPackageDirty();
  if (!VerifyNodePropertyIntents(Cue, PropertyIntents, OutError) || !GraphIntent.Verify(Cue, OutError)) {
    if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalAssetPath;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError);
    OutError.Reset();
  }
  if (OutPropertyIntents) OutPropertyIntents->Append(MoveTemp(PropertyIntents));
  if (OutGraphIntent) *OutGraphIntent = MoveTemp(GraphIntent);
  return true;
}
bool ApplySoundSubmixDocument(USoundSubmixBase &Submix,
                              const TSharedPtr<const FJsonObject> &Document,
                              FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!ValidateFields(Document, {TEXT("settings"), TEXT("children")},
                      TEXT("sound_submix_document"), OutError))
    return false;
  const TSharedPtr<FJsonObject> Current = BuildSoundSubmixDocument(Submix);
  if (!RequireCurrentValue(Document, TEXT("children"), Current, OutError))
    return false;
  const TSharedPtr<FJsonObject> *Settings = nullptr;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("settings"), EJson::Object}}, OutError)) return false;
  if (!Document.IsValid() ||
      !Document->TryGetObjectField(TEXT("settings"), Settings) || !Settings ||
      !Settings->IsValid())
    return true;
  FString RequestedClass;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**Settings,
      {{TEXT("submix_class"), EJson::String}, {TEXT("parent"), EJson::String, true},
       {TEXT("effect_chain"), EJson::Array}}, OutError, TEXT("/settings"))) return false;
  if ((*Settings)->TryGetStringField(TEXT("submix_class"), RequestedClass) &&
      !RequestedClass.Equals(Submix.GetClass()->GetPathName())) {
    OutError = DocumentFailure(TEXT("sound_submix_class_immutable"),
        TEXT("The class of an existing SoundSubmix cannot be changed in place."), TEXT("submix_class"),
        MakeShared<FJsonValueString>(RequestedClass), MakeShared<FJsonValueString>(Submix.GetClass()->GetPathName()),
        Settings->Get(), TEXT("submix_class"));
    return false;
  }
  FString ParentPath;
  if ((*Settings)->HasField(TEXT("parent"))) {
    if (!UeAgentAuthoringJsonFields::ReadReferenceField(**Settings, TEXT("parent"), ParentPath, OutError)) return false;
    USoundSubmixWithParentBase *WithParent =
        Cast<USoundSubmixWithParentBase>(&Submix);
    if (!WithParent) {
      OutError = DocumentFailure(TEXT("parent_submix_not_applicable"),
          TEXT("This SoundSubmix class does not support a parent submix."), TEXT("submix_class"),
          MakeShared<FJsonValueString>(Submix.GetClass()->GetPathName()),
          MakeShared<FJsonValueString>(USoundSubmixWithParentBase::StaticClass()->GetPathName()), Settings->Get(), TEXT("parent"));
      return false;
    }
    USoundSubmixBase *Parent =
        ParentPath.IsEmpty()
            ? nullptr
            : Cast<USoundSubmixBase>(StaticLoadObject(
                  USoundSubmixBase::StaticClass(), nullptr, *ParentPath));
    if (!ParentPath.IsEmpty() && !Parent) {
      OutError = DocumentFailure(TEXT("parent_submix_not_found"),
          TEXT("The requested parent path could not be loaded as a SoundSubmix asset."), TEXT("parent"),
          MakeShared<FJsonValueString>(ParentPath), MakeShared<FJsonValueString>(TEXT("an existing SoundSubmix asset path")),
          Settings->Get(), TEXT("parent"));
      return false;
    }
    if (WithParent->ParentSubmix != Parent) {
      WithParent->SetParentSubmix(Parent, true);
      ++OutStats.References;
    }
  }
  const TArray<TSharedPtr<FJsonValue>> *Chain = nullptr;
  if ((*Settings)->TryGetArrayField(TEXT("effect_chain"), Chain) && Chain) {
    USoundSubmix *Concrete = Cast<USoundSubmix>(&Submix);
    if (!Concrete && !Chain->IsEmpty()) {
      OutError = DocumentFailure(TEXT("submix_effect_chain_not_applicable"),
          TEXT("This SoundSubmix class cannot accept a nonempty effect chain."), TEXT("submix_class"),
          MakeShared<FJsonValueString>(Submix.GetClass()->GetPathName()), MakeShared<FJsonValueString>(USoundSubmix::StaticClass()->GetPathName()),
          Settings->Get(), TEXT("effect_chain"));
      return false;
    }
    if (Concrete) {
      TArray<TObjectPtr<USoundEffectSubmixPreset>> Desired;
      for (int32 EffectIndex = 0; EffectIndex < Chain->Num(); ++EffectIndex) {
        const TSharedPtr<FJsonValue>& Value = (*Chain)[EffectIndex];
        const FString Path = Value.IsValid() ? Value->AsString() : FString();
        USoundEffectSubmixPreset *Preset =
            Cast<USoundEffectSubmixPreset>(StaticLoadObject(
                USoundEffectSubmixPreset::StaticClass(), nullptr, *Path));
        if (!Preset) {
          OutError = DocumentFailure(TEXT("submix_effect_preset_not_found"),
              TEXT("The effect-chain entry could not be loaded as a SoundEffectSubmixPreset asset."), TEXT("effect_chain"),
              Value, MakeShared<FJsonValueString>(TEXT("an existing SoundEffectSubmixPreset asset path")), Settings->Get(), FString(), FString(),
              FString::Printf(TEXT("/effect_chain/%d"), EffectIndex));
          return false;
        }
        Desired.Add(Preset);
      }
      if (Concrete->SubmixEffectChain != Desired) {
        Concrete->Modify();
        Concrete->SubmixEffectChain = MoveTemp(Desired);
        OutStats.References += Concrete->SubmixEffectChain.Num();
      }
    }
  }
  Submix.PostEditChange();
  Submix.MarkPackageDirty();
  return true;
}
} // namespace

FString NormalizePackagePath(const FString &InPath) {
  FString Result = InPath;
  Result.TrimStartAndEndInline();
  if (Result.Contains(TEXT(".")))
    Result = FPackageName::ObjectPathToPackageName(Result);
  while (Result.EndsWith(TEXT("/")))
    Result.LeftChopInline(1);
  return Result;
}

FString ToObjectPath(const FString &PackagePath) {
  const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
  return Name.IsEmpty() ? FString() : PackagePath + TEXT(".") + Name;
}

UObject *LoadAsset(const EAssetKind Kind, const FString &InPath) {
  const FString ObjectPath = ToObjectPath(NormalizePackagePath(InPath));
  UObject *Asset =
      ObjectPath.IsEmpty()
          ? nullptr
          : StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
  if (!Asset)
    return nullptr;
  switch (Kind) {
  case EAssetKind::MetaSound:
    return Asset->IsA<UMetaSoundSource>() || Asset->IsA<UMetaSoundPatch>()
               ? Asset
               : nullptr;
  case EAssetKind::SoundCue:
    return Asset->IsA<USoundCue>() ? Asset : nullptr;
  case EAssetKind::SoundSubmix:
    return Asset->IsA<USoundSubmixBase>() ? Asset : nullptr;
  default:
    return nullptr;
  }
}

UObject *CreateFactorySeed(const EAssetKind Kind, UObject *Outer, FName Name,
                           EObjectFlags Flags,
                           const TSharedPtr<const FJsonObject> &SeedDocument,
                           UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Outer) {
    OutError = DocumentFailure(TEXT("audio_seed_outer_missing"),
        TEXT("An audio asset seed cannot be created without an owning object or package."), TEXT("outer_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return nullptr;
  }
  if (Name.IsNone())
    Name = MakeUniqueObjectName(Outer, UObject::StaticClass(), TEXT("Seed"));
  if (SeedDocument && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*SeedDocument,
      {{TEXT("settings"), EJson::Object}}, OutError)) return nullptr;
  if (Kind == EAssetKind::SoundCue) {
    auto* Seed = NewObject<USoundCue>(Outer, Name, Flags);
    if (!Seed) OutError = DocumentFailure(TEXT("audio_factory_seed_failed"),
        TEXT("Unreal did not create the SoundCue asset seed."), TEXT("asset_created"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), nullptr, FString(), TEXT("SoundCue"));
    return Seed;
  }
  if (Kind == EAssetKind::SoundSubmix) {
    UClass *Class = USoundSubmix::StaticClass();
    const TSharedPtr<FJsonObject> *Settings = nullptr;
    FString ClassPath;
    if (SeedDocument && SeedDocument->TryGetObjectField(TEXT("settings"), Settings)
        && !UeAgentAuthoringJsonFields::ValidateOptionalFields(**Settings,
            {{TEXT("submix_class"), EJson::String}}, OutError, TEXT("/settings"))) return nullptr;
    if (SeedDocument.IsValid() &&
        SeedDocument->TryGetObjectField(TEXT("settings"), Settings) &&
        Settings && Settings->IsValid() &&
        (*Settings)->TryGetStringField(TEXT("submix_class"), ClassPath) &&
        !ClassPath.IsEmpty()) {
      Class = LoadObject<UClass>(nullptr, *ClassPath);
    }
    if (!Class || !Class->IsChildOf(USoundSubmixBase::StaticClass())) {
      OutError = DocumentFailure(TEXT("invalid_submix_class"),
          !Class ? TEXT("The requested submix class could not be loaded.") : TEXT("The requested class does not derive from SoundSubmixBase."),
          TEXT("submix_class"), MakeShared<FJsonValueString>(ClassPath),
          MakeShared<FJsonValueString>(USoundSubmixBase::StaticClass()->GetPathName()), Settings ? Settings->Get() : SeedDocument.Get(), TEXT("submix_class"));
      return nullptr;
    }
    auto* Seed = NewObject<UObject>(Outer, Class, Name, Flags);
    if (!Seed) OutError = DocumentFailure(TEXT("audio_factory_seed_failed"),
        TEXT("Unreal did not create an asset seed of the selected SoundSubmix class."), TEXT("asset_created"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), nullptr, FString(), Class->GetPathName());
    return Seed;
  }
  if (Kind == EAssetKind::MetaSound) {
    FString AssetType = TEXT("source");
    const TSharedPtr<FJsonObject> *Settings = nullptr;
    if (SeedDocument.IsValid() &&
        SeedDocument->TryGetObjectField(TEXT("settings"), Settings) &&
        Settings && Settings->IsValid()) {
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**Settings,
          {{TEXT("asset_type"), EJson::String}}, OutError, TEXT("/settings"))) return nullptr;
      (*Settings)->TryGetStringField(TEXT("asset_type"), AssetType);
    }
    if (!AssetType.Equals(TEXT("source"), ESearchCase::IgnoreCase)
        && !AssetType.Equals(TEXT("patch"), ESearchCase::IgnoreCase)) {
      OutError = DocumentFailure(TEXT("metasound_asset_type_invalid"),
          TEXT("A MetaSound asset type must be source or patch."), TEXT("asset_type"), MakeShared<FJsonValueString>(AssetType),
          MakeShared<FJsonValueString>(TEXT("source or patch")), Settings ? Settings->Get() : SeedDocument.Get(), TEXT("asset_type"));
      return nullptr;
    }
    const bool bPatch =
        AssetType.Equals(TEXT("patch"), ESearchCase::IgnoreCase);
    UClass *Class = bPatch ? UMetaSoundPatch::StaticClass()
                           : UMetaSoundSource::StaticClass();
    UFactory *Factory =
        bPatch ? static_cast<UFactory *>(NewObject<UMetaSoundFactory>())
               : static_cast<UFactory *>(NewObject<UMetaSoundSourceFactory>());
    UObject *Asset = Factory ? Factory->FactoryCreateNew(Class, Outer, Name,
                                                         Flags, nullptr, GWarn)
                             : nullptr;
    if (!Asset)
      OutError = DocumentFailure(TEXT("metasound_factory_seed_failed"),
          !Factory ? TEXT("The MetaSound asset factory could not be instantiated.") : TEXT("The MetaSound factory did not return an asset."),
          !Factory ? TEXT("factory_available") : TEXT("asset_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
          nullptr, FString(), AssetType);
    return Asset;
  }
  OutError = DocumentFailure(TEXT("audio_asset_kind_unsupported"),
      TEXT("No audio asset factory supports the requested asset kind."), TEXT("asset_kind"),
      MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), MakeShared<FJsonValueString>(TEXT("MetaSound, SoundCue, or SoundSubmix")));
  return nullptr;
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath) {
  OutDocument.Reset();
  OutError.Reset();
  ON_SCOPE_EXIT {
    if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalAssetPath;
  };
  switch (Kind) {
  case EAssetKind::MetaSound:
    OutDocument = BuildMetaSoundDocument(Asset, OutError);
    break;
  case EAssetKind::SoundCue:
    if (USoundCue *Cue = Cast<USoundCue>(&Asset))
      OutDocument = BuildSoundCueDocument(*Cue, OutError);
    else OutError = DocumentFailure(TEXT("audio_asset_class_mismatch"),
        TEXT("The asset selected for SoundCue export is not a SoundCue."), TEXT("asset_class"),
        MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(USoundCue::StaticClass()->GetPathName()));
    break;
  case EAssetKind::SoundSubmix:
    if (USoundSubmixBase *Submix = Cast<USoundSubmixBase>(&Asset))
      OutDocument = BuildSoundSubmixDocument(*Submix);
    else OutError = DocumentFailure(TEXT("audio_asset_class_mismatch"),
        TEXT("The asset selected for SoundSubmix export is not a SoundSubmixBase."), TEXT("asset_class"),
        MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(USoundSubmixBase::StaticClass()->GetPathName()));
    break;
  default:
    OutError = DocumentFailure(TEXT("audio_asset_class_mismatch"),
        TEXT("The requested audio asset kind has no document exporter."), TEXT("asset_kind"),
        MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), MakeShared<FJsonValueString>(TEXT("MetaSound, SoundCue, or SoundSubmix")));
    break;
  }
  return OutDocument.IsValid();
}

TSharedPtr<FJsonObject> EncodeMetaSoundLiteral(const FMetasoundFrontendLiteral& Literal) {
  return EncodeLiteral(Literal);
}

bool DecodeMetaSoundLiteral(const TSharedPtr<const FJsonObject>& Value,
    FMetasoundFrontendLiteral& OutLiteral, UE::UAI::Contract::FDiagnostic& OutError) {
  return DecodeLiteral(Value, OutLiteral, OutError);
}

bool VerifyNodePropertyIntents(UObject& Asset,
    const TArray<FNodePropertyIntent>& Intents, UE::UAI::Contract::FDiagnostic& OutError) {
  for (const FNodePropertyIntent& Intent : Intents) {
    USoundCue* Cue = Cast<USoundCue>(&Asset);
    USoundNode* Node = Cue ? FindObject<USoundNode>(Cue, *Intent.NodeName.ToString()) : nullptr;
    if (!Node || !Cue->AllNodes.Contains(Node)) {
      OutError = DocumentFailure(TEXT("audio_property_intent_owner_missing"),
          !Cue ? TEXT("The asset is no longer a SoundCue while verifying node properties.")
               : !Node ? TEXT("The node owning the requested properties no longer exists.")
                       : TEXT("The node owning the requested properties is no longer in the SoundCue node collection."),
          TEXT("property_owner_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
          Intent.Source.Get(), TEXT("properties"), Intent.NodeName.ToString());
      return false;
    }
    UeAgentAuthoringObjectApply::FApplyResult Result;
    if (!Intent.Properties.VerifyObject(Node, Result)) {
      OutError = Result.Diagnostic;
      return false;
    }
  }
  return true;
}

bool VerifyDocumentIntent(EAssetKind Kind, UObject& Asset,
    const TSharedPtr<const FJsonObject>& Document, bool bCompleteDocument, UE::UAI::Contract::FDiagnostic& OutError) {
  if (Kind != EAssetKind::MetaSound || !Document) return true;
  const TSharedPtr<FJsonObject>* Members = nullptr;
  if (!Document->TryGetObjectField(TEXT("members"), Members)) return true;
  TArray<FMetaSoundMemberIntent> Intents;
  if (!ReadMetaSoundMemberIntents(*Members, Intents, OutError)) return false;
  UMetaSoundBuilderBase* Builder = nullptr;
  if (!GetMetaSoundBuilder(Asset, Builder, OutError)) return false;
  FMetaSoundDefaultPageScope PageScope(Builder);
  if (!PageScope.IsReady()) {
    OutError = DocumentFailure(TEXT("metasound_default_graph_missing"),
        TEXT("The MetaSound builder could not select the asset's default graph page for verification."),
        TEXT("default_page_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  return VerifyMetaSoundMemberIntents(*Builder, *Members, Intents, bCompleteDocument, OutError);
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath,
                   TArray<FNodePropertyIntent>* OutPropertyIntents,
                   bool bCompleteDocument, FAudioGraphIntent* OutGraphIntent) {
  OutStats = FMutationStats();
  OutError.Reset();
  ON_SCOPE_EXIT {
    if (!OutError.IsEmpty()) {
      if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalAssetPath;
      if (Document) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
    }
  };
  if (OutPropertyIntents) OutPropertyIntents->Reset();
  if (OutGraphIntent) *OutGraphIntent = FAudioGraphIntent();
  if (!Document.IsValid())
    return true;
  switch (Kind) {
  case EAssetKind::MetaSound:
    return ApplyMetaSoundDocument(Asset, Document, OutStats, OutError, bCompleteDocument, OutGraphIntent, CanonicalAssetPath);
  case EAssetKind::SoundCue:
    if (USoundCue *Cue = Cast<USoundCue>(&Asset))
      return ApplySoundCueDocument(*Cue, Document, OutStats, OutError, CanonicalAssetPath, OutPropertyIntents, OutGraphIntent);
    break;
  case EAssetKind::SoundSubmix:
    if (USoundSubmixBase *Submix = Cast<USoundSubmixBase>(&Asset))
      return ApplySoundSubmixDocument(*Submix, Document, OutStats, OutError);
    break;
  default:
    break;
  }
  OutError = DocumentFailure(TEXT("audio_asset_class_mismatch"),
      TEXT("The asset class cannot be edited as the requested audio asset kind."), TEXT("asset_class"),
      MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(AssetKindToString(Kind)));
  OutError.ProposedValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (Kind == EAssetKind::SoundCue) {
    USoundCue *Cue = Cast<USoundCue>(&Asset);
    if (!Cue) {
      OutError = DocumentFailure(TEXT("sound_cue_not_found"),
          TEXT("The asset selected for SoundCue compilation is not a SoundCue."), TEXT("asset_class"),
          MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(USoundCue::StaticClass()->GetPathName()));
      return false;
    }
    Cue->LinkGraphNodesFromSoundNodes();
    Cue->CompileSoundNodesFromGraphNodes();
  } else if (Kind == EAssetKind::MetaSound) {
    if (!Cast<IMetaSoundDocumentInterface>(&Asset)) {
      OutError = DocumentFailure(TEXT("metasound_not_found"),
          TEXT("The asset selected for MetaSound compilation does not implement the MetaSound document interface."), TEXT("asset_class"),
          MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("a class implementing MetaSoundDocumentInterface")));
      return false;
    }
    if (UMetaSoundEditorSubsystem *Subsystem =
            GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
                    : nullptr) {
      Subsystem->RegisterGraphWithFrontend(Asset, true);
    }
  }
  Asset.PostEditChange();
  Asset.MarkPackageDirty();
  return true;
}

bool SaveAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  return UeAgentAudioAuthoring::SaveSoundCuePackages({&Asset}, OutError);
}
} // namespace UeAgentAudioAuthoring
