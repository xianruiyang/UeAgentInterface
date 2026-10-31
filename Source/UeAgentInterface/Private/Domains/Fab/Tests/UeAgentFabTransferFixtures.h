#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"
#include "HttpModule.h"
#include "HttpRequestAdapter.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/Archive.h"

namespace UE::UAI::Fab::Tests
{
// Replace only the response boundary. The wrapped native request is never sent.
class FTransferResponse final : public IHttpResponse
{
public:
    int32 Code = 200;
    TArray<uint8> Body;
    FString Url = TEXT("https://fab-test.invalid/fixture");
    const FString& GetURL() const override { return Url; }
    const FString& GetEffectiveURL() const override { return Url; }
    EHttpRequestStatus::Type GetStatus() const override { return EHttpRequestStatus::Succeeded; }
    EHttpFailureReason GetFailureReason() const override { return EHttpFailureReason::None; }
    FString GetURLParameter(const FString&) const override { return {}; }
    FString GetHeader(const FString&) const override { return {}; }
    TArray<FString> GetAllHeaders() const override { return {}; }
    FString GetContentType() const override { return TEXT("application/octet-stream"); }
    uint64 GetContentLength() const override { return Body.Num(); }
    const TArray<uint8>& GetContent() const override { return Body; }
    int32 GetResponseCode() const override { return Code; }
    FString GetContentAsString() const override { return {}; }
    FUtf8StringView GetContentAsUtf8StringView() const override { return {}; }
};
class FTransferRequest final : public FHttpRequestAdapterBase
{
public:
    explicit FTransferRequest(int32 Code, int32 Size)
        : FHttpRequestAdapterBase(FHttpModule::Get().CreateRequest()), Response(MakeShared<FTransferResponse>())
    { Response->Code = Code; Response->Body.Init(42, Size); }
    TSharedPtr<FArchive> Stream;
    TSharedRef<FTransferResponse> Response;
    bool bTransportSuccess = true, bStart = true, bDeferred = false, bCancelled = false, bNoResponse = false;
    bool SetResponseBodyReceiveStream(TSharedRef<FArchive> InStream) override { Stream = InStream; return true; }
    bool ProcessRequest() override
    {
        if (!bStart) return false;
        if (!bDeferred) Complete(bTransportSuccess);
        return true;
    }
    void Complete(bool Success)
    {
        if (Stream && !bCancelled && !Response->Body.IsEmpty()) Stream->Serialize(Response->Body.GetData(), Response->Body.Num());
        FHttpResponsePtr NativeResponse;
        if (!bNoResponse) NativeResponse = Response;
        OnProcessRequestComplete().ExecuteIfBound(AsShared(), NativeResponse, Success);
        Stream.Reset();
    }
    void CancelRequest() override { bCancelled = true; Complete(false); }
};
}
#endif
