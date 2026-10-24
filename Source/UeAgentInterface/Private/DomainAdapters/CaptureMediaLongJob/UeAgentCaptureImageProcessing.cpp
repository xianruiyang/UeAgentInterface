#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UeAgentInterfaceSettings.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		Contract::FDiagnostic PixelFailure(const FString& Code, const TArray<FColor>& Pixels,
			const FIntPoint& Size, const FString& Stage)
		{
			if (Size.X <= 0 || Size.Y <= 0)
				return CaptureDiagnostics::Native(Code, TEXT("The captured image dimension must be positive."), Stage,
					Size.X <= 0 ? TEXT("width") : TEXT("height"), CaptureDiagnostics::Number(Size.X <= 0 ? Size.X : Size.Y),
					MakeShared<FJsonValueString>(TEXT("positive pixel dimension")));
			return CaptureDiagnostics::Native(Code, TEXT("The pixel count does not match the image dimensions."), Stage,
				TEXT("pixel_count"), CaptureDiagnostics::Number(Pixels.Num()), CaptureDiagnostics::Number(int64(Size.X) * Size.Y));
		}

		bool ReadSheetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			const TCHAR* Alias, int32 Minimum, int32& InOutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			double Number = 0;
			if (!Object.IsValid()) return true;
			const TCHAR* SelectedField = Field;
			if (!Object->TryGetNumberField(Field, Number))
			{
				SelectedField = Alias;
				if (!Object->TryGetNumberField(Alias, Number)) return true;
			}
			int32 Rounded = 0;
			if (!UeAgentPropertyValueCodec::TryConvertInt32(Number, Rounded, OutError, true))
			{
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = SelectedField;
				CaptureDiagnostics::LocateInput(OutError, TEXT("/") + FString(SelectedField));
				return false;
			}
			InOutValue = FMath::Max(Rounded, Minimum);
			return true;
		}

		FString MakeDefaultShotPath(const FString& Extension)
		{
			const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
			const FString RootRelative = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
			const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRelative / TEXT("Shots"));
			return FPaths::Combine(Directory, FString::Printf(TEXT("shot_%s_%s.%s"), FPlatformProcess::ComputerName(), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")), *Extension));
		}

		const TCHAR* Glyph(const TCHAR Character, const int32 Row)
		{
			static const TCHAR* Empty[7] = { TEXT("00000"), TEXT("00000"), TEXT("00000"), TEXT("00000"), TEXT("00000"), TEXT("00000"), TEXT("00000") };
			static const TCHAR* Digits[10][7] = {
				{TEXT("01110"),TEXT("10001"),TEXT("10011"),TEXT("10101"),TEXT("11001"),TEXT("10001"),TEXT("01110")},
				{TEXT("00100"),TEXT("01100"),TEXT("00100"),TEXT("00100"),TEXT("00100"),TEXT("00100"),TEXT("01110")},
				{TEXT("01110"),TEXT("10001"),TEXT("00001"),TEXT("00010"),TEXT("00100"),TEXT("01000"),TEXT("11111")},
				{TEXT("11110"),TEXT("00001"),TEXT("00001"),TEXT("01110"),TEXT("00001"),TEXT("00001"),TEXT("11110")},
				{TEXT("00010"),TEXT("00110"),TEXT("01010"),TEXT("10010"),TEXT("11111"),TEXT("00010"),TEXT("00010")},
				{TEXT("11111"),TEXT("10000"),TEXT("10000"),TEXT("11110"),TEXT("00001"),TEXT("00001"),TEXT("11110")},
				{TEXT("01110"),TEXT("10000"),TEXT("10000"),TEXT("11110"),TEXT("10001"),TEXT("10001"),TEXT("01110")},
				{TEXT("11111"),TEXT("00001"),TEXT("00010"),TEXT("00100"),TEXT("01000"),TEXT("01000"),TEXT("01000")},
				{TEXT("01110"),TEXT("10001"),TEXT("10001"),TEXT("01110"),TEXT("10001"),TEXT("10001"),TEXT("01110")},
				{TEXT("01110"),TEXT("10001"),TEXT("10001"),TEXT("01111"),TEXT("00001"),TEXT("00001"),TEXT("01110")}
			};
			static const TCHAR* FRows[7] = {TEXT("11111"),TEXT("10000"),TEXT("10000"),TEXT("11110"),TEXT("10000"),TEXT("10000"),TEXT("10000")};
			static const TCHAR* DashRows[7] = {TEXT("00000"),TEXT("00000"),TEXT("00000"),TEXT("11110"),TEXT("00000"),TEXT("00000"),TEXT("00000")};
			if (Character >= TCHAR('0') && Character <= TCHAR('9')) return Digits[Character - TCHAR('0')][Row];
			if (Character == TCHAR('F') || Character == TCHAR('f')) return FRows[Row];
			if (Character == TCHAR('-')) return DashRows[Row];
			return Empty[Row];
		}
	}

	bool FCaptureImageProcessing::ResizePixelsMaxSize(const TArray<FColor>& InPixels, const FIntPoint& InSize, int32 MaxSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != static_cast<int64>(InSize.X) * InSize.Y) { OutError = PixelFailure(TEXT("invalid_pixels"), InPixels, InSize, TEXT("image_resize")); return false; }
		if (MaxSize <= 0) { OutPixels = InPixels; OutSize = InSize; return true; }
		MaxSize = FMath::Max(MaxSize, 1);
		if (InSize.X <= MaxSize && InSize.Y <= MaxSize) { OutPixels = InPixels; OutSize = InSize; return true; }
		const double Scale = static_cast<double>(MaxSize) / FMath::Max(InSize.X, InSize.Y);
		const int32 NewX = static_cast<int32>(FMath::Clamp<int64>(FMath::RoundToInt64(InSize.X * Scale), 1, MaxSize));
		const int32 NewY = static_cast<int32>(FMath::Clamp<int64>(FMath::RoundToInt64(InSize.Y * Scale), 1, MaxSize));
		return ResizePixelsToSize(InPixels, InSize, FIntPoint(NewX, NewY), OutPixels, OutSize, OutError);
	}

	bool FCaptureImageProcessing::ResizePixelsToSize(const TArray<FColor>& InPixels, const FIntPoint& InSize,
		const FIntPoint& RequestedSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != int64(InSize.X) * InSize.Y
			|| RequestedSize.X < 0 || RequestedSize.Y < 0)
		{
			OutError = InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != int64(InSize.X) * InSize.Y
				? PixelFailure(TEXT("invalid_pixels_or_size"), InPixels, InSize, TEXT("image_resize"))
				: CaptureDiagnostics::Native(TEXT("invalid_pixels_or_size"), TEXT("Requested image dimensions cannot be negative."), TEXT("image_resize"), TEXT("requested_size"), CaptureDiagnostics::Size(RequestedSize), MakeShared<FJsonValueString>(TEXT("non-negative width and height; zero preserves aspect ratio")));
			return false;
		}
		int64 Width = RequestedSize.X;
		int64 Height = RequestedSize.Y;
		if (!Width && !Height) { Width = InSize.X; Height = InSize.Y; }
		else if (!Width) Width = FMath::Max<int64>(1, FMath::RoundToInt64(double(Height) * InSize.X / InSize.Y));
		else if (!Height) Height = FMath::Max<int64>(1, FMath::RoundToInt64(double(Width) * InSize.Y / InSize.X));
		if (Width > MAX_int32 || Height > MAX_int32 || Width * Height > MAX_int32)
		{
			OutError = CaptureDiagnostics::Native(TEXT("image_size_exceeds_pixel_storage"), TEXT("The resized image exceeds the supported dimension or pixel count."), TEXT("image_resize"), TEXT("output_size"), CaptureDiagnostics::Facts({ { TEXT("width"), CaptureDiagnostics::Number(Width) }, { TEXT("height"), CaptureDiagnostics::Number(Height) } }), MakeShared<FJsonValueString>(TEXT("width, height and width * height at most 2147483647")));
			return false;
		}
		OutSize = FIntPoint(int32(Width), int32(Height));
		if (OutSize == InSize) { OutPixels = InPixels; return true; }
		OutPixels.SetNumUninitialized(int32(Width * Height));
		FImageUtils::ImageResize(InSize.X, InSize.Y, InPixels, OutSize.X, OutSize.Y, OutPixels, true);
		return true;
	}

	bool FCaptureImageProcessing::WriteCompressedImage(const TArray<FColor>& Pixels, const FIntPoint& Size, const FString& Format, int32 Quality, const FString& OutFilePath, UE::UAI::Contract::FDiagnostic& OutError, bool bForceOpaqueAlpha)
	{
		if (Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != static_cast<int64>(Size.X) * Size.Y) { OutError = PixelFailure(TEXT("invalid_pixels"), Pixels, Size, TEXT("image_encode")); return false; }
		const TArray<FColor>* PixelsToWrite = &Pixels;
		TArray<FColor> OpaquePixels;
		if (bForceOpaqueAlpha) { OpaquePixels = Pixels; for (FColor& Pixel : OpaquePixels) Pixel.A = 255; PixelsToWrite = &OpaquePixels; }
		TArray64<uint8> Compressed;
		const FImageView Image(const_cast<FColor*>(PixelsToWrite->GetData()), Size.X, Size.Y, ERawImageFormat::BGRA8);
		if (!FImageUtils::CompressImage(Compressed, *Format, Image, Quality)
			|| Compressed.IsEmpty())
		{
			OutError = CaptureDiagnostics::Native(TEXT("compress_failed"), TEXT("The image encoder failed or produced no encoded bytes."), TEXT("image_encode"), TEXT("encoding"), CaptureDiagnostics::Facts({ { TEXT("format"), MakeShared<FJsonValueString>(Format) }, { TEXT("quality"), CaptureDiagnostics::Number(Quality) }, { TEXT("encoded_bytes"), CaptureDiagnostics::Number(Compressed.Num()) } }), MakeShared<FJsonValueString>(TEXT("successful encoding with non-empty bytes")));
			OutError.TargetPath = OutFilePath;
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilePath), true);
		if (!FFileHelper::SaveArrayToFile(Compressed, *OutFilePath))
		{
			OutError = CaptureDiagnostics::Native(TEXT("write_file_failed"), TEXT("The encoded image could not be written to the output file."), TEXT("image_write"), TEXT("file_written"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = OutFilePath;
			return false;
		}
		return true;
	}

	bool FCaptureImageProcessing::WriteFrameSequenceSheetImage(const TArray<TArray<FColor>>& FramePixels, const TArray<FString>& FrameLabels, const FIntPoint& FrameSize, const TSharedPtr<FJsonObject>& Params, const FString& Format, int32 Quality, FString& OutFilePath, TSharedPtr<FJsonObject>& OutSheetData, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (FramePixels.IsEmpty() || FrameSize.X <= 0 || FrameSize.Y <= 0)
		{
			OutError = FramePixels.IsEmpty()
				? CaptureDiagnostics::Native(TEXT("frame_sheet_empty"), TEXT("A frame sheet requires at least one captured frame."), TEXT("frame_sheet"), TEXT("frame_count"), CaptureDiagnostics::Number(0), MakeShared<FJsonValueString>(TEXT("at least one frame")))
				: CaptureDiagnostics::Native(TEXT("frame_sheet_empty"), TEXT("The frame dimension must be positive."), TEXT("frame_sheet"),
					FrameSize.X <= 0 ? TEXT("frame_width") : TEXT("frame_height"), CaptureDiagnostics::Number(FrameSize.X <= 0 ? FrameSize.X : FrameSize.Y),
					MakeShared<FJsonValueString>(TEXT("positive pixel dimension")));
			return false;
		}
		for (int32 Index = 0; Index < FramePixels.Num(); ++Index)
		{
			if (FramePixels[Index].Num() != static_cast<int64>(FrameSize.X) * FrameSize.Y)
			{
				OutError = CaptureDiagnostics::Native(TEXT("frame_sheet_frame_size_mismatch"),
					TEXT("The captured frame pixel count differs from the frame-sheet dimensions."), TEXT("frame_sheet"),
					TEXT("frames/") + ::LexToString(Index) + TEXT("/pixel_count"),
					CaptureDiagnostics::Number(FramePixels[Index].Num()), CaptureDiagnostics::Number(static_cast<int64>(FrameSize.X) * FrameSize.Y));
				return false;
			}
		}
		const int32 Columns = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(FramePixels.Num()))));
		const int32 Rows = Columns;
		int32 Padding = 8, LabelScale = 3, SheetMaxSize = 0;
		if (!ReadSheetNumber(Params, TEXT("frame_sheet_padding"), TEXT("sheet_padding"), 0, Padding, OutError)
			|| !ReadSheetNumber(Params, TEXT("frame_sheet_label_scale"), TEXT("sheet_label_scale"), 1, LabelScale, OutError)
			|| !ReadSheetNumber(Params, TEXT("frame_sheet_max_size"), TEXT("sheet_max_size"), 1, SheetMaxSize, OutError)) return false;
		const int64 LabelMargin = FMath::Max<int64>(3, static_cast<int64>(LabelScale) * 2);
		const int64 Width64 = static_cast<int64>(Columns) * FrameSize.X + static_cast<int64>(Columns + 1) * Padding;
		const int64 Height64 = static_cast<int64>(Rows) * FrameSize.Y + static_cast<int64>(Rows + 1) * Padding;
		if (Width64 <= 0 || Height64 <= 0 || Width64 > MAX_int32 || Height64 > MAX_int32 || Width64 > MAX_int32 / Height64)
		{
			OutError = CaptureDiagnostics::Native(TEXT("frame_sheet_dimensions_unrepresentable"), TEXT("The computed frame-sheet dimensions exceed pixel storage constraints."), TEXT("frame_sheet"), TEXT("sheet_size"), CaptureDiagnostics::Facts({ { TEXT("width"), CaptureDiagnostics::Number(Width64) }, { TEXT("height"), CaptureDiagnostics::Number(Height64) } }), MakeShared<FJsonValueString>(TEXT("positive width, height and pixel count at most 2147483647")));
			return false;
		}
		const FIntPoint RawSize(static_cast<int32>(Width64), static_cast<int32>(Height64));
		TArray<FColor> SheetPixels;
		SheetPixels.Init(FColor(24, 24, 24, 255), RawSize.X * RawSize.Y);
		for (int32 Index = 0; Index < FramePixels.Num(); ++Index)
		{
			const int32 DestX = Padding + (Index % Columns) * (FrameSize.X + Padding);
			const int32 DestY = Padding + (Index / Columns) * (FrameSize.Y + Padding);
			for (int32 Y = 0; Y < FrameSize.Y; ++Y) FMemory::Memcpy(SheetPixels.GetData() + (DestY + Y) * RawSize.X + DestX, FramePixels[Index].GetData() + Y * FrameSize.X, sizeof(FColor) * FrameSize.X);
		}
		auto DrawRect = [&](int64 X, int64 Y, int64 W, int64 H, FColor Color)
		{
			const int32 Left = static_cast<int32>(FMath::Clamp<int64>(X, 0, RawSize.X));
			const int32 Top = static_cast<int32>(FMath::Clamp<int64>(Y, 0, RawSize.Y));
			const int32 Right = static_cast<int32>(FMath::Clamp<int64>(X + W, 0, RawSize.X));
			const int32 Bottom = static_cast<int32>(FMath::Clamp<int64>(Y + H, 0, RawSize.Y));
			for (int32 Py = Top; Py < Bottom; ++Py)
				for (int32 Px = Left; Px < Right; ++Px) SheetPixels[Py * RawSize.X + Px] = Color;
		};
		for (int32 Index = 0; Index < FramePixels.Num(); ++Index)
		{
			const int32 DestX = Padding + (Index % Columns) * (FrameSize.X + Padding);
			const int32 DestY = Padding + (Index / Columns) * (FrameSize.Y + Padding);
			const FString Label = FrameLabels.IsValidIndex(Index) && !FrameLabels[Index].IsEmpty() ? FrameLabels[Index] : FString::Printf(TEXT("F%d"), Index);
			const int64 AvailableWidth = FrameSize.X - LabelMargin * 2;
			const int64 AvailableHeight = FrameSize.Y - LabelMargin * 2;
			if (AvailableWidth <= 0 || AvailableHeight <= 0) continue;
			const int64 BgWidth = FMath::Min(AvailableWidth, static_cast<int64>(FMath::Max(1, Label.Len())) * LabelScale * 6 + LabelMargin * 2);
			const int64 BgHeight = FMath::Min(AvailableHeight, static_cast<int64>(LabelScale) * 7 + LabelMargin * 2);
			if (BgWidth <= 0 || BgHeight <= 0) continue;
			DrawRect(DestX + LabelMargin, DestY + LabelMargin, BgWidth, BgHeight, FColor(8, 8, 8, 255));
			int64 CursorX = DestX + LabelMargin * 2;
			for (const TCHAR Character : Label)
			{
				if (CursorX >= RawSize.X) break;
				for (int32 Gy = 0; Gy < 7; ++Gy) for (int32 Gx = 0; Gx < 5; ++Gx) if (Glyph(Character, Gy)[Gx] == TCHAR('1')) DrawRect(CursorX + static_cast<int64>(Gx) * LabelScale, DestY + LabelMargin * 2 + static_cast<int64>(Gy) * LabelScale, LabelScale, LabelScale, FColor(255, 230, 120, 255));
				CursorX += static_cast<int64>(LabelScale) * 6;
			}
		}
		TArray<FColor> OutputPixels; FIntPoint OutputSize;
		if (!ResizePixelsMaxSize(SheetPixels, RawSize, SheetMaxSize, OutputPixels, OutputSize, OutError)) return false;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("frame_sheet_output_file"), OutFilePath) || OutFilePath.TrimStartAndEnd().IsEmpty()) OutFilePath = MakeDefaultShotPath(Format);
		else OutFilePath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutFilePath);
		if (!WriteCompressedImage(OutputPixels, OutputSize, Format, Quality, OutFilePath, OutError)) return false;
		const int64 Bytes = IFileManager::Get().FileSize(*OutFilePath);
		OutSheetData = MakeShared<FJsonObject>();
		OutSheetData->SetBoolField(TEXT("created"), true); OutSheetData->SetStringField(TEXT("file_path"), OutFilePath);
		OutSheetData->SetNumberField(TEXT("width"), OutputSize.X); OutSheetData->SetNumberField(TEXT("height"), OutputSize.Y); OutSheetData->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		OutSheetData->SetNumberField(TEXT("columns"), Columns); OutSheetData->SetNumberField(TEXT("rows"), Rows); OutSheetData->SetNumberField(TEXT("frame_count"), FramePixels.Num());
		OutSheetData->SetNumberField(TEXT("tile_width"), FrameSize.X); OutSheetData->SetNumberField(TEXT("tile_height"), FrameSize.Y); OutSheetData->SetNumberField(TEXT("padding"), Padding); OutSheetData->SetNumberField(TEXT("label_scale"), LabelScale);
		OutSheetData->SetNumberField(TEXT("raw_width"), RawSize.X); OutSheetData->SetNumberField(TEXT("raw_height"), RawSize.Y); OutSheetData->SetNumberField(TEXT("max_size"), SheetMaxSize);
		OutSheetData->SetNumberField(TEXT("scale"), RawSize.X > 0 ? static_cast<double>(OutputSize.X) / static_cast<double>(RawSize.X) : 1.0);
		return true;
	}
}
