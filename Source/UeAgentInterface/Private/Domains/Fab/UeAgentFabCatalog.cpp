#include "Domains/Fab/UeAgentFabCatalog.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Internationalization/Regex.h"

namespace UE::UAI::Fab
{
namespace
{
using ContentDiscovery::FNameSelectionEntry;
FString String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    if (Object) Object->TryGetStringField(Field, Value);
    return Value;
}
TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    return Parent && Parent->TryGetObjectField(Field, Value) ? *Value : nullptr;
}
const TArray<TSharedPtr<FJsonValue>>& Array(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
    return Parent && Parent->TryGetArrayField(Field, Value) ? *Value : Empty;
}
TSharedPtr<FJsonObject> AsObject(const TSharedPtr<FJsonValue>& Value)
{
    return Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
}
FString Encode(const FString& Value) { return FGenericPlatformHttp::UrlEncode(Value); }
TArray<TSharedPtr<FJsonValue>> ReadableValues(const TArray<TSharedPtr<FJsonValue>>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    TSet<FString> Seen;
    for (const auto& Value : Values)
    {
        FString Name;
        if (!Value->TryGetString(Name))
        {
            const auto Item = AsObject(Value);
            Name = String(Item, TEXT("name"));
            if (Name.IsEmpty()) Name = String(Item, TEXT("code"));
        }
        if (!Name.IsEmpty() && !Seen.Contains(Name)) { Seen.Add(Name); Result.Add(MakeShared<FJsonValueString>(Name)); }
    }
    return Result;
}
FString NextCursor(const TSharedPtr<FJsonObject>& Page) { return String(Object(Page, TEXT("cursors")), TEXT("next")); }
bool ValidCursor(const TSharedPtr<FJsonObject>& Page)
{
    const auto Cursors = Page->TryGetField(TEXT("cursors"));
    if (!Cursors || Cursors->IsNull()) return true;
    if (Cursors->Type != EJson::Object) return false;
    const auto Next = Cursors->AsObject()->TryGetField(TEXT("next"));
    return !Next || Next->IsNull() || Next->Type == EJson::String;
}
TSharedPtr<FJsonObject> ListingOf(const TSharedPtr<FJsonObject>& Row)
{
    auto Listing = Object(Row, TEXT("listing"));
    return Listing ? Listing : Row;
}
bool Entry(const TSharedPtr<FJsonValue>& Value, FNameSelectionEntry& Out)
{
    const auto Listing = ListingOf(AsObject(Value));
    Out.Name = String(Listing, TEXT("title"));
    Out.Identity = String(Listing, TEXT("uid"));
    const auto Publisher = Object(Listing, TEXT("publisher"));
    FString Seller = String(Publisher, TEXT("sellerName"));
    if (Seller.IsEmpty()) Seller = String(Publisher, TEXT("name"));
    if (!Seller.IsEmpty()) Out.Select.Add(TEXT("publisher"), Seller);
    const FString Published = String(Listing, TEXT("publishedAt"));
    if (!Published.IsEmpty()) Out.Select.Add(TEXT("published"), Published);
    return !Out.Name.IsEmpty() && !Out.Identity.IsEmpty();
}
TSharedRef<FJsonObject> PublicEntry(const FNameSelectionEntry& Entry)
{
    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("name"), Entry.Name);
    Out->SetStringField(TEXT("listing_url"), TEXT("https://www.fab.com/listings/") + Entry.Identity);
    if (!Entry.Select.IsEmpty())
    {
        auto Select = MakeShared<FJsonObject>();
        for (const auto& Field : Entry.Select) Select->SetStringField(Field.Key, Field.Value);
        Out->SetObjectField(TEXT("select"), Select);
    }
    return Out;
}
bool FileEntry(const TSharedPtr<FJsonValue>& Value, FNameSelectionEntry& Out)
{
    const auto File = AsObject(Value);
    Out.Name = String(File, TEXT("name"));
    Out.Identity = String(File, TEXT("uid"));
    int64 Size = 0;
    if (File && File->TryGetNumberField(TEXT("fileSize"), Size)) Out.Select.Add(TEXT("size_bytes"), LexToString(Size));
    const auto Versions = ReadableValues(Array(File, TEXT("engineVersions")));
    TArray<FString> Names;
    for (const auto& Version : Versions) Names.Add(Version->AsString());
    if (!Names.IsEmpty())
    {
        Names.Sort(); Out.Select.Add(TEXT("engine_versions"), FString::Join(Names, TEXT(", ")));
    }
    else
    {
        for (const auto& Platform : ReadableValues(Array(File, TEXT("platformsIncluded")))) Names.Add(Platform->AsString());
        if (!Names.IsEmpty()) { Names.Sort(); Out.Select.Add(TEXT("platforms"), FString::Join(Names, TEXT(", "))); }
    }
    return !Out.Name.IsEmpty() && !Out.Identity.IsEmpty();
}
TSharedPtr<FJsonObject> FindCategory(const TArray<TSharedPtr<FJsonValue>>& Nodes, const FString& Slug)
{
    for (const auto& Value : Nodes)
    {
        const auto Node = AsObject(Value);
        if (!Node) continue;
        if (String(Node, TEXT("slug")) == Slug) return Node;
        if (auto Child = FindCategory(Array(Node, TEXT("children")), Slug)) return Child;
    }
    return nullptr;
}
bool ValidCategories(const TArray<TSharedPtr<FJsonValue>>& Nodes)
{
    for (const auto& Value : Nodes)
    {
        const auto Node = AsObject(Value);
        if (String(Node, TEXT("name")).IsEmpty() || String(Node, TEXT("slug")).IsEmpty()) return false;
        const auto Children = Node->TryGetField(TEXT("children"));
        if (Children && (!Children->IsNull()) && (Children->Type != EJson::Array || !ValidCategories(Children->AsArray()))) return false;
    }
    return true;
}
bool CategoryRows(const TArray<TSharedPtr<FJsonValue>>& Nodes, const FString& Type, int64 Depth,
    TArray<TSharedPtr<FJsonValue>>& Rows)
{
    for (const auto& Value : Nodes)
    {
        const auto Node = AsObject(Value);
        const FString Name = String(Node, TEXT("name")), Slug = String(Node, TEXT("slug"));
        if (Name.IsEmpty() || Slug.IsEmpty()) return false;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Name);
        Row->SetStringField(TEXT("category"), Slug);
        const auto& Children = Array(Node, TEXT("children"));
        if (!Children.IsEmpty())
        {
            if (Depth == 1) Row->SetBoolField(TEXT("has_children"), true);
            else
            {
                TArray<TSharedPtr<FJsonValue>> Projected;
                if (!CategoryRows(Children, Type, Depth == 0 ? 0 : Depth - 1, Projected)) return false;
                Row->SetArrayField(TEXT("children"), Projected);
            }
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    return true;
}
FString PlainDescription(const FString& Html)
{
    FString Text;
    int32 Start = 0;
    FRegexMatcher Tags(FRegexPattern(TEXT("<[^>]*>")), Html);
    while (Tags.FindNext())
    {
        Text += Html.Mid(Start, Tags.GetMatchBeginning() - Start);
        Text += TEXT(" ");
        Start = Tags.GetMatchEnding();
    }
    Text += Html.Mid(Start);
    Text.ReplaceInline(TEXT("&nbsp;"), TEXT(" "), ESearchCase::CaseSensitive);
    Text.ReplaceInline(TEXT("&amp;"), TEXT("&"), ESearchCase::CaseSensitive);
    Text.ReplaceInline(TEXT("&lt;"), TEXT("<"), ESearchCase::CaseSensitive);
    Text.ReplaceInline(TEXT("&gt;"), TEXT(">"), ESearchCase::CaseSensitive);
    Text.ReplaceInline(TEXT("&quot;"), TEXT("\""), ESearchCase::CaseSensitive);
    TArray<FString> Words;
    Text.ParseIntoArrayWS(Words);
    return FString::Join(Words, TEXT(" "));
}
}

FCatalogRequest::FCatalogRequest(FString InCommand, TSharedPtr<FJsonObject> InParams, FCatalogCache& InCache)
    : Command(MoveTemp(InCommand)), Params(MoveTemp(InParams)), Cache(InCache) {}

FCatalogRequest::EStep FCatalogRequest::Fail(const TCHAR* Code, const FString& Reason, const FString& Property)
{
    Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("fab_catalog"));
    Error.PropertyPath = Property;
    Error.bRetrySafe = true;
    Error.SourceKind = Property.IsEmpty() ? TEXT("native_result") : TEXT("command_input");
    if (!Property.IsEmpty())
    {
        Error.Source.JsonPath = TEXT("/") + Property;
        if (const auto* Actual = Params->Values.Find(Property)) Error.ActualValue = *Actual;
    }
    Error.SuggestedFix = TEXT("Use fab.categories for native filters and fab.search for exact names/selectors; retry the same query after resolving any Fab access error.");
    return EStep::Failed;
}

FCatalogRequest::EStep FCatalogRequest::Begin()
{
    Scope = String(Params, TEXT("scope"));
    if (Scope.IsEmpty()) Scope = TEXT("library");
    if (Scope != TEXT("library") && Scope != TEXT("free"))
        return Fail(TEXT("fab_scope_invalid"), TEXT("scope must be library or free."), TEXT("scope"));
    if (Command == TEXT("fab.status")) { Phase = EPhase::Status; Path = TEXT("/i/library/search?sort_by=-createdAt"); return EStep::Request; }
    if (Command == TEXT("fab.categories") || !String(Params, TEXT("category")).IsEmpty() || !String(Params, TEXT("type")).IsEmpty())
    {
        if (Cache.CategoryTree && FPlatformTime::Seconds() - Cache.TaxonomyCreated < 300) return AfterTaxonomy();
        Phase = EPhase::Types; Path = TEXT("/i/taxonomy/listing-types"); return EStep::Request;
    }
    return BeginSearch();
}

FCatalogRequest::EStep FCatalogRequest::AfterTaxonomy()
{
    return Command == TEXT("fab.categories") ? ProjectCategories() : BeginSearch();
}

FCatalogRequest::EStep FCatalogRequest::BeginSearch()
{
    FString Type = String(Params, TEXT("type"));
    const FString Category = String(Params, TEXT("category"));
    if (!Type.IsEmpty())
    {
        bool Found = false;
        for (const auto& Item : Cache.ListingTypes) if (String(AsObject(Item), TEXT("code")) == Type) Found = true;
        if (!Found) return Fail(TEXT("fab_type_not_found"), TEXT("Unknown Fab product type: ") + Type, TEXT("type"));
    }
    if (!Category.IsEmpty())
    {
        FString CategoryType;
        for (const auto& Root : Cache.CategoryTree->Values)
            if (Root.Value->Type == EJson::Array && FindCategory(Root.Value->AsArray(), Category)) CategoryType = Root.Key;
        if (CategoryType.IsEmpty()) return Fail(TEXT("fab_category_not_found"), TEXT("Unknown Fab category: ") + Category, TEXT("category"));
        if (!Type.IsEmpty() && Type != CategoryType)
            return Fail(TEXT("fab_category_type_mismatch"), TEXT("The selected category belongs to product type ") + CategoryType + TEXT(", not ") + Type, TEXT("category"));
        Type = CategoryType;
    }
    FString Query = String(Params, TEXT("query"));
    if (Command == TEXT("fab.asset") || Command == TEXT("fab.download"))
    {
        AssetName = String(Params, TEXT("name"));
        if (AssetName.IsEmpty()) return Fail(TEXT("fab_asset_name_required"), TEXT("Pass the exact name from fab.search."), TEXT("name"));
        const FString ListingUrl = String(Params, TEXT("listing_url"));
        if (!ListingUrl.IsEmpty())
        {
            if (Command != TEXT("fab.asset") || Scope != TEXT("free"))
                return Fail(TEXT("fab_listing_url_scope_invalid"), TEXT("listing_url is supported only for free-catalog asset details."), TEXT("listing_url"));
            const FString Prefix(TEXT("https://www.fab.com/listings/"));
            const FString Id = ListingUrl.Mid(Prefix.Len());
            FGuid ListingId;
            if (!ListingUrl.StartsWith(Prefix, ESearchCase::CaseSensitive) || !FGuid::ParseExact(Id, EGuidFormats::DigitsWithHyphens, ListingId))
                return Fail(TEXT("fab_listing_url_invalid"), TEXT("Copy the canonical public listing_url from fab.search."), TEXT("listing_url"));
            bDirectListing = true;
            AssetBase = TEXT("/i/listings/") + Encode(Id);
            Path = AssetBase;
            Phase = EPhase::Detail;
            return EStep::Request;
        }
        Query = AssetName;
        Phase = EPhase::Resolve;
    }
    else Phase = EPhase::Search;
    BaseQuery = Scope == TEXT("library") ? TEXT("/i/library/search?sort_by=-createdAt") : TEXT("/i/listings/search?is_free=true");
    if (!Query.IsEmpty()) BaseQuery += TEXT("&q=") + Encode(Query);
    if (!Type.IsEmpty()) BaseQuery += TEXT("&listing_types=") + Encode(Type);
    if (!Category.IsEmpty()) BaseQuery += TEXT("&categories=") + Encode(Category);
    const TArray<TSharedPtr<FJsonValue>>* Styles = nullptr;
    if (Params->TryGetArrayField(TEXT("styles"), Styles) && !Styles->IsEmpty())
    {
        if (Scope != TEXT("free"))
            return Fail(TEXT("fab_style_scope_invalid"), TEXT("Native Style facets are currently supported for scope=free. My Library style support has not been verified."), TEXT("styles"));
        TSet<FString> Added;
        for (const auto& Value : *Styles)
        {
            FString Style;
            if (!Value->TryGetString(Style) || Style.IsEmpty())
                return Fail(TEXT("fab_style_invalid"), TEXT("Each style must be a nonempty native facet slug from Fab's public search URL."), TEXT("styles"));
            for (const TCHAR Character : Style)
                if (!(Character >= 'a' && Character <= 'z') && !(Character >= '0' && Character <= '9') && Character != '-')
                    return Fail(TEXT("fab_style_invalid"), TEXT("Copy style slugs unchanged from Fab's public search URL; display labels belong in query."), TEXT("styles"));
            if (!Added.Contains(Style)) BaseQuery += TEXT("&styles=") + Encode(Style);
            Added.Add(Style);
        }
    }
    Params->TryGetNumberField(TEXT("page"), WantedPage);
    if (WantedPage < 1) return Fail(TEXT("fab_page_invalid"), TEXT("page must be a positive integer."), TEXT("page"));
    auto& Cached = Cache.Queries.FindOrAdd(BaseQuery);
    if (FPlatformTime::Seconds() - Cached.Created > 60) { Cached.Pages.Reset(); Cached.Created = FPlatformTime::Seconds(); }
    return NextPage();
}

FCatalogRequest::EStep FCatalogRequest::NextPage()
{
    auto& Cached = Cache.Queries.FindChecked(BaseQuery);
    while (Cached.Pages.IsValidIndex(PageIndex))
    {
        const auto Page = Cached.Pages[PageIndex];
        if (Phase == EPhase::Search && PageIndex + 1 == WantedPage) return ConsumePage(Page);
        if (Phase == EPhase::Resolve)
        {
            ResolveRows.Append(Array(Page, TEXT("results")));
            if (NextCursor(Page).IsEmpty()) return ResolveAsset();
        }
        if (NextCursor(Page).IsEmpty())
            return Fail(TEXT("fab_page_not_found"), FString::Printf(TEXT("The query has %d page(s); requested page %lld."), PageIndex + 1, WantedPage), TEXT("page"));
        ++PageIndex;
    }
    Path = BaseQuery;
    if (PageIndex > 0)
    {
        const FString Cursor = NextCursor(Cached.Pages[PageIndex - 1]);
        if (SeenCursors.Contains(Cursor)) return Fail(TEXT("fab_pagination_invalid"), TEXT("Fab repeated a pagination cursor; the query is incomplete."));
        SeenCursors.Add(Cursor);
        Path += TEXT("&cursor=") + Encode(Cursor);
    }
    return EStep::Request;
}

FCatalogRequest::EStep FCatalogRequest::ConsumePage(const TSharedPtr<FJsonObject>& Page)
{
    TArray<FNameSelectionEntry> Entries;
    TArray<int32> Indices;
    for (const auto& Value : Array(Page, TEXT("results")))
    {
        FNameSelectionEntry Item;
        if (!Entry(Value, Item)) return Fail(TEXT("fab_response_changed"), TEXT("A search result has no listing title or native identity; the page was not silently truncated."));
        Indices.Add(Entries.Num()); Entries.Add(MoveTemp(Item));
    }
    TArray<ContentDiscovery::FNameSelectionGroup> Groups;
    if (!ContentDiscovery::TryGroupNameSelectionEntries(Entries, Indices, Groups, Error)) return EStep::Failed;
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const auto& Group : Groups) Items.Add(MakeShared<FJsonValueObject>(PublicEntry(Entries[Group.EntryIndices[0]])));
    Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("scope"), Scope);
    Payload->SetField(TEXT("page"), MakeShared<FJsonValueNumberString>(LexToString(WantedPage)));
    Payload->SetArrayField(TEXT("items"), Items);
    Payload->SetBoolField(TEXT("has_more"), !NextCursor(Page).IsEmpty());
    if (!NextCursor(Page).IsEmpty()) Payload->SetField(TEXT("next_page"), MakeShared<FJsonValueNumberString>(LexToString(WantedPage + 1)));
    return EStep::Complete;
}

FCatalogRequest::EStep FCatalogRequest::ResolveAsset()
{
    TArray<FNameSelectionEntry> Entries;
    for (const auto& Row : ResolveRows)
    {
        FNameSelectionEntry Item;
        if (!Entry(Row, Item)) return Fail(TEXT("fab_response_changed"), TEXT("A listing lookup result has no name or native identity."));
        Entries.Add(MoveTemp(Item));
    }
    TMap<FString, FString> Select;
    if (const auto Fields = Object(Params, TEXT("select")))
        for (const auto& Field : Fields->Values)
        {
            FString Value;
            if (!Field.Value->TryGetString(Value)) return Fail(TEXT("fab_selector_invalid"), TEXT("select values must be strings."), TEXT("select." ) + Field.Key);
            Select.Add(Field.Key, Value);
        }
    int32 Index = INDEX_NONE;
    const ContentDiscovery::FNameSelectionInputContext Input{TEXT(""), TEXT("command_input")};
    if (!ContentDiscovery::TryResolveNameSelection(Entries, AssetName, Select, Index, Error,
        ContentDiscovery::ENameComparison::CaseSensitive, &Input)) return EStep::Failed;
    const auto Row = AsObject(ResolveRows[Index]);
    AssetRecord = Row;
    Listing = ListingOf(Row);
    const FString Id = String(Scope == TEXT("library") ? Row : Listing, TEXT("uid"));
    if (Id.IsEmpty()) return Fail(TEXT("fab_response_changed"), TEXT("The selected resource has no source-specific identity."));
    AssetBase = Scope == TEXT("library") ? TEXT("/i/library/assets/") : TEXT("/i/listings/");
    AssetBase += Encode(Id);
    Path = AssetBase; Phase = EPhase::Detail;
    return EStep::Request;
}

FCatalogRequest::EStep FCatalogRequest::ProjectCategories()
{
    const FString Type = String(Params, TEXT("type")), Parent = String(Params, TEXT("parent"));
    int64 Depth = 1; Params->TryGetNumberField(TEXT("depth"), Depth);
    if (Depth < 0) return Fail(TEXT("fab_depth_invalid"), TEXT("depth must be nonnegative; zero requests the complete subtree."), TEXT("depth"));
    TArray<TSharedPtr<FJsonValue>> Items;
    bool Found = false;
    for (const auto& Value : Cache.ListingTypes)
    {
        const auto NativeType = AsObject(Value);
        const FString Code = String(NativeType, TEXT("code"));
        if (!Type.IsEmpty() && Code != Type) continue;
        const auto& Children = Array(Cache.CategoryTree, *Code);
        if (!Parent.IsEmpty())
        {
            const auto Node = FindCategory(Children, Parent);
            if (!Node) continue;
            if (!CategoryRows(Array(Node, TEXT("children")), Code, Depth, Items))
                return Fail(TEXT("fab_response_changed"), TEXT("A native category lacks its name or slug."));
        }
        else if (!Type.IsEmpty())
        {
            if (!CategoryRows(Children, Code, Depth, Items)) return Fail(TEXT("fab_response_changed"), TEXT("A native category lacks its name or slug."));
        }
        else
        {
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("type"), Code);
            Row->SetStringField(TEXT("name"), String(NativeType, TEXT("name")));
            if (!Children.IsEmpty())
            {
                if (Depth == 1) Row->SetBoolField(TEXT("has_children"), true);
                else
                {
                    TArray<TSharedPtr<FJsonValue>> Projected;
                    if (!CategoryRows(Children, Code, Depth == 0 ? 0 : Depth - 1, Projected))
                        return Fail(TEXT("fab_response_changed"), TEXT("A native category lacks its name or slug."));
                    Row->SetArrayField(TEXT("children"), Projected);
                }
            }
            Items.Add(MakeShared<FJsonValueObject>(Row));
        }
        Found = true;
    }
    if (!Found) return Fail(TEXT("fab_category_not_found"), TEXT("No native category subtree matches the supplied type/parent."));
    Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("items"), Items);
    Payload->SetField(TEXT("depth"), MakeShared<FJsonValueNumberString>(LexToString(Depth)));
    if (!Type.IsEmpty()) Payload->SetStringField(TEXT("type"), Type);
    if (!Parent.IsEmpty()) Payload->SetStringField(TEXT("parent"), Parent);
    return EStep::Complete;
}

FCatalogRequest::EStep FCatalogRequest::ProjectDetails(const TArray<TSharedPtr<FJsonValue>>& Formats)
{
    if (Command == TEXT("fab.download")) return SelectDownload(Formats);
    Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), String(Listing, TEXT("title")));
    Payload->SetStringField(TEXT("scope"), Scope);
    const FString Publisher = String(Object(Listing, TEXT("publisher")), TEXT("sellerName"));
    if (!Publisher.IsEmpty()) Payload->SetStringField(TEXT("publisher"), Publisher);
    const FString Id = String(Listing, TEXT("uid"));
    if (!Id.IsEmpty()) Payload->SetStringField(TEXT("listing_url"), TEXT("https://www.fab.com/listings/") + Id);
    const FString Description = PlainDescription(String(Listing, TEXT("description")));
    if (!Description.IsEmpty()) Payload->SetStringField(TEXT("description"), Description.Left(600));
    if (Description.Len() > 600) Payload->SetBoolField(TEXT("description_truncated"), true);
    TArray<TSharedPtr<FJsonValue>> Licenses;
    for (const auto& Value : Array(Object(AssetRecord, TEXT("entitlement")), TEXT("licenses")))
    {
        const auto License = AsObject(Value);
        const FString Name = String(License, TEXT("name")), Group = String(Object(License, TEXT("group")), TEXT("name"));
        if (!Name.IsEmpty()) Licenses.Add(MakeShared<FJsonValueString>(Group.IsEmpty() ? Name : Group + TEXT(" / ") + Name));
    }
    if (!Licenses.IsEmpty()) Payload->SetArrayField(TEXT("licenses"), Licenses);
    TArray<TSharedPtr<FJsonValue>> ProjectedFormats;
    for (const auto& Value : Formats)
    {
        const auto Format = AsObject(Value), Type = Object(Format, TEXT("assetFormatType"));
        const FString Code = String(Type, TEXT("code"));
        if (Code.IsEmpty()) return Fail(TEXT("fab_response_changed"), TEXT("An asset format has no native format code."));
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("format"), Code);
        for (const auto& NativeFormatValue : Array(Listing, TEXT("assetFormats")))
        {
            const auto NativeFormat = AsObject(NativeFormatValue);
            if (String(Object(NativeFormat, TEXT("assetFormatType")), TEXT("code")) != Code) continue;
            const auto Specs = Object(NativeFormat, TEXT("technicalSpecs"));
            const auto Versions = ReadableValues(Array(Specs, TEXT("unrealEngineEngineVersions")));
            const auto Platforms = ReadableValues(Array(Specs, TEXT("unrealEngineTargetPlatforms")));
            if (!Versions.IsEmpty()) Row->SetArrayField(TEXT("engine_versions"), Versions);
            if (!Platforms.IsEmpty()) Row->SetArrayField(TEXT("platforms"), Platforms);
            const FString Distribution = String(Specs, TEXT("unrealEngineDistributionMethod"));
            if (!Distribution.IsEmpty()) Row->SetStringField(TEXT("distribution"), Distribution);
        }
        TArray<TSharedPtr<FJsonValue>> Files;
        TArray<FNameSelectionEntry> FileEntries;
        TArray<int32> FileIndices;
        for (const auto& FileValue : Array(Format, TEXT("files")))
        {
            FNameSelectionEntry Entry;
            if (!FileEntry(FileValue, Entry))
                return Fail(TEXT("fab_response_changed"), TEXT("A downloadable file lacks its name or native identity."));
            FileIndices.Add(FileEntries.Num()); FileEntries.Add(MoveTemp(Entry));
        }
        TArray<ContentDiscovery::FNameSelectionGroup> Groups;
        if (!ContentDiscovery::TryGroupNameSelectionEntries(FileEntries, FileIndices, Groups, Error)) return EStep::Failed;
        TMap<FString, int32> NameCounts;
        for (const auto& Group : Groups) ++NameCounts.FindOrAdd(FileEntries[Group.EntryIndices[0]].Name);
        for (const auto& Group : Groups)
        {
            const int32 Index = Group.EntryIndices[0];
            const auto& Entry = FileEntries[Index];
            const auto File = AsObject(Array(Format, TEXT("files"))[Index]);
            auto Public = MakeShared<FJsonObject>();
            Public->SetStringField(TEXT("file"), Entry.Name);
            if (NameCounts.FindChecked(Entry.Name) > 1 && !Entry.Select.IsEmpty())
            {
                auto Select = MakeShared<FJsonObject>();
                for (const auto& Field : Entry.Select) Select->SetStringField(Field.Key, Field.Value);
                Public->SetObjectField(TEXT("file_select"), Select);
            }
            double Size = 0;
            if (File->TryGetNumberField(TEXT("fileSize"), Size)) Public->SetNumberField(TEXT("size_bytes"), Size);
            for (const TCHAR* Key : {TEXT("engineVersions"), TEXT("platformsIncluded")})
                if (File->HasTypedField<EJson::Array>(Key)) Public->SetArrayField(FCString::Strcmp(Key, TEXT("engineVersions")) == 0 ? TEXT("engine_versions") : TEXT("platforms"), ReadableValues(Array(File, Key)));
            Files.Add(MakeShared<FJsonValueObject>(Public));
        }
        Row->SetArrayField(TEXT("files"), Files);
        ProjectedFormats.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("formats"), ProjectedFormats);
    return EStep::Complete;
}

FCatalogRequest::EStep FCatalogRequest::SelectDownload(const TArray<TSharedPtr<FJsonValue>>& Formats)
{
    FString FormatName = String(Params, TEXT("format")), FileName = String(Params, TEXT("file"));
    TSharedPtr<FJsonObject> SelectedFormat;
    if (FormatName.IsEmpty())
    {
        // A supplied filename is a real selection and must never be replaced
        // by a convenient default from another format.
        TArray<TSharedPtr<FJsonObject>> Matches;
        TSet<FString> SeenFiles;
        FString DefaultFile;
        for (const auto& Value : Formats)
        {
            const auto Format = AsObject(Value);
            for (const auto& FileValue : Array(Format, TEXT("files")))
            {
                const FString Name = String(AsObject(FileValue), TEXT("name"));
                if (!FileName.IsEmpty() && Name == FileName)
                { Matches.AddUnique(Format); DefaultFile = Name; }
                else if (FileName.IsEmpty() && Name.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase))
                {
                    const FString Key = String(Object(Format, TEXT("assetFormatType")), TEXT("code")) + TEXT(":") + String(AsObject(FileValue), TEXT("uid"));
                    if (!SeenFiles.Contains(Key)) { SeenFiles.Add(Key); Matches.Add(Format); DefaultFile = Name; }
                }
            }
        }
        if (Matches.Num() == 1) { SelectedFormat = Matches[0]; FileName = DefaultFile; }
        if (Matches.IsEmpty() && FileName.IsEmpty())
            for (const auto& Value : Formats)
                if (String(Object(AsObject(Value), TEXT("assetFormatType")), TEXT("code")) == TEXT("unreal-engine")) Matches.Add(AsObject(Value));
        if (!SelectedFormat && Matches.Num() == 1) SelectedFormat = Matches[0];
        if (!SelectedFormat && Matches.IsEmpty() && Formats.Num() == 1) SelectedFormat = AsObject(Formats[0]);
    }
    else
        for (const auto& Value : Formats)
            if (String(Object(AsObject(Value), TEXT("assetFormatType")), TEXT("code")) == FormatName) { SelectedFormat = AsObject(Value); break; }
    if (!SelectedFormat)
    {
        Fail(TEXT("fab_format_selection_required"), TEXT("No unique supported default format was selected. Pass format from fab.asset."), TEXT("format"));
        TArray<TSharedPtr<FJsonValue>> Choices;
        for (const auto& Value : Formats) Choices.Add(MakeShared<FJsonValueString>(String(Object(AsObject(Value), TEXT("assetFormatType")), TEXT("code"))));
        Error.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
        return EStep::Failed;
    }
    FormatName = String(Object(SelectedFormat, TEXT("assetFormatType")), TEXT("code"));
    const auto& Files = Array(SelectedFormat, TEXT("files"));
    TArray<FNameSelectionEntry> Entries;
    TArray<int32> Indices;
    for (const auto& Value : Files)
    {
        FNameSelectionEntry Item;
        if (!FileEntry(Value, Item)) return Fail(TEXT("fab_response_changed"), TEXT("A downloadable file lacks its name or identity."));
        Indices.Add(Entries.Num()); Entries.Add(MoveTemp(Item));
    }
    TArray<ContentDiscovery::FNameSelectionGroup> Groups;
    if (!ContentDiscovery::TryGroupNameSelectionEntries(Entries, Indices, Groups, Error)) return EStep::Failed;
    if (FileName.IsEmpty() && Groups.Num() == 1) FileName = Entries[Groups[0].EntryIndices[0]].Name;
    if (FileName.IsEmpty())
    {
        Fail(TEXT("fab_file_selection_required"), TEXT("This format has multiple files or no files. Pass an exact file name from fab.asset."), TEXT("file"));
        TArray<TSharedPtr<FJsonValue>> Choices;
        for (const auto& Group : Groups) Choices.Add(MakeShared<FJsonValueObject>(PublicEntry(Entries[Group.EntryIndices[0]])));
        Error.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
        return EStep::Failed;
    }
    int32 Index = INDEX_NONE;
    TMap<FString, FString> FileSelect;
    if (const auto Fields = Object(Params, TEXT("file_select")))
        for (const auto& Field : Fields->Values)
        {
            FString Value;
            if (!Field.Value->TryGetString(Value)) return Fail(TEXT("fab_selector_invalid"), TEXT("file_select values must be strings."), TEXT("file_select.") + Field.Key);
            FileSelect.Add(Field.Key, Value);
        }
    const ContentDiscovery::FNameSelectionInputContext FileInput{TEXT(""), TEXT("command_input"), TEXT("file"), TEXT("file_select")};
    if (!ContentDiscovery::TryResolveNameSelection(Entries, FileName, FileSelect, Index, Error,
        ContentDiscovery::ENameComparison::CaseSensitive, &FileInput)) return EStep::Failed;
    const auto File = AsObject(Files[Index]);
    DownloadSelection.Asset = String(Listing, TEXT("title"));
    DownloadSelection.Publisher = String(Object(Listing, TEXT("publisher")), TEXT("sellerName"));
    DownloadSelection.Format = FormatName;
    DownloadSelection.Filename = FileName;
    DownloadSelection.ListingId = String(Listing, TEXT("uid"));
    DownloadSelection.FileId = Entries[Index].Identity;
    File->TryGetNumberField(TEXT("fileSize"), DownloadSelection.ExpectedSize);
    DownloadSelection.Name = String(Params, TEXT("save_as")).TrimStartAndEnd();
    if (DownloadSelection.Name.IsEmpty()) DownloadSelection.Name = DownloadSelection.Asset + TEXT(" / ")
        + DownloadSelection.Publisher + TEXT(" / ") + FormatName + TEXT(" / ") + FileName;
    Phase = EPhase::Authorize;
    Path = AssetBase + TEXT("/asset-formats/") + Encode(FormatName) + TEXT("/files/") + Encode(Entries[Index].Identity) + TEXT("/download-info?platform=Windows");
    return EStep::Request;
}

FCatalogRequest::EStep FCatalogRequest::Accept(const TSharedPtr<FJsonValue>& Value)
{
    const auto Data = AsObject(Value);
    if (Phase == EPhase::Formats)
    {
        const TArray<TSharedPtr<FJsonValue>>* Formats = nullptr;
        if (Value && Value->Type == EJson::Array) Formats = &Value->AsArray();
        else if (Data) Data->TryGetArrayField(TEXT("results"), Formats);
        if (!Formats) return Fail(TEXT("fab_response_changed"), TEXT("Fab's formats response is not an array."));
        for (const auto& FormatValue : *Formats)
        {
            const auto Format = AsObject(FormatValue);
            if (String(Object(Format, TEXT("assetFormatType")), TEXT("code")).IsEmpty())
                return Fail(TEXT("fab_response_changed"), TEXT("An asset format has no native format code."));
            const auto Files = Format->TryGetField(TEXT("files"));
            if (Files && Files->Type != EJson::Array)
                return Fail(TEXT("fab_response_changed"), TEXT("An asset format's files field is not an array."));
        }
        return ProjectDetails(*Formats);
    }
    if (!Data) return Fail(TEXT("fab_response_changed"), TEXT("Fab returned a non-object response."));
    if (Phase == EPhase::Authorize)
    {
        DownloadAuthorization = Data;
        return EStep::DownloadReady;
    }
    if (Phase == EPhase::Status)
    {
        if (!Data->HasTypedField<EJson::Array>(TEXT("results"))) return Fail(TEXT("fab_response_changed"), TEXT("Fab's library response lacks results."));
        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("state"), TEXT("ready"));
        Payload->SetStringField(TEXT("session"), TEXT("editor_fab"));
        Payload->SetBoolField(TEXT("library_access"), true);
        return EStep::Complete;
    }
    if (Phase == EPhase::Types)
    {
        if (!Data->HasTypedField<EJson::Array>(TEXT("results"))) return Fail(TEXT("fab_response_changed"), TEXT("Fab listing types lack results."));
        Cache.ListingTypes = Array(Data, TEXT("results"));
        for (const auto& Type : Cache.ListingTypes)
            if (String(AsObject(Type), TEXT("code")).IsEmpty() || String(AsObject(Type), TEXT("name")).IsEmpty())
                return Fail(TEXT("fab_response_changed"), TEXT("A native product type lacks its name or code."));
        Phase = EPhase::Tree; Path = TEXT("/i/taxonomy/categories/tree"); return EStep::Request;
    }
    if (Phase == EPhase::Tree)
    {
        const auto Tree = Object(Data, TEXT("results"));
        if (!Tree) return Fail(TEXT("fab_response_changed"), TEXT("Fab category tree lacks its native root groups."));
        for (const auto& Group : Tree->Values)
            if (Group.Value->Type != EJson::Array || !ValidCategories(Group.Value->AsArray()))
                return Fail(TEXT("fab_response_changed"), TEXT("A native category group has an invalid name, slug or children array: ") + Group.Key);
        Cache.CategoryTree = Tree;
        Cache.TaxonomyCreated = FPlatformTime::Seconds();
        return AfterTaxonomy();
    }
    if (Phase == EPhase::Search || Phase == EPhase::Resolve)
    {
        if (!Data->HasTypedField<EJson::Array>(TEXT("results"))) return Fail(TEXT("fab_response_changed"), TEXT("Fab search response lacks results."));
        if (!ValidCursor(Data)) return Fail(TEXT("fab_response_changed"), TEXT("Fab's pagination cursor must be a string or null; this query is incomplete."));
        Cache.Queries.FindChecked(BaseQuery).Pages.Add(Data);
        return NextPage();
    }
    if (Phase == EPhase::Detail)
    {
        if (Object(Data, TEXT("listing"))) AssetRecord = Data;
        Listing = ListingOf(Data);
        if (String(Listing, TEXT("title")).IsEmpty()) return Fail(TEXT("fab_response_changed"), TEXT("The selected asset detail has no title."));
        if (bDirectListing)
        {
            FNameSelectionEntry Selected;
            if (!Entry(MakeShared<FJsonValueObject>(Listing), Selected))
                return Fail(TEXT("fab_response_changed"), TEXT("The public listing detail lacks its native identity."));
            TMap<FString, FString> Select;
            if (const auto Fields = Object(Params, TEXT("select")))
                for (const auto& Field : Fields->Values)
                {
                    FString FieldValue;
                    if (!Field.Value->TryGetString(FieldValue)) return Fail(TEXT("fab_selector_invalid"), TEXT("select values must be strings."), TEXT("select.") + Field.Key);
                    Select.Add(Field.Key, FieldValue);
                }
            int32 Index = INDEX_NONE;
            const ContentDiscovery::FNameSelectionInputContext Input{TEXT(""), TEXT("command_input")};
            if (!ContentDiscovery::TryResolveNameSelection({Selected}, AssetName, Select, Index, Error,
                ContentDiscovery::ENameComparison::CaseSensitive, &Input)) return EStep::Failed;
        }
        Phase = EPhase::Formats; Path = AssetBase + TEXT("/asset-formats"); return EStep::Request;
    }
    return Fail(TEXT("fab_response_changed"), TEXT("Unexpected Fab catalog phase."));
}
}
