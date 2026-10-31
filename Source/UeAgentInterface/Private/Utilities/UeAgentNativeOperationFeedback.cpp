#include "Utilities/UeAgentNativeOperationFeedback.h"

#include "HAL/PlatformTLS.h"
#include "Misc/OutputDeviceRedirector.h"

namespace UE::UAI::NativeFeedback
{
FNativeOperationFeedback::FNativeOperationFeedback() : ThreadId(FPlatformTLS::GetCurrentThreadId())
{ if (GLog) GLog->AddOutputDevice(this); }
FNativeOperationFeedback::~FNativeOperationFeedback()
{ if (GLog) GLog->RemoveOutputDevice(this); }

void FNativeOperationFeedback::Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category)
{
    // Immediate multi-thread delivery avoids GLog buffering; other threads only
    // read ThreadId and return without touching Messages.
    if (FPlatformTLS::GetCurrentThreadId() != ThreadId) return;
    if (Verbosity > ELogVerbosity::Warning && FCString::Strncmp(Text, TEXT("Message dialog closed,"), 22) != 0) return;
    if (!Messages.ContainsByPredicate([&](const FMessage& Item) { return Item.Category == Category && Item.Text == Text; }))
        Messages.Add({Text, Category, Verbosity});
}

FString FNativeOperationFeedback::GetDetail() const
{
    TArray<FString> Lines;
    for (const auto& Message : Messages)
        Lines.Add(Message.Category.IsNone() ? Message.Text : Message.Category.ToString() + TEXT(": ") + Message.Text);
    return FString::Join(Lines, TEXT("\n"));
}
}
