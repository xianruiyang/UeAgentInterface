// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Metasound.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Misc/PackageName.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundSubmix.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

namespace UeAgentAudioAuthoring {
namespace {
using namespace UeAgentAudioOps;

bool CanonicalEqual(const TSharedPtr<FJsonValue> &A,
                    const TSharedPtr<FJsonValue> &B) {
  return UeAgentAuthoringState::MakeStableCanonicalValueKey(A) ==
         UeAgentAuthoringState::MakeStableCanonicalValueKey(B);
}

bool RequireCurrentValue(const TSharedPtr<const FJsonObject> &Requested,
                         const TCHAR *Field,
                         const TSharedPtr<FJsonObject> &Current,
                         FString &OutError) {
  if (!Requested.IsValid() || !Requested->HasField(Field))
    return true;
  const TSharedPtr<FJsonValue> RequestedValue = Requested->TryGetField(Field);
  const TSharedPtr<FJsonValue> CurrentValue =
      Current.IsValid() ? Current->TryGetField(Field) : nullptr;
  if (RequestedValue.IsValid() && CurrentValue.IsValid() &&
      CanonicalEqual(RequestedValue, CurrentValue)) {
    return true;
  }
  OutError = FString(TEXT("audio_readonly_field_mismatch:")) + Field;
  return false;
}

bool ValidateFields(const TSharedPtr<const FJsonObject> &Object,
                    const TSet<FString> &Allowed, const FString &Prefix,
                    FString &OutError) {
  if (!Object.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field : Object->Values) {
    if (!Allowed.Contains(Field.Key)) {
      OutError = Prefix + TEXT("_unknown_field:") + Field.Key;
      return false;
    }
  }
  return true;
}

TArray<TSharedPtr<FJsonValue>>
BuildEditableProperties(UObject &Object, const TSet<FName> &Excluded) {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.ExcludedProperties = Excluded;
  UeAgentAuthoringState::FStateSnapshot Snapshot;
  TSharedPtr<FJsonObject> Document;
  FString Error;
  if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
          &Object, Policy, Snapshot, Document, Error)) {
    return {};
  }
  TArray<TSharedPtr<FJsonValue>> Result;
  if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(
          Document, Result, Error)) {
    return {};
  }
  return Result;
}

bool ApplyEditableProperties(UObject &Object,
                             const TSharedPtr<const FJsonObject> &Owner,
                             FMutationStats &OutStats, FString &OutError) {
  if (!Owner.IsValid() || !Owner->HasField(TEXT("properties")))
    return true;
  const TArray<TSharedPtr<FJsonValue>> *Properties = nullptr;
  if (!Owner->TryGetArrayField(TEXT("properties"), Properties) || !Properties) {
    OutError = TEXT("audio_properties_array_invalid");
    return false;
  }
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  UeAgentAuthoringDirectApply::FResult Result;
  if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(&Object, *Properties,
                                                     Options, Result)) {
    OutError = Result.ErrorCode;
    if (!Result.ErrorDetail.IsEmpty())
      OutError += TEXT(":") + Result.ErrorDetail;
    return false;
  }
  OutStats.Settings += Result.PropertiesSeen;
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
  return Result;
}

bool DecodeLiteral(const TSharedPtr<const FJsonObject> &Object,
                   FMetasoundFrontendLiteral &OutLiteral, FString &OutError) {
  OutLiteral = FMetasoundFrontendLiteral();
  if (!Object.IsValid()) {
    OutError = TEXT("metasound_literal_invalid");
    return false;
  }
  FString Type;
  if (!Object->TryGetStringField(TEXT("type"), Type)) {
    OutError = TEXT("metasound_literal_type_required");
    return false;
  }
  if (Type == TEXT("none")) {
    OutLiteral.Set(FMetasoundFrontendLiteral::FDefault());
    return true;
  }
  if (Type == TEXT("none_array")) {
    int32 Num = 0;
    Object->TryGetNumberField(TEXT("value"), Num);
    OutLiteral.Set(FMetasoundFrontendLiteral::FDefaultArray{Num});
    return true;
  }
  bool BoolValue = false;
  double NumberValue = 0.0;
  FString StringValue;
  if (Type == TEXT("bool") &&
      Object->TryGetBoolField(TEXT("value"), BoolValue)) {
    OutLiteral.Set(BoolValue);
    return true;
  }
  if ((Type == TEXT("int") || Type == TEXT("float")) &&
      Object->TryGetNumberField(TEXT("value"), NumberValue)) {
    if (Type == TEXT("int"))
      OutLiteral.Set(static_cast<int32>(NumberValue));
    else
      OutLiteral.Set(static_cast<float>(NumberValue));
    return true;
  }
  if ((Type == TEXT("string") || Type == TEXT("object")) &&
      Object->TryGetStringField(TEXT("value"), StringValue)) {
    if (Type == TEXT("string")) {
      OutLiteral.Set(StringValue);
    } else {
      UObject *Value =
          StringValue.IsEmpty()
              ? nullptr
              : StaticLoadObject(UObject::StaticClass(), nullptr, *StringValue);
      if (!StringValue.IsEmpty() && !Value) {
        OutError = TEXT("metasound_literal_object_not_found:") + StringValue;
        return false;
      }
      OutLiteral.Set(Value);
    }
    return true;
  }
  const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
  if (!Object->TryGetArrayField(TEXT("value"), Values) || !Values) {
    OutError = TEXT("metasound_literal_value_invalid");
    return false;
  }
  if (Type == TEXT("bool_array")) {
    TArray<bool> Result;
    for (const TSharedPtr<FJsonValue> &Value : *Values)
      Result.Add(Value.IsValid() && Value->AsBool());
    OutLiteral.Set(Result);
    return true;
  }
  if (Type == TEXT("int_array")) {
    TArray<int32> Result;
    for (const TSharedPtr<FJsonValue> &Value : *Values)
      Result.Add(Value.IsValid() ? static_cast<int32>(Value->AsNumber()) : 0);
    OutLiteral.Set(Result);
    return true;
  }
  if (Type == TEXT("float_array")) {
    TArray<float> Result;
    for (const TSharedPtr<FJsonValue> &Value : *Values)
      Result.Add(Value.IsValid() ? static_cast<float>(Value->AsNumber())
                                 : 0.0f);
    OutLiteral.Set(Result);
    return true;
  }
  if (Type == TEXT("string_array")) {
    TArray<FString> Result;
    for (const TSharedPtr<FJsonValue> &Value : *Values)
      Result.Add(Value.IsValid() ? Value->AsString() : FString());
    OutLiteral.Set(Result);
    return true;
  }
  if (Type == TEXT("object_array")) {
    TArray<UObject *> Result;
    for (const TSharedPtr<FJsonValue> &Value : *Values) {
      const FString Path = Value.IsValid() ? Value->AsString() : FString();
      UObject *ObjectValue =
          Path.IsEmpty()
              ? nullptr
              : StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
      if (!Path.IsEmpty() && !ObjectValue) {
        OutError = TEXT("metasound_literal_object_not_found:") + Path;
        return false;
      }
      Result.Add(ObjectValue);
    }
    OutLiteral.Set(Result);
    return true;
  }
  OutError = TEXT("metasound_literal_type_unsupported:") + Type;
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

TSharedPtr<FJsonObject> BuildMetaSoundDocument(UObject &Asset,
                                               FString &OutError) {
  IMetaSoundDocumentInterface *DocumentInterface =
      Cast<IMetaSoundDocumentInterface>(&Asset);
  if (!DocumentInterface) {
    OutError = TEXT("metasound_document_interface_missing");
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
    FString StableBase;
  };
  TArray<FNodeRecord> Records;
  for (const FMetasoundFrontendNode &Node : Graph.Nodes) {
    const FMetasoundFrontendClass *Dependency =
        Document.Dependencies.FindByPredicate(
            [&Node](const FMetasoundFrontendClass &Item) {
              return Item.ID == Node.ClassID;
            });
    if (!Dependency)
      continue;
    FNodeRecord Record;
    Record.Node = &Node;
    Record.Dependency = Dependency;
    Record.Kind = MetaSoundNodeKind(Dependency->Metadata.GetType());
    if (Record.Kind != TEXT("graph_input") &&
        Record.Kind != TEXT("graph_output") &&
        Record.Kind != TEXT("external"))
      continue;
    Record.ClassName = Dependency->Metadata.GetClassName().ToString();
    Record.StableBase = Record.Kind + TEXT("|") + Node.Name.ToString() +
                        TEXT("|") + Record.ClassName;
    Records.Add(MoveTemp(Record));
  }
  Records.Sort([](const FNodeRecord &A, const FNodeRecord &B) {
    return A.StableBase < B.StableBase;
  });

  TMap<FGuid, FString> NodeIds;
  TMap<FGuid, TMap<FGuid, FString>> InputNames;
  TMap<FGuid, TMap<FGuid, FString>> OutputNames;
  TMap<FString, int32> Occurrences;
  TArray<TSharedPtr<FJsonValue>> Nodes;
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
  for (const FNodeRecord &Record : Records) {
    const int32 Occurrence =
        Occurrences.FindOrAdd(Record.Kind + TEXT("|") + Record.ClassName)++;
    FString StableId;
    if (Record.Kind == TEXT("graph_input") ||
        Record.Kind == TEXT("graph_output")) {
      StableId = Record.Kind + TEXT(":") + Record.Node->Name.ToString();
    } else {
      StableId = Record.Kind + TEXT(":") + Record.ClassName + TEXT("#") +
                 FString::FromInt(Occurrence);
    }
    NodeIds.Add(Record.Node->GetID(), StableId);
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("id"), StableId);
    Item->SetStringField(TEXT("kind"), Record.Kind);
    Item->SetStringField(TEXT("class_name"), Record.ClassName);
    Item->SetNumberField(TEXT("major_version"),
                         Record.Dependency->Metadata.GetVersion().Major);
    if (Record.Kind == TEXT("graph_input") ||
        Record.Kind == TEXT("graph_output"))
      Item->SetStringField(TEXT("member_name"), Record.Node->Name.ToString());
    TArray<TSharedPtr<FJsonValue>> NodeInputs;
    for (const FMetasoundFrontendVertex &Input :
         Record.Node->Interface.Inputs) {
      InputNames.FindOrAdd(Record.Node->GetID())
          .Add(Input.VertexID, Input.Name.ToString());
      TSharedRef<FJsonObject> Vertex = MakeShared<FJsonObject>();
      Vertex->SetStringField(TEXT("name"), Input.Name.ToString());
      Vertex->SetStringField(TEXT("type"), Input.TypeName.ToString());
      if (Builder) {
        EMetaSoundBuilderResult LiteralResult = EMetaSoundBuilderResult::Failed;
        const FMetasoundFrontendLiteral Literal = Builder->GetNodeInputDefault(
            FMetaSoundBuilderNodeInputHandle(Record.Node->GetID(),
                                             Input.VertexID),
            LiteralResult);
        if (LiteralResult == EMetaSoundBuilderResult::Succeeded) {
          if (TSharedPtr<FJsonObject> Encoded = EncodeLiteral(Literal))
            Vertex->SetObjectField(TEXT("default"), Encoded);
        }
      }
      NodeInputs.Add(MakeShared<FJsonValueObject>(Vertex));
    }
    TArray<TSharedPtr<FJsonValue>> NodeOutputs;
    for (const FMetasoundFrontendVertex &Output :
         Record.Node->Interface.Outputs) {
      OutputNames.FindOrAdd(Record.Node->GetID())
          .Add(Output.VertexID, Output.Name.ToString());
      TSharedRef<FJsonObject> Vertex = MakeShared<FJsonObject>();
      Vertex->SetStringField(TEXT("name"), Output.Name.ToString());
      Vertex->SetStringField(TEXT("type"), Output.TypeName.ToString());
      NodeOutputs.Add(MakeShared<FJsonValueObject>(Vertex));
    }
    Item->SetArrayField(TEXT("inputs"), NodeInputs);
    Item->SetArrayField(TEXT("outputs"), NodeOutputs);
    Nodes.Add(MakeShared<FJsonValueObject>(Item));
  }

  TArray<TSharedPtr<FJsonValue>> Edges;
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
    Item->SetStringField(TEXT("from_node"), FromNode);
    Item->SetStringField(TEXT("from_pin"), FromPin);
    Item->SetStringField(TEXT("to_node"), ToNode);
    Item->SetStringField(TEXT("to_pin"), ToPin);
    Edges.Add(MakeShared<FJsonValueObject>(Item));
  }
  Edges.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return UeAgentAuthoringState::MakeStableCanonicalValueKey(A) <
               UeAgentAuthoringState::MakeStableCanonicalValueKey(B);
      });
  TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
  GraphJson->SetArrayField(TEXT("nodes"), Nodes);
  GraphJson->SetArrayField(TEXT("edges"), Edges);
  Result->SetObjectField(TEXT("graph"), GraphJson);
  return Result;
}

TSharedPtr<FJsonObject> BuildSoundCueDocument(USoundCue &Cue) {
  const TSharedPtr<FJsonObject> Info =
      BuildSoundCueInfo(&Cue, true, false, false);
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
    if (OldId.IsEmpty())
      OldId = FString::Printf(TEXT("node_%d"), Index);
    if (Type.IsEmpty())
      Type = Node->GetClass()->GetName();
    Records.Add({Node, OldId, Type});
  }
  Records.Sort([](const FNodeRecord &A, const FNodeRecord &B) {
    const FString AKey = A.Type + TEXT("|") + A.OldId;
    const FString BKey = B.Type + TEXT("|") + B.OldId;
    return AKey < BKey;
  });
  TMap<FString, FString> IdMap;
  TMap<FString, int32> Occurrences;
  TArray<TSharedPtr<FJsonValue>> Nodes;
  for (const FNodeRecord &Record : Records) {
    const int32 Occurrence = Occurrences.FindOrAdd(Record.Type)++;
    const FString Id =
        TEXT("node:") + Record.Type + TEXT("#") + FString::FromInt(Occurrence);
    IdMap.Add(Record.OldId, Id);
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("id"), Id);
    Item->SetStringField(TEXT("type"), Record.Type);
    Item->SetStringField(TEXT("class_path"),
                         Record.Node->GetClass()->GetPathName());
    Item->SetArrayField(
        TEXT("properties"),
        BuildEditableProperties(*Record.Node, {TEXT("ChildNodes")}));
    if (const UEdGraphNode *GraphNode = Record.Node->GetGraphNode()) {
      Item->SetArrayField(TEXT("position"),
                          {MakeShared<FJsonValueNumber>(GraphNode->NodePosX),
                           MakeShared<FJsonValueNumber>(GraphNode->NodePosY)});
    }
    Nodes.Add(MakeShared<FJsonValueObject>(Item));
  }
  TArray<TSharedPtr<FJsonValue>> Edges;
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
      TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
      Item->SetStringField(TEXT("from_node"), IdMap.FindRef(From));
      Item->SetStringField(TEXT("to_node"), IdMap.FindRef(To));
      int32 ChildIndex = 0;
      Edge->TryGetNumberField(TEXT("child_index"), ChildIndex);
      Item->SetNumberField(TEXT("child_index"), ChildIndex);
      Edges.Add(MakeShared<FJsonValueObject>(Item));
    }
  }
  TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
  FString RootId;
  Info->TryGetStringField(TEXT("root_node_id"), RootId);
  Graph->SetStringField(TEXT("root_node_id"), IdMap.FindRef(RootId));
  Graph->SetArrayField(TEXT("nodes"), Nodes);
  Graph->SetArrayField(TEXT("edges"), Edges);
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
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
                         FString &OutError) {
  OutBuilder = nullptr;
  IMetaSoundDocumentInterface *Interface =
      Cast<IMetaSoundDocumentInterface>(&Asset);
  UMetaSoundEditorSubsystem *Subsystem =
      GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
              : nullptr;
  if (!Interface || !Subsystem) {
    OutError = TEXT("metasound_builder_unavailable");
    return false;
  }
  TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface;
  ScriptInterface.SetObject(&Asset);
  ScriptInterface.SetInterface(Interface);
  EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
  OutBuilder = Subsystem->FindOrBeginBuilding(ScriptInterface, Result);
  if (!OutBuilder || Result != EMetaSoundBuilderResult::Succeeded) {
    OutError = TEXT("metasound_builder_unavailable");
    return false;
  }
  return true;
}

bool ApplyMetaSoundMembers(UMetaSoundBuilderBase &Builder,
                           const TSharedPtr<const FJsonObject> &Members,
                           TMap<FString, FMetaSoundNodeHandle> &OutNodeHandles,
                           FMutationStats &OutStats, FString &OutError) {
  if (!Members.IsValid())
    return true;
  for (const TPair<FString, bool> &Field :
       {TPair<FString, bool>(TEXT("inputs"), true),
        TPair<FString, bool>(TEXT("outputs"), false)}) {
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!Members->TryGetArrayField(Field.Key, Items) || !Items)
      continue;
    for (const TSharedPtr<FJsonValue> &Value : *Items) {
      const TSharedPtr<FJsonObject> Item =
          Value.IsValid() ? Value->AsObject() : nullptr;
      FString Name;
      FString Type;
      if (!Item.IsValid() || !Item->TryGetStringField(TEXT("name"), Name) ||
          !Item->TryGetStringField(TEXT("type"), Type) || Name.IsEmpty() ||
          Type.IsEmpty()) {
        OutError = TEXT("metasound_member_invalid");
        return false;
      }
      EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
      FMetaSoundNodeHandle Handle =
          Field.Value ? Builder.FindGraphInputNode(*Name, FindResult)
                      : Builder.FindGraphOutputNode(*Name, FindResult);
      if (FindResult != EMetaSoundBuilderResult::Succeeded) {
        FMetasoundFrontendLiteral Default;
        const TSharedPtr<FJsonObject> *DefaultObject = nullptr;
        if (Item->TryGetObjectField(TEXT("default"), DefaultObject) &&
            DefaultObject && DefaultObject->IsValid() &&
            !DecodeLiteral(*DefaultObject, Default, OutError)) {
          return false;
        }
        EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
        if (Field.Value) {
          const FMetaSoundBuilderNodeOutputHandle Output =
              Builder.AddGraphInputNode(*Name, *Type, Default, AddResult);
          Handle = FMetaSoundNodeHandle(Output.NodeID);
        } else {
          const FMetaSoundBuilderNodeInputHandle Input =
              Builder.AddGraphOutputNode(*Name, *Type, Default, AddResult);
          Handle = FMetaSoundNodeHandle(Input.NodeID);
        }
        if (AddResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = Field.Value ? TEXT("metasound_add_input_failed")
                                 : TEXT("metasound_add_output_failed");
          return false;
        }
        ++OutStats.Members;
      }
      OutNodeHandles.Add(
          (Field.Value ? TEXT("graph_input:") : TEXT("graph_output:")) + Name,
          Handle);
      if (Field.Value) {
        const TSharedPtr<FJsonObject> *DefaultObject = nullptr;
        if (Item->TryGetObjectField(TEXT("default"), DefaultObject) &&
            DefaultObject && DefaultObject->IsValid()) {
          FMetasoundFrontendLiteral Default;
          if (!DecodeLiteral(*DefaultObject, Default, OutError))
            return false;
          EMetaSoundBuilderResult DefaultResult =
              EMetaSoundBuilderResult::Failed;
          Builder.SetGraphInputDefault(*Name, Default, DefaultResult);
          if (DefaultResult != EMetaSoundBuilderResult::Succeeded) {
            OutError = TEXT("metasound_set_input_default_failed:") + Name;
            return false;
          }
        }
      }
    }
  }
  return true;
}

bool ApplyMetaSoundGraph(UMetaSoundBuilderBase &Builder,
                         const TSharedPtr<const FJsonObject> &Graph,
                         TMap<FString, FMetaSoundNodeHandle> &NodeHandles,
                         FMutationStats &OutStats, FString &OutError) {
  if (!Graph.IsValid())
    return true;
  const TArray<TSharedPtr<FJsonValue>> *Nodes = nullptr;
  if (!Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) {
    OutError = TEXT("metasound_graph_nodes_required");
    return false;
  }
  for (const TSharedPtr<FJsonValue> &Value : *Nodes) {
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString Id;
    FString Kind;
    FString ClassNameText;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id) ||
        !Item->TryGetStringField(TEXT("kind"), Kind) || Id.IsEmpty()) {
      OutError = TEXT("metasound_graph_node_invalid");
      return false;
    }
    if (Kind == TEXT("graph_input") || Kind == TEXT("graph_output")) {
      if (!NodeHandles.Contains(Id)) {
        FString MemberName;
        Item->TryGetStringField(TEXT("member_name"), MemberName);
        EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
        FMetaSoundNodeHandle Handle =
            Kind == TEXT("graph_input")
                ? Builder.FindGraphInputNode(*MemberName, FindResult)
                : Builder.FindGraphOutputNode(*MemberName, FindResult);
        if (FindResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = TEXT("metasound_graph_member_node_missing:") + Id;
          return false;
        }
        NodeHandles.Add(Id, Handle);
      }
      continue;
    }
    if (Kind != TEXT("external")) {
      OutError = TEXT("metasound_node_kind_unsupported:") + Kind;
      return false;
    }
    if (!Item->TryGetStringField(TEXT("class_name"), ClassNameText)) {
      OutError = TEXT("metasound_node_class_name_required:") + Id;
      return false;
    }
    FMetasoundFrontendClassName ClassName;
    if (!FMetasoundFrontendClassName::Parse(ClassNameText, ClassName)) {
      OutError = TEXT("metasound_node_class_name_invalid:") + ClassNameText;
      return false;
    }
    int32 MajorVersion = 1;
    Item->TryGetNumberField(TEXT("major_version"), MajorVersion);
    EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundNodeHandle Handle =
        Builder.AddNodeByClassName(ClassName, AddResult, MajorVersion);
    if (AddResult != EMetaSoundBuilderResult::Succeeded || !Handle.IsSet()) {
      OutError = TEXT("metasound_add_node_failed:") + ClassNameText;
      return false;
    }
    NodeHandles.Add(Id, Handle);
    ++OutStats.Nodes;
    const TArray<TSharedPtr<FJsonValue>> *Inputs = nullptr;
    if (Item->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs) {
      for (const TSharedPtr<FJsonValue> &InputValue : *Inputs) {
        const TSharedPtr<FJsonObject> Input =
            InputValue.IsValid() ? InputValue->AsObject() : nullptr;
        FString InputName;
        const TSharedPtr<FJsonObject> *DefaultObject = nullptr;
        if (!Input.IsValid() ||
            !Input->TryGetStringField(TEXT("name"), InputName) ||
            !Input->TryGetObjectField(TEXT("default"), DefaultObject) ||
            !DefaultObject || !DefaultObject->IsValid()) {
          continue;
        }
        EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
        const FMetaSoundBuilderNodeInputHandle InputHandle =
            Builder.FindNodeInputByName(Handle, *InputName, FindResult);
        if (FindResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = TEXT("metasound_node_input_not_found:") + Id + TEXT(":") +
                     InputName;
          return false;
        }
        FMetasoundFrontendLiteral Default;
        if (!DecodeLiteral(*DefaultObject, Default, OutError))
          return false;
        EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
        Builder.SetNodeInputDefault(InputHandle, Default, SetResult);
        if (SetResult != EMetaSoundBuilderResult::Succeeded) {
          OutError = TEXT("metasound_node_default_failed:") + Id + TEXT(":") +
                     InputName;
          return false;
        }
      }
    }
  }
  const TArray<TSharedPtr<FJsonValue>> *Edges = nullptr;
  if (!Graph->TryGetArrayField(TEXT("edges"), Edges) || !Edges)
    return true;
  for (const TSharedPtr<FJsonValue> &Value : *Edges) {
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
      OutError = TEXT("metasound_edge_invalid");
      return false;
    }
    EMetaSoundBuilderResult FromResult = EMetaSoundBuilderResult::Failed;
    EMetaSoundBuilderResult ToResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeOutputHandle Output =
        Builder.FindNodeOutputByName(NodeHandles.FindChecked(FromNode),
                                     *FromPin, FromResult);
    const FMetaSoundBuilderNodeInputHandle Input = Builder.FindNodeInputByName(
        NodeHandles.FindChecked(ToNode), *ToPin, ToResult);
    if (FromResult != EMetaSoundBuilderResult::Succeeded ||
        ToResult != EMetaSoundBuilderResult::Succeeded) {
      OutError = TEXT("metasound_edge_pin_not_found");
      return false;
    }
    EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
    Builder.ConnectNodes(Output, Input, ConnectResult);
    if (ConnectResult != EMetaSoundBuilderResult::Succeeded) {
      OutError = TEXT("metasound_connect_nodes_failed");
      return false;
    }
    ++OutStats.Edges;
  }
  return true;
}

bool ApplyMetaSoundDocument(UObject &Asset,
                            const TSharedPtr<const FJsonObject> &Document,
                            FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("interfaces"), TEXT("members"),
                       TEXT("graph")},
                      TEXT("metasound_document"), OutError))
    return false;
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
      CurrentSettings && CurrentSettings->IsValid() &&
      !CanonicalEqual(MakeShared<FJsonValueObject>(*RequestedSettings),
                      MakeShared<FJsonValueObject>(*CurrentSettings))) {
    OutError = TEXT("metasound_asset_type_immutable");
    return false;
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
  if (!bHasMembers && !bHasGraph)
    return true;
  UMetaSoundBuilderBase *Builder = nullptr;
  if (!GetMetaSoundBuilder(Asset, Builder, OutError))
    return false;

  if (bHasGraph) {
    IMetaSoundDocumentInterface *Interface =
        CastChecked<IMetaSoundDocumentInterface>(&Asset);
    const FMetasoundFrontendDocument &FrontendDocument =
        Interface->GetConstDocument();
    const FMetasoundFrontendGraph &FrontendGraph =
        FrontendDocument.RootGraph.GetConstDefaultGraph();
    TArray<FMetaSoundNodeHandle> ExistingExternal;
    for (const FMetasoundFrontendNode &Node : FrontendGraph.Nodes) {
      const FMetasoundFrontendClass *Dependency =
          FrontendDocument.Dependencies.FindByPredicate(
              [&Node](const FMetasoundFrontendClass &Item) {
                return Item.ID == Node.ClassID;
              });
      if (Dependency && Dependency->Metadata.GetType() ==
                            EMetasoundFrontendClassType::External) {
        ExistingExternal.Add(FMetaSoundNodeHandle(Node.GetID()));
      }
    }
    for (const FMetaSoundNodeHandle &Handle : ExistingExternal) {
      EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
      Builder->RemoveNode(Handle, RemoveResult);
      if (RemoveResult != EMetaSoundBuilderResult::Succeeded) {
        OutError = TEXT("metasound_remove_existing_node_failed");
        return false;
      }
    }
  }

  TMap<FString, FMetaSoundNodeHandle> NodeHandles;
  if (bHasMembers && !ApplyMetaSoundMembers(*Builder, *Members, NodeHandles,
                                            OutStats, OutError))
    return false;
  if (bHasGraph &&
      !ApplyMetaSoundGraph(*Builder, *Graph, NodeHandles, OutStats, OutError))
    return false;
  Builder->InitNodeLocations();
  Builder->ConformObjectToDocument();
  if (UMetaSoundEditorSubsystem *Subsystem =
          GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
                  : nullptr) {
    Subsystem->RegisterGraphWithFrontend(Asset, true);
  }
  Asset.PostEditChange();
  Asset.MarkPackageDirty();
  return true;
}

bool ApplySoundCueDocument(USoundCue &Cue,
                           const TSharedPtr<const FJsonObject> &Document,
                           FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document, {TEXT("graph")}, TEXT("sound_cue_document"),
                      OutError))
    return false;
  const TSharedPtr<FJsonObject> *Graph = nullptr;
  if (!Document.IsValid() ||
      !Document->TryGetObjectField(TEXT("graph"), Graph) || !Graph ||
      !Graph->IsValid())
    return true;
  const TArray<TSharedPtr<FJsonValue>> *Nodes = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *Edges = nullptr;
  if (!(*Graph)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes ||
      !(*Graph)->TryGetArrayField(TEXT("edges"), Edges) || !Edges) {
    OutError = TEXT("sound_cue_graph_shape_invalid");
    return false;
  }
  TSharedRef<FJsonObject> NodesJson = MakeShared<FJsonObject>();
  FString RootNodeId;
  (*Graph)->TryGetStringField(TEXT("root_node_id"), RootNodeId);
  NodesJson->SetStringField(TEXT("root_node_id"), RootNodeId);
  TArray<TSharedPtr<FJsonValue>> LegacyNodes;
  for (const TSharedPtr<FJsonValue> &Value : *Nodes) {
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString Id;
    FString Type;
    FString ClassPath;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id) ||
        !Item->TryGetStringField(TEXT("type"), Type) || Id.IsEmpty()) {
      OutError = TEXT("sound_cue_node_invalid");
      return false;
    }
    Item->TryGetStringField(TEXT("class_path"), ClassPath);
    TSharedRef<FJsonObject> Legacy = MakeShared<FJsonObject>();
    Legacy->SetStringField(TEXT("id"), Id);
    Legacy->SetStringField(TEXT("type"), Type);
    Legacy->SetStringField(TEXT("class"), ClassPath);
    LegacyNodes.Add(MakeShared<FJsonValueObject>(Legacy));
  }
  NodesJson->SetArrayField(TEXT("nodes"), LegacyNodes);
  TSharedRef<FJsonObject> EdgesJson = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> LegacyEdges;
  for (const TSharedPtr<FJsonValue> &Value : *Edges) {
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString From;
    FString To;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("from_node"), From) ||
        !Item->TryGetStringField(TEXT("to_node"), To)) {
      OutError = TEXT("sound_cue_edge_invalid");
      return false;
    }
    TSharedRef<FJsonObject> Legacy = MakeShared<FJsonObject>();
    Legacy->SetStringField(TEXT("from"), From);
    Legacy->SetStringField(TEXT("to"), To);
    int32 ChildIndex = 0;
    Item->TryGetNumberField(TEXT("child_index"), ChildIndex);
    Legacy->SetNumberField(TEXT("child_index"), ChildIndex);
    LegacyEdges.Add(MakeShared<FJsonValueObject>(Legacy));
  }
  EdgesJson->SetArrayField(TEXT("edges"), LegacyEdges);
  TArray<TSharedPtr<FJsonValue>> PropertyResults;
  int32 NodesCreated = 0;
  int32 EdgesCreated = 0;
  if (!ApplySoundCueGraph(&Cue, NodesJson, EdgesJson, PropertyResults,
                          NodesCreated, EdgesCreated, false, OutError))
    return false;
  OutStats.Nodes += NodesCreated;
  OutStats.Edges += EdgesCreated;
  for (const TSharedPtr<FJsonValue> &Value : *Nodes) {
    const TSharedPtr<FJsonObject> Item =
        Value.IsValid() ? Value->AsObject() : nullptr;
    FString Id;
    if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id))
      continue;
    const TObjectPtr<USoundNode> *FoundNode = Cue.AllNodes.FindByPredicate(
        [&Id](const TObjectPtr<USoundNode> &Candidate) {
          return Candidate && Candidate->GetName() == Id;
        });
    USoundNode *Node = FoundNode ? FoundNode->Get() : nullptr;
    if (!Node) {
      OutError = TEXT("sound_cue_node_readback_missing:") + Id;
      return false;
    }
    if (!ApplyEditableProperties(*Node, Item, OutStats, OutError))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Position = nullptr;
    if (Item->TryGetArrayField(TEXT("position"), Position) && Position &&
        Position->Num() == 2) {
      if (UEdGraphNode *GraphNode = Node->GetGraphNode()) {
        GraphNode->NodePosX = static_cast<int32>((*Position)[0]->AsNumber());
        GraphNode->NodePosY = static_cast<int32>((*Position)[1]->AsNumber());
      }
    }
  }
  Cue.PostEditChange();
  Cue.MarkPackageDirty();
  return true;
}

bool ApplySoundSubmixDocument(USoundSubmixBase &Submix,
                              const TSharedPtr<const FJsonObject> &Document,
                              FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document, {TEXT("settings"), TEXT("children")},
                      TEXT("sound_submix_document"), OutError))
    return false;
  const TSharedPtr<FJsonObject> Current = BuildSoundSubmixDocument(Submix);
  if (!RequireCurrentValue(Document, TEXT("children"), Current, OutError))
    return false;
  const TSharedPtr<FJsonObject> *Settings = nullptr;
  if (!Document.IsValid() ||
      !Document->TryGetObjectField(TEXT("settings"), Settings) || !Settings ||
      !Settings->IsValid())
    return true;
  FString RequestedClass;
  if ((*Settings)->TryGetStringField(TEXT("submix_class"), RequestedClass) &&
      !RequestedClass.Equals(Submix.GetClass()->GetPathName())) {
    OutError = TEXT("sound_submix_class_immutable");
    return false;
  }
  FString ParentPath;
  if ((*Settings)->TryGetStringField(TEXT("parent"), ParentPath)) {
    USoundSubmixWithParentBase *WithParent =
        Cast<USoundSubmixWithParentBase>(&Submix);
    if (!WithParent) {
      OutError = TEXT("parent_submix_not_applicable");
      return false;
    }
    USoundSubmixBase *Parent =
        ParentPath.IsEmpty()
            ? nullptr
            : Cast<USoundSubmixBase>(StaticLoadObject(
                  USoundSubmixBase::StaticClass(), nullptr, *ParentPath));
    if (!ParentPath.IsEmpty() && !Parent) {
      OutError = TEXT("parent_submix_not_found:") + ParentPath;
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
      OutError = TEXT("submix_effect_chain_not_applicable");
      return false;
    }
    if (Concrete) {
      TArray<TObjectPtr<USoundEffectSubmixPreset>> Desired;
      for (const TSharedPtr<FJsonValue> &Value : *Chain) {
        const FString Path = Value.IsValid() ? Value->AsString() : FString();
        USoundEffectSubmixPreset *Preset =
            Cast<USoundEffectSubmixPreset>(StaticLoadObject(
                USoundEffectSubmixPreset::StaticClass(), nullptr, *Path));
        if (!Preset) {
          OutError = TEXT("submix_effect_preset_not_found:") + Path;
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
                           FString &OutError) {
  OutError.Reset();
  if (!Outer) {
    OutError = TEXT("audio_seed_outer_missing");
    return nullptr;
  }
  if (Name.IsNone())
    Name = MakeUniqueObjectName(Outer, UObject::StaticClass(), TEXT("Seed"));
  if (Kind == EAssetKind::SoundCue)
    return NewObject<USoundCue>(Outer, Name, Flags);
  if (Kind == EAssetKind::SoundSubmix) {
    UClass *Class = USoundSubmix::StaticClass();
    const TSharedPtr<FJsonObject> *Settings = nullptr;
    FString ClassPath;
    if (SeedDocument.IsValid() &&
        SeedDocument->TryGetObjectField(TEXT("settings"), Settings) &&
        Settings && Settings->IsValid() &&
        (*Settings)->TryGetStringField(TEXT("submix_class"), ClassPath) &&
        !ClassPath.IsEmpty()) {
      Class = LoadObject<UClass>(nullptr, *ClassPath);
    }
    if (!Class || !Class->IsChildOf(USoundSubmixBase::StaticClass())) {
      OutError = TEXT("invalid_submix_class");
      return nullptr;
    }
    return NewObject<UObject>(Outer, Class, Name, Flags);
  }
  if (Kind == EAssetKind::MetaSound) {
    FString AssetType = TEXT("source");
    const TSharedPtr<FJsonObject> *Settings = nullptr;
    if (SeedDocument.IsValid() &&
        SeedDocument->TryGetObjectField(TEXT("settings"), Settings) &&
        Settings && Settings->IsValid()) {
      (*Settings)->TryGetStringField(TEXT("asset_type"), AssetType);
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
      OutError = TEXT("metasound_factory_seed_failed");
    return Asset;
  }
  OutError = TEXT("audio_asset_kind_unsupported");
  return nullptr;
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, FString &OutError,
                   const FString &CanonicalAssetPath) {
  OutDocument.Reset();
  OutError.Reset();
  switch (Kind) {
  case EAssetKind::MetaSound:
    OutDocument = BuildMetaSoundDocument(Asset, OutError);
    break;
  case EAssetKind::SoundCue:
    if (USoundCue *Cue = Cast<USoundCue>(&Asset))
      OutDocument = BuildSoundCueDocument(*Cue);
    break;
  case EAssetKind::SoundSubmix:
    if (USoundSubmixBase *Submix = Cast<USoundSubmixBase>(&Asset))
      OutDocument = BuildSoundSubmixDocument(*Submix);
    break;
  default:
    break;
  }
  if (!OutDocument.IsValid() && OutError.IsEmpty())
    OutError = TEXT("audio_asset_class_mismatch");
  return OutDocument.IsValid();
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, FString &OutError,
                   const FString &CanonicalAssetPath) {
  OutStats = FMutationStats();
  OutError.Reset();
  if (!Document.IsValid())
    return true;
  switch (Kind) {
  case EAssetKind::MetaSound:
    return ApplyMetaSoundDocument(Asset, Document, OutStats, OutError);
  case EAssetKind::SoundCue:
    if (USoundCue *Cue = Cast<USoundCue>(&Asset))
      return ApplySoundCueDocument(*Cue, Document, OutStats, OutError);
    break;
  case EAssetKind::SoundSubmix:
    if (USoundSubmixBase *Submix = Cast<USoundSubmixBase>(&Asset))
      return ApplySoundSubmixDocument(*Submix, Document, OutStats, OutError);
    break;
  default:
    break;
  }
  OutError = TEXT("audio_asset_class_mismatch");
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  OutError.Reset();
  if (Kind == EAssetKind::SoundCue) {
    USoundCue *Cue = Cast<USoundCue>(&Asset);
    if (!Cue) {
      OutError = TEXT("sound_cue_not_found");
      return false;
    }
    Cue->LinkGraphNodesFromSoundNodes();
    Cue->CompileSoundNodesFromGraphNodes();
  } else if (Kind == EAssetKind::MetaSound) {
    if (!Cast<IMetaSoundDocumentInterface>(&Asset)) {
      OutError = TEXT("metasound_not_found");
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

bool SaveAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  return SavePackagesForObjects({&Asset}, OutError);
}
} // namespace UeAgentAudioAuthoring
