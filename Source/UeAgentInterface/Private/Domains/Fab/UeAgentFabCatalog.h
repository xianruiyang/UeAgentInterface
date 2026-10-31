#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Domains/Fab/UeAgentFabDownload.h"

namespace UE::UAI::Fab
{
/** Private, short-lived remote pages. Native IDs/cursors never become public selectors. */
struct FCatalogCache
{
    struct FPages
    {
        double Created = 0;
        TArray<TSharedPtr<FJsonObject>> Pages;
    };
    TSharedPtr<FJsonObject> CategoryTree;
    TArray<TSharedPtr<FJsonValue>> ListingTypes;
    TMap<FString, FPages> Queries;
    double TaxonomyCreated = 0;
};

/** One query workflow, using the same native identities for discovery and resolution. */
class FCatalogRequest
{
public:
    enum class EStep { Request, Complete, DownloadReady, Failed };
    FCatalogRequest(FString InCommand, TSharedPtr<FJsonObject> InParams, FCatalogCache& InCache);
    EStep Begin();
    EStep Accept(const TSharedPtr<FJsonValue>& Value);
    const FString& GetPath() const { return Path; }
    const TSharedPtr<FJsonObject>& GetPayload() const { return Payload; }
    const Contract::FDiagnostic& GetError() const { return Error; }
    const FDownloadRecord& GetDownloadSelection() const { return DownloadSelection; }
    const TSharedPtr<FJsonObject>& GetDownloadAuthorization() const { return DownloadAuthorization; }
    bool IsAuthorizingDownload() const { return Phase == EPhase::Authorize; }

private:
    enum class EPhase { Status, Types, Tree, Search, Resolve, Detail, Formats, Authorize };
    FString Command;
    TSharedPtr<FJsonObject> Params;
    FCatalogCache& Cache;
    FString Path;
    FString Scope;
    FString BaseQuery;
    FString AssetBase;
    FString AssetName;
    bool bDirectListing = false;
    EPhase Phase = EPhase::Status;
    int64 WantedPage = 1;
    int32 PageIndex = 0;
    TArray<TSharedPtr<FJsonValue>> ResolveRows;
    TSet<FString> SeenCursors;
    TSharedPtr<FJsonObject> Listing;
    TSharedPtr<FJsonObject> AssetRecord;
    TSharedPtr<FJsonObject> Payload;
    Contract::FDiagnostic Error;
    FDownloadRecord DownloadSelection;
    TSharedPtr<FJsonObject> DownloadAuthorization;

    EStep Fail(const TCHAR* Code, const FString& Reason, const FString& Property = FString());
    EStep AfterTaxonomy();
    EStep BeginSearch();
    EStep NextPage();
    EStep ConsumePage(const TSharedPtr<FJsonObject>& Page);
    EStep ProjectCategories();
    EStep ResolveAsset();
    EStep ProjectDetails(const TArray<TSharedPtr<FJsonValue>>& Formats);
    EStep SelectDownload(const TArray<TSharedPtr<FJsonValue>>& Formats);
};
}
