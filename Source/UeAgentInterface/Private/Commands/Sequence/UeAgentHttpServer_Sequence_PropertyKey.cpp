// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Sequence.h"
#include "UeAgentHttpServer_SequenceOps.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/ActorComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/Widget.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Camera/CameraComponent.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "MovieSceneSequencePlayer.h"
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
#include "UeAgentInterfaceSettings.h"
#include "WidgetBlueprint.h"

bool FUeAgentLegacyMechanics::CmdSequenceAddPropertyKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString PropertyTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_type"), PropertyTypeText) || PropertyTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_type");
		return false;
	}

	UeAgentSequenceOps::ESequencePropertyTrackType PropertyType;
	if (!UeAgentSequenceOps::ParseSequencePropertyTrackType(PropertyTypeText, PropertyType))
	{
		OutError = TEXT("invalid_property_type");
		return false;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Bool)
	{
		return CmdSequenceAddBoolPropertyKey(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
		// handled below through unified byte path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		// handled below through unified vector path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		// handled below through unified rotator path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
		// handled below through unified actor reference path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Float)
	{
		return CmdSequenceAddFloatPropertyKey(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Integer)
	{
		return CmdSequenceAddIntegerPropertyKey(Ctx, OutData, OutError);
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	auto ResolveObjectPropertyClassAndValue = [&](UClass*& OutClass, UObject*& OutValueObject, UObject*& OutValueBeforeKeyObject) -> bool
	{
		OutClass = nullptr;
		OutValueObject = nullptr;
		OutValueBeforeKeyObject = nullptr;

		FString ValuePath;
		JsonTryGetString(Ctx.Params, TEXT("value_path"), ValuePath);
		if (ValuePath.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_path"), ValuePath);
		}
		ValuePath = ValuePath.TrimStartAndEnd();
		if (!ValuePath.IsEmpty())
		{
			OutValueObject = UeAgentSequenceOps::LoadAssetObject(ValuePath);
			if (!OutValueObject)
			{
				OutError = TEXT("value_object_not_found");
				return false;
			}
		}

		FString ValueBeforeKeyPath;
		JsonTryGetString(Ctx.Params, TEXT("value_before_key_path"), ValueBeforeKeyPath);
		if (ValueBeforeKeyPath.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_before_key_path"), ValueBeforeKeyPath);
		}
		ValueBeforeKeyPath = ValueBeforeKeyPath.TrimStartAndEnd();
		if (!ValueBeforeKeyPath.IsEmpty())
		{
			OutValueBeforeKeyObject = UeAgentSequenceOps::LoadAssetObject(ValueBeforeKeyPath);
			if (!OutValueBeforeKeyObject)
			{
				OutError = TEXT("value_before_key_object_not_found");
				return false;
			}
		}

		FString PropertyClassPath;
		JsonTryGetString(Ctx.Params, TEXT("property_class_path"), PropertyClassPath);
		PropertyClassPath = PropertyClassPath.TrimStartAndEnd();
		if (!PropertyClassPath.IsEmpty())
		{
			OutClass = LoadObject<UClass>(nullptr, *PropertyClassPath);
			if (!OutClass && !PropertyClassPath.EndsWith(TEXT("_C")))
			{
				OutClass = LoadObject<UClass>(nullptr, *(PropertyClassPath + TEXT("_C")));
			}
			if (!OutClass)
			{
				OutError = TEXT("property_class_not_found");
				return false;
			}
		}
		else if (OutValueObject)
		{
			OutClass = OutValueObject->GetClass();
		}

		if (!OutClass)
		{
			OutError = TEXT("missing_property_class_path");
			return false;
		}
		if (!OutValueObject)
		{
			OutError = TEXT("missing_value_path");
			return false;
		}

		return true;
	};

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneByteTrack* ByteTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneByteTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ByteTrack != nullptr;
		if (!ByteTrack)
		{
			ByteTrack = MovieScene->AddTrack<UMovieSceneByteTrack>(BindingGuid);
		}
		if (!ByteTrack)
		{
			OutError = TEXT("create_byte_property_track_failed");
			return false;
		}
		ByteTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UEnum* ByteEnum = ByteTrack->GetEnum();
		FString EnumPath;
		JsonTryGetString(Ctx.Params, TEXT("enum_path"), EnumPath);
		EnumPath = EnumPath.TrimStartAndEnd();
		if (!EnumPath.IsEmpty())
		{
			ByteEnum = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!ByteEnum)
			{
				OutError = TEXT("enum_not_found");
				return false;
			}
			ByteTrack->SetEnum(ByteEnum);
		}

		auto ParseByteValue = [&](const TCHAR* FieldName, const TCHAR* NameField, uint8& OutValue, bool& bOutProvided) -> bool
		{
			bOutProvided = false;

			double NumericValue = 0.0;
			if (JsonTryGetNumber(Ctx.Params, FieldName, NumericValue))
			{
				bOutProvided = true;
				const int32 Rounded = FMath::RoundToInt(NumericValue);
				if (Rounded < 0 || Rounded > 255)
				{
					OutError = TEXT("invalid_byte_value");
					return false;
				}
				OutValue = static_cast<uint8>(Rounded);
				return true;
			}

			FString NameValue;
			if (JsonTryGetString(Ctx.Params, NameField, NameValue) && !NameValue.TrimStartAndEnd().IsEmpty())
			{
				if (!ByteEnum)
				{
					OutError = TEXT("missing_enum_path");
					return false;
				}
				const int64 EnumValue = ByteEnum->GetValueByNameString(NameValue.TrimStartAndEnd());
				if (EnumValue == INDEX_NONE || EnumValue < 0 || EnumValue > 255)
				{
					OutError = TEXT("invalid_enum_value_name");
					return false;
				}
				bOutProvided = true;
				OutValue = static_cast<uint8>(EnumValue);
				return true;
			}

			return true;
		};

		uint8 ByteValue = 0;
		bool bHasByteValue = false;
		if (!ParseByteValue(TEXT("value"), TEXT("value_name"), ByteValue, bHasByteValue))
		{
			return false;
		}
		if (!bHasByteValue)
		{
			OutError = TEXT("missing_value");
			return false;
		}

		uint8 ByteValueBeforeKey = 0;
		bool bHasValueBeforeKey = false;
		if (!ParseByteValue(TEXT("value_before_key"), TEXT("value_before_key_name"), ByteValueBeforeKey, bHasValueBeforeKey))
		{
			return false;
		}

		UMovieSceneByteSection* ByteSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ByteTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ExistingSections[0]);
		}
		if (!ByteSection)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ByteTrack->CreateNewSection());
			if (!ByteSection)
			{
				OutError = TEXT("create_byte_property_section_failed");
				return false;
			}
			ByteTrack->AddSection(*ByteSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ByteTrack->Modify();
		ByteSection->Modify();
		ByteSection->SetRange(TRange<FFrameNumber>::All());
		ByteSection->ByteCurve.SetEnum(ByteTrack->GetEnum());

		if (bHasValueBeforeKey && ByteSection->ByteCurve.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			ByteSection->ByteCurve.GetData().AddKey(PlaybackStartFrame, ByteValueBeforeKey);
		}
		ByteSection->ByteCurve.GetData().UpdateOrAddKey(KeyFrame, ByteValue);

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("byte"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("enum_path"), ByteTrack->GetEnum() ? ByteTrack->GetEnum()->GetPathName() : TEXT(""));
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetNumberField(TEXT("value"), ByteValue);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::String)
	{
		FString ValueText;
		if (!JsonTryGetString(Ctx.Params, TEXT("value"), ValueText))
		{
			OutError = TEXT("missing_value");
			return false;
		}

		FString ValueBeforeKey;
		const bool bHasValueBeforeKey = JsonTryGetString(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneStringTrack* StringTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneStringTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = StringTrack != nullptr;
		if (!StringTrack)
		{
			StringTrack = MovieScene->AddTrack<UMovieSceneStringTrack>(BindingGuid);
		}
		if (!StringTrack)
		{
			OutError = TEXT("create_string_property_track_failed");
			return false;
		}
		StringTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneStringSection* StringSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = StringTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			StringSection = Cast<UMovieSceneStringSection>(ExistingSections[0]);
		}
		if (!StringSection)
		{
			StringSection = Cast<UMovieSceneStringSection>(StringTrack->CreateNewSection());
			if (!StringSection)
			{
				OutError = TEXT("create_string_property_section_failed");
				return false;
			}
			StringTrack->AddSection(*StringSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		StringTrack->Modify();
		StringSection->Modify();
		StringSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneStringChannel& Channel = const_cast<FMovieSceneStringChannel&>(StringSection->GetChannel());
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, ValueBeforeKey);
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, ValueText);

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("string"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value"), ValueText);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		struct FParsedSequenceVectorValue
		{
			double Components[4] = { 0.0, 0.0, 0.0, 0.0 };
			int32 ChannelsUsed = 0;
		};

		auto ParseVectorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FParsedSequenceVectorValue& OutValue) -> bool
		{
			const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
			if (!FieldValue || !(*FieldValue).IsValid())
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
			if ((*FieldValue)->TryGetArray(ArrayValue) && ArrayValue && ArrayValue->Num() >= 2)
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				double W = 0.0;
				if (!UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayValue)[0], X)
					|| !UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayValue)[1], Y))
				{
					return false;
				}
				const bool bHasZ = ArrayValue->Num() >= 3 && UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayValue)[2], Z);
				const bool bHasW = ArrayValue->Num() >= 4 && UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayValue)[3], W);
				OutValue.Components[0] = X;
				OutValue.Components[1] = Y;
				OutValue.Components[2] = bHasZ ? Z : 0.0;
				OutValue.Components[3] = bHasW ? W : 0.0;
				OutValue.ChannelsUsed = bHasW ? 4 : (bHasZ ? 3 : 2);
				return true;
			}

			const TSharedPtr<FJsonObject>* VectorObject = nullptr;
			if (!(*FieldValue)->TryGetObject(VectorObject) || !VectorObject || !VectorObject->IsValid())
			{
				return false;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double W = 0.0;
			const bool bHasX = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*VectorObject, { TEXT("x"), TEXT("X") }, X);
			const bool bHasY = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*VectorObject, { TEXT("y"), TEXT("Y") }, Y);
			const bool bHasZ = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*VectorObject, { TEXT("z"), TEXT("Z") }, Z);
			const bool bHasW = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*VectorObject, { TEXT("w"), TEXT("W") }, W);
			if (!bHasX || !bHasY)
			{
				return false;
			}

			OutValue.Components[0] = X;
			OutValue.Components[1] = Y;
			OutValue.Components[2] = bHasZ ? Z : 0.0;
			OutValue.Components[3] = bHasW ? W : 0.0;
			OutValue.ChannelsUsed = bHasW ? 4 : (bHasZ ? 3 : 2);
			return true;
		};

		auto ParseVectorTopLevel = [](const TSharedPtr<FJsonObject>& Obj, FParsedSequenceVectorValue& OutValue) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double W = 0.0;
			const bool bHasX = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X);
			const bool bHasY = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y);
			const bool bHasZ = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z);
			const bool bHasW = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("w"), TEXT("W") }, W);
			if (!bHasX || !bHasY)
			{
				return false;
			}

			OutValue.Components[0] = X;
			OutValue.Components[1] = Y;
			OutValue.Components[2] = bHasZ ? Z : 0.0;
			OutValue.Components[3] = bHasW ? W : 0.0;
			OutValue.ChannelsUsed = bHasW ? 4 : (bHasZ ? 3 : 2);
			return true;
		};

		FParsedSequenceVectorValue VectorValue;
		bool bHasVectorValue = ParseVectorObject(Ctx.Params, TEXT("value"), VectorValue);
		if (!bHasVectorValue)
		{
			bHasVectorValue = ParseVectorTopLevel(Ctx.Params, VectorValue);
		}
		if (!bHasVectorValue)
		{
			OutError = TEXT("missing_or_invalid_vector_value");
			return false;
		}

		FParsedSequenceVectorValue VectorValueBeforeKey;
		const bool bHasVectorValueBeforeKey = ParseVectorObject(Ctx.Params, TEXT("value_before_key"), VectorValueBeforeKey);

		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);

		auto ResolveVectorTrackSettings = [&](UeAgentSequenceOps::ESequenceVectorPrecision& OutPrecision, int32& OutChannels) -> bool
		{
			bool bHasExplicitPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision ExplicitPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			FString VectorPrecisionText;
			if (JsonTryGetString(Ctx.Params, TEXT("vector_precision"), VectorPrecisionText) && !VectorPrecisionText.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentSequenceOps::ParseSequenceVectorPrecision(VectorPrecisionText, ExplicitPrecision))
				{
					OutError = TEXT("invalid_vector_precision");
					return false;
				}
				bHasExplicitPrecision = true;
			}

			bool bHasExplicitChannels = false;
			int32 ExplicitChannels = 3;
			double ChannelsUsedNumber = 0.0;
			if (JsonTryGetNumber(Ctx.Params, TEXT("channels_used"), ChannelsUsedNumber))
			{
				ExplicitChannels = static_cast<int32>(ChannelsUsedNumber);
				if (ExplicitChannels < 2 || ExplicitChannels > 4)
				{
					OutError = TEXT("invalid_channels_used");
					return false;
				}
				bHasExplicitChannels = true;
			}

			bool bHasInferredPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision InferredPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			bool bHasInferredChannels = false;
			int32 InferredChannels = 3;
			UeAgentSequenceOps::InferSequenceVectorDefaultsFromTypeText(PropertyTypeText, bHasInferredPrecision, InferredPrecision, bHasInferredChannels, InferredChannels);

			UMovieSceneFloatVectorTrack* ExistingFloatTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			UMovieSceneDoubleVectorTrack* ExistingDoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			if (ExistingFloatTrack && ExistingDoubleTrack)
			{
				OutError = TEXT("multiple_vector_tracks_for_property");
				return false;
			}

			if (bHasExplicitPrecision)
			{
				OutPrecision = ExplicitPrecision;
			}
			else if (ExistingFloatTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Float;
			}
			else if (ExistingDoubleTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}
			else if (bHasInferredPrecision)
			{
				OutPrecision = InferredPrecision;
			}
			else
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}

			if (bHasExplicitChannels)
			{
				OutChannels = ExplicitChannels;
			}
			else if (ExistingFloatTrack)
			{
				OutChannels = FMath::Clamp(ExistingFloatTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (ExistingDoubleTrack)
			{
				OutChannels = FMath::Clamp(ExistingDoubleTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (bHasInferredChannels)
			{
				OutChannels = InferredChannels;
			}
			else
			{
				OutChannels = VectorValue.ChannelsUsed > 0 ? VectorValue.ChannelsUsed : 3;
			}

			if (OutChannels < 2 || OutChannels > 4)
			{
				OutError = TEXT("invalid_channels_used");
				return false;
			}
			if (ExistingFloatTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Float)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Double)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingFloatTrack && ExistingFloatTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && ExistingDoubleTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}
			if (VectorValue.ChannelsUsed != OutChannels)
			{
				OutError = TEXT("vector_value_channels_mismatch");
				return false;
			}
			if (bHasVectorValueBeforeKey && VectorValueBeforeKey.ChannelsUsed != OutChannels)
			{
				OutError = TEXT("vector_value_before_key_channels_mismatch");
				return false;
			}

			return true;
		};

		UeAgentSequenceOps::ESequenceVectorPrecision VectorPrecision;
		int32 ChannelsUsed = 3;
		if (!ResolveVectorTrackSettings(VectorPrecision, ChannelsUsed))
		{
			return false;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("x"), VectorValue.Components[0]);
		ValueObject->SetNumberField(TEXT("y"), VectorValue.Components[1]);
		if (ChannelsUsed >= 3)
		{
			ValueObject->SetNumberField(TEXT("z"), VectorValue.Components[2]);
		}
		if (ChannelsUsed >= 4)
		{
			ValueObject->SetNumberField(TEXT("w"), VectorValue.Components[3]);
		}

		if (VectorPrecision == UeAgentSequenceOps::ESequenceVectorPrecision::Float)
		{
			UMovieSceneFloatVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			const bool bTrackAlreadyExists = VectorTrack != nullptr;
			if (!VectorTrack)
			{
				VectorTrack = MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(BindingGuid);
			}
			if (!VectorTrack)
			{
				OutError = TEXT("create_float_vector_property_track_failed");
				return false;
			}
			VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
			VectorTrack->SetNumChannelsUsed(ChannelsUsed);

			UMovieSceneFloatVectorSection* VectorSection = nullptr;
			const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
			if (ExistingSections.Num() > 0)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(ExistingSections[0]);
			}
			if (!VectorSection)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(VectorTrack->CreateNewSection());
				if (!VectorSection)
				{
					OutError = TEXT("create_float_vector_property_section_failed");
					return false;
				}
				VectorTrack->AddSection(*VectorSection);
			}

			VectorTrack->Modify();
			VectorSection->Modify();
			VectorSection->SetChannelsUsed(ChannelsUsed);
			VectorSection->SetRange(TRange<FFrameNumber>::All());

			bool bSectionEmpty = true;
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				if (VectorSection->GetChannel(ChannelIndex).GetData().GetTimes().Num() > 0)
				{
					bSectionEmpty = false;
					break;
				}
			}
			if (bHasVectorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
			{
				for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
				{
					FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(VectorSection->GetChannel(ChannelIndex));
					Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(static_cast<float>(VectorValueBeforeKey.Components[ChannelIndex])));
				}
			}

			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(VectorSection->GetChannel(ChannelIndex));
				Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(VectorValue.Components[ChannelIndex])));
			}

			Sequence->MarkPackageDirty();

			bool bSaveAfterSet = false;
			JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
			if (bSaveAfterSet)
			{
				if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
				{
					return false;
				}
			}

			OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
			OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
			OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
			OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
			OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
			OutData->SetStringField(TEXT("property_path"), PropertyPathText);
			OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
			OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
			OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
			OutData->SetObjectField(TEXT("value"), ValueObject);
			OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
			return true;
		}

		UMovieSceneDoubleVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = VectorTrack != nullptr;
		if (!VectorTrack)
		{
			VectorTrack = MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(BindingGuid);
		}
		if (!VectorTrack)
		{
			OutError = TEXT("create_double_vector_property_track_failed");
			return false;
		}
		VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
		VectorTrack->SetNumChannelsUsed(ChannelsUsed);

		UMovieSceneDoubleVectorSection* VectorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(ExistingSections[0]);
		}
		if (!VectorSection)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(VectorTrack->CreateNewSection());
			if (!VectorSection)
			{
				OutError = TEXT("create_double_vector_property_section_failed");
				return false;
			}
			VectorTrack->AddSection(*VectorSection);
		}

		VectorTrack->Modify();
		VectorSection->Modify();
		VectorSection->SetChannelsUsed(ChannelsUsed);
		VectorSection->SetRange(TRange<FFrameNumber>::All());

		bool bSectionEmpty = true;
		for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
		{
			if (VectorSection->GetChannel(ChannelIndex).GetData().GetTimes().Num() > 0)
			{
				bSectionEmpty = false;
				break;
			}
		}
		if (bHasVectorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
		{
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(VectorSection->GetChannel(ChannelIndex));
				Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(VectorValueBeforeKey.Components[ChannelIndex]));
			}
		}

		for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
		{
			FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(VectorSection->GetChannel(ChannelIndex));
			Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(VectorValue.Components[ChannelIndex]));
		}

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
		OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
		OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetObjectField(TEXT("value"), ValueObject);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		struct FParsedSequenceRotatorValue
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
		};

		auto ParseRotatorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FParsedSequenceRotatorValue& OutValue) -> bool
		{
			const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
			if (!FieldValue || !(*FieldValue).IsValid())
			{
				return false;
			}

			FRotator ParsedRotator = FRotator::ZeroRotator;
			if (UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, ParsedRotator))
			{
				OutValue.Pitch = ParsedRotator.Pitch;
				OutValue.Yaw = ParsedRotator.Yaw;
				OutValue.Roll = ParsedRotator.Roll;
				return true;
			}

			const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
			if (!(*FieldValue)->TryGetObject(RotatorObject) || !RotatorObject || !RotatorObject->IsValid())
			{
				return false;
			}

			const bool bHasPitch = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*RotatorObject, { TEXT("pitch"), TEXT("Pitch") }, OutValue.Pitch);
			const bool bHasYaw = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*RotatorObject, { TEXT("yaw"), TEXT("Yaw") }, OutValue.Yaw);
			const bool bHasRoll = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*RotatorObject, { TEXT("roll"), TEXT("Roll") }, OutValue.Roll);
			return bHasPitch && bHasYaw && bHasRoll;
		};

		auto ParseRotatorTopLevel = [](const TSharedPtr<FJsonObject>& Obj, FParsedSequenceRotatorValue& OutValue) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}

			const bool bHasPitch = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("pitch"), TEXT("Pitch") }, OutValue.Pitch);
			const bool bHasYaw = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("yaw"), TEXT("Yaw") }, OutValue.Yaw);
			const bool bHasRoll = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("roll"), TEXT("Roll") }, OutValue.Roll);
			return bHasPitch && bHasYaw && bHasRoll;
		};

		FParsedSequenceRotatorValue RotatorValue;
		bool bHasRotatorValue = ParseRotatorObject(Ctx.Params, TEXT("value"), RotatorValue);
		if (!bHasRotatorValue)
		{
			bHasRotatorValue = ParseRotatorTopLevel(Ctx.Params, RotatorValue);
		}
		if (!bHasRotatorValue)
		{
			OutError = TEXT("missing_or_invalid_rotator_value");
			return false;
		}

		FParsedSequenceRotatorValue RotatorValueBeforeKey;
		const bool bHasRotatorValueBeforeKey = ParseRotatorObject(Ctx.Params, TEXT("value_before_key"), RotatorValueBeforeKey);

		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneRotatorTrack* RotatorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneRotatorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = RotatorTrack != nullptr;
		if (!RotatorTrack)
		{
			RotatorTrack = MovieScene->AddTrack<UMovieSceneRotatorTrack>(BindingGuid);
		}
		if (!RotatorTrack)
		{
			OutError = TEXT("create_rotator_property_track_failed");
			return false;
		}
		RotatorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneRotatorSection* RotatorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = RotatorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(ExistingSections[0]);
		}
		if (!RotatorSection)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(RotatorTrack->CreateNewSection());
			if (!RotatorSection)
			{
				OutError = TEXT("create_rotator_property_section_failed");
				return false;
			}
			RotatorTrack->AddSection(*RotatorSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		RotatorTrack->Modify();
		RotatorSection->Modify();
		RotatorSection->SetRange(TRange<FFrameNumber>::All());

		auto AddRotatorKey = [&](const int32 ChannelIndex, const double Value)
		{
			FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(ChannelIndex));
			Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(Value));
		};

		const bool bSectionEmpty =
			RotatorSection->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex).GetData().GetTimes().Num() == 0 &&
			RotatorSection->GetChannel(UMovieSceneRotatorSection::YawChannelIndex).GetData().GetTimes().Num() == 0 &&
			RotatorSection->GetChannel(UMovieSceneRotatorSection::RollChannelIndex).GetData().GetTimes().Num() == 0;
		if (bHasRotatorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
		{
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Pitch));
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::YawChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Yaw));
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::RollChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Roll));
		}

		AddRotatorKey(UMovieSceneRotatorSection::PitchChannelIndex, RotatorValue.Pitch);
		AddRotatorKey(UMovieSceneRotatorSection::YawChannelIndex, RotatorValue.Yaw);
		AddRotatorKey(UMovieSceneRotatorSection::RollChannelIndex, RotatorValue.Roll);

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("pitch"), RotatorValue.Pitch);
		ValueObject->SetNumberField(TEXT("yaw"), RotatorValue.Yaw);
		ValueObject->SetNumberField(TEXT("roll"), RotatorValue.Roll);

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("rotator"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetObjectField(TEXT("value"), ValueObject);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		auto ResolveActorReferenceKey = [&](const TCHAR* BindingGuidField, const TCHAR* ActorIdField, const TCHAR* ComponentField, const TCHAR* SocketField, FMovieSceneActorReferenceKey& OutKey, bool& bOutProvided, FString& OutResolvedActorId) -> bool
		{
			OutKey = FMovieSceneActorReferenceKey();
			bOutProvided = false;
			OutResolvedActorId.Reset();

			FString TargetBindingGuidText;
			if (JsonTryGetString(Ctx.Params, BindingGuidField, TargetBindingGuidText) && !TargetBindingGuidText.TrimStartAndEnd().IsEmpty())
			{
				FGuid TargetBindingGuid;
				if (!FGuid::Parse(TargetBindingGuidText, TargetBindingGuid))
				{
					OutError = TEXT("invalid_value_binding_guid");
					return false;
				}
				if (!MovieScene->FindBinding(TargetBindingGuid))
				{
					OutError = TEXT("value_binding_not_found");
					return false;
				}
				OutKey.Object = UE::MovieScene::FRelativeObjectBindingID(TargetBindingGuid);
				bOutProvided = true;
			}
			else
			{
				FString ActorId;
				if (JsonTryGetString(Ctx.Params, ActorIdField, ActorId) && !ActorId.TrimStartAndEnd().IsEmpty())
				{
					FString WorldError;
					UWorld* World = GetEditorWorld(WorldError);
					if (!World)
					{
						OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
						return false;
					}

					AActor* TargetActor = FindActorByNameOrLabel(World, ActorId.TrimStartAndEnd());
					if (!TargetActor)
					{
						OutError = TEXT("value_actor_not_found");
						return false;
					}

					FGuid TargetBindingGuid;
					PRAGMA_DISABLE_DEPRECATION_WARNINGS
					TargetBindingGuid = Sequence->FindBindingFromObject(TargetActor, World);
					PRAGMA_ENABLE_DEPRECATION_WARNINGS
					if (!TargetBindingGuid.IsValid())
					{
						const FString BindingName = TargetActor->GetActorLabel().IsEmpty() ? TargetActor->GetName() : TargetActor->GetActorLabel();
						TargetBindingGuid = MovieScene->AddPossessable(BindingName, TargetActor->GetClass());
						if (!TargetBindingGuid.IsValid())
						{
							OutError = TEXT("add_possessable_failed");
							return false;
						}
						Sequence->BindPossessableObject(TargetBindingGuid, *TargetActor, World);
					}

					OutKey.Object = UE::MovieScene::FRelativeObjectBindingID(TargetBindingGuid);
					OutResolvedActorId = TargetActor->GetActorLabel().IsEmpty() ? TargetActor->GetName() : TargetActor->GetActorLabel();
					bOutProvided = true;
				}
			}

			if (bOutProvided)
			{
				FString ComponentName;
				if (JsonTryGetString(Ctx.Params, ComponentField, ComponentName) && !ComponentName.TrimStartAndEnd().IsEmpty())
				{
					OutKey.ComponentName = FName(*ComponentName.TrimStartAndEnd());
				}
				FString SocketName;
				if (JsonTryGetString(Ctx.Params, SocketField, SocketName) && !SocketName.TrimStartAndEnd().IsEmpty())
				{
					OutKey.SocketName = FName(*SocketName.TrimStartAndEnd());
				}
			}

			return true;
		};

		const FName PropertyName(*PropertyNameText);
		UMovieSceneActorReferenceTrack* ActorReferenceTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneActorReferenceTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ActorReferenceTrack != nullptr;
		if (!ActorReferenceTrack)
		{
			ActorReferenceTrack = MovieScene->AddTrack<UMovieSceneActorReferenceTrack>(BindingGuid);
		}
		if (!ActorReferenceTrack)
		{
			OutError = TEXT("create_actor_reference_property_track_failed");
			return false;
		}
		ActorReferenceTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		FMovieSceneActorReferenceKey ValueKey;
		bool bHasValueKey = false;
		FString ResolvedActorId;
		if (!ResolveActorReferenceKey(TEXT("value_binding_guid"), TEXT("value_actor_id"), TEXT("component_name"), TEXT("socket_name"), ValueKey, bHasValueKey, ResolvedActorId))
		{
			return false;
		}
		if (!bHasValueKey)
		{
			OutError = TEXT("missing_value_binding_guid_or_value_actor_id");
			return false;
		}

		FMovieSceneActorReferenceKey ValueBeforeKey;
		bool bHasValueBeforeKey = false;
		FString ResolvedBeforeActorId;
		if (!ResolveActorReferenceKey(TEXT("value_before_key_binding_guid"), TEXT("value_before_key_actor_id"), TEXT("value_before_key_component_name"), TEXT("value_before_key_socket_name"), ValueBeforeKey, bHasValueBeforeKey, ResolvedBeforeActorId))
		{
			return false;
		}

		UMovieSceneActorReferenceSection* ActorReferenceSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ActorReferenceTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ExistingSections[0]);
		}
		if (!ActorReferenceSection)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ActorReferenceTrack->CreateNewSection());
			if (!ActorReferenceSection)
			{
				OutError = TEXT("create_actor_reference_property_section_failed");
				return false;
			}
			ActorReferenceTrack->AddSection(*ActorReferenceSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ActorReferenceTrack->Modify();
		ActorReferenceSection->Modify();
		ActorReferenceSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneActorReferenceData& Channel = const_cast<FMovieSceneActorReferenceData&>(ActorReferenceSection->GetActorReferenceData());
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, ValueBeforeKey);
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, ValueKey);

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("actor_reference"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value_binding_guid"), ValueKey.Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("value_actor_id"), ResolvedActorId);
		OutData->SetStringField(TEXT("component_name"), ValueKey.ComponentName.ToString());
		OutData->SetStringField(TEXT("socket_name"), ValueKey.SocketName.ToString());
		if (bHasValueBeforeKey)
		{
			OutData->SetStringField(TEXT("value_before_key_binding_guid"), ValueBeforeKey.Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetStringField(TEXT("value_before_key_actor_id"), ResolvedBeforeActorId);
			OutData->SetStringField(TEXT("value_before_key_component_name"), ValueBeforeKey.ComponentName.ToString());
			OutData->SetStringField(TEXT("value_before_key_socket_name"), ValueBeforeKey.SocketName.ToString());
		}
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Object)
	{
		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UClass* PropertyClass = nullptr;
		UObject* ValueObject = nullptr;
		UObject* ValueBeforeKeyObject = nullptr;
		if (!ResolveObjectPropertyClassAndValue(PropertyClass, ValueObject, ValueBeforeKeyObject))
		{
			return false;
		}

		UMovieSceneObjectPropertyTrack* ObjectTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneObjectPropertyTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ObjectTrack != nullptr;
		if (!ObjectTrack)
		{
			ObjectTrack = MovieScene->AddTrack<UMovieSceneObjectPropertyTrack>(BindingGuid);
		}
		if (!ObjectTrack)
		{
			OutError = TEXT("create_object_property_track_failed");
			return false;
		}
		ObjectTrack->PropertyClass = PropertyClass;
		ObjectTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneObjectPropertySection* ObjectSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ObjectTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ExistingSections[0]);
		}
		if (!ObjectSection)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ObjectTrack->CreateNewSection());
			if (!ObjectSection)
			{
				OutError = TEXT("create_object_property_section_failed");
				return false;
			}
			ObjectTrack->AddSection(*ObjectSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ObjectTrack->Modify();
		ObjectSection->Modify();
		ObjectSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneObjectPathChannel& ObjectChannel = ObjectSection->ObjectChannel;
		TMovieSceneChannelData<FMovieSceneObjectPathChannelKeyValue> ChannelData = ObjectChannel.GetData();
		if (ValueBeforeKeyObject && ChannelData.GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			ChannelData.AddKey(PlaybackStartFrame, FMovieSceneObjectPathChannelKeyValue(ValueBeforeKeyObject));
		}
		ChannelData.UpdateOrAddKey(KeyFrame, FMovieSceneObjectPathChannelKeyValue(ValueObject));

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("object"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("property_class_path"), PropertyClass->GetPathName());
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value_path"), ValueObject->GetPathName());
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Double)
	{
		double ValueNumber = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), ValueNumber))
		{
			OutError = TEXT("missing_value");
			return false;
		}

		double ValueBeforeKey = 0.0;
		const bool bHasValueBeforeKey = JsonTryGetNumber(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

		ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneDoubleTrack* DoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = DoubleTrack != nullptr;
		if (!DoubleTrack)
		{
			DoubleTrack = MovieScene->AddTrack<UMovieSceneDoubleTrack>(BindingGuid);
		}
		if (!DoubleTrack)
		{
			OutError = TEXT("create_double_property_track_failed");
			return false;
		}
		DoubleTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneDoubleSection* DoubleSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = DoubleTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(ExistingSections[0]);
		}
		if (!DoubleSection)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(DoubleTrack->CreateNewSection());
			if (!DoubleSection)
			{
				OutError = TEXT("create_double_property_section_failed");
				return false;
			}
			DoubleTrack->AddSection(*DoubleSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		DoubleTrack->Modify();
		DoubleSection->Modify();
		DoubleSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneDoubleChannel& Channel = DoubleSection->GetChannel();
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(ValueBeforeKey));
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(ValueNumber));

		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("double"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetNumberField(TEXT("value"), ValueNumber);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	auto ParseColorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FLinearColor& OutColor) -> bool
	{
		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
		{
			return false;
		}

		double Red = 0.0;
		double Green = 0.0;
		double Blue = 0.0;
		double Alpha = 1.0;
		const bool bHasRed = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, Red);
		const bool bHasGreen = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, Green);
		const bool bHasBlue = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, Blue);
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, Alpha);
		if (!bHasRed || !bHasGreen || !bHasBlue)
		{
			return false;
		}
		OutColor = FLinearColor(static_cast<float>(Red), static_cast<float>(Green), static_cast<float>(Blue), static_cast<float>(Alpha));
		return true;
	};

	FLinearColor ColorValue = FLinearColor::White;
	bool bHasColorValue = ParseColorObject(Ctx.Params, TEXT("value"), ColorValue);
	if (!bHasColorValue)
	{
		double Red = 0.0;
		double Green = 0.0;
		double Blue = 0.0;
		double Alpha = 1.0;
		const bool bHasRed = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Ctx.Params, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, Red);
		const bool bHasGreen = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Ctx.Params, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, Green);
		const bool bHasBlue = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Ctx.Params, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, Blue);
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Ctx.Params, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, Alpha);
		if (!bHasRed || !bHasGreen || !bHasBlue)
		{
			OutError = TEXT("missing_or_invalid_color_value");
			return false;
		}
		ColorValue = FLinearColor(static_cast<float>(Red), static_cast<float>(Green), static_cast<float>(Blue), static_cast<float>(Alpha));
	}

	FLinearColor ColorValueBeforeKey = FLinearColor::Black;
	const bool bHasColorValueBeforeKey = ParseColorObject(Ctx.Params, TEXT("value_before_key"), ColorValueBeforeKey);

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);

	UMovieSceneColorTrack* ColorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneColorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = ColorTrack != nullptr;
	if (!ColorTrack)
	{
		ColorTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
	}
	if (!ColorTrack)
	{
		OutError = TEXT("create_color_property_track_failed");
		return false;
	}
	ColorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneColorSection* ColorSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = ColorTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ExistingSections[0]);
	}
	if (!ColorSection)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ColorTrack->CreateNewSection());
		if (!ColorSection)
		{
			OutError = TEXT("create_color_property_section_failed");
			return false;
		}
		ColorTrack->AddSection(*ColorSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	ColorTrack->Modify();
	ColorSection->Modify();
	ColorSection->SetRange(TRange<FFrameNumber>::All());

	auto AddColorKey = [KeyFrame](FMovieSceneFloatChannel& Channel, const float Value)
	{
		Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(Value));
	};

	const bool bChannelEmpty = ColorSection->GetRedChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetGreenChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetBlueChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetAlphaChannel().GetData().GetTimes().Num() == 0;
	if (bHasColorValueBeforeKey && bChannelEmpty && KeyFrame > PlaybackStartFrame)
	{
		ColorSection->GetRedChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.R));
		ColorSection->GetGreenChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.G));
		ColorSection->GetBlueChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.B));
		ColorSection->GetAlphaChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.A));
	}

	AddColorKey(ColorSection->GetRedChannel(), ColorValue.R);
	AddColorKey(ColorSection->GetGreenChannel(), ColorValue.G);
	AddColorKey(ColorSection->GetBlueChannel(), ColorValue.B);
	AddColorKey(ColorSection->GetAlphaChannel(), ColorValue.A);

	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
	ColorObj->SetNumberField(TEXT("r"), ColorValue.R);
	ColorObj->SetNumberField(TEXT("g"), ColorValue.G);
	ColorObj->SetNumberField(TEXT("b"), ColorValue.B);
	ColorObj->SetNumberField(TEXT("a"), ColorValue.A);

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_type"), TEXT("color"));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetObjectField(TEXT("value"), ColorObj);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
