// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Montage.h"

#include "UeAgentJsonDiagnostics.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimMontageFactory.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UeAgentMontageOps
{
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

	static TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	static bool TryParseLinearColorField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FLinearColor& OutColor)
	{
		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
		{
			return false;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		const bool bHasR = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, R);
		const bool bHasG = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, G);
		const bool bHasB = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, B);
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, A);
		if (!bHasR || !bHasG || !bHasB)
		{
			return false;
		}

		const bool bLooksByteRange = R > 1.0 || G > 1.0 || B > 1.0 || A > 1.0;
		if (bLooksByteRange)
		{
			OutColor = FLinearColor(
				static_cast<float>(R / 255.0),
				static_cast<float>(G / 255.0),
				static_cast<float>(B / 255.0),
				static_cast<float>(A / 255.0));
		}
		else
		{
			OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		}
		return true;
	}

	static bool TryGetStringFieldValue(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumberFieldValue(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetBoolFieldValue(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static UAnimMontage* LoadMontage(const FString& InPath)
	{
		return Cast<UAnimMontage>(LoadAssetObject(InPath));
	}

	static USkeleton* LoadSkeleton(const FString& InPath)
	{
		return Cast<USkeleton>(LoadAssetObject(InPath));
	}

	static USkeleton* ResolveSkeleton(const FString& AssetPath, const FString& SkeletonPath, FString& OutSourceField)
	{
		OutSourceField.Reset();
		if (!SkeletonPath.TrimStartAndEnd().IsEmpty())
		{
			OutSourceField = TEXT("skeleton_path");
			return LoadSkeleton(SkeletonPath);
		}

		if (!AssetPath.TrimStartAndEnd().IsEmpty())
		{
			if (UAnimMontage* Montage = LoadMontage(AssetPath))
			{
				OutSourceField = TEXT("asset_path");
				return Montage->GetSkeleton();
			}
		}

		return nullptr;
	}

	static USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	static UAnimSequence* LoadAnimSequence(const FString& InPath)
	{
		return Cast<UAnimSequence>(LoadAssetObject(InPath));
	}

	static UAnimSequenceBase* LoadAnimSequenceBase(const FString& InPath)
	{
		return Cast<UAnimSequenceBase>(LoadAssetObject(InPath));
	}

	static UClass* LoadClassObject(UClass* BaseClass, const FString& InPath)
	{
		FString ClassPath = InPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		return StaticLoadClass(BaseClass, nullptr, *ClassPath);
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

	static int32 FindSlotTrackIndexByName(const UAnimMontage* Montage, const FString& InSlotName)
	{
		if (!Montage || InSlotName.IsEmpty())
		{
			return INDEX_NONE;
		}

		const FString SlotName = InSlotName.TrimStartAndEnd();
		for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
		{
			if (Montage->SlotAnimTracks[Index].SlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static int32 FindSectionIndexByName(const UAnimMontage* Montage, const FString& InSectionName)
	{
		if (!Montage || InSectionName.IsEmpty())
		{
			return INDEX_NONE;
		}

		const FString SectionName = InSectionName.TrimStartAndEnd();
		for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
		{
			if (Montage->CompositeSections[Index].SectionName.ToString().Equals(SectionName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static int32 FindNotifyTrackIndexByName(const UAnimMontage* Montage, const FString& InTrackName)
	{
#if WITH_EDITORONLY_DATA
		if (!Montage || InTrackName.IsEmpty())
		{
			return INDEX_NONE;
		}

		const FString TrackName = InTrackName.TrimStartAndEnd();
		for (int32 Index = 0; Index < Montage->AnimNotifyTracks.Num(); ++Index)
		{
			if (Montage->AnimNotifyTracks[Index].TrackName.ToString().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
#endif
		return INDEX_NONE;
	}

	static FString GetNotifyTrackName(const UAnimMontage* Montage, const int32 TrackIndex)
	{
#if WITH_EDITORONLY_DATA
		if (Montage && Montage->AnimNotifyTracks.IsValidIndex(TrackIndex))
		{
			return Montage->AnimNotifyTracks[TrackIndex].TrackName.ToString();
		}
#endif
		return FString();
	}

	static int32 FindNotifyIndexByGuid(const UAnimMontage* Montage, const FGuid& Guid)
	{
#if WITH_EDITORONLY_DATA
		if (!Montage || !Guid.IsValid())
		{
			return INDEX_NONE;
		}

		for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
		{
			if (Montage->Notifies[Index].Guid == Guid)
			{
				return Index;
			}
		}
#endif
		return INDEX_NONE;
	}

	static bool EnsureNotifyTrack(UAnimMontage* Montage, const FName TrackName, int32& OutTrackIndex)
	{
#if WITH_EDITORONLY_DATA
		if (!Montage)
		{
			return false;
		}

		Montage->InitializeNotifyTrack();
		OutTrackIndex = FindNotifyTrackIndexByName(Montage, TrackName.ToString());
		if (OutTrackIndex != INDEX_NONE)
		{
			return true;
		}

		Montage->AnimNotifyTracks.Add(FAnimNotifyTrack(TrackName, FLinearColor::White));
		OutTrackIndex = Montage->AnimNotifyTracks.Num() - 1;
		return true;
#else
		OutTrackIndex = INDEX_NONE;
		return false;
#endif
	}

	static void SortSectionsByTime(UAnimMontage* Montage)
	{
		if (!Montage)
		{
			return;
		}

		Montage->CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B)
		{
			return A.GetTime() < B.GetTime();
		});
	}

	static void RelinkMontageSections(UAnimMontage* Montage)
	{
		if (!Montage)
		{
			return;
		}

		const int32 MaxSlotIndex = FMath::Max(0, Montage->SlotAnimTracks.Num() - 1);
		for (FCompositeSection& Section : Montage->CompositeSections)
		{
			const float CurrentTime = Section.GetTime();
			const int32 TargetSlotIndex = FMath::Clamp(Section.GetSlotIndex(), 0, MaxSlotIndex);
			Section.Link(Montage, CurrentTime, TargetSlotIndex);
		}
	}

	static bool FinalizeMontageMutation(UAnimMontage* Montage, const bool bSave, FString& OutError)
	{
		if (!Montage)
		{
			OutError = TEXT("montage_not_found");
			return false;
		}

		for (FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
		{
#if WITH_EDITOR
			SlotTrack.AnimTrack.SortAnimSegments();
#endif
		}

		SortSectionsByTime(Montage);
		Montage->SetCompositeLength(Montage->CalculateSequenceLength());
		RelinkMontageSections(Montage);
		Montage->UpdateLinkableElements();
		Montage->RefreshCacheData();
		Montage->MarkPackageDirty();

		if (bSave)
		{
			return SaveAssetPackage(Montage, OutError);
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSegmentJson(const FAnimSegment& Segment, const int32 SegmentIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("segment_index"), SegmentIndex);
		Item->SetStringField(TEXT("animation_asset"), Segment.GetAnimReference() ? Segment.GetAnimReference()->GetPathName() : TEXT(""));
		Item->SetNumberField(TEXT("animation_length"), Segment.GetAnimReference() ? Segment.GetAnimReference()->GetPlayLength() : 0.0);
		Item->SetNumberField(TEXT("start_pos"), Segment.StartPos);
		Item->SetNumberField(TEXT("anim_start_time"), Segment.AnimStartTime);
		Item->SetNumberField(TEXT("anim_end_time"), Segment.AnimEndTime);
		Item->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
		Item->SetNumberField(TEXT("loop_count"), Segment.LoopingCount);
		Item->SetNumberField(TEXT("segment_length"), Segment.GetLength());
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildSlotTrackJson(const FSlotAnimationTrack& SlotTrack, const int32 SlotIndex, const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("slot_index"), SlotIndex);
		Item->SetStringField(TEXT("slot_name"), SlotTrack.SlotName.ToString());
		Item->SetBoolField(TEXT("skeleton_slot_registered"), Skeleton ? Skeleton->ContainsSlotName(SlotTrack.SlotName) : false);
		Item->SetStringField(TEXT("skeleton_group_name"), (Skeleton && Skeleton->ContainsSlotName(SlotTrack.SlotName)) ? Skeleton->GetSlotGroupName(SlotTrack.SlotName).ToString() : TEXT(""));
		Item->SetNumberField(TEXT("track_length"), SlotTrack.AnimTrack.GetLength());
		Item->SetNumberField(TEXT("segment_count"), SlotTrack.AnimTrack.AnimSegments.Num());

		TArray<TSharedPtr<FJsonValue>> Segments;
		for (int32 SegmentIndex = 0; SegmentIndex < SlotTrack.AnimTrack.AnimSegments.Num(); ++SegmentIndex)
		{
			Segments.Add(MakeShared<FJsonValueObject>(BuildSegmentJson(SlotTrack.AnimTrack.AnimSegments[SegmentIndex], SegmentIndex)));
		}
		Item->SetArrayField(TEXT("segments"), Segments);
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildSectionJson(const FCompositeSection& Section, const int32 SectionIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("section_index"), SectionIndex);
		Item->SetStringField(TEXT("section_name"), Section.SectionName.ToString());
		Item->SetNumberField(TEXT("time_seconds"), Section.GetTime());
		Item->SetStringField(TEXT("next_section_name"), Section.NextSectionName.IsNone() ? TEXT("") : Section.NextSectionName.ToString());
		Item->SetNumberField(TEXT("slot_index"), Section.GetSlotIndex());
		Item->SetNumberField(TEXT("segment_index"), Section.GetSegmentIndex());
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildNotifyTrackJson(const FAnimNotifyTrack& Track, const int32 TrackIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("track_index"), TrackIndex);
		Item->SetStringField(TEXT("track_name"), Track.TrackName.ToString());
		Item->SetStringField(TEXT("track_color"), Track.TrackColor.ToString());
		Item->SetObjectField(TEXT("track_color_linear"), BuildLinearColorJson(Track.TrackColor));
		Item->SetNumberField(TEXT("notify_count"), Track.Notifies.Num());
		return Item;
	}

	static FString MontageNotifyTickTypeToString(const EMontageNotifyTickType::Type TickType)
	{
		return TickType == EMontageNotifyTickType::BranchingPoint ? TEXT("branching_point") : TEXT("queued");
	}

	static bool ParseMontageNotifyTickType(const FString& InValue, EMontageNotifyTickType::Type& OutTickType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("queued") || Normalized == TEXT("queue"))
		{
			OutTickType = EMontageNotifyTickType::Queued;
			return true;
		}
		if (Normalized == TEXT("branchingpoint") || Normalized == TEXT("branch"))
		{
			OutTickType = EMontageNotifyTickType::BranchingPoint;
			return true;
		}
		return false;
	}

	static FString NotifyFilterTypeToString(const ENotifyFilterType::Type FilterType)
	{
		switch (FilterType)
		{
		case ENotifyFilterType::LOD:
			return TEXT("lod");
		case ENotifyFilterType::NoFiltering:
		default:
			return TEXT("none");
		}
	}

	static bool ParseNotifyFilterType(const FString& InValue, ENotifyFilterType::Type& OutFilterType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none") || Normalized == TEXT("nofiltering") || Normalized == TEXT("nofilter"))
		{
			OutFilterType = ENotifyFilterType::NoFiltering;
			return true;
		}
		if (Normalized == TEXT("lod"))
		{
			OutFilterType = ENotifyFilterType::LOD;
			return true;
		}
		return false;
	}

	static bool ApplyNotifyCommonSettings(const TSharedPtr<FJsonObject>& Params, FAnimNotifyEvent& Notify, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		double TriggerWeightThreshold = 0.0;
		if (TryGetNumberFieldValue(Params, TEXT("trigger_weight_threshold"), TriggerWeightThreshold))
		{
			if (TriggerWeightThreshold < 0.0)
			{
				OutError = TEXT("invalid_trigger_weight_threshold");
				return false;
			}
			Notify.TriggerWeightThreshold = static_cast<float>(TriggerWeightThreshold);
		}

		double NotifyTriggerChance = 0.0;
		if (TryGetNumberFieldValue(Params, TEXT("notify_trigger_chance"), NotifyTriggerChance))
		{
			if (NotifyTriggerChance < 0.0 || NotifyTriggerChance > 1.0)
			{
				OutError = TEXT("invalid_notify_trigger_chance");
				return false;
			}
			Notify.NotifyTriggerChance = static_cast<float>(NotifyTriggerChance);
		}

		FString TickTypeText;
		if (TryGetStringFieldValue(Params, TEXT("tick_type"), TickTypeText) && !TickTypeText.TrimStartAndEnd().IsEmpty())
		{
			EMontageNotifyTickType::Type TickType = EMontageNotifyTickType::Queued;
			if (!ParseMontageNotifyTickType(TickTypeText, TickType))
			{
				OutError = TEXT("invalid_tick_type");
				return false;
			}
			Notify.MontageTickType = TickType;
		}
		else
		{
			bool bBranchingPoint = (Notify.MontageTickType == EMontageNotifyTickType::BranchingPoint);
			if (TryGetBoolFieldValue(Params, TEXT("branching_point"), bBranchingPoint))
			{
				Notify.MontageTickType = bBranchingPoint ? EMontageNotifyTickType::BranchingPoint : EMontageNotifyTickType::Queued;
			}
		}

		FString FilterTypeText;
		if (TryGetStringFieldValue(Params, TEXT("notify_filter_type"), FilterTypeText) && !FilterTypeText.TrimStartAndEnd().IsEmpty())
		{
			ENotifyFilterType::Type FilterType = ENotifyFilterType::NoFiltering;
			if (!ParseNotifyFilterType(FilterTypeText, FilterType))
			{
				OutError = TEXT("invalid_notify_filter_type");
				return false;
			}
			Notify.NotifyFilterType = FilterType;
		}

		double NotifyFilterLod = 0.0;
		if (TryGetNumberFieldValue(Params, TEXT("notify_filter_lod"), NotifyFilterLod))
		{
			if (NotifyFilterLod < 0.0)
			{
				OutError = TEXT("invalid_notify_filter_lod");
				return false;
			}
			Notify.NotifyFilterLOD = static_cast<int32>(NotifyFilterLod);
		}

		bool BoolValue = false;
		if (TryGetBoolFieldValue(Params, TEXT("can_be_filtered_via_request"), BoolValue))
		{
			Notify.bCanBeFilteredViaRequest = BoolValue;
		}
		if (TryGetBoolFieldValue(Params, TEXT("trigger_on_dedicated_server"), BoolValue))
		{
			Notify.bTriggerOnDedicatedServer = BoolValue;
		}
		if (TryGetBoolFieldValue(Params, TEXT("trigger_on_follower"), BoolValue))
		{
			Notify.bTriggerOnFollower = BoolValue;
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildNotifyJson(const UAnimMontage* Montage, const FAnimNotifyEvent& Notify, const int32 NotifyIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("notify_index"), NotifyIndex);
		Item->SetStringField(TEXT("notify_name"), Notify.NotifyName.ToString());
		Item->SetStringField(TEXT("track_name"), GetNotifyTrackName(Montage, Notify.TrackIndex));
		Item->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
		Item->SetNumberField(TEXT("time_seconds"), Notify.GetTime());
		Item->SetNumberField(TEXT("trigger_time_seconds"), Notify.GetTriggerTime());
		Item->SetNumberField(TEXT("duration_seconds"), Notify.GetDuration());
		Item->SetBoolField(TEXT("is_state"), Notify.NotifyStateClass != nullptr);
		Item->SetBoolField(TEXT("is_blueprint_notify"), Notify.IsBlueprintNotify());
		Item->SetBoolField(TEXT("branching_point"), Notify.MontageTickType == EMontageNotifyTickType::BranchingPoint);
		Item->SetStringField(TEXT("tick_type"), MontageNotifyTickTypeToString(Notify.MontageTickType));
		Item->SetNumberField(TEXT("trigger_weight_threshold"), Notify.TriggerWeightThreshold);
		Item->SetNumberField(TEXT("notify_trigger_chance"), Notify.NotifyTriggerChance);
		Item->SetStringField(TEXT("notify_filter_type"), NotifyFilterTypeToString(Notify.NotifyFilterType));
		Item->SetNumberField(TEXT("notify_filter_lod"), Notify.NotifyFilterLOD);
		Item->SetBoolField(TEXT("can_be_filtered_via_request"), Notify.bCanBeFilteredViaRequest);
		Item->SetBoolField(TEXT("trigger_on_dedicated_server"), Notify.bTriggerOnDedicatedServer);
		Item->SetBoolField(TEXT("trigger_on_follower"), Notify.bTriggerOnFollower);
		Item->SetStringField(TEXT("notify_color"), Notify.NotifyColor.ToString());
		Item->SetObjectField(TEXT("notify_color_linear"), BuildLinearColorJson(FLinearColor(Notify.NotifyColor)));
		Item->SetStringField(TEXT("notify_class"), Notify.Notify ? Notify.Notify->GetClass()->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("notify_state_class"), Notify.NotifyStateClass ? Notify.NotifyStateClass->GetClass()->GetPathName() : TEXT(""));
#if WITH_EDITORONLY_DATA
		Item->SetStringField(TEXT("guid"), Notify.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
#endif
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildSyncMarkerJson(const UAnimMontage* Montage, const FAnimSyncMarker& Marker, const int32 MarkerIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("marker_index"), MarkerIndex);
		Item->SetStringField(TEXT("marker_name"), Marker.MarkerName.ToString());
		Item->SetNumberField(TEXT("time_seconds"), Marker.Time);
#if WITH_EDITORONLY_DATA
		Item->SetNumberField(TEXT("track_index"), Marker.TrackIndex);
		Item->SetStringField(TEXT("track_name"), GetNotifyTrackName(Montage, Marker.TrackIndex));
		Item->SetStringField(TEXT("guid"), Marker.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
#endif
		return Item;
	}

	static bool ResolveMontageSyncSlotIndex(const UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Params, int32& OutSyncSlotIndex, bool& bExplicitSlotSelection, FString& OutError)
	{
		bExplicitSlotSelection = false;
		OutSyncSlotIndex = INDEX_NONE;

		if (!Montage)
		{
			OutError = TEXT("montage_not_found");
			return false;
		}

		if (Montage->SlotAnimTracks.Num() <= 0)
		{
			OutError = TEXT("montage_has_no_slot_tracks");
			return false;
		}

		FString SyncSlotNameText;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("sync_slot_name"), SyncSlotNameText))
		{
			SyncSlotNameText = SyncSlotNameText.TrimStartAndEnd();
			if (!SyncSlotNameText.IsEmpty())
			{
				const int32 SlotIndex = FindSlotTrackIndexByName(Montage, SyncSlotNameText);
				if (SlotIndex == INDEX_NONE)
				{
					OutError = TEXT("sync_slot_name_not_found");
					return false;
				}

				OutSyncSlotIndex = SlotIndex;
				bExplicitSlotSelection = true;
				return true;
			}
		}

		double SyncSlotIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("sync_slot_index"), SyncSlotIndexNumber))
		{
			const int32 SlotIndex = static_cast<int32>(SyncSlotIndexNumber);
			if (!Montage->SlotAnimTracks.IsValidIndex(SlotIndex))
			{
				OutError = TEXT("invalid_sync_slot_index");
				return false;
			}

			OutSyncSlotIndex = SlotIndex;
			bExplicitSlotSelection = true;
			return true;
		}

		OutSyncSlotIndex = Montage->SlotAnimTracks.IsValidIndex(Montage->SyncSlotIndex) ? Montage->SyncSlotIndex : 0;
		return true;
	}

	static bool ParseBlendMode(const FString& InText, EMontageBlendMode& OutMode)
	{
		FString Value = InText.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value.IsEmpty() || Value == TEXT("standard"))
		{
			OutMode = EMontageBlendMode::Standard;
			return true;
		}
		if (Value == TEXT("inertialization"))
		{
			OutMode = EMontageBlendMode::Inertialization;
			return true;
		}
		return false;
	}

	static FString BlendModeToString(const EMontageBlendMode InMode)
	{
		switch (InMode)
		{
		case EMontageBlendMode::Inertialization:
			return TEXT("inertialization");
		case EMontageBlendMode::Standard:
		default:
			return TEXT("standard");
		}
	}
}

bool FUeAgentHttpServer::ExecuteMontageCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}
	else
	{
		OutData->Values.Reset();
	}

	if (CommandLower == TEXT("montage_list_montages")) return CmdMontageListMontages(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_create")) return CmdMontageCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_open_editor")) return CmdOpenAssetEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_get_info")) return CmdMontageGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_export_json")) return CmdMontageExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_apply_json")) return CmdMontageApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_preview_mesh")) return CmdMontageSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_blend_options")) return CmdMontageSetBlendOptions(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_sync_group")) return CmdMontageSetSyncGroup(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_list_skeleton_slots")) return CmdMontageListSkeletonSlots(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_skeleton_slot_group")) return CmdMontageSetSkeletonSlotGroup(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_rename_skeleton_slot")) return CmdMontageRenameSkeletonSlot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_skeleton_slot")) return CmdMontageRemoveSkeletonSlot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_notify_track")) return CmdMontageAddNotifyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_notify_track")) return CmdMontageRemoveNotifyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_notify")) return CmdMontageAddNotify(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_notify_state")) return CmdMontageAddNotifyState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_update_notify")) return CmdMontageUpdateNotify(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_notify")) return CmdMontageRemoveNotify(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_slot_track")) return CmdMontageAddSlotTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_rename_slot_track")) return CmdMontageRenameSlotTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_slot_track")) return CmdMontageRemoveSlotTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_segment")) return CmdMontageAddSegment(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_update_segment")) return CmdMontageUpdateSegment(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_segment")) return CmdMontageRemoveSegment(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_add_section")) return CmdMontageAddSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_rename_section")) return CmdMontageRenameSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_section_time")) return CmdMontageSetSectionTime(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_remove_section")) return CmdMontageRemoveSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("montage_set_next_section")) return CmdMontageSetNextSection(Ctx, OutData, OutError);

	OutError = TEXT("unknown_montage_command");
	return false;
}

bool FUeAgentHttpServer::CmdMontageListMontages(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Game");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	int32 Limit = 200;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 5000);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UAnimMontage::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& AssetData : AssetList)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdMontageCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentMontageOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentMontageOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	FString SourceAnimationPath;
	JsonTryGetString(Ctx.Params, TEXT("source_animation"), SourceAnimationPath);
	UAnimSequence* SourceAnimation = nullptr;
	if (!SourceAnimationPath.TrimStartAndEnd().IsEmpty())
	{
		SourceAnimation = UeAgentMontageOps::LoadAnimSequence(SourceAnimationPath);
		if (!SourceAnimation)
		{
			OutError = TEXT("source_animation_not_found_or_not_anim_sequence");
			return false;
		}
	}

	FString TargetSkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("target_skeleton"), TargetSkeletonPath);
	USkeleton* TargetSkeleton = nullptr;
	if (!TargetSkeletonPath.TrimStartAndEnd().IsEmpty())
	{
		TargetSkeleton = UeAgentMontageOps::LoadSkeleton(TargetSkeletonPath);
		if (!TargetSkeleton)
		{
			OutError = TEXT("target_skeleton_not_found");
			return false;
		}
	}
	if (!TargetSkeleton && SourceAnimation)
	{
		TargetSkeleton = SourceAnimation->GetSkeleton();
	}
	if (!TargetSkeleton)
	{
		OutError = TEXT("missing_target_skeleton_or_source_animation");
		return false;
	}

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
	USkeletalMesh* PreviewMesh = nullptr;
	if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		PreviewMesh = UeAgentMontageOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
		if (PreviewMesh->GetSkeleton() && !TargetSkeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_skeletal_mesh_incompatible");
			return false;
		}
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	if (!Factory)
	{
		OutError = TEXT("create_anim_montage_factory_failed");
		return false;
	}
	Factory->TargetSkeleton = TargetSkeleton;
	Factory->SourceAnimation = SourceAnimation;
	Factory->PreviewSkeletalMesh = PreviewMesh;

	UAnimMontage* Montage = Cast<UAnimMontage>(Factory->FactoryCreateNew(UAnimMontage::StaticClass(), Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!Montage)
	{
		OutError = TEXT("create_anim_montage_failed");
		return false;
	}

	FAssetRegistryModule::AssetCreated(Montage);
	Montage->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(Montage);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Montage, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), ObjectPath);
	OutData->SetStringField(TEXT("skeleton"), Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Montage->GetPreviewMesh() ? Montage->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("source_animation"), SourceAnimation ? SourceAnimation->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("slot_track_count"), Montage->SlotAnimTracks.Num());
	OutData->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdMontageGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}
	else
	{
		OutData->Values.Reset();
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> SlotTracks;
	int32 TotalSegmentCount = 0;
	for (int32 SlotIndex = 0; SlotIndex < Montage->SlotAnimTracks.Num(); ++SlotIndex)
	{
		const FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks[SlotIndex];
		TotalSegmentCount += SlotTrack.AnimTrack.AnimSegments.Num();
		SlotTracks.Add(MakeShared<FJsonValueObject>(UeAgentMontageOps::BuildSlotTrackJson(SlotTrack, SlotIndex, Montage->GetSkeleton())));
	}

	TArray<TSharedPtr<FJsonValue>> Sections;
	for (int32 SectionIndex = 0; SectionIndex < Montage->CompositeSections.Num(); ++SectionIndex)
	{
		Sections.Add(MakeShared<FJsonValueObject>(UeAgentMontageOps::BuildSectionJson(Montage->CompositeSections[SectionIndex], SectionIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> NotifyTracks;
#if WITH_EDITORONLY_DATA
	for (int32 TrackIndex = 0; TrackIndex < Montage->AnimNotifyTracks.Num(); ++TrackIndex)
	{
		NotifyTracks.Add(MakeShared<FJsonValueObject>(UeAgentMontageOps::BuildNotifyTrackJson(Montage->AnimNotifyTracks[TrackIndex], TrackIndex)));
	}
#endif

	TArray<TSharedPtr<FJsonValue>> Notifies;
	for (int32 NotifyIndex = 0; NotifyIndex < Montage->Notifies.Num(); ++NotifyIndex)
	{
		Notifies.Add(MakeShared<FJsonValueObject>(UeAgentMontageOps::BuildNotifyJson(Montage, Montage->Notifies[NotifyIndex], NotifyIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SyncMarkers;
	for (int32 MarkerIndex = 0; MarkerIndex < Montage->MarkerData.AuthoredSyncMarkers.Num(); ++MarkerIndex)
	{
		SyncMarkers.Add(MakeShared<FJsonValueObject>(UeAgentMontageOps::BuildSyncMarkerJson(Montage, Montage->MarkerData.AuthoredSyncMarkers[MarkerIndex], MarkerIndex)));
	}

	const bool bSyncSlotValid = Montage->SlotAnimTracks.IsValidIndex(Montage->SyncSlotIndex);
	const FString SyncSlotName = bSyncSlotValid ? Montage->SlotAnimTracks[Montage->SyncSlotIndex].SlotName.ToString() : FString();

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("skeleton"), Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Montage->GetPreviewMesh() ? Montage->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("first_animation_reference"), Montage->GetFirstAnimReference() ? Montage->GetFirstAnimReference()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("sequence_length"), Montage->GetPlayLength());
	OutData->SetNumberField(TEXT("slot_track_count"), Montage->SlotAnimTracks.Num());
	OutData->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
	OutData->SetNumberField(TEXT("segment_total_count"), TotalSegmentCount);
	OutData->SetNumberField(TEXT("blend_in_time"), Montage->BlendIn.GetBlendTime());
	OutData->SetNumberField(TEXT("blend_out_time"), Montage->BlendOut.GetBlendTime());
	OutData->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
	OutData->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
	OutData->SetStringField(TEXT("blend_mode_in"), UeAgentMontageOps::BlendModeToString(Montage->BlendModeIn));
	OutData->SetStringField(TEXT("blend_mode_out"), UeAgentMontageOps::BlendModeToString(Montage->BlendModeOut));
	OutData->SetStringField(TEXT("sync_group_name"), Montage->SyncGroup.ToString());
	OutData->SetBoolField(TEXT("sync_enabled"), !Montage->SyncGroup.IsNone());
	OutData->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);
	OutData->SetStringField(TEXT("sync_slot_name"), SyncSlotName);
	OutData->SetBoolField(TEXT("sync_slot_valid"), bSyncSlotValid);
	OutData->SetBoolField(TEXT("can_use_marker_sync"), Montage->CanUseMarkerSync());
	OutData->SetNumberField(TEXT("sync_marker_count"), Montage->MarkerData.AuthoredSyncMarkers.Num());
	OutData->SetArrayField(TEXT("slot_tracks"), SlotTracks);
	OutData->SetArrayField(TEXT("sections"), Sections);
	OutData->SetArrayField(TEXT("notify_tracks"), NotifyTracks);
	OutData->SetArrayField(TEXT("notifies"), Notifies);
	OutData->SetArrayField(TEXT("sync_markers"), SyncMarkers);
	return true;
}

bool FUeAgentHttpServer::CmdMontageExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMontageOps::NormalizeAssetPath(AssetPathInput);

	FUeAgentRequestContext InfoCtx;
	InfoCtx.RequestId = Ctx.RequestId + TEXT("_info");
	InfoCtx.Command = TEXT("montage_get_info");
	InfoCtx.Params = MakeShared<FJsonObject>();
	InfoCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);

	TSharedPtr<FJsonObject> InfoData;
	if (!CmdMontageGetInfo(InfoCtx, InfoData, OutError) || !InfoData.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.montage.v1"));
	Root->SetNumberField(TEXT("format_version"), 1);
	Root->SetStringField(TEXT("asset_kind"), TEXT("anim_montage"));
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	Root->SetStringField(TEXT("skeleton"), InfoData->GetStringField(TEXT("skeleton")));
	Root->SetStringField(TEXT("preview_skeletal_mesh"), InfoData->GetStringField(TEXT("preview_skeletal_mesh")));

	TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetNumberField(TEXT("blend_in_time"), InfoData->GetNumberField(TEXT("blend_in_time")));
	Settings->SetNumberField(TEXT("blend_out_time"), InfoData->GetNumberField(TEXT("blend_out_time")));
	Settings->SetNumberField(TEXT("blend_out_trigger_time"), InfoData->GetNumberField(TEXT("blend_out_trigger_time")));
	Settings->SetBoolField(TEXT("enable_auto_blend_out"), InfoData->GetBoolField(TEXT("enable_auto_blend_out")));
	Settings->SetStringField(TEXT("blend_mode_in"), InfoData->GetStringField(TEXT("blend_mode_in")));
	Settings->SetStringField(TEXT("blend_mode_out"), InfoData->GetStringField(TEXT("blend_mode_out")));
	Root->SetObjectField(TEXT("settings"), Settings);

	TSharedPtr<FJsonObject> Sync = MakeShared<FJsonObject>();
	Sync->SetStringField(TEXT("sync_group_name"), InfoData->GetStringField(TEXT("sync_group_name")));
	Sync->SetStringField(TEXT("sync_slot_name"), InfoData->GetStringField(TEXT("sync_slot_name")));
	Root->SetObjectField(TEXT("sync"), Sync);

	const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
	if (InfoData->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks)
	{
		Root->SetArrayField(TEXT("slot_tracks"), *SlotTracks);
	}
	else
	{
		Root->SetArrayField(TEXT("slot_tracks"), TArray<TSharedPtr<FJsonValue>>());
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (InfoData->TryGetArrayField(TEXT("sections"), Sections) && Sections)
	{
		Root->SetArrayField(TEXT("sections"), *Sections);
	}
	else
	{
		Root->SetArrayField(TEXT("sections"), TArray<TSharedPtr<FJsonValue>>());
	}

	const TArray<TSharedPtr<FJsonValue>>* NotifyTracks = nullptr;
	if (InfoData->TryGetArrayField(TEXT("notify_tracks"), NotifyTracks) && NotifyTracks)
	{
		Root->SetArrayField(TEXT("notify_tracks"), *NotifyTracks);
	}
	else
	{
		Root->SetArrayField(TEXT("notify_tracks"), TArray<TSharedPtr<FJsonValue>>());
	}

	const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
	if (InfoData->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies)
	{
		Root->SetArrayField(TEXT("notifies"), *Notifies);
	}
	else
	{
		Root->SetArrayField(TEXT("notifies"), TArray<TSharedPtr<FJsonValue>>());
	}

	const TArray<TSharedPtr<FJsonValue>>* SyncMarkers = nullptr;
	if (InfoData->TryGetArrayField(TEXT("sync_markers"), SyncMarkers) && SyncMarkers)
	{
		Root->SetArrayField(TEXT("sync_markers"), *SyncMarkers);
	}
	else
	{
		Root->SetArrayField(TEXT("sync_markers"), TArray<TSharedPtr<FJsonValue>>());
	}

	FUeAgentRequestContext SkeletonSlotsCtx;
	SkeletonSlotsCtx.RequestId = Ctx.RequestId + TEXT("_skeleton_slots");
	SkeletonSlotsCtx.Command = TEXT("montage_list_skeleton_slots");
	SkeletonSlotsCtx.Params = MakeShared<FJsonObject>();
	SkeletonSlotsCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> SkeletonSlotsData;
	FString SkeletonSlotsError;
	TArray<TSharedPtr<FJsonValue>> SkeletonSlotsArray;
	if (CmdMontageListSkeletonSlots(SkeletonSlotsCtx, SkeletonSlotsData, SkeletonSlotsError) && SkeletonSlotsData.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* SlotGroups = nullptr;
		if (SkeletonSlotsData->TryGetArrayField(TEXT("slot_groups"), SlotGroups) && SlotGroups)
		{
			for (const TSharedPtr<FJsonValue>& GroupValue : *SlotGroups)
			{
				const TSharedPtr<FJsonObject>* GroupObj = nullptr;
				if (!GroupValue.IsValid() || !GroupValue->TryGetObject(GroupObj) || !GroupObj || !GroupObj->IsValid())
				{
					continue;
				}

				FString GroupName;
				(*GroupObj)->TryGetStringField(TEXT("group_name"), GroupName);
				const TArray<TSharedPtr<FJsonValue>>* SlotNames = nullptr;
				if (!(*GroupObj)->TryGetArrayField(TEXT("slot_names"), SlotNames) || !SlotNames)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& SlotNameValue : *SlotNames)
				{
					const FString SlotName = SlotNameValue.IsValid() ? SlotNameValue->AsString() : FString();
					if (SlotName.IsEmpty())
					{
						continue;
					}
					TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
					SlotObj->SetStringField(TEXT("slot_name"), SlotName);
					SlotObj->SetStringField(TEXT("group_name"), GroupName);
					SkeletonSlotsArray.Add(MakeShared<FJsonValueObject>(SlotObj));
				}
			}
		}
	}
	Root->SetArrayField(TEXT("skeleton_slots"), SkeletonSlotsArray);

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentMontageOps::SaveJsonFile(OutputFile, Root, SavedOutputFile, OutError))
		{
			return false;
		}
	}

	OutData = Root;
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdMontageApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentMontageOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	const TSharedPtr<FJsonObject> SourceRoot = JsonRoot.IsValid() ? JsonRoot : Ctx.Params;
	if (!SourceRoot.IsValid())
	{
		OutError = TEXT("missing_json_payload");
		return false;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		SourceRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMontageOps::NormalizeAssetPath(AssetPathInput);

	UAnimMontage* ExistingMontage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!ExistingMontage)
	{
		bool bCreateIfMissing = true;
		JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
		if (!bCreateIfMissing)
		{
			OutError = TEXT("montage_not_found");
			return false;
		}

		FString SkeletonPath;
		if (!SourceRoot->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
		{
			OutError = TEXT("missing_skeleton_for_create");
			return false;
		}

		FUeAgentRequestContext CreateCtx;
		CreateCtx.RequestId = Ctx.RequestId + TEXT("_create");
		CreateCtx.Command = TEXT("montage_create");
		CreateCtx.Params = MakeShared<FJsonObject>();
		CreateCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CreateCtx.Params->SetStringField(TEXT("target_skeleton"), SkeletonPath);
		CreateCtx.Params->SetBoolField(TEXT("open_editor"), false);
		CreateCtx.Params->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!CmdMontageCreate(CreateCtx, CreateData, OutError))
		{
			return false;
		}
	}

	auto InvokeMontageSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& CommandData, FString& CommandError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("montage_json_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		return ExecuteMontageCommand(Command, SubCtx, CommandData, CommandError);
	};

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);

	int32 NotifyTracksApplied = 0;
	int32 NotifiesApplied = 0;
	int32 SlotsApplied = 0;
	int32 SegmentsApplied = 0;
	int32 SectionsApplied = 0;
	int32 SkeletonSlotsApplied = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	FString PreviewMeshPath;
	if (SourceRoot->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMeshPath) && !PreviewMeshPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("skeletal_mesh_path"), PreviewMeshPath);
		Params->SetBoolField(TEXT("save_after_set"), false);
		TSharedPtr<FJsonObject> Data;
		if (!InvokeMontageSubCommand(TEXT("montage_set_preview_mesh"), Params, Data, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> SettingsObj;
	if (JsonTryGetObject(SourceRoot, TEXT("settings"), SettingsObj) && SettingsObj.IsValid())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("save_after_set"), false);

		double NumberValue = 0.0;
		bool BoolValue = false;
		FString TextValue;
		if (SettingsObj->TryGetNumberField(TEXT("blend_in_time"), NumberValue)) Params->SetNumberField(TEXT("blend_in_time"), NumberValue);
		if (SettingsObj->TryGetNumberField(TEXT("blend_out_time"), NumberValue)) Params->SetNumberField(TEXT("blend_out_time"), NumberValue);
		if (SettingsObj->TryGetNumberField(TEXT("blend_out_trigger_time"), NumberValue)) Params->SetNumberField(TEXT("blend_out_trigger_time"), NumberValue);
		if (SettingsObj->TryGetBoolField(TEXT("enable_auto_blend_out"), BoolValue)) Params->SetBoolField(TEXT("enable_auto_blend_out"), BoolValue);
		if (SettingsObj->TryGetStringField(TEXT("blend_mode_in"), TextValue) && !TextValue.IsEmpty()) Params->SetStringField(TEXT("blend_mode_in"), TextValue);
		if (SettingsObj->TryGetStringField(TEXT("blend_mode_out"), TextValue) && !TextValue.IsEmpty()) Params->SetStringField(TEXT("blend_mode_out"), TextValue);

		TSharedPtr<FJsonObject> Data;
		if (!InvokeMontageSubCommand(TEXT("montage_set_blend_options"), Params, Data, OutError))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SkeletonSlots = nullptr;
	if (SourceRoot->TryGetArrayField(TEXT("skeleton_slots"), SkeletonSlots) && SkeletonSlots)
	{
		for (const TSharedPtr<FJsonValue>& SlotValue : *SkeletonSlots)
		{
			const TSharedPtr<FJsonObject>* SlotObj = nullptr;
			if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObj) || !SlotObj || !SlotObj->IsValid())
			{
				AddWarning(TEXT("skeleton_slot_invalid_item"), TEXT("Skipped skeleton slot entry because it is not a JSON object."));
				continue;
			}
			FString SlotName;
			FString GroupName;
			if (!(*SlotObj)->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
			{
				AddWarning(TEXT("skeleton_slot_missing_slot_name"), TEXT("Skipped skeleton slot entry because slot_name is missing."));
				continue;
			}
			if (!(*SlotObj)->TryGetStringField(TEXT("group_name"), GroupName) || GroupName.IsEmpty())
			{
				AddWarning(TEXT("skeleton_slot_missing_group_name"), FString::Printf(TEXT("Skipped skeleton slot '%s' because group_name is missing."), *SlotName));
				continue;
			}

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetStringField(TEXT("slot_name"), SlotName);
			Params->SetStringField(TEXT("group_name"), GroupName);
			Params->SetBoolField(TEXT("save_skeleton"), false);
			TSharedPtr<FJsonObject> Data;
			if (!InvokeMontageSubCommand(TEXT("montage_set_skeleton_slot_group"), Params, Data, OutError))
			{
				return false;
			}
			++SkeletonSlotsApplied;
		}
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found_after_create");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
	TArray<TSharedPtr<FJsonObject>> DesiredSlotTrackObjects;
	TSet<FString> DesiredSlotTrackNames;
	if (SourceRoot->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks)
	{
		for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
		{
			const TSharedPtr<FJsonObject>* SlotTrackObj = nullptr;
			if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObj) || !SlotTrackObj || !SlotTrackObj->IsValid())
			{
				AddWarning(TEXT("slot_track_invalid_item"), TEXT("Skipped slot track entry because it is not a JSON object."));
				continue;
			}

			FString SlotName;
			if (!(*SlotTrackObj)->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
			{
				AddWarning(TEXT("slot_track_missing_slot_name"), TEXT("Skipped slot track entry because slot_name is missing."));
				continue;
			}

			DesiredSlotTrackObjects.Add(*SlotTrackObj);
			DesiredSlotTrackNames.Add(SlotName);
		}
	}

	if (DesiredSlotTrackNames.Num() <= 0)
	{
		OutError = TEXT("slot_tracks_must_not_be_empty");
		return false;
	}

	TArray<FString> ExistingSlotTrackNames;
	for (const FSlotAnimationTrack& ExistingSlotTrack : Montage->SlotAnimTracks)
	{
		ExistingSlotTrackNames.Add(ExistingSlotTrack.SlotName.ToString());
	}

	for (const TSharedPtr<FJsonObject>& DesiredSlotTrackObj : DesiredSlotTrackObjects)
	{
		FString SlotName;
		if (!DesiredSlotTrackObj.IsValid() || !DesiredSlotTrackObj->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		{
			continue;
		}

		if (ExistingSlotTrackNames.Contains(SlotName))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AddSlotParams = MakeShared<FJsonObject>();
		AddSlotParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddSlotParams->SetStringField(TEXT("slot_name"), SlotName);
		AddSlotParams->SetBoolField(TEXT("save_after_add"), false);
		TSharedPtr<FJsonObject> AddSlotData;
		if (!InvokeMontageSubCommand(TEXT("montage_add_slot_track"), AddSlotParams, AddSlotData, OutError))
		{
			return false;
		}
		++SlotsApplied;
	}

	Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_reload_failed");
		return false;
	}

	TArray<FString> CurrentSlotTrackNames;
	for (const FSlotAnimationTrack& CurrentSlotTrack : Montage->SlotAnimTracks)
	{
		CurrentSlotTrackNames.Add(CurrentSlotTrack.SlotName.ToString());
	}
	for (const FString& CurrentSlotTrackName : CurrentSlotTrackNames)
	{
		if (DesiredSlotTrackNames.Contains(CurrentSlotTrackName))
		{
			continue;
		}

		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetStringField(TEXT("slot_name"), CurrentSlotTrackName);
		RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveData;
		if (!InvokeMontageSubCommand(TEXT("montage_remove_slot_track"), RemoveParams, RemoveData, OutError))
		{
			return false;
		}
	}

	Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_reload_failed");
		return false;
	}

	for (const TSharedPtr<FJsonObject>& DesiredSlotTrackObj : DesiredSlotTrackObjects)
	{
		FString SlotName;
		if (!DesiredSlotTrackObj.IsValid() || !DesiredSlotTrackObj->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		{
			continue;
		}

		const int32 DesiredSlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
		if (!Montage->SlotAnimTracks.IsValidIndex(DesiredSlotIndex))
		{
			OutError = TEXT("slot_track_rebuild_failed");
			return false;
		}

		for (int32 SegmentIndex = Montage->SlotAnimTracks[DesiredSlotIndex].AnimTrack.AnimSegments.Num() - 1; SegmentIndex >= 0; --SegmentIndex)
		{
			TSharedPtr<FJsonObject> RemoveSegmentParams = MakeShared<FJsonObject>();
			RemoveSegmentParams->SetStringField(TEXT("asset_path"), AssetPath);
			RemoveSegmentParams->SetStringField(TEXT("slot_name"), SlotName);
			RemoveSegmentParams->SetNumberField(TEXT("segment_index"), SegmentIndex);
			RemoveSegmentParams->SetBoolField(TEXT("save_after_remove"), false);
			TSharedPtr<FJsonObject> RemoveSegmentData;
			if (!InvokeMontageSubCommand(TEXT("montage_remove_segment"), RemoveSegmentParams, RemoveSegmentData, OutError))
			{
				return false;
			}
		}

		Montage = UeAgentMontageOps::LoadMontage(AssetPath);
		if (!Montage)
		{
			OutError = TEXT("montage_reload_failed");
			return false;
		}
	}

	for (const TSharedPtr<FJsonObject>& DesiredSlotTrackObj : DesiredSlotTrackObjects)
	{
		FString SlotName;
		if (!DesiredSlotTrackObj.IsValid() || !DesiredSlotTrackObj->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
		{
			const TSharedPtr<FJsonObject>* SlotTrackObj = nullptr;
			if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObj) || !SlotTrackObj || !SlotTrackObj->IsValid() || (*SlotTrackObj) != DesiredSlotTrackObj)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
			if (!(*SlotTrackObj)->TryGetArrayField(TEXT("segments"), Segments) || !Segments)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SegmentValue : *Segments)
			{
				const TSharedPtr<FJsonObject>* SegmentObj = nullptr;
				if (!SegmentValue.IsValid() || !SegmentValue->TryGetObject(SegmentObj) || !SegmentObj || !SegmentObj->IsValid())
				{
					AddWarning(TEXT("segment_invalid_item"), FString::Printf(TEXT("Skipped segment in slot '%s' because it is not a JSON object."), *SlotName));
					continue;
				}
				FString AnimationAsset;
				if (!(*SegmentObj)->TryGetStringField(TEXT("animation_asset"), AnimationAsset) || AnimationAsset.IsEmpty())
				{
					AddWarning(TEXT("segment_missing_animation_asset"), FString::Printf(TEXT("Skipped segment in slot '%s' because animation_asset is missing."), *SlotName));
					continue;
				}

				TSharedPtr<FJsonObject> AddSegmentParams = MakeShared<FJsonObject>();
				AddSegmentParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddSegmentParams->SetStringField(TEXT("slot_name"), SlotName);
				AddSegmentParams->SetStringField(TEXT("animation_asset"), AnimationAsset);
				AddSegmentParams->SetBoolField(TEXT("save_after_add"), false);

				double NumberValue = 0.0;
				if ((*SegmentObj)->TryGetNumberField(TEXT("start_pos"), NumberValue)) AddSegmentParams->SetNumberField(TEXT("start_pos"), NumberValue);
				if ((*SegmentObj)->TryGetNumberField(TEXT("anim_start_time"), NumberValue)) AddSegmentParams->SetNumberField(TEXT("anim_start_time"), NumberValue);
				if ((*SegmentObj)->TryGetNumberField(TEXT("anim_end_time"), NumberValue)) AddSegmentParams->SetNumberField(TEXT("anim_end_time"), NumberValue);
				if ((*SegmentObj)->TryGetNumberField(TEXT("play_rate"), NumberValue)) AddSegmentParams->SetNumberField(TEXT("play_rate"), NumberValue);
				if ((*SegmentObj)->TryGetNumberField(TEXT("loop_count"), NumberValue)) AddSegmentParams->SetNumberField(TEXT("loop_count"), NumberValue);

				TSharedPtr<FJsonObject> AddSegmentData;
				if (!InvokeMontageSubCommand(TEXT("montage_add_segment"), AddSegmentParams, AddSegmentData, OutError))
				{
					return false;
				}
				++SegmentsApplied;
			}
		}
	}

	Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_reload_failed");
		return false;
	}
	while (Montage->CompositeSections.Num() > 1)
	{
		const FString SectionName = Montage->CompositeSections.Last().SectionName.ToString();
		TSharedPtr<FJsonObject> RemoveSectionParams = MakeShared<FJsonObject>();
		RemoveSectionParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveSectionParams->SetStringField(TEXT("section_name"), SectionName);
		RemoveSectionParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveSectionData;
		if (!InvokeMontageSubCommand(TEXT("montage_remove_section"), RemoveSectionParams, RemoveSectionData, OutError))
		{
			return false;
		}
		Montage = UeAgentMontageOps::LoadMontage(AssetPath);
		if (!Montage)
		{
			OutError = TEXT("montage_reload_failed");
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (SourceRoot->TryGetArrayField(TEXT("sections"), Sections) && Sections)
	{
		bool bDefaultSectionConfigured = false;
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				AddWarning(TEXT("section_invalid_item"), TEXT("Skipped section entry because it is not a JSON object."));
				continue;
			}
			FString SectionName;
			if (!(*SectionObj)->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
			{
				AddWarning(TEXT("section_missing_section_name"), TEXT("Skipped section entry because section_name is missing."));
				continue;
			}

			double TimeSeconds = 0.0;
			(*SectionObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds);
			if (SectionName.Equals(TEXT("Default"), ESearchCase::CaseSensitive))
			{
				TSharedPtr<FJsonObject> DefaultTimeParams = MakeShared<FJsonObject>();
				DefaultTimeParams->SetStringField(TEXT("asset_path"), AssetPath);
				DefaultTimeParams->SetStringField(TEXT("section_name"), SectionName);
				DefaultTimeParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
				DefaultTimeParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> DefaultTimeData;
				if (!InvokeMontageSubCommand(TEXT("montage_set_section_time"), DefaultTimeParams, DefaultTimeData, OutError))
				{
					return false;
				}
				bDefaultSectionConfigured = true;
			}
			else
			{
				TSharedPtr<FJsonObject> AddSectionParams = MakeShared<FJsonObject>();
				AddSectionParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddSectionParams->SetStringField(TEXT("section_name"), SectionName);
				AddSectionParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
				AddSectionParams->SetBoolField(TEXT("save_after_add"), false);
				TSharedPtr<FJsonObject> AddSectionData;
				if (!InvokeMontageSubCommand(TEXT("montage_add_section"), AddSectionParams, AddSectionData, OutError))
				{
					return false;
				}
			}
			++SectionsApplied;
		}

		if (!bDefaultSectionConfigured)
		{
			TSharedPtr<FJsonObject> DefaultTimeParams = MakeShared<FJsonObject>();
			DefaultTimeParams->SetStringField(TEXT("asset_path"), AssetPath);
			DefaultTimeParams->SetStringField(TEXT("section_name"), TEXT("Default"));
			DefaultTimeParams->SetNumberField(TEXT("time_seconds"), 0.0);
			DefaultTimeParams->SetBoolField(TEXT("save_after_set"), false);
			TSharedPtr<FJsonObject> DefaultTimeData;
			if (!InvokeMontageSubCommand(TEXT("montage_set_section_time"), DefaultTimeParams, DefaultTimeData, OutError))
			{
				return false;
			}
		}

		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				continue;
			}
			FString SectionName;
			FString NextSectionName;
			if (!(*SectionObj)->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
			{
				continue;
			}
			if (!(*SectionObj)->TryGetStringField(TEXT("next_section_name"), NextSectionName))
			{
				NextSectionName.Reset();
			}
			TSharedPtr<FJsonObject> NextParams = MakeShared<FJsonObject>();
			NextParams->SetStringField(TEXT("asset_path"), AssetPath);
			NextParams->SetStringField(TEXT("section_name"), SectionName);
			if (NextSectionName.IsEmpty())
			{
				NextParams->SetBoolField(TEXT("clear_next_section"), true);
			}
			else
			{
				NextParams->SetStringField(TEXT("next_section_name"), NextSectionName);
			}
			NextParams->SetBoolField(TEXT("save_after_set"), false);
			TSharedPtr<FJsonObject> NextData;
			if (!InvokeMontageSubCommand(TEXT("montage_set_next_section"), NextParams, NextData, OutError))
			{
				return false;
			}
		}
	}

#if WITH_EDITORONLY_DATA
	Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_reload_failed");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* NotifyTracks = nullptr;
	TArray<TSharedPtr<FJsonObject>> DesiredNotifyTrackObjects;
	TSet<FString> DesiredNotifyTrackNames;
	if (SourceRoot->TryGetArrayField(TEXT("notify_tracks"), NotifyTracks) && NotifyTracks)
	{
		for (const TSharedPtr<FJsonValue>& NotifyTrackValue : *NotifyTracks)
		{
			const TSharedPtr<FJsonObject>* NotifyTrackObj = nullptr;
			if (!NotifyTrackValue.IsValid() || !NotifyTrackValue->TryGetObject(NotifyTrackObj) || !NotifyTrackObj || !NotifyTrackObj->IsValid())
			{
				AddWarning(TEXT("notify_track_invalid_item"), TEXT("Skipped notify track entry because it is not a JSON object."));
				continue;
			}
			FString TrackName;
			if (!(*NotifyTrackObj)->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
			{
				AddWarning(TEXT("notify_track_missing_track_name"), TEXT("Skipped notify track entry because track_name is missing."));
				continue;
			}
			DesiredNotifyTrackObjects.Add(*NotifyTrackObj);
			DesiredNotifyTrackNames.Add(TrackName);
		}
	}

	if (DesiredNotifyTrackNames.Num() > 0)
	{
		TArray<FString> ExistingNotifyTrackNames;
		for (const FAnimNotifyTrack& ExistingNotifyTrack : Montage->AnimNotifyTracks)
		{
			ExistingNotifyTrackNames.Add(ExistingNotifyTrack.TrackName.ToString());
		}

		for (const TSharedPtr<FJsonObject>& DesiredNotifyTrackObj : DesiredNotifyTrackObjects)
		{
			FString TrackName;
			if (!DesiredNotifyTrackObj.IsValid() || !DesiredNotifyTrackObj->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
			{
				continue;
			}

			if (ExistingNotifyTrackNames.Contains(TrackName))
			{
				continue;
			}

			TSharedPtr<FJsonObject> AddTrackParams = MakeShared<FJsonObject>();
			AddTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
			AddTrackParams->SetStringField(TEXT("track_name"), TrackName);
			AddTrackParams->SetBoolField(TEXT("save_after_add"), false);

			const TSharedPtr<FJsonObject>* LinearColorObj = nullptr;
			if (DesiredNotifyTrackObj->TryGetObjectField(TEXT("track_color_linear"), LinearColorObj) && LinearColorObj && LinearColorObj->IsValid())
			{
				AddTrackParams->SetObjectField(TEXT("track_color"), *LinearColorObj);
			}

			TSharedPtr<FJsonObject> AddTrackData;
			if (!InvokeMontageSubCommand(TEXT("montage_add_notify_track"), AddTrackParams, AddTrackData, OutError))
			{
				return false;
			}
			++NotifyTracksApplied;
		}

		Montage = UeAgentMontageOps::LoadMontage(AssetPath);
		if (!Montage)
		{
			OutError = TEXT("montage_reload_failed");
			return false;
		}

		TArray<FString> CurrentNotifyTrackNames;
		for (const FAnimNotifyTrack& CurrentNotifyTrack : Montage->AnimNotifyTracks)
		{
			CurrentNotifyTrackNames.Add(CurrentNotifyTrack.TrackName.ToString());
		}
		for (const FString& CurrentNotifyTrackName : CurrentNotifyTrackNames)
		{
			if (DesiredNotifyTrackNames.Contains(CurrentNotifyTrackName))
			{
				continue;
			}

			TSharedPtr<FJsonObject> RemoveTrackParams = MakeShared<FJsonObject>();
			RemoveTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
			RemoveTrackParams->SetStringField(TEXT("track_name"), CurrentNotifyTrackName);
			RemoveTrackParams->SetBoolField(TEXT("save_after_remove"), false);
			TSharedPtr<FJsonObject> RemoveTrackData;
			if (!InvokeMontageSubCommand(TEXT("montage_remove_notify_track"), RemoveTrackParams, RemoveTrackData, OutError))
			{
				return false;
			}
		}
	}
#endif

	Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_reload_failed");
		return false;
	}
	while (Montage->Notifies.Num() > 0)
	{
		const int32 NotifyIndex = Montage->Notifies.Num() - 1;
		TSharedPtr<FJsonObject> RemoveNotifyParams = MakeShared<FJsonObject>();
		RemoveNotifyParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveNotifyParams->SetNumberField(TEXT("notify_index"), NotifyIndex);
		RemoveNotifyParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveNotifyData;
		if (!InvokeMontageSubCommand(TEXT("montage_remove_notify"), RemoveNotifyParams, RemoveNotifyData, OutError))
		{
			return false;
		}
		Montage = UeAgentMontageOps::LoadMontage(AssetPath);
		if (!Montage)
		{
			OutError = TEXT("montage_reload_failed");
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
	if (SourceRoot->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies)
	{
		for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
		{
			const TSharedPtr<FJsonObject>* NotifyObj = nullptr;
			if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObj) || !NotifyObj || !NotifyObj->IsValid())
			{
				AddWarning(TEXT("notify_invalid_item"), TEXT("Skipped notify entry because it is not a JSON object."));
				continue;
			}

			FString TrackName;
			(*NotifyObj)->TryGetStringField(TEXT("track_name"), TrackName);
			double TimeSeconds = 0.0;
			(*NotifyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds);
			bool bIsState = false;
			(*NotifyObj)->TryGetBoolField(TEXT("is_state"), bIsState);
			const FString Kind = (*NotifyObj)->HasField(TEXT("kind")) ? (*NotifyObj)->GetStringField(TEXT("kind")) : (bIsState ? TEXT("notify_state") : TEXT("notify"));

			TSharedPtr<FJsonObject> AddNotifyParams = MakeShared<FJsonObject>();
			AddNotifyParams->SetStringField(TEXT("asset_path"), AssetPath);
			if (!TrackName.IsEmpty())
			{
				AddNotifyParams->SetStringField(TEXT("track_name"), TrackName);
			}
			AddNotifyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
			AddNotifyParams->SetBoolField(TEXT("save_after_add"), false);

			FString TickType;
			if ((*NotifyObj)->TryGetStringField(TEXT("tick_type"), TickType) && !TickType.IsEmpty())
			{
				AddNotifyParams->SetStringField(TEXT("tick_type"), TickType);
			}
			else
			{
				bool bBranchingPoint = false;
				if ((*NotifyObj)->TryGetBoolField(TEXT("branching_point"), bBranchingPoint))
				{
					AddNotifyParams->SetBoolField(TEXT("branching_point"), bBranchingPoint);
				}
			}
			double NumberValue = 0.0;
			bool BoolValue = false;
			FString TextValue;
			if ((*NotifyObj)->TryGetNumberField(TEXT("trigger_weight_threshold"), NumberValue)) AddNotifyParams->SetNumberField(TEXT("trigger_weight_threshold"), NumberValue);
			if ((*NotifyObj)->TryGetNumberField(TEXT("notify_trigger_chance"), NumberValue)) AddNotifyParams->SetNumberField(TEXT("notify_trigger_chance"), NumberValue);
			if ((*NotifyObj)->TryGetStringField(TEXT("notify_filter_type"), TextValue) && !TextValue.IsEmpty()) AddNotifyParams->SetStringField(TEXT("notify_filter_type"), TextValue);
			if ((*NotifyObj)->TryGetNumberField(TEXT("notify_filter_lod"), NumberValue)) AddNotifyParams->SetNumberField(TEXT("notify_filter_lod"), NumberValue);
			if ((*NotifyObj)->TryGetBoolField(TEXT("can_be_filtered_via_request"), BoolValue)) AddNotifyParams->SetBoolField(TEXT("can_be_filtered_via_request"), BoolValue);
			if ((*NotifyObj)->TryGetBoolField(TEXT("trigger_on_dedicated_server"), BoolValue)) AddNotifyParams->SetBoolField(TEXT("trigger_on_dedicated_server"), BoolValue);
			if ((*NotifyObj)->TryGetBoolField(TEXT("trigger_on_follower"), BoolValue)) AddNotifyParams->SetBoolField(TEXT("trigger_on_follower"), BoolValue);

			const TSharedPtr<FJsonObject>* NotifyColorObj = nullptr;
			if ((*NotifyObj)->TryGetObjectField(TEXT("notify_color_linear"), NotifyColorObj) && NotifyColorObj && NotifyColorObj->IsValid())
			{
				AddNotifyParams->SetObjectField(TEXT("notify_color"), *NotifyColorObj);
			}

			TSharedPtr<FJsonObject> AddNotifyData;
			if (Kind.Equals(TEXT("notify_state"), ESearchCase::IgnoreCase) || bIsState)
			{
				FString NotifyStateClass;
				if (!(*NotifyObj)->TryGetStringField(TEXT("notify_state_class"), NotifyStateClass) || NotifyStateClass.IsEmpty())
				{
					AddWarning(TEXT("notify_state_missing_class"), TEXT("Skipped notify state because notify_state_class is missing."));
					continue;
				}
				AddNotifyParams->SetStringField(TEXT("notify_state_class"), NotifyStateClass);
				if ((*NotifyObj)->TryGetNumberField(TEXT("duration_seconds"), NumberValue)) AddNotifyParams->SetNumberField(TEXT("duration_seconds"), NumberValue);
				if (!InvokeMontageSubCommand(TEXT("montage_add_notify_state"), AddNotifyParams, AddNotifyData, OutError))
				{
					return false;
				}
			}
			else
			{
				FString NotifyName;
				if ((*NotifyObj)->TryGetStringField(TEXT("notify_name"), NotifyName) && !NotifyName.IsEmpty())
				{
					AddNotifyParams->SetStringField(TEXT("notify_name"), NotifyName);
				}
				else if ((*NotifyObj)->TryGetStringField(TEXT("notify_class"), TextValue) && !TextValue.IsEmpty())
				{
					AddNotifyParams->SetStringField(TEXT("notify_class"), TextValue);
				}
				else
				{
					AddWarning(TEXT("notify_missing_name_or_class"), TEXT("Skipped notify because neither notify_name nor notify_class is present."));
					continue;
				}
				if (!InvokeMontageSubCommand(TEXT("montage_add_notify"), AddNotifyParams, AddNotifyData, OutError))
				{
					return false;
				}
			}
			++NotifiesApplied;
		}
	}

	TSharedPtr<FJsonObject> SyncObj;
	if (JsonTryGetObject(SourceRoot, TEXT("sync"), SyncObj) && SyncObj.IsValid())
	{
		TSharedPtr<FJsonObject> SyncParams = MakeShared<FJsonObject>();
		SyncParams->SetStringField(TEXT("asset_path"), AssetPath);
		SyncParams->SetBoolField(TEXT("save_after_set"), false);
		FString SyncGroupName;
		FString SyncSlotName;
		if (SyncObj->TryGetStringField(TEXT("sync_group_name"), SyncGroupName))
		{
			SyncParams->SetStringField(TEXT("sync_group_name"), SyncGroupName);
		}
		if (SyncObj->TryGetStringField(TEXT("sync_slot_name"), SyncSlotName) && !SyncSlotName.IsEmpty())
		{
			SyncParams->SetStringField(TEXT("sync_slot_name"), SyncSlotName);
		}
		TSharedPtr<FJsonObject> SyncData;
		if (!InvokeMontageSubCommand(TEXT("montage_set_sync_group"), SyncParams, SyncData, OutError))
		{
			return false;
		}
	}

	if (bSaveAfterApply)
	{
		Montage = UeAgentMontageOps::LoadMontage(AssetPath);
		if (!Montage)
		{
			OutError = TEXT("montage_reload_failed");
			return false;
		}
		if (!UeAgentMontageOps::SaveAssetPackage(Montage, OutError))
		{
			return false;
		}
	}

	FUeAgentRequestContext ExportCtx;
	ExportCtx.RequestId = Ctx.RequestId + TEXT("_export");
	ExportCtx.Command = TEXT("montage_export_json");
	ExportCtx.Params = MakeShared<FJsonObject>();
	ExportCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
	if (!CmdMontageExportJson(ExportCtx, OutData, OutError) || !OutData.IsValid())
	{
		return false;
	}
	OutData->SetNumberField(TEXT("skeleton_slots_applied"), SkeletonSlotsApplied);
	OutData->SetNumberField(TEXT("slot_tracks_applied"), SlotsApplied);
	OutData->SetNumberField(TEXT("segments_applied"), SegmentsApplied);
	OutData->SetNumberField(TEXT("sections_applied"), SectionsApplied);
	OutData->SetNumberField(TEXT("notify_tracks_applied"), NotifyTracksApplied);
	OutData->SetNumberField(TEXT("notifies_applied"), NotifiesApplied);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	USkeletalMesh* PreviewMesh = nullptr;
	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);
	if (!bClearPreviewMesh)
	{
		if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		PreviewMesh = UeAgentMontageOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
		if (Montage->GetSkeleton() && PreviewMesh->GetSkeleton() && !Montage->GetSkeleton()->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_mesh_skeleton_incompatible");
			return false;
		}
	}

	Montage->Modify();
	Montage->SetPreviewMesh(PreviewMesh);
	Montage->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Montage, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Montage->GetPreviewMesh() ? Montage->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetBlendOptions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	bool bChanged = false;
	Montage->Modify();

	double BlendInTime = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("blend_in_time"), BlendInTime))
	{
		const float NewValue = static_cast<float>(FMath::Max(0.0, BlendInTime));
		if (!FMath::IsNearlyEqual(Montage->BlendIn.GetBlendTime(), NewValue))
		{
			Montage->BlendIn.SetBlendTime(NewValue);
			bChanged = true;
		}
	}

	double BlendOutTime = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("blend_out_time"), BlendOutTime))
	{
		const float NewValue = static_cast<float>(FMath::Max(0.0, BlendOutTime));
		if (!FMath::IsNearlyEqual(Montage->BlendOut.GetBlendTime(), NewValue))
		{
			Montage->BlendOut.SetBlendTime(NewValue);
			bChanged = true;
		}
	}

	double BlendOutTriggerTime = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("blend_out_trigger_time"), BlendOutTriggerTime))
	{
		const float NewValue = static_cast<float>(BlendOutTriggerTime);
		if (!FMath::IsNearlyEqual(Montage->BlendOutTriggerTime, NewValue))
		{
			Montage->BlendOutTriggerTime = NewValue;
			bChanged = true;
		}
	}

	bool bEnableAutoBlendOut = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("enable_auto_blend_out"), bEnableAutoBlendOut) && Montage->bEnableAutoBlendOut != bEnableAutoBlendOut)
	{
		Montage->bEnableAutoBlendOut = bEnableAutoBlendOut;
		bChanged = true;
	}

	FString BlendModeInText;
	if (JsonTryGetString(Ctx.Params, TEXT("blend_mode_in"), BlendModeInText) && !BlendModeInText.TrimStartAndEnd().IsEmpty())
	{
		EMontageBlendMode NewMode = Montage->BlendModeIn;
		if (!UeAgentMontageOps::ParseBlendMode(BlendModeInText, NewMode))
		{
			OutError = TEXT("invalid_blend_mode_in");
			return false;
		}
		if (Montage->BlendModeIn != NewMode)
		{
			Montage->BlendModeIn = NewMode;
			bChanged = true;
		}
	}

	FString BlendModeOutText;
	if (JsonTryGetString(Ctx.Params, TEXT("blend_mode_out"), BlendModeOutText) && !BlendModeOutText.TrimStartAndEnd().IsEmpty())
	{
		EMontageBlendMode NewMode = Montage->BlendModeOut;
		if (!UeAgentMontageOps::ParseBlendMode(BlendModeOutText, NewMode))
		{
			OutError = TEXT("invalid_blend_mode_out");
			return false;
		}
		if (Montage->BlendModeOut != NewMode)
		{
			Montage->BlendModeOut = NewMode;
			bChanged = true;
		}
	}

	if (bChanged)
	{
		Montage->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bChanged && bSaveAfterSet)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Montage, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetNumberField(TEXT("blend_in_time"), Montage->BlendIn.GetBlendTime());
	OutData->SetNumberField(TEXT("blend_out_time"), Montage->BlendOut.GetBlendTime());
	OutData->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
	OutData->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
	OutData->SetStringField(TEXT("blend_mode_in"), UeAgentMontageOps::BlendModeToString(Montage->BlendModeIn));
	OutData->SetStringField(TEXT("blend_mode_out"), UeAgentMontageOps::BlendModeToString(Montage->BlendModeOut));
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bChanged && bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetSyncGroup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	FString SyncGroupNameText;
	JsonTryGetString(Ctx.Params, TEXT("sync_group_name"), SyncGroupNameText);
	SyncGroupNameText = SyncGroupNameText.TrimStartAndEnd();
	const bool bClearSyncGroup = SyncGroupNameText.IsEmpty();

	int32 SyncSlotIndex = INDEX_NONE;
	bool bExplicitSlotSelection = false;
	if (!UeAgentMontageOps::ResolveMontageSyncSlotIndex(Montage, Ctx.Params, SyncSlotIndex, bExplicitSlotSelection, OutError))
	{
		return false;
	}

	const FString PreviousSyncGroupName = Montage->SyncGroup.ToString();
	const int32 PreviousSyncSlotIndex = Montage->SyncSlotIndex;
	const FString PreviousSyncSlotName = Montage->SlotAnimTracks.IsValidIndex(Montage->SyncSlotIndex) ? Montage->SlotAnimTracks[Montage->SyncSlotIndex].SlotName.ToString() : FString();

	Montage->Modify();
	Montage->SyncGroup = bClearSyncGroup ? NAME_None : FName(*SyncGroupNameText);
	Montage->SyncSlotIndex = SyncSlotIndex;

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterSet, OutError))
	{
		return false;
	}

	const bool bSyncSlotValid = Montage->SlotAnimTracks.IsValidIndex(Montage->SyncSlotIndex);
	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("previous_sync_group_name"), PreviousSyncGroupName);
	OutData->SetStringField(TEXT("previous_sync_slot_name"), PreviousSyncSlotName);
	OutData->SetNumberField(TEXT("previous_sync_slot_index"), PreviousSyncSlotIndex);
	OutData->SetStringField(TEXT("sync_group_name"), Montage->SyncGroup.ToString());
	OutData->SetBoolField(TEXT("sync_enabled"), !Montage->SyncGroup.IsNone());
	OutData->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);
	OutData->SetStringField(TEXT("sync_slot_name"), bSyncSlotValid ? Montage->SlotAnimTracks[Montage->SyncSlotIndex].SlotName.ToString() : TEXT(""));
	OutData->SetBoolField(TEXT("sync_slot_valid"), bSyncSlotValid);
	OutData->SetBoolField(TEXT("explicit_slot_selection"), bExplicitSlotSelection);
	OutData->SetBoolField(TEXT("can_use_marker_sync"), Montage->CanUseMarkerSync());
	OutData->SetNumberField(TEXT("sync_marker_count"), Montage->MarkerData.AuthoredSyncMarkers.Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageListSkeletonSlots(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}
	else
	{
		OutData->Values.Reset();
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);

	FString SourceField;
	USkeleton* Skeleton = UeAgentMontageOps::ResolveSkeleton(AssetPath, SkeletonPath, SourceField);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> SlotGroupsJson;
	for (const FAnimSlotGroup& SlotGroup : Skeleton->GetSlotGroups())
	{
		TSharedPtr<FJsonObject> GroupObject = MakeShared<FJsonObject>();
		GroupObject->SetStringField(TEXT("group_name"), SlotGroup.GroupName.ToString());

		TArray<TSharedPtr<FJsonValue>> SlotNamesJson;
		for (const FName& SlotName : SlotGroup.SlotNames)
		{
			SlotNamesJson.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
		}
		GroupObject->SetArrayField(TEXT("slot_names"), SlotNamesJson);
		GroupObject->SetNumberField(TEXT("slot_count"), SlotNamesJson.Num());
		SlotGroupsJson.Add(MakeShared<FJsonValueObject>(GroupObject));
	}

	OutData->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("source_field"), SourceField);
	OutData->SetNumberField(TEXT("slot_group_count"), SlotGroupsJson.Num());
	OutData->SetArrayField(TEXT("slot_groups"), SlotGroupsJson);
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetSkeletonSlotGroup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SlotNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotNameText) || SlotNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotNameText = SlotNameText.TrimStartAndEnd();

	FString GroupNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("group_name"), GroupNameText) || GroupNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_group_name");
		return false;
	}
	GroupNameText = GroupNameText.TrimStartAndEnd();

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);

	FString SourceField;
	USkeleton* Skeleton = UeAgentMontageOps::ResolveSkeleton(AssetPath, SkeletonPath, SourceField);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FName SlotName(*SlotNameText);
	const FName GroupName(*GroupNameText);
	const bool bSlotAlreadyRegistered = Skeleton->ContainsSlotName(SlotName);
	const bool bGroupAlreadyExists = Skeleton->FindAnimSlotGroup(GroupName) != nullptr;

	Skeleton->Modify();
	Skeleton->RegisterSlotNode(SlotName);
	Skeleton->SetSlotGroupName(SlotName, GroupName);
	Skeleton->MarkPackageDirty();

	bool bSaveSkeleton = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_skeleton"), bSaveSkeleton);
	if (bSaveSkeleton)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Skeleton, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("source_field"), SourceField);
	OutData->SetStringField(TEXT("slot_name"), SlotName.ToString());
	OutData->SetStringField(TEXT("group_name"), Skeleton->GetSlotGroupName(SlotName).ToString());
	OutData->SetBoolField(TEXT("slot_already_registered"), bSlotAlreadyRegistered);
	OutData->SetBoolField(TEXT("group_already_exists"), bGroupAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveSkeleton);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRenameSkeletonSlot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SlotNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotNameText) || SlotNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotNameText = SlotNameText.TrimStartAndEnd();

	FString NewSlotNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_slot_name"), NewSlotNameText) || NewSlotNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_new_slot_name");
		return false;
	}
	NewSlotNameText = NewSlotNameText.TrimStartAndEnd();

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);

	FString SourceField;
	USkeleton* Skeleton = UeAgentMontageOps::ResolveSkeleton(AssetPath, SkeletonPath, SourceField);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FName SlotName(*SlotNameText);
	const FName NewSlotName(*NewSlotNameText);
	if (!Skeleton->ContainsSlotName(SlotName))
	{
		OutError = TEXT("skeleton_slot_not_found");
		return false;
	}
	if (Skeleton->ContainsSlotName(NewSlotName))
	{
		OutError = TEXT("new_slot_name_already_exists");
		return false;
	}

	Skeleton->Modify();
	Skeleton->RenameSlotName(SlotName, NewSlotName);
	Skeleton->MarkPackageDirty();

	bool bSaveSkeleton = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_skeleton"), bSaveSkeleton);
	if (bSaveSkeleton)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Skeleton, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("source_field"), SourceField);
	OutData->SetStringField(TEXT("slot_name"), SlotName.ToString());
	OutData->SetStringField(TEXT("new_slot_name"), NewSlotName.ToString());
	OutData->SetStringField(TEXT("group_name"), Skeleton->GetSlotGroupName(NewSlotName).ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveSkeleton);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveSkeletonSlot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SlotNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotNameText) || SlotNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotNameText = SlotNameText.TrimStartAndEnd();

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);

	FString SourceField;
	USkeleton* Skeleton = UeAgentMontageOps::ResolveSkeleton(AssetPath, SkeletonPath, SourceField);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FName SlotName(*SlotNameText);
	if (!Skeleton->ContainsSlotName(SlotName))
	{
		OutError = TEXT("skeleton_slot_not_found");
		return false;
	}

	const FString PreviousGroupName = Skeleton->GetSlotGroupName(SlotName).ToString();
	Skeleton->Modify();
	Skeleton->RemoveSlotName(SlotName);
	Skeleton->MarkPackageDirty();

	bool bSaveSkeleton = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_skeleton"), bSaveSkeleton);
	if (bSaveSkeleton)
	{
		if (!UeAgentMontageOps::SaveAssetPackage(Skeleton, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("source_field"), SourceField);
	OutData->SetStringField(TEXT("removed_slot_name"), SlotName.ToString());
	OutData->SetStringField(TEXT("previous_group_name"), PreviousGroupName);
	OutData->SetBoolField(TEXT("saved"), bSaveSkeleton);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddNotifyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString TrackName;
	if (!JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackName) || TrackName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_track_name");
		return false;
	}
	TrackName = TrackName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	FLinearColor TrackColor = FLinearColor::White;
	const bool bHasTrackColor = UeAgentMontageOps::TryParseLinearColorField(Ctx.Params, TEXT("track_color"), TrackColor);

	const int32 ExistingIndex = UeAgentMontageOps::FindNotifyTrackIndexByName(Montage, TrackName);
	if (ExistingIndex != INDEX_NONE)
	{
		bool bChanged = false;
#if WITH_EDITORONLY_DATA
		if (bHasTrackColor && Montage->AnimNotifyTracks.IsValidIndex(ExistingIndex))
		{
			Montage->Modify();
			Montage->AnimNotifyTracks[ExistingIndex].TrackColor = TrackColor;
			bChanged = true;
		}
#endif
		bool bSaveAfterAdd = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
		if (bChanged && !UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
		OutData->SetStringField(TEXT("track_name"), TrackName);
		OutData->SetNumberField(TEXT("track_index"), ExistingIndex);
		OutData->SetBoolField(TEXT("changed"), bChanged);
		if (bHasTrackColor)
		{
			OutData->SetObjectField(TEXT("track_color_linear"), UeAgentMontageOps::BuildLinearColorJson(TrackColor));
		}
		OutData->SetBoolField(TEXT("saved"), bChanged ? bSaveAfterAdd : false);
		return true;
	}

	int32 TrackIndex = INDEX_NONE;
	Montage->Modify();
	if (!UeAgentMontageOps::EnsureNotifyTrack(Montage, FName(*TrackName), TrackIndex))
	{
		OutError = TEXT("create_notify_track_failed");
		return false;
	}
#if WITH_EDITORONLY_DATA
	if (bHasTrackColor && Montage->AnimNotifyTracks.IsValidIndex(TrackIndex))
	{
		Montage->AnimNotifyTracks[TrackIndex].TrackColor = TrackColor;
	}
#endif

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("track_name"), TrackName);
	OutData->SetNumberField(TEXT("track_index"), TrackIndex);
	OutData->SetBoolField(TEXT("changed"), true);
	if (bHasTrackColor)
	{
		OutData->SetObjectField(TEXT("track_color_linear"), UeAgentMontageOps::BuildLinearColorJson(TrackColor));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveNotifyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString TrackName;
	if (!JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackName) || TrackName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_track_name");
		return false;
	}
	TrackName = TrackName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 TrackIndex = UeAgentMontageOps::FindNotifyTrackIndexByName(Montage, TrackName);
	if (TrackIndex == INDEX_NONE)
	{
		OutError = TEXT("notify_track_not_found");
		return false;
	}

#if WITH_EDITORONLY_DATA
	if (Montage->AnimNotifyTracks.Num() <= 1)
	{
		OutError = TEXT("cannot_remove_last_notify_track");
		return false;
	}
#endif

	for (const FAnimNotifyEvent& Notify : Montage->Notifies)
	{
		if (Notify.TrackIndex == TrackIndex)
		{
			OutError = TEXT("notify_track_not_empty");
			return false;
		}
	}

	Montage->Modify();
#if WITH_EDITORONLY_DATA
	Montage->AnimNotifyTracks.RemoveAt(TrackIndex);
#endif
	for (FAnimNotifyEvent& Notify : Montage->Notifies)
	{
		if (Notify.TrackIndex > TrackIndex)
		{
			Notify.TrackIndex -= 1;
		}
	}

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("removed_track_name"), TrackName);
	OutData->SetNumberField(TEXT("removed_track_index"), TrackIndex);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddNotify(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	FString NotifyName;
	JsonTryGetString(Ctx.Params, TEXT("notify_name"), NotifyName);
	NotifyName = NotifyName.TrimStartAndEnd();

	FString NotifyClassPath;
	JsonTryGetString(Ctx.Params, TEXT("notify_class"), NotifyClassPath);
	NotifyClassPath = NotifyClassPath.TrimStartAndEnd();

	if (NotifyName.IsEmpty() && NotifyClassPath.IsEmpty())
	{
		OutError = TEXT("missing_notify_name_or_notify_class");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	if (TimeSeconds < 0.0 || (Montage->GetPlayLength() > 0.0f && TimeSeconds > Montage->GetPlayLength() + KINDA_SMALL_NUMBER))
	{
		OutError = TEXT("invalid_time_seconds");
		return false;
	}

	FString TrackName = UeAgentMontageOps::GetNotifyTrackName(Montage, 0);
	JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackName);
	TrackName = TrackName.TrimStartAndEnd();
	if (TrackName.IsEmpty())
	{
		TrackName = TEXT("1");
	}

	int32 TrackIndex = INDEX_NONE;
	Montage->Modify();
	if (!UeAgentMontageOps::EnsureNotifyTrack(Montage, FName(*TrackName), TrackIndex))
	{
		OutError = TEXT("create_notify_track_failed");
		return false;
	}

	FAnimNotifyEvent& NewEvent = Montage->Notifies.AddDefaulted_GetRef();
	NewEvent.NotifyName = NAME_None;
	NewEvent.Link(Montage, static_cast<float>(TimeSeconds));
	NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Montage->CalculateOffsetForNotify(static_cast<float>(TimeSeconds)));
	NewEvent.TrackIndex = TrackIndex;
	NewEvent.Notify = nullptr;
	NewEvent.NotifyStateClass = nullptr;
#if WITH_EDITORONLY_DATA
	NewEvent.Guid = FGuid::NewGuid();
#endif
	const FGuid NotifyGuid = NewEvent.Guid;

	FLinearColor NotifyColorLinear = FLinearColor::White;
	const bool bHasNotifyColor = UeAgentMontageOps::TryParseLinearColorField(Ctx.Params, TEXT("notify_color"), NotifyColorLinear);
	if (bHasNotifyColor)
	{
		NewEvent.NotifyColor = NotifyColorLinear.ToFColor(true);
	}

	if (!NotifyClassPath.IsEmpty())
	{
		UClass* NotifyClass = UeAgentMontageOps::LoadClassObject(UAnimNotify::StaticClass(), NotifyClassPath);
		if (!NotifyClass || !NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
		{
			OutError = TEXT("notify_class_not_found");
			return false;
		}
		if (NotifyClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("notify_class_is_abstract");
			return false;
		}

		NewEvent.Notify = NewObject<UAnimNotify>(Montage, NotifyClass, NAME_None, RF_Transactional);
		if (!NewEvent.Notify)
		{
			OutError = TEXT("create_notify_object_failed");
			return false;
		}
		if (bHasNotifyColor)
		{
			NewEvent.Notify->NotifyColor = NewEvent.NotifyColor;
		}
		NewEvent.NotifyName = FName(*NewEvent.Notify->GetNotifyName());
		NewEvent.TriggerWeightThreshold = NewEvent.Notify->GetDefaultTriggerWeightThreshold();
	}
	else
	{
		if (Montage->GetSkeleton())
		{
			Montage->GetSkeleton()->AddNewAnimationNotify(FName(*NotifyName));
		}
		NewEvent.NotifyName = FName(*NotifyName);
	}
	if (!UeAgentMontageOps::ApplyNotifyCommonSettings(Ctx.Params, NewEvent, OutError))
	{
		return false;
	}

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	int32 NotifyIndex = INDEX_NONE;
#if WITH_EDITORONLY_DATA
	NotifyIndex = UeAgentMontageOps::FindNotifyIndexByGuid(Montage, NotifyGuid);
#endif
	if (NotifyIndex == INDEX_NONE)
	{
		NotifyIndex = Montage->Notifies.Num() - 1;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetNumberField(TEXT("notify_index"), NotifyIndex);
	OutData->SetStringField(TEXT("notify_name"), Montage->Notifies.IsValidIndex(NotifyIndex) ? Montage->Notifies[NotifyIndex].NotifyName.ToString() : NewEvent.NotifyName.ToString());
	OutData->SetStringField(TEXT("track_name"), TrackName);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetBoolField(TEXT("branching_point"), NewEvent.MontageTickType == EMontageNotifyTickType::BranchingPoint);
	OutData->SetStringField(TEXT("tick_type"), UeAgentMontageOps::MontageNotifyTickTypeToString(NewEvent.MontageTickType));
	OutData->SetNumberField(TEXT("trigger_weight_threshold"), NewEvent.TriggerWeightThreshold);
	OutData->SetNumberField(TEXT("notify_trigger_chance"), NewEvent.NotifyTriggerChance);
	OutData->SetStringField(TEXT("notify_filter_type"), UeAgentMontageOps::NotifyFilterTypeToString(NewEvent.NotifyFilterType));
	OutData->SetNumberField(TEXT("notify_filter_lod"), NewEvent.NotifyFilterLOD);
	OutData->SetBoolField(TEXT("can_be_filtered_via_request"), NewEvent.bCanBeFilteredViaRequest);
	OutData->SetBoolField(TEXT("trigger_on_dedicated_server"), NewEvent.bTriggerOnDedicatedServer);
	OutData->SetBoolField(TEXT("trigger_on_follower"), NewEvent.bTriggerOnFollower);
	if (bHasNotifyColor)
	{
		OutData->SetObjectField(TEXT("notify_color_linear"), UeAgentMontageOps::BuildLinearColorJson(NotifyColorLinear));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddNotifyState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double DurationSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds) || DurationSeconds <= 0.0)
	{
		OutError = TEXT("invalid_duration_seconds");
		return false;
	}

	FString NotifyStateClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("notify_state_class"), NotifyStateClassPath) || NotifyStateClassPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_notify_state_class");
		return false;
	}
	NotifyStateClassPath = NotifyStateClassPath.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	if (TimeSeconds < 0.0 || (Montage->GetPlayLength() > 0.0f && TimeSeconds > Montage->GetPlayLength() + KINDA_SMALL_NUMBER))
	{
		OutError = TEXT("invalid_time_seconds");
		return false;
	}

	FString TrackName = UeAgentMontageOps::GetNotifyTrackName(Montage, 0);
	JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackName);
	TrackName = TrackName.TrimStartAndEnd();
	if (TrackName.IsEmpty())
	{
		TrackName = TEXT("1");
	}

	int32 TrackIndex = INDEX_NONE;
	Montage->Modify();
	if (!UeAgentMontageOps::EnsureNotifyTrack(Montage, FName(*TrackName), TrackIndex))
	{
		OutError = TEXT("create_notify_track_failed");
		return false;
	}

	UClass* NotifyStateClass = UeAgentMontageOps::LoadClassObject(UAnimNotifyState::StaticClass(), NotifyStateClassPath);
	if (!NotifyStateClass || !NotifyStateClass->IsChildOf(UAnimNotifyState::StaticClass()))
	{
		OutError = TEXT("notify_state_class_not_found");
		return false;
	}
	if (NotifyStateClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = TEXT("notify_state_class_is_abstract");
		return false;
	}

	FAnimNotifyEvent& NewEvent = Montage->Notifies.AddDefaulted_GetRef();
	NewEvent.NotifyName = NAME_None;
	NewEvent.Link(Montage, static_cast<float>(TimeSeconds));
	NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Montage->CalculateOffsetForNotify(static_cast<float>(TimeSeconds)));
	NewEvent.TrackIndex = TrackIndex;
	NewEvent.Notify = nullptr;
	NewEvent.NotifyStateClass = NewObject<UAnimNotifyState>(Montage, NotifyStateClass, NAME_None, RF_Transactional);
	if (!NewEvent.NotifyStateClass)
	{
		OutError = TEXT("create_notify_state_object_failed");
		return false;
	}
	NewEvent.NotifyName = FName(*NewEvent.NotifyStateClass->GetNotifyName());
	NewEvent.TriggerWeightThreshold = NewEvent.NotifyStateClass->GetDefaultTriggerWeightThreshold();
	NewEvent.SetDuration(static_cast<float>(DurationSeconds));
	NewEvent.EndLink.Link(Montage, NewEvent.EndLink.GetTime());
#if WITH_EDITORONLY_DATA
	NewEvent.Guid = FGuid::NewGuid();
#endif
	const FGuid NotifyGuid = NewEvent.Guid;

	FLinearColor NotifyColorLinear = FLinearColor::White;
	const bool bHasNotifyColor = UeAgentMontageOps::TryParseLinearColorField(Ctx.Params, TEXT("notify_color"), NotifyColorLinear);
	if (bHasNotifyColor)
	{
		NewEvent.NotifyColor = NotifyColorLinear.ToFColor(true);
		NewEvent.NotifyStateClass->NotifyColor = NewEvent.NotifyColor;
	}
	if (!UeAgentMontageOps::ApplyNotifyCommonSettings(Ctx.Params, NewEvent, OutError))
	{
		return false;
	}

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	int32 NotifyIndex = INDEX_NONE;
#if WITH_EDITORONLY_DATA
	NotifyIndex = UeAgentMontageOps::FindNotifyIndexByGuid(Montage, NotifyGuid);
#endif
	if (NotifyIndex == INDEX_NONE)
	{
		NotifyIndex = Montage->Notifies.Num() - 1;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetNumberField(TEXT("notify_index"), NotifyIndex);
	OutData->SetStringField(TEXT("notify_name"), Montage->Notifies.IsValidIndex(NotifyIndex) ? Montage->Notifies[NotifyIndex].NotifyName.ToString() : NewEvent.NotifyName.ToString());
	OutData->SetStringField(TEXT("track_name"), TrackName);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("branching_point"), NewEvent.MontageTickType == EMontageNotifyTickType::BranchingPoint);
	OutData->SetStringField(TEXT("tick_type"), UeAgentMontageOps::MontageNotifyTickTypeToString(NewEvent.MontageTickType));
	OutData->SetNumberField(TEXT("trigger_weight_threshold"), NewEvent.TriggerWeightThreshold);
	OutData->SetNumberField(TEXT("notify_trigger_chance"), NewEvent.NotifyTriggerChance);
	OutData->SetStringField(TEXT("notify_filter_type"), UeAgentMontageOps::NotifyFilterTypeToString(NewEvent.NotifyFilterType));
	OutData->SetNumberField(TEXT("notify_filter_lod"), NewEvent.NotifyFilterLOD);
	OutData->SetBoolField(TEXT("can_be_filtered_via_request"), NewEvent.bCanBeFilteredViaRequest);
	OutData->SetBoolField(TEXT("trigger_on_dedicated_server"), NewEvent.bTriggerOnDedicatedServer);
	OutData->SetBoolField(TEXT("trigger_on_follower"), NewEvent.bTriggerOnFollower);
	if (bHasNotifyColor)
	{
		OutData->SetObjectField(TEXT("notify_color_linear"), UeAgentMontageOps::BuildLinearColorJson(NotifyColorLinear));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageUpdateNotify(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double NotifyIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("notify_index"), NotifyIndexValue))
	{
		OutError = TEXT("missing_notify_index");
		return false;
	}
	const int32 NotifyIndex = FMath::RoundToInt(NotifyIndexValue);

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}
	if (!Montage->Notifies.IsValidIndex(NotifyIndex))
	{
		OutError = TEXT("notify_not_found");
		return false;
	}

	FAnimNotifyEvent& Notify = Montage->Notifies[NotifyIndex];
	const FGuid NotifyGuid = Notify.Guid;
	Montage->Modify();

	double TimeSeconds = Notify.GetTime();
	if (JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		if (TimeSeconds < 0.0 || (Montage->GetPlayLength() > 0.0f && TimeSeconds > Montage->GetPlayLength() + KINDA_SMALL_NUMBER))
		{
			OutError = TEXT("invalid_time_seconds");
			return false;
		}
		Notify.SetTime(static_cast<float>(TimeSeconds));
		Notify.TriggerTimeOffset = GetTriggerTimeOffsetForType(Montage->CalculateOffsetForNotify(static_cast<float>(TimeSeconds)));
	}

	if (Notify.NotifyStateClass)
	{
		double DurationSeconds = Notify.GetDuration();
		if (JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds))
		{
			if (DurationSeconds <= 0.0)
			{
				OutError = TEXT("invalid_duration_seconds");
				return false;
			}
			Notify.SetDuration(static_cast<float>(DurationSeconds));
			Notify.EndLink.Link(Montage, Notify.EndLink.GetTime());
		}
	}
	else if (Ctx.Params->HasField(TEXT("duration_seconds")))
	{
		OutError = TEXT("notify_is_not_state");
		return false;
	}

	FString TrackName;
	if (JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackName) && !TrackName.TrimStartAndEnd().IsEmpty())
	{
		int32 TrackIndex = INDEX_NONE;
		if (!UeAgentMontageOps::EnsureNotifyTrack(Montage, FName(*TrackName.TrimStartAndEnd()), TrackIndex))
		{
			OutError = TEXT("create_notify_track_failed");
			return false;
		}
		Notify.TrackIndex = TrackIndex;
	}

	FString NotifyName;
	if (JsonTryGetString(Ctx.Params, TEXT("notify_name"), NotifyName) && !NotifyName.TrimStartAndEnd().IsEmpty())
	{
		if (Notify.IsBlueprintNotify())
		{
			OutError = TEXT("cannot_rename_blueprint_notify_by_name");
			return false;
		}
		NotifyName = NotifyName.TrimStartAndEnd();
		if (Montage->GetSkeleton())
		{
			Montage->GetSkeleton()->AddNewAnimationNotify(FName(*NotifyName));
		}
		Notify.NotifyName = FName(*NotifyName);
	}

	if (!UeAgentMontageOps::ApplyNotifyCommonSettings(Ctx.Params, Notify, OutError))
	{
		return false;
	}

	FLinearColor NotifyColorLinear = FLinearColor(Notify.NotifyColor);
	const bool bHasNotifyColor = UeAgentMontageOps::TryParseLinearColorField(Ctx.Params, TEXT("notify_color"), NotifyColorLinear);
	if (bHasNotifyColor)
	{
		Notify.NotifyColor = NotifyColorLinear.ToFColor(true);
		if (Notify.Notify)
		{
			Notify.Notify->NotifyColor = Notify.NotifyColor;
		}
		if (Notify.NotifyStateClass)
		{
			Notify.NotifyStateClass->NotifyColor = Notify.NotifyColor;
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterSet, OutError))
	{
		return false;
	}

	int32 UpdatedNotifyIndex = NotifyIndex;
#if WITH_EDITORONLY_DATA
	if (NotifyGuid.IsValid())
	{
		UpdatedNotifyIndex = UeAgentMontageOps::FindNotifyIndexByGuid(Montage, NotifyGuid);
	}
#endif
	if (!Montage->Notifies.IsValidIndex(UpdatedNotifyIndex))
	{
		UpdatedNotifyIndex = FMath::Clamp(NotifyIndex, 0, Montage->Notifies.Num() - 1);
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetNumberField(TEXT("notify_index"), UpdatedNotifyIndex);
	OutData->SetStringField(TEXT("notify_name"), Montage->Notifies[UpdatedNotifyIndex].NotifyName.ToString());
	OutData->SetStringField(TEXT("track_name"), UeAgentMontageOps::GetNotifyTrackName(Montage, Montage->Notifies[UpdatedNotifyIndex].TrackIndex));
	OutData->SetNumberField(TEXT("time_seconds"), Montage->Notifies[UpdatedNotifyIndex].GetTime());
	OutData->SetNumberField(TEXT("duration_seconds"), Montage->Notifies[UpdatedNotifyIndex].GetDuration());
	OutData->SetBoolField(TEXT("branching_point"), Montage->Notifies[UpdatedNotifyIndex].MontageTickType == EMontageNotifyTickType::BranchingPoint);
	OutData->SetStringField(TEXT("tick_type"), UeAgentMontageOps::MontageNotifyTickTypeToString(Montage->Notifies[UpdatedNotifyIndex].MontageTickType));
	OutData->SetNumberField(TEXT("trigger_weight_threshold"), Montage->Notifies[UpdatedNotifyIndex].TriggerWeightThreshold);
	OutData->SetNumberField(TEXT("notify_trigger_chance"), Montage->Notifies[UpdatedNotifyIndex].NotifyTriggerChance);
	OutData->SetStringField(TEXT("notify_filter_type"), UeAgentMontageOps::NotifyFilterTypeToString(Montage->Notifies[UpdatedNotifyIndex].NotifyFilterType));
	OutData->SetNumberField(TEXT("notify_filter_lod"), Montage->Notifies[UpdatedNotifyIndex].NotifyFilterLOD);
	OutData->SetBoolField(TEXT("can_be_filtered_via_request"), Montage->Notifies[UpdatedNotifyIndex].bCanBeFilteredViaRequest);
	OutData->SetBoolField(TEXT("trigger_on_dedicated_server"), Montage->Notifies[UpdatedNotifyIndex].bTriggerOnDedicatedServer);
	OutData->SetBoolField(TEXT("trigger_on_follower"), Montage->Notifies[UpdatedNotifyIndex].bTriggerOnFollower);
	if (bHasNotifyColor)
	{
		OutData->SetObjectField(TEXT("notify_color_linear"), UeAgentMontageOps::BuildLinearColorJson(NotifyColorLinear));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveNotify(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double NotifyIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("notify_index"), NotifyIndexValue))
	{
		OutError = TEXT("missing_notify_index");
		return false;
	}
	const int32 NotifyIndex = FMath::RoundToInt(NotifyIndexValue);

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}
	if (!Montage->Notifies.IsValidIndex(NotifyIndex))
	{
		OutError = TEXT("notify_not_found");
		return false;
	}

	const FString RemovedNotifyName = Montage->Notifies[NotifyIndex].NotifyName.ToString();
	Montage->Modify();
	Montage->Notifies.RemoveAt(NotifyIndex);

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("removed_notify_name"), RemovedNotifyName);
	OutData->SetNumberField(TEXT("removed_notify_index"), NotifyIndex);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddSlotTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 ExistingIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (ExistingIndex != INDEX_NONE)
	{
		OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
		OutData->SetStringField(TEXT("slot_name"), Montage->SlotAnimTracks[ExistingIndex].SlotName.ToString());
		OutData->SetNumberField(TEXT("slot_index"), ExistingIndex);
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	Montage->Modify();
	Montage->AddSlot(FName(*SlotName));

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("slot_name"), SlotName);
	OutData->SetNumberField(TEXT("slot_index"), SlotIndex);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRenameSlotTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	FString NewSlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_slot_name"), NewSlotName) || NewSlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_new_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();
	NewSlotName = NewSlotName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (SlotIndex == INDEX_NONE)
	{
		OutError = TEXT("slot_track_not_found");
		return false;
	}

	const int32 ExistingIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, NewSlotName);
	if (ExistingIndex != INDEX_NONE && ExistingIndex != SlotIndex)
	{
		OutError = TEXT("slot_name_already_exists");
		return false;
	}

	Montage->Modify();
	Montage->SlotAnimTracks[SlotIndex].SlotName = FName(*NewSlotName);

	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRename, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("old_slot_name"), SlotName);
	OutData->SetStringField(TEXT("new_slot_name"), NewSlotName);
	OutData->SetNumberField(TEXT("slot_index"), SlotIndex);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveSlotTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (SlotIndex == INDEX_NONE)
	{
		OutError = TEXT("slot_track_not_found");
		return false;
	}
	if (Montage->SlotAnimTracks.Num() <= 1)
	{
		OutError = TEXT("cannot_remove_last_slot_track");
		return false;
	}
	if (SlotIndex != Montage->SlotAnimTracks.Num() - 1)
	{
		OutError = TEXT("can_only_remove_last_slot_track");
		return false;
	}
	if (Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments.Num() > 0)
	{
		OutError = TEXT("slot_track_not_empty");
		return false;
	}

	Montage->Modify();
	Montage->SlotAnimTracks.RemoveAt(SlotIndex);

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("removed_slot_name"), SlotName);
	OutData->SetNumberField(TEXT("removed_slot_index"), SlotIndex);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddSegment(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();

	FString AnimationAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_asset"), AnimationAssetPath) || AnimationAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_animation_asset");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	UAnimSequenceBase* AnimationAsset = UeAgentMontageOps::LoadAnimSequenceBase(AnimationAssetPath);
	if (!AnimationAsset)
	{
		OutError = TEXT("animation_asset_not_found");
		return false;
	}
	if (Montage->GetSkeleton() && AnimationAsset->GetSkeleton() && !Montage->GetSkeleton()->IsCompatibleForEditor(AnimationAsset->GetSkeleton()))
	{
		OutError = TEXT("animation_asset_skeleton_incompatible");
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (SlotIndex == INDEX_NONE)
	{
		OutError = TEXT("slot_track_not_found");
		return false;
	}

	double StartPos = Montage->SlotAnimTracks[SlotIndex].AnimTrack.GetLength();
	JsonTryGetNumber(Ctx.Params, TEXT("start_pos"), StartPos);
	if (StartPos < 0.0)
	{
		OutError = TEXT("invalid_start_pos");
		return false;
	}

	double AnimStartTime = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("anim_start_time"), AnimStartTime);

	double AnimEndTime = AnimationAsset->GetPlayLength();
	JsonTryGetNumber(Ctx.Params, TEXT("anim_end_time"), AnimEndTime);
	if (AnimStartTime < 0.0 || AnimEndTime <= AnimStartTime || AnimEndTime > AnimationAsset->GetPlayLength() + KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("invalid_anim_time_range");
		return false;
	}

	double PlayRate = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("play_rate"), PlayRate);
	if (FMath::IsNearlyZero(PlayRate))
	{
		OutError = TEXT("invalid_play_rate");
		return false;
	}

	double LoopCount = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("loop_count"), LoopCount);
	const int32 LoopCountInt = FMath::Max(1, FMath::RoundToInt(LoopCount));

	FAnimSegment NewSegment;
	NewSegment.SetAnimReference(AnimationAsset, true);
	NewSegment.StartPos = static_cast<float>(StartPos);
	NewSegment.AnimStartTime = static_cast<float>(AnimStartTime);
	NewSegment.AnimEndTime = static_cast<float>(AnimEndTime);
	NewSegment.AnimPlayRate = static_cast<float>(PlayRate);
	NewSegment.LoopingCount = LoopCountInt;

	Montage->Modify();
	Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments.Add(NewSegment);

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	int32 SegmentIndex = INDEX_NONE;
	const TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FAnimSegment& Segment = Segments[Index];
		if (Segment.GetAnimReference() == AnimationAsset
			&& FMath::IsNearlyEqual(Segment.StartPos, static_cast<float>(StartPos))
			&& FMath::IsNearlyEqual(Segment.AnimStartTime, static_cast<float>(AnimStartTime))
			&& FMath::IsNearlyEqual(Segment.AnimEndTime, static_cast<float>(AnimEndTime))
			&& FMath::IsNearlyEqual(Segment.AnimPlayRate, static_cast<float>(PlayRate))
			&& Segment.LoopingCount == LoopCountInt)
		{
			SegmentIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("slot_name"), SlotName);
	OutData->SetNumberField(TEXT("slot_index"), SlotIndex);
	OutData->SetNumberField(TEXT("segment_index"), SegmentIndex);
	OutData->SetStringField(TEXT("animation_asset"), AnimationAsset->GetPathName());
	OutData->SetNumberField(TEXT("start_pos"), StartPos);
	OutData->SetNumberField(TEXT("anim_start_time"), AnimStartTime);
	OutData->SetNumberField(TEXT("anim_end_time"), AnimEndTime);
	OutData->SetNumberField(TEXT("play_rate"), PlayRate);
	OutData->SetNumberField(TEXT("loop_count"), LoopCountInt);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageUpdateSegment(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();

	double SegmentIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("segment_index"), SegmentIndexValue))
	{
		OutError = TEXT("missing_segment_index");
		return false;
	}
	const int32 SegmentIndex = FMath::RoundToInt(SegmentIndexValue);

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (SlotIndex == INDEX_NONE)
	{
		OutError = TEXT("slot_track_not_found");
		return false;
	}

	TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments;
	if (!Segments.IsValidIndex(SegmentIndex))
	{
		OutError = TEXT("segment_not_found");
		return false;
	}

	FAnimSegment& Segment = Segments[SegmentIndex];
	UAnimSequenceBase* AnimationAsset = Segment.GetAnimReference();

	FString AnimationAssetPath;
	if (JsonTryGetString(Ctx.Params, TEXT("animation_asset"), AnimationAssetPath) && !AnimationAssetPath.TrimStartAndEnd().IsEmpty())
	{
		AnimationAsset = UeAgentMontageOps::LoadAnimSequenceBase(AnimationAssetPath);
		if (!AnimationAsset)
		{
			OutError = TEXT("animation_asset_not_found");
			return false;
		}
		if (Montage->GetSkeleton() && AnimationAsset->GetSkeleton() && !Montage->GetSkeleton()->IsCompatibleForEditor(AnimationAsset->GetSkeleton()))
		{
			OutError = TEXT("animation_asset_skeleton_incompatible");
			return false;
		}
	}

	double StartPos = Segment.StartPos;
	JsonTryGetNumber(Ctx.Params, TEXT("start_pos"), StartPos);
	if (StartPos < 0.0)
	{
		OutError = TEXT("invalid_start_pos");
		return false;
	}

	double AnimStartTime = Segment.AnimStartTime;
	JsonTryGetNumber(Ctx.Params, TEXT("anim_start_time"), AnimStartTime);

	double AnimEndTime = Segment.AnimEndTime;
	const bool bHasAnimEndTime = JsonTryGetNumber(Ctx.Params, TEXT("anim_end_time"), AnimEndTime);
	if (!bHasAnimEndTime && AnimationAsset != Segment.GetAnimReference())
	{
		AnimEndTime = AnimationAsset->GetPlayLength();
	}
	if (AnimStartTime < 0.0 || AnimEndTime <= AnimStartTime || AnimEndTime > AnimationAsset->GetPlayLength() + KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("invalid_anim_time_range");
		return false;
	}

	double PlayRate = Segment.AnimPlayRate;
	JsonTryGetNumber(Ctx.Params, TEXT("play_rate"), PlayRate);
	if (FMath::IsNearlyZero(PlayRate))
	{
		OutError = TEXT("invalid_play_rate");
		return false;
	}

	double LoopCount = Segment.LoopingCount;
	JsonTryGetNumber(Ctx.Params, TEXT("loop_count"), LoopCount);
	const int32 LoopCountInt = FMath::Max(1, FMath::RoundToInt(LoopCount));

	Montage->Modify();
	if (AnimationAsset != Segment.GetAnimReference())
	{
		Segment.SetAnimReference(AnimationAsset, false);
	}
	Segment.StartPos = static_cast<float>(StartPos);
	Segment.AnimStartTime = static_cast<float>(AnimStartTime);
	Segment.AnimEndTime = static_cast<float>(AnimEndTime);
	Segment.AnimPlayRate = static_cast<float>(PlayRate);
	Segment.LoopingCount = LoopCountInt;

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("slot_name"), SlotName);
	OutData->SetNumberField(TEXT("slot_index"), SlotIndex);
	OutData->SetNumberField(TEXT("segment_index"), SegmentIndex);
	OutData->SetStringField(TEXT("animation_asset"), AnimationAsset ? AnimationAsset->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("start_pos"), StartPos);
	OutData->SetNumberField(TEXT("anim_start_time"), AnimStartTime);
	OutData->SetNumberField(TEXT("anim_end_time"), AnimEndTime);
	OutData->SetNumberField(TEXT("play_rate"), PlayRate);
	OutData->SetNumberField(TEXT("loop_count"), LoopCountInt);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveSegment(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SlotName;
	if (!JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) || SlotName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_slot_name");
		return false;
	}
	SlotName = SlotName.TrimStartAndEnd();

	double SegmentIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("segment_index"), SegmentIndexValue))
	{
		OutError = TEXT("missing_segment_index");
		return false;
	}
	const int32 SegmentIndex = FMath::RoundToInt(SegmentIndexValue);

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SlotIndex = UeAgentMontageOps::FindSlotTrackIndexByName(Montage, SlotName);
	if (SlotIndex == INDEX_NONE)
	{
		OutError = TEXT("slot_track_not_found");
		return false;
	}

	TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments;
	if (!Segments.IsValidIndex(SegmentIndex))
	{
		OutError = TEXT("segment_not_found");
		return false;
	}

	const FString RemovedAnimationAsset = Segments[SegmentIndex].GetAnimReference() ? Segments[SegmentIndex].GetAnimReference()->GetPathName() : TEXT("");
	Montage->Modify();
	Segments.RemoveAt(SegmentIndex);

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("slot_name"), SlotName);
	OutData->SetNumberField(TEXT("slot_index"), SlotIndex);
	OutData->SetNumberField(TEXT("removed_segment_index"), SegmentIndex);
	OutData->SetStringField(TEXT("removed_animation_asset"), RemovedAnimationAsset);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdMontageAddSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("section_name"), SectionName) || SectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_section_name");
		return false;
	}
	SectionName = SectionName.TrimStartAndEnd();

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 ExistingIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	if (ExistingIndex != INDEX_NONE)
	{
		OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
		OutData->SetStringField(TEXT("section_name"), Montage->CompositeSections[ExistingIndex].SectionName.ToString());
		OutData->SetNumberField(TEXT("section_index"), ExistingIndex);
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	Montage->Modify();
	if (Montage->AddAnimCompositeSection(FName(*SectionName), static_cast<float>(TimeSeconds)) == INDEX_NONE)
	{
		OutError = TEXT("add_section_failed");
		return false;
	}

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterAdd, OutError))
	{
		return false;
	}

	const int32 SectionIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("section_name"), SectionName);
	OutData->SetNumberField(TEXT("section_index"), SectionIndex);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRenameSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("section_name"), SectionName) || SectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_section_name");
		return false;
	}
	FString NewSectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_section_name"), NewSectionName) || NewSectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_new_section_name");
		return false;
	}
	SectionName = SectionName.TrimStartAndEnd();
	NewSectionName = NewSectionName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SectionIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	if (SectionIndex == INDEX_NONE)
	{
		OutError = TEXT("section_not_found");
		return false;
	}
	const int32 ExistingIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, NewSectionName);
	if (ExistingIndex != INDEX_NONE && ExistingIndex != SectionIndex)
	{
		OutError = TEXT("section_name_already_exists");
		return false;
	}

	Montage->Modify();
	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.NextSectionName.ToString().Equals(SectionName, ESearchCase::IgnoreCase))
		{
			Section.NextSectionName = FName(*NewSectionName);
		}
	}
	Montage->CompositeSections[SectionIndex].SectionName = FName(*NewSectionName);

	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRename, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("old_section_name"), SectionName);
	OutData->SetStringField(TEXT("new_section_name"), NewSectionName);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetSectionTime(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("section_name"), SectionName) || SectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_section_name");
		return false;
	}
	SectionName = SectionName.TrimStartAndEnd();

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SectionIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	if (SectionIndex == INDEX_NONE)
	{
		OutError = TEXT("section_not_found");
		return false;
	}

	Montage->Modify();
	Montage->CompositeSections[SectionIndex].SetTime(static_cast<float>(TimeSeconds));

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("section_name"), SectionName);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMontageRemoveSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("section_name"), SectionName) || SectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_section_name");
		return false;
	}
	SectionName = SectionName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SectionIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	if (SectionIndex == INDEX_NONE)
	{
		OutError = TEXT("section_not_found");
		return false;
	}
	if (Montage->CompositeSections.Num() <= 1)
	{
		OutError = TEXT("cannot_remove_last_section");
		return false;
	}

	Montage->Modify();
	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.NextSectionName.ToString().Equals(SectionName, ESearchCase::IgnoreCase))
		{
			Section.NextSectionName = NAME_None;
		}
	}
	if (!Montage->DeleteAnimCompositeSection(SectionIndex))
	{
		OutError = TEXT("remove_section_failed");
		return false;
	}

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("removed_section_name"), SectionName);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdMontageSetNextSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SectionName;
	if (!JsonTryGetString(Ctx.Params, TEXT("section_name"), SectionName) || SectionName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_section_name");
		return false;
	}
	SectionName = SectionName.TrimStartAndEnd();

	UAnimMontage* Montage = UeAgentMontageOps::LoadMontage(AssetPath);
	if (!Montage)
	{
		OutError = TEXT("montage_not_found");
		return false;
	}

	const int32 SectionIndex = UeAgentMontageOps::FindSectionIndexByName(Montage, SectionName);
	if (SectionIndex == INDEX_NONE)
	{
		OutError = TEXT("section_not_found");
		return false;
	}

	bool bClearNextSection = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_next_section"), bClearNextSection);

	FName NextSectionName = NAME_None;
	if (!bClearNextSection)
	{
		FString NextSectionNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("next_section_name"), NextSectionNameText) || NextSectionNameText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_next_section_name");
			return false;
		}
		NextSectionNameText = NextSectionNameText.TrimStartAndEnd();
		if (UeAgentMontageOps::FindSectionIndexByName(Montage, NextSectionNameText) == INDEX_NONE)
		{
			OutError = TEXT("next_section_not_found");
			return false;
		}
		NextSectionName = FName(*NextSectionNameText);
	}

	Montage->Modify();
	Montage->CompositeSections[SectionIndex].NextSectionName = NextSectionName;

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentMontageOps::FinalizeMontageMutation(Montage, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	OutData->SetStringField(TEXT("section_name"), SectionName);
	OutData->SetStringField(TEXT("next_section_name"), NextSectionName.IsNone() ? TEXT("") : NextSectionName.ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
