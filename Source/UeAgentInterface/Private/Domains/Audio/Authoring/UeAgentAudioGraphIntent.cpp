// Copyright Epic Games, Inc. All Rights Reserved.
#include "Domains/Audio/Authoring/UeAgentAudioGraphIntent.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "EdGraph/EdGraph.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentAudioAuthoring {
USoundCueGraphNode_Root* FindSoundCueOutputNode(USoundCue& Cue) {
  if (auto* Graph = Cue.GetGraph())
    for (UEdGraphNode* Node : Graph->Nodes)
      if (auto* Root = Cast<USoundCueGraphNode_Root>(Node)) return Root;
  return nullptr;
}
const FMetasoundFrontendLiteral* FindMetaSoundEffectiveInputDefault(
    UMetaSoundBuilderBase& Builder, const FGuid& Node, const FGuid& Pin) {
  if (const auto* Value = Builder.GetBuilder().GetNodeInputDefault(
      Node, Pin, &Metasound::Frontend::DefaultPageID)) return Value;
  return Builder.GetBuilder().GetNodeInputClassDefault(
      Node, Pin, &Metasound::Frontend::DefaultPageID);
}

bool FAudioGraphIntent::Verify(UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError) const {
  auto Fail = [&](const TCHAR* Code, const FString& Reason, const FString& Pointer,
      const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
      const FString& Candidate = FString()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("audio_graph_verify"));
    OutError.PropertyPath = Field;
    OutError.CandidateName = Candidate;
    OutError.ActualValuePresent = Actual.IsValid(); OutError.ExpectedValuePresent = Expected.IsValid();
    OutError.ActualValue = MoveTemp(Actual); OutError.ExpectedValue = MoveTemp(Expected);
    if (Pointer.IsEmpty()) OutError.SourceKind = TEXT("native_result");
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Pointer);
    return false;
  };
  const auto PositionValue = [](double X, double Y) {
    auto Value = MakeShared<FJsonObject>();
    Value->SetNumberField(TEXT("x"), X); Value->SetNumberField(TEXT("y"), Y);
    return MakeShared<FJsonValueObject>(Value);
  };
  const auto FailPosition = [&](const TCHAR* Code, const FString& Reason, const FString& Pointer,
      bool bPresent, double ActualX, double ActualY, double ExpectedX, double ExpectedY, const FString& Candidate = FString()) {
    if (!bPresent) return Fail(Code, Reason, Pointer, TEXT("position"), nullptr, PositionValue(ExpectedX, ExpectedY), Candidate);
    const bool bX = ActualX != ExpectedX;
    return Fail(Code, Reason + (bX ? TEXT(" The x coordinate differs.") : TEXT(" The y coordinate differs.")),
        Pointer.IsEmpty() ? FString() : Pointer + (bX ? TEXT("/x") : TEXT("/y")),
        bX ? TEXT("position.x") : TEXT("position.y"), MakeShared<FJsonValueNumber>(bX ? ActualX : ActualY),
        MakeShared<FJsonValueNumber>(bX ? ExpectedX : ExpectedY), Candidate);
  };
  if (!MetaNodes.IsEmpty() || !MetaLinks.IsEmpty() || !MetaDefaults.IsEmpty()
      || CompleteMetaExternalNodes.IsSet() || CompleteMetaLinkNodes.IsSet()) {
    const auto* Interface = Cast<IMetaSoundDocumentInterface>(&Asset);
    if (!Interface) return Fail(TEXT("metasound_document_interface_missing"),
        TEXT("The asset no longer implements the MetaSound document interface during graph verification."), FString(), TEXT("asset_class"),
        MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("a class implementing MetaSoundDocumentInterface")));
    const auto& Document = Interface->GetConstDocument();
    const auto& Graph = Document.RootGraph.GetConstDefaultGraph();
    for (const auto& Intent : MetaNodes) {
      const auto* Node = Graph.Nodes.FindByPredicate([&](const auto& Item) { return Item.GetID() == Intent.Id; });
      if ((Node != nullptr) != Intent.bPresent)
        return Fail(TEXT("metasound_node_intent_not_retained"),
            Intent.bPresent ? TEXT("The requested MetaSound node is missing after apply.") : TEXT("The removed MetaSound node still exists after apply."),
            Intent.SourcePointer, TEXT("node_present"), MakeShared<FJsonValueBoolean>(Node != nullptr), MakeShared<FJsonValueBoolean>(Intent.bPresent), Intent.PublicName);
      if (!Node) continue;
      if (!Intent.ClassName.IsEmpty()) {
        const auto* Class = Document.Dependencies.FindByPredicate([&](const auto& Item) { return Item.ID == Node->ClassID; });
        FMetasoundFrontendClassName Expected;
        const bool bParsed = Class && FMetasoundFrontendClassName::Parse(Intent.ClassName, Expected);
        if (!Class || !bParsed
            || Class->Metadata.GetClassName() != Expected
            || (Intent.Major != INDEX_NONE && Class->Metadata.GetVersion().Major != Intent.Major)
            || (Intent.Minor != INDEX_NONE && Class->Metadata.GetVersion().Minor != Intent.Minor))
        {
          if (!Class) return Fail(TEXT("metasound_node_class_intent_not_retained"),
              TEXT("The MetaSound node has no native class dependency during verification."), Intent.SourcePointer,
              TEXT("class_dependency_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Intent.ClassName);
          if (!bParsed) return Fail(TEXT("metasound_node_class_intent_not_retained"),
              TEXT("The retained class selector cannot be parsed as a MetaSound frontend class name."), Intent.SourcePointer,
              TEXT("class_name"), MakeShared<FJsonValueString>(Intent.ClassName), MakeShared<FJsonValueString>(TEXT("a valid MetaSound frontend class name")));
          if (Class->Metadata.GetClassName() != Expected) return Fail(TEXT("metasound_node_class_intent_not_retained"),
              TEXT("The MetaSound node retained a different class."), Intent.SourcePointer, TEXT("class_name"),
              MakeShared<FJsonValueString>(Class->Metadata.GetClassName().ToString()), MakeShared<FJsonValueString>(Intent.ClassName));
          const bool bMajor = Intent.Major != INDEX_NONE && Class->Metadata.GetVersion().Major != Intent.Major;
          return Fail(TEXT("metasound_node_class_intent_not_retained"),
              bMajor ? TEXT("The MetaSound node retained a different major class version.") : TEXT("The MetaSound node retained a different minor class version."), Intent.SourcePointer,
              bMajor ? TEXT("major_version") : TEXT("minor_version"),
              MakeShared<FJsonValueNumber>(bMajor ? Class->Metadata.GetVersion().Major : Class->Metadata.GetVersion().Minor),
              MakeShared<FJsonValueNumber>(bMajor ? Intent.Major : Intent.Minor), Intent.ClassName);
        }
      }
      if (Intent.Position.IsSet()) {
        const auto* Actual = Node->Style.Display.Locations.Find(Intent.Id);
        if (!Actual || *Actual != Intent.Position.GetValue())
          return FailPosition(TEXT("metasound_node_position_intent_not_retained"),
              !Actual ? TEXT("The MetaSound node has no stored position after apply.") : TEXT("The MetaSound node retained a different position."),
              Intent.PositionSourcePointer.IsEmpty() ? Intent.SourcePointer : Intent.PositionSourcePointer,
              Actual != nullptr, Actual ? Actual->X : 0.0, Actual ? Actual->Y : 0.0, Intent.Position->X, Intent.Position->Y);
      }
    }
    if (CompleteMetaExternalNodes.IsSet()) {
      TSet<FGuid> Actual;
      for (const auto& Node : Graph.Nodes) {
        const auto* Class = Document.Dependencies.FindByPredicate([&](const auto& Item) { return Item.ID == Node.ClassID; });
        if (Class && Class->Metadata.GetType() == EMetasoundFrontendClassType::External) Actual.Add(Node.GetID());
      }
      if (Actual.Num() != CompleteMetaExternalNodes->Num() || Actual.Difference(*CompleteMetaExternalNodes).Num()) {
        const auto Unexpected = Actual.Difference(*CompleteMetaExternalNodes);
        const bool bUnexpected = !Unexpected.IsEmpty();
        const auto Different = bUnexpected ? Unexpected : CompleteMetaExternalNodes->Difference(Actual);
        const FGuid Id = *Different.CreateConstIterator();
        const auto* Intent = MetaNodes.FindByPredicate([&](const auto& Item) { return Item.Id == Id; });
        const auto* Node = Graph.Nodes.FindByPredicate([&](const auto& Item) { return Item.GetID() == Id; });
        Fail(TEXT("metasound_node_collection_intent_not_retained"),
            bUnexpected ? TEXT("An unexpected native external node remains in the complete MetaSound graph.")
                        : TEXT("A requested native external node is missing from the complete MetaSound graph."),
            Intent ? Intent->SourcePointer : TEXT("/nodes"), TEXT("native_node_present"),
            MakeShared<FJsonValueBoolean>(bUnexpected), MakeShared<FJsonValueBoolean>(!bUnexpected),
            Node ? Node->Name.ToString() : Intent ? Intent->ClassName : FString());
        OutError.InternalContext = Id.ToString();
        return false;
      }
    }
    auto Matches = [](const auto& Edge, const FMetaSoundLinkIntent& Intent) {
      return Edge.FromNodeID == Intent.FromNode && Edge.FromVertexID == Intent.FromPin
          && Edge.ToNodeID == Intent.ToNode && Edge.ToVertexID == Intent.ToPin;
    };
    for (const auto& Intent : MetaLinks) {
      if (Graph.Edges.ContainsByPredicate([&](const auto& Edge) { return Matches(Edge, Intent); }) != Intent.bPresent)
        return Fail(TEXT("graph_connection_intent_not_retained"),
            Intent.bPresent ? TEXT("The requested MetaSound connection is missing after apply.") : TEXT("The removed MetaSound connection still exists after apply."),
            Intent.SourcePointer, TEXT("connection_present"), MakeShared<FJsonValueBoolean>(!Intent.bPresent), MakeShared<FJsonValueBoolean>(Intent.bPresent));
    }
    if (CompleteMetaLinkNodes.IsSet()) {
      for (const auto& Edge : Graph.Edges) {
        if (CompleteMetaLinkNodes->Contains(Edge.FromNodeID) && CompleteMetaLinkNodes->Contains(Edge.ToNodeID)
            && !MetaLinks.ContainsByPredicate([&](const auto& Intent) { return Intent.bPresent && Matches(Edge, Intent); }))
        {
          const auto* From = Graph.Nodes.FindByPredicate([&](const auto& Node) { return Node.GetID() == Edge.FromNodeID; });
          const auto* To = Graph.Nodes.FindByPredicate([&](const auto& Node) { return Node.GetID() == Edge.ToNodeID; });
          const auto* FromPin = From ? From->Interface.Outputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.FromVertexID; }) : nullptr;
          const auto* ToPin = To ? To->Interface.Inputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.ToVertexID; }) : nullptr;
          return Fail(TEXT("graph_connection_removal_intent_not_retained"),
              TEXT("A connection omitted from the complete MetaSound graph still exists after apply."), TEXT("/edges"), TEXT("connection_present"),
              MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
              (From ? From->Name.ToString() : TEXT("<missing node>")) + FString(TEXT(":")) + (FromPin ? FromPin->Name.ToString() : TEXT("<missing pin>"))
                  + TEXT(" -> ") + (To ? To->Name.ToString() : TEXT("<missing node>")) + TEXT(":") + (ToPin ? ToPin->Name.ToString() : TEXT("<missing pin>")));
        }
      }
    }
    if (!MetaDefaults.IsEmpty()) {
      const auto ClassName = Document.RootGraph.Metadata.GetClassName();
      if (!ClassName.IsValid()) return Fail(TEXT("metasound_class_identity_missing"),
          TEXT("The native MetaSound graph has no valid class identity for default-value readback."), FString(), TEXT("class_identity_valid"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      auto& Registry = Metasound::Engine::FDocumentBuilderRegistry::GetChecked();
      auto* ExistingBuilder = Registry.FindBuilderObject(&Asset);
      auto& Builder = ExistingBuilder ? *ExistingBuilder : Registry.FindOrBeginBuilding(Asset, true);
      const auto HintPath = Builder.GetBuilder().GetHintPath();
      // Use native effective-default semantics, with explicit page and no page
      // switch. A builder borrowed from the editor remains owned by the editor;
      // one needed only for this read is released before returning.
      const bool bMatches = [&]() {
        for (const auto& Intent : MetaDefaults) {
          FMetasoundFrontendLiteral Expected;
          if (!DecodeMetaSoundLiteral(Intent.Value, Expected, OutError)) {
            if (Intent.Source) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Intent.Source, TEXT("metasound_pin_default_verify"));
            return false;
          }
          const auto* Actual = Intent.bRequireInstanceOverride
              ? Builder.GetBuilder().GetNodeInputDefault(Intent.Node, Intent.Pin, &Metasound::Frontend::DefaultPageID)
              : FindMetaSoundEffectiveInputDefault(Builder, Intent.Node, Intent.Pin);
          if (!Actual || !Actual->IsEqual(Expected)) {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("metasound_pin_default_intent_not_retained"),
                Actual ? TEXT("The MetaSound input pin retained a different default.")
                       : Intent.bRequireInstanceOverride ? TEXT("The MetaSound input pin lost its required instance default override.")
                                                        : TEXT("The MetaSound input pin has no effective default after apply."),
                TEXT("metasound_pin_default_verify"));
            TSharedPtr<FJsonValue> ActualValue, ExpectedValue;
            if (Actual) if (const auto Value = EncodeMetaSoundLiteral(*Actual)) ActualValue = MakeShared<FJsonValueObject>(Value);
            if (const auto Value = EncodeMetaSoundLiteral(Expected)) ExpectedValue = MakeShared<FJsonValueObject>(Value);
            const auto Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(ExpectedValue, ActualValue);
            if (!Difference.bEquivalent) Difference.ApplyTo(OutError);
            else {
              if (Actual) OutError.PublicMessage += TEXT(" The public values encode identically, but the native literals do not match.");
              OutError.PropertyPath = Actual ? TEXT("native_value_matches_requested") : TEXT("default_present");
              OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
              OutError.ActualValuePresent = OutError.ExpectedValuePresent = true;
              OutError.ProposedValue = ExpectedValue;
            }
            if (Intent.Source) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Intent.Source, TEXT("metasound_pin_default_verify"));
            return false;
          }
        }
        return true;
      }();
      if (!ExistingBuilder && !Registry.FinishBuilding(ClassName, HintPath)) {
        auto ReadbackError = MoveTemp(OutError);
        Fail(TEXT("metasound_readback_builder_release_failed"),
            TEXT("The temporary MetaSound builder could not be released after reading node defaults."), FString(), TEXT("builder_released"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        if (!bMatches && !ReadbackError.IsEmpty()) OutError.Diagnostics.Add(MoveTemp(ReadbackError));
        return false;
      }
      if (!bMatches) return false;
    }
  }
  if (!SoundNodes.IsEmpty() || !SoundLinks.IsEmpty() || CompleteSoundNodes.IsSet()
      || !CompleteSoundChildren.IsEmpty() || SoundRoot.IsSet() || SoundOutputPosition.IsSet()) {
    auto* Cue = Cast<USoundCue>(&Asset);
    if (!Cue) return Fail(TEXT("sound_cue_not_found"),
        TEXT("The asset is no longer a SoundCue during graph verification."), FString(), TEXT("asset_class"),
        MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(USoundCue::StaticClass()->GetPathName()));
    if (SoundOutputPosition.IsSet()) {
      const auto* Output = FindSoundCueOutputNode(*Cue);
      if (!Output || FIntPoint(Output->NodePosX, Output->NodePosY) != SoundOutputPosition.GetValue())
        return FailPosition(TEXT("sound_cue_node_position_intent_not_retained"),
            !Output ? TEXT("The SoundCue output node is missing during position verification.") : TEXT("The SoundCue output retained a different position."),
            SoundOutputSourcePointer.IsEmpty() ? FString() : SoundOutputSourcePointer + TEXT("/pos"),
            Output != nullptr, Output ? Output->NodePosX : 0, Output ? Output->NodePosY : 0,
            SoundOutputPosition->X, SoundOutputPosition->Y, TEXT("sound_cue_root"));
    }
    auto Find = [&](FName Name) -> USoundNode* {
      auto* Node = Name.IsNone() ? nullptr : FindObject<USoundNode>(Cue, *Name.ToString());
      return Node && Cue->AllNodes.Contains(Node) ? Node : nullptr;
    };
    for (const auto& Intent : SoundNodes) {
      const auto* Node = Intent.Name.IsNone() ? nullptr : FindObject<USoundNode>(Cue, *Intent.Name.ToString());
      if ((Node != nullptr) != Intent.bPresent || (Node && !Cue->AllNodes.Contains(Node))) {
        const bool bPresenceMismatch = (Node != nullptr) != Intent.bPresent;
        return Fail(TEXT("sound_cue_node_intent_not_retained"),
            !bPresenceMismatch ? TEXT("The SoundCue node exists but is absent from the asset's node collection.")
                : Intent.bPresent ? TEXT("The requested SoundCue node is missing after apply.") : TEXT("The removed SoundCue node still exists after apply."),
            Intent.SourcePointer, bPresenceMismatch ? TEXT("node_present") : TEXT("node_in_collection"),
            MakeShared<FJsonValueBoolean>(bPresenceMismatch && Node != nullptr), MakeShared<FJsonValueBoolean>(bPresenceMismatch ? Intent.bPresent : true), Intent.Name.ToString());
      }
      if (!Node) continue;
      if (!Intent.ClassPath.IsEmpty()) {
        UClass* Expected = Intent.bExactClass ? nullptr : LoadObject<UClass>(nullptr, *Intent.ClassPath);
        if (Intent.bExactClass ? Node->GetClass()->GetPathName() != Intent.ClassPath : (!Expected || !Node->IsA(Expected)))
          return Fail(TEXT("sound_cue_node_class_intent_not_retained"),
              !Intent.bExactClass && !Expected ? TEXT("The requested SoundCue node base class could not be loaded during verification.")
                  : Intent.bExactClass ? TEXT("The SoundCue node retained a different class.")
                                       : TEXT("The SoundCue node does not derive from the requested class."),
              Intent.SourcePointer, TEXT("class_path"), MakeShared<FJsonValueString>(Node->GetClass()->GetPathName()),
              MakeShared<FJsonValueString>(Intent.ClassPath), Intent.Name.ToString());
      }
      if (Intent.Position.IsSet()) {
        const auto* GraphNode = Node->GetGraphNode();
        if (!GraphNode || GraphNode->NodePosX != Intent.Position->X || GraphNode->NodePosY != Intent.Position->Y)
          return FailPosition(TEXT("sound_cue_node_position_intent_not_retained"),
              !GraphNode ? TEXT("The SoundCue node has no editor graph node during position verification.") : TEXT("The SoundCue node retained a different position."),
              Intent.SourcePointer.IsEmpty() ? FString() : Intent.SourcePointer + TEXT("/pos"),
              GraphNode != nullptr, GraphNode ? GraphNode->NodePosX : 0, GraphNode ? GraphNode->NodePosY : 0,
              Intent.Position->X, Intent.Position->Y, Intent.Name.ToString());
      }
    }
    if (CompleteSoundNodes.IsSet()) {
      TSet<FName> Actual;
      for (const USoundNode* Node : Cue->AllNodes) if (Node) Actual.Add(Node->GetFName());
      if (Actual.Num() != CompleteSoundNodes->Num() || Actual.Difference(*CompleteSoundNodes).Num()) {
        const auto Unexpected = Actual.Difference(*CompleteSoundNodes);
        const bool bUnexpected = !Unexpected.IsEmpty();
        const auto Different = bUnexpected ? Unexpected : CompleteSoundNodes->Difference(Actual);
        const FName Name = *Different.CreateConstIterator();
        const auto* Intent = SoundNodes.FindByPredicate([&](const auto& Item) { return Item.Name == Name; });
        return Fail(TEXT("sound_cue_node_collection_intent_not_retained"),
            bUnexpected ? TEXT("An unexpected SoundCue node remains after applying the complete graph.")
                        : TEXT("A requested SoundCue node is missing from the complete graph."),
            Intent ? Intent->SourcePointer : TEXT("/nodes"), TEXT("node_present"),
            MakeShared<FJsonValueBoolean>(bUnexpected), MakeShared<FJsonValueBoolean>(!bUnexpected), Name.ToString());
      }
    }
    for (const auto& Intent : SoundLinks) {
      const auto* Parent = Find(Intent.Parent);
      const bool bExact = Parent && Parent->ChildNodes.IsValidIndex(Intent.Order)
          && Parent->ChildNodes[Intent.Order] && Parent->ChildNodes[Intent.Order]->GetFName() == Intent.Child;
      const bool bPresent = Parent && Parent->ChildNodes.ContainsByPredicate([&](const USoundNode* Child) {
        return Child && Child->GetFName() == Intent.Child;
      });
      if (Intent.bPresent ? !bExact : bPresent) {
        if (!Parent) return Fail(TEXT("sound_cue_relation_intent_not_retained"),
            TEXT("The parent of the requested SoundCue relation is missing."), Intent.SourcePointer, TEXT("parent_present"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Intent.Parent.ToString());
        TSharedPtr<FJsonValue> ActualChild;
        if (!Intent.bPresent) ActualChild = MakeShared<FJsonValueString>(Intent.Child.ToString());
        else if (Parent->ChildNodes.IsValidIndex(Intent.Order) && Parent->ChildNodes[Intent.Order])
          ActualChild = MakeShared<FJsonValueString>(Parent->ChildNodes[Intent.Order]->GetName());
        return Fail(TEXT("sound_cue_relation_intent_not_retained"),
            Intent.bPresent ? FString::Printf(TEXT("SoundCue input slot %d did not retain the requested child node."), Intent.Order)
                            : TEXT("The removed SoundCue child relation still exists."),
            Intent.SourcePointer, FString::Printf(TEXT("child_slots[%d]"), Intent.Order), ActualChild,
            Intent.bPresent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Intent.Child.ToString())) : nullptr, Intent.Parent.ToString());
      }
    }
    for (const auto& Pair : CompleteSoundChildren) {
      const auto* Parent = Find(Pair.Key);
      if (!Parent || Parent->ChildNodes.Num() != Pair.Value.Num())
        return Fail(TEXT("sound_cue_relation_collection_intent_not_retained"),
            !Parent ? TEXT("The parent node is missing while verifying its complete SoundCue input collection.")
                    : TEXT("The SoundCue node retained a different number of input slots."),
            TEXT("/edges"), TEXT("input_slot_count"),
            Parent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Parent->ChildNodes.Num())) : nullptr,
            MakeShared<FJsonValueNumber>(Pair.Value.Num()), Pair.Key.ToString());
      for (int32 Index = 0; Index < Pair.Value.Num(); ++Index) {
        const FName Actual = Parent->ChildNodes[Index] ? Parent->ChildNodes[Index]->GetFName() : NAME_None;
        if (Actual != Pair.Value[Index]) return Fail(TEXT("sound_cue_relation_order_intent_not_retained"),
            FString::Printf(TEXT("SoundCue input slot %d retained a different child node."), Index), TEXT("/edges"), FString::Printf(TEXT("child_slots[%d]"), Index),
            Actual.IsNone() ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Actual.ToString())),
            Pair.Value[Index].IsNone() ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pair.Value[Index].ToString())), Pair.Key.ToString());
      }
    }
    if (SoundRoot.IsSet() && (Cue->FirstNode ? Cue->FirstNode->GetFName() : NAME_None) != SoundRoot.GetValue())
      return Fail(TEXT("sound_cue_root_intent_not_retained"),
          TEXT("The SoundCue output retained a different root node."), TEXT("/graph/node_references/root"), TEXT("root"),
          Cue->FirstNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Cue->FirstNode->GetName())) : nullptr,
          SoundRoot->IsNone() ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(SoundRoot->ToString())));
  }
  return true;
}
} // namespace UeAgentAudioAuthoring
