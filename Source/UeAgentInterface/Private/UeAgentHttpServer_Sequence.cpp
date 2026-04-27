// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Sequence.h"

#include "UeAgentJsonDiagnostics.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneDoubleSection.h"
#include "Sections/MovieSceneObjectPropertySection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneActorReferenceSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Sections/MovieSceneStringSection.h"
#include "Sections/MovieSceneSpawnSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Sections/MovieSceneRotatorSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneVisibilitySection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneActorReferenceTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "Tracks/MovieSceneRotatorTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "WidgetBlueprint.h"

namespace UeAgentSequenceOps
{
	enum class ESequencePropertyTrackType : uint8
	{
		Bool,
		Byte,
		Double,
		Float,
		Integer,
		Color,
		Vector,
		Rotator,
		ActorReference,
		Object,
		String
	};

	enum class ESequenceVectorPrecision : uint8
	{
		Float,
		Double
	};

	static bool ParseSequencePropertyTrackType(const FString& InText, ESequencePropertyTrackType& OutType)
	{
		FString Value = InText.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value == TEXT("bool") || Value == TEXT("boolean"))
		{
			OutType = ESequencePropertyTrackType::Bool;
			return true;
		}
		if (Value == TEXT("byte") || Value == TEXT("uint8") || Value == TEXT("enum"))
		{
			OutType = ESequencePropertyTrackType::Byte;
			return true;
		}
		if (Value == TEXT("double") || Value == TEXT("float64"))
		{
			OutType = ESequencePropertyTrackType::Double;
			return true;
		}
		if (Value == TEXT("float"))
		{
			OutType = ESequencePropertyTrackType::Float;
			return true;
		}
		if (Value == TEXT("int") || Value == TEXT("integer") || Value == TEXT("int32"))
		{
			OutType = ESequencePropertyTrackType::Integer;
			return true;
		}
		if (Value == TEXT("color") || Value == TEXT("linearcolor") || Value == TEXT("flinearcolor"))
		{
			OutType = ESequencePropertyTrackType::Color;
			return true;
		}
		if (Value == TEXT("vector") ||
			Value == TEXT("vector2") ||
			Value == TEXT("vector3") ||
			Value == TEXT("vector4") ||
			Value == TEXT("vector2d") ||
			Value == TEXT("vector3d") ||
			Value == TEXT("vector4d") ||
			Value == TEXT("vector2f") ||
			Value == TEXT("vector3f") ||
			Value == TEXT("vector4f") ||
			Value == TEXT("floatvector") ||
			Value == TEXT("doublevector"))
		{
			OutType = ESequencePropertyTrackType::Vector;
			return true;
		}
		if (Value == TEXT("rotator") || Value == TEXT("frotator"))
		{
			OutType = ESequencePropertyTrackType::Rotator;
			return true;
		}
		if (Value == TEXT("actorreference") || Value == TEXT("actor_reference") || Value == TEXT("actorref") || Value == TEXT("actorbinding") || Value == TEXT("actor"))
		{
			OutType = ESequencePropertyTrackType::ActorReference;
			return true;
		}
		if (Value == TEXT("object") || Value == TEXT("objectreference") || Value == TEXT("objectpath") || Value == TEXT("assetreference"))
		{
			OutType = ESequencePropertyTrackType::Object;
			return true;
		}
		if (Value == TEXT("string") || Value == TEXT("fstring"))
		{
			OutType = ESequencePropertyTrackType::String;
			return true;
		}
		return false;
	}

	static bool ParseSequenceVectorPrecision(const FString& InText, ESequenceVectorPrecision& OutPrecision)
	{
		FString Value = InText.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value == TEXT("float") || Value == TEXT("float32") || Value == TEXT("single") || Value == TEXT("f"))
		{
			OutPrecision = ESequenceVectorPrecision::Float;
			return true;
		}
		if (Value == TEXT("double") || Value == TEXT("float64") || Value == TEXT("d"))
		{
			OutPrecision = ESequenceVectorPrecision::Double;
			return true;
		}
		return false;
	}

	static void InferSequenceVectorDefaultsFromTypeText(
		const FString& InText,
		bool& bOutHasPrecision,
		ESequenceVectorPrecision& OutPrecision,
		bool& bOutHasChannels,
		int32& OutChannels)
	{
		bOutHasPrecision = false;
		bOutHasChannels = false;
		OutPrecision = ESequenceVectorPrecision::Double;
		OutChannels = 3;

		FString Value = InText.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value == TEXT("vector2"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 2;
			return;
		}
		if (Value == TEXT("vector") || Value == TEXT("vector3"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 3;
			return;
		}
		if (Value == TEXT("vector4"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 4;
			return;
		}
		if (Value == TEXT("vector2d"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 2;
			return;
		}
		if (Value == TEXT("vector3d") || Value == TEXT("doublevector"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 3;
			return;
		}
		if (Value == TEXT("vector4d"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Double;
			OutChannels = 4;
			return;
		}
		if (Value == TEXT("vector2f"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Float;
			OutChannels = 2;
			return;
		}
		if (Value == TEXT("vector3f") || Value == TEXT("floatvector"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Float;
			OutChannels = 3;
			return;
		}
		if (Value == TEXT("vector4f"))
		{
			bOutHasPrecision = true;
			bOutHasChannels = true;
			OutPrecision = ESequenceVectorPrecision::Float;
			OutChannels = 4;
		}
	}

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
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

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static ULevelSequence* LoadLevelSequence(const FString& InPath)
	{
		return Cast<ULevelSequence>(LoadAssetObject(InPath));
	}

	static UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
	{
		return Cast<UWidgetBlueprint>(LoadAssetObject(InPath));
	}

	static UAnimSequenceBase* LoadAnimSequenceBase(const FString& InPath)
	{
		return Cast<UAnimSequenceBase>(LoadAssetObject(InPath));
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}

		FString OutputPath = OutputPathInput;
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!OutputDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}

		OutOutputPath = OutputPath;
		return true;
	}

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();

		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}

		InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		OutResolvedPath = InputPath;
		return true;
	}

	static bool LoadOptionalJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();

		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			return true;
		}

		InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			return true;
		}

		FString LoadError;
		if (!LoadJsonFile(InputPath, OutRoot, OutResolvedPath, LoadError))
		{
			OutError = FString::Printf(TEXT("%s:%s"), *LoadError, *InputPath);
			return false;
		}
		return true;
	}

	static bool SetMovieScenePlaybackRangeSeconds(UMovieScene* MovieScene, const double StartSeconds, const double DurationSeconds)
	{
		if (!MovieScene || DurationSeconds <= 0.0)
		{
			return false;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		FFrameNumber StartFrame = TickResolution.AsFrameTime(StartSeconds).RoundToFrame();
		FFrameNumber EndExclusiveFrame = TickResolution.AsFrameTime(StartSeconds + DurationSeconds).CeilToFrame();
		if (EndExclusiveFrame.Value <= StartFrame.Value)
		{
			EndExclusiveFrame = FFrameNumber(StartFrame.Value + 1);
		}

		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndExclusiveFrame));
		return true;
	}

	static UWidgetAnimation* FindWidgetAnimationByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& InName)
	{
		if (!WidgetBlueprint || InName.IsEmpty())
		{
			return nullptr;
		}

		const FString Needle = InName.TrimStartAndEnd();
		for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (!Animation)
			{
				continue;
			}
			if (Animation->GetName().Equals(Needle, ESearchCase::IgnoreCase))
			{
				return Animation;
			}
#if WITH_EDITOR
			if (Animation->GetDisplayLabel().Equals(Needle, ESearchCase::IgnoreCase))
			{
				return Animation;
			}
#endif
		}
		return nullptr;
	}

	static UWidget* FindWidgetByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& InName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || InName.IsEmpty())
		{
			return nullptr;
		}

		const FString Needle = InName.TrimStartAndEnd();
		if (UWidget* Exact = WidgetBlueprint->WidgetTree->FindWidget(FName(*Needle)))
		{
			return Exact;
		}

		TArray<UWidget*> Widgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (Widget && Widget->GetName().Equals(Needle, ESearchCase::IgnoreCase))
			{
				return Widget;
			}
		}
		return nullptr;
	}

	static int32 GetFloatChannelKeyCount(const FMovieSceneFloatChannel& Channel)
	{
		return Channel.GetData().GetTimes().Num();
	}

	static int32 GetDoubleChannelKeyCount(const FMovieSceneDoubleChannel& Channel)
	{
		return Channel.GetData().GetTimes().Num();
	}

	static FString LexToString(const ESequenceVectorPrecision Precision)
	{
		return Precision == ESequenceVectorPrecision::Float ? TEXT("float") : TEXT("double");
	}

	static void PopulateSectionRangeFields(const UMovieSceneSection* Section, TSharedPtr<FJsonObject>& OutObject)
	{
		if (!Section || !OutObject.IsValid())
		{
			return;
		}

		const TRange<FFrameNumber> Range = Section->GetRange();
		OutObject->SetBoolField(TEXT("has_start_frame"), Range.HasLowerBound());
		OutObject->SetBoolField(TEXT("has_end_frame"), Range.HasUpperBound());
		OutObject->SetNumberField(TEXT("start_frame"), Range.HasLowerBound() ? Range.GetLowerBoundValue().Value : 0);
		OutObject->SetNumberField(TEXT("end_exclusive_frame"), Range.HasUpperBound() ? Range.GetUpperBoundValue().Value : 0);
	}

	static TSharedPtr<FJsonObject> BuildMovieSceneTrackSummary(UMovieScene* MovieScene, UMovieSceneTrack* Track)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("class"), Track ? Track->GetClass()->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("name"), Track ? Track->GetName() : TEXT(""));
		Item->SetNumberField(TEXT("section_count"), Track ? Track->GetAllSections().Num() : 0);

		if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
		{
			Item->SetStringField(TEXT("property_name"), PropertyTrack->GetPropertyName().ToString());
			Item->SetStringField(TEXT("property_path"), PropertyTrack->GetPropertyPath().ToString());
		}
		if (UMovieSceneByteTrack* ByteTrack = Cast<UMovieSceneByteTrack>(Track))
		{
			Item->SetStringField(TEXT("enum_path"), ByteTrack->GetEnum() ? ByteTrack->GetEnum()->GetPathName() : TEXT(""));
		}
		if (UMovieSceneObjectPropertyTrack* ObjectTrack = Cast<UMovieSceneObjectPropertyTrack>(Track))
		{
			Item->SetStringField(TEXT("property_class_path"), ObjectTrack->PropertyClass ? ObjectTrack->PropertyClass->GetPathName() : TEXT(""));
		}
		if (UMovieSceneFloatVectorTrack* FloatVectorTrack = Cast<UMovieSceneFloatVectorTrack>(Track))
		{
			Item->SetStringField(TEXT("vector_precision"), TEXT("float"));
			Item->SetNumberField(TEXT("channels_used"), FloatVectorTrack->GetNumChannelsUsed());
		}
		if (UMovieSceneDoubleVectorTrack* DoubleVectorTrack = Cast<UMovieSceneDoubleVectorTrack>(Track))
		{
			Item->SetStringField(TEXT("vector_precision"), TEXT("double"));
			Item->SetNumberField(TEXT("channels_used"), DoubleVectorTrack->GetNumChannelsUsed());
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		if (Track)
		{
			const TArray<UMovieSceneSection*>& AllSections = Track->GetAllSections();
			for (int32 SectionIndex = 0; SectionIndex < AllSections.Num(); ++SectionIndex)
			{
				UMovieSceneSection* Section = AllSections[SectionIndex];
				if (!Section)
				{
					continue;
				}

				TSharedPtr<FJsonObject> SectionObject = MakeShared<FJsonObject>();
				SectionObject->SetNumberField(TEXT("section_index"), SectionIndex);
				SectionObject->SetStringField(TEXT("class"), Section->GetClass()->GetPathName());
				PopulateSectionRangeFields(Section, SectionObject);

				if (UMovieSceneSkeletalAnimationSection* SkeletalAnimationSection = Cast<UMovieSceneSkeletalAnimationSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("skeletal_animation"));
					SectionObject->SetStringField(TEXT("animation_asset"), SkeletalAnimationSection->Params.Animation ? SkeletalAnimationSection->Params.Animation->GetPathName() : TEXT(""));
					SectionObject->SetStringField(TEXT("slot_name"), SkeletalAnimationSection->Params.SlotName.ToString());
					SectionObject->SetNumberField(TEXT("row_index"), SkeletalAnimationSection->GetRowIndex());
					SectionObject->SetNumberField(
						TEXT("play_rate"),
						SkeletalAnimationSection->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate
							? SkeletalAnimationSection->Params.PlayRate.AsFixedPlayRateFloat()
							: 0.0f);
					SectionObject->SetStringField(
						TEXT("play_rate_type"),
						SkeletalAnimationSection->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate ? TEXT("fixed") : TEXT("custom"));
					SectionObject->SetBoolField(TEXT("reverse"), SkeletalAnimationSection->Params.bReverse);
					SectionObject->SetBoolField(TEXT("skip_anim_notifiers"), SkeletalAnimationSection->Params.bSkipAnimNotifiers);
					SectionObject->SetBoolField(TEXT("force_custom_mode"), SkeletalAnimationSection->Params.bForceCustomMode);
					SectionObject->SetNumberField(TEXT("first_loop_start_offset_frames"), SkeletalAnimationSection->Params.FirstLoopStartFrameOffset.Value);
					SectionObject->SetNumberField(TEXT("start_offset_frames"), SkeletalAnimationSection->Params.StartFrameOffset.Value);
					SectionObject->SetNumberField(TEXT("end_offset_frames"), SkeletalAnimationSection->Params.EndFrameOffset.Value);
				}
				else if (UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("transform"));
					TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
					SectionObject->SetNumberField(TEXT("channel_count"), Channels.Num());
				}
				else if (UMovieScene2DTransformSection* Transform2DSection = Cast<UMovieScene2DTransformSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("widget_transform"));
					SectionObject->SetNumberField(TEXT("translation_x_key_count"), GetFloatChannelKeyCount(Transform2DSection->Translation[0]));
					SectionObject->SetNumberField(TEXT("translation_y_key_count"), GetFloatChannelKeyCount(Transform2DSection->Translation[1]));
					SectionObject->SetNumberField(TEXT("rotation_key_count"), GetFloatChannelKeyCount(Transform2DSection->Rotation));
					SectionObject->SetNumberField(TEXT("scale_x_key_count"), GetFloatChannelKeyCount(Transform2DSection->Scale[0]));
					SectionObject->SetNumberField(TEXT("scale_y_key_count"), GetFloatChannelKeyCount(Transform2DSection->Scale[1]));
					SectionObject->SetNumberField(TEXT("shear_x_key_count"), GetFloatChannelKeyCount(Transform2DSection->Shear[0]));
					SectionObject->SetNumberField(TEXT("shear_y_key_count"), GetFloatChannelKeyCount(Transform2DSection->Shear[1]));
				}
				else if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("float"));
					SectionObject->SetNumberField(TEXT("key_count"), GetFloatChannelKeyCount(FloatSection->GetChannel()));
				}
				else if (UMovieSceneDoubleSection* DoubleSection = Cast<UMovieSceneDoubleSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("double"));
					SectionObject->SetNumberField(TEXT("key_count"), GetDoubleChannelKeyCount(DoubleSection->GetChannel()));
				}
				else if (UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("byte"));
					SectionObject->SetNumberField(TEXT("key_count"), ByteSection->ByteCurve.GetData().GetTimes().Num());
				}
				else if (UMovieSceneStringSection* StringSection = Cast<UMovieSceneStringSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("string"));
					SectionObject->SetNumberField(TEXT("key_count"), StringSection->GetChannel().GetData().GetTimes().Num());
				}
				else if (UMovieSceneVisibilitySection* VisibilitySection = Cast<UMovieSceneVisibilitySection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("visibility"));
					SectionObject->SetNumberField(TEXT("key_count"), VisibilitySection->GetChannel().GetData().GetTimes().Num());
				}
				else if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("bool"));
					SectionObject->SetNumberField(TEXT("key_count"), BoolSection->GetChannel().GetData().GetTimes().Num());
				}
				else if (UMovieSceneIntegerSection* IntegerSection = Cast<UMovieSceneIntegerSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("integer"));
					TArrayView<FMovieSceneIntegerChannel*> Channels = IntegerSection->GetChannelProxy().GetChannels<FMovieSceneIntegerChannel>();
					SectionObject->SetNumberField(TEXT("key_count"), Channels.Num() > 0 ? Channels[0]->GetData().GetTimes().Num() : 0);
				}
				else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("color"));
					SectionObject->SetNumberField(TEXT("red_key_count"), GetFloatChannelKeyCount(ColorSection->GetRedChannel()));
					SectionObject->SetNumberField(TEXT("green_key_count"), GetFloatChannelKeyCount(ColorSection->GetGreenChannel()));
					SectionObject->SetNumberField(TEXT("blue_key_count"), GetFloatChannelKeyCount(ColorSection->GetBlueChannel()));
					SectionObject->SetNumberField(TEXT("alpha_key_count"), GetFloatChannelKeyCount(ColorSection->GetAlphaChannel()));
				}
				else if (UMovieSceneFloatVectorSection* FloatVectorSection = Cast<UMovieSceneFloatVectorSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("vector"));
					SectionObject->SetStringField(TEXT("vector_precision"), TEXT("float"));
					SectionObject->SetNumberField(TEXT("channels_used"), FloatVectorSection->GetChannelsUsed());
					SectionObject->SetNumberField(TEXT("x_key_count"), GetFloatChannelKeyCount(FloatVectorSection->GetChannel(0)));
					SectionObject->SetNumberField(TEXT("y_key_count"), GetFloatChannelKeyCount(FloatVectorSection->GetChannel(1)));
					if (FloatVectorSection->GetChannelsUsed() >= 3)
					{
						SectionObject->SetNumberField(TEXT("z_key_count"), GetFloatChannelKeyCount(FloatVectorSection->GetChannel(2)));
					}
					if (FloatVectorSection->GetChannelsUsed() >= 4)
					{
						SectionObject->SetNumberField(TEXT("w_key_count"), GetFloatChannelKeyCount(FloatVectorSection->GetChannel(3)));
					}
				}
				else if (UMovieSceneDoubleVectorSection* DoubleVectorSection = Cast<UMovieSceneDoubleVectorSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("vector"));
					SectionObject->SetStringField(TEXT("vector_precision"), TEXT("double"));
					SectionObject->SetNumberField(TEXT("channels_used"), DoubleVectorSection->GetChannelsUsed());
					SectionObject->SetNumberField(TEXT("x_key_count"), GetDoubleChannelKeyCount(DoubleVectorSection->GetChannel(0)));
					SectionObject->SetNumberField(TEXT("y_key_count"), GetDoubleChannelKeyCount(DoubleVectorSection->GetChannel(1)));
					if (DoubleVectorSection->GetChannelsUsed() >= 3)
					{
						SectionObject->SetNumberField(TEXT("z_key_count"), GetDoubleChannelKeyCount(DoubleVectorSection->GetChannel(2)));
					}
					if (DoubleVectorSection->GetChannelsUsed() >= 4)
					{
						SectionObject->SetNumberField(TEXT("w_key_count"), GetDoubleChannelKeyCount(DoubleVectorSection->GetChannel(3)));
					}
				}
				else if (UMovieSceneRotatorSection* RotatorSection = Cast<UMovieSceneRotatorSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("rotator"));
					SectionObject->SetNumberField(TEXT("pitch_key_count"), GetDoubleChannelKeyCount(RotatorSection->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex)));
					SectionObject->SetNumberField(TEXT("yaw_key_count"), GetDoubleChannelKeyCount(RotatorSection->GetChannel(UMovieSceneRotatorSection::YawChannelIndex)));
					SectionObject->SetNumberField(TEXT("roll_key_count"), GetDoubleChannelKeyCount(RotatorSection->GetChannel(UMovieSceneRotatorSection::RollChannelIndex)));
				}
				else if (UMovieSceneActorReferenceSection* ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("actor_reference"));
					const FMovieSceneActorReferenceData& ActorReferenceData = ActorReferenceSection->GetActorReferenceData();
					SectionObject->SetNumberField(TEXT("key_count"), ActorReferenceData.GetData().GetTimes().Num());
					SectionObject->SetStringField(TEXT("default_binding_guid"), ActorReferenceData.GetDefault().Object.GetGuid().IsValid() ? ActorReferenceData.GetDefault().Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT(""));
					SectionObject->SetStringField(TEXT("default_component_name"), ActorReferenceData.GetDefault().ComponentName.ToString());
					SectionObject->SetStringField(TEXT("default_socket_name"), ActorReferenceData.GetDefault().SocketName.ToString());
				}
				else if (UMovieSceneObjectPropertySection* ObjectSection = Cast<UMovieSceneObjectPropertySection>(Section))
				{
					SectionObject->SetStringField(TEXT("section_type"), TEXT("object"));
					SectionObject->SetNumberField(TEXT("key_count"), ObjectSection->ObjectChannel.GetData().GetTimes().Num());
					const UObject* DefaultObject = ObjectSection->ObjectChannel.GetDefault().Get();
					SectionObject->SetStringField(TEXT("default_value_path"), DefaultObject ? DefaultObject->GetPathName() : TEXT(""));
				}

				Sections.Add(MakeShared<FJsonValueObject>(SectionObject));
			}
		}

		Item->SetArrayField(TEXT("sections"), Sections);
		return Item;
	}

	template<typename TTrackType>
	static TTrackType* FindPropertyTrackByNameAndPath(UMovieScene* MovieScene, const FGuid& BindingGuid, const FName PropertyName, const FString& PropertyPath)
	{
		if (!MovieScene)
		{
			return nullptr;
		}

		if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				TTrackType* TypedTrack = Cast<TTrackType>(Track);
				UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
				if (!TypedTrack || !PropertyTrack)
				{
					continue;
				}

				if (PropertyTrack->GetPropertyName() == PropertyName &&
					(PropertyPath.IsEmpty() || PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::IgnoreCase)))
				{
					return TypedTrack;
				}
			}
		}

		return MovieScene->FindTrack<TTrackType>(BindingGuid, PropertyName);
	}

	static TSharedPtr<FJsonObject> BuildMovieSceneBindingSummary(UMovieScene* MovieScene, const FMovieSceneBinding& Binding)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("guid"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Item->SetStringField(TEXT("name"), Binding.GetName());
		Item->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());

		TArray<TSharedPtr<FJsonValue>> Tracks;
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (Track)
			{
				Tracks.Add(MakeShared<FJsonValueObject>(BuildMovieSceneTrackSummary(MovieScene, Track)));
			}
		}
		Item->SetArrayField(TEXT("tracks"), Tracks);
		return Item;
	}

	static bool FindOrCreateWidgetAnimationBinding(
		UWidgetBlueprint* WidgetBlueprint,
		UWidgetAnimation* TargetAnimation,
		UWidget* TargetWidget,
		UMovieScene*& OutMovieScene,
		FGuid& OutBindingGuid,
		bool& bOutBindingAlreadyExists,
		FString& OutError)
	{
		OutMovieScene = TargetAnimation ? TargetAnimation->GetMovieScene() : nullptr;
		OutBindingGuid.Invalidate();
		bOutBindingAlreadyExists = false;

		if (!WidgetBlueprint || !TargetAnimation || !TargetWidget)
		{
			OutError = TEXT("invalid_widget_animation_context");
			return false;
		}
		if (!OutMovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}

		for (const FWidgetAnimationBinding& Binding : TargetAnimation->AnimationBindings)
		{
			if (Binding.WidgetName == TargetWidget->GetFName())
			{
				OutBindingGuid = Binding.AnimationGuid;
				bOutBindingAlreadyExists = true;
				break;
			}
		}

		if (!OutBindingGuid.IsValid() || !OutMovieScene->FindBinding(OutBindingGuid))
		{
			OutBindingGuid = OutMovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
			if (!OutBindingGuid.IsValid())
			{
				OutError = TEXT("add_widget_possessable_failed");
				return false;
			}

			FWidgetAnimationBinding NewBinding;
			NewBinding.WidgetName = TargetWidget->GetFName();
			NewBinding.AnimationGuid = OutBindingGuid;
			NewBinding.bIsRootWidget = (WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget == TargetWidget);
			TargetAnimation->AnimationBindings.Add(NewBinding);
			bOutBindingAlreadyExists = false;
		}

		return true;
	}

	static bool FindOrCreateUmgTransformSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		UMovieScene2DTransformTrack*& OutTrack,
		UMovieScene2DTransformSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		FString& OutError)
	{
		OutTrack = nullptr;
		OutSection = nullptr;
		bOutTrackAlreadyExists = false;
		if (!MovieScene || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_widget_transform_context");
			return false;
		}

		OutTrack = MovieScene->FindTrack<UMovieScene2DTransformTrack>(BindingGuid);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack)
		{
			OutTrack = MovieScene->AddTrack<UMovieScene2DTransformTrack>(BindingGuid);
			if (!OutTrack)
			{
				OutError = TEXT("create_umg_transform_track_failed");
				return false;
			}
			OutTrack->SetPropertyNameAndPath(TEXT("RenderTransform"), TEXT("RenderTransform"));
		}

		const TArray<UMovieSceneSection*>& ExistingSections = OutTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			OutSection = Cast<UMovieScene2DTransformSection>(ExistingSections[0]);
		}
		if (!OutSection)
		{
			OutSection = Cast<UMovieScene2DTransformSection>(OutTrack->CreateNewSection());
			if (!OutSection)
			{
				OutError = TEXT("create_umg_transform_section_failed");
				return false;
			}
			OutTrack->AddSection(*OutSection);
		}

		return true;
	}

	static bool FindOrCreateUmgOpacitySection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		UMovieSceneFloatTrack*& OutTrack,
		UMovieSceneFloatSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		FString& OutError)
	{
		OutTrack = nullptr;
		OutSection = nullptr;
		bOutTrackAlreadyExists = false;
		if (!MovieScene || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_widget_opacity_context");
			return false;
		}

		const FName TrackName(TEXT("RenderOpacity"));
		OutTrack = MovieScene->FindTrack<UMovieSceneFloatTrack>(BindingGuid, TrackName);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack)
		{
			OutTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
			if (!OutTrack)
			{
				OutError = TEXT("create_umg_opacity_track_failed");
				return false;
			}
			OutTrack->SetPropertyNameAndPath(TrackName, TEXT("RenderOpacity"));
		}

		const TArray<UMovieSceneSection*>& ExistingSections = OutTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			OutSection = Cast<UMovieSceneFloatSection>(ExistingSections[0]);
		}
		if (!OutSection)
		{
			OutSection = Cast<UMovieSceneFloatSection>(OutTrack->CreateNewSection());
			if (!OutSection)
			{
				OutError = TEXT("create_umg_opacity_section_failed");
				return false;
			}
			OutTrack->AddSection(*OutSection);
		}

		return true;
	}

	static bool FindOrCreateUmgFloatSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FName PropertyName,
		const FString& PropertyPath,
		UMovieSceneFloatTrack*& OutTrack,
		UMovieSceneFloatSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		FString& OutError)
	{
		OutTrack = nullptr;
		OutSection = nullptr;
		bOutTrackAlreadyExists = false;
		if (!MovieScene || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_widget_float_context");
			return false;
		}

		OutTrack = MovieScene->FindTrack<UMovieSceneFloatTrack>(BindingGuid, PropertyName);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack)
		{
			OutTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
			if (!OutTrack)
			{
				OutError = TEXT("create_umg_float_track_failed");
				return false;
			}
			OutTrack->SetPropertyNameAndPath(PropertyName, PropertyPath);
		}

		const TArray<UMovieSceneSection*>& ExistingSections = OutTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			OutSection = Cast<UMovieSceneFloatSection>(ExistingSections[0]);
		}
		if (!OutSection)
		{
			OutSection = Cast<UMovieSceneFloatSection>(OutTrack->CreateNewSection());
			if (!OutSection)
			{
				OutError = TEXT("create_umg_float_section_failed");
				return false;
			}
			OutTrack->AddSection(*OutSection);
		}

		return true;
	}

	static bool FindOrCreateUmgColorSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FName PropertyName,
		const FString& PropertyPath,
		UMovieSceneColorTrack*& OutTrack,
		UMovieSceneColorSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		FString& OutError)
	{
		OutTrack = nullptr;
		OutSection = nullptr;
		bOutTrackAlreadyExists = false;
		if (!MovieScene || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_widget_color_context");
			return false;
		}

		OutTrack = MovieScene->FindTrack<UMovieSceneColorTrack>(BindingGuid, PropertyName);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack)
		{
			OutTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
			if (!OutTrack)
			{
				OutError = TEXT("create_umg_color_track_failed");
				return false;
			}
			OutTrack->SetPropertyNameAndPath(PropertyName, PropertyPath);
		}

		const TArray<UMovieSceneSection*>& ExistingSections = OutTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			OutSection = Cast<UMovieSceneColorSection>(ExistingSections[0]);
		}
		if (!OutSection)
		{
			OutSection = Cast<UMovieSceneColorSection>(OutTrack->CreateNewSection());
			if (!OutSection)
			{
				OutError = TEXT("create_umg_color_section_failed");
				return false;
			}
			OutTrack->AddSection(*OutSection);
		}

		return true;
	}
}

#include "UeAgentHttpServer_Sequence_DispatchAndLevelSequence.inl"
#include "UeAgentHttpServer_Sequence_BindingAndTransformTrack.inl"
#include "UeAgentHttpServer_Sequence_UmgAnimation.inl"
#include "UeAgentHttpServer_Sequence_FolderFormat.inl"
