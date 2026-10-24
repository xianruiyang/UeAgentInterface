#include "DomainAdapters/CaptureMediaLongJob/UeAgentAudioAssetEditorCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetEditorImageCapture.h"
#include "Dom/JsonObject.h"
#include "Metasound.h"
#include "MetasoundSource.h"
#include "Misc/PackageName.h"
#include "Sound/SoundCue.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		FString ToObjectPath(const FString& InputPath)
		{
			FString ObjectPath = InputPath;
			ObjectPath.TrimStartAndEndInline();
			if (ObjectPath.IsEmpty() || ObjectPath.Contains(TEXT(".")))
			{
				return ObjectPath;
			}
			const FString AssetName =
				FPackageName::GetLongPackageAssetName(ObjectPath);
			return AssetName.IsEmpty()
				? FString()
				: FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
		}

		UObject* LoadAsset(const FString& AssetPath)
		{
			const FString ObjectPath = ToObjectPath(AssetPath);
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
			{
				return Existing;
			}
			return LoadObject<UObject>(nullptr, *ObjectPath);
		}

		void FillAssetIdentity(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& OutData)
		{
			if (!Asset || !OutData.IsValid())
			{
				return;
			}
			OutData->SetStringField(
				TEXT("asset_path"), Asset->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
			OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
			OutData->SetStringField(
				TEXT("class_path"), Asset->GetClass()->GetPathName());
		}

		TSharedPtr<FJsonObject> MakeCaptureParameters(
			const TSharedPtr<FJsonObject>& Parameters)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Parameters.IsValid())
			{
				Result->Values = Parameters->Values;
			}
			return Result;
		}

		bool TryCaptureAudioAsset(
			const TSharedPtr<FJsonObject>& Parameters,
			const FString& AssetNotFoundError,
			const FString& ExpectedAssetClasses,
			const TFunctionRef<bool(UObject*)> IsSupportedAsset,
			const TFunctionRef<void(UObject*, const TSharedPtr<FJsonObject>&)>
				ProjectDomainResult,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutData.Reset();
			FString AssetPath;
			if (!Parameters.IsValid()
				|| !Parameters->TryGetStringField(TEXT("asset_path"), AssetPath)
				|| AssetPath.IsEmpty())
			{
				OutError = CaptureDiagnostics::Input(TEXT("missing_asset_path"), TEXT("Audio asset capture requires a non-empty asset_path."),
					TEXT("asset_capture"), TEXT("/asset_path"), Parameters.IsValid() ? Parameters->TryGetField(TEXT("asset_path")) : nullptr,
					MakeShared<FJsonValueString>(TEXT("non-empty audio asset path")));
				return false;
			}
			UObject* Asset = LoadAsset(AssetPath);
			if (!Asset || !IsSupportedAsset(Asset))
			{
				OutError = CaptureDiagnostics::Input(AssetNotFoundError,
					Asset ? TEXT("The requested asset class is unsupported by this audio capture command.") : TEXT("The requested audio asset could not be loaded."),
					TEXT("asset_capture"), TEXT("/asset_path"),
					Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(AssetPath)),
					MakeShared<FJsonValueString>(ExpectedAssetClasses));
                OutError.TargetPath = AssetPath;
				return false;
			}

			const TSharedPtr<FJsonObject> CaptureParameters =
				MakeCaptureParameters(Parameters);
			if (!TryCaptureAssetEditorImage(
					CaptureParameters,
					AssetNotFoundError,
					OutData,
					OutError))
			{
				return false;
			}
			FString FilePath;
			bool bAppearsNonBlack = false;
			OutData->TryGetStringField(TEXT("file_path"), FilePath);
			OutData->TryGetBoolField(
				TEXT("appears_non_black"), bAppearsNonBlack);

			TSharedPtr<FJsonObject> ProjectedResult = MakeShared<FJsonObject>();
			FillAssetIdentity(Asset, ProjectedResult);
			ProjectedResult->SetStringField(TEXT("file_path"), FilePath);
			ProjectedResult->SetBoolField(
				TEXT("appears_non_black"), bAppearsNonBlack);
			ProjectedResult->SetStringField(
				TEXT("capture_mode"), TEXT("offscreen_widget_renderer"));
			ProjectDomainResult(Asset, ProjectedResult);
			OutData = MoveTemp(ProjectedResult);
			return true;
		}
	}

	bool TryCaptureMetaSoundEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TryCaptureAudioAsset(
			Parameters,
			AssetNotFoundError,
			UMetaSoundSource::StaticClass()->GetPathName() + TEXT(" or ") + UMetaSoundPatch::StaticClass()->GetPathName(),
			[](UObject* Asset)
			{
				return Asset->IsA<UMetaSoundSource>()
					|| Asset->IsA<UMetaSoundPatch>();
			},
			[](UObject*, const TSharedPtr<FJsonObject>&)
			{
			},
			OutData,
			OutError);
	}

	bool TryCaptureSoundCueEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TryCaptureAudioAsset(
			Parameters,
			AssetNotFoundError,
			USoundCue::StaticClass()->GetPathName(),
			[](UObject* Asset)
			{
				return Asset->IsA<USoundCue>();
			},
			[](UObject* Asset, const TSharedPtr<FJsonObject>& Result)
			{
				Result->SetNumberField(
					TEXT("node_count"), CastChecked<USoundCue>(Asset)->AllNodes.Num());
			},
			OutData,
			OutError);
	}
}
