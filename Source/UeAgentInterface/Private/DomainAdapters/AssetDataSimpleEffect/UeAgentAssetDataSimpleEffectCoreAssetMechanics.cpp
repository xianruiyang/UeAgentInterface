#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCoreAssetMechanics.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "FileMediaSource.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ExecutionMutation;
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		enum class ECoreAssetKind : uint8
		{
			AudioEffectPreset,
			SoundAttenuation,
			SoundClass,
			SoundConcurrency,
			SoundMix,
			SoundSubmix,
			Curve,
			DataObject,
			MediaSource,
			RuntimeVirtualTexture,
			SparseVolumeTexture,
		};

		const ECoreAssetKind* FindCoreAssetKind(
			const FString& NativeMechanicId)
		{
			static const TMap<FString, ECoreAssetKind> Kinds{
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_audio_effect_preset:v1"),
					ECoreAssetKind::AudioEffectPreset,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_attenuation:v1"),
					ECoreAssetKind::SoundAttenuation,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_class:v1"),
					ECoreAssetKind::SoundClass,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_concurrency:v1"),
					ECoreAssetKind::SoundConcurrency,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_mix:v1"),
					ECoreAssetKind::SoundMix,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_submix:v1"),
					ECoreAssetKind::SoundSubmix,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"replace_curve:v1"),
					ECoreAssetKind::Curve,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_data_object:v1"),
					ECoreAssetKind::DataObject,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_media_source:v1"),
					ECoreAssetKind::MediaSource,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_runtime_virtual_texture:v1"),
					ECoreAssetKind::RuntimeVirtualTexture,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sparse_volume_texture:v1"),
					ECoreAssetKind::SparseVolumeTexture,
				},
			};
			return Kinds.Find(NativeMechanicId);
		}

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValueRef* FindObjectFieldRef(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}

		const FCoreAssetChangeView* FindChange(
			const TArray<FCoreAssetChangeView>& Changes,
			const FString& FieldId)
		{
			for (const FCoreAssetChangeView& Change : Changes)
			{
				if (Change.FieldId == FieldId)
				{
					return &Change;
				}
			}
			return nullptr;
		}

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Names)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object
				|| Object.GetObjectItems().Num()
					!= static_cast<int32>(Names.size()))
			{
				return false;
			}
			for (const TCHAR* Name : Names)
			{
				if (!FindObjectField(Object, Name))
				{
					return false;
				}
			}
			return true;
		}

		bool TryReadNumber(
			const FUeAgentCanonicalValue& Value,
			double& OutNumber)
		{
			if (Value.GetKind() != EUeAgentValueKind::Integer
				&& Value.GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			return LexTryParseString(
					OutNumber,
					*Value.GetLexeme())
				&& FMath::IsFinite(OutNumber);
		}

		bool TryReadFloat(
			const FUeAgentCanonicalValue& Value,
			float& OutValue,
			const double MinValue,
			const double MaxValue)
		{
			double Number = 0.0;
			if (!TryReadNumber(Value, Number)
				|| Number < MinValue
				|| Number > MaxValue)
			{
				return false;
			}
			OutValue = static_cast<float>(Number);
			return FMath::IsFinite(OutValue);
		}

		bool TryReadInt32(
			const FUeAgentCanonicalValue& Value,
			int32& OutValue,
			const int32 MinValue,
			const int32 MaxValue)
		{
			double Number = 0.0;
			if (!TryReadNumber(Value, Number)
				|| FMath::FloorToDouble(Number) != Number
				|| Number < MinValue
				|| Number > MaxValue)
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		bool TryReadBool(
			const FUeAgentCanonicalValue& Value,
			bool& bOutValue)
		{
			if (Value.GetKind() != EUeAgentValueKind::Boolean)
			{
				return false;
			}
			bOutValue = Value.GetBoolean();
			return true;
		}

		bool TryReadString(
			const FUeAgentCanonicalValue& Value,
			FString& OutValue)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value.GetText();
			return true;
		}

		template <typename TObjectType>
		bool TryResolveObject(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TObjectType*& OutObject,
			FExecutionMutationError& OutError)
		{
			OutObject = nullptr;
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			UObject* Resolved = nullptr;
			if (!Resolver.TryResolveExisting(
					Value.GetText(),
					Resolved,
					OutError))
			{
				return false;
			}
			OutObject = Cast<TObjectType>(Resolved);
			return OutObject != nullptr;
		}

		template <typename TObjectType>
		bool TryResolveUniqueArray(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<TObjectType*>& OutObjects,
			FExecutionMutationError& OutError)
		{
			OutObjects.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				TObjectType* Object = nullptr;
				if (!TryResolveObject(
						Resolver,
						Item.Get(),
						Object,
						OutError)
					|| OutObjects.Contains(Object))
				{
					return false;
				}
				OutObjects.Add(Object);
			}
			return true;
		}

		void PinUnique(
			TArray<TStrongObjectPtr<UObject>>& Pins,
			UObject* Object)
		{
			if (!Object)
			{
				return;
			}
			for (const TStrongObjectPtr<UObject>& Pin : Pins)
			{
				if (Pin.Get() == Object)
				{
					return;
				}
			}
			Pins.Emplace(Object);
		}

		struct FPackageDirtySnapshot final
		{
			TWeakObjectPtr<UPackage> Package;
			bool bWasDirty = false;
		};

		void CapturePackage(
			TArray<FPackageDirtySnapshot>& Snapshots,
			UObject* Object)
		{
			UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (!Package)
			{
				return;
			}
			for (const FPackageDirtySnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Package.Get() == Package)
				{
					return;
				}
			}
			FPackageDirtySnapshot Snapshot;
			Snapshot.Package = Package;
			Snapshot.bWasDirty = Package->IsDirty();
			Snapshots.Add(Snapshot);
		}

		void RestorePackageFlags(
			const TArray<FPackageDirtySnapshot>& Snapshots)
		{
			for (const FPackageDirtySnapshot& Snapshot : Snapshots)
			{
				if (UPackage* Package = Snapshot.Package.Get())
				{
					Package->SetDirtyFlag(Snapshot.bWasDirty);
				}
			}
		}

		bool TryParseEqSettings(
			const FUeAgentCanonicalValue& Value,
			FSubmixEffectSubmixEQSettings& OutSettings)
		{
			if (!HasExactObjectFields(Value, {TEXT("eq_bands")}))
			{
				return false;
			}
			const FUeAgentCanonicalValue* Bands =
				FindObjectField(Value, TEXT("eq_bands"));
			if (!Bands
				|| Bands->GetKind() != EUeAgentValueKind::Array
				|| Bands->GetArrayItems().Num() > 64)
			{
				return false;
			}
			OutSettings.EQBands.Reset();
			for (const FUeAgentCanonicalValueRef& Item
				: Bands->GetArrayItems())
			{
				const FUeAgentCanonicalValue& BandValue = Item.Get();
				if (!HasExactObjectFields(
						BandValue,
						{
							TEXT("frequency_hz"),
							TEXT("bandwidth_octaves"),
							TEXT("gain_db"),
							TEXT("enabled"),
						}))
				{
					return false;
				}
				FSubmixEffectEQBand Band;
				bool bEnabled = false;
				if (!TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("frequency_hz")),
						Band.Frequency,
						20.0,
						20000.0)
					|| !TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("bandwidth_octaves")),
						Band.Bandwidth,
						0.1,
						2.0)
					|| !TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("gain_db")),
						Band.GainDb,
						-90.0,
						20.0)
					|| !TryReadBool(
						*FindObjectField(
							BandValue,
							TEXT("enabled")),
						bEnabled))
				{
					return false;
				}
				Band.bEnabled = bEnabled;
				OutSettings.EQBands.Add(Band);
			}
			return true;
		}

		bool TryParseDistanceAlgorithm(
			const FString& Name,
			EAttenuationDistanceModel& OutValue)
		{
			const FString Normalized =
				Name.TrimStartAndEnd().ToLower();
			if (Normalized == TEXT("linear"))
			{
				OutValue = EAttenuationDistanceModel::Linear;
			}
			else if (Normalized == TEXT("logarithmic"))
			{
				OutValue = EAttenuationDistanceModel::Logarithmic;
			}
			else if (Normalized == TEXT("inverse"))
			{
				OutValue = EAttenuationDistanceModel::Inverse;
			}
			else if (Normalized == TEXT("log_reverse"))
			{
				OutValue = EAttenuationDistanceModel::LogReverse;
			}
			else if (Normalized == TEXT("natural_sound"))
			{
				OutValue = EAttenuationDistanceModel::NaturalSound;
			}
			else if (Normalized == TEXT("custom"))
			{
				OutValue = EAttenuationDistanceModel::Custom;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryParseAttenuationSettings(
			const FUeAgentCanonicalValue& Value,
			FSoundAttenuationSettings& OutSettings)
		{
			if (!HasExactObjectFields(
					Value,
					{
						TEXT("attenuate"),
						TEXT("spatialize"),
						TEXT("distance_algorithm"),
						TEXT("attenuation_shape"),
						TEXT("inner_radius"),
						TEXT("falloff_distance"),
						TEXT("spatialization_algorithm"),
						TEXT("enable_occlusion"),
						TEXT("occlusion_low_pass_filter_frequency"),
						TEXT("occlusion_volume_attenuation"),
						TEXT("occlusion_interpolation_time"),
					}))
			{
				return false;
			}
			bool bAttenuate = false;
			bool bSpatialize = false;
			bool bEnableOcclusion = false;
			FString DistanceName;
			FString ShapeName;
			FString SpatializationName;
			float InnerRadius = 0.0f;
			if (!TryReadBool(
					*FindObjectField(Value, TEXT("attenuate")),
					bAttenuate)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("spatialize")),
					bSpatialize)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("distance_algorithm")),
					DistanceName)
				|| !TryParseDistanceAlgorithm(
					DistanceName,
					OutSettings.DistanceAlgorithm)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("attenuation_shape")),
					ShapeName)
				|| ShapeName.TrimStartAndEnd().ToLower()
					!= TEXT("sphere")
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("inner_radius")),
					InnerRadius,
					0.0,
					1000000000.0)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("falloff_distance")),
					OutSettings.FalloffDistance,
					0.0,
					1000000000.0)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("spatialization_algorithm")),
					SpatializationName)
				|| !TryReadBool(
					*FindObjectField(
						Value,
						TEXT("enable_occlusion")),
					bEnableOcclusion)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT(
							"occlusion_low_pass_filter_frequency")),
					OutSettings.OcclusionLowPassFilterFrequency,
					0.0,
					20000.0)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("occlusion_volume_attenuation")),
					OutSettings.OcclusionVolumeAttenuation,
					0.0,
					1.0)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("occlusion_interpolation_time")),
					OutSettings.OcclusionInterpolationTime,
					0.0,
					1000000.0))
			{
				return false;
			}
			const FString Spatialization =
				SpatializationName.TrimStartAndEnd().ToLower();
			if (Spatialization == TEXT("panning"))
			{
				OutSettings.SpatializationAlgorithm =
					SPATIALIZATION_Default;
			}
			else if (Spatialization == TEXT("binaural"))
			{
				OutSettings.SpatializationAlgorithm =
					SPATIALIZATION_HRTF;
			}
			else
			{
				return false;
			}
			OutSettings.bAttenuate = bAttenuate;
			OutSettings.bSpatialize = bSpatialize;
			OutSettings.bEnableOcclusion = bEnableOcclusion;
			OutSettings.AttenuationShape = EAttenuationShape::Sphere;
			OutSettings.AttenuationShapeExtents.X = InnerRadius;
			return true;
		}

		bool TryParseSoundClassProperties(
			const FUeAgentCanonicalValue& Value,
			FSoundClassProperties& OutProperties)
		{
			bool bIsUiSound = false;
			bool bIsMusic = false;
			if (!HasExactObjectFields(
					Value,
					{
						TEXT("volume"),
						TEXT("pitch"),
						TEXT("is_ui_sound"),
						TEXT("is_music"),
					})
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("volume")),
					OutProperties.Volume,
					0.0,
					1000.0)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("pitch")),
					OutProperties.Pitch,
					0.0,
					8.0)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("is_ui_sound")),
					bIsUiSound)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("is_music")),
					bIsMusic))
			{
				return false;
			}
			OutProperties.bIsUISound = bIsUiSound;
			OutProperties.bIsMusic = bIsMusic;
			return true;
		}

		struct FSoundClassNodeSnapshot final
		{
			TWeakObjectPtr<USoundClass> Node;
			TWeakObjectPtr<USoundClass> Parent;
			TArray<TWeakObjectPtr<USoundClass>> Children;
		};

		void CaptureSoundClassNode(
			TArray<FSoundClassNodeSnapshot>& Snapshots,
			TArray<FPackageDirtySnapshot>& Packages,
			USoundClass* Node)
		{
			if (!Node)
			{
				return;
			}
			for (const FSoundClassNodeSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Node.Get() == Node)
				{
					return;
				}
			}
			FSoundClassNodeSnapshot Snapshot;
			Snapshot.Node = Node;
			Snapshot.Parent = Node->ParentClass;
			for (USoundClass* Child : Node->ChildClasses)
			{
				Snapshot.Children.Add(Child);
			}
			Snapshots.Add(MoveTemp(Snapshot));
			CapturePackage(Packages, Node);
		}

		bool TryParseConcurrencySettings(
			const FUeAgentCanonicalValue& Value,
			FSoundConcurrencySettings& OutSettings)
		{
			if (!HasExactObjectFields(
					Value,
					{
						TEXT("max_count"),
						TEXT("limit_to_owner"),
						TEXT("retrigger_time"),
						TEXT("resolution_rule"),
						TEXT("volume_scale"),
						TEXT("volume_scale_mode"),
						TEXT("volume_scale_attack_time"),
						TEXT("volume_scale_release_time"),
						TEXT("voice_steal_release_time"),
					}))
			{
				return false;
			}
			bool bLimitToOwner = false;
			FString ResolutionRule;
			FString VolumeScaleMode;
			float VolumeScale = 1.0f;
			if (!TryReadInt32(
					*FindObjectField(Value, TEXT("max_count")),
					OutSettings.MaxCount,
					1,
					1024)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("limit_to_owner")),
					bLimitToOwner)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("retrigger_time")),
					OutSettings.RetriggerTime,
					0.0,
					1000000.0)
				|| !TryReadString(
					*FindObjectField(Value, TEXT("resolution_rule")),
					ResolutionRule)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("volume_scale")),
					VolumeScale,
					0.0,
					1.0)
				|| !TryReadString(
					*FindObjectField(Value, TEXT("volume_scale_mode")),
					VolumeScaleMode)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("volume_scale_attack_time")),
					OutSettings.VolumeScaleAttackTime,
					0.0,
					1000000.0)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("volume_scale_release_time")),
					OutSettings.VolumeScaleReleaseTime,
					0.0,
					1000000.0)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("voice_steal_release_time")),
					OutSettings.VoiceStealReleaseTime,
					0.0,
					1000000.0))
			{
				return false;
			}
			const FString Rule =
				ResolutionRule.TrimStartAndEnd().ToLower();
			if (Rule == TEXT("prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::PreventNew;
			}
			else if (Rule == TEXT("stop_oldest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopOldest;
			}
			else if (Rule == TEXT("stop_farthest_then_oldest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopFarthestThenOldest;
			}
			else if (Rule == TEXT("stop_farthest_then_prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopFarthestThenPreventNew;
			}
			else if (Rule == TEXT("stop_lowest_priority"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopLowestPriority;
			}
			else if (Rule == TEXT("stop_quietest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopQuietest;
			}
			else if (Rule == TEXT("stop_lowest_priority_then_prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::
						StopLowestPriorityThenPreventNew;
			}
			else
			{
				return false;
			}
			const FString ScaleMode =
				VolumeScaleMode.TrimStartAndEnd().ToLower();
			if (ScaleMode == TEXT("default"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Default;
			}
			else if (ScaleMode == TEXT("distance"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Distance;
			}
			else if (ScaleMode == TEXT("priority"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Priority;
			}
			else
			{
				return false;
			}
			FFloatProperty* VolumeScaleProperty =
				FindFProperty<FFloatProperty>(
					FSoundConcurrencySettings::StaticStruct(),
					TEXT("VolumeScale"));
			if (!VolumeScaleProperty)
			{
				return false;
			}
			VolumeScaleProperty->SetPropertyValue_InContainer(
				&OutSettings,
				VolumeScale);
			OutSettings.bLimitToOwner = bLimitToOwner;
			return true;
		}

		bool TryParseSoundMixModifiers(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FSoundClassAdjuster>& OutModifiers,
			TArray<TStrongObjectPtr<UObject>>& Pins,
			FExecutionMutationError& OutError)
		{
			OutModifiers.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue& Object = Item.Get();
				if (!HasExactObjectFields(
						Object,
						{
							TEXT("sound_class_ref"),
							TEXT("volume_adjuster"),
							TEXT("pitch_adjuster"),
							TEXT("low_pass_filter_frequency"),
							TEXT("apply_to_children"),
							TEXT(
								"voice_center_channel_volume_adjuster"),
						}))
				{
					return false;
				}
				USoundClass* SoundClass = nullptr;
				FSoundClassAdjuster Adjuster;
				bool bApplyToChildren = false;
				if (!TryResolveObject(
						Resolver,
						*FindObjectField(
							Object,
							TEXT("sound_class_ref")),
						SoundClass,
						OutError)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("volume_adjuster")),
						Adjuster.VolumeAdjuster,
						0.0,
						4.0)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("pitch_adjuster")),
						Adjuster.PitchAdjuster,
						0.0,
						8.0)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("low_pass_filter_frequency")),
						Adjuster.LowPassFilterFrequency,
						0.0,
						20000.0)
					|| !TryReadBool(
						*FindObjectField(
							Object,
							TEXT("apply_to_children")),
						bApplyToChildren)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT(
								"voice_center_channel_volume_adjuster")),
						Adjuster.VoiceCenterChannelVolumeAdjuster,
						0.0,
						4.0))
				{
					return false;
				}
				for (const FSoundClassAdjuster& Existing : OutModifiers)
				{
					if (Existing.SoundClassObject == SoundClass)
					{
						return false;
					}
				}
				Adjuster.SoundClassObject = SoundClass;
				Adjuster.bApplyToChildren = bApplyToChildren;
				OutModifiers.Add(Adjuster);
				PinUnique(Pins, SoundClass);
			}
			return true;
		}

		bool TryApplyCollectionPresence(
			const FString& Presence,
			const TArray<FSoundClassAdjuster>& Requested,
			TArray<FSoundClassAdjuster>& InOutModifiers)
		{
			if (Presence == TEXT("clear"))
			{
				InOutModifiers.Reset();
				return true;
			}
			if (Presence == TEXT("replace"))
			{
				InOutModifiers = Requested;
				return true;
			}
			if (Presence == TEXT("remove"))
			{
				for (const FSoundClassAdjuster& Item : Requested)
				{
					InOutModifiers.RemoveAll(
						[&Item](const FSoundClassAdjuster& Existing)
						{
							return Existing.SoundClassObject
								== Item.SoundClassObject;
						});
				}
				return true;
			}
			if (Presence == TEXT("set"))
			{
				for (const FSoundClassAdjuster& Item : Requested)
				{
					const int32 Index =
						InOutModifiers.IndexOfByPredicate(
							[&Item](
								const FSoundClassAdjuster& Existing)
							{
								return Existing.SoundClassObject
									== Item.SoundClassObject;
							});
					if (Index == INDEX_NONE)
					{
						InOutModifiers.Add(Item);
					}
					else
					{
						InOutModifiers[Index] = Item;
					}
				}
				return true;
			}
			return false;
		}

		struct FSoundSubmixNodeSnapshot final
		{
			TWeakObjectPtr<USoundSubmixBase> Node;
			TArray<TWeakObjectPtr<USoundSubmixBase>> Children;
			TWeakObjectPtr<USoundSubmixBase> Parent;
		};

		void CaptureSoundSubmixNode(
			TArray<FSoundSubmixNodeSnapshot>& Snapshots,
			TArray<FPackageDirtySnapshot>& Packages,
			USoundSubmixBase* Node)
		{
			if (!Node)
			{
				return;
			}
			for (const FSoundSubmixNodeSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Node.Get() == Node)
				{
					return;
				}
			}
			FSoundSubmixNodeSnapshot Snapshot;
			Snapshot.Node = Node;
			for (USoundSubmixBase* Child : Node->ChildSubmixes)
			{
				Snapshot.Children.Add(Child);
			}
			if (USoundSubmixWithParentBase* WithParent =
				Cast<USoundSubmixWithParentBase>(Node))
			{
				Snapshot.Parent = WithParent->ParentSubmix;
			}
			Snapshots.Add(MoveTemp(Snapshot));
			CapturePackage(Packages, Node);
		}

		bool WouldCreateSubmixCycle(
			USoundSubmix* Target,
			USoundSubmixBase* NewParent)
		{
			for (USoundSubmixBase* Node = NewParent;
				Node;
				)
			{
				if (Node == Target)
				{
					return true;
				}
				const USoundSubmixWithParentBase* WithParent =
					Cast<USoundSubmixWithParentBase>(Node);
				Node = WithParent
					? WithParent->ParentSubmix.Get()
					: nullptr;
			}
			return false;
		}

		bool TryParseInterpMode(
			const FString& Name,
			ERichCurveInterpMode& OutMode)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("constant"))
			{
				OutMode = RCIM_Constant;
			}
			else if (Value == TEXT("linear"))
			{
				OutMode = RCIM_Linear;
			}
			else if (Value == TEXT("cubic"))
			{
				OutMode = RCIM_Cubic;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryParseTangentMode(
			const FString& Name,
			ERichCurveTangentMode& OutMode)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("auto"))
			{
				OutMode = RCTM_Auto;
			}
			else if (Value == TEXT("user"))
			{
				OutMode = RCTM_User;
			}
			else if (Value == TEXT("break"))
			{
				OutMode = RCTM_Break;
			}
			else if (Value == TEXT("smart_auto"))
			{
				OutMode = RCTM_SmartAuto;
			}
			else if (Value == TEXT("none"))
			{
				OutMode = RCTM_None;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryParseTangentWeightMode(
			const FString& Name,
			ERichCurveTangentWeightMode& OutMode)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("none"))
			{
				OutMode = RCTWM_WeightedNone;
			}
			else if (Value == TEXT("arrive"))
			{
				OutMode = RCTWM_WeightedArrive;
			}
			else if (Value == TEXT("leave"))
			{
				OutMode = RCTWM_WeightedLeave;
			}
			else if (Value == TEXT("both"))
			{
				OutMode = RCTWM_WeightedBoth;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryReadFloatArray(
			const FUeAgentCanonicalValue& Value,
			const int32 ExpectedCount,
			TArray<float>& OutValues)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() != ExpectedCount)
			{
				return false;
			}
			OutValues.Reset(ExpectedCount);
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				float Number = 0.0f;
				if (!TryReadFloat(
						Item.Get(),
						Number,
						-TNumericLimits<float>::Max(),
						TNumericLimits<float>::Max()))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		int32 GetCurveDimension(UObject* Target)
		{
			if (Cast<UCurveFloat>(Target))
			{
				return 1;
			}
			if (Cast<UCurveVector>(Target))
			{
				return 3;
			}
			if (Cast<UCurveLinearColor>(Target))
			{
				return 4;
			}
			return 0;
		}

		FRichCurve* GetCurveChannel(
			UObject* Target,
			const int32 Index)
		{
			if (UCurveFloat* Curve = Cast<UCurveFloat>(Target))
			{
				return Index == 0 ? &Curve->FloatCurve : nullptr;
			}
			if (UCurveVector* Curve = Cast<UCurveVector>(Target))
			{
				return Index >= 0 && Index < 3
					? &Curve->FloatCurves[Index]
					: nullptr;
			}
			if (UCurveLinearColor* Curve =
				Cast<UCurveLinearColor>(Target))
			{
				return Index >= 0 && Index < 4
					? &Curve->FloatCurves[Index]
					: nullptr;
			}
			return nullptr;
		}

		bool TryParseCurveKeys(
			const FUeAgentCanonicalValue& Value,
			const int32 Dimension,
			TArray<TArray<FRichCurveKey>>& OutChannels)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Dimension < 1
				|| Dimension > 4)
			{
				return false;
			}
			OutChannels.SetNum(Dimension);
			float PreviousTime = 0.0f;
			bool bHasPreviousTime = false;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue& KeyObject = Item.Get();
				if (!HasExactObjectFields(
						KeyObject,
						{
							TEXT("time"),
							TEXT("values"),
							TEXT("interp_mode"),
							TEXT("tangent_mode"),
							TEXT("tangent_weight_mode"),
							TEXT("arrive_tangents"),
							TEXT("leave_tangents"),
							TEXT("arrive_tangent_weights"),
							TEXT("leave_tangent_weights"),
						}))
				{
					return false;
				}
				float Time = 0.0f;
				FString InterpName;
				FString TangentName;
				FString WeightName;
				ERichCurveInterpMode InterpMode = RCIM_Linear;
				ERichCurveTangentMode TangentMode = RCTM_Auto;
				ERichCurveTangentWeightMode WeightMode =
					RCTWM_WeightedNone;
				TArray<float> Values;
				TArray<float> ArriveTangents;
				TArray<float> LeaveTangents;
				TArray<float> ArriveWeights;
				TArray<float> LeaveWeights;
				if (!TryReadFloat(
						*FindObjectField(KeyObject, TEXT("time")),
						Time,
						-TNumericLimits<float>::Max(),
						TNumericLimits<float>::Max())
					|| (bHasPreviousTime && Time <= PreviousTime)
					|| !TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("interp_mode")),
						InterpName)
					|| !TryParseInterpMode(InterpName, InterpMode)
					|| !TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("tangent_mode")),
						TangentName)
					|| !TryParseTangentMode(
						TangentName,
						TangentMode)
					|| !TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("tangent_weight_mode")),
						WeightName)
					|| !TryParseTangentWeightMode(
						WeightName,
						WeightMode)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("values")),
						Dimension,
						Values)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("arrive_tangents")),
						Dimension,
						ArriveTangents)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("leave_tangents")),
						Dimension,
						LeaveTangents)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("arrive_tangent_weights")),
						Dimension,
						ArriveWeights)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("leave_tangent_weights")),
						Dimension,
						LeaveWeights))
				{
					return false;
				}
				for (int32 ChannelIndex = 0;
					ChannelIndex < Dimension;
					++ChannelIndex)
				{
					FRichCurveKey Key(Time, Values[ChannelIndex]);
					Key.InterpMode = InterpMode;
					Key.TangentMode = TangentMode;
					Key.TangentWeightMode = WeightMode;
					Key.ArriveTangent = ArriveTangents[ChannelIndex];
					Key.LeaveTangent = LeaveTangents[ChannelIndex];
					Key.ArriveTangentWeight =
						ArriveWeights[ChannelIndex];
					Key.LeaveTangentWeight =
						LeaveWeights[ChannelIndex];
					OutChannels[ChannelIndex].Add(Key);
				}
				PreviousTime = Time;
				bHasPreviousTime = true;
			}
			return true;
		}

		bool WriteCurveChannels(
			UObject* Target,
			const TArray<TArray<FRichCurveKey>>& Channels,
			const bool bModify)
		{
			if (!Target
				|| Channels.Num() != GetCurveDimension(Target))
			{
				return false;
			}
			if (bModify)
			{
				Target->Modify();
			}
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
				if (!Curve)
				{
					return false;
				}
				Curve->SetKeys(Channels[Index]);
			}
			Target->PostEditChange();
			return true;
		}

		enum class ERegisteredFieldKind : uint8
		{
			String,
			Int32,
			Float,
			Boolean,
			AssetReference,
		};

		struct FRegisteredFieldValue final
		{
			FProperty* Property = nullptr;
			ERegisteredFieldKind Kind =
				ERegisteredFieldKind::String;
			FString StringValue;
			int32 IntValue = 0;
			float FloatValue = 0.0f;
			bool bBoolValue = false;
			TWeakObjectPtr<UObject> ObjectValue;
		};

		bool TryClassifyRegisteredField(
			FProperty* Property,
			ERegisteredFieldKind& OutKind)
		{
			if (!Property)
			{
				return false;
			}
			if (CastField<FStrProperty>(Property))
			{
				OutKind = ERegisteredFieldKind::String;
				return true;
			}
			if (CastField<FIntProperty>(Property))
			{
				OutKind = ERegisteredFieldKind::Int32;
				return true;
			}
			if (CastField<FFloatProperty>(Property))
			{
				OutKind = ERegisteredFieldKind::Float;
				return true;
			}
			if (CastField<FBoolProperty>(Property))
			{
				OutKind = ERegisteredFieldKind::Boolean;
				return true;
			}
			if (CastField<FObjectPropertyBase>(Property)
				&& Property->GetMetaData(TEXT("UAIAssetDataType"))
					== TEXT("asset_ref"))
			{
				OutKind = ERegisteredFieldKind::AssetReference;
				return true;
			}
			return false;
		}

		void ReadRegisteredValue(
			UObject* Container,
			FRegisteredFieldValue& InOutValue)
		{
			switch (InOutValue.Kind)
			{
			case ERegisteredFieldKind::String:
				InOutValue.StringValue =
					CastFieldChecked<FStrProperty>(
						InOutValue.Property)
						->GetPropertyValue_InContainer(Container);
				break;
			case ERegisteredFieldKind::Int32:
				InOutValue.IntValue =
					CastFieldChecked<FIntProperty>(
						InOutValue.Property)
						->GetPropertyValue_InContainer(Container);
				break;
			case ERegisteredFieldKind::Float:
				InOutValue.FloatValue =
					CastFieldChecked<FFloatProperty>(
						InOutValue.Property)
						->GetPropertyValue_InContainer(Container);
				break;
			case ERegisteredFieldKind::Boolean:
				InOutValue.bBoolValue =
					CastFieldChecked<FBoolProperty>(
						InOutValue.Property)
						->GetPropertyValue_InContainer(Container);
				break;
			case ERegisteredFieldKind::AssetReference:
				InOutValue.ObjectValue =
					CastFieldChecked<FObjectPropertyBase>(
						InOutValue.Property)
						->GetObjectPropertyValue_InContainer(
							Container);
				break;
			}
		}

		void WriteRegisteredValue(
			UObject* Container,
			const FRegisteredFieldValue& Value)
		{
			switch (Value.Kind)
			{
			case ERegisteredFieldKind::String:
				CastFieldChecked<FStrProperty>(Value.Property)
					->SetPropertyValue_InContainer(
						Container,
						Value.StringValue);
				break;
			case ERegisteredFieldKind::Int32:
				CastFieldChecked<FIntProperty>(Value.Property)
					->SetPropertyValue_InContainer(
						Container,
						Value.IntValue);
				break;
			case ERegisteredFieldKind::Float:
				CastFieldChecked<FFloatProperty>(Value.Property)
					->SetPropertyValue_InContainer(
						Container,
						Value.FloatValue);
				break;
			case ERegisteredFieldKind::Boolean:
				CastFieldChecked<FBoolProperty>(Value.Property)
					->SetPropertyValue_InContainer(
						Container,
						Value.bBoolValue);
				break;
			case ERegisteredFieldKind::AssetReference:
				CastFieldChecked<FObjectPropertyBase>(Value.Property)
					->SetObjectPropertyValue_InContainer(
						Container,
						Value.ObjectValue.Get());
				break;
			}
		}

		bool TryParseRegisteredFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UObject* Target,
			const FUeAgentCanonicalValue* Value,
			const bool bClear,
			TArray<FRegisteredFieldValue>& OutOriginal,
			TArray<FRegisteredFieldValue>& OutNew,
			TArray<TStrongObjectPtr<UObject>>& Pins,
			FExecutionMutationError& OutError)
		{
			if (!Target
				|| (!bClear
					&& (!Value
						|| Value->GetKind()
							!= EUeAgentValueKind::Object)))
			{
				return false;
			}
			TMap<FString, FProperty*> Registered;
			for (TFieldIterator<FProperty> It(Target->GetClass());
				It;
				++It)
			{
				FProperty* Property = *It;
				if (!Property->HasMetaData(TEXT("UAIAssetDataField")))
				{
					continue;
				}
				const FString PublicId =
					Property->GetMetaData(
						TEXT("UAIAssetDataField"));
				ERegisteredFieldKind Kind;
				if (PublicId.IsEmpty()
					|| Registered.Contains(PublicId)
					|| !TryClassifyRegisteredField(Property, Kind))
				{
					return false;
				}
				Registered.Add(PublicId, Property);
			}
			if (Registered.IsEmpty())
			{
				return false;
			}
			if (!bClear)
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>&
					Item : Value->GetObjectItems())
				{
					if (!Registered.Contains(Item.Key))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_registered_field_unknown"),
							Item.Key);
					}
				}
			}
			UObject* Default = Target->GetClass()->GetDefaultObject();
			for (const TPair<FString, FProperty*>& Item : Registered)
			{
				const FUeAgentCanonicalValue* Requested =
					bClear ? nullptr : FindObjectField(*Value, Item.Key);
				if (!bClear && !Requested)
				{
					continue;
				}
				FRegisteredFieldValue Original;
				Original.Property = Item.Value;
				if (!TryClassifyRegisteredField(
						Item.Value,
						Original.Kind))
				{
					return false;
				}
				ReadRegisteredValue(Target, Original);
				FRegisteredFieldValue NewValue = Original;
				if (bClear)
				{
					ReadRegisteredValue(Default, NewValue);
				}
				else
				{
					switch (NewValue.Kind)
					{
					case ERegisteredFieldKind::String:
						if (!TryReadString(
								*Requested,
								NewValue.StringValue))
						{
							return false;
						}
						break;
					case ERegisteredFieldKind::Int32:
						if (!TryReadInt32(
								*Requested,
								NewValue.IntValue,
								MIN_int32,
								MAX_int32))
						{
							return false;
						}
						break;
					case ERegisteredFieldKind::Float:
						if (!TryReadFloat(
								*Requested,
								NewValue.FloatValue,
								-TNumericLimits<float>::Max(),
								TNumericLimits<float>::Max()))
						{
							return false;
						}
						break;
					case ERegisteredFieldKind::Boolean:
						if (!TryReadBool(
								*Requested,
								NewValue.bBoolValue))
						{
							return false;
						}
						break;
					case ERegisteredFieldKind::AssetReference:
						if (Requested->GetKind()
							== EUeAgentValueKind::Null)
						{
							NewValue.ObjectValue = nullptr;
						}
						else
						{
							UObject* Resolved = nullptr;
							if (!TryResolveObject(
									Resolver,
									*Requested,
									Resolved,
									OutError)
								|| !Resolved->IsA(
									CastFieldChecked<
										FObjectPropertyBase>(
											Item.Value)
										->PropertyClass))
							{
								return false;
							}
							NewValue.ObjectValue = Resolved;
							PinUnique(Pins, Resolved);
						}
						break;
					}
				}
				OutOriginal.Add(MoveTemp(Original));
				OutNew.Add(MoveTemp(NewValue));
			}
			return !OutNew.IsEmpty();
		}

		bool TryBuildRegisteredFieldProjection(
			UObject* Container,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutFields,
			FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			OutFields.Reset();
			OutContext.Reset();
			if (!Container)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_registered_projection_target_missing"));
			}

			struct FProjectedProperty final
			{
				FString PublicId;
				FProperty* Property = nullptr;
				ERegisteredFieldKind Kind =
					ERegisteredFieldKind::String;
			};
			TArray<FProjectedProperty> Properties;
			TSet<FString> SeenPublicIds;
			for (TFieldIterator<FProperty> It(
					Container->GetClass());
				It;
				++It)
			{
				FProperty* Property = *It;
				if (!Property->HasMetaData(
						TEXT("UAIAssetDataField")))
				{
					continue;
				}
				FProjectedProperty Projected;
				Projected.PublicId =
					Property->GetMetaData(
						TEXT("UAIAssetDataField"));
				Projected.Property = Property;
				if (Projected.PublicId.IsEmpty()
					|| SeenPublicIds.Contains(
						Projected.PublicId)
					|| !TryClassifyRegisteredField(
						Property,
						Projected.Kind))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_registered_projection_metadata_invalid"),
						Projected.PublicId);
				}
				SeenPublicIds.Add(Projected.PublicId);
				Properties.Add(MoveTemp(Projected));
			}
			Properties.Sort(
				[](const FProjectedProperty& Left,
					const FProjectedProperty& Right)
				{
					return Left.PublicId < Right.PublicId;
				});
			if (Properties.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_registered_projection_fields_missing"));
			}

			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				FieldItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			FUeAgentStateSemanticError StateError;
			for (const FProjectedProperty& Projected
				: Properties)
			{
				FUeAgentCanonicalValuePtr Value;
				FUeAgentCanonicalValuePtr Kind;
				bool bBuilt = false;
				FString KindId;
				switch (Projected.Kind)
				{
				case ERegisteredFieldKind::String:
					KindId = TEXT("string");
					bBuilt =
						FUeAgentCanonicalValue::TryString(
							CastFieldChecked<FStrProperty>(
								Projected.Property)
								->GetPropertyValue_InContainer(
									Container),
							Resources,
							Value,
							StateError);
					break;
				case ERegisteredFieldKind::Int32:
					KindId = TEXT("integer");
					bBuilt =
						FUeAgentCanonicalValue::TryInteger(
							FString::FromInt(
								CastFieldChecked<FIntProperty>(
									Projected.Property)
									->GetPropertyValue_InContainer(
										Container)),
							Resources,
							Value,
							StateError);
					break;
				case ERegisteredFieldKind::Float:
					KindId = TEXT("decimal");
					bBuilt =
						FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(
								static_cast<double>(
									CastFieldChecked<
										FFloatProperty>(
											Projected.Property)
										->GetPropertyValue_InContainer(
											Container))),
							Resources,
							Value,
							StateError);
					break;
				case ERegisteredFieldKind::Boolean:
					KindId = TEXT("boolean");
					bBuilt =
						FUeAgentCanonicalValue::TryBoolean(
							CastFieldChecked<FBoolProperty>(
								Projected.Property)
								->GetPropertyValue_InContainer(
									Container),
							Resources,
							Value,
							StateError);
					break;
				case ERegisteredFieldKind::AssetReference:
					{
						KindId = TEXT("asset_ref");
						UObject* Object =
							CastFieldChecked<
								FObjectPropertyBase>(
									Projected.Property)
								->GetObjectPropertyValue_InContainer(
									Container);
						bBuilt = Object
							? FUeAgentCanonicalValue::TryString(
								Object->GetPathName(),
								Resources,
								Value,
								StateError)
							: FUeAgentCanonicalValue::TryNull(
								Resources,
								Value,
								StateError);
						break;
					}
				}
				if (!bBuilt
					|| !FUeAgentCanonicalValue::TryString(
						KindId,
						Resources,
						Kind,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code.IsEmpty()
							? TEXT(
								"asset_data_registered_projection_value_invalid")
							: StateError.Code,
						Projected.PublicId);
				}
				FieldItems.Emplace(
					Projected.PublicId,
					Value.ToSharedRef());
				ContextItems.Emplace(
					Projected.PublicId,
					Kind.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryObject(
					FieldItems,
					Resources,
					OutFields,
					StateError)
				&& FUeAgentCanonicalValue::TryObject(
					ContextItems,
					Resources,
					OutContext,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_registered_projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryWrapRegisteredFieldProjection(
			const FUeAgentCanonicalValueRef& RegisteredFields,
			const FUeAgentCanonicalValueRef& RegisteredContext,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutFields,
			FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (FUeAgentCanonicalValue::TryObject(
					{
						TPair<FString, FUeAgentCanonicalValueRef>(
							TEXT("registered_fields"),
							RegisteredFields),
					},
					Resources,
					OutFields,
					StateError)
				&& FUeAgentCanonicalValue::TryObject(
					{
						TPair<FString, FUeAgentCanonicalValueRef>(
							TEXT("registered_fields"),
							RegisteredContext),
					},
					Resources,
					OutContext,
					StateError))
			{
				return true;
			}
			return Fail(
				OutError,
				StateError.Code.IsEmpty()
					? TEXT(
						"asset_data_registered_projection_wrapper_invalid")
					: StateError.Code,
				StateError.Detail);
		}

		bool TryBuildExpectedRegisteredFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			OutExpected.Reset();
			const FUeAgentCanonicalValue* Current =
				FindObjectField(
					CurrentFields,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValueRef* DefaultsRef =
				FindObjectFieldRef(
					DefaultFields,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValueRef* ContextRef =
				FindObjectFieldRef(
					ProjectionContext,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValue* Defaults =
				DefaultsRef ? &DefaultsRef->Get() : nullptr;
			const FUeAgentCanonicalValue* Context =
				ContextRef ? &ContextRef->Get() : nullptr;
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
			if (!Current
				|| Current->GetKind()
					!= EUeAgentValueKind::Object
				|| !Defaults
				|| Defaults->GetKind()
					!= EUeAgentValueKind::Object
				|| !Context
				|| Context->GetKind()
					!= EUeAgentValueKind::Object
				|| !TypedChanges
				|| TypedChanges->GetKind()
					!= EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num() != 1)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_registered_expected_state_invalid"));
			}
			const FUeAgentCanonicalValue& Change =
				TypedChanges->GetArrayItems()[0].Get();
			FString FieldId;
			FString Presence;
			const FUeAgentCanonicalValue* Field =
				FindObjectField(Change, TEXT("field_id"));
			const FUeAgentCanonicalValue* PresenceValue =
				FindObjectField(Change, TEXT("presence"));
			if (!Field
				|| !TryReadString(*Field, FieldId)
				|| FieldId != TEXT("registered_fields")
				|| !PresenceValue
				|| !TryReadString(
					*PresenceValue,
					Presence))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_registered_expected_change_invalid"));
			}

			if (Presence == TEXT("clear"))
			{
				FUeAgentCanonicalValuePtr WrappedContext;
				return TryWrapRegisteredFieldProjection(
					*DefaultsRef,
					*ContextRef,
					Resources,
					OutExpected,
					WrappedContext,
					OutError);
			}
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			const FUeAgentCanonicalValue* Requested =
				TypedValue
					? FindObjectField(
						*TypedValue,
						TEXT("canonical_value"))
					: nullptr;
			if (Presence != TEXT("set")
				|| !Requested
				|| Requested->GetKind()
					!= EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_registered_expected_value_invalid"));
			}

			TMap<FString, FUeAgentCanonicalValueRef> Merged;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Current->GetObjectItems())
			{
				Merged.Add(Item.Key, Item.Value);
			}
			FUeAgentStateSemanticError StateError;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Requested->GetObjectItems())
			{
				const FUeAgentCanonicalValue* KindValue =
					FindObjectField(*Context, Item.Key);
				FString Kind;
				if (!KindValue
					|| !TryReadString(*KindValue, Kind)
					|| !Merged.Contains(Item.Key))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_registered_expected_field_unknown"),
						Item.Key);
				}
				FUeAgentCanonicalValuePtr Normalized;
				bool bBuilt = false;
				if (Kind == TEXT("string"))
				{
					bBuilt =
						Item.Value->GetKind()
								== EUeAgentValueKind::String
							&& FUeAgentCanonicalValue::TryString(
								Item.Value->GetText(),
								Resources,
								Normalized,
								StateError);
				}
				else if (Kind == TEXT("integer"))
				{
					int32 Value = 0;
					bBuilt = TryReadInt32(
							Item.Value.Get(),
							Value,
							MIN_int32,
							MAX_int32)
						&& FUeAgentCanonicalValue::TryInteger(
							FString::FromInt(Value),
							Resources,
							Normalized,
							StateError);
				}
				else if (Kind == TEXT("decimal"))
				{
					float Value = 0.0f;
					bBuilt = TryReadFloat(
							Item.Value.Get(),
							Value,
							-TNumericLimits<float>::Max(),
							TNumericLimits<float>::Max())
						&& FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(
								static_cast<double>(Value)),
							Resources,
							Normalized,
							StateError);
				}
				else if (Kind == TEXT("boolean"))
				{
					bool bValue = false;
					bBuilt = TryReadBool(
							Item.Value.Get(),
							bValue)
						&& FUeAgentCanonicalValue::TryBoolean(
							bValue,
							Resources,
							Normalized,
							StateError);
				}
				else if (Kind == TEXT("asset_ref"))
				{
					if (Item.Value->GetKind()
						== EUeAgentValueKind::Null)
					{
						bBuilt =
							FUeAgentCanonicalValue::TryNull(
								Resources,
								Normalized,
								StateError);
					}
					else if (Item.Value->GetKind()
						== EUeAgentValueKind::String)
					{
						UObject* Resolved = nullptr;
						bBuilt =
							Resolver.TryResolveExisting(
								Item.Value->GetText(),
								Resolved,
								OutError)
							&& IsValid(Resolved)
							&& FUeAgentCanonicalValue::TryString(
								Resolved->GetPathName(),
								Resources,
								Normalized,
								StateError);
					}
				}
				if (!bBuilt || !Normalized.IsValid())
				{
					return Fail(
						OutError,
						OutError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_registered_expected_field_invalid")
							: OutError.GetCode(),
						Item.Key);
				}
				Merged[Item.Key] =
					Normalized.ToSharedRef();
			}

			TArray<FString> Keys;
			Merged.GetKeys(Keys);
			Keys.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				MergedItems;
			for (const FString& Key : Keys)
			{
				MergedItems.Emplace(Key, Merged[Key]);
			}
			FUeAgentCanonicalValuePtr MergedObject;
			if (!FUeAgentCanonicalValue::TryObject(
					MergedItems,
					Resources,
					MergedObject,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_registered_expected_object_invalid")
						: StateError.Code,
					StateError.Detail);
			}
			FUeAgentCanonicalValuePtr WrappedContext;
			return TryWrapRegisteredFieldProjection(
				MergedObject.ToSharedRef(),
				*ContextRef,
				Resources,
				OutExpected,
				WrappedContext,
				OutError);
		}

		bool TryNormalizeFileUri(
			const FUeAgentCanonicalValue& Value,
			FString& OutPath)
		{
			FString Uri;
			if (!TryReadString(Value, Uri))
			{
				return false;
			}
			Uri.TrimStartAndEndInline();
			if (Uri.IsEmpty())
			{
				return false;
			}
			if (Uri.StartsWith(
					TEXT("file:"),
					ESearchCase::IgnoreCase))
			{
				const FString Remainder = Uri.Mid(5);
				if (Remainder.StartsWith(TEXT("///")))
				{
					const FString AbsolutePart =
						Remainder.Mid(3);
					const bool bWindowsDrive =
						AbsolutePart.Len() >= 2
						&& FChar::IsAlpha(AbsolutePart[0])
						&& AbsolutePart[1] == TEXT(':');
					OutPath = bWindowsDrive
						? AbsolutePart
						: TEXT("/") + AbsolutePart;
				}
				else if (Remainder.StartsWith(TEXT("//")))
				{
					OutPath = Remainder;
				}
				else if (Remainder.StartsWith(TEXT("/")))
				{
					OutPath = Remainder;
				}
				else
				{
					return false;
				}
			}
			else
			{
				OutPath = Uri;
			}
			return !OutPath.IsEmpty()
				&& !OutPath.Contains(TEXT("\r"))
				&& !OutPath.Contains(TEXT("\n"));
		}

		bool TryParseRuntimeVirtualTextureMaterialType(
			const FString& Name,
			ERuntimeVirtualTextureMaterialType& OutType)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("base_color"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::BaseColor;
			}
			else if (Value == TEXT("mask4"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::Mask4;
			}
			else if (Value == TEXT("base_color_normal_roughness"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Roughness;
			}
			else if (Value == TEXT("base_color_normal_specular"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular;
			}
			else if (
				Value
				== TEXT("base_color_normal_specular_ycocg"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular_YCoCg;
			}
			else if (
				Value
				== TEXT(
					"base_color_normal_specular_mask_ycocg"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular_Mask_YCoCg;
			}
			else if (Value == TEXT("world_height"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::WorldHeight;
			}
			else if (Value == TEXT("displacement"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::Displacement;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryReadRvtProperties(
			URuntimeVirtualTexture* Target,
			int32& OutTileCount,
			ERuntimeVirtualTextureMaterialType& OutMaterialType,
			bool& bOutCompression)
		{
			FIntProperty* TileCount =
				FindFProperty<FIntProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("TileCount"));
			FEnumProperty* MaterialType =
				FindFProperty<FEnumProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("MaterialType"));
			FBoolProperty* Compression =
				FindFProperty<FBoolProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("bCompressTextures"));
			if (!Target || !TileCount || !MaterialType || !Compression)
			{
				return false;
			}
			OutTileCount =
				TileCount->GetPropertyValue_InContainer(Target);
			OutMaterialType =
				static_cast<ERuntimeVirtualTextureMaterialType>(
					MaterialType->GetUnderlyingProperty()
						->GetUnsignedIntPropertyValue(
							MaterialType->ContainerPtrToValuePtr<void>(
								Target)));
			bOutCompression =
				Compression->GetPropertyValue_InContainer(Target);
			return true;
		}

		bool TryWriteRvtProperties(
			URuntimeVirtualTexture* Target,
			const int32 TileCountValue,
			const ERuntimeVirtualTextureMaterialType MaterialTypeValue,
			const bool bCompression,
			const bool bModify)
		{
			FIntProperty* TileCount =
				FindFProperty<FIntProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("TileCount"));
			FEnumProperty* MaterialType =
				FindFProperty<FEnumProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("MaterialType"));
			FBoolProperty* Compression =
				FindFProperty<FBoolProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("bCompressTextures"));
			if (!Target || !TileCount || !MaterialType || !Compression)
			{
				return false;
			}
			if (bModify)
			{
				Target->Modify();
			}
			TileCount->SetPropertyValue_InContainer(
				Target,
				TileCountValue);
			MaterialType->GetUnderlyingProperty()
				->SetIntPropertyValue(
					MaterialType->ContainerPtrToValuePtr<void>(
						Target),
					static_cast<uint64>(MaterialTypeValue));
			Compression->SetPropertyValue_InContainer(
				Target,
				bCompression);
			Target->PostEditChange();
			return true;
		}

		bool TryParseTextureAddress(
			const FString& Name,
			TEnumAsByte<TextureAddress>& OutAddress)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("wrap"))
			{
				OutAddress = TA_Wrap;
			}
			else if (Value == TEXT("clamp"))
			{
				OutAddress = TA_Clamp;
			}
			else if (Value == TEXT("mirror"))
			{
				OutAddress = TA_Mirror;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryFormatTextureAddress(
			const TEnumAsByte<TextureAddress> Address,
			FString& OutName)
		{
			switch (Address.GetValue())
			{
			case TA_Wrap:
				OutName = TEXT("wrap");
				return true;
			case TA_Clamp:
				OutName = TEXT("clamp");
				return true;
			case TA_Mirror:
				OutName = TEXT("mirror");
				return true;
			default:
				OutName.Reset();
				return false;
			}
		}

		bool TryFormatRuntimeVirtualTextureMaterialType(
			const ERuntimeVirtualTextureMaterialType Type,
			FString& OutName)
		{
			switch (Type)
			{
			case ERuntimeVirtualTextureMaterialType::BaseColor:
				OutName = TEXT("base_color");
				return true;
			case ERuntimeVirtualTextureMaterialType::Mask4:
				OutName = TEXT("mask4");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Roughness:
				OutName = TEXT("base_color_normal_roughness");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular:
				OutName = TEXT("base_color_normal_specular");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular_YCoCg:
				OutName =
					TEXT("base_color_normal_specular_ycocg");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular_Mask_YCoCg:
				OutName = TEXT(
					"base_color_normal_specular_mask_ycocg");
				return true;
			case ERuntimeVirtualTextureMaterialType::WorldHeight:
				OutName = TEXT("world_height");
				return true;
			case ERuntimeVirtualTextureMaterialType::Displacement:
				OutName = TEXT("displacement");
				return true;
			default:
				OutName.Reset();
				return false;
			}
		}

		bool TryBuildStringProjectionValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_string_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildIntegerProjectionValue(
			const int32 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Value),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_integer_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildDecimalProjectionValue(
			const float Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FMath::IsFinite(Value)
				&& FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(
						static_cast<double>(Value)),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_decimal_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildBooleanProjectionValue(
			const bool bValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_boolean_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildObjectProjectionValue(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			Items.Sort(
				[](
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Left,
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Right)
				{
					return Left.Key < Right.Key;
				});
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildArrayProjectionValue(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_array_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildNullProjectionValue(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_core_projection_null_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildObjectReferenceProjection(
			const UObject* Object,
			const bool bNullable,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (!Object)
			{
				return bNullable
					? TryBuildNullProjectionValue(
						Resources,
						OutValue,
						OutError)
					: Fail(
						OutError,
						TEXT(
							"asset_data_core_projection_reference_missing"));
			}
			return TryBuildStringProjectionValue(
				Object->GetPathName(),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildObjectReferenceArrayProjection(
			const TArray<UObject*>& Objects,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			TSet<const UObject*> Seen;
			for (const UObject* Object : Objects)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!Object
					|| Seen.Contains(Object)
					|| !TryBuildObjectReferenceProjection(
						Object,
						false,
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Seen.Add(Object);
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryFormatDistanceAlgorithm(
			const EAttenuationDistanceModel Value,
			FString& OutName)
		{
			switch (Value)
			{
			case EAttenuationDistanceModel::Linear:
				OutName = TEXT("linear");
				return true;
			case EAttenuationDistanceModel::Logarithmic:
				OutName = TEXT("logarithmic");
				return true;
			case EAttenuationDistanceModel::Inverse:
				OutName = TEXT("inverse");
				return true;
			case EAttenuationDistanceModel::LogReverse:
				OutName = TEXT("log_reverse");
				return true;
			case EAttenuationDistanceModel::NaturalSound:
				OutName = TEXT("natural_sound");
				return true;
			case EAttenuationDistanceModel::Custom:
				OutName = TEXT("custom");
				return true;
			default:
				return false;
			}
		}

		bool TryFormatConcurrencyResolutionRule(
			const EMaxConcurrentResolutionRule::Type Value,
			FString& OutName)
		{
			switch (Value)
			{
			case EMaxConcurrentResolutionRule::PreventNew:
				OutName = TEXT("prevent_new");
				return true;
			case EMaxConcurrentResolutionRule::StopOldest:
				OutName = TEXT("stop_oldest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopFarthestThenOldest:
				OutName = TEXT("stop_farthest_then_oldest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopFarthestThenPreventNew:
				OutName =
					TEXT("stop_farthest_then_prevent_new");
				return true;
			case EMaxConcurrentResolutionRule::StopLowestPriority:
				OutName = TEXT("stop_lowest_priority");
				return true;
			case EMaxConcurrentResolutionRule::StopQuietest:
				OutName = TEXT("stop_quietest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopLowestPriorityThenPreventNew:
				OutName =
					TEXT(
						"stop_lowest_priority_then_prevent_new");
				return true;
			default:
				return false;
			}
		}

		bool TryFormatConcurrencyVolumeScaleMode(
			const EConcurrencyVolumeScaleMode Value,
			FString& OutName)
		{
			switch (Value)
			{
			case EConcurrencyVolumeScaleMode::Default:
				OutName = TEXT("default");
				return true;
			case EConcurrencyVolumeScaleMode::Distance:
				OutName = TEXT("distance");
				return true;
			case EConcurrencyVolumeScaleMode::Priority:
				OutName = TEXT("priority");
				return true;
			default:
				return false;
			}
		}

		bool TryFormatInterpMode(
			const ERichCurveInterpMode Value,
			FString& OutName)
		{
			switch (Value)
			{
			case RCIM_Constant:
				OutName = TEXT("constant");
				return true;
			case RCIM_Linear:
				OutName = TEXT("linear");
				return true;
			case RCIM_Cubic:
				OutName = TEXT("cubic");
				return true;
			default:
				return false;
			}
		}

		bool TryFormatTangentMode(
			const ERichCurveTangentMode Value,
			FString& OutName)
		{
			switch (Value)
			{
			case RCTM_Auto:
				OutName = TEXT("auto");
				return true;
			case RCTM_User:
				OutName = TEXT("user");
				return true;
			case RCTM_Break:
				OutName = TEXT("break");
				return true;
			case RCTM_SmartAuto:
				OutName = TEXT("smart_auto");
				return true;
			case RCTM_None:
				OutName = TEXT("none");
				return true;
			default:
				return false;
			}
		}

		bool TryFormatTangentWeightMode(
			const ERichCurveTangentWeightMode Value,
			FString& OutName)
		{
			switch (Value)
			{
			case RCTWM_WeightedNone:
				OutName = TEXT("none");
				return true;
			case RCTWM_WeightedArrive:
				OutName = TEXT("arrive");
				return true;
			case RCTWM_WeightedLeave:
				OutName = TEXT("leave");
				return true;
			case RCTWM_WeightedBoth:
				OutName = TEXT("both");
				return true;
			default:
				return false;
			}
		}

		bool TryBuildFloatArrayProjection(
			const TArray<float>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const float Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildDecimalProjectionValue(
						Value,
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildEqSettingsProjection(
			const FSubmixEffectSubmixEQSettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Bands;
			for (const FSubmixEffectEQBand& Band : Settings.EQBands)
			{
				FUeAgentCanonicalValuePtr Frequency;
				FUeAgentCanonicalValuePtr Bandwidth;
				FUeAgentCanonicalValuePtr Gain;
				FUeAgentCanonicalValuePtr Enabled;
				FUeAgentCanonicalValuePtr BandValue;
				if (!TryBuildDecimalProjectionValue(
						Band.Frequency,
						Resources,
						Frequency,
						OutError)
					|| !TryBuildDecimalProjectionValue(
						Band.Bandwidth,
						Resources,
						Bandwidth,
						OutError)
					|| !TryBuildDecimalProjectionValue(
						Band.GainDb,
						Resources,
						Gain,
						OutError)
					|| !TryBuildBooleanProjectionValue(
						Band.bEnabled,
						Resources,
						Enabled,
						OutError)
					|| !TryBuildObjectProjectionValue(
						{
							{
								TEXT("frequency_hz"),
								Frequency.ToSharedRef(),
							},
							{
								TEXT("bandwidth_octaves"),
								Bandwidth.ToSharedRef(),
							},
							{
								TEXT("gain_db"),
								Gain.ToSharedRef(),
							},
							{
								TEXT("enabled"),
								Enabled.ToSharedRef(),
							},
						},
						Resources,
						BandValue,
						OutError))
				{
					return false;
				}
				Bands.Add(BandValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr BandArray;
			return TryBuildArrayProjectionValue(
					Bands,
					Resources,
					BandArray,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("eq_bands"),
							BandArray.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildAttenuationProjection(
			const FSoundAttenuationSettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FString DistanceName;
			const FString SpatializationName =
				Settings.SpatializationAlgorithm
						== SPATIALIZATION_Default
					? TEXT("panning")
					: Settings.SpatializationAlgorithm
							== SPATIALIZATION_HRTF
						? TEXT("binaural")
						: FString();
			if (!TryFormatDistanceAlgorithm(
					Settings.DistanceAlgorithm,
					DistanceName)
				|| Settings.AttenuationShape
					!= EAttenuationShape::Sphere
				|| SpatializationName.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_sound_attenuation_projection_unsupported"));
			}
			FUeAgentCanonicalValuePtr Attenuate;
			FUeAgentCanonicalValuePtr Spatialize;
			FUeAgentCanonicalValuePtr Distance;
			FUeAgentCanonicalValuePtr Shape;
			FUeAgentCanonicalValuePtr InnerRadius;
			FUeAgentCanonicalValuePtr Falloff;
			FUeAgentCanonicalValuePtr Spatialization;
			FUeAgentCanonicalValuePtr Occlusion;
			FUeAgentCanonicalValuePtr OcclusionLowPass;
			FUeAgentCanonicalValuePtr OcclusionVolume;
			FUeAgentCanonicalValuePtr OcclusionTime;
			return TryBuildBooleanProjectionValue(
					Settings.bAttenuate,
					Resources,
					Attenuate,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Settings.bSpatialize,
					Resources,
					Spatialize,
					OutError)
				&& TryBuildStringProjectionValue(
					DistanceName,
					Resources,
					Distance,
					OutError)
				&& TryBuildStringProjectionValue(
					TEXT("sphere"),
					Resources,
					Shape,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.AttenuationShapeExtents.X,
					Resources,
					InnerRadius,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.FalloffDistance,
					Resources,
					Falloff,
					OutError)
				&& TryBuildStringProjectionValue(
					SpatializationName,
					Resources,
					Spatialization,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Settings.bEnableOcclusion,
					Resources,
					Occlusion,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionLowPassFilterFrequency,
					Resources,
					OcclusionLowPass,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionVolumeAttenuation,
					Resources,
					OcclusionVolume,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionInterpolationTime,
					Resources,
					OcclusionTime,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("attenuate"), Attenuate.ToSharedRef()},
						{TEXT("spatialize"), Spatialize.ToSharedRef()},
						{
							TEXT("distance_algorithm"),
							Distance.ToSharedRef(),
						},
						{
							TEXT("attenuation_shape"),
							Shape.ToSharedRef(),
						},
						{
							TEXT("inner_radius"),
							InnerRadius.ToSharedRef(),
						},
						{
							TEXT("falloff_distance"),
							Falloff.ToSharedRef(),
						},
						{
							TEXT("spatialization_algorithm"),
							Spatialization.ToSharedRef(),
						},
						{
							TEXT("enable_occlusion"),
							Occlusion.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_low_pass_filter_frequency"),
							OcclusionLowPass.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_volume_attenuation"),
							OcclusionVolume.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_interpolation_time"),
							OcclusionTime.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildSoundClassPropertiesProjection(
			const FSoundClassProperties& Properties,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Volume;
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr UiSound;
			FUeAgentCanonicalValuePtr Music;
			return TryBuildDecimalProjectionValue(
					Properties.Volume,
					Resources,
					Volume,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Properties.Pitch,
					Resources,
					Pitch,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Properties.bIsUISound,
					Resources,
					UiSound,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Properties.bIsMusic,
					Resources,
					Music,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("volume"), Volume.ToSharedRef()},
						{TEXT("pitch"), Pitch.ToSharedRef()},
						{
							TEXT("is_ui_sound"),
							UiSound.ToSharedRef(),
						},
						{TEXT("is_music"), Music.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryReadConcurrencyVolumeScale(
			const FSoundConcurrencySettings& Settings,
			float& OutValue)
		{
			const FFloatProperty* Property =
				FindFProperty<FFloatProperty>(
					FSoundConcurrencySettings::StaticStruct(),
					TEXT("VolumeScale"));
			if (!Property)
			{
				return false;
			}
			OutValue =
				Property->GetPropertyValue_InContainer(&Settings);
			return FMath::IsFinite(OutValue);
		}

		bool TryBuildConcurrencyProjection(
			const FSoundConcurrencySettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FString ResolutionName;
			FString ScaleModeName;
			float VolumeScale = 0.0f;
			if (!TryFormatConcurrencyResolutionRule(
					Settings.ResolutionRule,
					ResolutionName)
				|| !TryFormatConcurrencyVolumeScaleMode(
					Settings.VolumeScaleMode,
					ScaleModeName)
				|| !TryReadConcurrencyVolumeScale(
					Settings,
					VolumeScale))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_sound_concurrency_projection_unsupported"));
			}
			FUeAgentCanonicalValuePtr MaxCount;
			FUeAgentCanonicalValuePtr LimitToOwner;
			FUeAgentCanonicalValuePtr RetriggerTime;
			FUeAgentCanonicalValuePtr Resolution;
			FUeAgentCanonicalValuePtr Scale;
			FUeAgentCanonicalValuePtr ScaleMode;
			FUeAgentCanonicalValuePtr Attack;
			FUeAgentCanonicalValuePtr Release;
			FUeAgentCanonicalValuePtr VoiceSteal;
			return TryBuildIntegerProjectionValue(
					Settings.MaxCount,
					Resources,
					MaxCount,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Settings.bLimitToOwner,
					Resources,
					LimitToOwner,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.RetriggerTime,
					Resources,
					RetriggerTime,
					OutError)
				&& TryBuildStringProjectionValue(
					ResolutionName,
					Resources,
					Resolution,
					OutError)
				&& TryBuildDecimalProjectionValue(
					VolumeScale,
					Resources,
					Scale,
					OutError)
				&& TryBuildStringProjectionValue(
					ScaleModeName,
					Resources,
					ScaleMode,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.VolumeScaleAttackTime,
					Resources,
					Attack,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.VolumeScaleReleaseTime,
					Resources,
					Release,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Settings.VoiceStealReleaseTime,
					Resources,
					VoiceSteal,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("max_count"), MaxCount.ToSharedRef()},
						{
							TEXT("limit_to_owner"),
							LimitToOwner.ToSharedRef(),
						},
						{
							TEXT("retrigger_time"),
							RetriggerTime.ToSharedRef(),
						},
						{
							TEXT("resolution_rule"),
							Resolution.ToSharedRef(),
						},
						{
							TEXT("volume_scale"),
							Scale.ToSharedRef(),
						},
						{
							TEXT("volume_scale_mode"),
							ScaleMode.ToSharedRef(),
						},
						{
							TEXT("volume_scale_attack_time"),
							Attack.ToSharedRef(),
						},
						{
							TEXT("volume_scale_release_time"),
							Release.ToSharedRef(),
						},
						{
							TEXT("voice_steal_release_time"),
							VoiceSteal.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildSoundMixModifierProjection(
			const FSoundClassAdjuster& Modifier,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr SoundClass;
			FUeAgentCanonicalValuePtr Volume;
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr LowPass;
			FUeAgentCanonicalValuePtr ApplyToChildren;
			FUeAgentCanonicalValuePtr CenterVolume;
			return TryBuildObjectReferenceProjection(
					Modifier.SoundClassObject,
					false,
					Resources,
					SoundClass,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Modifier.VolumeAdjuster,
					Resources,
					Volume,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Modifier.PitchAdjuster,
					Resources,
					Pitch,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Modifier.LowPassFilterFrequency,
					Resources,
					LowPass,
					OutError)
				&& TryBuildBooleanProjectionValue(
					Modifier.bApplyToChildren,
					Resources,
					ApplyToChildren,
					OutError)
				&& TryBuildDecimalProjectionValue(
					Modifier.VoiceCenterChannelVolumeAdjuster,
					Resources,
					CenterVolume,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("sound_class_ref"),
							SoundClass.ToSharedRef(),
						},
						{
							TEXT("volume_adjuster"),
							Volume.ToSharedRef(),
						},
						{
							TEXT("pitch_adjuster"),
							Pitch.ToSharedRef(),
						},
						{
							TEXT("low_pass_filter_frequency"),
							LowPass.ToSharedRef(),
						},
						{
							TEXT("apply_to_children"),
							ApplyToChildren.ToSharedRef(),
						},
						{
							TEXT(
								"voice_center_channel_volume_adjuster"),
							CenterVolume.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildSoundMixModifiersProjection(
			const TArray<FSoundClassAdjuster>& Modifiers,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			TSet<const USoundClass*> Seen;
			for (const FSoundClassAdjuster& Modifier : Modifiers)
			{
				if (!Modifier.SoundClassObject
					|| Seen.Contains(Modifier.SoundClassObject))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_mix_projection_identity_invalid"));
				}
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildSoundMixModifierProjection(
						Modifier,
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Seen.Add(Modifier.SoundClassObject);
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildCurveChannelsProjection(
			const TArray<TArray<FRichCurveKey>>& ChannelValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const int32 Dimension = ChannelValues.Num();
			if (Dimension < 1 || Dimension > 4)
			{
				return Fail(
					OutError,
					TEXT("asset_data_curve_projection_target_invalid"));
			}
			TArray<const TArray<FRichCurveKey>*> Channels;
			for (const TArray<FRichCurveKey>& Channel
				: ChannelValues)
			{
				Channels.Add(&Channel);
			}
			const int32 KeyCount = Channels[0]->Num();
			for (int32 Index = 1; Index < Channels.Num(); ++Index)
			{
				if (Channels[Index]->Num() != KeyCount)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_curve_projection_channel_topology_unsupported"));
				}
			}
			TArray<FUeAgentCanonicalValueRef> ProjectedKeys;
			for (int32 KeyIndex = 0;
				KeyIndex < KeyCount;
				++KeyIndex)
			{
				const FRichCurveKey& First =
					(*Channels[0])[KeyIndex];
				FString InterpName;
				FString TangentName;
				FString WeightName;
				TArray<float> Values;
				TArray<float> ArriveTangents;
				TArray<float> LeaveTangents;
				TArray<float> ArriveWeights;
				TArray<float> LeaveWeights;
				for (int32 ChannelIndex = 0;
					ChannelIndex < Channels.Num();
					++ChannelIndex)
				{
					const FRichCurveKey& Key =
						(*Channels[ChannelIndex])[KeyIndex];
					if (!FMath::IsNearlyEqual(Key.Time, First.Time)
						|| Key.InterpMode != First.InterpMode
						|| Key.TangentMode != First.TangentMode
						|| Key.TangentWeightMode
							!= First.TangentWeightMode)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_curve_projection_channel_semantics_unsupported"));
					}
					Values.Add(Key.Value);
					ArriveTangents.Add(Key.ArriveTangent);
					LeaveTangents.Add(Key.LeaveTangent);
					ArriveWeights.Add(
						Key.ArriveTangentWeight);
					LeaveWeights.Add(
						Key.LeaveTangentWeight);
				}
				FUeAgentCanonicalValuePtr Time;
				FUeAgentCanonicalValuePtr ValueArray;
				FUeAgentCanonicalValuePtr Interp;
				FUeAgentCanonicalValuePtr Tangent;
				FUeAgentCanonicalValuePtr Weight;
				FUeAgentCanonicalValuePtr ArriveTangentArray;
				FUeAgentCanonicalValuePtr LeaveTangentArray;
				FUeAgentCanonicalValuePtr ArriveWeightArray;
				FUeAgentCanonicalValuePtr LeaveWeightArray;
				FUeAgentCanonicalValuePtr ProjectedKey;
				if (!TryFormatInterpMode(
						First.InterpMode,
						InterpName)
					|| !TryFormatTangentMode(
						First.TangentMode,
						TangentName)
					|| !TryFormatTangentWeightMode(
						First.TangentWeightMode,
						WeightName)
					|| !TryBuildDecimalProjectionValue(
						First.Time,
						Resources,
						Time,
						OutError)
					|| !TryBuildFloatArrayProjection(
						Values,
						Resources,
						ValueArray,
						OutError)
					|| !TryBuildStringProjectionValue(
						InterpName,
						Resources,
						Interp,
						OutError)
					|| !TryBuildStringProjectionValue(
						TangentName,
						Resources,
						Tangent,
						OutError)
					|| !TryBuildStringProjectionValue(
						WeightName,
						Resources,
						Weight,
						OutError)
					|| !TryBuildFloatArrayProjection(
						ArriveTangents,
						Resources,
						ArriveTangentArray,
						OutError)
					|| !TryBuildFloatArrayProjection(
						LeaveTangents,
						Resources,
						LeaveTangentArray,
						OutError)
					|| !TryBuildFloatArrayProjection(
						ArriveWeights,
						Resources,
						ArriveWeightArray,
						OutError)
					|| !TryBuildFloatArrayProjection(
						LeaveWeights,
						Resources,
						LeaveWeightArray,
						OutError)
					|| !TryBuildObjectProjectionValue(
						{
							{TEXT("time"), Time.ToSharedRef()},
							{
								TEXT("values"),
								ValueArray.ToSharedRef(),
							},
							{
								TEXT("interp_mode"),
								Interp.ToSharedRef(),
							},
							{
								TEXT("tangent_mode"),
								Tangent.ToSharedRef(),
							},
							{
								TEXT("tangent_weight_mode"),
								Weight.ToSharedRef(),
							},
							{
								TEXT("arrive_tangents"),
								ArriveTangentArray.ToSharedRef(),
							},
							{
								TEXT("leave_tangents"),
								LeaveTangentArray.ToSharedRef(),
							},
							{
								TEXT(
									"arrive_tangent_weights"),
								ArriveWeightArray.ToSharedRef(),
							},
							{
								TEXT(
									"leave_tangent_weights"),
								LeaveWeightArray.ToSharedRef(),
							},
						},
						Resources,
						ProjectedKey,
						OutError))
				{
					return false;
				}
				ProjectedKeys.Add(ProjectedKey.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				ProjectedKeys,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildCurveKeysProjection(
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const int32 Dimension = GetCurveDimension(Target);
			if (Dimension < 1 || Dimension > 4)
			{
				return Fail(
					OutError,
					TEXT("asset_data_curve_projection_target_invalid"));
			}
			TArray<TArray<FRichCurveKey>> Channels;
			Channels.SetNum(Dimension);
			for (int32 Index = 0; Index < Dimension; ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
				if (!Curve)
				{
					return false;
				}
				Channels[Index] = Curve->GetCopyOfKeys();
			}
			return TryBuildCurveChannelsProjection(
				Channels,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildMediaSourceOptionsProjection(
			const bool bPrecacheFile,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Precache;
			return TryBuildBooleanProjectionValue(
					bPrecacheFile,
					Resources,
					Precache,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("precache_file"),
							Precache.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildSparseAddressingProjection(
			const TEnumAsByte<TextureAddress> X,
			const TEnumAsByte<TextureAddress> Y,
			const TEnumAsByte<TextureAddress> Z,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FString XName;
			FString YName;
			FString ZName;
			FUeAgentCanonicalValuePtr XValue;
			FUeAgentCanonicalValuePtr YValue;
			FUeAgentCanonicalValuePtr ZValue;
			return TryFormatTextureAddress(X, XName)
				&& TryFormatTextureAddress(Y, YName)
				&& TryFormatTextureAddress(Z, ZName)
				&& TryBuildStringProjectionValue(
					XName,
					Resources,
					XValue,
					OutError)
				&& TryBuildStringProjectionValue(
					YName,
					Resources,
					YValue,
					OutError)
				&& TryBuildStringProjectionValue(
					ZName,
					Resources,
					ZValue,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("x"), XValue.ToSharedRef()},
						{TEXT("y"), YValue.ToSharedRef()},
						{TEXT("z"), ZValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildSparseStreamingProjection(
			const bool bLocalDdcOnly,
			const float PoolSizeFactor,
			const int32 PrefetchFrames,
			const float PrefetchPercentageStep,
			const float PrefetchPercentageBias,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr LocalDdcOnly;
			FUeAgentCanonicalValuePtr PoolSize;
			FUeAgentCanonicalValuePtr Frames;
			FUeAgentCanonicalValuePtr PercentageStep;
			FUeAgentCanonicalValuePtr PercentageBias;
			return TryBuildBooleanProjectionValue(
					bLocalDdcOnly,
					Resources,
					LocalDdcOnly,
					OutError)
				&& TryBuildDecimalProjectionValue(
					PoolSizeFactor,
					Resources,
					PoolSize,
					OutError)
				&& TryBuildIntegerProjectionValue(
					PrefetchFrames,
					Resources,
					Frames,
					OutError)
				&& TryBuildDecimalProjectionValue(
					PrefetchPercentageStep,
					Resources,
					PercentageStep,
					OutError)
				&& TryBuildDecimalProjectionValue(
					PrefetchPercentageBias,
					Resources,
					PercentageBias,
					OutError)
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("local_ddc_only"),
							LocalDdcOnly.ToSharedRef(),
						},
						{
							TEXT("pool_size_factor"),
							PoolSize.ToSharedRef(),
						},
						{
							TEXT("prefetch_frames"),
							Frames.ToSharedRef(),
						},
						{
							TEXT("prefetch_percentage_step"),
							PercentageStep.ToSharedRef(),
						},
						{
							TEXT("prefetch_percentage_bias"),
							PercentageBias.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError);
		}

		const TCHAR* FindCoreAudioCurveContextTag(
			const ECoreAssetKind Kind,
			const FString& FieldId)
		{
			switch (Kind)
			{
			case ECoreAssetKind::AudioEffectPreset:
				return FieldId == TEXT("settings")
					? TEXT("eq_settings")
					: nullptr;
			case ECoreAssetKind::SoundAttenuation:
				return FieldId == TEXT("attenuation")
					? TEXT("attenuation_settings")
					: nullptr;
			case ECoreAssetKind::SoundClass:
				if (FieldId == TEXT("properties"))
				{
					return TEXT("sound_class_properties");
				}
				if (FieldId == TEXT("child_classes"))
				{
					return TEXT("ordered_asset_refs");
				}
				return nullptr;
			case ECoreAssetKind::SoundConcurrency:
				return FieldId == TEXT("concurrency")
					? TEXT("concurrency_settings")
					: nullptr;
			case ECoreAssetKind::SoundMix:
				if (FieldId == TEXT("modifiers"))
				{
					return TEXT("sound_mix_modifiers");
				}
				if (FieldId == TEXT("initial_delay"))
				{
					return TEXT("decimal");
				}
				return nullptr;
			case ECoreAssetKind::SoundSubmix:
				if (FieldId == TEXT("parent"))
				{
					return TEXT("asset_ref_or_null");
				}
				if (FieldId == TEXT("effect_chain"))
				{
					return TEXT("ordered_asset_refs");
				}
				return nullptr;
			case ECoreAssetKind::Curve:
				return FieldId == TEXT("keys")
					? TEXT("curve_rich_keys")
					: nullptr;
			default:
				return nullptr;
			}
		}

		bool TryBuildCoreAudioCurveValueMap(
			const ECoreAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			FUeAgentCanonicalValuePtr First;
			FUeAgentCanonicalValuePtr Second;
			switch (Kind)
			{
			case ECoreAssetKind::AudioEffectPreset:
				{
					const USubmixEffectSubmixEQPreset* Preset =
						Cast<USubmixEffectSubmixEQPreset>(Target);
					if (!Preset
						|| !TryBuildEqSettingsProjection(
							Preset->Settings,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("settings"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundAttenuation:
				{
					const USoundAttenuation* Attenuation =
						Cast<USoundAttenuation>(Target);
					if (!Attenuation
						|| !TryBuildAttenuationProjection(
							Attenuation->Attenuation,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("attenuation"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundClass:
				{
					const USoundClass* SoundClass =
						Cast<USoundClass>(Target);
					TArray<UObject*> Children;
					if (!SoundClass)
					{
						return false;
					}
					for (USoundClass* Child
						: SoundClass->ChildClasses)
					{
						Children.Add(Child);
					}
					if (!TryBuildSoundClassPropertiesProjection(
							SoundClass->Properties,
							Resources,
							First,
							OutError)
						|| !TryBuildObjectReferenceArrayProjection(
							Children,
							Resources,
							Second,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("properties"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("child_classes"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundConcurrency:
				{
					const USoundConcurrency* Concurrency =
						Cast<USoundConcurrency>(Target);
					if (!Concurrency
						|| !TryBuildConcurrencyProjection(
							Concurrency->Concurrency,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("concurrency"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundMix:
				{
					const USoundMix* SoundMix =
						Cast<USoundMix>(Target);
					if (!SoundMix
						|| !TryBuildSoundMixModifiersProjection(
							SoundMix->SoundClassEffects,
							Resources,
							First,
							OutError)
						|| !TryBuildDecimalProjectionValue(
							SoundMix->InitialDelay,
							Resources,
							Second,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("modifiers"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("initial_delay"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundSubmix:
				{
					const USoundSubmix* Submix =
						Cast<USoundSubmix>(Target);
					TArray<UObject*> Effects;
					if (!Submix)
					{
						return false;
					}
					for (USoundEffectSubmixPreset* Effect
						: Submix->SubmixEffectChain)
					{
						Effects.Add(Effect);
					}
					if (!TryBuildObjectReferenceProjection(
							Submix->ParentSubmix,
							true,
							Resources,
							First,
							OutError)
						|| !TryBuildObjectReferenceArrayProjection(
							Effects,
							Resources,
							Second,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("parent"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("effect_chain"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::Curve:
				if (!TryBuildCurveKeysProjection(
						Target,
						Resources,
						First,
						OutError))
				{
					return false;
				}
				OutValues.Add(TEXT("keys"), First.ToSharedRef());
				return true;
			default:
				return Fail(
					OutError,
					TEXT(
						"asset_data_core_audio_curve_projection_unsupported"));
			}
		}

		bool TryBuildCoreAudioCurveProjection(
			const ECoreAssetKind Kind,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			UObject* Default =
				Target
					? Target->GetClass()->GetDefaultObject()
					: nullptr;
			TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
			TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
			if (!Target
				|| !Default
				|| SelectedFieldIds.IsEmpty()
				|| !TryBuildCoreAudioCurveValueMap(
					Kind,
					Target,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildCoreAudioCurveValueMap(
					Kind,
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					DefaultValues.Find(FieldId);
				const TCHAR* ContextTag =
					FindCoreAudioCurveContextTag(Kind, FieldId);
				const FString ContextText =
					Kind == ECoreAssetKind::Curve
							&& FieldId == TEXT("keys")
						? FString::Printf(
							TEXT("%s:%d"),
							ContextTag
								? ContextTag
								: TEXT(""),
							GetCurveDimension(Target))
						: FString(ContextTag ? ContextTag : TEXT(""));
				FUeAgentCanonicalValuePtr ContextValue;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !DefaultValue
					|| !ContextTag
					|| ContextText.IsEmpty()
					|| !TryBuildStringProjectionValue(
						ContextText,
						Resources,
						ContextValue,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_selected_field_invalid"),
						FieldId);
				}
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(
					FieldId,
					ContextValue.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError)
				&& TryBuildObjectProjectionValue(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError)
				&& TryBuildObjectProjectionValue(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError);
		}

		const TCHAR* FindCoreConfigurationContextTag(
			const ECoreAssetKind Kind,
			const FString& FieldId)
		{
			switch (Kind)
			{
			case ECoreAssetKind::MediaSource:
				if (FieldId == TEXT("source_uri"))
				{
					return TEXT("file_path");
				}
				if (FieldId == TEXT("options"))
				{
					return TEXT("media_source_options");
				}
				break;
			case ECoreAssetKind::RuntimeVirtualTexture:
				if (FieldId == TEXT("size"))
				{
					return TEXT("rvt_size");
				}
				if (FieldId == TEXT("material_type"))
				{
					return TEXT("rvt_material_type");
				}
				if (FieldId == TEXT("compression"))
				{
					return TEXT("boolean");
				}
				break;
			case ECoreAssetKind::SparseVolumeTexture:
				if (FieldId == TEXT("addressing"))
				{
					return TEXT("texture_addressing");
				}
				if (FieldId == TEXT("streaming"))
				{
					return TEXT("sparse_streaming");
				}
				break;
			default:
				break;
			}
			return nullptr;
		}

		bool TryBuildCoreConfigurationValueMap(
			const ECoreAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (Kind == ECoreAssetKind::MediaSource)
			{
				const UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				FUeAgentCanonicalValuePtr SourceUri;
				FUeAgentCanonicalValuePtr Options;
				if (!MediaSource
					|| !TryBuildStringProjectionValue(
						MediaSource->GetFilePath(),
						Resources,
						SourceUri,
						OutError)
					|| !TryBuildMediaSourceOptionsProjection(
						MediaSource->PrecacheFile,
						Resources,
						Options,
						OutError))
				{
					return Fail(
						OutError,
						OutError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_media_source_projection_invalid")
							: OutError.GetCode());
				}
				OutValues.Add(
					TEXT("source_uri"),
					SourceUri.ToSharedRef());
				OutValues.Add(
					TEXT("options"),
					Options.ToSharedRef());
				return true;
			}
			if (Kind == ECoreAssetKind::RuntimeVirtualTexture)
			{
				URuntimeVirtualTexture* VirtualTexture =
					Cast<URuntimeVirtualTexture>(Target);
				int32 TileCount = 0;
				ERuntimeVirtualTextureMaterialType MaterialType =
					ERuntimeVirtualTextureMaterialType::BaseColor;
				bool bCompression = false;
				FString MaterialTypeName;
				FUeAgentCanonicalValuePtr Size;
				FUeAgentCanonicalValuePtr Material;
				FUeAgentCanonicalValuePtr Compression;
				if (!VirtualTexture
					|| !TryReadRvtProperties(
						VirtualTexture,
						TileCount,
						MaterialType,
						bCompression)
					|| !TryFormatRuntimeVirtualTextureMaterialType(
						MaterialType,
						MaterialTypeName)
					|| !TryBuildIntegerProjectionValue(
						VirtualTexture->GetSize(),
						Resources,
						Size,
						OutError)
					|| !TryBuildStringProjectionValue(
						MaterialTypeName,
						Resources,
						Material,
						OutError)
					|| !TryBuildBooleanProjectionValue(
						bCompression,
						Resources,
						Compression,
						OutError))
				{
					return Fail(
						OutError,
						OutError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_rvt_projection_invalid")
							: OutError.GetCode());
				}
				OutValues.Add(TEXT("size"), Size.ToSharedRef());
				OutValues.Add(
					TEXT("material_type"),
					Material.ToSharedRef());
				OutValues.Add(
					TEXT("compression"),
					Compression.ToSharedRef());
				return true;
			}
			if (Kind == ECoreAssetKind::SparseVolumeTexture)
			{
				const UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				FUeAgentCanonicalValuePtr Addressing;
				FUeAgentCanonicalValuePtr Streaming;
				if (!Sparse
					|| !TryBuildSparseAddressingProjection(
						Sparse->AddressX,
						Sparse->AddressY,
						Sparse->AddressZ,
						Resources,
						Addressing,
						OutError)
					|| !TryBuildSparseStreamingProjection(
						Sparse->bLocalDDCOnly,
						Sparse->StreamingPoolSizeFactor,
						Sparse->NumberOfPrefetchFrames,
						Sparse->PrefetchPercentageStepSize,
						Sparse->PrefetchPercentageBias,
						Resources,
						Streaming,
						OutError))
				{
					return Fail(
						OutError,
						OutError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_sparse_projection_invalid")
							: OutError.GetCode());
				}
				OutValues.Add(
					TEXT("addressing"),
					Addressing.ToSharedRef());
				OutValues.Add(
					TEXT("streaming"),
					Streaming.ToSharedRef());
				return true;
			}
			return Fail(
				OutError,
				TEXT(
					"asset_data_core_configuration_projection_unsupported"));
		}

		bool TryBuildCoreConfigurationProjection(
			const ECoreAssetKind Kind,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			UObject* Default = nullptr;
			switch (Kind)
			{
			case ECoreAssetKind::MediaSource:
				Default = GetMutableDefault<UFileMediaSource>();
				break;
			case ECoreAssetKind::RuntimeVirtualTexture:
				Default = GetMutableDefault<URuntimeVirtualTexture>();
				break;
			case ECoreAssetKind::SparseVolumeTexture:
				Default =
					GetMutableDefault<
						UStreamableSparseVolumeTexture>();
				break;
			default:
				break;
			}
			TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
			TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
			if (!Target
				|| !Default
				|| SelectedFieldIds.IsEmpty()
				|| !TryBuildCoreConfigurationValueMap(
					Kind,
					Target,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildCoreConfigurationValueMap(
					Kind,
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					DefaultValues.Find(FieldId);
				const TCHAR* ContextTag =
					FindCoreConfigurationContextTag(
						Kind,
						FieldId);
				FUeAgentCanonicalValuePtr ContextValue;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !DefaultValue
					|| !ContextTag
					|| !TryBuildStringProjectionValue(
						ContextTag,
						Resources,
						ContextValue,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_selected_field_invalid"),
						FieldId);
				}
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(
					FieldId,
					ContextValue.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError)
				&& TryBuildObjectProjectionValue(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError)
				&& TryBuildObjectProjectionValue(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError);
		}

		bool TryNormalizeCoreConfigurationValue(
			const ECoreAssetKind Kind,
			const FString& FieldId,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (Kind == ECoreAssetKind::MediaSource)
			{
				if (FieldId == TEXT("source_uri"))
				{
					FString FilePath;
					return TryNormalizeFileUri(
							Requested,
							FilePath)
						&& TryBuildStringProjectionValue(
							FilePath,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("options"))
				{
					const FUeAgentCanonicalValue* Precache =
						FindObjectField(
							Requested,
							TEXT("precache_file"));
					bool bPrecache = false;
					return HasExactObjectFields(
							Requested,
							{TEXT("precache_file")})
						&& Precache
						&& TryReadBool(*Precache, bPrecache)
						&& TryBuildMediaSourceOptionsProjection(
							bPrecache,
							Resources,
							OutValue,
							OutError);
				}
			}
			else if (Kind
				== ECoreAssetKind::RuntimeVirtualTexture)
			{
				if (FieldId == TEXT("size"))
				{
					int32 Size = 0;
					return TryReadInt32(
							Requested,
							Size,
							1,
							MAX_int32)
						&& TryBuildIntegerProjectionValue(
							Size,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("material_type"))
				{
					FString RequestedName;
					FString CanonicalName;
					ERuntimeVirtualTextureMaterialType MaterialType =
						ERuntimeVirtualTextureMaterialType::
							BaseColor;
					return TryReadString(
							Requested,
							RequestedName)
						&& TryParseRuntimeVirtualTextureMaterialType(
							RequestedName,
							MaterialType)
						&& TryFormatRuntimeVirtualTextureMaterialType(
							MaterialType,
							CanonicalName)
						&& TryBuildStringProjectionValue(
							CanonicalName,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("compression"))
				{
					bool bCompression = false;
					return TryReadBool(
							Requested,
							bCompression)
						&& TryBuildBooleanProjectionValue(
							bCompression,
							Resources,
							OutValue,
							OutError);
				}
			}
			else if (Kind == ECoreAssetKind::SparseVolumeTexture)
			{
				if (FieldId == TEXT("addressing"))
				{
					const FUeAgentCanonicalValue* XValue =
						FindObjectField(Requested, TEXT("x"));
					const FUeAgentCanonicalValue* YValue =
						FindObjectField(Requested, TEXT("y"));
					const FUeAgentCanonicalValue* ZValue =
						FindObjectField(Requested, TEXT("z"));
					FString XName;
					FString YName;
					FString ZName;
					TEnumAsByte<TextureAddress> X;
					TEnumAsByte<TextureAddress> Y;
					TEnumAsByte<TextureAddress> Z;
					return HasExactObjectFields(
							Requested,
							{
								TEXT("x"),
								TEXT("y"),
								TEXT("z"),
							})
						&& XValue
						&& YValue
						&& ZValue
						&& TryReadString(*XValue, XName)
						&& TryReadString(*YValue, YName)
						&& TryReadString(*ZValue, ZName)
						&& TryParseTextureAddress(XName, X)
						&& TryParseTextureAddress(YName, Y)
						&& TryParseTextureAddress(ZName, Z)
						&& TryBuildSparseAddressingProjection(
							X,
							Y,
							Z,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("streaming"))
				{
					const FUeAgentCanonicalValue* LocalDdc =
						FindObjectField(
							Requested,
							TEXT("local_ddc_only"));
					const FUeAgentCanonicalValue* PoolSize =
						FindObjectField(
							Requested,
							TEXT("pool_size_factor"));
					const FUeAgentCanonicalValue* Frames =
						FindObjectField(
							Requested,
							TEXT("prefetch_frames"));
					const FUeAgentCanonicalValue* Step =
						FindObjectField(
							Requested,
							TEXT("prefetch_percentage_step"));
					const FUeAgentCanonicalValue* Bias =
						FindObjectField(
							Requested,
							TEXT("prefetch_percentage_bias"));
					bool bLocalDdc = false;
					float PoolSizeFactor = 0.0f;
					int32 PrefetchFrames = 0;
					float PercentageStep = 0.0f;
					float PercentageBias = 0.0f;
					return HasExactObjectFields(
							Requested,
							{
								TEXT("local_ddc_only"),
								TEXT("pool_size_factor"),
								TEXT("prefetch_frames"),
								TEXT(
									"prefetch_percentage_step"),
								TEXT(
									"prefetch_percentage_bias"),
							})
						&& LocalDdc
						&& PoolSize
						&& Frames
						&& Step
						&& Bias
						&& TryReadBool(*LocalDdc, bLocalDdc)
						&& TryReadFloat(
							*PoolSize,
							PoolSizeFactor,
							1.0,
							1000.0)
						&& TryReadInt32(
							*Frames,
							PrefetchFrames,
							0,
							1024)
						&& TryReadFloat(
							*Step,
							PercentageStep,
							0.0,
							100.0)
						&& TryReadFloat(
							*Bias,
							PercentageBias,
							0.0,
							100.0)
						&& TryBuildSparseStreamingProjection(
							bLocalDdc,
							PoolSizeFactor,
							PrefetchFrames,
							PercentageStep,
							PercentageBias,
							Resources,
							OutValue,
							OutError);
				}
			}
			return Fail(
				OutError,
				TEXT(
					"asset_data_core_expected_field_invalid"),
				FieldId);
		}

		bool TryBuildExpectedCoreConfigurationFields(
			const ECoreAssetKind Kind,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpectedFields,
			FExecutionMutationError& OutError)
		{
			OutExpectedFields.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
			if (CurrentFields.GetKind()
					!= EUeAgentValueKind::Object
				|| DefaultFields.GetKind()
					!= EUeAgentValueKind::Object
				|| ProjectionContext.GetKind()
					!= EUeAgentValueKind::Object
				|| !TypedChanges
				|| TypedChanges->GetKind()
					!= EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_core_expected_state_invalid"));
			}
			TMap<FString, const FUeAgentCanonicalValue*> Changes;
			for (const FUeAgentCanonicalValueRef& ChangeRef
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue& Change =
					ChangeRef.Get();
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change, TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadString(*Field, FieldId)
					|| Changes.Contains(FieldId))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_change_invalid"));
				}
				Changes.Add(FieldId, &Change);
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ExpectedItems;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FUeAgentCanonicalValue* const* ChangePtr =
					Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* DefaultValueRef =
					FindObjectFieldRef(
						DefaultFields,
						Current.Key);
				const FUeAgentCanonicalValue* DefaultValue =
					DefaultValueRef
						? &DefaultValueRef->Get()
						: nullptr;
				const FUeAgentCanonicalValue* ContextValue =
					FindObjectField(
						ProjectionContext,
						Current.Key);
				const TCHAR* ExpectedContextTag =
					FindCoreConfigurationContextTag(
						Kind,
						Current.Key);
				FString ContextTag;
				if (!ChangePtr
					|| !DefaultValue
					|| !ContextValue
					|| !ExpectedContextTag
					|| !TryReadString(
						*ContextValue,
						ContextTag)
					|| ContextTag != ExpectedContextTag)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_context_invalid"),
						Current.Key);
				}
				const FUeAgentCanonicalValue& Change =
					**ChangePtr;
				const FUeAgentCanonicalValue* PresenceValue =
					FindObjectField(
						Change,
						TEXT("presence"));
				FString Presence;
				if (!PresenceValue
					|| !TryReadString(
						*PresenceValue,
						Presence))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_presence_invalid"),
						Current.Key);
				}
				if (Presence == TEXT("clear"))
				{
					ExpectedItems.Emplace(
						Current.Key,
						*DefaultValueRef);
					continue;
				}
				const FUeAgentCanonicalValue* TypedValue =
					FindObjectField(Change, TEXT("value"));
				const FUeAgentCanonicalValue* Requested =
					TypedValue
						? FindObjectField(
							*TypedValue,
							TEXT("canonical_value"))
						: nullptr;
				FUeAgentCanonicalValuePtr Normalized;
				if ((Presence != TEXT("set")
						&& Presence != TEXT("replace"))
					|| !Requested
					|| !TryNormalizeCoreConfigurationValue(
						Kind,
						Current.Key,
						*Requested,
						Resources,
						Normalized,
						OutError))
				{
					return false;
				}
				ExpectedItems.Emplace(
					Current.Key,
					Normalized.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
				MoveTemp(ExpectedItems),
				Resources,
				OutExpectedFields,
				OutError);
		}

		bool TryReadCollectionIdentity(
			const FUeAgentCanonicalValue& Value,
			const FString& IdentityField,
			FString& OutIdentity)
		{
			if (IdentityField.IsEmpty())
			{
				return TryReadString(Value, OutIdentity);
			}
			const FUeAgentCanonicalValue* Identity =
				FindObjectField(Value, IdentityField);
			return Identity
				&& TryReadString(*Identity, OutIdentity);
		}

		bool TryApplyProjectedCollectionPresence(
			const FString& Presence,
			const FUeAgentCanonicalValue& Current,
			const FUeAgentCanonicalValue* Requested,
			const FString& IdentityField,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (Current.GetKind() != EUeAgentValueKind::Array
				|| (Presence != TEXT("clear")
					&& (!Requested
						|| Requested->GetKind()
							!= EUeAgentValueKind::Array)))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_core_expected_collection_invalid"));
			}
			TArray<FUeAgentCanonicalValueRef> Items =
				Current.GetArrayItems();
			if (Presence == TEXT("clear"))
			{
				Items.Reset();
			}
			else if (Presence == TEXT("replace"))
			{
				Items = Requested->GetArrayItems();
			}
			else if (Presence == TEXT("remove"))
			{
				TSet<FString> Removed;
				for (const FUeAgentCanonicalValueRef& Item
					: Requested->GetArrayItems())
				{
					FString Identity;
					if (!TryReadCollectionIdentity(
							Item.Get(),
							IdentityField,
							Identity)
						|| Removed.Contains(Identity))
					{
						return false;
					}
					Removed.Add(Identity);
				}
				Items.RemoveAll(
					[&Removed, &IdentityField](
						const FUeAgentCanonicalValueRef& Item)
					{
						FString Identity;
						return TryReadCollectionIdentity(
								Item.Get(),
								IdentityField,
								Identity)
							&& Removed.Contains(Identity);
					});
			}
			else if (Presence == TEXT("set"))
			{
				for (const FUeAgentCanonicalValueRef& RequestedItem
					: Requested->GetArrayItems())
				{
					FString RequestedIdentity;
					if (!TryReadCollectionIdentity(
							RequestedItem.Get(),
							IdentityField,
							RequestedIdentity))
					{
						return false;
					}
					const int32 Index =
						Items.IndexOfByPredicate(
							[
								&RequestedIdentity,
								&IdentityField](
								const
									FUeAgentCanonicalValueRef&
										Existing)
							{
								FString ExistingIdentity;
								return TryReadCollectionIdentity(
										Existing.Get(),
										IdentityField,
										ExistingIdentity)
									&& ExistingIdentity
										== RequestedIdentity;
							});
					if (Index == INDEX_NONE)
					{
						Items.Add(RequestedItem);
					}
					else
					{
						Items[Index] = RequestedItem;
					}
				}
			}
			else
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_core_expected_collection_presence_invalid"),
					Presence);
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryNormalizeCoreAudioCurveValue(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const ECoreAssetKind Kind,
			const FString& FieldId,
			const FUeAgentCanonicalValue& Requested,
			const int32 CurveDimension,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (Kind == ECoreAssetKind::AudioEffectPreset
				&& FieldId == TEXT("settings"))
			{
				FSubmixEffectSubmixEQSettings Settings;
				return TryParseEqSettings(Requested, Settings)
					&& TryBuildEqSettingsProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundAttenuation
				&& FieldId == TEXT("attenuation"))
			{
				FSoundAttenuationSettings Settings;
				return TryParseAttenuationSettings(
						Requested,
						Settings)
					&& TryBuildAttenuationProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundClass)
			{
				if (FieldId == TEXT("properties"))
				{
					FSoundClassProperties Properties =
						GetDefault<USoundClass>()->Properties;
					return TryParseSoundClassProperties(
							Requested,
							Properties)
						&& TryBuildSoundClassPropertiesProjection(
							Properties,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("child_classes"))
				{
					TArray<USoundClass*> Resolved;
					TArray<UObject*> Objects;
					if (!TryResolveUniqueArray(
							Resolver,
							Requested,
							Resolved,
							OutError))
					{
						return false;
					}
					for (USoundClass* Object : Resolved)
					{
						Objects.Add(Object);
					}
					return TryBuildObjectReferenceArrayProjection(
						Objects,
						Resources,
						OutValue,
						OutError);
				}
			}
			if (Kind == ECoreAssetKind::SoundConcurrency
				&& FieldId == TEXT("concurrency"))
			{
				FSoundConcurrencySettings Settings;
				return TryParseConcurrencySettings(
						Requested,
						Settings)
					&& TryBuildConcurrencyProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundMix)
			{
				if (FieldId == TEXT("modifiers"))
				{
					TArray<FSoundClassAdjuster> Modifiers;
					TArray<TStrongObjectPtr<UObject>> Pins;
					return TryParseSoundMixModifiers(
							Resolver,
							Requested,
							Modifiers,
							Pins,
							OutError)
						&& TryBuildSoundMixModifiersProjection(
							Modifiers,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("initial_delay"))
				{
					float Delay = 0.0f;
					return TryReadFloat(
							Requested,
							Delay,
							0.0,
							1000000.0)
						&& TryBuildDecimalProjectionValue(
							Delay,
							Resources,
							OutValue,
							OutError);
				}
			}
			if (Kind == ECoreAssetKind::SoundSubmix)
			{
				if (FieldId == TEXT("parent"))
				{
					USoundSubmixBase* Parent = nullptr;
					return TryResolveObject(
							Resolver,
							Requested,
							Parent,
							OutError)
						&& TryBuildObjectReferenceProjection(
							Parent,
							false,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("effect_chain"))
				{
					TArray<USoundEffectSubmixPreset*> Resolved;
					TArray<UObject*> Objects;
					if (!TryResolveUniqueArray(
							Resolver,
							Requested,
							Resolved,
							OutError))
					{
						return false;
					}
					for (USoundEffectSubmixPreset* Object : Resolved)
					{
						Objects.Add(Object);
					}
					return TryBuildObjectReferenceArrayProjection(
						Objects,
						Resources,
						OutValue,
						OutError);
				}
			}
			if (Kind == ECoreAssetKind::Curve
				&& FieldId == TEXT("keys"))
			{
				TArray<TArray<FRichCurveKey>> Channels;
				return CurveDimension >= 1
					&& CurveDimension <= 4
					&& TryParseCurveKeys(
						Requested,
						CurveDimension,
						Channels)
					&& TryBuildCurveChannelsProjection(
						Channels,
						Resources,
						OutValue,
						OutError);
			}
			return Fail(
				OutError,
				TEXT("asset_data_core_expected_field_invalid"),
				FieldId);
		}

		bool TryBuildExpectedCoreAudioCurveFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const ECoreAssetKind Kind,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpectedFields,
			FExecutionMutationError& OutError)
		{
			OutExpectedFields.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
			if (CurrentFields.GetKind()
					!= EUeAgentValueKind::Object
				|| DefaultFields.GetKind()
					!= EUeAgentValueKind::Object
				|| ProjectionContext.GetKind()
					!= EUeAgentValueKind::Object
				|| !TypedChanges
				|| TypedChanges->GetKind()
					!= EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_core_expected_state_invalid"));
			}
			TMap<FString, const FUeAgentCanonicalValue*> Changes;
			for (const FUeAgentCanonicalValueRef& ChangeRef
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue& Change =
					ChangeRef.Get();
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change, TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadString(*Field, FieldId)
					|| Changes.Contains(FieldId))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_change_invalid"));
				}
				Changes.Add(FieldId, &Change);
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ExpectedItems;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FUeAgentCanonicalValue* const* ChangePtr =
					Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* DefaultValueRef =
					FindObjectFieldRef(
						DefaultFields,
						Current.Key);
				const FUeAgentCanonicalValue* ContextValue =
					FindObjectField(
						ProjectionContext,
						Current.Key);
				FString ContextText;
				const TCHAR* ExpectedContextTag =
					FindCoreAudioCurveContextTag(
						Kind,
						Current.Key);
				if (!ChangePtr
					|| !DefaultValueRef
					|| !ContextValue
					|| !ExpectedContextTag
					|| !TryReadString(
						*ContextValue,
						ContextText))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_context_invalid"),
						Current.Key);
				}
				int32 CurveDimension = 0;
				if (Kind == ECoreAssetKind::Curve)
				{
					const FString Prefix =
						FString(ExpectedContextTag) + TEXT(":");
					if (!ContextText.StartsWith(Prefix))
					{
						return false;
					}
					CurveDimension =
						FCString::Atoi(
							*ContextText.RightChop(
								Prefix.Len()));
					if (CurveDimension < 1 || CurveDimension > 4)
					{
						return false;
					}
				}
				else if (ContextText != ExpectedContextTag)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_core_expected_context_invalid"),
						Current.Key);
				}
				const FUeAgentCanonicalValue& Change =
					**ChangePtr;
				const FUeAgentCanonicalValue* PresenceValue =
					FindObjectField(
						Change,
						TEXT("presence"));
				FString Presence;
				if (!PresenceValue
					|| !TryReadString(
						*PresenceValue,
						Presence))
				{
					return false;
				}
				const bool bCollection =
					(Kind == ECoreAssetKind::SoundClass
							&& Current.Key
								== TEXT("child_classes"))
					|| (Kind == ECoreAssetKind::SoundMix
							&& Current.Key == TEXT("modifiers"))
					|| (Kind == ECoreAssetKind::SoundSubmix
							&& Current.Key
								== TEXT("effect_chain"));
				if (Presence == TEXT("clear"))
				{
					if (bCollection)
					{
						FUeAgentCanonicalValuePtr Empty;
						if (!TryApplyProjectedCollectionPresence(
								Presence,
								Current.Value.Get(),
								nullptr,
								Kind
											== ECoreAssetKind::SoundMix
										? TEXT("sound_class_ref")
										: FString(),
								Resources,
								Empty,
								OutError))
						{
							return false;
						}
						ExpectedItems.Emplace(
							Current.Key,
							Empty.ToSharedRef());
					}
					else if (
						Kind == ECoreAssetKind::SoundSubmix
						&& Current.Key == TEXT("parent"))
					{
						FUeAgentCanonicalValuePtr NullValue;
						if (!TryBuildNullProjectionValue(
								Resources,
								NullValue,
								OutError))
						{
							return false;
						}
						ExpectedItems.Emplace(
							Current.Key,
							NullValue.ToSharedRef());
					}
					else
					{
						ExpectedItems.Emplace(
							Current.Key,
							*DefaultValueRef);
					}
					continue;
				}
				if (Presence == TEXT("null")
					&& Kind == ECoreAssetKind::SoundSubmix
					&& Current.Key == TEXT("parent"))
				{
					FUeAgentCanonicalValuePtr NullValue;
					if (!TryBuildNullProjectionValue(
							Resources,
							NullValue,
							OutError))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						NullValue.ToSharedRef());
					continue;
				}
				const FUeAgentCanonicalValue* TypedValue =
					FindObjectField(Change, TEXT("value"));
				const FUeAgentCanonicalValue* Requested =
					TypedValue
						? FindObjectField(
							*TypedValue,
							TEXT("canonical_value"))
						: nullptr;
				FUeAgentCanonicalValuePtr Normalized;
				if (!Requested
					|| !TryNormalizeCoreAudioCurveValue(
						Resolver,
						Kind,
						Current.Key,
						*Requested,
						CurveDimension,
						Resources,
						Normalized,
						OutError))
				{
					return false;
				}
				if (bCollection)
				{
					FUeAgentCanonicalValuePtr Applied;
					if (!TryApplyProjectedCollectionPresence(
							Presence,
							Current.Value.Get(),
							Normalized.Get(),
							Kind == ECoreAssetKind::SoundMix
								? TEXT("sound_class_ref")
								: FString(),
							Resources,
							Applied,
							OutError))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						Applied.ToSharedRef());
				}
				else
				{
					if (Presence != TEXT("set")
						&& Presence != TEXT("replace"))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						Normalized.ToSharedRef());
				}
			}
			return TryBuildObjectProjectionValue(
				MoveTemp(ExpectedItems),
				Resources,
				OutExpectedFields,
				OutError);
		}
	}

	bool IsCoreAssetMechanicId(
		const FString& NativeMechanicId)
	{
		return FindCoreAssetKind(NativeMechanicId) != nullptr;
	}

	struct FCoreAssetPreparedData
	{
		ECoreAssetKind Kind = ECoreAssetKind::AudioEffectPreset;
		TArray<TStrongObjectPtr<UObject>> Pins;
		TArray<FPackageDirtySnapshot> RelatedPackages;

		FSubmixEffectSubmixEQSettings OriginalEqSettings;
		FSubmixEffectSubmixEQSettings NewEqSettings;

		FSoundAttenuationSettings OriginalAttenuation;
		FSoundAttenuationSettings NewAttenuation;

		TOptional<FSoundClassProperties>
			OriginalSoundClassProperties;
		TOptional<FSoundClassProperties>
			NewSoundClassProperties;
		bool bChangeSoundClassProperties = false;
		bool bChangeSoundClassChildren = false;
		TArray<TWeakObjectPtr<USoundClass>> NewSoundClassChildren;
		TArray<FSoundClassNodeSnapshot> SoundClassSnapshots;

		FSoundConcurrencySettings OriginalConcurrency;
		FSoundConcurrencySettings NewConcurrency;

		TArray<FSoundClassAdjuster> OriginalSoundMixModifiers;
		TArray<FSoundClassAdjuster> NewSoundMixModifiers;
		float OriginalSoundMixInitialDelay = 0.0f;
		float NewSoundMixInitialDelay = 0.0f;
		bool bChangeSoundMixModifiers = false;
		bool bChangeSoundMixInitialDelay = false;

		TWeakObjectPtr<USoundSubmixBase> NewSoundSubmixParent;
		TArray<TWeakObjectPtr<USoundEffectSubmixPreset>>
			OriginalSoundSubmixEffects;
		TArray<TWeakObjectPtr<USoundEffectSubmixPreset>>
			NewSoundSubmixEffects;
		bool bChangeSoundSubmixParent = false;
		bool bChangeSoundSubmixEffects = false;
		TArray<FSoundSubmixNodeSnapshot> SoundSubmixSnapshots;

		TArray<TArray<FRichCurveKey>> OriginalCurveChannels;
		TArray<TArray<FRichCurveKey>> NewCurveChannels;

		TArray<FRegisteredFieldValue> OriginalRegisteredFields;
		TArray<FRegisteredFieldValue> NewRegisteredFields;

		FString OriginalMediaFilePath;
		FString NewMediaFilePath;
		bool bOriginalMediaPrecache = false;
		bool bNewMediaPrecache = false;
		bool bChangeMediaFilePath = false;
		bool bChangeMediaPrecache = false;

		int32 OriginalRvtTileCount = 0;
		int32 NewRvtTileCount = 0;
		ERuntimeVirtualTextureMaterialType OriginalRvtMaterialType =
			ERuntimeVirtualTextureMaterialType::BaseColor;
		ERuntimeVirtualTextureMaterialType NewRvtMaterialType =
			ERuntimeVirtualTextureMaterialType::BaseColor;
		bool bOriginalRvtCompression = false;
		bool bNewRvtCompression = false;

		TEnumAsByte<TextureAddress> OriginalSparseAddressX = TA_Wrap;
		TEnumAsByte<TextureAddress> OriginalSparseAddressY = TA_Wrap;
		TEnumAsByte<TextureAddress> OriginalSparseAddressZ = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressX = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressY = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressZ = TA_Wrap;
		bool bOriginalSparseLocalDdcOnly = true;
		bool bNewSparseLocalDdcOnly = true;
		float OriginalSparsePoolSizeFactor = 3.0f;
		float NewSparsePoolSizeFactor = 3.0f;
		int32 OriginalSparsePrefetchFrames = 3;
		int32 NewSparsePrefetchFrames = 3;
		float OriginalSparsePrefetchPercentageStep = 20.0f;
		float NewSparsePrefetchPercentageStep = 20.0f;
		float OriginalSparsePrefetchPercentageBias = 20.0f;
		float NewSparsePrefetchPercentageBias = 20.0f;
		bool bChangeSparseAddressing = false;
		bool bChangeSparseStreaming = false;
	};

	bool TryPrepareCoreAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FCoreAssetChangeView>& Changes,
		FCoreAssetPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		if (!Kind)
		{
			return Fail(
				OutError,
				TEXT("asset_data_native_profile_unsupported"),
				NativeMechanicId);
		}
		if (!Target || Changes.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("asset_data_core_asset_changes_missing"),
				NativeMechanicId);
		}
		const TSharedRef<FCoreAssetPreparedData, ESPMode::ThreadSafe>
			Data =
				MakeShared<
					FCoreAssetPreparedData,
					ESPMode::ThreadSafe>();
		Data->Kind = *Kind;

		if (Data->Kind == ECoreAssetKind::AudioEffectPreset)
		{
			USubmixEffectSubmixEQPreset* Preset =
				Cast<USubmixEffectSubmixEQPreset>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("settings"));
			if (!Preset || !Settings || Changes.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("asset_data_audio_effect_preset_invalid"));
			}
			Data->OriginalEqSettings = Preset->Settings;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewEqSettings =
					GetDefault<USubmixEffectSubmixEQPreset>()
						->Settings;
			}
			else if (
				Settings->Presence != TEXT("set")
				|| !Settings->CanonicalValue
				|| !TryParseEqSettings(
					*Settings->CanonicalValue,
					Data->NewEqSettings))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_audio_effect_preset_"
						"settings_invalid"));
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundAttenuation)
		{
			USoundAttenuation* Attenuation =
				Cast<USoundAttenuation>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("attenuation"));
			if (!Attenuation || !Settings || Changes.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sound_attenuation_invalid"));
			}
			Data->OriginalAttenuation = Attenuation->Attenuation;
			Data->NewAttenuation = Data->OriginalAttenuation;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewAttenuation =
					GetDefault<USoundAttenuation>()->Attenuation;
			}
			else if (
				Settings->Presence != TEXT("set")
				|| !Settings->CanonicalValue
				|| !TryParseAttenuationSettings(
					*Settings->CanonicalValue,
					Data->NewAttenuation))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_sound_attenuation_"
						"settings_invalid"));
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundClass)
		{
			USoundClass* SoundClass = Cast<USoundClass>(Target);
			if (!SoundClass || Changes.Num() > 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sound_class_invalid"));
			}
			Data->OriginalSoundClassProperties =
				SoundClass->Properties;
			Data->NewSoundClassProperties =
				SoundClass->Properties;
			for (USoundClass* Child : SoundClass->ChildClasses)
			{
				Data->NewSoundClassChildren.Add(Child);
				PinUnique(Data->Pins, Child);
			}
			if (const FCoreAssetChangeView* Properties =
				FindChange(Changes, TEXT("properties")))
			{
				Data->bChangeSoundClassProperties = true;
				if (Properties->Presence == TEXT("clear"))
				{
					const FSoundClassProperties& Default =
						GetDefault<USoundClass>()->Properties;
					Data->NewSoundClassProperties->Volume =
						Default.Volume;
					Data->NewSoundClassProperties->Pitch =
						Default.Pitch;
					Data->NewSoundClassProperties->bIsUISound =
						Default.bIsUISound;
					Data->NewSoundClassProperties->bIsMusic =
						Default.bIsMusic;
				}
				else if (
					Properties->Presence != TEXT("set")
					|| !Properties->CanonicalValue
					|| !TryParseSoundClassProperties(
						*Properties->CanonicalValue,
						*Data->NewSoundClassProperties))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_class_"
							"properties_invalid"));
				}
			}
			if (const FCoreAssetChangeView* Children =
				FindChange(Changes, TEXT("child_classes")))
			{
				Data->bChangeSoundClassChildren = true;
				TArray<USoundClass*> Requested;
				if (Children->Presence != TEXT("clear")
					&& (!Children->CanonicalValue
						|| !TryResolveUniqueArray(
							Resolver,
							*Children->CanonicalValue,
							Requested,
							OutError)))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_class_"
							"children_invalid"));
				}
				TArray<USoundClass*> NewChildren;
				for (const TWeakObjectPtr<USoundClass>& Child
					: Data->NewSoundClassChildren)
				{
					if (!Child.IsValid())
					{
						return false;
					}
					NewChildren.Add(Child.Get());
				}
				if (Children->Presence == TEXT("clear"))
				{
					NewChildren.Reset();
				}
				else if (Children->Presence == TEXT("replace"))
				{
					NewChildren = Requested;
				}
				else if (Children->Presence == TEXT("remove"))
				{
					NewChildren.RemoveAll(
						[&Requested](USoundClass* Child)
						{
							return Requested.Contains(Child);
						});
				}
				else if (Children->Presence == TEXT("set"))
				{
					for (USoundClass* Child : Requested)
					{
						NewChildren.AddUnique(Child);
					}
				}
				else
				{
					return false;
				}
				for (USoundClass* Child : NewChildren)
				{
					if (!Child
						|| Child == SoundClass
						|| Child->RecurseCheckChild(SoundClass))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_sound_class_cycle"));
					}
					PinUnique(Data->Pins, Child);
				}
				Data->NewSoundClassChildren.Reset();
				for (USoundClass* Child : NewChildren)
				{
					Data->NewSoundClassChildren.Add(Child);
				}
			}
			if (!Data->bChangeSoundClassProperties
				&& !Data->bChangeSoundClassChildren)
			{
				return false;
			}
			CaptureSoundClassNode(
				Data->SoundClassSnapshots,
				Data->RelatedPackages,
				SoundClass);
			for (USoundClass* Child : SoundClass->ChildClasses)
			{
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child);
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child ? Child->ParentClass.Get() : nullptr);
			}
			for (const TWeakObjectPtr<USoundClass>& Child
				: Data->NewSoundClassChildren)
			{
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child.Get());
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child.IsValid()
						? Child->ParentClass.Get()
						: nullptr);
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundConcurrency)
		{
			USoundConcurrency* Concurrency =
				Cast<USoundConcurrency>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("concurrency"));
			if (!Concurrency || !Settings || Changes.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sound_concurrency_invalid"));
			}
			Data->OriginalConcurrency = Concurrency->Concurrency;
			Data->NewConcurrency = Data->OriginalConcurrency;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewConcurrency =
					GetDefault<USoundConcurrency>()->Concurrency;
			}
			else if (
				Settings->Presence != TEXT("set")
				|| !Settings->CanonicalValue
				|| !TryParseConcurrencySettings(
					*Settings->CanonicalValue,
					Data->NewConcurrency))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_sound_concurrency_"
						"settings_invalid"));
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundMix)
		{
			USoundMix* SoundMix = Cast<USoundMix>(Target);
			if (!SoundMix || Changes.Num() > 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sound_mix_invalid"));
			}
			Data->OriginalSoundMixModifiers =
				SoundMix->SoundClassEffects;
			Data->NewSoundMixModifiers =
				SoundMix->SoundClassEffects;
			Data->OriginalSoundMixInitialDelay =
				SoundMix->InitialDelay;
			Data->NewSoundMixInitialDelay =
				SoundMix->InitialDelay;
			if (const FCoreAssetChangeView* Modifiers =
				FindChange(Changes, TEXT("modifiers")))
			{
				Data->bChangeSoundMixModifiers = true;
				TArray<FSoundClassAdjuster> Requested;
				if (Modifiers->Presence != TEXT("clear")
					&& (!Modifiers->CanonicalValue
						|| !TryParseSoundMixModifiers(
							Resolver,
							*Modifiers->CanonicalValue,
							Requested,
							Data->Pins,
							OutError)))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_mix_"
							"modifiers_invalid"));
				}
				if (!TryApplyCollectionPresence(
						Modifiers->Presence,
						Requested,
						Data->NewSoundMixModifiers))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_mix_"
							"modifier_presence_invalid"));
				}
			}
			if (const FCoreAssetChangeView* Delay =
				FindChange(Changes, TEXT("initial_delay")))
			{
				Data->bChangeSoundMixInitialDelay = true;
				if (Delay->Presence == TEXT("clear"))
				{
					Data->NewSoundMixInitialDelay =
						GetDefault<USoundMix>()->InitialDelay;
				}
				else if (
					Delay->Presence != TEXT("set")
					|| !Delay->CanonicalValue
					|| !TryReadFloat(
						*Delay->CanonicalValue,
						Data->NewSoundMixInitialDelay,
						0.0,
						1000000.0))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_mix_"
							"initial_delay_invalid"));
				}
			}
			if (!Data->bChangeSoundMixModifiers
				&& !Data->bChangeSoundMixInitialDelay)
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundSubmix)
		{
			USoundSubmix* Submix = Cast<USoundSubmix>(Target);
			if (!Submix || Changes.Num() > 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sound_submix_invalid"));
			}
			Data->NewSoundSubmixParent = Submix->ParentSubmix;
			for (USoundEffectSubmixPreset* Effect
				: Submix->SubmixEffectChain)
			{
				Data->OriginalSoundSubmixEffects.Add(Effect);
				Data->NewSoundSubmixEffects.Add(Effect);
				PinUnique(Data->Pins, Effect);
			}
			if (const FCoreAssetChangeView* Parent =
				FindChange(Changes, TEXT("parent")))
			{
				Data->bChangeSoundSubmixParent = true;
				USoundSubmixBase* NewParent = nullptr;
				if (Parent->Presence == TEXT("set"))
				{
					if (!Parent->CanonicalValue
						|| !TryResolveObject(
							Resolver,
							*Parent->CanonicalValue,
							NewParent,
							OutError))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_sound_submix_"
								"parent_invalid"));
					}
				}
				else if (Parent->Presence != TEXT("null")
					&& Parent->Presence != TEXT("clear"))
				{
					return false;
				}
				if (WouldCreateSubmixCycle(Submix, NewParent))
				{
					return Fail(
						OutError,
						TEXT("asset_data_sound_submix_cycle"));
				}
				Data->NewSoundSubmixParent = NewParent;
				PinUnique(Data->Pins, NewParent);
			}
			if (const FCoreAssetChangeView* Effects =
				FindChange(Changes, TEXT("effect_chain")))
			{
				Data->bChangeSoundSubmixEffects = true;
				TArray<USoundEffectSubmixPreset*> Requested;
				if (Effects->Presence != TEXT("clear")
					&& (!Effects->CanonicalValue
						|| !TryResolveUniqueArray(
							Resolver,
							*Effects->CanonicalValue,
							Requested,
							OutError)))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sound_submix_"
							"effect_chain_invalid"));
				}
				TArray<USoundEffectSubmixPreset*> NewEffects;
				for (const TWeakObjectPtr<USoundEffectSubmixPreset>&
					Effect : Data->NewSoundSubmixEffects)
				{
					if (!Effect.IsValid())
					{
						return false;
					}
					NewEffects.Add(Effect.Get());
				}
				if (Effects->Presence == TEXT("clear"))
				{
					NewEffects.Reset();
				}
				else if (Effects->Presence == TEXT("replace"))
				{
					NewEffects = Requested;
				}
				else if (Effects->Presence == TEXT("remove"))
				{
					NewEffects.RemoveAll(
						[&Requested](
							USoundEffectSubmixPreset* Effect)
						{
							return Requested.Contains(Effect);
						});
				}
				else if (Effects->Presence == TEXT("set"))
				{
					for (USoundEffectSubmixPreset* Effect : Requested)
					{
						NewEffects.AddUnique(Effect);
					}
				}
				else
				{
					return false;
				}
				Data->NewSoundSubmixEffects.Reset();
				for (USoundEffectSubmixPreset* Effect : NewEffects)
				{
					Data->NewSoundSubmixEffects.Add(Effect);
					PinUnique(Data->Pins, Effect);
				}
			}
			if (!Data->bChangeSoundSubmixParent
				&& !Data->bChangeSoundSubmixEffects)
			{
				return false;
			}
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Submix);
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Submix->ParentSubmix);
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Data->NewSoundSubmixParent.Get());
		}
		else if (Data->Kind == ECoreAssetKind::Curve)
		{
			const FCoreAssetChangeView* Keys =
				FindChange(Changes, TEXT("keys"));
			const int32 Dimension = GetCurveDimension(Target);
			if (!Keys
				|| Changes.Num() != 1
				|| Keys->Presence != TEXT("replace")
				|| !Keys->CanonicalValue
				|| Dimension == 0)
			{
				return Fail(
					OutError,
					TEXT("asset_data_curve_invalid"));
			}
			Data->OriginalCurveChannels.SetNum(Dimension);
			for (int32 Index = 0; Index < Dimension; ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
				if (!Curve)
				{
					return false;
				}
				Data->OriginalCurveChannels[Index] =
					Curve->GetCopyOfKeys();
			}
			if (!TryParseCurveKeys(
					*Keys->CanonicalValue,
					Dimension,
					Data->NewCurveChannels))
			{
				return Fail(
					OutError,
					TEXT("asset_data_curve_keys_invalid"));
			}
		}
		else if (Data->Kind == ECoreAssetKind::DataObject)
		{
			const FCoreAssetChangeView* Fields =
				FindChange(Changes, TEXT("registered_fields"));
			if (!Fields || Changes.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("asset_data_registered_fields_invalid"));
			}
			if (Fields->Presence != TEXT("set")
				&& Fields->Presence != TEXT("clear"))
			{
				return false;
			}
			if (!TryParseRegisteredFields(
					Resolver,
					Target,
					Fields->CanonicalValue,
					Fields->Presence == TEXT("clear"),
					Data->OriginalRegisteredFields,
					Data->NewRegisteredFields,
					Data->Pins,
					OutError))
			{
				return Fail(
					OutError,
					TEXT("asset_data_registered_fields_invalid"));
			}
		}
		else if (Data->Kind == ECoreAssetKind::MediaSource)
		{
			UFileMediaSource* MediaSource =
				Cast<UFileMediaSource>(Target);
			if (!MediaSource || Changes.Num() > 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_source_invalid"));
			}
			Data->OriginalMediaFilePath =
				MediaSource->GetFilePath();
			Data->NewMediaFilePath =
				Data->OriginalMediaFilePath;
			Data->bOriginalMediaPrecache =
				MediaSource->PrecacheFile;
			Data->bNewMediaPrecache =
				Data->bOriginalMediaPrecache;
			if (const FCoreAssetChangeView* Uri =
				FindChange(Changes, TEXT("source_uri")))
			{
				Data->bChangeMediaFilePath = true;
				if (Uri->Presence == TEXT("clear"))
				{
					Data->NewMediaFilePath.Reset();
				}
				else if (
					Uri->Presence != TEXT("set")
					|| !Uri->CanonicalValue
					|| !TryNormalizeFileUri(
						*Uri->CanonicalValue,
						Data->NewMediaFilePath))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_media_source_uri_invalid"));
				}
			}
			if (const FCoreAssetChangeView* Options =
				FindChange(Changes, TEXT("options")))
			{
				Data->bChangeMediaPrecache = true;
				if (Options->Presence == TEXT("clear"))
				{
					Data->bNewMediaPrecache =
						GetDefault<UFileMediaSource>()
							->PrecacheFile;
				}
				else if (
					(Options->Presence != TEXT("set")
						&& Options->Presence != TEXT("replace"))
					|| !Options->CanonicalValue
					|| !HasExactObjectFields(
						*Options->CanonicalValue,
						{TEXT("precache_file")})
					|| !TryReadBool(
						*FindObjectField(
							*Options->CanonicalValue,
							TEXT("precache_file")),
						Data->bNewMediaPrecache))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_media_source_"
							"options_invalid"));
				}
			}
			if (!Data->bChangeMediaFilePath
				&& !Data->bChangeMediaPrecache)
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::RuntimeVirtualTexture)
		{
			URuntimeVirtualTexture* VirtualTexture =
				Cast<URuntimeVirtualTexture>(Target);
			if (!VirtualTexture || Changes.Num() > 3
				|| !TryReadRvtProperties(
					VirtualTexture,
					Data->OriginalRvtTileCount,
					Data->OriginalRvtMaterialType,
					Data->bOriginalRvtCompression))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_runtime_virtual_texture_invalid"));
			}
			Data->NewRvtTileCount = Data->OriginalRvtTileCount;
			Data->NewRvtMaterialType =
				Data->OriginalRvtMaterialType;
			Data->bNewRvtCompression =
				Data->bOriginalRvtCompression;
			if (const FCoreAssetChangeView* Size =
				FindChange(Changes, TEXT("size")))
			{
				int32 DesiredSize = 0;
				const int32 TileSize = VirtualTexture->GetTileSize();
				if (Size->Presence != TEXT("set")
					|| !Size->CanonicalValue
					|| !TryReadInt32(
						*Size->CanonicalValue,
						DesiredSize,
						TileSize,
						TileSize
							* (1
								<< URuntimeVirtualTexture::
									GetMaxTileCountLog2(
										VirtualTexture
											->GetAdaptivePageTable())))
					|| DesiredSize % TileSize != 0
					|| !FMath::IsPowerOfTwo(
						DesiredSize / TileSize))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_runtime_virtual_texture_"
							"size_invalid"));
				}
				Data->NewRvtTileCount =
					FMath::FloorLog2(DesiredSize / TileSize);
			}
			if (const FCoreAssetChangeView* Material =
				FindChange(Changes, TEXT("material_type")))
			{
				FString Name;
				if (Material->Presence != TEXT("set")
					|| !Material->CanonicalValue
					|| !TryReadString(
						*Material->CanonicalValue,
						Name)
					|| !TryParseRuntimeVirtualTextureMaterialType(
						Name,
						Data->NewRvtMaterialType))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_runtime_virtual_texture_"
							"material_type_invalid"));
				}
			}
			if (const FCoreAssetChangeView* Compression =
				FindChange(Changes, TEXT("compression")))
			{
				if (Compression->Presence != TEXT("set")
					|| !Compression->CanonicalValue
					|| !TryReadBool(
						*Compression->CanonicalValue,
						Data->bNewRvtCompression))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_runtime_virtual_texture_"
							"compression_invalid"));
				}
			}
		}
		else if (Data->Kind == ECoreAssetKind::SparseVolumeTexture)
		{
			UStreamableSparseVolumeTexture* Sparse =
				Cast<UStreamableSparseVolumeTexture>(Target);
			if (!Sparse || Changes.Num() > 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sparse_volume_texture_invalid"));
			}
			Data->OriginalSparseAddressX = Sparse->AddressX;
			Data->OriginalSparseAddressY = Sparse->AddressY;
			Data->OriginalSparseAddressZ = Sparse->AddressZ;
			Data->NewSparseAddressX = Sparse->AddressX;
			Data->NewSparseAddressY = Sparse->AddressY;
			Data->NewSparseAddressZ = Sparse->AddressZ;
			Data->bOriginalSparseLocalDdcOnly =
				Sparse->bLocalDDCOnly;
			Data->bNewSparseLocalDdcOnly =
				Sparse->bLocalDDCOnly;
			Data->OriginalSparsePoolSizeFactor =
				Sparse->StreamingPoolSizeFactor;
			Data->NewSparsePoolSizeFactor =
				Sparse->StreamingPoolSizeFactor;
			Data->OriginalSparsePrefetchFrames =
				Sparse->NumberOfPrefetchFrames;
			Data->NewSparsePrefetchFrames =
				Sparse->NumberOfPrefetchFrames;
			Data->OriginalSparsePrefetchPercentageStep =
				Sparse->PrefetchPercentageStepSize;
			Data->NewSparsePrefetchPercentageStep =
				Sparse->PrefetchPercentageStepSize;
			Data->OriginalSparsePrefetchPercentageBias =
				Sparse->PrefetchPercentageBias;
			Data->NewSparsePrefetchPercentageBias =
				Sparse->PrefetchPercentageBias;
			if (const FCoreAssetChangeView* Addressing =
				FindChange(Changes, TEXT("addressing")))
			{
				Data->bChangeSparseAddressing = true;
				if (Addressing->Presence == TEXT("clear"))
				{
					const UStreamableSparseVolumeTexture* Default =
						GetDefault<UStreamableSparseVolumeTexture>();
					Data->NewSparseAddressX = Default->AddressX;
					Data->NewSparseAddressY = Default->AddressY;
					Data->NewSparseAddressZ = Default->AddressZ;
				}
				else
				{
					FString X;
					FString Y;
					FString Z;
					if (Addressing->Presence != TEXT("set")
						|| !Addressing->CanonicalValue
						|| !HasExactObjectFields(
							*Addressing->CanonicalValue,
							{
								TEXT("x"),
								TEXT("y"),
								TEXT("z"),
							})
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("x")),
							X)
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("y")),
							Y)
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("z")),
							Z)
						|| !TryParseTextureAddress(
							X,
							Data->NewSparseAddressX)
						|| !TryParseTextureAddress(
							Y,
							Data->NewSparseAddressY)
						|| !TryParseTextureAddress(
							Z,
							Data->NewSparseAddressZ))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_sparse_volume_texture_"
								"addressing_invalid"));
					}
				}
			}
			if (const FCoreAssetChangeView* Streaming =
				FindChange(Changes, TEXT("streaming")))
			{
				Data->bChangeSparseStreaming = true;
				if (Streaming->Presence == TEXT("clear"))
				{
					const UStreamableSparseVolumeTexture* Default =
						GetDefault<UStreamableSparseVolumeTexture>();
					Data->bNewSparseLocalDdcOnly =
						Default->bLocalDDCOnly;
					Data->NewSparsePoolSizeFactor =
						Default->StreamingPoolSizeFactor;
					Data->NewSparsePrefetchFrames =
						Default->NumberOfPrefetchFrames;
					Data->NewSparsePrefetchPercentageStep =
						Default->PrefetchPercentageStepSize;
					Data->NewSparsePrefetchPercentageBias =
						Default->PrefetchPercentageBias;
				}
				else if (
					Streaming->Presence != TEXT("set")
					|| !Streaming->CanonicalValue
					|| !HasExactObjectFields(
						*Streaming->CanonicalValue,
						{
							TEXT("local_ddc_only"),
							TEXT("pool_size_factor"),
							TEXT("prefetch_frames"),
							TEXT("prefetch_percentage_step"),
							TEXT("prefetch_percentage_bias"),
						})
					|| !TryReadBool(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("local_ddc_only")),
						Data->bNewSparseLocalDdcOnly)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("pool_size_factor")),
						Data->NewSparsePoolSizeFactor,
						1.0,
						1000.0)
					|| !TryReadInt32(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_frames")),
						Data->NewSparsePrefetchFrames,
						0,
						1024)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_percentage_step")),
						Data->NewSparsePrefetchPercentageStep,
						0.0,
						100.0)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_percentage_bias")),
						Data->NewSparsePrefetchPercentageBias,
						0.0,
						100.0))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_sparse_volume_texture_"
							"streaming_invalid"));
				}
			}
			if (!Data->bChangeSparseAddressing
				&& !Data->bChangeSparseStreaming)
			{
				return false;
			}
		}
		else
		{
			return Fail(
				OutError,
				TEXT("asset_data_core_asset_mechanic_invalid"),
				NativeMechanicId);
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
		if (!Target || !State.Data.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_core_asset_state_invalid"));
		}
		const FCoreAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case ECoreAssetKind::AudioEffectPreset:
			{
				USubmixEffectSubmixEQPreset* Preset =
					Cast<USubmixEffectSubmixEQPreset>(Target);
				if (!Preset)
				{
					return false;
				}
				Preset->Modify();
				Preset->Settings = Data.NewEqSettings;
				Preset->SetSettings(Data.NewEqSettings);
				Preset->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundAttenuation:
			{
				USoundAttenuation* Attenuation =
					Cast<USoundAttenuation>(Target);
				if (!Attenuation)
				{
					return false;
				}
				Attenuation->Modify();
				Attenuation->Attenuation = Data.NewAttenuation;
				Attenuation->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundClass:
			{
				USoundClass* SoundClass = Cast<USoundClass>(Target);
				if (!SoundClass)
				{
					return false;
				}
				SoundClass->Modify();
				if (Data.bChangeSoundClassProperties)
				{
					SoundClass->Properties =
						*Data.NewSoundClassProperties;
				}
				if (Data.bChangeSoundClassChildren)
				{
					TArray<USoundClass*> NewChildren;
					for (const TWeakObjectPtr<USoundClass>& Child
						: Data.NewSoundClassChildren)
					{
						if (!Child.IsValid())
						{
							return false;
						}
						NewChildren.Add(Child.Get());
					}
					for (USoundClass* OldChild
						: SoundClass->ChildClasses)
					{
						if (OldChild
							&& !NewChildren.Contains(OldChild)
							&& OldChild->ParentClass == SoundClass)
						{
							OldChild->Modify();
							OldChild->ParentClass = nullptr;
							OldChild->PostEditChange();
						}
					}
					for (USoundClass* Child : NewChildren)
					{
						if (Child->ParentClass
							&& Child->ParentClass != SoundClass)
						{
							Child->ParentClass->Modify();
							Child->ParentClass
								->ChildClasses.Remove(Child);
							Child->ParentClass->PostEditChange();
						}
						Child->Modify();
						Child->ParentClass = SoundClass;
						Child->PostEditChange();
					}
					SoundClass->ChildClasses.Reset(
						NewChildren.Num());
					for (USoundClass* Child : NewChildren)
					{
						SoundClass->ChildClasses.Add(Child);
					}
				}
				SoundClass->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundConcurrency:
			{
				USoundConcurrency* Concurrency =
					Cast<USoundConcurrency>(Target);
				if (!Concurrency)
				{
					return false;
				}
				Concurrency->Modify();
				Concurrency->Concurrency = Data.NewConcurrency;
				Concurrency->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundMix:
			{
				USoundMix* SoundMix = Cast<USoundMix>(Target);
				if (!SoundMix)
				{
					return false;
				}
				SoundMix->Modify();
				if (Data.bChangeSoundMixModifiers)
				{
					SoundMix->SoundClassEffects =
						Data.NewSoundMixModifiers;
				}
				if (Data.bChangeSoundMixInitialDelay)
				{
					SoundMix->InitialDelay =
						Data.NewSoundMixInitialDelay;
				}
				SoundMix->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundSubmix:
			{
				USoundSubmix* Submix = Cast<USoundSubmix>(Target);
				if (!Submix)
				{
					return false;
				}
				Submix->Modify();
				if (Data.bChangeSoundSubmixParent)
				{
					Submix->SetParentSubmix(
						Data.NewSoundSubmixParent.Get(),
						true);
				}
				if (Data.bChangeSoundSubmixEffects)
				{
					Submix->SubmixEffectChain.Reset(
						Data.NewSoundSubmixEffects.Num());
					for (const TWeakObjectPtr<
							USoundEffectSubmixPreset>& Effect
						: Data.NewSoundSubmixEffects)
					{
						if (!Effect.IsValid())
						{
							return false;
						}
						Submix->SubmixEffectChain.Add(Effect.Get());
					}
				}
				Submix->PostEditChange();
				break;
			}
		case ECoreAssetKind::Curve:
			return WriteCurveChannels(
				Target,
				Data.NewCurveChannels,
				true)
					? true
					: Fail(
						OutError,
						TEXT("asset_data_curve_apply_failed"));
		case ECoreAssetKind::DataObject:
			{
				Target->Modify();
				for (const FRegisteredFieldValue& Value
					: Data.NewRegisteredFields)
				{
					WriteRegisteredValue(Target, Value);
				}
				Target->PostEditChange();
				break;
			}
		case ECoreAssetKind::MediaSource:
			{
				UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				if (!MediaSource)
				{
					return false;
				}
				MediaSource->Modify();
				if (Data.bChangeMediaFilePath)
				{
					MediaSource->SetFilePath(
						Data.NewMediaFilePath);
				}
				if (Data.bChangeMediaPrecache)
				{
					MediaSource->PrecacheFile =
						Data.bNewMediaPrecache;
				}
				MediaSource->PostEditChange();
				break;
			}
		case ECoreAssetKind::RuntimeVirtualTexture:
			return TryWriteRvtProperties(
				Cast<URuntimeVirtualTexture>(Target),
				Data.NewRvtTileCount,
				Data.NewRvtMaterialType,
				Data.bNewRvtCompression,
				true)
					? true
					: Fail(
						OutError,
						TEXT(
							"asset_data_runtime_virtual_texture_"
							"apply_failed"));
		case ECoreAssetKind::SparseVolumeTexture:
			{
				UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				if (!Sparse)
				{
					return false;
				}
				Sparse->Modify();
				if (Data.bChangeSparseAddressing)
				{
					Sparse->AddressX = Data.NewSparseAddressX;
					Sparse->AddressY = Data.NewSparseAddressY;
					Sparse->AddressZ = Data.NewSparseAddressZ;
				}
				if (Data.bChangeSparseStreaming)
				{
					Sparse->bLocalDDCOnly =
						Data.bNewSparseLocalDdcOnly;
					Sparse->StreamingPoolSizeFactor =
						Data.NewSparsePoolSizeFactor;
					Sparse->NumberOfPrefetchFrames =
						Data.NewSparsePrefetchFrames;
					Sparse->PrefetchPercentageStepSize =
						Data.NewSparsePrefetchPercentageStep;
					Sparse->PrefetchPercentageBias =
						Data.NewSparsePrefetchPercentageBias;
				}
				Sparse->PostEditChange();
				break;
			}
		}
		return true;
	}

	bool TryRollbackCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State)
	{
		if (!Target || !State.Data.IsValid())
		{
			return false;
		}
		const FCoreAssetPreparedData& Data = *State.Data;
		bool bRestored = true;
		switch (Data.Kind)
		{
		case ECoreAssetKind::AudioEffectPreset:
			{
				USubmixEffectSubmixEQPreset* Preset =
					Cast<USubmixEffectSubmixEQPreset>(Target);
				if (!Preset)
				{
					return false;
				}
				Preset->Settings = Data.OriginalEqSettings;
				Preset->SetSettings(Data.OriginalEqSettings);
				Preset->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundAttenuation:
			{
				USoundAttenuation* Attenuation =
					Cast<USoundAttenuation>(Target);
				if (!Attenuation)
				{
					return false;
				}
				Attenuation->Attenuation =
					Data.OriginalAttenuation;
				Attenuation->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundClass:
			{
				USoundClass* SoundClass = Cast<USoundClass>(Target);
				if (!SoundClass)
				{
					return false;
				}
				SoundClass->Properties =
					*Data.OriginalSoundClassProperties;
				for (const FSoundClassNodeSnapshot& Snapshot
					: Data.SoundClassSnapshots)
				{
					USoundClass* Node = Snapshot.Node.Get();
					if (!Node)
					{
						bRestored = false;
						continue;
					}
					Node->ParentClass = Snapshot.Parent.Get();
					Node->ChildClasses.Reset(
						Snapshot.Children.Num());
					for (const TWeakObjectPtr<USoundClass>& Child
						: Snapshot.Children)
					{
						if (!Child.IsValid())
						{
							bRestored = false;
							continue;
						}
						Node->ChildClasses.Add(Child.Get());
					}
					Node->PostEditChange();
				}
				SoundClass->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundConcurrency:
			{
				USoundConcurrency* Concurrency =
					Cast<USoundConcurrency>(Target);
				if (!Concurrency)
				{
					return false;
				}
				Concurrency->Concurrency =
					Data.OriginalConcurrency;
				Concurrency->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundMix:
			{
				USoundMix* SoundMix = Cast<USoundMix>(Target);
				if (!SoundMix)
				{
					return false;
				}
				SoundMix->SoundClassEffects =
					Data.OriginalSoundMixModifiers;
				SoundMix->InitialDelay =
					Data.OriginalSoundMixInitialDelay;
				SoundMix->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundSubmix:
			{
				USoundSubmix* Submix = Cast<USoundSubmix>(Target);
				if (!Submix)
				{
					return false;
				}
				Submix->SubmixEffectChain.Reset(
					Data.OriginalSoundSubmixEffects.Num());
				for (const TWeakObjectPtr<
						USoundEffectSubmixPreset>& Effect
					: Data.OriginalSoundSubmixEffects)
				{
					if (!Effect.IsValid())
					{
						bRestored = false;
						continue;
					}
					Submix->SubmixEffectChain.Add(Effect.Get());
				}
				for (const FSoundSubmixNodeSnapshot& Snapshot
					: Data.SoundSubmixSnapshots)
				{
					USoundSubmixBase* Node = Snapshot.Node.Get();
					if (!Node)
					{
						bRestored = false;
						continue;
					}
					Node->ChildSubmixes.Reset(
						Snapshot.Children.Num());
					for (const TWeakObjectPtr<USoundSubmixBase>&
						Child : Snapshot.Children)
					{
						if (!Child.IsValid())
						{
							bRestored = false;
							continue;
						}
						Node->ChildSubmixes.Add(Child.Get());
					}
					if (USoundSubmixWithParentBase* WithParent =
						Cast<USoundSubmixWithParentBase>(Node))
					{
						WithParent->ParentSubmix =
							Snapshot.Parent.Get();
					}
					Node->PostEditChange();
				}
				Submix->PostEditChange();
				break;
			}
		case ECoreAssetKind::Curve:
			bRestored = WriteCurveChannels(
				Target,
				Data.OriginalCurveChannels,
				false);
			break;
		case ECoreAssetKind::DataObject:
			for (const FRegisteredFieldValue& Value
				: Data.OriginalRegisteredFields)
			{
				WriteRegisteredValue(Target, Value);
			}
			Target->PostEditChange();
			break;
		case ECoreAssetKind::MediaSource:
			{
				UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				if (!MediaSource)
				{
					return false;
				}
				MediaSource->SetFilePath(
					Data.OriginalMediaFilePath);
				MediaSource->PrecacheFile =
					Data.bOriginalMediaPrecache;
				MediaSource->PostEditChange();
				break;
			}
		case ECoreAssetKind::RuntimeVirtualTexture:
			bRestored = TryWriteRvtProperties(
				Cast<URuntimeVirtualTexture>(Target),
				Data.OriginalRvtTileCount,
				Data.OriginalRvtMaterialType,
				Data.bOriginalRvtCompression,
				false);
			break;
		case ECoreAssetKind::SparseVolumeTexture:
			{
				UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				if (!Sparse)
				{
					return false;
				}
				Sparse->AddressX = Data.OriginalSparseAddressX;
				Sparse->AddressY = Data.OriginalSparseAddressY;
				Sparse->AddressZ = Data.OriginalSparseAddressZ;
				Sparse->bLocalDDCOnly =
					Data.bOriginalSparseLocalDdcOnly;
				Sparse->StreamingPoolSizeFactor =
					Data.OriginalSparsePoolSizeFactor;
				Sparse->NumberOfPrefetchFrames =
					Data.OriginalSparsePrefetchFrames;
				Sparse->PrefetchPercentageStepSize =
					Data.OriginalSparsePrefetchPercentageStep;
				Sparse->PrefetchPercentageBias =
					Data.OriginalSparsePrefetchPercentageBias;
				Sparse->PostEditChange();
				break;
			}
		}
		RestorePackageFlags(Data.RelatedPackages);
		return bRestored;
	}

	bool CanProjectCoreAssetFieldState(
		const FString& NativeMechanicId)
	{
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		return Kind != nullptr;
	}

	bool TryProjectCoreAssetCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		if (!CanProjectCoreAssetFieldState(NativeMechanicId)
			|| !Kind
			|| !Target)
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_core_field_projection_unsupported"),
				NativeMechanicId);
		}
		if (*Kind == ECoreAssetKind::MediaSource
			|| *Kind
				== ECoreAssetKind::RuntimeVirtualTexture
			|| *Kind
				== ECoreAssetKind::SparseVolumeTexture)
		{
			return TryBuildCoreConfigurationProjection(
				*Kind,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (*Kind != ECoreAssetKind::DataObject)
		{
			return TryBuildCoreAudioCurveProjection(
				*Kind,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (SelectedFieldIds
			!= TArray<FString>{TEXT("registered_fields")})
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_core_field_projection_unsupported"),
				NativeMechanicId);
		}

		FUeAgentCanonicalValuePtr CurrentRegistered;
		FUeAgentCanonicalValuePtr CurrentContext;
		if (!TryBuildRegisteredFieldProjection(
				Target,
				Resources,
				CurrentRegistered,
				CurrentContext,
				OutError))
		{
			return false;
		}
		UObject* Default =
			Target->GetClass()->GetDefaultObject();
		FUeAgentCanonicalValuePtr DefaultRegistered;
		FUeAgentCanonicalValuePtr DefaultContext;
		if (!TryBuildRegisteredFieldProjection(
				Default,
				Resources,
				DefaultRegistered,
				DefaultContext,
				OutError))
		{
			return false;
		}
		FUeAgentCanonicalValuePtr IgnoredContext;
		if (!TryWrapRegisteredFieldProjection(
				CurrentRegistered.ToSharedRef(),
				CurrentContext.ToSharedRef(),
				Resources,
				OutCurrentFields,
				OutProjectionContext,
				OutError)
			|| !TryWrapRegisteredFieldProjection(
				DefaultRegistered.ToSharedRef(),
				DefaultContext.ToSharedRef(),
				Resources,
				OutDefaultFields,
				IgnoredContext,
				OutError))
		{
			return false;
		}
		return OutCurrentFields.IsValid()
			&& OutDefaultFields.IsValid()
			&& OutProjectionContext.IsValid();
	}

	bool TryProjectCoreAssetExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		OutExpectedFields.Reset();
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		if (!CanProjectCoreAssetFieldState(NativeMechanicId)
			|| !Kind)
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_core_field_projection_unsupported"),
				NativeMechanicId);
		}
		if (*Kind == ECoreAssetKind::MediaSource
			|| *Kind
				== ECoreAssetKind::RuntimeVirtualTexture
			|| *Kind
				== ECoreAssetKind::SparseVolumeTexture)
		{
			return TryBuildExpectedCoreConfigurationFields(
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (*Kind != ECoreAssetKind::DataObject)
		{
			return TryBuildExpectedCoreAudioCurveFields(
				Resolver,
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		return TryBuildExpectedRegisteredFields(
			Resolver,
			CurrentFields,
			DefaultFields,
			ProjectionContext,
			ProposedAfter,
			Resources,
			OutExpectedFields,
			OutError);
	}
}
