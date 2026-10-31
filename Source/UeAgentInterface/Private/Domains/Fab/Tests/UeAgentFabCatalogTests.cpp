#if WITH_DEV_AUTOMATION_TESTS

#include "Domains/Fab/UeAgentFabCatalog.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Fab::Tests
{
TSharedPtr<FJsonValue> Json(const FString& Text)
{
    TSharedPtr<FJsonValue> Value;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    return Value;
}
TSharedPtr<FJsonObject> Object(const FString& Text) { return Json(Text)->AsObject(); }
TSharedPtr<FJsonValue> Page(const FString& Rows, const FString& Cursor = TEXT("null"))
{
    return Json(TEXT("{\"results\":[") + Rows + TEXT("],\"cursors\":{\"next\":") + Cursor + TEXT("}}"));
}
const FString RowA = TEXT(R"json({"uid":"library-a","listing":{"uid":"listing-a","title":"Twin","publisher":{"sellerName":"A"}}})json");
const FString RowB = TEXT(R"json({"uid":"library-b","listing":{"uid":"listing-b","title":"Twin","publisher":{"sellerName":"B"}}})json");
const FString Detail = TEXT(R"json({"listing":{"uid":"listing-a","title":"Twin","publisher":{"sellerName":"A"},"description":"<p>Readable description</p>"},"entitlement":{"licenses":[{"name":"Standard"}]}})json");
const FString Formats = TEXT(R"json([{"assetFormatType":{"code":"converted-files"},"files":[{"uid":"file-a","name":"mesh.glb","fileSize":128},{"uid":"file-b","name":"mesh.fbx","fileSize":256}]}])json");
using EStep = FCatalogRequest::EStep;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCatalogContractTest, "UeAgentInterface.Fab.Catalog.Contracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCatalogContractTest::RunTest(const FString& Parameters)
{
    {
        FCatalogCache Cache;
        FCatalogRequest Request(TEXT("fab.search"), Object(TEXT("{}")), Cache);
        TestTrue(TEXT("search requests library by default"), Request.Begin() == EStep::Request && Request.GetPath().StartsWith(TEXT("/i/library/search")));
        TestTrue(TEXT("same listing is grouped, distinct same-name listings survive"), Request.Accept(Page(RowA + TEXT(",") + RowA + TEXT(",") + RowB, TEXT("\"next-1\""))) == EStep::Complete);
        const auto& Items = Request.GetPayload()->GetArrayField(TEXT("items"));
        if (!TestEqual(TEXT("identity-aware distinct count"), Items.Num(), 2)) return false;
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Items[0]->AsObject()->GetStringField(TEXT("name")));
        Params->SetObjectField(TEXT("select"), Items[0]->AsObject()->GetObjectField(TEXT("select")));
        FCatalogRequest DetailRequest(TEXT("fab.asset"), Params, Cache);
        TestTrue(TEXT("detail enumerates pages before resolving"), DetailRequest.Begin() == EStep::Request);
        TestTrue(TEXT("detail follows its own name query cursor"), DetailRequest.Accept(Page(RowA + TEXT(",") + RowB, TEXT("\"next-1\""))) == EStep::Request);
        TestTrue(TEXT("selector resolves with a later different-name row"), DetailRequest.Accept(Page(TEXT("{\"uid\":\"other-row\",\"listing\":{\"uid\":\"other\",\"title\":\"Other\"}}"))) == EStep::Request);
        TestTrue(TEXT("library endpoint uses library row identity"), DetailRequest.GetPath().Contains(TEXT("/library-a")));
        TestTrue(TEXT("detail requests file formats"), DetailRequest.Accept(Json(Detail)) == EStep::Request);
        TestTrue(TEXT("details complete"), DetailRequest.Accept(Json(Formats)) == EStep::Complete);
        TestEqual(TEXT("public detail name"), DetailRequest.GetPayload()->GetStringField(TEXT("name")), FString(TEXT("Twin")));
        TestFalse(TEXT("missing engine metadata is not fabricated"), DetailRequest.GetPayload()->GetArrayField(TEXT("formats"))[0]->AsObject()->HasField(TEXT("engine_versions")));
    }
    {
        FCatalogCache Cache;
        FCatalogRequest Request(TEXT("fab.search"), Object(TEXT("{\"scope\":\"free\",\"page\":2}")), Cache);
        Request.Begin();
        TestTrue(TEXT("free search stays restricted"), Request.GetPath().Contains(TEXT("is_free=true")));
        TestTrue(TEXT("second page follows cursor"), Request.Accept(Page(TEXT("{\"uid\":\"a\",\"title\":\"One\"}"), TEXT("\"cursor 1\""))) == EStep::Request && Request.GetPath().Contains(TEXT("cursor=cursor%201")));
        TestTrue(TEXT("nonempty second page"), Request.Accept(Page(TEXT("{\"uid\":\"b\",\"title\":\"Two\"}"))) == EStep::Complete);
        TestEqual(TEXT("second page item"), Request.GetPayload()->GetArrayField(TEXT("items"))[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Two")));
        FCatalogRequest Beyond(TEXT("fab.search"), Object(TEXT("{\"scope\":\"free\",\"page\":3}")), Cache);
        TestTrue(TEXT("past terminal page reports error"), Beyond.Begin() == EStep::Failed && Beyond.GetError().Code == TEXT("fab_page_not_found"));
    }
    {
        FCatalogCache Cache;
        FCatalogRequest Request(TEXT("fab.search"), Object(TEXT("{\"page\":3}")), Cache);
        Request.Begin(); Request.Accept(Page(RowA,TEXT("\"repeat\"")));
        TestTrue(TEXT("repeated cursor terminates traversal"), Request.Accept(Page(RowB,TEXT("\"repeat\""))) == EStep::Failed && Request.GetError().Code == TEXT("fab_pagination_invalid"));
    }
    for (const FString& Cursor : {FString(TEXT("42")), FString(TEXT("{}"))})
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.search"), Object(TEXT("{}")), Cache);
        Request.Begin();
        TestTrue(TEXT("malformed cursor cannot masquerade as terminal page"), Request.Accept(Page(RowA,Cursor)) == EStep::Failed && Request.GetError().Code == TEXT("fab_response_changed"));
    }
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.search"), Object(TEXT("{}")), Cache);
        Request.Begin();
        TestTrue(TEXT("malformed result is not silently omitted"), Request.Accept(Page(RowA + TEXT(",{}"))) == EStep::Failed);
    }
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.categories"), Object(TEXT("{\"depth\":0}")), Cache);
        Request.Begin();
        TestTrue(TEXT("taxonomy requests tree"), Request.Accept(Json(TEXT("{\"results\":[{\"code\":\"3d-model\",\"name\":\"3D\"}]}"))) == EStep::Request);
        TestTrue(TEXT("taxonomy accepts native recursive structure"), Request.Accept(Json(TEXT(R"json({"results":{"3d-model":[{"slug":"objects","name":"Objects","children":[{"slug":"tools","name":"Tools","children":[]}]}]}})json"))) == EStep::Complete);
        const auto TypeRow = Request.GetPayload()->GetArrayField(TEXT("items"))[0]->AsObject();
        TestEqual(TEXT("deep category selector retained"), TypeRow->GetArrayField(TEXT("children"))[0]->AsObject()->GetArrayField(TEXT("children"))[0]->AsObject()->GetStringField(TEXT("category")), FString(TEXT("tools")));
        FCatalogRequest Search(TEXT("fab.search"), Object(TEXT("{\"category\":\"tools\"}")), Cache);
        TestTrue(TEXT("native category directly feeds search"), Search.Begin() == EStep::Request && Search.GetPath().Contains(TEXT("listing_types=3d-model&categories=tools")));
    }
    for (const FString& Types : {FString(TEXT("[{}]")), FString(TEXT("[{\"code\":\"3d-model\"}]"))})
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.categories"), Object(TEXT("{}")), Cache);
        Request.Begin();
        EStep Step = Request.Accept(Json(TEXT("{\"results\":") + Types + TEXT("}")));
        if (Step == EStep::Request) Step = Request.Accept(Json(TEXT("{\"results\":{}}")));
        TestTrue(TEXT("type missing public name/code is a protocol error"), Step == EStep::Failed && Request.GetError().Code == TEXT("fab_response_changed"));
    }
    for (const FString& Command : {FString(TEXT("fab.asset")), FString(TEXT("fab.download"))})
    {
        FCatalogCache Cache; FCatalogRequest Request(Command, Object(TEXT("{\"name\":\"Twin\"}")), Cache);
        Request.Begin(); Request.Accept(Page(RowA)); Request.Accept(Json(Detail));
        TestTrue(TEXT("wrong files shape is not empty availability"), Request.Accept(Json(TEXT("[{\"assetFormatType\":{\"code\":\"converted-files\"},\"files\":\"changed\"}]"))) == EStep::Failed && Request.GetError().Code == TEXT("fab_response_changed"));
    }
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.download"), Object(TEXT("{\"name\":\"Twin\"}")), Cache);
        Request.Begin(); Request.Accept(Page(RowA)); Request.Accept(Json(Detail));
        TestTrue(TEXT("default unique GLB reaches authorization"), Request.Accept(Json(Formats)) == EStep::Request && Request.IsAuthorizingDownload());
        TestEqual(TEXT("default GLB selection"), Request.GetDownloadSelection().Filename, FString(TEXT("mesh.glb")));
        TestTrue(TEXT("authorization hands off without exposing its body"), Request.Accept(Json(TEXT("{\"downloadInfo\":[]}"))) == EStep::DownloadReady && !Request.GetPayload());
    }
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.download"), Object(TEXT("{\"name\":\"Twin\",\"format\":\"converted-files\"}")), Cache);
        Request.Begin(); Request.Accept(Page(RowA)); Request.Accept(Json(Detail));
        TestTrue(TEXT("explicit format with multiple files needs a file"), Request.Accept(Json(Formats)) == EStep::Failed && Request.GetError().Code == TEXT("fab_file_selection_required"));
    }
    {
        FCatalogCache Cache; FCatalogRequest Request(TEXT("fab.download"), Object(TEXT("{\"name\":\"Twin\",\"file\":\"mesh.fbx\"}")), Cache);
        Request.Begin(); Request.Accept(Page(RowA)); Request.Accept(Json(Detail));
        TestTrue(TEXT("explicit file wins over GLB default"), Request.Accept(Json(Formats)) == EStep::Request && Request.GetDownloadSelection().Filename == TEXT("mesh.fbx"));
    }
    return !HasAnyErrors();
}
}
#endif
