#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "BlueprintNodeBinder.h"
#include "Math/Vector2D.h"

class UBlueprintNodeSpawner;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
struct FBlueprintActionContext;
struct FBlueprintActionMenuItem;
struct FEdGraphSchemaAction;

namespace UE::UAI::ContentDiscovery::NativeNodeAction
{
	// Reads the selected native menu item's bindings through its public UI-spec
	// protocol. The item and the spawner's original delegate remain unchanged.
	UEAGENTINTERFACE_API bool ReadBindings(const FBlueprintActionMenuItem& Item,
		const FBlueprintActionContext& Context,
		IBlueprintNodeBinder::FBindingSet& OutBindings, UE::UAI::Contract::FDiagnostic& OutError);

	// Native spawning, binding, positioning, autowiring and dirty notification,
	// without menu-only selection or opening/focusing an asset editor.
	UEAGENTINTERFACE_API UEdGraphNode* Invoke(const UBlueprintNodeSpawner& Spawner, UEdGraph& Graph,
		UEdGraphPin* FromPin, const FVector2f& Location,
		const IBlueprintNodeBinder::FBindingSet& Bindings);

	enum class EPinInitialization { BeforePlacement, AfterPlacement };
	// Template menus that ignore bSelectNewNode use the same native node hooks,
	// with an explicit non-selecting AddNode. Relationship actions remain native.
	UEAGENTINTERFACE_API UEdGraphNode* InvokeTemplate(UEdGraphNode& Node, UEdGraph& Graph,
		UEdGraphPin* FromPin, const FVector2f& Location, EPinInitialization PinOrder);

	UEAGENTINTERFACE_API UEdGraphNode* InvokeSchemaAction(FEdGraphSchemaAction& Action,
		UEdGraph& Graph, UEdGraphPin* FromPin, const FVector2f& Location);
}
