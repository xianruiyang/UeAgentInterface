#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetFolderThumbnailCapture.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetCompilingManager.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Editor/UnrealEdEngine.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UnrealEdGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	FString GetAssetFolderPreviewOptionsSchema()
	{
		return TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"recursive":{"type":"boolean"},
"class_paths":{"type":"array","items":{"type":"string","minLength":1}},
"page_size":{"type":"integer","minimum":1},
"generate_missing":{"type":"boolean"},
"after":{"type":"string"}
}})schema");
	}

	namespace
	{
		constexpr int32 SheetCapacity = 24;
		constexpr int32 Columns = 4;
		constexpr int32 TileWidth = 272;
		constexpr int32 ThumbnailSize = 208;
		constexpr int32 Padding = 12;
		constexpr int32 HeaderHeight = 76;
		constexpr int32 LineHeight = 20;

		struct FTile
		{
			FAssetData Asset;
			TArray<FColor> Pixels;
			FIntPoint Size = FIntPoint::ZeroValue;
			FString ThumbnailStatus = TEXT("missing");
			TArray<FString> NameLines;
		};

		bool CopyThumbnail(const FObjectThumbnail& Thumbnail, FTile& Tile)
		{
			if (Thumbnail.IsEmpty() || !Thumbnail.HasValidImageData()) return false;
			const FIntPoint Size(Thumbnail.GetImageWidth(), Thumbnail.GetImageHeight());
			const TArray<uint8>& Bytes = Thumbnail.GetUncompressedImageData();
			if (Size.X <= 0 || Size.Y <= 0 || Bytes.Num() != int64(Size.X) * Size.Y * sizeof(FColor)) return false;
			Tile.Size = Size;
			Tile.Pixels.SetNumUninitialized(Size.X * Size.Y);
			FMemory::Memcpy(Tile.Pixels.GetData(), Bytes.GetData(), Bytes.Num());
			return true;
		}

		TArray<FString> WrapLabel(const FString& Text, const FSlateFontInfo& Font)
		{
			const auto Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
			TArray<FString> Lines;
			FString Line;
			for (const TCHAR Character : Text)
			{
				const FString Candidate = Line + FString::Chr(Character);
				if (!Line.IsEmpty() && Measure->Measure(Candidate, Font).X > TileWidth - Padding * 2)
				{
					Lines.Add(MoveTemp(Line));
					Line.Reset();
				}
				Line.AppendChar(Character);
			}
			if (!Line.IsEmpty()) Lines.Add(MoveTemp(Line));
			return Lines;
		}

		bool WriteSheet(const TArray<FTile>& Tiles, const int32 First, const int32 Count,
			const FString& Folder, const int32 TotalAssets, const int32 PageStart,
			const FString& OutputPath, FIntPoint& OutSize, Contract::FDiagnostic& OutError)
		{
			const FSlateFontInfo Font(GEngine->GetSmallFont(), 14);
			int32 NameLineCount = 1;
			for (int32 Index = First; Index < First + Count; ++Index)
				NameLineCount = FMath::Max(NameLineCount, Tiles[Index].NameLines.Num());
			const int32 TileHeight = ThumbnailSize + 66 + NameLineCount * LineHeight;
			const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(Count, Columns));
			OutSize = FIntPoint(Columns * TileWidth + Padding * 2, HeaderHeight + Rows * TileHeight + Padding);
			TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
			Target->ClearColor = FLinearColor(FColor(22, 25, 31));
			Target->InitCustomFormat(OutSize.X, OutSize.Y, PF_B8G8R8A8, false);
			Target->UpdateResourceImmediate(true);
			auto* Resource = Target->GameThread_GetRenderTargetResource();
			FCanvas Canvas(Resource, nullptr, FGameTime(), GMaxRHIFeatureLevel);
			Canvas.Clear(Target->ClearColor);
			auto Text = [&](const FString& Label, const int32 X, const int32 Y, const FLinearColor Color)
			{
				FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Label), Font, Color);
				Canvas.DrawItem(Item);
			};
			Text(Folder, Padding + 4, 12, FLinearColor::White);
			Text(FString::Printf(TEXT("Assets %d-%d of %d  |  Native thumbnails  |  Missing previews stay listed"),
				Count ? PageStart + First + 1 : 0, PageStart + First + Count, TotalAssets), Padding + 4, 40,
				FLinearColor(FColor(167, 183, 203)));
			if (!Count) Text(TEXT("No matching assets on this page."), Padding + 16, HeaderHeight + 30, FLinearColor::White);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FTile& Tile = Tiles[First + Index];
				const int32 X = Padding + (Index % Columns) * TileWidth;
				const int32 Y = HeaderHeight + (Index / Columns) * TileHeight;
				FCanvasTileItem Card(FVector2D(X + 3, Y + 3), FVector2D(TileWidth - 6, TileHeight - 6), FLinearColor(FColor(32, 37, 45)));
				Card.BlendMode = SE_BLEND_Opaque;
				Canvas.DrawItem(Card);
				Text(FString::Printf(TEXT("#%d  %s"), PageStart + First + Index + 1, *Tile.Asset.AssetClassPath.GetAssetName().ToString()),
					X + Padding, Y + 9, FLinearColor(FColor(169, 190, 214)));
				if (Tile.Pixels.IsEmpty())
				{
					Text(TEXT("Preview: ") + Tile.ThumbnailStatus,
						X + 32, Y + 112, FLinearColor(FColor(235, 192, 116)));
				}
				for (int32 Line = 0; Line < Tile.NameLines.Num(); ++Line)
					Text(Tile.NameLines[Line], X + Padding, Y + ThumbnailSize + 42 + Line * LineHeight, FLinearColor::White);
			}
			Canvas.Flush_GameThread();
			FlushRenderingCommands();
			TArray<FColor> Sheet;
			const bool bRead = Resource->ReadPixels(Sheet);
			Canvas.SetRenderTarget_GameThread(nullptr);
			Target->ReleaseResource();
			if (!bRead || Sheet.Num() != OutSize.X * OutSize.Y)
			{
				OutError = CaptureDiagnostics::Native(TEXT("asset_folder_sheet_read_failed"), TEXT("The offscreen contact sheet could not be read."),
					TEXT("folder_sheet"), TEXT("pixels_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FTile& Tile = Tiles[First + Index];
				if (Tile.Pixels.IsEmpty()) continue;
				const double Scale = double(ThumbnailSize) / FMath::Max(Tile.Size.X, Tile.Size.Y);
				const FIntPoint DrawSize(FMath::Max(1, FMath::RoundToInt(Tile.Size.X * Scale)), FMath::Max(1, FMath::RoundToInt(Tile.Size.Y * Scale)));
				TArray<FColor> Resized;
				FImageUtils::ImageResize(Tile.Size.X, Tile.Size.Y, Tile.Pixels, DrawSize.X, DrawSize.Y, Resized, true);
				const int32 X = Padding + (Index % Columns) * TileWidth + (TileWidth - DrawSize.X) / 2;
				const int32 Y = HeaderHeight + (Index / Columns) * TileHeight + 32 + (ThumbnailSize - DrawSize.Y) / 2;
				for (int32 Row = 0; Row < DrawSize.Y; ++Row)
					FMemory::Memcpy(Sheet.GetData() + (Y + Row) * OutSize.X + X, Resized.GetData() + Row * DrawSize.X, DrawSize.X * sizeof(FColor));
			}
			return FCaptureImageProcessing::WriteCompressedImage(Sheet, OutSize, TEXT("png"), 100, OutputPath, OutError, true);
		}
	}

	bool TryCaptureAssetFolderThumbnails(const FUeAgentRequestContext& Context,
		TSharedPtr<FJsonObject>& OutResult, Contract::FDiagnostic& OutError)
	{
		using namespace CaptureDiagnostics;
		const double Started = FPlatformTime::Seconds();
		const auto& Params = Context.Params;
		FString Folder, After, OutputPath;
		Params->TryGetStringField(TEXT("folder"), Folder);
		while (Folder.EndsWith(TEXT("/"))) Folder.LeftChopInline(1);
		FText PathReason;
		if (!FPackageName::IsValidLongPackageName(Folder, true, &PathReason))
		{
			OutError = Input(TEXT("asset_folder_path_invalid"), PathReason.ToString(), TEXT("folder_query"), TEXT("/folder"),
				MakeShared<FJsonValueString>(Folder), MakeShared<FJsonValueString>(TEXT("UE content folder such as /Game/RuinedCrypt")));
			return false;
		}
		Params->TryGetStringField(TEXT("after"), After);
		Params->TryGetStringField(TEXT("output_file"), OutputPath);
		bool bRecursive = true;
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);
		bool bGenerateMissing = true;
		Params->TryGetBoolField(TEXT("generate_missing"), bGenerateMissing);
		int32 PageSize = 24;
		double PageSizeValue = PageSize;
		if (Params->TryGetNumberField(TEXT("page_size"), PageSizeValue)
			&& (!UeAgentPropertyValueCodec::TryConvertInt32(PageSizeValue, PageSize, OutError, true) || PageSize < 1))
		{
			if (OutError.IsEmpty()) OutError = Input(TEXT("asset_folder_page_size_invalid"), TEXT("page_size must be positive."),
				TEXT("folder_query"), TEXT("/page_size"), Number(PageSizeValue), MakeShared<FJsonValueString>(TEXT("positive integer")));
			return false;
		}
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.bRecursivePaths = bRecursive;
		Filter.bRecursiveClasses = true;
		const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
		if (Params->TryGetArrayField(TEXT("class_paths"), Classes))
			for (const auto& Class : *Classes) Filter.ClassPaths.Add(FTopLevelAssetPath(Class->AsString()));
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
		int32 PageStart = 0;
		while (PageStart < Assets.Num() && !After.IsEmpty() && Assets[PageStart].GetObjectPathString() <= After) ++PageStart;
		const int32 Count = FMath::Min(PageSize, Assets.Num() - PageStart);
		const double RegistryDone = FPlatformTime::Seconds();
		TArray<FName> FullNames;
		for (int32 Index = PageStart; Index < PageStart + Count; ++Index) FullNames.Add(FName(*Assets[Index].GetFullName()));
		FThumbnailMap Thumbnails;
		// The native loader checks memory, package thumbnails and external caches without loading asset UObjects.
		ThumbnailTools::ConditionallyLoadThumbnailsForObjects(FullNames, Thumbnails);
		TArray<FTile> Tiles;
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Available = 0, Cached = 0, GeneratedCount = 0;
		double GenerationSeconds = 0;
		const FSlateFontInfo Font(GEngine->GetSmallFont(), 14);
		for (int32 Index = PageStart; Index < PageStart + Count; ++Index)
		{
			FTile& Tile = Tiles.AddDefaulted_GetRef();
			Tile.Asset = Assets[Index];
			Tile.NameLines = WrapLabel(Tile.Asset.AssetName.ToString(), Font);
			if (const FObjectThumbnail* Thumbnail = Thumbnails.Find(FName(*Tile.Asset.GetFullName())))
			{
				if (CopyThumbnail(*Thumbnail, Tile))
				{
					Tile.ThumbnailStatus = TEXT("cached");
					++Cached;
				}
				else if (!Thumbnail->IsEmpty()) Tile.ThumbnailStatus = TEXT("invalid");
			}
			if (Tile.Pixels.IsEmpty() && bGenerateMissing)
			{
				const double GenerationStarted = FPlatformTime::Seconds();
				// Only this page's missing assets are loaded, one at a time. Never flush all
				// asset compilation or texture streaming merely to build a folder overview.
				TStrongObjectPtr<UObject> Object(Tile.Asset.GetAsset());
				if (!Object.IsValid()) Tile.ThumbnailStatus = TEXT("load_failed");
				else
				{
					UObject* ObjectToFinish = Object.Get();
					FAssetCompilingManager::Get().FinishCompilationForObjects(MakeArrayView(&ObjectToFinish, 1));
					if (!GUnrealEd || !GUnrealEd->GetThumbnailManager()->GetRenderingInfo(Object.Get()))
						Tile.ThumbnailStatus = TEXT("unsupported");
					else
					{
						FObjectThumbnail Generated;
						ThumbnailTools::RenderThumbnail(Object.Get(), ThumbnailSize, ThumbnailSize,
							ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Generated);
						if (CopyThumbnail(Generated, Tile))
						{
							Tile.ThumbnailStatus = TEXT("generated");
							++GeneratedCount;
							// Native memory cache only: no package save or dirty marking.
							ThumbnailTools::CacheThumbnail(Tile.Asset.GetFullName(), &Generated, Object->GetOutermost());
						}
						else Tile.ThumbnailStatus = TEXT("render_failed");
					}
				}
				GenerationSeconds += FPlatformTime::Seconds() - GenerationStarted;
			}
			if (!Tile.Pixels.IsEmpty()) ++Available;
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Tile.Asset.AssetName.ToString());
			Item->SetStringField(TEXT("object_path"), Tile.Asset.GetObjectPathString());
			Item->SetStringField(TEXT("class_path"), Tile.Asset.AssetClassPath.ToString());
			Item->SetStringField(TEXT("thumbnail_status"), Tile.ThumbnailStatus);
			Item->SetNumberField(TEXT("sheet_index"), (Index - PageStart) / SheetCapacity);
			Item->SetNumberField(TEXT("tile_index"), (Index - PageStart) % SheetCapacity);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Thumbnails.Empty();
		const double ThumbnailsDone = FPlatformTime::Seconds();
		TArray<TSharedPtr<FJsonValue>> Sheets, AdditionalPaths;
		FIntPoint FirstSize = FIntPoint::ZeroValue;
		for (int32 First = 0; First < FMath::Max(1, Count); First += SheetCapacity)
		{
			const int32 SheetIndex = First / SheetCapacity;
			const int32 SheetCount = FMath::Min(SheetCapacity, Count - First);
			const FString Path = SheetIndex == 0 ? OutputPath : FPaths::GetPath(OutputPath) / FString::Printf(TEXT("%s_sheet_%d.png"), *FPaths::GetBaseFilename(OutputPath), SheetIndex + 1);
			FIntPoint Size;
			if (!WriteSheet(Tiles, First, SheetCount, Folder, Assets.Num(), PageStart, Path, Size, OutError)) return false;
			if (!SheetIndex) FirstSize = Size;
			else AdditionalPaths.Add(MakeShared<FJsonValueString>(Path));
			const auto Sheet = MakeShared<FJsonObject>();
			Sheet->SetNumberField(TEXT("sheet_index"), SheetIndex);
			Sheet->SetBoolField(TEXT("uses_primary_artifact"), SheetIndex == 0);
			if (SheetIndex > 0) Sheet->SetNumberField(TEXT("raw_artifact_index"), SheetIndex - 1);
			Sheet->SetNumberField(TEXT("first_item"), First);
			Sheet->SetNumberField(TEXT("item_count"), SheetCount);
			Sheet->SetNumberField(TEXT("width"), Size.X);
			Sheet->SetNumberField(TEXT("height"), Size.Y);
			Sheets.Add(MakeShared<FJsonValueObject>(Sheet));
		}
		const auto Catalog = MakeShared<FJsonObject>();
		Catalog->SetStringField(TEXT("folder"), Folder);
		Catalog->SetBoolField(TEXT("recursive"), bRecursive);
		Catalog->SetBoolField(TEXT("generate_missing"), bGenerateMissing);
		if (Classes) Catalog->SetArrayField(TEXT("class_paths"), *Classes);
		Catalog->SetNumberField(TEXT("total_assets"), Assets.Num());
		Catalog->SetNumberField(TEXT("page_start"), PageStart);
		Catalog->SetNumberField(TEXT("returned_assets"), Count);
		Catalog->SetNumberField(TEXT("available_thumbnails"), Available);
		Catalog->SetNumberField(TEXT("cached_thumbnails"), Cached);
		Catalog->SetNumberField(TEXT("generated_thumbnails"), GeneratedCount);
		Catalog->SetBoolField(TEXT("has_more"), PageStart + Count < Assets.Num());
		if (Count && PageStart + Count < Assets.Num()) Catalog->SetStringField(TEXT("next_after"), Assets[PageStart + Count - 1].GetObjectPathString());
		Catalog->SetArrayField(TEXT("items"), MoveTemp(Items));
		Catalog->SetArrayField(TEXT("sheets"), MoveTemp(Sheets));
		const auto Timings = MakeShared<FJsonObject>();
		Timings->SetNumberField(TEXT("registry_seconds"), RegistryDone - Started);
		Timings->SetNumberField(TEXT("thumbnail_seconds"), ThumbnailsDone - RegistryDone);
		Timings->SetNumberField(TEXT("generation_seconds"), GenerationSeconds);
		Timings->SetNumberField(TEXT("sheet_seconds"), FPlatformTime::Seconds() - ThumbnailsDone);
		Catalog->SetObjectField(TEXT("timings"), Timings);
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("file_path"), OutputPath);
		OutResult->SetNumberField(TEXT("width"), FirstSize.X);
		OutResult->SetNumberField(TEXT("height"), FirstSize.Y);
		OutResult->SetStringField(TEXT("format"), TEXT("png"));
		OutResult->SetNumberField(TEXT("bytes"), IFileManager::Get().FileSize(*OutputPath));
		OutResult->SetObjectField(TEXT("asset_catalog"), Catalog);
		OutResult->SetArrayField(TEXT("folder_sheet_files"), MoveTemp(AdditionalPaths));
		return true;
	}
}
