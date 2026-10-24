#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "EdGraph/EdGraphPin.h"

class UEdGraphNode;
class UEdGraph;
class UObject;

namespace UeAgentAuthoringGraphLinks
{
    using UE::UAI::Contract::FDiagnostic;
    /** Request-derived link expectations, retained until the entire native graph edit settles. */
    class UEAGENTINTERFACE_API FIntent
    {
    public:
        void Expect(UEdGraphPin* From, UEdGraphPin* To, bool bConnected, FString SourcePointer);

        // A schema may implement a requested edge with conversion nodes. Only nodes
        // created by that single schema call may bridge the original endpoints.
        bool ExpectConnected(UEdGraphPin* From, UEdGraphPin* To,
            const TSet<UEdGraphNode*>& ConversionNodes, const FString& SourcePointer, FDiagnostic& Error);

        bool Verify(FDiagnostic& Error) const;
        void Observe() const;

        // Freeze only native identity bindings after creation identities settle.
        // The requested connected/disconnected state is never recaptured.
        bool Freeze(UObject& Owner, FDiagnostic& Error);
        bool Verify(UObject& Owner, FDiagnostic& Error) const;
        bool Verify(UObject& Owner, TConstArrayView<UEdGraph*> Graphs, FDiagnostic& Error) const;

    private:
        struct FEndpoint
        {
            FEdGraphPinReference Pin;
            TWeakObjectPtr<UEdGraphNode> PendingNode;
            FString GraphPath;
            FGuid GraphId;
            FGuid NodeId;
            FGuid PinId;
            FName PinName;
            EEdGraphPinDirection Direction = EGPD_Input;
            int32 Occurrence = 0;
            bool bFrozen = false;

            explicit FEndpoint(UEdGraphPin* InPin);
            bool Freeze(UObject& Owner, bool bAllowRemoved, FDiagnostic& Error);
        };
        struct FLink
        {
            FEndpoint From;
            FEndpoint To;
            bool bConnected = true;
            FString SourcePointer;
        };
        TArray<FLink> Links;
        bool bFrozen = false;
    };
}
