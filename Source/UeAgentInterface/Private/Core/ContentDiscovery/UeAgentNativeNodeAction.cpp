#include "Core/ContentDiscovery/UeAgentNativeNodeAction.h"

#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "SNodePanel.h"
#include "ScopedTransaction.h"
#include "Settings/EditorStyleSettings.h"

namespace UE::UAI::ContentDiscovery::NativeNodeAction
{
	bool ReadBindings(const FBlueprintActionMenuItem& Item,
		const FBlueprintActionContext& Context,
		IBlueprintNodeBinder::FBindingSet& OutBindings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutBindings.Reset();
		auto* Spawner = const_cast<UBlueprintNodeSpawner*>(Item.GetRawAction());
		if (!Spawner) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_action_spawner_missing"), TEXT("The selected native Blueprint menu action has no node spawner."), TEXT("candidate_resolve")); OutError.CandidateName = Item.GetMenuDescription().ToString(); OutError.SourceKind = TEXT("native_result"); return false; }
		// UE exposes no binding accessor on the menu item. AppendBindings with an
		// empty set recomputes the UI spec using that exact item's full bindings.
		// Work on a copy; restore the native delegate before executing the action.
		const auto Original = Spawner->DynamicUiSignatureGetter;
		bool bObserved = false;
		Spawner->DynamicUiSignatureGetter = UBlueprintNodeSpawner::FUiSpecOverrideDelegate::CreateLambda(
			[&](const FBlueprintActionContext& UiContext,
				const IBlueprintNodeBinder::FBindingSet& Bindings, FBlueprintActionUiSpec* Spec)
			{
				if (!bObserved) { OutBindings = Bindings; bObserved = true; }
				Original.ExecuteIfBound(UiContext, Bindings, Spec);
			});
		ON_SCOPE_EXIT { Spawner->DynamicUiSignatureGetter = Original; };
		FBlueprintActionMenuItem Copy(Item);
		// AppendBindings rebuilds search data with an empty category, but does not
		// reset the category chain copied from the menu. Use the native updater
		// to clear that cache before it rebuilds search data a second time.
		Copy.CosmeticUpdateCategory(FText());
		Copy.AppendBindings(Context, IBlueprintNodeBinder::FBindingSet());
		if (!bObserved) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_action_native_bindings_unavailable"), TEXT("Refreshing the native Blueprint menu action did not invoke its binding delegate, so the action's bindings could not be read."), TEXT("candidate_resolve")); OutError.CandidateName = Item.GetMenuDescription().ToString(); OutError.SourceKind = TEXT("native_result"); return false; }
		OutError.Reset();
		return true;
	}

	namespace
	{
		bool LeadsTo(const UEdGraphNode& From, const UEdGraphNode& To,
			const EEdGraphPinDirection Direction)
		{
			TArray<const UEdGraphNode*> Pending{ &From };
			TSet<const UEdGraphNode*> Seen;
			while (!Pending.IsEmpty())
			{
				const UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
				if (Seen.Contains(Node)) continue;
				Seen.Add(Node);
				for (const UEdGraphPin* Pin : Node->Pins)
					if (Pin && Pin->Direction == Direction)
						for (const UEdGraphPin* Link : Pin->LinkedTo)
						{
							if (!Link) continue;
							const UEdGraphNode* LinkedNode = Link->GetOwningNode();
							if (LinkedNode == &To) return true;
							if (LinkedNode) Pending.Add(LinkedNode);
						}
			}
			return false;
		}

		void Autowire(UEdGraphPin& FromPin, TArray<UEdGraphNode*>& NewNodes)
		{
			// Retain native leading-node order without assuming all newly created
			// nodes are disconnected or recursing indefinitely through graph cycles.
			TArray<UEdGraphNode*> Ordered;
			while (!NewNodes.IsEmpty())
			{
				int32 Next = NewNodes.IndexOfByPredicate([&](const UEdGraphNode* Candidate)
					{
						return !NewNodes.ContainsByPredicate([&](const UEdGraphNode* Other)
							{ return Other != Candidate && LeadsTo(*Other, *Candidate, FromPin.Direction)
								&& !LeadsTo(*Candidate, *Other, FromPin.Direction); });
					});
				if (Next == INDEX_NONE) Next = 0;
				Ordered.Add(NewNodes[Next]);
				NewNodes.RemoveAt(Next, EAllowShrinking::No);
			}
			const int32 BeforeCount = FromPin.LinkedTo.Num();
			const UEdGraphPin* BeforeFirst = BeforeCount ? FromPin.LinkedTo[0] : nullptr;
			for (UEdGraphNode* Node : Ordered)
			{
				Node->AutowireNewNode(&FromPin);
				if (FromPin.LinkedTo.Num() && (FromPin.LinkedTo.Num() != BeforeCount
					|| FromPin.LinkedTo[0] != BeforeFirst)) break;
			}
		}

		void NotifyCreatedNode(UEdGraphNode& Node)
		{
			UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph());
			if (!Blueprint) return;
			if (Node.GetSchema()->MarkBlueprintDirtyFromNewNode(Blueprint, &Node)) return;
			const UK2Node* K2Node = Cast<UK2Node>(&Node);
			if (!K2Node || K2Node->NodeCausesStructuralBlueprintChange())
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			else
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
	}

	UEdGraphNode* Invoke(const UBlueprintNodeSpawner& Spawner, UEdGraph& Graph,
		UEdGraphPin* FromPin, const FVector2f& Location,
		const IBlueprintNodeBinder::FBindingSet& Bindings)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "CreateNativeCandidate", "Create Node"));
		FVector2f Position = Location;
		if (FromPin)
		{
			if (FromPin->Direction == EGPD_Input)
			{
				const float FromX = FromPin->GetOwningNode()->NodePosX;
				if (FMath::Abs(FromX - Position.X) < 60.f) Position.X = FromX - 60.f;
			}
			FromPin->Modify();
		}
		UEdGraphNode* LastNode = nullptr;
		auto Binding = Bindings.CreateConstIterator();
		do
		{
			IBlueprintNodeBinder::FBindingSet Subset;
			for (; Binding && (Spawner.CanBindMultipleObjects() || Subset.IsEmpty()); ++Binding)
				if (Binding->IsValid()) Subset.Add(*Binding);
			const TSet<UEdGraphNode*> Before(Graph.Nodes);
			LastNode = Spawner.Invoke(&Graph, Subset, FVector2D(Position));
			if (!LastNode) return nullptr;
			TArray<UEdGraphNode*> Created = TSet<UEdGraphNode*>(Graph.Nodes).Difference(Before).Array();
			const bool bNewNode = !Before.Contains(LastNode) && Graph.Nodes.Contains(LastNode);
			if (bNewNode) LastNode->SnapToGrid(SNodePanel::GetSnapGridSize());
			// As in UE's menu, wire before the notification can compile the Blueprint.
			if (FromPin) Autowire(*FromPin, Created);
			if (bNewNode) NotifyCreatedNode(*LastNode);
			Position.Y += UEdGraphSchema_K2::EstimateNodeHeight(LastNode);
		} while (Binding);
		return LastNode;
	}

	UEdGraphNode* InvokeTemplate(UEdGraphNode& Node, UEdGraph& Graph,
		UEdGraphPin* FromPin, const FVector2f& Location, const EPinInitialization PinOrder)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "CreateNativeTemplate", "Create Node"));
		Graph.Modify();
		if (FromPin) FromPin->Modify();
		Node.SetFlags(RF_Transactional);
		Node.Rename(nullptr, &Graph, REN_NonTransactional);
		Graph.AddNode(&Node, true, false);
		Node.CreateNewGuid();
		Node.PostPlacedNewNode();
		const auto SetupPins = [&]() { Node.AllocateDefaultPins(); Node.AutowireNewNode(FromPin); };
		if (PinOrder == EPinInitialization::BeforePlacement) SetupPins();
		Node.NodePosX = static_cast<int32>(Location.X);
		Node.NodePosY = static_cast<int32>(Location.Y);
		if (FromPin && FromPin->Direction == EGPD_Input)
		{
			const int32 FromX = FromPin->GetOwningNode()->NodePosX;
			if (FMath::Abs(FromX - Location.X) < 60.f) Node.NodePosX = FromX - 60;
		}
		Node.SnapToGrid(GetDefault<UEditorStyleSettings>()->GridSnapSize);
		if (PinOrder == EPinInitialization::AfterPlacement) SetupPins();
		return &Node;
	}

	UEdGraphNode* InvokeSchemaAction(FEdGraphSchemaAction& Action,
		UEdGraph& Graph, UEdGraphPin* FromPin, const FVector2f& Location)
	{
		// A view action is navigation, never creation. Do not invoke its UI.
		if (Action.GetTypeId() == FEdGraphSchemaAction_K2ViewNode::StaticGetTypeId()) return nullptr;
		// In UE's native extractor, every remaining non-menu template branch is
		// a K2NewNode subclass. Keep its virtual creation behavior, suppress only
		// the optional GotoNewNode flag, and restore the menu item afterwards.
		if (Action.GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()
			&& FBlueprintActionMenuUtils::ExtractNodeTemplateFromAction(Action))
		{
			auto& CreateAction = static_cast<FEdGraphSchemaAction_K2NewNode&>(Action);
			const TGuardValue<bool> Background(CreateAction.bGotoNode, false);
			return Action.PerformAction(&Graph, FromPin, Location, false);
		}
		return Action.PerformAction(&Graph, FromPin, Location, false);
	}
}
