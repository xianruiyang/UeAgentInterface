#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

namespace UE::UAI::NativeFeedback
{
struct FMessage
{
    FString Text;
    FName Category;
    ELogVerbosity::Type Verbosity;
};

// Scoped evidence for native APIs without a structured result. Only records the
// calling thread; it never interprets log messages as the operation's outcome.
class FNativeOperationFeedback final : public FOutputDevice
{
public:
    FNativeOperationFeedback();
    ~FNativeOperationFeedback() override;
    bool CanBeUsedOnAnyThread() const override { return true; }
    bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override;
    const TArray<FMessage>& GetMessages() const { return Messages; }
    FString GetDetail() const;
private:
    uint32 ThreadId;
    TArray<FMessage> Messages;
};
}
