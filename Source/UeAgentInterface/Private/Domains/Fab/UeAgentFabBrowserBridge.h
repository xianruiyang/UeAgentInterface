#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UeAgentFabBrowserBridge.generated.h"

/** Private IPC endpoint for the owned Fab browser. Never registered as a UAI command. */
UCLASS()
class UUeAgentFabBrowserBridge final : public UObject
{
    GENERATED_BODY()
public:
    TFunction<void(const FString&)> OnReceive;
    TFunction<FString(const FString&)> OnAuthentication;

    UFUNCTION()
    void Receive(const FString& Message) { if (OnReceive) OnReceive(Message); }

    UFUNCTION()
    FString Authentication(const FString& Request) { return OnAuthentication ? OnAuthentication(Request) : FString(); }
};
