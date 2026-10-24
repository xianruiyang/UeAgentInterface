#include "Composition/GraphCompound/UeAgentGraphCompoundDeformerProduction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"

#include "Animation/MeshDeformerCollection.h"
#include "Animation/MeshDeformer.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodeLink.h"
#include "OptimusNodePin.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace DomainAdapters::AnimationRigGraph;
	using namespace StateSemantic;

	namespace
	{
		Contract::FDiagnostic SemanticDeformerFailure(const FDeformerSemanticGraphEdit& Edit,
			const UOptimusDeformer& Asset, const TCHAR* Code, const FString& Reason,
			const FString& InputPointer = FString(), const FString& Field = FString(),
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"));
			Error.TargetPath = Edit.TargetPath.IsEmpty() ? Asset.GetPathName() : Edit.TargetPath;
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			if (!InputPointer.IsEmpty())
			{
				Error.Source.SourceUnit = Edit.SourceUnit;
				Error.Source.InternalJsonPath = InputPointer == TEXT("/") && !Field.IsEmpty() ? Field : InputPointer + Field;
				Error.SourceKind = TEXT("related_input");
				if (Edit.LocateDiagnostic) Edit.LocateDiagnostic(Error);
			}
			return Error;
		}

		TSharedPtr<FJsonValue> DeformerPositionValue(const FVector2D& Position)
		{
			const auto Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("x"), Position.X);
			Value->SetNumberField(TEXT("y"), Position.Y);
			return MakeShared<FJsonValueObject>(Value);
		}

		FDeformerSemanticGraphEdit::FConnection ObserveDeformerConnection(
			const UOptimusNodePin* From, const UOptimusNodePin* To)
		{
			FDeformerSemanticGraphEdit::FConnection Connection;
			if (From)
			{
				if (const auto* Node = From->GetOwningNode()) Connection.FromNode = Node->GetName();
				Connection.FromPin = From->GetUniqueName().ToString();
			}
			if (To)
			{
				if (const auto* Node = To->GetOwningNode()) Connection.ToNode = Node->GetName();
				Connection.ToPin = To->GetUniqueName().ToString();
			}
			return Connection;
		}

		TSharedPtr<FJsonValue> DeformerConnectionValue(const FDeformerSemanticGraphEdit::FConnection& Connection)
		{
			const auto Value = MakeShared<FJsonObject>();
			const auto From = MakeShared<FJsonObject>();
			From->SetStringField(TEXT("node"), Connection.FromNode);
			From->SetStringField(TEXT("pin"), Connection.FromPin);
			const auto To = MakeShared<FJsonObject>();
			To->SetStringField(TEXT("node"), Connection.ToNode);
			To->SetStringField(TEXT("pin"), Connection.ToPin);
			Value->SetObjectField(TEXT("from"), From);
			Value->SetObjectField(TEXT("to"), To);
			return MakeShared<FJsonValueObject>(Value);
		}
	}

	static UOptimusNodePin* ResolveSemanticPin(UOptimusNodeGraph& Graph,
		const TMap<FString, FName>& NodeBindings, const FString& Identity, const FString& PinName)
	{
		const FName* Name = NodeBindings.Find(Identity);
		if (!Name) return nullptr;
		for (UOptimusNode* Node : Graph.GetAllNodes())
			if (Node && Node->GetFName() == *Name) return Node->FindPin(PinName);
		return nullptr;
	}

	bool TryApplyDeformerSemanticGraph(UOptimusDeformer& Asset,
		const FDeformerSemanticGraphEdit& Edit, TMap<FString, FName>& OutNodeBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutNodeBindings.Reset();
		OutError = {};
		UOptimusNodeGraph* Graph = Asset.GetUpdateGraph();
		if (!Graph)
		{
			OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_update_graph_missing"),
				TEXT("The Deformer update graph is unavailable for node editing."));
			return false;
		}
		if (Edit.ConnectionContext && Edit.ConnectionContext->bHasResolvedIntent)
		{
			TMap<FString, FName> ExistingBindings;
			for (const auto& Node : Edit.Nodes)
				if (!Node.ExistingName.IsNone()) ExistingBindings.Add(Node.Identity, Node.ExistingName);
			TArray<TPair<UOptimusNodePin*, UOptimusNodePin*>> ContextLinks;
			for (const auto& Connection : Edit.Connections)
				if (Edit.ConnectionContext->IsUnchanged(Connection.FromNode, Connection.FromPin, Connection.ToNode, Connection.ToPin))
					ContextLinks.Emplace(ResolveSemanticPin(*Graph, ExistingBindings, Connection.FromNode, Connection.FromPin),
						ResolveSemanticPin(*Graph, ExistingBindings, Connection.ToNode, Connection.ToPin));
			// Clear authored removals while captured pin identities still exist.
			// Native node/property edits own subsequent rename and compatibility changes.
			const auto ExistingLinks = Graph->GetAllLinks();
			for (const UOptimusNodeLink* Link : ExistingLinks)
			{
				if (ContextLinks.ContainsByPredicate([Link](const auto& Pair) {
					return Pair.Key == Link->GetNodeOutputPin() && Pair.Value == Link->GetNodeInputPin(); })) continue;
				UOptimusNodePin* FromPin = Link->GetNodeOutputPin();
				UOptimusNodePin* ToPin = Link->GetNodeInputPin();
				const auto Observed = ObserveDeformerConnection(FromPin, ToPin);
				if (!Graph->RemoveLink(FromPin, ToPin))
				{
					const auto* Removed = Edit.RemovedConnections.FindByPredicate([&](const auto& Connection) {
						return FromPin && ToPin && ResolveSemanticPin(*Graph, ExistingBindings, Connection.FromNode, Connection.FromPin) == FromPin
							&& ResolveSemanticPin(*Graph, ExistingBindings, Connection.ToNode, Connection.ToPin) == ToPin;
					});
					OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_connection_remove_failed"),
						TEXT("The native graph could not remove the connection."), Removed ? Removed->InputPointer : FString());
					OutError.ProposedValue = DeformerConnectionValue(Observed);
					return false;
				}
			}
		}
		for (const FDeformerSemanticNodeEdit& NodeEdit : Edit.Nodes)
		{
			UOptimusNode* Node = nullptr;
			if (!NodeEdit.ExistingName.IsNone())
			{
				for (UOptimusNode* Existing : Graph->GetAllNodes())
					if (Existing && Existing->GetFName() == NodeEdit.ExistingName) { Node = Existing; break; }
				if (!Node)
				{
					OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_existing_node_missing"),
						TEXT("The requested existing node was not found in the Deformer update graph."), NodeEdit.InputPointer, TEXT("/node_name"),
						MakeShared<FJsonValueString>(NodeEdit.ExistingName.ToString()), MakeShared<FJsonValueString>(TEXT("existing node")));
					return false;
				}
			}
			if (NodeEdit.bRemove)
			{
				if (!Node || !Graph->RemoveNode(Node))
				{
					OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_remove_node_failed"),
						!Node ? TEXT("The node to remove is unavailable.") : TEXT("The native graph could not remove the node."),
						NodeEdit.InputPointer, TEXT("/operation"), MakeShared<FJsonValueString>(NodeEdit.ExistingName.ToString()));
					return false;
				}
				continue;
			}
			if (NodeEdit.CreateClass)
			{
				const FVector2D Initial = NodeEdit.CreateClass->GetDefaultObject<UOptimusNode>()->GetGraphPosition();
				// AddNode configures position before insertion, and native configuration
				// rejects an unchanged position. Complete creation and requested layout
				// through the two public operations when the initial position is requested.
				const FVector2D CreatePosition = NodeEdit.Position.Equals(Initial)
					? Initial + FVector2D(1.0, 0.0) : NodeEdit.Position;
				Node = Graph->AddNode(NodeEdit.CreateClass, CreatePosition);
				if (!Node)
				{
					OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_add_node_failed"),
						TEXT("The native graph could not create the requested node."), NodeEdit.InputPointer, TEXT("/candidate"),
						nullptr, MakeShared<FJsonValueString>(NodeEdit.CreateClass->GetPathName()));
					return false;
				}
				if (!Node->GetGraphPosition().Equals(NodeEdit.Position) && !Node->SetGraphPosition(NodeEdit.Position))
				{
					OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_created_node_position_failed"),
						TEXT("The new node could not be moved to its requested position."), NodeEdit.InputPointer,
						NodeEdit.bSetPosition ? TEXT("/pos") : FString(),
						DeformerPositionValue(Node->GetGraphPosition()), DeformerPositionValue(NodeEdit.Position));
					return false;
				}
			}
			else if (NodeEdit.bSetPosition && (!Node || (!Node->GetGraphPosition().Equals(NodeEdit.Position)
				&& !Node->SetGraphPosition(NodeEdit.Position))))
			{
				OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_move_node_failed"),
					!Node ? TEXT("The node to move is unavailable.") : TEXT("The node could not be moved to its requested position."),
					NodeEdit.InputPointer, TEXT("/pos"), Node ? DeformerPositionValue(Node->GetGraphPosition()) : nullptr,
					DeformerPositionValue(NodeEdit.Position));
				return false;
			}
			if (Node) OutNodeBindings.Add(NodeEdit.Identity, Node->GetFName());
		}
		return true;
	}

	bool TryApplyDeformerSemanticConnections(UOptimusDeformer& Asset,
		const FDeformerSemanticGraphEdit& Edit, const TMap<FString, FName>& NodeBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutError = {};
		UOptimusNodeGraph* Graph = Asset.GetUpdateGraph();
		if (!Graph)
		{
			OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_update_graph_missing"),
				TEXT("The Deformer update graph is unavailable for connection editing."));
			return false;
		}
		auto Resolve = [&](const FString& Identity, const FString& PinName) -> UOptimusNodePin*
		{
			return ResolveSemanticPin(*Graph, NodeBindings, Identity, PinName);
		};
		const bool bHasConnectionIntent = Edit.ConnectionContext && Edit.ConnectionContext->bHasResolvedIntent;
		auto IsContext = [&](const FDeformerSemanticGraphEdit::FConnection& Connection) {
			return bHasConnectionIntent && Edit.ConnectionContext->IsUnchanged(
				Connection.FromNode, Connection.FromPin, Connection.ToNode, Connection.ToPin);
		};
		TArray<TPair<UOptimusNodePin*, UOptimusNodePin*>> Desired;
		for (const auto& Connection : Edit.Connections)
		{
			if (IsContext(Connection)) continue;
			UOptimusNodePin* From = Resolve(Connection.FromNode, Connection.FromPin);
			UOptimusNodePin* To = Resolve(Connection.ToNode, Connection.ToPin);
			if (!From || !To || From->GetDirection() != EOptimusNodePinDirection::Output
				|| To->GetDirection() != EOptimusNodePinDirection::Input)
			{
				const bool bFrom = !From || (To && From->GetDirection() != EOptimusNodePinDirection::Output);
				const auto* Pin = bFrom ? From : To;
				const FString& Identity = bFrom ? Connection.FromNode : Connection.ToNode;
				const FName* Name = NodeBindings.Find(Identity);
				const bool bNodeExists = Name && Graph->GetAllNodes().ContainsByPredicate([Name](const auto* Node) { return Node && Node->GetFName() == *Name; });
				const FString Field = (bFrom ? FString(TEXT("/from")) : FString(TEXT("/to"))) + (!bNodeExists ? TEXT("/node") : TEXT("/pin"));
				OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_connection_endpoint_invalid"),
					!bNodeExists ? TEXT("The connection refers to a node that is not available in the graph.")
						: !Pin ? TEXT("The connection pin was not found on its node.") : TEXT("The connection pin has the wrong direction."),
					Connection.InputPointer, Field, nullptr,
					MakeShared<FJsonValueString>(!bNodeExists ? TEXT("existing node") : !Pin ? TEXT("existing pin") : bFrom ? TEXT("output pin") : TEXT("input pin")));
				OutError.ProposedValue = DeformerConnectionValue(Connection);
				if (Pin) OutError.ActualValue = MakeShared<FJsonValueString>(Pin->GetDirection() == EOptimusNodePinDirection::Input ? TEXT("input")
					: Pin->GetDirection() == EOptimusNodePinDirection::Output ? TEXT("output") : FString::FromInt(static_cast<int32>(Pin->GetDirection())));
				return false;
			}
			Desired.Emplace(From, To);
		}
		TArray<TPair<UOptimusNodePin*, UOptimusNodePin*>> Remove;
		for (const UOptimusNodeLink* Link : Graph->GetAllLinks())
		{
			if (bHasConnectionIntent) break;
			if (!Desired.ContainsByPredicate([Link](const auto& Pair)
				{ return Pair.Key == Link->GetNodeOutputPin() && Pair.Value == Link->GetNodeInputPin(); }))
				Remove.Emplace(Link->GetNodeOutputPin(), Link->GetNodeInputPin());
		}
		for (const auto& Pair : Remove)
		{
			const auto Observed = ObserveDeformerConnection(Pair.Key, Pair.Value);
			if (!Graph->RemoveLink(Pair.Key, Pair.Value))
			{
				OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_connection_remove_failed"),
					TEXT("The native graph could not remove an obsolete connection."));
				OutError.ProposedValue = DeformerConnectionValue(Observed);
				return false;
			}
		}
		for (const auto& Pair : Desired)
		{
			const bool bExists = Graph->GetAllLinks().ContainsByPredicate([&Pair](const UOptimusNodeLink* Link)
				{ return Pair.Key == Link->GetNodeOutputPin() && Pair.Value == Link->GetNodeInputPin(); });
			if (!bExists && !Graph->AddLink(Pair.Key, Pair.Value))
			{
				const auto* Connection = Edit.Connections.FindByPredicate([&](const auto& Candidate) {
					return !IsContext(Candidate) && Resolve(Candidate.FromNode, Candidate.FromPin) == Pair.Key
						&& Resolve(Candidate.ToNode, Candidate.ToPin) == Pair.Value;
				});
				OutError = SemanticDeformerFailure(Edit, Asset, TEXT("deformer_connection_add_failed"),
					TEXT("The native graph could not add the requested connection."), Connection ? Connection->InputPointer : FString());
				OutError.ProposedValue = DeformerConnectionValue(Connection ? *Connection : ObserveDeformerConnection(Pair.Key, Pair.Value));
				return false;
			}
		}
		// AddLink replaces existing links on a single input. Its successful
		// return cannot prove that all earlier requested links still exist.
		for (const auto& Connection : Edit.Connections)
		{
			if (IsContext(Connection)) continue;
			UOptimusNodePin* From = Resolve(Connection.FromNode, Connection.FromPin);
			UOptimusNodePin* To = Resolve(Connection.ToNode, Connection.ToPin);
			if (!From || !To || !Graph->GetAllLinks().ContainsByPredicate([&](const UOptimusNodeLink* Link)
				{ return Link && Link->GetNodeOutputPin() == From && Link->GetNodeInputPin() == To; }))
			{
				auto Warning = SemanticDeformerFailure(Edit, Asset, TEXT("graph_connection_intent_not_retained"),
					TEXT("The requested connection was not retained after native graph editing."), Connection.InputPointer,
					FString(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Warning.Stage = TEXT("deformer_connection_observation");
				Warning.ProposedValue = DeformerConnectionValue(Connection);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(MoveTemp(Warning));
			}
		}
		return true;
	}

	namespace
	{
		void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		void LocateDeformerInput(Contract::FDiagnostic& Error, const FString& Base,
			const FString& Relative = FString())
		{
			if (Base.IsEmpty()) return;
			Error.Source.JsonPath = Base == TEXT("/") && !Relative.IsEmpty() ? Relative : Base + Relative;
			Error.SourceKind = TEXT("related_input");
		}

		Contract::FDiagnostic DeformerFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const TSharedPtr<FJsonValue>& Actual = nullptr,
			const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			return Error;
		}

		Contract::FDiagnostic DeformerInputFailure(const FGraphCompoundAnimationOperationRequest& Request,
			const FGraphCompoundCanonicalEdit& Edit, const TCHAR* Code, const FString& Reason,
			const FString& Relative, const FUeAgentCanonicalValue* Actual, const FString& Expected)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.TargetPath = Request.GetTargetRef();
			Error.SourceKind = TEXT("command_input");
			Error.SemanticId = Edit.GetIdentity().GetSemanticId();
			Error.PropertyPath = Relative;
			Error.ActualValuePresent = Actual != nullptr;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (Actual)
			{
				Contract::FDiagnostic ProjectionError;
				if (!CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
					Error.Append(ProjectionError);
			}
			if (const FString* Base = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
				LocateDeformerInput(Error, *Base, Relative);
			return Error;
		}

		bool RequireDeformerString(const FGraphCompoundAnimationOperationRequest& Request,
			const FGraphCompoundCanonicalEdit& Edit, const FUeAgentCanonicalValue* Object,
			const FString& Field, FString& OutValue, const TCHAR* Code,
			FGraphCompoundRuntimeError& OutError, const FString& Parent = FString())
		{
			const bool bObject = Object && Object->GetKind() == EUeAgentValueKind::Object;
			const FUeAgentCanonicalValue* Value = bObject ? FindField(*Object, Field) : Object;
			if (!bObject || !Value || Value->GetKind() != EUeAgentValueKind::String || Value->GetText().IsEmpty())
			{
				OutError = DeformerInputFailure(Request, Edit, Code,
					!bObject ? TEXT("The proposed value must be an object.")
						: FString::Printf(TEXT("Field '%s' %s."), *Field,
							Value ? TEXT("must be a non-empty string") : TEXT("is missing")),
					bObject ? Parent + TEXT("/") + Field : Parent, Value,
					bObject ? TEXT("non-empty string") : TEXT("object"));
				return false;
			}
			OutValue = Value->GetText();
			return true;
		}

		bool TryIntegerField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value = FindField(Object, Name);
			if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			int64 Number = 0;
			if (!TryParseInt64Number(Value->GetLexeme(), Number)
				|| Number < 0 || Number > MAX_int32)
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		FString EntryRef(const FGraphCompoundCanonicalEdit& Edit)
		{
			const FString Prefix = TEXT("item:collection_entry:");
			const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
			return SemanticId.StartsWith(Prefix)
				? SemanticId.Mid(Prefix.Len()) : FString();
		}

		FString ObjectPath(const FString& TargetRef)
		{
			if (TargetRef.Contains(TEXT(".")))
			{
				return TargetRef;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(TargetRef);
			return TargetRef + TEXT(".") + AssetName;
		}

		bool TryValidateCollectionAcyclic(
			UMeshDeformerCollection& Root,
			const TArray<FGraphCompoundDeformerEntrySpec>& Entries,
			FGraphCompoundRuntimeError& OutError)
		{
			struct FFrame final
			{
				UMeshDeformerCollection* Collection = nullptr;
				int32 NextChild = 0;
			};
			TSet<const UMeshDeformerCollection*> Visiting;
			TSet<const UMeshDeformerCollection*> Visited;
			TArray<FFrame> Stack;
			Stack.Add({&Root, 0});
			Visiting.Add(&Root);
			while (!Stack.IsEmpty())
			{
				FFrame& Frame = Stack.Last();
				if (Frame.NextChild >= Frame.Collection->MeshDeformerCollections.Num())
				{
					Visiting.Remove(Frame.Collection);
					Visited.Add(Frame.Collection);
					Stack.Pop(EAllowShrinking::No);
					continue;
				}
				UMeshDeformerCollection* const Child =
					Frame.Collection->MeshDeformerCollections[Frame.NextChild++];
				if (!Child)
				{
					OutError = DeformerFailure(TEXT("animation_rig_graph_collection_entry_invalid"),
						FString::Printf(TEXT("Child collection at index %d in '%s' is null."),
							Frame.NextChild - 1, *Frame.Collection->GetPathName()), Root.GetPathName(),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(UMeshDeformerCollection::StaticClass()->GetPathName()));
					OutError.PropertyPath = FString::Printf(TEXT("/MeshDeformerCollections/%d"), Frame.NextChild - 1);
					return false;
				}
				if (Visiting.Contains(Child))
				{
					TArray<TSharedPtr<FJsonValue>> Cycle;
					bool bInCycle = false;
					for (const FFrame& CycleFrame : Stack)
					{
						bInCycle |= CycleFrame.Collection == Child;
						if (bInCycle) Cycle.Add(MakeShared<FJsonValueString>(CycleFrame.Collection->GetPathName()));
					}
					Cycle.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
					OutError = DeformerFailure(TEXT("animation_rig_graph_collection_cycle"),
						TEXT("The child collection references form a cycle."), Root.GetPathName(),
						MakeShared<FJsonValueArray>(MoveTemp(Cycle)), MakeShared<FJsonValueString>(TEXT("acyclic collection references")));
					OutError.PropertyPath = FString::Printf(TEXT("/MeshDeformerCollections/%d"), Frame.NextChild - 1);
					// This is the authored root dependency leading to the observed cycle,
					// not a claim that the native closing edge is an input field.
					const auto* RootChild = Stack.Num() > 1 ? Stack[1].Collection : Child;
					const auto* Entry = Entries.FindByPredicate([RootChild](const auto& Candidate) {
						return Candidate.Intent != EChangeIntent::Delete && Candidate.EntryRef == RootChild->GetPathName();
					});
					if (Entry) LocateDeformerInput(OutError, Entry->InputPointer);
					return false;
				}
				if (!Visited.Contains(Child))
				{
					Visiting.Add(Child);
					Stack.Add({Child, 0});
				}
			}
			return true;
		}

		TArray<const FGraphCompoundDeformerEntrySpec*> OrderedPresentEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			const FString& Kind,
			TSet<FString>& OutMentioned)
		{
			TArray<const FGraphCompoundDeformerEntrySpec*> Present;
			OutMentioned.Reset();
			for (const FGraphCompoundDeformerEntrySpec& Spec : Specs)
			{
				if (Spec.EntryKind != Kind)
				{
					continue;
				}
				OutMentioned.Add(Spec.EntryRef);
				if (Spec.Intent != EChangeIntent::Delete)
				{
					Present.Add(&Spec);
				}
			}
			Present.Sort([](
				const FGraphCompoundDeformerEntrySpec& Left,
				const FGraphCompoundDeformerEntrySpec& Right)
			{
				return Left.RequestedOrderIndex == Right.RequestedOrderIndex
					? Left.EntryRef < Right.EntryRef
					: Left.RequestedOrderIndex < Right.RequestedOrderIndex;
			});

			return Present;
		}

		void ApplyOrderedDeformerEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			TArray<TSoftObjectPtr<UMeshDeformer>>& InOutEntries)
		{
			TSet<FString> Mentioned;
			const TArray<const FGraphCompoundDeformerEntrySpec*> Present =
				OrderedPresentEntries(Specs, TEXT("deformer"), Mentioned);
			TArray<TSoftObjectPtr<UMeshDeformer>> Result;
			for (const FGraphCompoundDeformerEntrySpec* Spec : Present)
			{
				Result.Add(LoadObject<UMeshDeformer>(nullptr, *Spec->EntryRef));
			}
			for (const TSoftObjectPtr<UMeshDeformer>& Existing : InOutEntries)
			{
				const UMeshDeformer* const Object = Existing.Get();
				if (Object && !Mentioned.Contains(Object->GetPathName()))
				{
					Result.Add(Existing);
				}
			}
			InOutEntries = MoveTemp(Result);
		}

		void ApplyOrderedCollectionEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			TArray<TObjectPtr<UMeshDeformerCollection>>& InOutEntries)
		{
			TSet<FString> Mentioned;
			const TArray<const FGraphCompoundDeformerEntrySpec*> Present =
				OrderedPresentEntries(Specs, TEXT("child_collection"), Mentioned);
			TArray<TObjectPtr<UMeshDeformerCollection>> Result;
			for (const FGraphCompoundDeformerEntrySpec* Spec : Present)
			{
				Result.Add(LoadObject<UMeshDeformerCollection>(
					nullptr, *Spec->EntryRef));
			}
			for (UMeshDeformerCollection* const Existing : InOutEntries)
			{
				if (Existing && !Mentioned.Contains(Existing->GetPathName()))
				{
					Result.Add(Existing);
				}
			}
			InOutEntries = MoveTemp(Result);
		}

		int32 ExpectedNativeIndex(
			const FGraphCompoundDeformerReadbackSpec& Spec,
			const FGraphCompoundDeformerEntrySpec& Entry)
		{
			if (Entry.Intent == EChangeIntent::Delete)
			{
				return INDEX_NONE;
			}
			int32 Index = 0;
			for (const FGraphCompoundDeformerEntrySpec& Candidate : Spec.Entries)
			{
				if (Candidate.EntryKind == Entry.EntryKind
					&& Candidate.Intent != EChangeIntent::Delete
					&& (Candidate.RequestedOrderIndex < Entry.RequestedOrderIndex
						|| (Candidate.RequestedOrderIndex == Entry.RequestedOrderIndex
							&& Candidate.EntryRef < Entry.EntryRef)))
				{
					++Index;
				}
			}
			return Index;
		}

		TSharedRef<FJsonObject> EntryJson(
			const FGraphCompoundDeformerEntrySpec& Entry,
			const int32 NativeIndex,
			const bool bPresent)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), Entry.EntryKind);
			Value->SetStringField(TEXT("entry_ref"), Entry.EntryRef);
			Value->SetNumberField(TEXT("native_order_index"), NativeIndex);
			Value->SetBoolField(TEXT("present"), bPresent);
			return Value;
		}
	}

	bool IsDeformerCreateProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id == TEXT("animation.deformer_graph.asset.create")
			|| Id == TEXT("animation.deformer_source_library.asset.create")
			|| Id == TEXT("animation.mesh_deformer_collection.asset.create")
			|| Id == TEXT("animation.ml_deformer.asset.create");
	}

	bool IsDeformerMutationProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id == TEXT("animation.deformer_graph.topology.apply")
			|| Id == TEXT("animation.deformer_source_library.bindings.apply")
			|| Id == TEXT("animation.mesh_deformer_collection.entries.apply")
			|| Id == TEXT("animation.ml_deformer.configuration.apply");
	}

	bool IsDeformerProductionProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		return IsDeformerCreateProfile(Profile)
			|| IsDeformerMutationProfile(Profile);
	}

	bool IsUnavailableConcreteDeformerProfile(
		const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id.StartsWith(TEXT("animation.deformer_source_library."))
			|| Id.StartsWith(TEXT("animation.ml_deformer."));
	}

	bool TrySetUnavailableConcreteDeformerError(
		const FPcgAnimationGraphRouteProfile& Profile,
		FGraphCompoundRuntimeError& OutError)
	{
		if (!IsUnavailableConcreteDeformerProfile(Profile))
		{
			return false;
		}
		SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_concrete_type_unavailable"), TEXT("No enabled plugin provides an adjudicated concrete native owner for this family."), TEXT("graph_compound"), FString()), Profile.GetCapabilityId());
		return true;
	}

	bool TryParseDeformerReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FGraphCompoundDeformerReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError)
	{
		OutSpec = {};
		OutSpec.TargetKind = Request.GetTargetKind();
		if (TrySetUnavailableConcreteDeformerError(Profile, OutError))
		{
			return false;
		}
		const TArray<FGraphCompoundCanonicalEdit>& Edits =
			Request.GetPayload().GetEdits();
		if (Edits.IsEmpty())
		{
			OutError = DeformerFailure(TEXT("graph_compound_deformer_plan_invalid"),
				TEXT("The Deformer operation requires at least one edit."), Request.GetTargetRef(),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
			OutError.Stage = TEXT("graph_compound");
			OutError.SourceKind = TEXT("command_input");
			return false;
		}

		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.deformer_graph.asset.create"))
		{
			OutSpec.Kind = EGraphCompoundDeformerReadbackKind::DeformerGraphCreate;
			OutSpec.NativeClassPath = UOptimusDeformer::StaticClass()->GetPathName();
			if (Edits.Num() != 1)
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_skeletal_mesh_invalid"),
					TEXT("Creating a Deformer Graph requires exactly one edit."), Request.GetTargetRef(),
					MakeShared<FJsonValueNumber>(Edits.Num()), MakeShared<FJsonValueNumber>(1));
				OutError.Stage = TEXT("graph_compound");
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			const auto& Edit = Edits[0];
			if (!RequireDeformerString(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("skeletal_mesh_ref"),
				OutSpec.SkeletalMeshRef, TEXT("graph_compound_deformer_skeletal_mesh_invalid"), OutError)) return false;
			if (const FString* Base = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
				OutSpec.SkeletalMeshInputPointer = *Base == TEXT("/") ? TEXT("/skeletal_mesh_ref") : *Base + TEXT("/skeletal_mesh_ref");
			if (!LoadObject<USkeletalMesh>(nullptr, *OutSpec.SkeletalMeshRef))
			{
				OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_skeletal_mesh_invalid"),
					TEXT("The referenced Skeletal Mesh could not be loaded."), TEXT("/skeletal_mesh_ref"),
					FindField(*Edit.GetProposedAfter(), TEXT("skeletal_mesh_ref")), USkeletalMesh::StaticClass()->GetPathName());
				return false;
			}
			return true;
		}
		if (Capability
			== TEXT("animation.mesh_deformer_collection.asset.create"))
		{
			OutSpec.Kind =
				EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionCreate;
			OutSpec.NativeClassPath =
				UMeshDeformerCollection::StaticClass()->GetPathName();
			if (Edits.Num() != 1)
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_plan_invalid"),
					TEXT("Creating a Mesh Deformer Collection requires exactly one edit."), Request.GetTargetRef(),
					MakeShared<FJsonValueNumber>(Edits.Num()), MakeShared<FJsonValueNumber>(1));
				OutError.Stage = TEXT("graph_compound");
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			return true;
		}
		if (Capability == TEXT("animation.deformer_graph.topology.apply"))
		{
			OutSpec.Kind = EGraphCompoundDeformerReadbackKind::DeformerGraphTopology;
			OutSpec.NativeClassPath = UOptimusDeformer::StaticClass()->GetPathName();
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				if (Edit.GetElementKind() == EGraphCompoundElementKind::Item)
				{
					FString NodeName;
					if (!RequireDeformerString(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("node_name"),
						NodeName, TEXT("graph_compound_deformer_node_identity_invalid"), OutError)) return false;
					OutSpec.CommentNodeName = *NodeName;
					if (const FString* Base = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
						OutSpec.CommentNodeInputPointer = *Base == TEXT("/") ? TEXT("/node_name") : *Base + TEXT("/node_name");
					continue;
				}
				if (Edit.GetElementKind() != EGraphCompoundElementKind::Relation
					|| !Edit.GetProposedAfter().IsValid())
				{
					OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_topology_edit_unsupported"),
						Edit.GetElementKind() != EGraphCompoundElementKind::Relation
							? TEXT("Deformer topology accepts only node items and connection relations.")
							: TEXT("The connection edit has no proposed value."), FString(), Edit.GetProposedAfter().Get(),
						TEXT("node item or connection relation with a proposed value"));
					return false;
				}
			const FUeAgentCanonicalValue* From = FindField(*Edit.GetProposedAfter(), TEXT("from"));
			const FUeAgentCanonicalValue* To = FindField(*Edit.GetProposedAfter(), TEXT("to"));
			FGraphCompoundDeformerLinkSpec Link;
			if (!From || !To)
			{
				const bool bObject = Edit.GetProposedAfter()->GetKind() == EUeAgentValueKind::Object;
				OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_link_fields_invalid"),
					!bObject ? TEXT("The connection value must be an object.")
						: !From ? TEXT("The connection is missing its 'from' endpoint.") : TEXT("The connection is missing its 'to' endpoint."),
					bObject ? (!From ? TEXT("/from") : TEXT("/to")) : FString(),
					bObject ? nullptr : Edit.GetProposedAfter().Get(), TEXT("object"));
				return false;
			}
			if (!RequireDeformerString(Request, Edit, From, TEXT("node"), Link.FromNode, TEXT("graph_compound_deformer_link_fields_invalid"), OutError, TEXT("/from"))
				|| !RequireDeformerString(Request, Edit, From, TEXT("pin"), Link.FromPin, TEXT("graph_compound_deformer_link_fields_invalid"), OutError, TEXT("/from"))
				|| !RequireDeformerString(Request, Edit, To, TEXT("node"), Link.ToNode, TEXT("graph_compound_deformer_link_fields_invalid"), OutError, TEXT("/to"))
				|| !RequireDeformerString(Request, Edit, To, TEXT("pin"), Link.ToPin, TEXT("graph_compound_deformer_link_fields_invalid"), OutError, TEXT("/to"))) return false;
			Link.Intent = Edit.GetIntent();
			if (const FString* Base = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
				Link.InputPointer = *Base;
			OutSpec.Links.Add(MoveTemp(Link));
			}
			if (OutSpec.CommentNodeName.IsNone() && OutSpec.Links.IsEmpty())
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_topology_edit_empty"),
					TEXT("The topology operation contains neither a named node nor a connection."), Request.GetTargetRef());
				OutError.Stage = TEXT("graph_compound");
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			return true;
		}

		OutSpec.Kind =
			EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionEntries;
		OutSpec.NativeClassPath =
			UMeshDeformerCollection::StaticClass()->GetPathName();
		TSet<FString> Seen;
		for (const FGraphCompoundCanonicalEdit& Edit : Edits)
		{
			FGraphCompoundDeformerEntrySpec Entry;
			Entry.EntryRef = EntryRef(Edit);
			Entry.Intent = Edit.GetIntent();
			if (const FString* Base = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
				Entry.InputPointer = *Base;
			if (Entry.EntryRef.IsEmpty())
			{
				OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_entry_invalid"),
					TEXT("The collection entry identity does not contain an asset reference."), FString(), nullptr,
					TEXT("collection entry asset reference"));
				return false;
			}
			if (Entry.Intent != EChangeIntent::Delete)
			{
				if (!RequireDeformerString(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("entry_kind"),
					Entry.EntryKind, TEXT("graph_compound_deformer_collection_entry_invalid"), OutError)) return false;
				if (!TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"), Entry.RequestedOrderIndex))
				{
					OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_entry_invalid"),
						TEXT("Field 'order_index' must be an integer from 0 through 2147483647."), TEXT("/order_index"),
						FindField(*Edit.GetProposedAfter(), TEXT("order_index")), TEXT("integer in [0, 2147483647]"));
					return false;
				}
			}
			if (Seen.Contains(Entry.EntryRef))
			{
				OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_entry_invalid"),
					TEXT("The collection contains more than one edit for the same entry reference."), FString(), nullptr,
					TEXT("unique entry reference"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Entry.EntryRef);
				OutError.ActualValuePresent = true;
				return false;
			}
			Seen.Add(Entry.EntryRef);
			if (Entry.Intent == EChangeIntent::Delete)
			{
				if (UObject* Existing = LoadObject<UObject>(nullptr, *Entry.EntryRef))
				{
					Entry.EntryKind = Existing->IsA<UMeshDeformerCollection>()
						? TEXT("child_collection") : TEXT("deformer");
				}
			}
			if (Entry.EntryKind == TEXT("deformer"))
			{
				if (!LoadObject<UMeshDeformer>(nullptr, *Entry.EntryRef))
				{
					OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_entry_invalid"),
						TEXT("The referenced Mesh Deformer could not be loaded."), FString(), nullptr,
						UMeshDeformer::StaticClass()->GetPathName());
					OutError.ActualValue = MakeShared<FJsonValueString>(Entry.EntryRef);
					OutError.ActualValuePresent = true;
					return false;
				}
			}
			else if (Entry.EntryKind == TEXT("child_collection"))
			{
				if (!LoadObject<UMeshDeformerCollection>(nullptr, *Entry.EntryRef))
				{
					OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_entry_invalid"),
						TEXT("The referenced child Mesh Deformer Collection could not be loaded."), FString(), nullptr,
						UMeshDeformerCollection::StaticClass()->GetPathName());
					OutError.ActualValue = MakeShared<FJsonValueString>(Entry.EntryRef);
					OutError.ActualValuePresent = true;
					return false;
				}
			}
			else
			{
				if (Entry.Intent == EChangeIntent::Delete)
				{
					OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_kind_invalid"),
						TEXT("The entry to delete could not be loaded to determine its kind."), FString(), nullptr,
						TEXT("loadable Mesh Deformer or Mesh Deformer Collection"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Entry.EntryRef);
					OutError.ActualValuePresent = true;
				}
				else OutError = DeformerInputFailure(Request, Edit, TEXT("graph_compound_deformer_collection_kind_invalid"),
					TEXT("The collection entry kind is unsupported."), TEXT("/entry_kind"),
					FindField(*Edit.GetProposedAfter(), TEXT("entry_kind")), TEXT("deformer or child_collection"));
				return false;
			}
			OutSpec.Entries.Add(MoveTemp(Entry));
		}
		return true;
	}

	bool TryCaptureDeformerValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FGraphCompoundDeformerCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError,
		bool bAuthoring,
		const FString& DiagnosticTargetPath)
	{
		OutValues.Reset();
		if (Profile.GetCapabilityId()
			== TEXT("animation.deformer_graph.topology.apply"))
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(&Target);
			const UOptimusNodeGraph* const Graph =
				Deformer ? Deformer->GetUpdateGraph() : nullptr;
			if (!Graph)
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_target_class_mismatch"),
					Deformer ? TEXT("The Deformer update graph is unavailable for capture.")
						: TEXT("The capture target is not an Optimus Deformer."),
					DiagnosticTargetPath.IsEmpty() ? Target.GetPathName() : DiagnosticTargetPath);
				if (!Deformer)
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UOptimusDeformer::StaticClass()->GetPathName());
				}
				return false;
			}
			for (const UOptimusNode* Node : Graph->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("node_name"), Node->GetName());
				if (bAuthoring)
				{
					Value->SetStringField(TEXT("id"), TEXT("item:") + Node->GetName());
					Value->SetStringField(TEXT("name"), Node->GetDisplayName().ToString());
					Value->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
					const auto Position = MakeShared<FJsonObject>();
					Position->SetNumberField(TEXT("x"), Node->GetGraphPosition().X);
					Position->SetNumberField(TEXT("y"), Node->GetGraphPosition().Y);
					Value->SetObjectField(TEXT("position"), Position);
				}
				TArray<TSharedPtr<FJsonValue>> Pins;
				TArray<UOptimusNodePin*> AllPins;
				for (UOptimusNodePin* RootPin : Node->GetPins())
					if (RootPin) AllPins.Append(RootPin->GetSubPinsRecursively(true));
				for (const UOptimusNodePin* Pin : AllPins)
				{
					if (!Pin || Pin->IsGroupingPin()) continue;
					const EOptimusNodePinDirection Direction = Pin->GetDirection();
					if (Direction != EOptimusNodePinDirection::Input
						&& Direction != EOptimusNodePinDirection::Output) continue;
					const TSharedRef<FJsonObject> PinValue = MakeShared<FJsonObject>();
					PinValue->SetStringField(TEXT("name"), Pin->GetUniqueName().ToString());
					PinValue->SetStringField(TEXT("direction"), Direction == EOptimusNodePinDirection::Input ? TEXT("input") : TEXT("output"));
					if (const FOptimusDataTypeHandle DataType = Pin->GetDataType(); DataType.IsValid())
					{
						PinValue->SetStringField(TEXT("type"), DataType->TypeName.ToString());
					}
					Pins.Add(MakeShared<FJsonValueObject>(PinValue));
				}
				if (bAuthoring)
				{
					const auto Interface = MakeShared<FJsonObject>();
					Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));
					Value->SetObjectField(TEXT("pin_interface"), Interface);
				}
				else Value->SetArrayField(TEXT("pin_interface"), MoveTemp(Pins));
				OutValues.Emplace(TEXT("item:") + Node->GetName(), Value);
			}
			for (const UOptimusNodeLink* Link : Graph->GetAllLinks())
			{
				const UOptimusNodePin* FromPin = Link ? Link->GetNodeOutputPin() : nullptr;
				const UOptimusNodePin* ToPin = Link ? Link->GetNodeInputPin() : nullptr;
				const UOptimusNode* FromNode = FromPin ? FromPin->GetOwningNode() : nullptr;
				const UOptimusNode* ToNode = ToPin ? ToPin->GetOwningNode() : nullptr;
				if (!FromPin || !ToPin || !FromNode || !ToNode) continue;
				const TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
				From->SetStringField(TEXT("node"), bAuthoring ? TEXT("item:") + FromNode->GetName() : FromNode->GetName());
				From->SetStringField(TEXT("pin"), FromPin->GetUniqueName().ToString());
				const TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
				To->SetStringField(TEXT("node"), bAuthoring ? TEXT("item:") + ToNode->GetName() : ToNode->GetName());
				To->SetStringField(TEXT("pin"), ToPin->GetUniqueName().ToString());
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetObjectField(TEXT("from"), From);
				Value->SetObjectField(TEXT("to"), To);
				OutValues.Emplace(TEXT("relation:") + FromNode->GetName() + TEXT(".")
					+ FromPin->GetUniqueName().ToString() + TEXT("->")
					+ ToNode->GetName() + TEXT(".") + ToPin->GetUniqueName().ToString(), Value);
			}
			return true;
		}

		const UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(&Target);
		if (!Collection)
		{
			OutError = DeformerFailure(TEXT("graph_compound_deformer_target_class_mismatch"),
				TEXT("The capture target is not a Mesh Deformer Collection."),
				DiagnosticTargetPath.IsEmpty() ? Target.GetPathName() : DiagnosticTargetPath,
				MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(UMeshDeformerCollection::StaticClass()->GetPathName()));
			return false;
		}
		for (int32 Index = 0; Index < Collection->MeshDeformers.Num(); ++Index)
		{
			const FString Ref =
				Collection->MeshDeformers[Index].ToSoftObjectPath().ToString();
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), TEXT("deformer"));
			Value->SetNumberField(TEXT("order_index"), Index);
			OutValues.Emplace(TEXT("item:collection_entry:") + Ref, Value);
		}
		for (int32 Index = 0;
			Index < Collection->MeshDeformerCollections.Num(); ++Index)
		{
			const UMeshDeformerCollection* const Child =
				Collection->MeshDeformerCollections[Index];
			if (!Child)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), TEXT("child_collection"));
			Value->SetNumberField(TEXT("order_index"), Index);
			OutValues.Emplace(
				TEXT("item:collection_entry:") + Child->GetPathName(), Value);
		}
		return true;
	}

	TSharedRef<FJsonObject> DeformerExpectedJson(
		const FGraphCompoundDeformerReadbackSpec& Spec)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		Root->SetStringField(TEXT("native_class"), Spec.NativeClassPath);
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			Root->SetStringField(TEXT("skeletal_mesh_ref"), Spec.SkeletalMeshRef);
			Root->SetBoolField(TEXT("primary_binding_present"), true);
		}
		else if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::DeformerGraphTopology)
		{
			Root->SetStringField(TEXT("node_name"), Spec.CommentNodeName.ToString());
			Root->SetBoolField(TEXT("present"), true);
		}
		else if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionEntries)
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
			{
				Entries.Add(MakeShared<FJsonValueObject>(EntryJson(
					Entry, ExpectedNativeIndex(Spec, Entry),
					Entry.Intent != EChangeIntent::Delete)));
			}
			Root->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		}
		return Root;
	}

	bool TryBuildDeformerActualJson(
		const FString& TargetRef,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		OutRoot = MakeShared<FJsonObject>();
		UObject* const Target = LoadObject<UObject>(nullptr, *ObjectPath(TargetRef));
		if (!Target || Target->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			OutError = DeformerFailure(TEXT("deformer_readback_target_class_mismatch"),
				Target ? TEXT("The Deformer readback target has an unexpected class.")
					: TEXT("The Deformer readback target could not be loaded."), TargetRef,
				nullptr, MakeShared<FJsonValueString>(Spec.NativeClassPath));
			OutError.ActualValuePresent = Target != nullptr;
			if (Target) OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
			return false;
		}
		OutRoot->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		OutRoot->SetStringField(TEXT("native_class"), Target->GetClass()->GetPathName());
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(Target);
			OutRoot->SetStringField(TEXT("skeletal_mesh_ref"),
				Deformer && Deformer->Mesh ? Deformer->Mesh->GetPathName() : FString());
			OutRoot->SetBoolField(TEXT("primary_binding_present"),
				Deformer && Deformer->GetComponentBindings().ContainsByPredicate(
					[](const UOptimusComponentSourceBinding* Binding)
					{
						return Binding && Binding->GetFName()
							== UOptimusComponentSourceBinding::GetPrimaryBindingName();
					}));
			return true;
		}
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphTopology)
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(Target);
			const UOptimusNodeGraph* const Graph =
				Deformer ? Deformer->GetUpdateGraph() : nullptr;
			const UOptimusNode* const* Found = Graph
				? Graph->GetAllNodes().FindByPredicate(
					[&Spec](const UOptimusNode* Node)
					{
						return Node && Node->GetFName() == Spec.CommentNodeName;
					}) : nullptr;
			OutRoot->SetStringField(TEXT("node_name"), Spec.CommentNodeName.ToString());
			OutRoot->SetBoolField(TEXT("present"), Found && *Found);
			return true;
		}
		if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionCreate)
		{
			return true;
		}
		const UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(Target);
		if (!Collection)
		{
			OutError = DeformerFailure(TEXT("deformer_readback_collection_missing"),
				TEXT("The readback target is not a Mesh Deformer Collection."), TargetRef,
				MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(UMeshDeformerCollection::StaticClass()->GetPathName()));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
		{
			int32 NativeIndex = INDEX_NONE;
			if (Entry.EntryKind == TEXT("deformer"))
			{
				NativeIndex = Collection->MeshDeformers.IndexOfByPredicate(
					[&Entry](const TSoftObjectPtr<UMeshDeformer>& Value)
					{
						return Value.ToSoftObjectPath().ToString() == Entry.EntryRef;
					});
			}
			else
			{
				NativeIndex = Collection->MeshDeformerCollections.IndexOfByPredicate(
					[&Entry](const UMeshDeformerCollection* Value)
					{
						return Value && Value->GetPathName() == Entry.EntryRef;
					});
			}
			Entries.Add(MakeShared<FJsonValueObject>(
				EntryJson(Entry, NativeIndex, NativeIndex != INDEX_NONE)));
		}
		OutRoot->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		return true;
	}

	void AddDeformerDependencyAnchors(
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors)
	{
		if (!Spec.SkeletalMeshRef.IsEmpty())
		{
			OutAnchors.Emplace(LoadObject<UObject>(nullptr, *Spec.SkeletalMeshRef));
		}
		for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
		{
			if (UObject* const Object = LoadObject<UObject>(nullptr, *Entry.EntryRef))
			{
				OutAnchors.Emplace(Object);
			}
		}
	}

	UObject* TryCreateDeformerAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError)
	{
		(void)Profile;
		const FName AssetName(*FPackageName::ObjectPathToObjectName(
			ObjectPath(Request.GetTargetRef())));
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			USkeletalMesh* const Mesh =
				LoadObject<USkeletalMesh>(nullptr, *Spec.SkeletalMeshRef);
			UOptimusDeformer* const Deformer = NewObject<UOptimusDeformer>(
				&Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Mesh || !Deformer)
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_factory_failed"),
					!Mesh ? TEXT("The Skeletal Mesh required to create the Deformer could not be loaded.")
						: TEXT("The Optimus Deformer object could not be created."), Request.GetTargetRef());
				if (!Mesh)
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(Spec.SkeletalMeshRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName());
					LocateDeformerInput(OutError, Spec.SkeletalMeshInputPointer);
				}
				return nullptr;
			}
			Deformer->Mesh = Mesh;
			if (!Deformer->AddComponentBinding(
					nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName()))
			{
				OutError = DeformerFailure(TEXT("animation_rig_graph_compile_incomplete"),
					TEXT("The Deformer primary component binding could not be added."), Request.GetTargetRef(),
					nullptr, MakeShared<FJsonValueString>(UOptimusComponentSourceBinding::GetPrimaryBindingName().ToString()));
				return nullptr;
			}
			if (!Deformer->Compile())
			{
				OutError = DeformerFailure(TEXT("animation_rig_graph_compile_incomplete"),
					TEXT("The newly created Deformer failed to compile."), Request.GetTargetRef());
				return nullptr;
			}
			return Deformer;
		}
		UMeshDeformerCollection* const Collection =
			NewObject<UMeshDeformerCollection>(
				&Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
		if (!Collection)
		{
			OutError = DeformerFailure(TEXT("graph_compound_deformer_factory_failed"),
				TEXT("The Mesh Deformer Collection object could not be created."), Request.GetTargetRef());
		}
		return Collection;
	}

	bool TryApplyDeformerMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError)
	{
		if (Profile.GetCapabilityId()
			== TEXT("animation.deformer_graph.topology.apply"))
		{
			UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(&Target);
			UOptimusNodeGraph* const Graph = Deformer
				? Deformer->GetUpdateGraph() : nullptr;
			if (!Graph)
			{
				OutError = DeformerFailure(TEXT("graph_compound_deformer_graph_unavailable"),
					Deformer ? TEXT("The Deformer update graph is unavailable for topology editing.")
						: TEXT("The topology target is not an Optimus Deformer."), Target.GetPathName());
				if (!Deformer)
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UOptimusDeformer::StaticClass()->GetPathName());
				}
				return false;
			}
			auto FindNode = [Graph](const FString& Ref) -> UOptimusNode*
			{
				for (UOptimusNode* Node : Graph->GetAllNodes())
				{
					if (Node && (Node->GetPathName() == Ref || Node->GetName() == Ref
						|| (Ref.StartsWith(TEXT("optimus_node:"))
							&& Node->GetPathName() == Ref.Mid(13))))
					{
						return Node;
					}
				}
				return nullptr;
			};
			auto FindPin = [](UOptimusNode* Node, const FString& Ref) -> UOptimusNodePin*
			{
				if (!Node) return nullptr;
				FString PinRef = Ref;
				if (PinRef.StartsWith(TEXT("optimus_pin:"))) PinRef = PinRef.Mid(12);
				if (UOptimusNodePin* Pin = Node->FindPin(PinRef)) return Pin;
				return Node->GetPinByName(FName(*PinRef));
			};
			for (const FGraphCompoundDeformerLinkSpec& Link : Spec.Links)
			{
				UOptimusNode* FromNode = FindNode(Link.FromNode);
				UOptimusNode* ToNode = FindNode(Link.ToNode);
				UOptimusNodePin* FromPin = FindPin(FromNode, Link.FromPin);
				UOptimusNodePin* ToPin = FindPin(ToNode, Link.ToPin);
				if (!FromPin || !ToPin || FromPin->GetDirection() != EOptimusNodePinDirection::Output
					|| ToPin->GetDirection() != EOptimusNodePinDirection::Input)
				{
					const bool bFrom = !FromPin || (ToPin && FromPin->GetDirection() != EOptimusNodePinDirection::Output);
					const UOptimusNode* Node = bFrom ? FromNode : ToNode;
					const UOptimusNodePin* Pin = bFrom ? FromPin : ToPin;
					const FString NodeRef = bFrom ? Link.FromNode : Link.ToNode;
					const FString PinRef = bFrom ? Link.FromPin : Link.ToPin;
					const FString Field = (bFrom ? TEXT("/from") : TEXT("/to")) + FString(!Node ? TEXT("/node") : TEXT("/pin"));
					OutError = DeformerFailure(TEXT("graph_compound_deformer_link_endpoint_invalid"),
						!Node ? FString::Printf(TEXT("Connection node '%s' was not found."), *NodeRef)
							: !Pin ? FString::Printf(TEXT("Pin '%s' was not found on node '%s'."), *PinRef, *NodeRef)
							: FString::Printf(TEXT("Pin '%s' on node '%s' must be an %s pin."), *PinRef, *NodeRef, bFrom ? TEXT("output") : TEXT("input")),
						Target.GetPathName(), MakeShared<FJsonValueString>(!Node ? NodeRef : !Pin ? PinRef
							: Pin->GetDirection() == EOptimusNodePinDirection::Input ? TEXT("input")
							: Pin->GetDirection() == EOptimusNodePinDirection::Output ? TEXT("output")
							: FString::FromInt(static_cast<int32>(Pin->GetDirection()))),
						MakeShared<FJsonValueString>(!Node ? TEXT("existing node") : !Pin ? TEXT("existing pin") : bFrom ? TEXT("output pin") : TEXT("input pin")));
					OutError.PropertyPath = Field;
					LocateDeformerInput(OutError, Link.InputPointer, Field);
					return false;
				}
				const bool bChanged = Link.Intent == EChangeIntent::Delete
					? Graph->RemoveLink(FromPin, ToPin)
					: Graph->AddLink(FromPin, ToPin);
				if (!bChanged && Link.Intent != EChangeIntent::Delete)
				{
					OutError = DeformerFailure(TEXT("graph_compound_deformer_link_apply_failed"),
						FString::Printf(TEXT("The connection from '%s.%s' to '%s.%s' could not be added."),
							*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin), Target.GetPathName());
					LocateDeformerInput(OutError, Link.InputPointer);
					return false;
				}
			}
			FAnimationRigGraphNativeMutationReceipt Apply;
			FAnimationRigGraphBarrierReceipt Barrier;
			FAnimationRigGraphTopology Topology;
			FString Evidence;
			FAnimationRigGraphAdapterError NativeError;
			const bool bCaptured = Hook.TryEnsureCompileAndCaptureDeformerGraph(
					*Deformer, Spec.CommentNodeName, Apply, Barrier,
					Topology, OutRevision, Evidence, NativeError, Spec.CommentNodeInputPointer);
			if (!bCaptured || !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
			{
				OutError = NativeError.IsValid() ? NativeError : DeformerFailure(TEXT("graph_compound_deformer_native_failed"),
					!bCaptured ? TEXT("Deformer topology capture did not complete.")
						: !Barrier.IsComplete() ? TEXT("The Deformer compile barrier did not complete.")
						: TEXT("The Deformer compile barrier reported errors."), Target.GetPathName());
				if (!NativeError.IsValid() && bCaptured && Barrier.GetErrorCount() != 0)
				{
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Barrier.GetErrorCount());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(0);
				}
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Target.GetPathName();
				return false;
			}
			return true;
		}

		UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(&Target);
		if (!Collection)
		{
			OutError = DeformerFailure(TEXT("graph_compound_deformer_target_class_mismatch"),
				TEXT("The entry mutation target is not a Mesh Deformer Collection."), Target.GetPathName(),
				MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(UMeshDeformerCollection::StaticClass()->GetPathName()));
			return false;
		}
		Collection->Modify();
		ApplyOrderedDeformerEntries(Spec.Entries, Collection->MeshDeformers);
		ApplyOrderedCollectionEntries(
			Spec.Entries, Collection->MeshDeformerCollections);
		if (!TryValidateCollectionAcyclic(*Collection, Spec.Entries, OutError))
		{
			return false;
		}
		FString Identity = Collection->GetPathName();
		for (const TSoftObjectPtr<UMeshDeformer>& Entry : Collection->MeshDeformers)
		{
			Identity += TEXT("|D:") + Entry.ToSoftObjectPath().ToString();
		}
		for (const UMeshDeformerCollection* Entry
			: Collection->MeshDeformerCollections)
		{
			Identity += TEXT("|C:")
				+ (Entry ? Entry->GetPathName() : FString(TEXT("null")));
		}
		OutRevision = TEXT("md5:") + FMD5::HashAnsiString(*Identity);
		return true;
	}
}
