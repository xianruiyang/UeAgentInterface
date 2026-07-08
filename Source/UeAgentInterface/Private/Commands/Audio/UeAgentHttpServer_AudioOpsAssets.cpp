// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundSubmixSend.h"

namespace UeAgentAudioOps
{
	TSharedPtr<FJsonObject> BuildSoundAttenuationInfo(USoundAttenuation* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		const FSoundAttenuationSettings& A = Asset->Attenuation;
		Info->SetBoolField(TEXT("attenuate"), A.bAttenuate != 0);
		Info->SetBoolField(TEXT("spatialize"), A.bSpatialize != 0);
		Info->SetNumberField(TEXT("falloff_distance"), A.FalloffDistance);
		Info->SetBoolField(TEXT("occlusion"), A.bEnableOcclusion != 0);
		Info->SetNumberField(TEXT("occlusion_low_pass_filter_frequency"), A.OcclusionLowPassFilterFrequency);
		Info->SetNumberField(TEXT("occlusion_volume_attenuation"), A.OcclusionVolumeAttenuation);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundAttenuationProfile, true));
		}
		return Info;
	}
	TSharedPtr<FJsonObject> BuildSoundConcurrencyInfo(USoundConcurrency* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("max_count"), Asset->Concurrency.MaxCount);
		Info->SetBoolField(TEXT("limit_to_owner"), Asset->Concurrency.bLimitToOwner != 0);
		FString ValueText;
		FString CppType;
		if (ExportPropertyText(Asset, TEXT("Concurrency.ResolutionRule"), ValueText, CppType))
		{
			Info->SetStringField(TEXT("resolution_rule"), ValueText);
		}
		Info->SetNumberField(TEXT("retrigger_time"), Asset->Concurrency.RetriggerTime);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundConcurrencyProfile, true));
		}
		return Info;
	}
	TSharedPtr<FJsonObject> BuildSoundClassInfo(USoundClass* Asset, bool bIncludeChildren, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("volume"), Asset->Properties.Volume);
		Info->SetNumberField(TEXT("pitch"), Asset->Properties.Pitch);
		Info->SetBoolField(TEXT("is_ui_sound"), Asset->Properties.bIsUISound != 0);
		Info->SetBoolField(TEXT("is_music"), Asset->Properties.bIsMusic != 0);
		Info->SetStringField(TEXT("parent_class"), ObjectPathOrEmpty(Asset->ParentClass));
		Info->SetStringField(TEXT("default_submix"), ObjectPathOrEmpty(Asset->Properties.DefaultSubmix));
		if (bIncludeChildren)
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (USoundClass* Child : Asset->ChildClasses)
			{
				Children.Add(JsonStringValue(ObjectPathOrEmpty(Child)));
			}
			Info->SetArrayField(TEXT("children"), Children);
		}
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundClassProfile, true));
		}
		return Info;
	}
	TSharedPtr<FJsonObject> BuildSoundMixInfo(USoundMix* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("initial_delay"), Asset->InitialDelay);
		Info->SetNumberField(TEXT("fade_in_time"), Asset->FadeInTime);
		Info->SetNumberField(TEXT("duration"), Asset->Duration);
		Info->SetNumberField(TEXT("fade_out_time"), Asset->FadeOutTime);
		TArray<TSharedPtr<FJsonValue>> Adjusters;
		for (const FSoundClassAdjuster& Adjuster : Asset->SoundClassEffects)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("sound_class"), ObjectPathOrEmpty(Adjuster.SoundClassObject));
			Obj->SetNumberField(TEXT("volume_adjuster"), Adjuster.VolumeAdjuster);
			Obj->SetNumberField(TEXT("pitch_adjuster"), Adjuster.PitchAdjuster);
			Obj->SetNumberField(TEXT("low_pass_filter_frequency"), Adjuster.LowPassFilterFrequency);
			Obj->SetBoolField(TEXT("apply_to_children"), Adjuster.bApplyToChildren != 0);
			Obj->SetNumberField(TEXT("voice_center_channel_volume_adjuster"), Adjuster.VoiceCenterChannelVolumeAdjuster);
			Adjusters.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Info->SetArrayField(TEXT("class_adjusters"), Adjusters);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundMixProfile, true));
		}
		return Info;
	}
	TSharedPtr<FJsonObject> BuildSoundSubmixInfo(USoundSubmixBase* Asset, bool bIncludeChildren, bool bIncludeEffectChain, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		if (USoundSubmixWithParentBase* WithParent = Cast<USoundSubmixWithParentBase>(Asset))
		{
			Info->SetStringField(TEXT("parent"), ObjectPathOrEmpty(WithParent->ParentSubmix));
		}
		if (bIncludeChildren)
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (USoundSubmixBase* Child : Asset->ChildSubmixes)
			{
				Children.Add(JsonStringValue(ObjectPathOrEmpty(Child)));
			}
			Info->SetArrayField(TEXT("children"), Children);
		}
		if (bIncludeEffectChain)
		{
			TArray<TSharedPtr<FJsonValue>> Chain;
			if (USoundSubmix* Submix = Cast<USoundSubmix>(Asset))
			{
				for (USoundEffectSubmixPreset* Preset : Submix->SubmixEffectChain)
				{
					Chain.Add(JsonStringValue(ObjectPathOrEmpty(Preset)));
				}
			}
			Info->SetArrayField(TEXT("effect_chain"), Chain);
		}
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundSubmixProfile, true));
		}
		return Info;
	}
}
