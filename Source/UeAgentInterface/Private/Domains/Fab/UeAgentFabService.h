#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Fab
{
/** Fab protocol and browser ownership stay in the Editor; SDK callers only see business results. */
class FService final : public TSharedFromThis<FService, ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FService, ESPMode::ThreadSafe> Create();
    ~FService();
    void Execute(CommandEdge::FUeAgentCommandRequest Request, Contract::FUeAgentCallerContext Caller, TFunction<bool()> IsCancelled,
        TFunction<void(CommandEdge::FUeAgentCommandResult)> Complete);
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FFabQueuedCancellationTest;
#endif
    FService();
    class FState;
    TUniquePtr<FState> State;
};
}
