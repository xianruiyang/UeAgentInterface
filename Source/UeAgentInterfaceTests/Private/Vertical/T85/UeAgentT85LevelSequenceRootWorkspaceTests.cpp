#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Algo/Reverse.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/Sequence/UeAgentSequenceDomainMechanics.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineScalarChannel.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Engine/PointLight.h"
#include "Components/PointLightComponent.h"
#include "PreviewScene.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneFolder.h"
#include "UniversalObjectLocatorResolveParams.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "Conditions/MovieSceneGroupCondition.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneDoubleSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Engine/DataAsset.h"
#include "HLOD/HLODLevelExclusion.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "ObjectTools.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "TestSupport/UeAgentFixtureDirectoryLifecycle.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Vertical/T167/UeAgentT167AssetReferenceTestTypes.h"
#include <type_traits>

namespace
{
	using namespace UE::UAI;

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	UMovieSceneGroupCondition* AddGroupConditionFixture(UMovieSceneTrack& Track)
	{
		auto* Group = NewObject<UMovieSceneGroupCondition>(&Track, TEXT("ExistingGroup"), RF_Transactional);
		auto* Child = NewObject<UMovieSceneGroupCondition>(Group, TEXT("ExistingChild"), RF_Transactional);
		Child->Operator = EMovieSceneGroupConditionOperator::Or;
		FMovieSceneConditionContainer Container; Container.Condition = Child;
		Group->SubConditions.Add(Container); Track.ConditionContainer.Condition = Group;
		return Group;
	}

	bool SetSparseGroupOperator(const TSharedPtr<FJsonObject>& ConditionRow, const FString& Name)
	{
		const auto Group = ConditionRow->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))
			->GetObjectField(TEXT("Condition"))->GetObjectField(TEXT("owned_object"));
		for (const auto& Category : Group->GetObjectField(TEXT("properties"))->Values)
			if (Category.Value->AsObject()->HasField(TEXT("Operator"))) {
				const auto Row = Category.Value->AsObject()->GetObjectField(TEXT("Operator"));
				Row->SetStringField(TEXT("value"), Name);
				const auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("Operator"), Row);
				const auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(Category.Key, Fields);
				Group->SetObjectField(TEXT("properties"), Properties); return true;
			}
		return false;
	}

	TSharedPtr<FJsonObject> SingleChannel(const TSharedPtr<FJsonObject>& Section)
	{
		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (Section->TryGetArrayField(TEXT("channels"), Channels)) return (*Channels)[0]->AsObject();
		const TSharedPtr<FJsonObject>* Visible = nullptr;
		if (Section->TryGetObjectField(TEXT("channels"), Visible)) return (*Visible)->GetObjectField(TEXT("Value"));
		return Section; // Independent missing-target initialization fixture.
	}

	void SetPublicKeys(const TSharedPtr<FJsonObject>& Section, const TSharedPtr<FJsonObject>& Keys)
	{
		const auto Channel = MakeShared<FJsonObject>(); Channel->SetObjectField(TEXT("keys"), Keys);
		const auto Channels = MakeShared<FJsonObject>(); Channels->SetObjectField(TEXT("Value"), Channel);
		Section->SetObjectField(TEXT("channels"), Channels);
	}

	TMap<FString, FString> CaptureMovieScene(UMovieScene& MovieScene)
	{
		TMap<FString, FString> Result;
		for (TFieldIterator<FProperty> It(MovieScene.GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value;
			It->ExportText_InContainer(0, Value, &MovieScene, nullptr, &MovieScene, PPF_None);
			Result.Add(It->GetName(), MoveTemp(Value));
		}
		return Result;
	}

	TMap<FString, FString> CapturePersistentObject(UObject& Object)
	{
		TMap<FString, FString> Result;
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value;
			It->ExportText_InContainer(0, Value, &Object, nullptr, &Object, PPF_None);
			Result.Add(It->GetName(), MoveTemp(Value));
		}
		return Result;
	}

	TSharedRef<ISequencer> OpenFixtureSequencer(ULevelSequence* Sequence, UWorld* World)
	{
		FSequencerInitParams Init;
		Init.RootSequence = Sequence;
		Init.PlaybackContext = World;
		Init.ViewParams.UniqueName = TEXT("UaiT85TracksBinding");
		Init.bEditWithinLevelEditor = false;
		return FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(Init);
	}

	bool CheckPersistentObject(FAutomationTestBase& Test, const TCHAR* Label,
		const TMap<FString, FString>& Before, UObject& Object)
	{
		const auto After = CapturePersistentObject(Object);
		const bool bEqual = Before.OrderIndependentCompareEqual(After);
		if (!bEqual) for (const auto& Field : Before)
			if (After.FindRef(Field.Key) != Field.Value)
				Test.AddError(FString(Label) + TEXT(" field=") + Field.Key + TEXT(" before=") + Field.Value + TEXT(" after=") + After.FindRef(Field.Key));
		return Test.TestTrue(Label, bEqual);
	}

	bool HasFixtureBinding(ULevelSequence* Sequence, UWorld* World, const FGuid& Guid, UObject* Object)
	{
		const auto Sequencer = OpenFixtureSequencer(Sequence, World);
		ON_SCOPE_EXIT { Sequencer->Close(); };
		const auto Objects = Sequencer->FindBoundObjects(Guid, MovieSceneSequenceID::Root);
		return Objects.Num() == 1 && Objects[0].Get() == Object;
	}

	const TArray<FString>& TransformChannelNames()
	{
		static const TArray<FString> Names{TEXT("Location.X"), TEXT("Location.Y"), TEXT("Location.Z"),
			TEXT("Rotation.X"), TEXT("Rotation.Y"), TEXT("Rotation.Z"), TEXT("Scale.X"), TEXT("Scale.Y"), TEXT("Scale.Z"), TEXT("Weight")};
		return Names;
	}

	bool HasNativeTransformChannels(UMovieSceneSection& Section)
	{
		const auto& Proxy = Section.GetChannelProxy();
		if (Proxy.GetAllEntries().Num() != 2 || Proxy.GetChannels<FMovieSceneDoubleChannel>().Num() != 9
			|| Proxy.GetChannels<FMovieSceneFloatChannel>().Num() != 1) return false;
		for (const auto& Entry : Proxy.GetAllEntries())
		{
			const bool bDouble = Entry.GetChannelTypeName() == FMovieSceneDoubleChannel::StaticStruct()->GetFName();
			if (!bDouble && Entry.GetChannelTypeName() != FMovieSceneFloatChannel::StaticStruct()->GetFName()) return false;
			if (Entry.GetMetaData().Num() != (bDouble ? 9 : 1)) return false;
			for (int32 Index = 0; Index < Entry.GetMetaData().Num(); ++Index)
				if (Entry.GetMetaData()[Index].Name.ToString() != TransformChannelNames()[bDouble ? Index : 9]) return false;
		}
		return true;
	}

	UMovieSceneBoolTrack* AddBoolFixtureTrack(UMovieScene& Scene, const TCHAR* Name)
	{
		auto* Track = Scene.AddTrack<UMovieSceneBoolTrack>(); Track->SetDisplayName(FText::FromString(Name));
		auto* Section = CastChecked<UMovieSceneBoolSection>(Track->CreateNewSection()); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		Section->GetChannel().GetData().AddKey(FFrameNumber(0), false); Section->GetChannel().GetData().AddKey(FFrameNumber(9000), true);
		return Track;
	}

	FMovieSceneIntegerChannel& FixtureChannel(UMovieSceneIntegerSection& Section) { return *Section.GetChannelProxy().GetChannel<FMovieSceneIntegerChannel>(0); }
	FMovieSceneBoolChannel& FixtureChannel(UMovieSceneBoolSection& Section) { return Section.GetChannel(); }
	FMovieSceneByteChannel& FixtureChannel(UMovieSceneByteSection& Section) { return Section.ByteCurve; }
	UEnum* ByteFixtureEnum() { return StaticEnum<EHLODLevelExclusion>(); }
	UMovieSceneStringTrack* AddStringFixtureTrack(UMovieScene& Scene)
	{
		auto* Track = Scene.AddTrack<UMovieSceneStringTrack>(); Track->SetDisplayName(FText::FromString(TEXT("Precise")));
		auto* Section = Track->CreateNewSection(); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
		Channel->GetData().AddKey(FFrameNumber(0), FString(TEXT("原文\"quoted\"\n下一行")));
		Channel->GetData().AddKey(FFrameNumber(9000), FString(TEXT("保留"))); Channel->SetDefault(TEXT("原默认")); return Track;
	}
	bool CreateReferenceAssets(FAutomationTestBase& Test, const FString& Token, TArray<FString>& Paths)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const FString Name = TEXT("DA_") + Token + FString::FromInt(Index);
			const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceObjectAssets/") + Name;
			UPackage* Package = CreatePackage(*PackageName);
			auto* Asset = NewObject<UUeAgentT167AssetReferenceData>(Package, *Name, RF_Public | RF_Standalone);
			Paths.Add(Asset->GetPathName()); FAssetRegistryModule::AssetCreated(Asset);
			FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!Test.TestTrue(TEXT("external reference fixture saved independently"), UPackage::SavePackage(Package, Asset,
				*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args))) return false;
		}
		return true;
	}
	UMovieSceneObjectPropertyTrack* AddObjectFixtureTrack(UMovieScene& Scene, const FString& Path, const bool bClass = false)
	{
		auto* Track = Scene.AddTrack<UMovieSceneObjectPropertyTrack>(); Track->SetDisplayName(FText::FromString(TEXT("Precise"))); Track->PropertyClass = bClass ? USceneComponent::StaticClass() : UDataAsset::StaticClass(); Track->bClassProperty = bClass;
		auto* Section = Track->CreateNewSection(); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
		UObject* Asset = LoadObject<UObject>(nullptr, *Path); Channel->SetDefault(Asset);
		Channel->GetData().AddKey(FFrameNumber(0), FMovieSceneObjectPathChannelKeyValue(Asset));
		Channel->GetData().AddKey(FFrameNumber(9000), FMovieSceneObjectPathChannelKeyValue(nullptr));
		FMovieSceneObjectPathChannelKeyValue Dangling;
		// Fixture-only initialization of the real native soft field; production never reflects a surrogate property.
		auto* Soft = FindFProperty<FSoftObjectProperty>(FMovieSceneObjectPathChannelKeyValue::StaticStruct(), TEXT("SoftPtr"));
		Soft->SetPropertyValue(Soft->ContainerPtrToValuePtr<void>(&Dangling), FSoftObjectPtr(FSoftObjectPath(TEXT("/Game/UAI/Transient/AbsentObject.AbsentObject"))));
		Channel->GetData().AddKey(FFrameNumber(18000), Dangling); return Track;
	}

	UMovieSceneByteTrack* AddByteEnumFixtureTrack(UMovieScene& Scene, UEnum* Enum)
	{
		auto* Track = Scene.AddTrack<UMovieSceneByteTrack>(); Track->SetDisplayName(FText::FromString(TEXT("Precise"))); Track->SetEnum(Enum);
		auto* Section = CastChecked<UMovieSceneByteSection>(Track->CreateNewSection()); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		Section->ByteCurve.GetData().AddKey(FFrameNumber(0), 1); Section->ByteCurve.GetData().AddKey(FFrameNumber(9000), 4); return Track;
	}
	template <typename Visitor>
	bool VisitPrimitiveFixture(UMovieSceneSection* Section, Visitor&& Consume)
	{
		if (auto* Byte = Cast<UMovieSceneByteSection>(Section)) return Consume(Byte->ByteCurve);
		if (auto* Integer = Cast<UMovieSceneIntegerSection>(Section)) return Consume(FixtureChannel(*Integer));
		if (auto* Bool = Cast<UMovieSceneBoolSection>(Section)) return Consume(Bool->GetChannel());
		return false;
	}
	UMovieSceneIntegerTrack* AddIntegerFixtureTrack(UMovieScene& Scene, const TCHAR* Name)
	{
		auto* Track = Scene.AddTrack<UMovieSceneIntegerTrack>(); Track->SetDisplayName(FText::FromString(Name));
		auto* Section = CastChecked<UMovieSceneIntegerSection>(Track->CreateNewSection()); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		FixtureChannel(*Section).GetData().AddKey(FFrameNumber(0), MIN_int32); FixtureChannel(*Section).GetData().AddKey(FFrameNumber(9000), MAX_int32);
		return Track;
	}

	UMovieScene3DTransformTrack* AddTransformFixtureTrack(UMovieScene& Scene, const TCHAR* Name)
	{
		auto* Track = Scene.AddTrack<UMovieScene3DTransformTrack>(); Track->SetDisplayName(FText::FromString(Name));
		auto* Section = CastChecked<UMovieScene3DTransformSection>(Track->CreateNewSection());
		Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		Section->SetMask(FMovieSceneTransformMask(EMovieSceneTransformChannel::All));
		auto Fill = [](auto& Channel, int32 Ordinal) {
			using Native = std::decay_t<decltype(Channel)>; using Value = typename Native::ChannelValueType;
			const bool bTarget = Ordinal == 0;
			Channel.SetDefault(static_cast<typename Native::CurveValueType>(bTarget ? 1.2345678901234567 : 10.25 + Ordinal));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				Value Key(static_cast<typename Native::CurveValueType>(bTarget ? (Index ? 9007199254740991.0 : 1.0000000000000002) : 50.25 + Ordinal + Index));
				Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_Break;
				Key.Tangent.ArriveTangent = 12.5f + Index; Key.Tangent.LeaveTangent = -15.25f - Index;
				Key.Tangent.ArriveTangentWeight = 2.5f; Key.Tangent.LeaveTangentWeight = 3.25f;
				Key.Tangent.TangentWeightMode = RCTWM_WeightedBoth;
				Channel.GetData().AddKey(FFrameNumber(Index * 9000), Key);
			}
		};
		// Independent fixture initialization uses the native typed arrays, not product enumeration.
		const auto Doubles = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		for (int32 Index = 0; Index < Doubles.Num(); ++Index) Fill(*Doubles[Index], Index);
		Fill(*Section->GetWeightChannel(), 9);
		return Track;
	}

	TMap<FString, FString> CaptureOtherChannels(UMovieSceneSection& Section, const FString& Selected)
	{
		TMap<FString, FString> Result;
		for (const auto& Entry : Section.GetChannelProxy().GetAllEntries())
		{
			const UScriptStruct* Type = Entry.GetChannelTypeName() == FMovieSceneDoubleChannel::StaticStruct()->GetFName()
				? FMovieSceneDoubleChannel::StaticStruct() : Entry.GetChannelTypeName() == FMovieSceneFloatChannel::StaticStruct()->GetFName()
				? FMovieSceneFloatChannel::StaticStruct() : nullptr;
			if (!Type) { Result.Add(TEXT("unsupported"), Entry.GetChannelTypeName().ToString()); continue; }
			for (int32 Index = 0; Index < Entry.GetChannels().Num(); ++Index)
			{
				const auto& Metadata = Entry.GetMetaData()[Index];
				const FString Name = Metadata.Name.IsNone() ? TEXT("Value") : Metadata.Name.ToString();
				if (Name != Selected) { FString Text;
					Type->ExportText(Text, Entry.GetChannels()[Index], nullptr, nullptr, PPF_None, nullptr); Result.Add(Name, Text); }
			}
		}
		return Result;
	}

	UMovieSceneTrack* AddVectorColorFixture(UMovieScene& Scene, const int32 Kind, const TCHAR* Name)
	{
		UMovieSceneTrack* Track = nullptr;
		if (Kind == 0) Track = Scene.AddTrack<UMovieSceneColorTrack>();
		else if (Kind == 1) { auto* Vector = Scene.AddTrack<UMovieSceneFloatVectorTrack>(); Vector->SetNumChannelsUsed(2); Track = Vector; }
		else { auto* Vector = Scene.AddTrack<UMovieSceneDoubleVectorTrack>(); Vector->SetNumChannelsUsed(3); Track = Vector; }
		CastChecked<UMovieScenePropertyTrack>(Track)->SetDisplayName(FText::FromString(Name));
		auto* Section = Track->CreateNewSection(); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
		auto Fill = [](auto Channels) {
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				auto& Channel = *Channels[Index]; using Native = std::decay_t<decltype(Channel)>; using Value = typename Native::ChannelValueType;
				Channel.SetDefault(static_cast<typename Native::CurveValueType>(10.25 + Index));
				for (int32 KeyIndex = 0; KeyIndex < 2; ++KeyIndex)
				{
					Value Key(static_cast<typename Native::CurveValueType>(Index == 0 ? (KeyIndex ? 9007199254740991.0 : 1.0000000000000002) : 50.25 + Index + KeyIndex));
					Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_Break;
					Key.Tangent.ArriveTangent = 12.5f; Key.Tangent.LeaveTangent = -15.25f;
					Channel.GetData().AddKey(FFrameNumber(KeyIndex * 9000), Key);
				}
			}
		};
		Fill(Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>());
		Fill(Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>());
		return Track;
	}

	bool HasVectorColorLayout(UMovieSceneTrack& Track, UMovieSceneSection& Section, const int32 Kind)
	{
		const int32 Count = Kind == 0 ? 4 : Kind == 1 ? 2 : 3;
		const auto& Entries = Section.GetChannelProxy().GetAllEntries();
		if (Entries.Num() != 1 || Entries[0].GetChannels().Num() != Count || Entries[0].GetMetaData().Num() != Count
			|| Entries[0].GetChannelTypeName() != (Kind == 2 ? FMovieSceneDoubleChannel::StaticStruct() : FMovieSceneFloatChannel::StaticStruct())->GetFName()) return false;
		if (Kind == 1 && (CastChecked<UMovieSceneFloatVectorTrack>(&Track)->GetNumChannelsUsed() != Count
			|| CastChecked<UMovieSceneFloatVectorSection>(&Section)->GetChannelsUsed() != Count)) return false;
		if (Kind == 2 && (CastChecked<UMovieSceneDoubleVectorTrack>(&Track)->GetNumChannelsUsed() != Count
			|| CastChecked<UMovieSceneDoubleVectorSection>(&Section)->GetChannelsUsed() != Count)) return false;
		const TCHAR* ColorNames[] = {TEXT("Color.R"), TEXT("Color.G"), TEXT("Color.B"), TEXT("Color.A")};
		const TCHAR* VectorNames[] = {TEXT("Vector.X"), TEXT("Vector.Y"), TEXT("Vector.Z"), TEXT("Vector.W")};
		for (int32 Index = 0; Index < Count; ++Index)
			if (Entries[0].GetMetaData()[Index].Name != FName(Kind == 0 ? ColorNames[Index] : VectorNames[Index])) return false;
		return true;
	}

	UMovieSceneDoubleTrack* AddDoubleFixtureTrack(UMovieScene& Scene, const TCHAR* Name)
	{
		auto* Track = Scene.AddTrack<UMovieSceneDoubleTrack>();
		Track->SetDisplayName(FText::FromString(Name));
		auto* Section = CastChecked<UMovieSceneDoubleSection>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>::All()); Track->AddSection(*Section);
		Section->GetChannel().SetDefault(1.2345678901234567);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			FMovieSceneDoubleValue Value(Index ? 9007199254740991.0 : 1.0000000000000002);
			Value.InterpMode = RCIM_Cubic; Value.TangentMode = RCTM_Break;
			Value.Tangent.ArriveTangent = 12.5f + Index; Value.Tangent.LeaveTangent = -15.25f - Index;
			Value.Tangent.TangentWeightMode = RCTWM_WeightedBoth;
			Value.Tangent.ArriveTangentWeight = 2.5f; Value.Tangent.LeaveTangentWeight = 3.25f;
			Section->GetChannel().GetData().AddKey(FFrameNumber(Index * 9000), Value);
		}
		return Track;
	}

	TSharedPtr<FJsonObject> FindObjectFieldRecursive(
		const TSharedPtr<FJsonObject>& Root, const FString& FieldName)
	{
		if (!Root.IsValid()) return nullptr;
		const TSharedPtr<FJsonObject>* Direct = nullptr;
		if (Root->TryGetObjectField(FieldName, Direct)) return *Direct;
		TArray<TSharedPtr<FJsonValue>> Pending;
		for (const auto& Field : Root->Values) Pending.Add(Field.Value);
		while (!Pending.IsEmpty())
		{
			const auto Value = Pending.Pop(EAllowShrinking::No);
			if (!Value.IsValid()) continue;
			if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
			else if (Value->Type == EJson::Object)
			{
				const auto Object = Value->AsObject();
				if (Object->TryGetObjectField(FieldName, Direct)) return *Direct;
				for (const auto& Field : Object->Values) Pending.Add(Field.Value);
			}
		}
		return nullptr;
	}

	bool EditOrReadAssetUserData(const FString& Workspace, const bool bDesiredNullSlot,
		const bool bWrite, FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("properties.hjson"), true, false);
		int32 Matches = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
			for (const auto& Category : (*Categories)->Values)
			{
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (!Category.Value->AsObject()->TryGetObjectField(TEXT("AssetUserData"), Property)) continue;
				++Matches;
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!(*Property)->TryGetArrayField(TEXT("value"), Values)) return false;
				const bool bHasNullSlot = Values->Num() == 1 && (*Values)[0]->IsNull();
				if (bWrite)
				{
					(*Property)->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNull>()});
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, OutError)
						|| !FFileHelper::SaveStringToFile(Text, *File,
							FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
				else if (bHasNullSlot != bDesiredNullSlot) return false;
			}
		}
		return Matches == 1;
	}

	TSharedRef<FJsonObject> MakeRate(const int32 Numerator, const int32 Denominator)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("numerator"), FString::FromInt(Numerator));
		Result->SetStringField(TEXT("denominator"), FString::FromInt(Denominator));
		return Result;
	}

	TSharedRef<FJsonObject> MakePoint(
		const FString& ClockId, const int32 Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), ClockId);
		Result->SetStringField(TEXT("tick"), FString::FromInt(Tick));
		Result->SetObjectField(TEXT("rate"), MakeRate(24000, 1));
		Result->SetStringField(TEXT("state"), TEXT("authored"));
		return Result;
	}

	TSharedRef<FJsonObject> MakeRange(
		const FString& ClockId, const int32 LowerTick, const int32 UpperTick)
	{
		const TSharedRef<FJsonObject> Lower = MakeShared<FJsonObject>();
		Lower->SetStringField(TEXT("kind"), TEXT("inclusive"));
		Lower->SetObjectField(TEXT("point"), MakePoint(ClockId, LowerTick));
		const TSharedRef<FJsonObject> Upper = MakeShared<FJsonObject>();
		Upper->SetStringField(TEXT("kind"), TEXT("exclusive"));
		Upper->SetObjectField(TEXT("point"), MakePoint(ClockId, UpperTick));
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), ClockId);
		Result->SetStringField(TEXT("state"), TEXT("authored"));
		Result->SetObjectField(TEXT("lower"), Lower);
		Result->SetObjectField(TEXT("upper"), Upper);
		return Result;
	}

	TSharedRef<FJsonObject> MakeSequenceStructure(const FString& ObjectPath)
	{
		const FString ClockId = TEXT("level_sequence:") + ObjectPath;
		auto Key = [&](const FString& Ref, const int32 Tick, const TCHAR* Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("key_ref"), Ref);
			Result->SetObjectField(TEXT("time"), MakePoint(ClockId, Tick));
			Result->SetStringField(TEXT("value_type"), TEXT("decimal"));
			Result->SetStringField(TEXT("value_lexeme"), Value);
			return MakeShared<FJsonValueObject>(Result);
		};
		auto Track = [&](const FString& Ref, const FString& PropertyPath)
		{
			const TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			Section->SetStringField(TEXT("section_ref"), Ref + TEXT(":section"));
			Section->SetStringField(TEXT("section_kind"), TEXT("float"));
			Section->SetObjectField(TEXT("range"), MakeRange(ClockId, 0, 24000));
			SingleChannel(Section)->SetArrayField(TEXT("keys"), {
				Key(Ref + TEXT(":key:0"), 0, TEXT("1.25")),
				Key(Ref + TEXT(":key:1"), 12000, TEXT("2.5"))});
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("track_ref"), Ref);
			Result->SetStringField(TEXT("track_kind"), TEXT("float"));
			if (!PropertyPath.IsEmpty())
			{
				Result->SetStringField(TEXT("property_path"), PropertyPath);
			}
			Result->SetArrayField(
				TEXT("sections"), {MakeShared<FJsonValueObject>(Section)});
			return Result;
		};
		const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("binding_ref"), TEXT("CameraRig"));
		Binding->SetStringField(TEXT("binding_kind"), TEXT("possessable"));
		Binding->SetArrayField(TEXT("tracks"), {
			MakeShared<FJsonValueObject>(Track(TEXT("BoundIntensity"), TEXT("Intensity")))});
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("display_rate"), MakeRate(24, 1));
		Result->SetObjectField(TEXT("tick_resolution"), MakeRate(24000, 1));
		Result->SetObjectField(TEXT("playback_range"), MakeRange(ClockId, 0, 48000));
		Result->SetArrayField(
			TEXT("bindings"), {MakeShared<FJsonValueObject>(Binding)});
		Result->SetArrayField(TEXT("root_tracks"), {
			MakeShared<FJsonValueObject>(Track(TEXT("RootWeight"), FString()))});
		return Result;
	}

	TSharedRef<FJsonObject> MakeSequenceCreateImport(
		const FString& ObjectPath,
		const TSharedRef<FJsonObject>& Structure,
		const bool bIncludeRetiredFields = false)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("/Script/LevelSequence.LevelSequence"));
		Root->SetObjectField(TEXT("structure"), Structure);
		if (bIncludeRetiredFields)
		{
			const TSharedRef<FJsonObject> PlaybackRange = MakeShared<FJsonObject>();
			PlaybackRange->SetStringField(TEXT("intent"), TEXT("preserve"));
			const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
			Fields->SetObjectField(TEXT("playback_range"), PlaybackRange);
			Root->SetObjectField(TEXT("fields"), Fields);
		}
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.sequence.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		FString Error;
		CommandContract::Json::TryCanonicalize(Document, DocumentText, Error);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), DocumentText);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), ObjectPath);
		Params->SetObjectField(TEXT("source"), Source);
		return Params;
	}

	void AddSequenceSurvivor(ULevelSequence& Sequence)
	{
		auto* Track = CastChecked<UMovieSceneFloatTrack>(Sequence.GetMovieScene()->GetTracks()[0]);
		auto* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>(FFrameNumber(24000), FFrameNumber(36000)));
		Track->AddSection(*Section);
		auto Data = Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData();
		for (int32 Index = 0; Index < 2; ++Index)
		{
			FMovieSceneFloatValue Value(5.f + Index);
			Value.InterpMode = RCIM_Cubic;
			Value.TangentMode = RCTM_Break;
			Value.Tangent.ArriveTangent = 12.5f + Index;
			Value.Tangent.LeaveTangent = -13.25f - Index;
			Data.AddKey(FFrameNumber(24000 + Index * 6000), Value);
		}
	}

	// Expected refs are the reader's documented native order, fixed before import.
	void ReindexExpectedSections(const TSharedPtr<FJsonObject>& Track)
	{
		const FString TrackRef = Track->GetStringField(TEXT("track_ref"));
		const auto Sections = Track->GetArrayField(TEXT("sections"));
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			const FString Ref = FString::Printf(TEXT("%s:section:%d"), *TrackRef, Index);
			const auto Section = Sections[Index]->AsObject();
			Section->SetStringField(TEXT("section_ref"), Ref);
			const auto Channel = SingleChannel(Section);
			const auto Defaults = Track->GetArrayField(TEXT("default_channels"))[0]->AsObject();
			for (const FString Field : {FString(TEXT("default_key")), FString(TEXT("key_properties_definition")), FString(TEXT("default")), FString(TEXT("properties")), FString(TEXT("properties_definition"))})
				if (!Channel->HasField(Field) && Defaults->HasField(Field)) Channel->SetField(Field, Defaults->TryGetField(Field));
			const auto Keys = Channel->GetArrayField(TEXT("keys"));
			for (const auto& Key : Keys) if (!Key->AsObject()->HasField(TEXT("properties")))
				Key->AsObject()->SetField(TEXT("properties"), Defaults->GetObjectField(TEXT("default_key"))->TryGetField(TEXT("properties")));
			for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
				Keys[KeyIndex]->AsObject()->SetStringField(TEXT("key_ref"), FString::Printf(TEXT("%s:key:%d"), *SingleChannel(Section)->GetStringField(TEXT("channel_ref")), KeyIndex));
		}
	}

	TSharedRef<FJsonObject> MakeAddedSection(const FString& ObjectPath)
	{
		const FString Clock = TEXT("level_sequence:") + ObjectPath;
		const auto Key = MakeShared<FJsonObject>();
		Key->SetStringField(TEXT("key_ref"), TEXT("AddedKey"));
		Key->SetObjectField(TEXT("time"), MakePoint(Clock, 7000));
		Key->SetStringField(TEXT("value_type"), TEXT("decimal"));
		Key->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
		const auto Section = MakeShared<FJsonObject>();
		Section->SetStringField(TEXT("section_ref"), TEXT("AddedSection"));
		Section->SetStringField(TEXT("section_kind"), UMovieSceneFloatTrack::StaticClass()->GetName());
		Section->SetObjectField(TEXT("range"), MakeRange(Clock, 6000, 18000));
		const auto Channel = MakeShared<FJsonObject>();
		Channel->SetStringField(TEXT("channel_ref"), TEXT("MovieSceneFloatChannel:0"));
		Channel->SetStringField(TEXT("name"), TEXT("Value"));
		Channel->SetStringField(TEXT("channel_kind"), TEXT("float"));
		Channel->SetArrayField(TEXT("keys"), {MakeShared<FJsonValueObject>(Key)});
		Section->SetArrayField(TEXT("channels"), {MakeShared<FJsonValueObject>(Channel)});
		return Section;
	}

	bool HasCreatedSequenceStructure(const ULevelSequence* Sequence)
	{
		const UMovieScene* Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (!Scene || Scene->GetDisplayRate() != FFrameRate(24, 1)
			|| Scene->GetTickResolution() != FFrameRate(24000, 1)
			|| Scene->GetPlaybackRange().GetLowerBoundValue().Value != 0
			|| Scene->GetPlaybackRange().GetUpperBoundValue().Value != 48000
			|| Scene->GetBindings().Num() != 1 || Scene->GetTracks().Num() != 1
			|| Scene->GetBindings()[0].GetTracks().Num() != 1)
		{
			return false;
		}
		for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{
			Scene->GetTracks()[0], Scene->GetBindings()[0].GetTracks()[0]})
		{
			const UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
			const UMovieSceneFloatSection* Section = FloatTrack && FloatTrack->GetAllSections().Num() == 1
				? Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]) : nullptr;
			const FMovieSceneFloatChannel* Channel = Section
				? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
			if (!Channel || Channel->GetData().GetTimes().Num() != 2
				|| Channel->GetData().GetTimes()[0].Value != 0
				|| Channel->GetData().GetTimes()[1].Value != 12000
				|| !FMath::IsNearlyEqual(Channel->GetData().GetValues()[0].Value, 1.25f)
				|| !FMath::IsNearlyEqual(Channel->GetData().GetValues()[1].Value, 2.5f))
			{
				return false;
			}
		}
		return true;
	}
}

namespace
{
	bool IsSequenceAbsent(const FString& ObjectPath, const FString& Filename)
	{
		return !FindObject<UObject>(nullptr, *ObjectPath)
			&& !IFileManager::Get().FileExists(*Filename)
			&& !FAssetRegistryModule::GetRegistry()
				.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	bool RunLevelSequenceCreateRecovery(FAutomationTestBase& Test, const bool bEarly)
	{
		using namespace UE::UAI::DomainAdapters::Sequence;
		using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentAuthoringWorkspaceTransaction;
		const FString Name = TEXT("LS_CreateRecovery_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName =
			TEXT("/Game/UAI/Transient/T85LevelSequenceCreate/") + Name;
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		ON_SCOPE_EXIT
		{
			if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath))
			{
				Test.TestEqual(TEXT("remaining owned create fixture deletes through UE"),
					ObjectTools::ForceDeleteObjects({Remaining}, false), 1);
			}
			T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		};
		if (!Test.TestTrue(TEXT("Sequence create starts from observed absence"),
			IsSequenceAbsent(ObjectPath, Filename)))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Structure = MakeSequenceStructure(ObjectPath);
		TWeakObjectPtr<ULevelSequence> StagedWitness;
		bool bStageObserved = false;
		bool bFinalReadbackCalled = false;
		bool bLateObserved = false;
		class FRejectSequenceCreateStage final : public IDurablePackageStager
		{
		public:
			FString ObjectPath;
			FString Filename;
			bool* bObserved = nullptr;
			bool bReject = false;
			TWeakObjectPtr<ULevelSequence>* Witness = nullptr;
			TSharedRef<IDurablePackageStager> Inner = CreateDefaultDurablePackageStager();
			bool BuildArtifactInventory(
				const TArray<FPackageDurablePlan>& Plans,
				TArray<FDurableArtifact>& Artifacts,
				FOperationFailure& Failure) override
			{
				return Inner->BuildArtifactInventory(Plans, Artifacts, Failure);
			}
			bool StagePackages(
				const TArray<FPackageDurablePlan>& Plans,
				TArray<FDurableArtifact>& Artifacts,
				FOperationFailure& Failure) override
			{
				if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
				ULevelSequence* Actual = FindObject<ULevelSequence>(nullptr, *ObjectPath);
				// Absent before-checkpoint staging precedes creation and must pass.
				if (!Actual) return true;
				bool bAnyStaged = false;
				bool bValid = HasCreatedSequenceStructure(Actual)
					&& Actual->GetOutermost()->IsDirty()
					&& !IFileManager::Get().FileExists(*Filename)
					&& FAssetRegistryModule::GetRegistry()
						.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
				for (const FDurableArtifact& Artifact : Artifacts)
				{
					bValid &= IFileManager::Get().FileExists(*Artifact.StagingPath)
						== Artifact.bStagedExists;
					bAnyStaged |= Artifact.bStagedExists;
				}
				*bObserved = bValid && bAnyStaged;
				*Witness = Actual;
				if (!bReject) return true;
				Failure.Diagnostic.Code = TEXT("t85_sequence_create_stage_rejected");
				Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
				return false;
			}
		};
		const auto Stager = MakeShared<FRejectSequenceCreateStage>();
		Stager->ObjectPath = ObjectPath;
		Stager->Filename = Filename;
		Stager->bObserved = &bStageObserved;
		Stager->Witness = &StagedWitness;
		Stager->bReject = bEarly;
		FBackendOptions Options;
		Options.PackageStager = Stager;
		FRootPropertyApplyOutcome Outcome;
		Outcome.FinalReadback = [&](FOperationFailure& Failure)
		{
			bFinalReadbackCalled = true;
			ULevelSequence* Reloaded = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
			bLateObserved = !bEarly && bStageObserved
				&& !StagedWitness.IsValid(true, true)
				&& HasCreatedSequenceStructure(Reloaded)
				&& Reloaded && !Reloaded->GetOutermost()->IsDirty()
				&& IFileManager::Get().FileExists(*Filename)
				&& FAssetRegistryModule::GetRegistry()
					.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
			Failure.Diagnostic.Code = TEXT("t85_sequence_create_late_rejected");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		};
		const bool bApplied = ApplySequenceStructureAuthoring(
			PackageName,
			(bEarly ? TEXT("t85-sequence-create-early-")
				: TEXT("t85-sequence-create-late-")) + Name,
			Structure,
			Outcome,
			&Options);
		if (!Test.TestFalse(TEXT("Sequence create rejects at requested phase"), bApplied)
			|| !Test.TestEqual(TEXT("requested failure is retained"), Outcome.Diagnostic.Code,
				FString(bEarly ? TEXT("t85_sequence_create_stage_rejected")
					: TEXT("t85_sequence_create_late_rejected")))
			|| !Test.TestTrue(TEXT("requested lifecycle phase is observed"),
				bEarly ? (bStageObserved && !bFinalReadbackCalled)
					: (bFinalReadbackCalled && bLateObserved))
			|| !Test.TestTrue(TEXT("failed create restores observed absence"),
				IsSequenceAbsent(ObjectPath, Filename)))
		{
			Test.AddError(Outcome.Phase + TEXT(":") + Outcome.Diagnostic.Code
				+ TEXT(":") + Outcome.Diagnostic.PublicMessage);
			return false;
		}
		FAssetRegistryModule::TickAssetRegistry(0.15f);
		return Test.TestTrue(TEXT("restored absence survives registry tick"),
			IsSequenceAbsent(ObjectPath, Filename));
	}

	bool RunLevelSequenceRootRecovery(FAutomationTestBase& Test, const bool bEarly,
		const bool bStructure = false, const bool bTime = false,
		const bool bKeyCollection = false, const bool bSectionCollection = false, const bool bOpenRange = false,
		const bool bTrackCollection = false, const bool bDoubleNumeric = false, const bool bTransformNumeric = false, const bool bKeyProperties = false, const bool bChannelSettings = false, const bool bBool = false, const bool bInteger = false, const bool bByteEnum = false, const bool bVector = false, const bool bString = false, const bool bObjectPath = false, const bool bObjectProperties = false, const bool bGroupCondition = false)
	{
		const bool bPrimitive = bBool || bInteger || bByteEnum;
		const bool bNumeric = bDoubleNumeric || bTransformNumeric || bPrimitive || bVector || bString || bObjectPath;
		const FString NumericName = bVector ? TEXT("Vector.X") : bTransformNumeric ? TEXT("Location.X") : TEXT("Value");
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentAuthoringWorkspaceTransaction;
		using namespace UeAgentReflectionAuthoring;
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!Test.TestTrue(TEXT("reflection authoring registered"),
			RegisterAuthoring(RegistrationError)))
		{
			Test.AddError(RegistrationError.ToPublicDetail());
			return false;
		}
		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TArray<FString> ReferenceAssets;
		const FString PackageName = TEXT("/Game/UAI/Transient/T85LevelSequenceRootRecovery/LS_") + Token;
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		ON_SCOPE_EXIT
		{
			DeleteAssetIfPresent(ObjectPath);
			T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
			for (const auto& Path : ReferenceAssets) DeleteAssetIfPresent(Path);
			if (!ReferenceAssets.IsEmpty()) T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, FPackageName::ObjectPathToPackageName(ReferenceAssets[0]));
		};
		if (bObjectPath && !CreateReferenceAssets(Test, Token, ReferenceAssets)) return false;

		UPackage* Package = CreatePackage(*PackageName);
		ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		Sequence->Initialize();
		if (bStructure)
		{
			using namespace UE::UAI::DomainAdapters::TimelineAnimation;
			FTimelineAnimationStructure Structure;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Structure, Error)
				|| !TryApplyLevelSequenceStructure(*Sequence, Structure, ObjectPath, Error)) return false;
			for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{
				Sequence->GetMovieScene()->GetTracks()[0], Sequence->GetMovieScene()->GetBindings()[0].GetTracks()[0]})
			{
				auto* Section = CastChecked<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
				for (auto& Value : Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData().GetValues())
				{
					Value.InterpMode = RCIM_Cubic;
					Value.TangentMode = RCTM_User;
					Value.Tangent.ArriveTangent = 2.25f;
					Value.Tangent.LeaveTangent = -1.75f;
				}
			}
		}
		FAssetRegistryModule::AssetCreated(Sequence);
		const FGuid OriginalFixtureBindingGuid = bStructure ? Sequence->GetMovieScene()->GetBindings()[0].GetObjectGuid() : FGuid();
		if (bSectionCollection || bOpenRange) AddSequenceSurvivor(*Sequence);
		TUniquePtr<FPreviewScene> Preview;
		APointLight* BoundActor = nullptr;
		UPointLightComponent* BoundComponent = nullptr;
		FGuid RealGuid, RealActorGuid;
		if (bTrackCollection || bNumeric || bObjectProperties)
		{
			Preview = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
			BoundActor = Preview->GetWorld()->SpawnActor<APointLight>();
			BoundComponent = BoundActor ? Cast<UPointLightComponent>(BoundActor->GetLightComponent()) : nullptr;
			if (!BoundComponent) return false;
			{
				const auto Sequencer = OpenFixtureSequencer(Sequence, Preview->GetWorld());
				ON_SCOPE_EXIT { Sequencer->Close(); };
				RealGuid = Sequencer->GetHandleToObject(BoundComponent, true);
				RealActorGuid = Sequencer->GetHandleToObject(BoundActor, false);
			}
			if (bNumeric) { if (bObjectPath) AddObjectFixtureTrack(*Sequence->GetMovieScene(), ReferenceAssets[0]);
				else if (bString) AddStringFixtureTrack(*Sequence->GetMovieScene());
				else if (bByteEnum) AddByteEnumFixtureTrack(*Sequence->GetMovieScene(), ByteFixtureEnum());
				else if (bInteger) AddIntegerFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise"));
				else if (bBool) AddBoolFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise"));
				else if (bVector) AddVectorColorFixture(*Sequence->GetMovieScene(), 2, TEXT("Precise"));
				else if (bTransformNumeric) AddTransformFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise"));
				else AddDoubleFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise")); }
			else if (bTrackCollection)
			{
			CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0])->SetDisplayName(FText::FromString(TEXT("Shared")));
			auto* Survivor = Sequence->GetMovieScene()->AddTrack<UMovieSceneFloatTrack>();
			Survivor->SetDisplayName(FText::FromString(TEXT("Shared")));
			auto* Section = CastChecked<UMovieSceneFloatSection>(Survivor->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>::All()); Survivor->AddSection(*Section);
			Section->GetChannel().SetDefault(71.5f);
			FMovieSceneFloatValue Value(19.25f); Value.InterpMode = RCIM_Cubic; Value.TangentMode = RCTM_Break;
			Value.Tangent.ArriveTangent = 12.5f; Value.Tangent.LeaveTangent = -9.75f;
			Section->GetChannel().GetData().AddKey(FFrameNumber(45), Value);
			}
		}
		if (bGroupCondition) AddGroupConditionFixture(*Sequence->GetMovieScene()->GetTracks()[0]);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!Test.TestTrue(TEXT("recovery baseline saved"),
			UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))) return false;
		Package->SetDirtyFlag(false);
		TArray<uint8> DiskBefore;
		if (!Test.TestTrue(TEXT("recovery baseline disk captured"),
			FFileHelper::LoadFileToArray(DiskBefore, *Filename))) return false;

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->Modify();
		MovieScene->SetPlaybackRange(17, 31);
		if (bObjectProperties)
		{
			auto* Track = MovieScene->GetTracks()[0]; Track->Modify(); Track->SetColorTint(FColor(13,27,41,255));
			Track->EvalOptions.bEvaluateInPostroll = true;
			auto* Section = Track->GetAllSections()[0]; Section->Modify(); Section->SetPreRollFrames(87);
			if (bGroupCondition) {
				auto* Group = CastChecked<UMovieSceneGroupCondition>(Track->ConditionContainer.Condition); Group->Modify(); Group->Operator = EMovieSceneGroupConditionOperator::Or;
				auto* Child = CastChecked<UMovieSceneGroupCondition>(Group->SubConditions[0].Condition); Child->Modify(); Child->Operator = EMovieSceneGroupConditionOperator::Xor;
			}
		}
		if (bObjectPath)
		{
			auto* Section = MovieScene->GetTracks()[1]->GetAllSections()[0]; Section->Modify();
			auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
			Channel->SetDefault(LoadObject<UDataAsset>(nullptr, *ReferenceAssets[1])); Channel->GetData().GetValues()[0] = nullptr;
		}
		else if (bString)
		{
			auto* Section = MovieScene->GetTracks()[1]->GetAllSections()[0]; Section->Modify();
			auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
			Channel->SetDefault(TEXT("dirty默认")); Channel->GetData().GetValues()[0] = TEXT("dirty内存");
		}
		else if (bPrimitive)
		{
			auto* Section = MovieScene->GetTracks()[1]->GetAllSections()[0]; Section->Modify();
			if (!VisitPrimitiveFixture(Section, [bInteger, bByteEnum](auto& Channel) { using Value = typename std::decay_t<decltype(Channel)>::CurveValueType;
				Channel.SetDefault(static_cast<Value>(bByteEnum ? 4 : bInteger ? -15 : true)); Channel.GetData().GetValues()[0] = static_cast<Value>(bByteEnum ? 201 : bInteger ? -17 : true); return true; })) return false;
		}
		else if (bNumeric)
		{
			auto* Section = MovieScene->GetTracks()[1]->GetAllSections()[0];
			Section->Modify(); (*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).SetDefault(2.3456789012345678);
			(*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues()[0].Value = 1.0000000000000004;
			(*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues()[0].Tangent.ArriveTangent = 23.75f;
		}
		if (bTrackCollection)
		{
			auto* Section = CastChecked<UMovieSceneFloatSection>(MovieScene->GetTracks()[1]->GetAllSections()[0]);
			Section->Modify();
			Section->GetChannel().SetDefault(87.25f);
			auto& Value = Section->GetChannel().GetData().GetValues()[0];
			Value.Value = 33.75f; Value.Tangent.ArriveTangent = 29.5f;
		}
		TArray<FMovieSceneFloatValue> DesiredKeyValues;
		FGuid DesiredBindingGuid;
		if (bTime || bKeyCollection || bSectionCollection || bOpenRange)
		{
			DesiredBindingGuid = MovieScene->GetBindings()[0].GetObjectGuid();
			for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{MovieScene->GetTracks()[0], MovieScene->GetBindings()[0].GetTracks()[0]})
			{
				auto* Section = CastChecked<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
				Section->Modify();
				for (auto& Value : Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData().GetValues())
				{
					// Dirty keys deliberately differ from the original disk checkpoint.
					Value.Value += 10.f + DesiredKeyValues.Num();
					Value.TangentMode = DesiredKeyValues.Num() % 2 ? RCTM_Break : RCTM_User;
					Value.Tangent.ArriveTangent += 0.5f + DesiredKeyValues.Num();
					Value.Tangent.LeaveTangent -= 0.25f + DesiredKeyValues.Num();
					DesiredKeyValues.Add(Value);
				}
			}
			if (bTime) DesiredKeyValues.Swap(0, 1);
		}
		FString ExpectedDesiredStructure;
		if (bTime || bKeyCollection)
		{
			TSharedPtr<FJsonObject> Expected;
			UE::UAI::DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
			if (!UE::UAI::DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, Expected, NativeError)) return false;
			const auto Section = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject();
			auto Keys = SingleChannel(Section)->GetArrayField(TEXT("keys"));
			Keys[0]->AsObject()->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), bKeyCollection ? TEXT("6000") : TEXT("18000"));
			if (bKeyCollection) Keys[0]->AsObject()->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
			else
			{
				const FString FirstRef = Keys[0]->AsObject()->GetStringField(TEXT("key_ref"));
				const FString SecondRef = Keys[1]->AsObject()->GetStringField(TEXT("key_ref"));
				Keys.Swap(0, 1);
				Keys[0]->AsObject()->SetStringField(TEXT("key_ref"), FirstRef);
				Keys[1]->AsObject()->SetStringField(TEXT("key_ref"), SecondRef);
			}
			SingleChannel(Section)->SetArrayField(TEXT("keys"), Keys);
			FString CanonicalError;
			if (!UE::UAI::CommandContract::Json::TryCanonicalize(Expected, ExpectedDesiredStructure, CanonicalError)) return false;
		}
		const auto DesiredKeysMatch = [DesiredKeyValues, DesiredBindingGuid, bKeyCollection, ObjectPath, ExpectedDesiredStructure](ULevelSequence* Actual)
		{
			if (!Actual || !Actual->GetMovieScene() || DesiredKeyValues.Num() != 4) return false;
			TSharedPtr<FJsonObject> Readback;
			UE::UAI::DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
			FString ActualStructure, CanonicalError;
			if (!UE::UAI::DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Actual, ObjectPath, Readback, NativeError)
				|| !UE::UAI::CommandContract::Json::TryCanonicalize(Readback, ActualStructure, CanonicalError)
				|| ActualStructure != ExpectedDesiredStructure) return false;
			const UMovieScene* Scene = Actual->GetMovieScene();
			if (Scene->GetTracks().Num() != 1 || Scene->GetBindings().Num() != 1
				|| Scene->GetBindings()[0].GetTracks().Num() != 1
				|| Scene->GetBindings()[0].GetObjectGuid() != DesiredBindingGuid) return false;
			if (bKeyCollection)
			{
				const auto* RootSection = Cast<UMovieSceneFloatSection>(Scene->GetTracks()[0]->GetAllSections()[0]);
				const auto* BoundSection = Cast<UMovieSceneFloatSection>(Scene->GetBindings()[0].GetTracks()[0]->GetAllSections()[0]);
				const auto* RootChannel = RootSection ? RootSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
				const auto* BoundChannel = BoundSection ? BoundSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
				if (!RootChannel || !BoundChannel) return false;
				const auto Root = RootChannel->GetData();
				const auto Bound = BoundChannel->GetData();
				return Root.GetTimes().Num() == 2 && Root.GetTimes()[0].Value == 6000
					&& Root.GetTimes()[1].Value == 12000
					&& Root.GetValues()[0] == FMovieSceneFloatValue(9.5f)
					&& Root.GetValues()[1] == DesiredKeyValues[1]
					&& Bound.GetTimes().Num() == 2 && Bound.GetTimes()[0].Value == 0
					&& Bound.GetTimes()[1].Value == 12000
					&& Bound.GetValues().Num() == 2
					&& Bound.GetValues()[0] == DesiredKeyValues[2]
					&& Bound.GetValues()[1] == DesiredKeyValues[3];
			}
			int32 TrackIndex = 0;
			for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{Scene->GetTracks()[0], Scene->GetBindings()[0].GetTracks()[0]})
			{
				if (Track->GetAllSections().Num() != 1) return false;
				const auto* Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
				const auto* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
				if (!Channel || Channel->GetData().GetTimes().Num() != 2) return false;
				const auto Data = Channel->GetData();
				if (Data.GetTimes()[0].Value != (TrackIndex == 0 ? 12000 : 0)
					|| Data.GetTimes()[1].Value != (TrackIndex == 0 ? 18000 : 12000)
					|| Data.GetValues()[0] != DesiredKeyValues[TrackIndex * 2]
					|| Data.GetValues()[1] != DesiredKeyValues[TrackIndex * 2 + 1]) return false;
				++TrackIndex;
			}
			return true;
		};
		TFunction<bool(ULevelSequence*)> DesiredSectionsMatch;
		if (bSectionCollection || bOpenRange)
		{
			TSharedPtr<FJsonObject> Expected;
			DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
			if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, Expected, NativeError)) return false;
			const auto Track = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
			auto Sections = Track->GetArrayField(TEXT("sections"));
			if (bOpenRange)
			{
				const auto Range = Sections[0]->AsObject()->GetObjectField(TEXT("range"));
				for (const FString Side : {FString(TEXT("lower")), FString(TEXT("upper"))})
				{ const auto Bound = MakeShared<FJsonObject>(); Bound->SetStringField(TEXT("kind"), TEXT("open")); Range->SetObjectField(Side, Bound); }
			}
			else
			{
				Sections.RemoveAt(0);
				Sections.Add(MakeShared<FJsonValueObject>(MakeAddedSection(ObjectPath)));
			}
			Track->SetArrayField(TEXT("sections"), Sections);
			ReindexExpectedSections(Track);
			FString ExpectedText, Error;
			if (!CommandContract::Json::TryCanonicalize(Expected, ExpectedText, Error)) return false;
			const auto Survivor = CapturePersistentObject(*MovieScene->GetTracks()[0]->GetAllSections()[1]);
			const auto BoundTrack = CapturePersistentObject(*MovieScene->GetBindings()[0].GetTracks()[0]);
			const auto BoundSection = CapturePersistentObject(*MovieScene->GetBindings()[0].GetTracks()[0]->GetAllSections()[0]);
			DesiredSectionsMatch = [ExpectedText, ObjectPath, Survivor, BoundTrack, BoundSection, DesiredBindingGuid, bOpenRange](ULevelSequence* Actual) {
				if (!Actual || !Actual->GetMovieScene()) return false;
				TSharedPtr<FJsonObject> Readback;
				DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
				FString Text, Error;
				if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Actual, ObjectPath, Readback, NativeError)
					|| !CommandContract::Json::TryCanonicalize(Readback, Text, Error) || Text != ExpectedText) return false;
				const auto* Scene = Actual->GetMovieScene();
				return Scene->GetBindings()[0].GetObjectGuid() == DesiredBindingGuid
					&& Survivor.OrderIndependentCompareEqual(CapturePersistentObject(*Scene->GetTracks()[0]->GetAllSections()[bOpenRange ? 1 : 0]))
					&& BoundTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Scene->GetBindings()[0].GetTracks()[0]))
					&& BoundSection.OrderIndependentCompareEqual(CapturePersistentObject(*Scene->GetBindings()[0].GetTracks()[0]->GetAllSections()[0]));
			};
		}
		TFunction<bool(ULevelSequence*)> DesiredDoubleMatch;
		if (bObjectPath)
		{
			const auto FloatTrack = CapturePersistentObject(*MovieScene->GetTracks()[0]);
			const auto FloatSection = CapturePersistentObject(*MovieScene->GetTracks()[0]->GetAllSections()[0]);
			DesiredDoubleMatch = [ReferenceAssets, FloatTrack, FloatSection](ULevelSequence* Actual) {
				if (!Actual || Actual->GetMovieScene()->GetTracks().Num() != 2) return false;
				const auto* Track = Cast<UMovieSceneObjectPropertyTrack>(Actual->GetMovieScene()->GetTracks()[1]);
				if (!Track || Track->PropertyClass != UDataAsset::StaticClass() || Track->bClassProperty) return false;
				const auto* Channel = Track->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
				if (!Channel || Channel->GetPropertyClass() != UDataAsset::StaticClass()) return false;
				const auto Values = Channel->GetData().GetValues(); const auto Times = Channel->GetData().GetTimes();
				return Values.Num() == 3 && Values[0].GetSoftPtr().ToString() == ReferenceAssets[1] && Times[0] == FFrameNumber(6000)
					&& Values[1].GetSoftPtr().IsNull() && Times[1] == FFrameNumber(9000)
					&& Values[2].GetSoftPtr().ToString() == TEXT("/Game/UAI/Transient/AbsentObject.AbsentObject") && Times[2] == FFrameNumber(18000)
					&& Channel->GetDefault().GetSoftPtr().ToString() == ReferenceAssets[0]
					&& FloatTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]))
					&& FloatSection.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]));
			};
		}
		else if (bString)
		{
			const auto FloatTrack = CapturePersistentObject(*MovieScene->GetTracks()[0]);
			const auto FloatSection = CapturePersistentObject(*MovieScene->GetTracks()[0]->GetAllSections()[0]);
			DesiredDoubleMatch = [FloatTrack, FloatSection](ULevelSequence* Actual) {
				if (!Actual || Actual->GetMovieScene()->GetTracks().Num() != 2) return false;
				const auto* Channel = Actual->GetMovieScene()->GetTracks()[1]->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
				if (!Channel) return false;
				const auto Values = Channel->GetData().GetValues(); const auto Times = Channel->GetData().GetTimes();
				return Values.Num() == 2 && Values[0] == TEXT("已持久\"文本\"\n新行") && Times[0] == FFrameNumber(6000)
					&& Values[1] == TEXT("保留") && Times[1] == FFrameNumber(9000) && Channel->GetDefault().IsSet() && Channel->GetDefault().GetValue().IsEmpty()
					&& FloatTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]))
					&& FloatSection.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]));
			};
		}
		else if (bPrimitive)
		{
			const auto FloatTrack = CapturePersistentObject(*MovieScene->GetTracks()[0]);
			const auto FloatSection = CapturePersistentObject(*MovieScene->GetTracks()[0]->GetAllSections()[0]);
			DesiredDoubleMatch = [FloatTrack, FloatSection, bInteger, bByteEnum](ULevelSequence* Actual) {
				if (!Actual || Actual->GetMovieScene()->GetTracks().Num() != 2) return false;
				const auto* Track = Actual->GetMovieScene()->GetTracks()[1];
				if (Track->GetAllSections().Num() != 1) return false;
				const bool bValues = VisitPrimitiveFixture(Track->GetAllSections()[0], [bInteger, bByteEnum](auto& Channel) {
					using Native = std::decay_t<decltype(Channel)>; using Value = typename Native::CurveValueType;
					const auto Times = Channel.GetData().GetTimes(); const auto Values = Channel.GetData().GetValues();
					bool bInterpolation = true; if constexpr (std::is_same_v<Value, int32>) bInterpolation = Channel.bInterpolateLinearKeys;
					if constexpr (std::is_same_v<Value, uint8>) bInterpolation = Channel.GetEnum() && Channel.GetEnum()->GetValueByNameString(TEXT("HLOD7")) == 128;
					return bInterpolation && Times.Num() == 2 && Times[0] == FFrameNumber(6000) && Times[1] == FFrameNumber(9000)
						&& Values[0] == static_cast<Value>(bByteEnum ? 128 : bInteger ? -31 : false) && Values[1] == static_cast<Value>(bByteEnum ? 4 : bInteger ? MAX_int32 : true)
						&& Channel.GetDefault().IsSet() && Channel.GetDefault().GetValue() == static_cast<Value>(bByteEnum ? 1 : bInteger ? MIN_int32 : false)
						&& Channel.PreInfinityExtrap == RCCE_Linear && Channel.PostInfinityExtrap == RCCE_Cycle;
				});
				return bValues && FloatTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]))
					&& FloatSection.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]));
			};
		}
		else if (bNumeric)
		{
			const auto* Section = MovieScene->GetTracks()[1]->GetAllSections()[0];
			TArray<FMovieSceneDoubleValue> Values; Values.Append((*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues());
			if (!bChannelSettings) Values[0].Value = 2.0000000000000004;
			if (bKeyProperties) { Values[0].InterpMode = RCIM_Constant; Values[0].TangentMode = RCTM_User; Values[0].Tangent.LeaveTangent = 67.25f; }
			const auto Default = bChannelSettings ? TOptional<double>(3.1415926535897931) : (*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault();
			const FString TrackName = MovieScene->GetTracks()[1]->GetName();
			const auto FloatTrack = CapturePersistentObject(*MovieScene->GetTracks()[0]);
			const auto FloatSection = CapturePersistentObject(*MovieScene->GetTracks()[0]->GetAllSections()[0]);
			const auto OtherChannels = CaptureOtherChannels(*const_cast<UMovieSceneSection*>(Section), NumericName);
			DesiredDoubleMatch = [Values, Default, TrackName, FloatTrack, FloatSection, OtherChannels, NumericName, bVector, bChannelSettings](ULevelSequence* Actual) {
				if (!Actual || Actual->GetMovieScene()->GetTracks().Num() != 2) return false;
				const auto* Track = Actual->GetMovieScene()->GetTracks()[1];
				const auto* Read = Track->GetName() == TrackName && Track->GetAllSections().Num() == 1
					? Track->GetAllSections()[0] : nullptr;
				if (!Read) return false;
				if (bVector && !HasVectorColorLayout(*const_cast<UMovieSceneTrack*>(Track), *const_cast<UMovieSceneSection*>(Read), 2)) return false;
				TArray<FMovieSceneDoubleValue> Current; Current.Append((*Read->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues());
				const auto Times = (*Read->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetTimes();
				return OtherChannels.OrderIndependentCompareEqual(CaptureOtherChannels(*const_cast<UMovieSceneSection*>(Read), NumericName)) && Current == Values && Times.Num() == 2 && Times[0] == FFrameNumber(bChannelSettings ? 0 : 6000) && Times[1] == FFrameNumber(9000)
					&& (!bChannelSettings || (Read->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)->PreInfinityExtrap == RCCE_CycleWithOffset
						&& Read->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)->PostInfinityExtrap == RCCE_Linear))
					&& (*Read->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault() == Default
					&& FloatTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]))
					&& FloatSection.OrderIndependentCompareEqual(CapturePersistentObject(*Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]));
			};
		}
		TFunction<bool(ULevelSequence*)> DesiredTracksMatch;
		if (bTrackCollection)
		{
			const auto Survivor = CapturePersistentObject(*MovieScene->GetTracks()[1]->GetAllSections()[0]);
			const auto* OriginalBinding = MovieScene->FindBinding(OriginalFixtureBindingGuid);
			if (!Test.TestTrue(TEXT("original unrelated binding has its fixture track"), OriginalBinding && OriginalBinding->GetTracks().Num() == 1)) return false;
			const auto OtherTrack = CapturePersistentObject(*OriginalBinding->GetTracks()[0]);
			const auto OtherSection = CapturePersistentObject(*OriginalBinding->GetTracks()[0]->GetAllSections()[0]);
			const FGuid OtherGuid = OriginalFixtureBindingGuid;
			const FString SurvivorName = MovieScene->GetTracks()[1]->GetName();
			DesiredTracksMatch = [&, Survivor, OtherTrack, OtherSection, OtherGuid, SurvivorName](ULevelSequence* Actual) {
				if (!Actual || !Actual->GetMovieScene()) return false;
				const auto* Current = Actual->GetMovieScene();
				const auto* Other = Current->FindBinding(OtherGuid);
				if (Current->GetTracks().Num() != 1 || !Other || Other->GetTracks().Num() != 1) return false;
				const auto* Track = Current->GetTracks()[0];
				return Track->GetName() == SurvivorName && Track->GetDisplayName().ToString() == TEXT("Renamed")
					&& Track->GetAllSections().Num() == 1
					&& Survivor.OrderIndependentCompareEqual(CapturePersistentObject(*Track->GetAllSections()[0]))
					&& OtherTrack.OrderIndependentCompareEqual(CapturePersistentObject(*Other->GetTracks()[0]))
					&& OtherSection.OrderIndependentCompareEqual(CapturePersistentObject(*Other->GetTracks()[0]->GetAllSections()[0]));
			};
		}
		Sequence->MarkPackageDirty();
		const TMap<FString, FString> SequenceBefore = CapturePersistentObject(*Sequence);
		const TMap<FString, FString> MovieSceneBefore = CapturePersistentObject(*MovieScene);
		TMap<FString, TMap<FString, FString>> OwnedBefore;
		if (bStructure)
		{
			TArray<UObject*> Owned;
			GetObjectsWithOuter(MovieScene, Owned, true);
			for (UObject* Object : Owned)
				OwnedBefore.Add(Object->GetPathName(MovieScene), CapturePersistentObject(*Object));
		}
		TWeakObjectPtr<ULevelSequence> OldSequence = Sequence;
		TWeakObjectPtr<UMovieSceneGroupCondition> OldGroup, OldChild;
		if (bGroupCondition) {
			auto* Group = CastChecked<UMovieSceneGroupCondition>(Sequence->GetMovieScene()->GetTracks()[0]->ConditionContainer.Condition);
			OldGroup = Group; OldChild = CastChecked<UMovieSceneGroupCondition>(Group->SubConditions[0].Condition);
		}
		TWeakObjectPtr<UMovieSceneTrack> OldRemovedTrack = bTrackCollection ? MovieScene->GetTracks()[0] : nullptr;
		TWeakObjectPtr<UMovieSceneSection> OldRemovedSection = (bSectionCollection || bOpenRange)
			? MovieScene->GetTracks()[0]->GetAllSections()[0] : nullptr;
		TWeakObjectPtr<ULevelSequence> DesiredStageWitness;
		bool bDesiredStageObserved = false;
		bool bFinalReadbackCalled = false;

		class FSequenceStageDecorator final : public IDurablePackageStager
		{
		public:
			FString ObjectPath;
			FString Filename;
			TArray<uint8> DiskBefore;
			bool bReject = false;
			bool bStructure = false;
			TFunction<bool(ULevelSequence*)> DesiredKeysMatch;
			bool* bObserved = nullptr;
			TWeakObjectPtr<ULevelSequence>* Witness = nullptr;
			TSharedRef<IDurablePackageStager> Inner = CreateDefaultDurablePackageStager();
			bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
				TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
			{
				return Inner->BuildArtifactInventory(Plans, Artifacts, Failure);
			}
			bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
				TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
			{
				if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
				ULevelSequence* Actual = FindObject<ULevelSequence>(nullptr, *ObjectPath);
				const TArray<UAssetUserData*>* Data = Actual ? Actual->GetAssetUserDataArray() : nullptr;
				if (!Data || Data->Num() != 1 || (*Data)[0] != nullptr) return true;
				TArray<uint8> CurrentDisk;
				bool bAnyStaged = false;
				bool bValid = Actual->GetOutermost()->IsDirty()
					&& FFileHelper::LoadFileToArray(CurrentDisk, *Filename)
					&& CurrentDisk == DiskBefore;
				if (DesiredKeysMatch) bValid &= DesiredKeysMatch(Actual);
				else if (bStructure)
				{
					const auto* Section = Cast<UMovieSceneFloatSection>(Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]);
					bValid &= Section && Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)
						->GetData().GetValues()[0].Value == 7.75f;
				}
				for (const FDurableArtifact& Artifact : Artifacts)
				{
					bValid &= IFileManager::Get().FileExists(*Artifact.StagingPath)
						== Artifact.bStagedExists;
					bAnyStaged |= Artifact.bStagedExists;
				}
				bValid &= bAnyStaged;
				*bObserved = bValid;
				*Witness = Actual;
				if (!bReject) return true;
				Failure.Diagnostic.Code = TEXT("t85_sequence_root_early_stage_rejected");
				Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
				return false;
			}
		};

		auto Stager = MakeShared<FSequenceStageDecorator>();
		Stager->ObjectPath = ObjectPath;
		Stager->Filename = Filename;
		Stager->DiskBefore = DiskBefore;
		Stager->bReject = bEarly;
		Stager->bStructure = bStructure;
		auto DesiredObjectPropertiesMatch = [bGroupCondition](ULevelSequence* Actual) {
			if (!Actual || !Actual->GetMovieScene() || Actual->GetMovieScene()->GetTracks().IsEmpty()) return false;
			const auto* Track = Actual->GetMovieScene()->GetTracks()[0];
			if (bGroupCondition) return !Track->ConditionContainer.Condition && Track->GetColorTint() == FColor(13,27,41,255)
				&& Track->EvalOptions.bEvaluateInPostroll && Track->GetAllSections()[0]->GetPreRollFrames() == 87;
			return Track->EvalOptions.bEvaluateInPreroll && Track->EvalOptions.bEvaluateInPostroll
				&& Track->GetColorTint() == FColor(91,27,41,255) && !Track->GetAllSections()[0]->IsActive()
				&& Track->GetAllSections()[0]->GetPreRollFrames() == 87;
		};
		if (bObjectProperties) Stager->DesiredKeysMatch = DesiredObjectPropertiesMatch;
		if (bTime || bKeyCollection) Stager->DesiredKeysMatch = DesiredKeysMatch;
		if (bSectionCollection || bOpenRange) Stager->DesiredKeysMatch = DesiredSectionsMatch;
		if (bTrackCollection) Stager->DesiredKeysMatch = DesiredTracksMatch;
		if (bNumeric) Stager->DesiredKeysMatch = DesiredDoubleMatch;
		Stager->bObserved = &bDesiredStageObserved;
		Stager->Witness = &DesiredStageWitness;
		FAssetApplyRequest Request;
		Request.TypeId = TEXT("sequence");
		Request.Operation = UeAgentApplyInputContract::EOperation::Update;
		Request.AssetPath = PackageName;
		Request.bSave = true;
		Request.bCompactValues = true;
		Request.PackageStager = Stager;
		auto Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("property_path"), TEXT("AssetUserData"));
		Property->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNull>()});
		Request.Properties.Add(MakeShared<FJsonValueObject>(Property));
		if (bStructure)
		{
			using namespace UE::UAI::DomainAdapters::TimelineAnimation;
			TSharedPtr<FJsonObject> Structure;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!TryReadLevelSequenceStructure(*Sequence, ObjectPath, Structure, Error)) return false;
			const auto Key = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]
				->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0]->AsObject();
			if (bObjectProperties)
			{
				const auto Track = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
				const auto Properties = Track->GetObjectField(TEXT("properties"));
				if (bGroupCondition) {
					const auto Condition = FindObjectFieldRecursive(Properties, TEXT("ConditionContainer"));
					Condition->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetField(TEXT("Condition"), MakeShared<FJsonValueNull>());
				} else {
				const auto Eval = FindObjectFieldRecursive(Properties, TEXT("EvalOptions"));
				const auto Tint = FindObjectFieldRecursive(Properties, TEXT("TrackTint"));
				if (!Eval || !Tint) return false;
				Eval->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetBoolField(TEXT("bEvaluateInPreroll"), true);
				auto TintValue = Tint->GetArrayField(TEXT("value"));
				if (TintValue.Num() != 4) return false;
				TintValue[0] = MakeShared<FJsonValueNumber>(91); Tint->SetArrayField(TEXT("value"), TintValue);
				const auto SectionProperties = Track->GetArrayField(TEXT("sections"))[0]->AsObject()->GetObjectField(TEXT("properties"));
				const auto Active = FindObjectFieldRecursive(SectionProperties, TEXT("bIsActive")); if (!Active) return false;
				Active->SetBoolField(TEXT("value"), false);
				}
			}
			else if (bObjectPath)
			{
				const auto Channel = Structure->GetArrayField(TEXT("root_tracks"))[1]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject();
				const auto ObjectKey = Channel->GetArrayField(TEXT("keys"))[0]->AsObject(); ObjectKey->SetField(TEXT("value"), UeAgentPropertyValueCodec::EncodeReferencePath(ReferenceAssets[1]));
				ObjectKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
				const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), true); Default->SetField(TEXT("value"), UeAgentPropertyValueCodec::EncodeReferencePath(ReferenceAssets[0])); Channel->SetObjectField(TEXT("default"), Default);
			}
			else if (bString)
			{
				const auto Channel = Structure->GetArrayField(TEXT("root_tracks"))[1]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject();
				const auto StringKey = Channel->GetArrayField(TEXT("keys"))[0]->AsObject(); StringKey->SetStringField(TEXT("value_lexeme"), TEXT("已持久\"文本\"\n新行"));
				StringKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
				const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), true); Default->SetStringField(TEXT("value_lexeme"), TEXT("")); Channel->SetObjectField(TEXT("default"), Default);
			}
			else if (bPrimitive)
			{
				const auto Channel = Structure->GetArrayField(TEXT("root_tracks"))[1]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject();
				const auto BoolKey = Channel->GetArrayField(TEXT("keys"))[0]->AsObject(); BoolKey->SetStringField(TEXT("value_lexeme"), bByteEnum ? TEXT("HLOD7") : bInteger ? TEXT("-31") : TEXT("false"));
				BoolKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
				const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), true); Default->SetStringField(TEXT("value_lexeme"), bByteEnum ? TEXT("HLOD0") : bInteger ? TEXT("-2147483648") : TEXT("false")); Channel->SetObjectField(TEXT("default"), Default);
				const auto Fields = Channel->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"));
				if (bInteger) { const auto Literal = MakeShared<FJsonObject>(); Literal->SetBoolField(TEXT("literal"), true); Fields->SetObjectField(TEXT("bInterpolateLinearKeys"), Literal); }
				const auto Pre = MakeShared<FJsonObject>(); Pre->SetStringField(TEXT("selected"), TEXT("RCCE_Linear")); Fields->SetObjectField(TEXT("PreInfinityExtrap"), Pre);
				const auto Post = MakeShared<FJsonObject>(); Post->SetStringField(TEXT("selected"), TEXT("RCCE_Cycle")); Fields->SetObjectField(TEXT("PostInfinityExtrap"), Post);
			}
			else if (bNumeric)
			{
				const auto DoubleKey = Structure->GetArrayField(TEXT("root_tracks"))[1]->AsObject()->GetArrayField(TEXT("sections"))[0]
					->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0]->AsObject();
				if (bChannelSettings)
				{
					const auto Channel = Structure->GetArrayField(TEXT("root_tracks"))[1]->AsObject()->GetArrayField(TEXT("sections"))[0]
						->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject();
					const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), true);
					Default->SetStringField(TEXT("value_lexeme"), TEXT("3.1415926535897931")); Channel->SetObjectField(TEXT("default"), Default);
					const auto Fields = Channel->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"));
					const auto Pre = MakeShared<FJsonObject>(); Pre->SetStringField(TEXT("selected"), TEXT("RCCE_CycleWithOffset")); Fields->SetObjectField(TEXT("PreInfinityExtrap"), Pre);
					const auto Post = MakeShared<FJsonObject>(); Post->SetStringField(TEXT("selected"), TEXT("RCCE_Linear")); Fields->SetObjectField(TEXT("PostInfinityExtrap"), Post);
				}
				else
				{
				DoubleKey->SetStringField(TEXT("value_lexeme"), TEXT("2.0000000000000004"));
				DoubleKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
				}
				if (bKeyProperties)
				{
					const auto Fields = DoubleKey->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"));
					const auto Interp = MakeShared<FJsonObject>(); Interp->SetStringField(TEXT("selected"), TEXT("RCIM_Constant")); Fields->SetObjectField(TEXT("InterpMode"), Interp);
					const auto Mode = MakeShared<FJsonObject>(); Mode->SetStringField(TEXT("selected"), TEXT("RCTM_User")); Fields->SetObjectField(TEXT("TangentMode"), Mode);
					const auto Leave = MakeShared<FJsonObject>(); Leave->SetNumberField(TEXT("literal"), 67.25);
					Fields->GetObjectField(TEXT("Tangent"))->GetObjectField(TEXT("fields"))->SetObjectField(TEXT("LeaveTangent"), Leave);
				}
			}
			else if (bTrackCollection)
			{
				auto Tracks = Structure->GetArrayField(TEXT("root_tracks"));
				Tracks.RemoveAt(0);
				Tracks[0]->AsObject()->SetStringField(TEXT("display_name"), TEXT("Renamed"));
				Structure->SetArrayField(TEXT("root_tracks"), Tracks);
			}
			else if (bOpenRange)
			{
				const auto Range = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject()->GetObjectField(TEXT("range"));
				for (const FString Side : {FString(TEXT("lower")), FString(TEXT("upper"))})
				{ const auto Bound = MakeShared<FJsonObject>(); Bound->SetStringField(TEXT("kind"), TEXT("open")); Range->SetObjectField(Side, Bound); }
			}
			else if (bSectionCollection)
			{
				const auto Track = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
				auto Sections = Track->GetArrayField(TEXT("sections"));
				Sections.RemoveAt(0);
				Sections.Add(MakeShared<FJsonValueObject>(MakeAddedSection(ObjectPath)));
				Track->SetArrayField(TEXT("sections"), Sections);
			}
			else if (bKeyCollection)
			{
				auto Keys = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]
					->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"));
				Keys.RemoveAt(0);
				const auto Added = MakeShared<FJsonObject>();
				Added->SetStringField(TEXT("key_ref"), TEXT("Added"));
				Added->SetObjectField(TEXT("time"), MakePoint(TEXT("level_sequence:") + ObjectPath, 6000));
				Added->SetStringField(TEXT("value_type"), TEXT("decimal"));
				Added->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
				Keys.Add(MakeShared<FJsonValueObject>(Added));
				Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]
					->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->SetArrayField(TEXT("keys"), Keys);
			}
			else if (bTime) Key->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("18000"));
			else Key->SetStringField(TEXT("value_lexeme"), TEXT("7.75"));
			Request.ProviderExtension = MakeShared<FJsonObject>();
			Request.ProviderExtension->SetObjectField(TEXT("movie_scene"), Structure);
		}
		bool bLateObserved = false;
		Request.FinalReadback = [&](FOperationFailure& Failure)
		{
			bFinalReadbackCalled = true;
			ULevelSequence* Actual = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
			const TArray<UAssetUserData*>* Data = Actual ? Actual->GetAssetUserDataArray() : nullptr;
			TArray<uint8> CurrentDisk;
			bLateObserved = !bEarly && bDesiredStageObserved
				&& !DesiredStageWitness.IsValid(true, true) && !OldSequence.IsValid(true, true)
				&& Actual && Data && Data->Num() == 1 && (*Data)[0] == nullptr
				&& Actual->GetMovieScene()
				&& Actual->GetMovieScene()->GetPlaybackRange().GetLowerBoundValue().Value == 17
				&& Actual->GetMovieScene()->GetPlaybackRange().GetUpperBoundValue().Value == 48
				&& !Actual->GetOutermost()->IsDirty()
				&& FFileHelper::LoadFileToArray(CurrentDisk, *Filename)
				&& CurrentDisk != DiskBefore;
			if (bObjectProperties && bLateObserved) {
				bLateObserved &= DesiredObjectPropertiesMatch(Actual);
				if (bGroupCondition) bLateObserved &= !OldGroup.IsValid(true, true) && !OldChild.IsValid(true, true);
			}
			else if (bNumeric && bLateObserved) bLateObserved &= DesiredDoubleMatch(Actual);
			else if (bTrackCollection && bLateObserved)
				bLateObserved &= !OldRemovedTrack.IsValid(true, true) && DesiredTracksMatch(Actual);
			else if ((bSectionCollection || bOpenRange) && bLateObserved)
				bLateObserved &= !OldRemovedSection.IsValid(true, true) && DesiredSectionsMatch(Actual);
			else if ((bTime || bKeyCollection) && bLateObserved) bLateObserved &= DesiredKeysMatch(Actual);
			else if (bStructure && bLateObserved)
			{
				const auto* Section = Cast<UMovieSceneFloatSection>(Actual->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]);
				bLateObserved &= Section && Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)
					->GetData().GetValues()[0].Value == 7.75f;
			}
			Failure.Diagnostic.Code = TEXT("t85_sequence_root_late_readback_rejected");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		};
		Sequence = nullptr;
		MovieScene = nullptr;
		Package = nullptr;
		FAssetApplyResult Result;
		Test.TestFalse(TEXT("saved Sequence Update rejects at requested lifecycle phase"),
			ApplyAsset(Request, Result, (bEarly ? TEXT("sequence-root-early-") : TEXT("sequence-root-late-")) + Token));
		Test.TestEqual(TEXT("requested Sequence diagnostic retained"), Result.Diagnostic.Code,
			FString(bEarly ? TEXT("t85_sequence_root_early_stage_rejected")
				: TEXT("t85_sequence_root_late_readback_rejected")));
		if (!Test.TestTrue(TEXT("requested Sequence lifecycle phase observed"),
				bEarly ? (bDesiredStageObserved && !bFinalReadbackCalled) : bLateObserved)
			|| !Test.TestTrue(TEXT("Sequence checkpoint recovery confirmed"),
				Result.bRollbackAttempted && Result.bRollbackSucceeded))
		{
			Test.AddError(Result.Phase + TEXT(":") + Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.PublicMessage);
			return false;
		}
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("restored Sequence reload"), Sequence)
			|| !Test.TestNotNull(TEXT("restored MovieScene"), Sequence->GetMovieScene())) return false;
		Test.TestTrue(TEXT("complete dirty Sequence state restored"),
			SequenceBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Sequence)));
		Test.TestTrue(TEXT("complete dirty MovieScene state restored"),
			MovieSceneBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Sequence->GetMovieScene())));
		Test.TestTrue(TEXT("dirty memory checkpoint restored"), Sequence->GetOutermost()->IsDirty());
		if (bStructure)
		{
			TArray<UObject*> Owned;
			GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
			Test.TestEqual(TEXT("restored Sequence owns the original object count"), Owned.Num(), OwnedBefore.Num());
			for (UObject* Object : Owned)
			{
				const auto* Before = OwnedBefore.Find(Object->GetPathName(Sequence->GetMovieScene()));
				Test.TestTrue(TEXT("all owned track section key interpolation and tangent state restored"),
					Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object)));
			}
		}
		TArray<uint8> DiskAfter;
		Test.TestTrue(TEXT("original Sequence disk independently restored"),
			FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		if (bTrackCollection || bNumeric || bObjectProperties)
		{
			Test.TestTrue(TEXT("track recovery restores real original component and actor binding"),
				HasFixtureBinding(Sequence, Preview->GetWorld(), RealGuid, BoundComponent)
				&& HasFixtureBinding(Sequence, Preview->GetWorld(), RealActorGuid, BoundActor));
			Test.AddInfo(bNumeric
				? TEXT("DoubleLateRecovery precise_value_time_saved_reloaded_before_injection=1 old_weak_exited=1 new_clean_disk_changed=1 dirty_owned_memory_and_distinct_disk_restored=1")
				: TEXT("TrackLateRecovery deleted_renamed_before_injection=1 old_weak_exited=1 new_clean_disk_changed=1 dirty_owned_memory_and_distinct_disk_restored=1"));
		}
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceCreatePublicTest,
	"UAI.Root.T85.LevelSequence.CreatePublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceCreatePublicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Name = TEXT("LS_CreatePublic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85LevelSequenceCreate/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85/LevelSequenceCreate") / Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath))
		{
			TestEqual(TEXT("created fixture deletes through UE"),
				ObjectTools::ForceDeleteObjects({Remaining}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	if (!TestTrue(TEXT("public create begins with object registry disk absent"),
		IsSequenceAbsent(ObjectPath, Filename)))
	{
		return false;
	}
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(
			Registry, Dispatch, Error))
	{
		AddError(Error);
		return false;
	}
	const auto RetiredFields = Tests::PublicCommand::SubmitActiveCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		MakeSequenceCreateImport(
			ObjectPath, MakeSequenceStructure(ObjectPath), true),
		TEXT("t85-level-sequence-create-retired-fields-") + Name);
	if (!TestTrue(
			TEXT("missing Sequence rejects the retired fields model before write"),
			!RetiredFields.IsOk()
				&& RetiredFields.GetErrorJson().Contains(
					TEXT("sequence_hjson_fields_retired_use_structure_create_or_directory_edit")))
		|| !TestTrue(
			TEXT("retired fields rejection leaves object registry disk absent before any cleanup or later create"),
			IsSequenceAbsent(ObjectPath, Filename)))
	{
		AddError(RetiredFields.GetErrorJson());
		return false;
	}
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)),
		TEXT("t85-level-sequence-create-public-") + Name);
	if (!TestTrue(TEXT("public registry dispatch creates nonempty Level Sequence"),
		Imported.IsOk()))
	{
		AddError(Imported.GetErrorJson());
		return false;
	}
	if (!TestTrue(TEXT("created Level Sequence independently unloads"),
		T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)))
	{
		AddError(Error);
		return false;
	}
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("reloaded Sequence retains complete nonempty structure"),
		HasCreatedSequenceStructure(Sequence))
		|| !TestTrue(TEXT("created package is clean"),
			Sequence && !Sequence->GetOutermost()->IsDirty())
		|| !TestTrue(TEXT("created package exists on disk"),
			IFileManager::Get().FileExists(*Filename))
		|| !TestTrue(TEXT("created asset is registered"),
			FAssetRegistryModule::GetRegistry()
				.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid()))
	{
		return false;
	}
	Sequence = nullptr;
	const TSharedRef<FJsonObject> Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), ObjectPath);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(
		Registry, Dispatch, TEXT("content_graph.export_workspace"), Export,
		TEXT("t85-level-sequence-create-reexport-"),
		{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!TestTrue(TEXT("created Sequence publicly reexports"), Exported.IsOk()))
	{
		AddError(Exported.GetErrorJson());
		return false;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(
		Files, *Workspace, TEXT("*.hjson"), true, false);
	bool bFloatTrackVisible = false;
	bool bFloatSectionVisible = false;
	bool bPossessableBindingVisible = false;
	bool bValueLexemeVisible = false;
	bool bMachineNoise = false;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")))
		{
			continue;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) continue;
		TSharedPtr<FJsonObject> Read; FString ReadError;
		if (!FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, ReadError)) return false;
		if (const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks")))
			for (const auto& Pair : Tracks->Values)
			{
				const auto Track = Pair.Value->AsObject();
				if (Track->GetStringField(TEXT("track_kind")) != UMovieSceneFloatTrack::StaticClass()->GetName()) continue;
				bFloatTrackVisible = true;
				for (const auto& Section : Track->GetObjectField(TEXT("sections"))->Values)
					bFloatSectionVisible |= Section.Value->AsObject()->GetObjectField(TEXT("channels"))->HasField(TEXT("Value"));
			}
		bPossessableBindingVisible |= Text.Contains(TEXT("binding_kind"))
			&& Text.Contains(TEXT("possessable"));
		bValueLexemeVisible |= Text.Contains(TEXT("value_lexeme"));
		bMachineNoise |= Text.Contains(TEXT("observed_state"))
			|| Text.Contains(TEXT("source_id"))
			|| Text.Contains(TEXT("generation"));
	}
	TestTrue(TEXT("ordinary reexport exposes float track"), bFloatTrackVisible);
	TestTrue(TEXT("ordinary reexport exposes float section"), bFloatSectionVisible);
	TestTrue(TEXT("ordinary reexport exposes possessable binding"),
		bPossessableBindingVisible);
	TestTrue(TEXT("ordinary reexport exposes value_lexeme"), bValueLexemeVisible);
	TestFalse(TEXT("ordinary reexport omits internal revision state"),
		bMachineNoise);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceStructureValueTest,
	"UAI.Root.T85.LevelSequence.StructureValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool RunLevelSequenceStructureEdit(FAutomationTestBase& Test, const int32 EditMode)
{
	using namespace UE::UAI;
	using namespace DomainAdapters::TimelineAnimation;
	const bool bTime = EditMode == 1;
	const bool bCollection = EditMode == 2;
	const bool bEmptyAdd = EditMode == 3;
	const FString Name = TEXT("LS_Structure_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85LevelSequenceStructure/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/SequenceStructure") / Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath))
			Test.TestEqual(TEXT("structure fixture deletes through UE"), ObjectTools::DeleteObjects({Remaining}, false), 1);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{ Test.AddError(Error); return false; }
	const auto Created = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)), Name);
	if (!Test.TestTrue(TEXT("structure fixture public create succeeds"), Created.IsOk()))
	{ Test.AddError(Created.GetErrorJson()); return false; }
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !HasCreatedSequenceStructure(Sequence)) return false;
	TArray<FMovieSceneFloatValue> ExpectedValues;
	const FGuid ExpectedBindingGuid = Sequence->GetMovieScene()->GetBindings()[0].GetObjectGuid();
	for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{
		Sequence->GetMovieScene()->GetTracks()[0], Sequence->GetMovieScene()->GetBindings()[0].GetTracks()[0]})
	{
		auto* Section = CastChecked<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
		Section->Modify();
		for (auto& Value : Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData().GetValues())
		{
			if (ExpectedValues.Num() >= 2) Value.Value += 20.f; // Public values uniquely select root keys before import.
			Value.InterpMode = RCIM_Cubic;
			Value.TangentMode = RCTM_User;
			Value.Tangent.ArriveTangent = 2.25f + ExpectedValues.Num();
			Value.Tangent.LeaveTangent = -1.75f - ExpectedValues.Num();
			ExpectedValues.Add(Value);
		}
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	TSharedPtr<FJsonObject> Expected;
	FUeAgentTimelineAnimationExecutionError NativeError;
	if (!Sequence || !TryReadLevelSequenceStructure(*Sequence, ObjectPath, Expected, NativeError)) return false;
	Sequence = nullptr;
	auto Export = [&](const FString& Path)
	{
		const auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Path);
		Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
			TEXT("content_graph.export_workspace"), Args, Name + TEXT("-export"),
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) Test.AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	if (!Export(Workspace / TEXT("edit"))) return false;
	FString KeyRef;
	auto FindNativeKey = [&](const TSharedPtr<FJsonObject>& Root, const bool bChoose) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("root_tracks"), Tracks) || Tracks->Num() != 1) return nullptr;
		const auto& Sections = (*Tracks)[0]->AsObject()->GetArrayField(TEXT("sections"));
		if (Sections.Num() != 1) return nullptr;
		const auto Channel = SingleChannel(Sections[0]->AsObject());
		if (!Channel.IsValid()) return nullptr;
		TSharedPtr<FJsonObject> Found;
		for (const auto& Value : Channel->GetArrayField(TEXT("keys")))
		{
			const auto Key = Value->AsObject(); FString Ref, Lexeme;
			if (!Key->TryGetStringField(TEXT("key_ref"), Ref) || !Key->TryGetStringField(TEXT("value_lexeme"), Lexeme)) return nullptr;
			if (bChoose ? Lexeme == TEXT("1.25") : Ref == KeyRef)
			{
				if (Found.IsValid()) return nullptr;
				Found = Key;
			}
		}
		if (Found.IsValid() && bChoose) KeyRef = Found->GetStringField(TEXT("key_ref"));
		return Found;
	};
	TMap<FString, TSharedPtr<FJsonObject>> KeyDefinitions;
	auto FindOrdinaryKey = [&KeyDefinitions](const TSharedPtr<FJsonObject>& Root,
		const FString& ValueLexeme, FString* OutReadableKey,
		TSharedPtr<FJsonObject>* OutCollection = nullptr,
		TSharedPtr<FJsonObject>* OutTemplate = nullptr) -> TSharedPtr<FJsonObject>
	{
		TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Root)};
		while (!Pending.IsEmpty())
		{
			const auto Value = Pending.Pop(EAllowShrinking::No);
			if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
			else if (Value->Type == EJson::Object)
			{
				const auto Object = Value->AsObject();
				const TSharedPtr<FJsonObject>* Keys = nullptr;
				if (Object->TryGetObjectField(TEXT("keys"), Keys))
				{
					for (const auto& Pair : (*Keys)->Values)
					{
						const auto Key = Pair.Value->AsObject();
						FString Lexeme;
						if (Key.IsValid() && Key->TryGetStringField(TEXT("value_lexeme"), Lexeme)
							&& Lexeme == ValueLexeme)
						{
							if (OutReadableKey) *OutReadableKey = Pair.Key;
							if (OutCollection) *OutCollection = *Keys;
							if (OutTemplate && KeyDefinitions.Num() == 1) *OutTemplate = KeyDefinitions.CreateConstIterator().Value();
							return Key;
						}
					}
				}
				for (const auto& Field : Object->Values) Pending.Add(Field.Value);
			}
		}
		return nullptr;
	};
	auto FindEmptyKeyCollection = [&KeyDefinitions](const TSharedPtr<FJsonObject>& Root, TSharedPtr<FJsonObject>& OutTemplate) -> TSharedPtr<FJsonObject>
	{
		TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Root)};
		while (!Pending.IsEmpty())
		{
			const auto Value = Pending.Pop(EAllowShrinking::No);
			if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
			else if (Value->Type == EJson::Object)
			{
				const auto Object = Value->AsObject();
				const TSharedPtr<FJsonObject>* Keys = nullptr;
				if (Object->TryGetObjectField(TEXT("keys"), Keys) && (*Keys)->Values.IsEmpty())
					{ if (KeyDefinitions.Num() == 1) OutTemplate = KeyDefinitions.CreateConstIterator().Value(); return *Keys; }
				for (const auto& Field : Object->Values) Pending.Add(Field.Value);
			}
		}
		return nullptr;
	};
	auto ImportEdit = [&](const FString& Suffix)
	{
		const auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
		const auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetObjectField(TEXT("source"), Source);
		const auto Result = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
			TEXT("content_graph.import_workspace"), Args, Name + Suffix);
		if (!Result.IsOk()) Test.AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("edit")), TEXT("*.hjson"), true, false);
	auto ReadDefinitions = [&]()
	{
		KeyDefinitions.Reset();
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Document)};
			while (!Pending.IsEmpty())
			{
				const auto Value = Pending.Pop(EAllowShrinking::No);
				if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
				else if (Value->Type == EJson::Object)
				{
					const auto Object = Value->AsObject();
					FString Ref, DefinitionName;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName)
						&& DefinitionName == TEXT("LevelSequenceScalarKey")
						&& Object->TryGetStringField(TEXT("ref"), Ref))
					{
						if (KeyDefinitions.Contains(Ref)) return false;
						const auto Contract = Object->GetObjectField(TEXT("value_definition"));
						if (Contract->GetStringField(TEXT("time_unit")) != TEXT("sequence_tick")) return false;
						KeyDefinitions.Add(Ref, Contract->GetObjectField(TEXT("template")));
					}
					for (const auto& Field : Object->Values) Pending.Add(Field.Value);
				}
			}
		}
		return Test.TestEqual(TEXT("two collections share exactly one public key definition"), KeyDefinitions.Num(), 1);
	};
	if (!ReadDefinitions()) return false;
	if (bEmptyAdd)
	{
		bool bCleared = false;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document, Keys;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			if (!FindOrdinaryKey(Document, TEXT("1.25"), nullptr, &Keys)) continue;
			Keys->Values.Empty();
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			bCleared = true;
			break;
		}
		if (!Test.TestTrue(TEXT("ordinary root collection selected for clear before import"), bCleared)
			|| !ImportEdit(TEXT("-clear"))
			|| !T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		TSharedPtr<FJsonObject> Cleared;
		if (!Sequence || !TryReadLevelSequenceStructure(*Sequence, ObjectPath, Cleared, NativeError)) return false;
		const auto ExpectedSection = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject();
		const auto OriginalKeys = SingleChannel(ExpectedSection)->GetArrayField(TEXT("keys"));
		SingleChannel(ExpectedSection)->SetArrayField(TEXT("keys"), {});
		FString ExpectedClear, ActualClear;
		if (!CommandContract::Json::TryCanonicalize(Expected, ExpectedClear, Error)
			|| !CommandContract::Json::TryCanonicalize(Cleared, ActualClear, Error)) return false;
		Test.TestEqual(TEXT("ordinary clear persists exactly root empty and all other structure unchanged"), ActualClear, ExpectedClear);
		Test.TestFalse(TEXT("cleared reloaded sequence is clean"), Sequence->GetOutermost()->IsDirty());
		const auto* BoundSection = CastChecked<UMovieSceneFloatSection>(Sequence->GetMovieScene()->GetBindings()[0].GetTracks()[0]->GetAllSections()[0]);
		const auto Bound = BoundSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData();
		Test.TestTrue(TEXT("clear preserves complete non-target native keys"), Bound.GetValues().Num() == 2
			&& Bound.GetValues()[0] == ExpectedValues[2] && Bound.GetValues()[1] == ExpectedValues[3]);
		SingleChannel(ExpectedSection)->SetArrayField(TEXT("keys"), OriginalKeys);
		Sequence = nullptr;
		if (Test.HasAnyErrors() || !Export(Workspace / TEXT("edit"))) return false;
		Files.Reset();
		IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("edit")), TEXT("*.hjson"), true, false);
		if (!ReadDefinitions()) return false;
	}
	bool bEdited = false;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		FString ReadableKey;
		TSharedPtr<FJsonObject> KeyCollection, KeyTemplate;
		const auto Key = bEmptyAdd ? nullptr
			: FindOrdinaryKey(Document, TEXT("1.25"), &ReadableKey, &KeyCollection, &KeyTemplate);
		if (bEmptyAdd)
		{
			if (!FindObjectFieldRecursive(Document, TEXT("root_tracks")).IsValid()) continue;
			KeyCollection = FindEmptyKeyCollection(Document, KeyTemplate);
			if (!KeyCollection.IsValid()) continue;
			const auto Added = MakeShared<FJsonObject>();
			if (!KeyTemplate.IsValid() || KeyTemplate->GetObjectField(TEXT("time"))->HasField(TEXT("clock_id"))) return false;
			Added->Values = KeyTemplate->Values;
			const auto Time = MakeShared<FJsonObject>();
			Time->Values = KeyTemplate->GetObjectField(TEXT("time"))->Values;
			if (!Time->HasTypedField<EJson::Object>(TEXT("rate")) || !Time->HasField(TEXT("tick"))) return false;
			Time->SetStringField(TEXT("tick"), TEXT("6000"));
			Added->SetObjectField(TEXT("time"), Time);
			Added->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
			KeyCollection->SetObjectField(TEXT("Added"), Added);
		}
		else if (!Key.IsValid()) continue;
		else if (bCollection)
		{
			KeyCollection->RemoveField(ReadableKey);
			const auto Added = MakeShared<FJsonObject>();
			if (!KeyTemplate.IsValid() || KeyTemplate->GetObjectField(TEXT("time"))->HasField(TEXT("clock_id"))) return false;
			Added->Values = KeyTemplate->Values;
			const auto Time = MakeShared<FJsonObject>();
			Time->Values = KeyTemplate->GetObjectField(TEXT("time"))->Values;
			if (!Time->HasTypedField<EJson::Object>(TEXT("rate")) || !Time->HasField(TEXT("tick"))) return false;
			Time->SetStringField(TEXT("tick"), TEXT("6000"));
			Added->SetObjectField(TEXT("time"), Time);
			Added->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
			KeyCollection->SetObjectField(TEXT("Added"), Added);
		}
		else if (bTime)
		{
			const TSharedPtr<FJsonObject>* Time = nullptr;
			if (!Key->TryGetObjectField(TEXT("time"), Time)
				|| (*Time)->GetStringField(TEXT("tick")) != TEXT("0")) return false;
			// A sparse nested edit must preserve the captured clock, rate and state.
			const auto SparseTime = MakeShared<FJsonObject>();
			SparseTime->SetStringField(TEXT("tick"), TEXT("18000"));
			Key->SetObjectField(TEXT("time"), SparseTime);
		}
		else Key->SetStringField(TEXT("value_lexeme"), TEXT("7.75"));
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEdited = true;
		break;
	}
	if (!Test.TestTrue(TEXT("ordinary workspace exposes an editable existing key"), bEdited)) return false;
	const auto ExpectedKey = (bCollection || bEmptyAdd) ? nullptr : FindNativeKey(Expected, true);
	if (!bCollection && !bEmptyAdd && !Test.TestTrue(TEXT("ordinary key identity matches independent native structure"), ExpectedKey.IsValid())) return false;
	if (bCollection || bEmptyAdd)
	{
		const auto Section = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject();
		auto Keys = SingleChannel(Section)->GetArrayField(TEXT("keys"));
		Keys[0]->AsObject()->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
		Keys[0]->AsObject()->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
		Keys[0]->AsObject()->SetField(TEXT("properties"), SingleChannel(Section)->GetObjectField(TEXT("default_key"))->TryGetField(TEXT("properties")));
		if (bEmptyAdd) Keys.SetNum(1);
		SingleChannel(Section)->SetArrayField(TEXT("keys"), Keys);
		ExpectedValues[0] = FMovieSceneFloatValue(9.5f);
	}
	else if (bTime)
	{
		ExpectedKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("18000"));
		auto Section = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject();
		auto Keys = SingleChannel(Section)->GetArrayField(TEXT("keys"));
		const FString OtherRef = Keys[1]->AsObject()->GetStringField(TEXT("key_ref"));
		Keys.Swap(0, 1);
		Keys[0]->AsObject()->SetStringField(TEXT("key_ref"), KeyRef);
		Keys[1]->AsObject()->SetStringField(TEXT("key_ref"), OtherRef);
		SingleChannel(Section)->SetArrayField(TEXT("keys"), Keys);
		KeyRef = OtherRef;
		ExpectedValues.Swap(0, 1);
	}
	else
	{
		ExpectedKey->SetStringField(TEXT("value_lexeme"), TEXT("7.75"));
		ExpectedValues[0].Value = 7.75f;
	}
	if (!Test.TestTrue(TEXT("ordinary key edit public import succeeds"), ImportEdit(TEXT("-edit")))) return false;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { Test.AddError(Error); return false; }
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	TSharedPtr<FJsonObject> Actual;
	if (!Sequence || !TryReadLevelSequenceStructure(*Sequence, ObjectPath, Actual, NativeError)) return false;
	FString ExpectedText, ActualText;
	CommandContract::Json::TryCanonicalize(Expected, ExpectedText, Error);
	CommandContract::Json::TryCanonicalize(Actual, ActualText, Error);
	Test.TestEqual(TEXT("independent reload has exactly the intended full structure, keys and sorted times"), ActualText, ExpectedText);
	Test.TestEqual(TEXT("possessable binding GUID survives edit and reload"),
		Sequence->GetMovieScene()->GetBindings()[0].GetObjectGuid(), ExpectedBindingGuid);
	int32 TrackIndex = 0;
	for (UMovieSceneTrack* Track : TArray<UMovieSceneTrack*>{
		Sequence->GetMovieScene()->GetTracks()[0], Sequence->GetMovieScene()->GetBindings()[0].GetTracks()[0]})
	{
		const auto* Section = CastChecked<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
		const auto Data = Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->GetData();
		const int32 Count = TrackIndex == 0 && bEmptyAdd ? 1 : 2;
		Test.TestEqual(TEXT("exact native key count in preselected channel"), Data.GetValues().Num(), Count);
		for (int32 Index = 0; Index < FMath::Min(Count, Data.GetValues().Num()); ++Index)
			Test.TestTrue(TEXT("target and non-target retain complete expected native values, interpolation and unique tangents"),
				Data.GetValues()[Index] == ExpectedValues[TrackIndex * 2 + Index]);
		++TrackIndex;
	}
	Test.TestFalse(TEXT("reloaded edited Sequence is clean"), Sequence->GetOutermost()->IsDirty());
	Sequence = nullptr;
	if (!Export(Workspace / TEXT("readback"))) return false;
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("readback")), TEXT("*.hjson"), true, false);
	bool bReadback = false;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		const FString ReadbackLexeme = (bCollection || bEmptyAdd) ? TEXT("9.5") : bTime ? TEXT("1.25") : TEXT("7.75");
		const auto Key = FindOrdinaryKey(Document, ReadbackLexeme, nullptr);
		if (Key.IsValid()) bReadback |= (bCollection || bEmptyAdd)
			? Key->GetObjectField(TEXT("time"))->GetStringField(TEXT("tick")) == TEXT("6000")
			: !bTime || Key->GetObjectField(TEXT("time"))->GetStringField(TEXT("tick")) == TEXT("18000");
	}
	Test.TestTrue(TEXT("fresh ordinary export contains the changed key value"), bReadback);
	return !Test.HasAnyErrors();
}

bool FUeAgentT85LevelSequenceStructureValueTest::RunTest(const FString&)
{
	return RunLevelSequenceStructureEdit(*this, 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceKeyTimeTest,
	"UAI.Root.T85.LevelSequence.KeyTime.Public",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceKeyTimeTest::RunTest(const FString&)
{
	return RunLevelSequenceStructureEdit(*this, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceKeyCollectionTest,
	"UAI.Root.T85.LevelSequence.KeysCollection.Public",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceKeyCollectionTest::RunTest(const FString&)
{
	return RunLevelSequenceStructureEdit(*this, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceEmptyKeyCollectionTest,
	"UAI.Root.T85.LevelSequence.KeysCollection.EmptyAdd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceEmptyKeyCollectionTest::RunTest(const FString&)
{
	return RunLevelSequenceStructureEdit(*this, 3);
}

static bool RunSequenceSectionsPublic(FAutomationTestBase& Test, bool bEmptyAdd)
{
	using namespace UE::UAI;
	using namespace DomainAdapters::TimelineAnimation;
	const FString Name = TEXT("LS_Sections_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceSections/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/SequenceSections") / Name;
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	const auto Created = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)), Name);
	if (!Created.IsOk()) { Test.AddError(Created.GetErrorJson()); return false; }
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence) return false;
	AddSequenceSurvivor(*Sequence);
	auto* Scene = Sequence->GetMovieScene();
	const FGuid BindingGuid = Scene->GetBindings()[0].GetObjectGuid();
	auto* SelectedTrack = CastChecked<UMovieSceneFloatTrack>(Scene->GetTracks()[0]);
	SelectedTrack->SetSectionToKey(SelectedTrack->GetAllSections()[0]);
	const FName RemovedSelectionName = SelectedTrack->GetSectionToKey()->GetFName();
	const FName BoundSelectionName = GetFNameSafe(CastChecked<UMovieSceneFloatTrack>(Scene->GetBindings()[0].GetTracks()[0])->GetSectionToKey());
	const auto Survivor = CapturePersistentObject(*Scene->GetTracks()[0]->GetAllSections()[1]);
	const auto BoundTrack = CapturePersistentObject(*Scene->GetBindings()[0].GetTracks()[0]);
	const auto BoundSection = CapturePersistentObject(*Scene->GetBindings()[0].GetTracks()[0]->GetAllSections()[0]);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	TSharedPtr<FJsonObject> Expected;
	FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryReadLevelSequenceStructure(*Sequence, ObjectPath, Expected, NativeError)) return false;
	const auto ExpectedTrack = Expected->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
	Sequence = nullptr; Scene = nullptr;
	auto Export = [&]() {
		const auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Workspace);
		Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
			TEXT("content_graph.export_workspace"), Args, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) Test.AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Import = [&]() {
		const auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		const auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		const auto Result = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, Name);
		if (!Result.IsOk()) Test.AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Verify = [&](bool bCleared) {
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		auto* Actual = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		TSharedPtr<FJsonObject> Readback; FString ExpectedText, ActualText;
		if (!Actual || !TryReadLevelSequenceStructure(*Actual, ObjectPath, Readback, NativeError)
			|| !CommandContract::Json::TryCanonicalize(Expected, ExpectedText, Error)
			|| !CommandContract::Json::TryCanonicalize(Readback, ActualText, Error)) return false;
		Test.TestEqual(TEXT("fixed target sections and complete other structure survive independent reload"), ActualText, ExpectedText);
		const auto* ActualScene = Actual->GetMovieScene();
		Test.TestEqual(TEXT("binding GUID preserved"), ActualScene->GetBindings()[0].GetObjectGuid(), BindingGuid);
		const auto* ActualTrack = CastChecked<UMovieSceneFloatTrack>(ActualScene->GetTracks()[0]);
		const auto* Selection = ActualTrack->GetSectionToKey();
		Test.TestTrue(TEXT("removed selected section resolves through native section lifecycle"),
			(ActualTrack->GetAllSections().IsEmpty() ? Selection == nullptr
				: (!Selection || (ActualTrack->GetAllSections().Contains(Selection) && Selection->GetFName() != RemovedSelectionName))));
		if (!bCleared && !bEmptyAdd) Test.TestTrue(TEXT("native added section becomes selected"), Selection == ActualTrack->GetAllSections().Last());
		Test.TestEqual(TEXT("non-target native section selection remains"),
			GetFNameSafe(CastChecked<UMovieSceneFloatTrack>(ActualScene->GetBindings()[0].GetTracks()[0])->GetSectionToKey()), BoundSelectionName);
		Test.TestTrue(TEXT("complete non-target track preserved"), BoundTrack.OrderIndependentCompareEqual(CapturePersistentObject(*ActualScene->GetBindings()[0].GetTracks()[0])));
		Test.TestTrue(TEXT("complete non-target section channel preserved"), BoundSection.OrderIndependentCompareEqual(CapturePersistentObject(*ActualScene->GetBindings()[0].GetTracks()[0]->GetAllSections()[0])));
		if (!bEmptyAdd && !bCleared)
			Test.TestTrue(TEXT("surviving section retains full channel and unique tangents"), Survivor.OrderIndependentCompareEqual(CapturePersistentObject(*ActualScene->GetTracks()[0]->GetAllSections()[0])));
		Test.TestFalse(TEXT("persisted sections clean"), Actual->GetOutermost()->IsDirty());
		return !Test.HasAnyErrors();
	};
	FString TargetFile;
	auto Edit = [&](bool bClear, bool bFromEmpty) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		TSharedPtr<FJsonObject> SectionTemplate, KeyTemplate, TargetDocument, Sections;
		FString SelectedKey;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Document)};
			while (!Pending.IsEmpty())
			{
				const auto Value = Pending.Pop(EAllowShrinking::No);
				if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
				else if (Value->Type == EJson::Object)
				{
					const auto Object = Value->AsObject(); FString DefinitionName;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName) && Object->HasTypedField<EJson::Object>(TEXT("value_definition")))
					{
						const auto Template = Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template"));
						if (DefinitionName == TEXT("LevelSequenceScalarSection")) SectionTemplate = Template;
						if (DefinitionName == TEXT("LevelSequenceScalarKey")) KeyTemplate = Template;
					}
					const TSharedPtr<FJsonObject>* FoundSections = nullptr;
					if (Object->TryGetObjectField(TEXT("sections"), FoundSections))
					{
						bool bTarget = bFromEmpty && File == TargetFile && (*FoundSections)->Values.IsEmpty();
						for (const auto& Pair : (*FoundSections)->Values)
						{
							const auto Keys = SingleChannel(Pair.Value->AsObject())->GetObjectField(TEXT("keys"));
							for (const auto& Key : Keys->Values)
								if (Key.Value->AsObject()->GetStringField(TEXT("value_lexeme")) == TEXT("1.25")
									&& (*FoundSections)->Values.Num() == 2) { bTarget = true; SelectedKey = Pair.Key; }
						}
						if (bTarget) { TargetDocument = Document; Sections = *FoundSections; TargetFile = File; }
					}
					for (const auto& Field : Object->Values) Pending.Add(Field.Value);
				}
			}
		}
		if (!Test.TestTrue(TEXT("preselected root sections ordinary collection located"), Sections.IsValid())) return false;
		if (bClear) Sections->Values.Empty();
		else
		{
			if (!SectionTemplate || !KeyTemplate) return false;
			if (!bFromEmpty) Sections->RemoveField(SelectedKey);
			const auto Added = MakeShared<FJsonObject>(); Added->Values = SectionTemplate->Values;
			const auto Range = MakeShared<FJsonObject>();
			for (const auto& BoundName : {TEXT("lower"), TEXT("upper")})
			{
				const auto Bound = MakeShared<FJsonObject>(); const auto Point = MakeShared<FJsonObject>();
				Point->SetStringField(TEXT("tick"), FString(BoundName) == TEXT("lower") ? TEXT("6000") : TEXT("18000"));
				Bound->SetObjectField(TEXT("point"), Point); Range->SetObjectField(BoundName, Bound);
			}
			Added->SetObjectField(TEXT("range"), Range);
			const auto Key = MakeShared<FJsonObject>(); Key->Values = KeyTemplate->Values;
			const auto Time = MakeShared<FJsonObject>(); Time->SetStringField(TEXT("tick"), TEXT("7000"));
			Key->SetObjectField(TEXT("time"), Time); Key->SetStringField(TEXT("value_lexeme"), TEXT("9.5"));
			const auto Keys = MakeShared<FJsonObject>(); Keys->SetObjectField(TEXT("AddedKey"), Key);
			SetPublicKeys(Added, Keys); Sections->SetObjectField(TEXT("AddedSection"), Added);
		}
		FString Text;
		return FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if (!Export()) return false;
	if (bEmptyAdd)
	{
		ExpectedTrack->SetArrayField(TEXT("sections"), {});
		if (!Edit(true, false) || !Import() || !Verify(true) || !Export()) return false;
	}
	auto Desired = ExpectedTrack->GetArrayField(TEXT("sections"));
	if (!bEmptyAdd) Desired.RemoveAt(0);
	const auto AddedExpectedSection = MakeAddedSection(ObjectPath);
	// Copy the records while retaining their immutable native numeric lexemes.
	AddedExpectedSection->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>(*ExpectedTrack->GetObjectField(TEXT("default_properties"))));
	AddedExpectedSection->SetObjectField(TEXT("property_definitions"), MakeShared<FJsonObject>(*ExpectedTrack->GetObjectField(TEXT("default_property_definitions"))));
	Desired.Add(MakeShared<FJsonValueObject>(AddedExpectedSection));
	ExpectedTrack->SetArrayField(TEXT("sections"), Desired); ReindexExpectedSections(ExpectedTrack);
	if (!Edit(false, bEmptyAdd) || !Import() || !Verify(false) || !Export()) return false;
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceSectionsPublicTest,
	"UAI.Root.T85.LevelSequence.SectionsCollection.Public",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceSectionsPublicTest::RunTest(const FString&) { return RunSequenceSectionsPublic(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceSectionsEmptyTest,
	"UAI.Root.T85.LevelSequence.SectionsCollection.EmptyAdd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceSectionsEmptyTest::RunTest(const FString&) { return RunSequenceSectionsPublic(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceSectionsRecoveryTest,
	"UAI.Root.T85.LevelSequence.SectionsCollection.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceSectionsRecoveryTest::RunTest(const FString&) { return RunLevelSequenceRootRecovery(*this, false, true, false, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceOpenRangeRecoveryTest,
	"UAI.Root.T85.LevelSequence.OpenRange.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceOpenRangeRecoveryTest::RunTest(const FString&) { return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceKeyTimeLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.KeyTime.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceKeyTimeLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceKeysCollectionLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.KeysCollection.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceKeysCollectionLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceCreateLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.CreateLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceCreateLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceCreateRecovery(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceKeyPropertiesRecoveryTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.KeyPropertiesLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceKeyPropertiesRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceTransformLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.TransformLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceTransformLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceDoubleLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.DoubleLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceDoubleLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceCreateEarlyRecoveryTest,
	"UAI.Root.T85.LevelSequence.CreateEarlyRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceCreateEarlyRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceCreateRecovery(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceStructureLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.StructureLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceStructureLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceRootLateRecoveryTest,
	"UAI.Root.T85.LevelSequence.RootLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceRootLateRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceRootEarlyRecoveryTest,
	"UAI.Root.T85.LevelSequence.RootEarlyRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelSequenceRootEarlyRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceTrackCollectionTest,
	"UAI.Root.T85.LevelSequence.TracksCollection.ExistingFloat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceTrackCollectionRecoveryTest,
	"UAI.Root.T85.LevelSequence.TracksCollection.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85LevelSequenceTrackCollectionRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, true);
}

bool FUeAgentT85LevelSequenceTrackCollectionTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!TestNotNull(TEXT("real track fixture component"), Component)) return false;
	const FString Name = TEXT("LS_Tracks_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceTracks/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/SequenceTracks") / Name;
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	const auto Created = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)), Name);
	if (!Created.IsOk()) { AddError(Created.GetErrorJson()); return false; }
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !Sequence->GetMovieScene()) return false;
	auto* Scene = Sequence->GetMovieScene();
	const FGuid BindingGuid = Scene->GetBindings()[0].GetObjectGuid();
	FGuid RealGuid, ActorGuid;
	{
		const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld());
		ON_SCOPE_EXIT { Sequencer->Close(); };
		RealGuid = Sequencer->GetHandleToObject(Component, true);
		ActorGuid = Sequencer->GetHandleToObject(Actor, false);
	}
	if (!TestTrue(TEXT("real component binding established before export"), HasFixtureBinding(Sequence, Preview.GetWorld(), RealGuid, Component))) return false;
	auto* RealTrack = Scene->AddTrack<UMovieSceneFloatTrack>(RealGuid);
	RealTrack->SetPropertyNameAndPath(TEXT("Intensity"), TEXT("Intensity"));
	auto* RealSection = CastChecked<UMovieSceneFloatSection>(RealTrack->CreateNewSection());
	RealSection->SetRange(TRange<FFrameNumber>::All()); RealTrack->AddSection(*RealSection);
	RealSection->GetChannel().SetDefault(1234.5f);
	auto* First = Cast<UMovieSceneFloatTrack>(Scene->GetTracks()[0]);
	auto* Second = Scene->AddTrack<UMovieSceneFloatTrack>();
	if (!First || !Second) return false;
	First->SetDisplayName(FText::FromString(TEXT("Shared")));
	Second->SetDisplayName(FText::FromString(TEXT("Shared")));
	auto* SecondSection = Cast<UMovieSceneFloatSection>(Second->CreateNewSection());
	if (!SecondSection) return false;
	SecondSection->SetRange(TRange<FFrameNumber>(0, 24000));
	Second->AddSection(*SecondSection);
	auto* SecondChannel = SecondSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
	if (!SecondChannel) return false;
	FMovieSceneFloatValue UniqueValue(17.25f);
	UniqueValue.InterpMode = RCIM_Cubic;
	UniqueValue.TangentMode = RCTM_Break;
	UniqueValue.Tangent.ArriveTangent = 31.5f;
	UniqueValue.Tangent.LeaveTangent = -42.75f;
	SecondChannel->GetData().AddKey(FFrameNumber(9000), UniqueValue);
	auto* LiteralSuffix = Scene->AddTrack<UMovieSceneFloatTrack>();
	LiteralSuffix->SetDisplayName(FText::FromString(TEXT("Shared2")));
	{
		const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld());
		Sequencer->Close();
	}
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	// Fix the full-object oracle only after native initialization and a separate
	// baseline save/reload, so serialization initialization is not mistaken for an edit.
	Sequence = nullptr; Scene = nullptr; First = nullptr; Second = nullptr; SecondSection = nullptr; SecondChannel = nullptr;
	RealTrack = nullptr; RealSection = nullptr; LiteralSuffix = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !Sequence->GetMovieScene()) return false;
	Scene = Sequence->GetMovieScene();
	if (!TestEqual(TEXT("baseline retains all three root tracks"), Scene->GetTracks().Num(), 3)) return false;
	const auto SurvivorSection = CapturePersistentObject(*Scene->GetTracks()[1]->GetAllSections()[0]);
	const auto LiteralBefore = CapturePersistentObject(*Scene->GetTracks()[2]);
	const auto* OriginalBinding = Scene->FindBinding(BindingGuid);
	const auto* RealBinding = Scene->FindBinding(RealGuid);
	if (!TestTrue(TEXT("baseline contains both bound track fixtures"), OriginalBinding && OriginalBinding->GetTracks().Num() == 1
		&& RealBinding && RealBinding->GetTracks().Num() == 1)) return false;
	const auto BoundTrack = CapturePersistentObject(*OriginalBinding->GetTracks()[0]);
	const auto BoundSection = CapturePersistentObject(*OriginalBinding->GetTracks()[0]->GetAllSections()[0]);
	const auto RealTrackBefore = CapturePersistentObject(*RealBinding->GetTracks()[0]);
	const auto RealSectionBefore = CapturePersistentObject(*RealBinding->GetTracks()[0]->GetAllSections()[0]);
	const int32 ExpectedBindingCount = Scene->GetBindings().Num();
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Workspace);
	Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
		TEXT("content_graph.export_workspace"), Args, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	FString TargetFile; TSharedPtr<FJsonObject> TargetDocument, Tracks;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		const auto Found = FindObjectFieldRecursive(Document, TEXT("root_tracks"));
		if (Found.IsValid() && Found->Values.Num() == 3)
		{ TargetFile = File; TargetDocument = Document; Tracks = Found; break; }
	}
	if (!TestTrue(TEXT("ordinary duplicate root track collection located"), Tracks.IsValid())) return false;
	TestNotNull(TEXT("first duplicate alias exists"), Tracks->Values.Find(TEXT("Shared")));
	TestNotNull(TEXT("second duplicate alias exists"), Tracks->Values.Find(TEXT("Shared2")));
	TestNotNull(TEXT("literal suffix collision remains a third editable track"), Tracks->Values.Find(TEXT("Shared22")));
	TestFalse(TEXT("ordinary track omits captured identity"),
		Tracks->GetObjectField(TEXT("Shared2"))->HasField(TEXT("track_ref")));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	FString Text;
	Tracks->RemoveField(TEXT("Shared"));
	Tracks->GetObjectField(TEXT("Shared2"))->SetStringField(TEXT("display_name"), TEXT("Renamed"));
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TWeakObjectPtr<ULevelSequence> OldSequence = Sequence;
	Sequence = nullptr; Scene = nullptr; First = nullptr; Second = nullptr; SecondSection = nullptr; SecondChannel = nullptr;
	RealTrack = nullptr; RealSection = nullptr; LiteralSuffix = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-edit"));
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	TestFalse(TEXT("old track fixture exits before independent disk readback"), OldSequence.IsValid(true, true));
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !Sequence->GetMovieScene()) return false;
	Scene = Sequence->GetMovieScene();
	if (!TestEqual(TEXT("two distinct survivors remain after deleting first duplicate"), Scene->GetTracks().Num(), 2)) return false;
	Second = Cast<UMovieSceneFloatTrack>(Scene->GetTracks()[0]);
	TestEqual(TEXT("survivor display name persists"), Second->GetDisplayName().ToString(), FString(TEXT("Renamed")));
	TestTrue(TEXT("survivor full section channel and unique tangents persist"),
		SurvivorSection.OrderIndependentCompareEqual(CapturePersistentObject(*Second->GetAllSections()[0])));
	OriginalBinding = Scene->FindBinding(BindingGuid);
	if (!TestTrue(TEXT("original binding preserved"), OriginalBinding && OriginalBinding->GetTracks().Num() == 1)) return false;
	CheckPersistentObject(*this, TEXT("unrelated bound track preserved"), BoundTrack, *OriginalBinding->GetTracks()[0]);
	CheckPersistentObject(*this, TEXT("unrelated bound section preserved"), BoundSection, *OriginalBinding->GetTracks()[0]->GetAllSections()[0]);
	TestFalse(TEXT("persisted track edit clean"), Sequence->GetOutermost()->IsDirty());
	CheckPersistentObject(*this, TEXT("literal suffix track survives intact"), LiteralBefore, *Scene->GetTracks()[1]);
	auto VerifyRealBinding = [&] {
		const auto* Unrelated = Sequence->GetMovieScene()->FindBinding(BindingGuid);
		if (!TestTrue(TEXT("unrelated binding retains its single original track"), Unrelated && Unrelated->GetTracks().Num() == 1)) return false;
		const bool bUnrelatedTrack = CheckPersistentObject(*this, TEXT("unrelated track full state after collection edit"), BoundTrack, *Unrelated->GetTracks()[0]);
		const bool bUnrelatedSection = CheckPersistentObject(*this, TEXT("unrelated channel full state after collection edit"), BoundSection, *Unrelated->GetTracks()[0]->GetAllSections()[0]);
		const bool bBindingCount = TestEqual(TEXT("all original bindings remain"), Sequence->GetMovieScene()->GetBindings().Num(), ExpectedBindingCount);
		const auto* Binding = Sequence->GetMovieScene()->FindBinding(RealGuid);
		if (!TestTrue(TEXT("real binding retains exactly one track"), Binding && Binding->GetTracks().Num() == 1)) return false;
		const bool bTrack = CheckPersistentObject(*this, TEXT("real track full state"), RealTrackBefore, *Binding->GetTracks()[0]);
		const bool bSection = CheckPersistentObject(*this, TEXT("real section full state"), RealSectionBefore, *Binding->GetTracks()[0]->GetAllSections()[0]);
		const bool bComponent = TestTrue(TEXT("original component resolves"), HasFixtureBinding(Sequence, Preview.GetWorld(), RealGuid, Component));
		const bool bActor = TestTrue(TEXT("original actor resolves"), HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		return bTrack && bSection && bComponent && bActor && bUnrelatedTrack && bUnrelatedSection && bBindingCount;
	};
	TestTrue(TEXT("edit preserves full bound channel and resolves original actor component"), VerifyRealBinding());
	const auto Fresh = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
		TEXT("content_graph.export_workspace"), Args, Name + TEXT("-fresh"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Fresh.IsOk()) { AddError(Fresh.GetErrorJson()); return false; }
	if (!FFileHelper::LoadFileToString(Text, *TargetFile)
		|| !FUeAgentStructuredHjsonParser::ParseObject(Text, TargetFile, TargetDocument, Error)) return false;
	Tracks = FindObjectFieldRecursive(TargetDocument, TEXT("root_tracks"));
	if (!TestTrue(TEXT("fresh export shows both surviving tracks"), Tracks && Tracks->Values.Num() == 2)) return false;
	Tracks->Values.Reset();
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	OldSequence = Sequence; Sequence = nullptr; Scene = nullptr; Second = nullptr;
	const auto Cleared = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-clear"));
	if (!Cleared.IsOk()) { AddError(Cleared.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	TestFalse(TEXT("clear old fixture exits"), OldSequence.IsValid(true, true));
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("ordinary empty track collection persists"), Sequence && Sequence->GetMovieScene()->GetTracks().IsEmpty())) return false;
	TestTrue(TEXT("clear preserves real binding and its full channel"), VerifyRealBinding());
	TestFalse(TEXT("cleared track fixture clean"), Sequence->GetOutermost()->IsDirty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelSequenceRootWorkspaceTest,
	"UAI.Root.T85.LevelSequence.RootWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceDoubleNumericTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.DoublePublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool RunNumericChannelsPublic(FAutomationTestBase& Test, const bool bTransform, const bool bProperties = false, const bool bSettings = false, const bool bVector = false)
{
	using namespace UE::UAI;
	const FString Name = TEXT("LS_Numeric_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceNumeric/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/Numeric") / Name;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!Component) return false;
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	DomainAdapters::TimelineAnimation::FTimelineAnimationStructure Initial;
	DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
	if (!DomainAdapters::TimelineAnimation::TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !DomainAdapters::TimelineAnimation::TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	if (bVector) { AddVectorColorFixture(*Sequence->GetMovieScene(), 2, TEXT("Precise"));
		AddVectorColorFixture(*Sequence->GetMovieScene(), 0, TEXT("Color")); AddVectorColorFixture(*Sequence->GetMovieScene(), 1, TEXT("Vector2")); }
	else if (bTransform) AddTransformFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise"));
	else AddDoubleFixtureTrack(*Sequence->GetMovieScene(), TEXT("Precise"));
	AddDoubleFixtureTrack(*Sequence->GetMovieScene(), TEXT("OtherDouble"));
	FGuid ComponentGuid, ActorGuid;
	{
		const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true); ActorGuid = Sequencer->GetHandleToObject(Actor, false);
	}
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs)) return false;
	FString Error; Sequence = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence) return false;
	const FString SelectedName = bVector ? TEXT("Vector.X") : bTransform ? TEXT("Location.X") : TEXT("Value");
	auto FindTarget = [](ULevelSequence* Asset) -> UMovieSceneTrack* {
		for (auto* Track : Asset->GetMovieScene()->GetTracks())
			if (Track->GetDisplayName().ToString() == TEXT("Precise")) return Track;
		return nullptr;
	};
	auto* Target = FindTarget(Sequence);
	if (!Target || Target->GetAllSections().Num() != 1) return false;
	auto* InitialSection = Target->GetAllSections()[0];
	if (bTransform && !Test.TestTrue(TEXT("independent native proxy has exactly nine named Double channels and Weight Float"), HasNativeTransformChannels(*InitialSection))) return false;
	const auto OtherChannels = CaptureOtherChannels(*InitialSection, SelectedName);
	TMap<FString, TMap<FString, FString>> AuxiliaryChannels;
	TMap<FString, FString> ExpectedAuxiliarySelected;
	if (bVector) for (auto* Track : Sequence->GetMovieScene()->GetTracks())
	{
		const FString Label = Track->GetDisplayName().ToString();
		if (Label == TEXT("Color") || Label == TEXT("Vector2"))
		{
			AuxiliaryChannels.Add(Label, CaptureOtherChannels(*Track->GetAllSections()[0], Label == TEXT("Color") ? TEXT("Color.R") : TEXT("Vector.X")));
			FMovieSceneFloatChannel Expected = *Track->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
			Expected.GetData().GetTimes()[0] = FFrameNumber(1200); auto& Value = Expected.GetData().GetValues()[0];
			Value.Value = 0.375f; Value.InterpMode = RCIM_Constant; Value.Tangent.LeaveTangent = 4.5f; Expected.SetDefault(0.625f);
			FString Text; FMovieSceneFloatChannel::StaticStruct()->ExportText(Text, &Expected, nullptr, nullptr, PPF_None, nullptr);
			ExpectedAuxiliarySelected.Add(Label, Text);
		}
	}
	const auto Mask = bTransform ? CastChecked<UMovieScene3DTransformSection>(InitialSection)->GetMask().GetChannels() : EMovieSceneTransformChannel::None;
	auto Default = (*InitialSection->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault();
	double CurveBefore = 0; (*InitialSection->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).Evaluate(FFrameTime(4500), CurveBefore);
	TArray<FMovieSceneDoubleValue> ExpectedValues; ExpectedValues.Append((*InitialSection->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues());
	TArray<FFrameNumber> ExpectedTimes; ExpectedTimes.Append((*InitialSection->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetTimes());
	Test.TestTrue(TEXT("double fixture distinguishes float precision"), ExpectedValues[0].Value != static_cast<double>(static_cast<float>(ExpectedValues[0].Value)));
	TMap<FString, TMap<FString, FString>> NonTarget;
	TArray<UObject*> Owned; GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
	for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target))
		if (!bVector || (!Object->GetTypedOuter<UMovieSceneColorTrack>() && !Object->GetTypedOuter<UMovieSceneFloatVectorTrack>()
			&& !Object->IsA<UMovieSceneColorTrack>() && !Object->IsA<UMovieSceneFloatVectorTrack>()))
		NonTarget.Add(Object->GetPathName(Sequence->GetMovieScene()), CapturePersistentObject(*Object));
	Owned.Reset();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	for (int32 Phase = 0; Phase < 4; ++Phase)
	{
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"),
			ExportArgs, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { Test.AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		FString TargetFile, SectionDefinitionRef, KeyDefinitionRef; TSharedPtr<FJsonObject> Document, Sections, Keys;
		TMap<FString, TSharedPtr<FJsonObject>> Templates, KeyTemplates;
		TSharedPtr<FJsonObject> EnumCatalog;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			if (const auto Catalog = FindObjectFieldRecursive(Read, TEXT("enums"))) EnumCatalog = Catalog;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Read)};
			while (!Pending.IsEmpty())
			{
				const auto Item = Pending.Pop(EAllowShrinking::No);
				if (Item->Type == EJson::Array) Pending.Append(Item->AsArray());
				else if (Item->Type == EJson::Object)
				{
					const auto Object = Item->AsObject(); FString DefinitionName, Ref;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName) && DefinitionName == TEXT("LevelSequenceScalarSection")
						&& Object->TryGetStringField(TEXT("ref"), Ref)) Templates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
					if (DefinitionName == TEXT("LevelSequenceScalarKey") && Object->TryGetStringField(TEXT("ref"), Ref))
						KeyTemplates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
					for (const auto& Pair : Object->Values) Pending.Add(Pair.Value);
				}
			}
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks"));
			if (!Tracks || !Tracks->HasField(TEXT("Precise"))) continue;
			Document = Read; TargetFile = File; Sections = Tracks->GetObjectField(TEXT("Precise"))->GetObjectField(TEXT("sections"));
			Keys = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"))->GetObjectField(SelectedName)->GetObjectField(TEXT("keys"));
			const auto ContextDefinitions = FindObjectFieldRecursive(Read, TEXT("section_definitions"));
			if (!ContextDefinitions || !ContextDefinitions->TryGetStringField(TEXT("Precise"), SectionDefinitionRef)) return false;
			const auto ContextKeys = FindObjectFieldRecursive(Read, TEXT("key_definitions"));
			if (!ContextKeys || !ContextKeys->GetObjectField(TEXT("Precise"))->TryGetStringField(SelectedName, KeyDefinitionRef)) return false;
		}
		if (!Test.TestTrue(TEXT("mixed numeric collection exposes precise Double track"), Keys.IsValid())) return false;
		const auto SectionTemplate = Templates.FindRef(SectionDefinitionRef);
		const auto KeyTemplate = KeyTemplates.FindRef(KeyDefinitionRef);
		if (bProperties && !Test.TestTrue(TEXT("native key properties and enum catalog are published by the common definitions owner"), KeyTemplate.IsValid()
			&& KeyTemplate->HasField(TEXT("properties")) && EnumCatalog.IsValid()
			&& EnumCatalog->HasField(TEXT("ERichCurveInterpMode")) && EnumCatalog->HasField(TEXT("ERichCurveTangentMode"))
			&& EnumCatalog->HasField(TEXT("ERichCurveTangentWeightMode")))) return false;
		if (!Test.TestTrue(TEXT("target track resolves its exported native section template"), SectionTemplate.IsValid())) return false;
		if (bVector)
		{
			const auto Tracks = FindObjectFieldRecursive(Document, TEXT("root_tracks"));
			const auto Definitions = FindObjectFieldRecursive(Document, TEXT("section_definitions"));
			const TCHAR* Labels[] = {TEXT("Color"), TEXT("Vector2"), TEXT("Precise")};
			for (int32 Kind = 0; Kind < 3; ++Kind)
			{
				const auto PublicTrack = Tracks->GetObjectField(Labels[Kind]);
				const UClass* Classes[] = {UMovieSceneColorTrack::StaticClass(), UMovieSceneFloatVectorTrack::StaticClass(), UMovieSceneDoubleVectorTrack::StaticClass()};
				Test.TestEqual(TEXT("track kind describes its stable native class"), PublicTrack->GetStringField(TEXT("track_kind")), Classes[Kind]->GetName());
				const auto PublicChannels = PublicTrack->GetObjectField(TEXT("sections"))->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"));
				const auto NativeTemplate = Templates.FindRef(Definitions->GetStringField(Labels[Kind]));
				const int32 Count = Kind == 0 ? 4 : Kind == 1 ? 2 : 3;
				if (!Test.TestTrue(TEXT("Color and differing Vector dimensions expose complete contextual native templates"), NativeTemplate
					&& PublicChannels->Values.Num() == Count && NativeTemplate->GetObjectField(TEXT("channels"))->Values.Num() == Count)) return false;
				for (const auto& Pair : PublicChannels->Values) if (!Test.TestTrue(TEXT("each native template channel is named and has no cloned keys"),
					NativeTemplate->GetObjectField(TEXT("channels"))->HasField(Pair.Key)
					&& NativeTemplate->GetObjectField(TEXT("channels"))->GetObjectField(Pair.Key)->GetObjectField(TEXT("keys"))->Values.IsEmpty())) return false;
				if (Kind == 2) continue;
				const FString SelectedChannel = Kind == 0 ? TEXT("Color.R") : TEXT("Vector.X");
				if (Phase == 0)
				{
					const auto Channel = PublicChannels->GetObjectField(SelectedChannel);
					const auto Key = Channel->GetObjectField(TEXT("keys"))->GetObjectField(TEXT("Key1"));
					Key->SetStringField(TEXT("value_lexeme"), TEXT("0.375")); Key->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("1200"));
					const auto Fields = Key->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"));
					Fields->GetObjectField(TEXT("InterpMode"))->SetStringField(TEXT("selected"), TEXT("RCIM_Constant"));
					Fields->GetObjectField(TEXT("Tangent"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("LeaveTangent"))->SetNumberField(TEXT("literal"), 4.5);
					const auto DefaultValue = MakeShared<FJsonObject>(); DefaultValue->SetBoolField(TEXT("is_set"), true);
					DefaultValue->SetStringField(TEXT("value_lexeme"), TEXT("0.625")); Channel->SetObjectField(TEXT("default"), DefaultValue);
				}
				if (Phase == 3)
				{
					TSharedPtr<FJsonObject> Added; FString TemplateText;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(NativeTemplate.ToSharedRef(), TemplateText, Error)
						|| !FUeAgentStructuredHjsonParser::ParseObject(TemplateText, TEXT("Color Vector native template"), Added, Error)) return false;
					PublicTrack->GetObjectField(TEXT("sections"))->SetObjectField(TEXT("AddedSection"), Added);
				}
			}
		}
		if (bTransform)
		{
			const auto PublicChannels = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"));
			const auto DefaultChannels = SectionTemplate->GetObjectField(TEXT("channels"));
			if (!Test.TestEqual(TEXT("public Transform exposes all ten native names"), PublicChannels->Values.Num(), 10)
				|| !Test.TestEqual(TEXT("exported native template exposes all ten names"), DefaultChannels->Values.Num(), 10)) return false;
			for (const auto& ChannelName : TransformChannelNames())
				if (!Test.TestTrue(TEXT("native/public/template channel names correspond exactly"), PublicChannels->HasField(ChannelName)
					&& DefaultChannels->HasField(ChannelName) && DefaultChannels->GetObjectField(ChannelName)->GetObjectField(TEXT("keys"))->Values.IsEmpty())) return false;
		}
		auto NewKey = [](const TCHAR* Tick, const TCHAR* Value) {
			const auto Key = MakeShared<FJsonObject>(); const auto Time = MakeShared<FJsonObject>();
			Time->SetStringField(TEXT("tick"), Tick); Key->SetObjectField(TEXT("time"), Time);
			Key->SetStringField(TEXT("value_lexeme"), Value); return Key;
		};
		auto Selected = [](const TCHAR* Name) { const auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("selected"), Name); return Value; };
		auto Literal = [](double Number) { const auto Value = MakeShared<FJsonObject>(); Value->SetNumberField(TEXT("literal"), Number); return Value; };
		auto SparseProperties = [&](const TCHAR* Field, const TSharedPtr<FJsonObject>& Value) {
			const auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(Field, Value);
			const auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("fields"), Fields); return Properties;
		};
		if (bSettings)
		{
			const auto Channel = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"))->GetObjectField(SelectedName);
			if (!Test.TestTrue(TEXT("channel default and filtered extrapolation definitions are public"), Channel->HasField(TEXT("default"))
				&& Channel->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"))->Values.Num() == 2
				&& EnumCatalog.IsValid() && EnumCatalog->HasField(TEXT("ERichCurveExtrapolation"))
				&& FindObjectFieldRecursive(Document, TEXT("channel_definitions")).IsValid())) return false;
			if (Phase == 0)
			{
				const auto OriginalDefault = Channel->GetObjectField(TEXT("default"));
				const auto OriginalProperties = Channel->GetObjectField(TEXT("properties"));
				const auto NativeBefore = CapturePersistentObject(*Target->GetAllSections()[0]);
				TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
				for (int32 Invalid = 0; Invalid < 3; ++Invalid)
				{
					Channel->SetObjectField(TEXT("default"), OriginalDefault); Channel->SetObjectField(TEXT("properties"), OriginalProperties);
					if (Invalid == 0) Channel->SetObjectField(TEXT("properties"), SparseProperties(TEXT("PreInfinityExtrap"), Selected(TEXT("invalid_enum"))));
					else if (Invalid == 1) { const auto Bad = MakeShared<FJsonObject>(); Bad->SetBoolField(TEXT("is_set"), false);
						Bad->SetStringField(TEXT("value_lexeme"), TEXT("9.25")); Channel->SetObjectField(TEXT("default"), Bad); }
					else Channel->SetObjectField(TEXT("properties"), SparseProperties(TEXT("DefaultValue"), Literal(9.25)));
					FString InvalidText;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), InvalidText, Error)
						|| !FFileHelper::SaveStringToFile(InvalidText, *TargetFile)) return false;
					const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
					Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Target = Sequence ? FindTarget(Sequence) : nullptr;
					TArray<uint8> DiskAfter;
					if (!Test.TestTrue(TEXT("invalid channel enum contradictory default and second default owner reject before write"), !Rejected.IsOk() && Target
						&& NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target->GetAllSections()[0]))
						&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore && !Sequence->GetOutermost()->IsDirty())) return false;
				}
				Channel->SetObjectField(TEXT("properties"), OriginalProperties);
				const auto Fields = OriginalProperties->GetObjectField(TEXT("fields"));
				Fields->SetObjectField(TEXT("PreInfinityExtrap"), Selected(TEXT("RCCE_CycleWithOffset")));
				Fields->SetObjectField(TEXT("PostInfinityExtrap"), Selected(TEXT("RCCE_CycleWithOffset")));
			}
			if (Phase == 1) { Channel->RemoveField(TEXT("default")); Keys->Values.Reset(); ExpectedValues.Reset(); ExpectedTimes.Reset(); }
			else
			{
				const auto Setting = MakeShared<FJsonObject>(); Setting->SetBoolField(TEXT("is_set"), Phase != 2);
				if (Phase == 2) Default.Reset();
				else { Default = 1.0000000000000002; Setting->SetStringField(TEXT("value_lexeme"), TEXT("1.0000000000000002")); }
				Channel->SetObjectField(TEXT("default"), Setting);
			}
			if (Phase == 3)
			{
				TSharedPtr<FJsonObject> Added; FString TemplateText;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(SectionTemplate.ToSharedRef(), TemplateText, Error)
					|| !FUeAgentStructuredHjsonParser::ParseObject(TemplateText, TEXT("native section template"), Added, Error)) return false;
				Added->GetObjectField(TEXT("channels"))->GetObjectField(SelectedName)->GetObjectField(TEXT("keys"))
					->SetObjectField(TEXT("PreciseKey"), NewKey(TEXT("45"), TEXT("1.0000000000000002")));
				Sections->SetObjectField(TEXT("AddedSection"), Added);
			}
		}
		else if (bProperties && Phase < 3)
		{
			if (Phase == 0)
			{
				const auto Key = Keys->GetObjectField(TEXT("Key1")); const auto OriginalProperties = Key->GetObjectField(TEXT("properties"));
				const auto NativeBefore = CapturePersistentObject(*Target->GetAllSections()[0]); TArray<uint8> DiskBefore;
				if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
				for (int32 Invalid = 0; Invalid < 3; ++Invalid)
				{
					Key->SetObjectField(TEXT("properties"), Invalid == 0 ? SparseProperties(TEXT("InterpMode"), Selected(TEXT("invalid_enum")))
						: SparseProperties(Invalid == 1 ? TEXT("UnknownField") : TEXT("Value"), Literal(8.5)));
					FString InvalidText; if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), InvalidText, Error)
						|| !FFileHelper::SaveStringToFile(InvalidText, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
					const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
					// Preparation rejection can restore/rebind the package; reacquire the actual object before observing it.
					Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
					Target = Sequence ? FindTarget(Sequence) : nullptr;
					TArray<uint8> DiskAfter;
					if (!Test.TestTrue(TEXT("invalid enum field and properties Value reject before native writes"), !Rejected.IsOk()
						&& Target
						&& NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target->GetAllSections()[0]))
						&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore && !Sequence->GetOutermost()->IsDirty())) return false;
				}
				Key->SetObjectField(TEXT("properties"), OriginalProperties);
				const auto Fields = OriginalProperties->GetObjectField(TEXT("fields"));
				Fields->SetObjectField(TEXT("InterpMode"), Selected(TEXT("RCIM_Constant")));
				Fields->SetObjectField(TEXT("TangentMode"), Selected(TEXT("RCTM_User")));
				const auto Tangents = Fields->GetObjectField(TEXT("Tangent"))->GetObjectField(TEXT("fields"));
				Tangents->SetObjectField(TEXT("ArriveTangent"), Literal(7.25)); Tangents->SetObjectField(TEXT("LeaveTangent"), Literal(-2.5));
				Tangents->SetObjectField(TEXT("TangentWeightMode"), Selected(TEXT("RCTWM_WeightedNone")));
				ExpectedValues[0].InterpMode = RCIM_Constant; ExpectedValues[0].TangentMode = RCTM_User;
				ExpectedValues[0].Tangent.ArriveTangent = 7.25f; ExpectedValues[0].Tangent.LeaveTangent = -2.5f;
				ExpectedValues[0].Tangent.TangentWeightMode = RCTWM_WeightedNone;
			}
			else if (Phase == 1)
			{
				const auto Tangents = MakeShared<FJsonObject>(); Tangents->SetObjectField(TEXT("ArriveTangent"), Literal(9.5));
				const auto Tangent = MakeShared<FJsonObject>(); Tangent->SetObjectField(TEXT("fields"), Tangents);
				Keys->GetObjectField(TEXT("Key1"))->SetObjectField(TEXT("properties"), SparseProperties(TEXT("Tangent"), Tangent));
				ExpectedValues[0].Tangent.ArriveTangent = 9.5f;
			}
			else
			{
				TSharedPtr<FJsonObject> Added; FString TemplateText;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(KeyTemplate.ToSharedRef(), TemplateText, Error)
					|| !FUeAgentStructuredHjsonParser::ParseObject(TemplateText, TEXT("exported key template"), Added, Error)) return false;
				Added->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("4500"));
				Added->SetStringField(TEXT("value_lexeme"), TEXT("2.0000000000000004"));
				Added->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"))->SetObjectField(TEXT("InterpMode"), Selected(TEXT("RCIM_Linear")));
				Keys->SetObjectField(TEXT("Added"), Added);
				FMovieSceneDoubleValue Native(2.0000000000000004); Native.InterpMode = RCIM_Linear;
				ExpectedValues.Insert(Native, 1); ExpectedTimes.Insert(FFrameNumber(4500), 1);
			}
		}
		else if (Phase == 0)
		{
			if (bTransform)
			{
				const auto ChannelMap = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"));
				const auto Original = ChannelMap->GetObjectField(SelectedName);
				const auto NativeBefore = CapturePersistentObject(*Target->GetAllSections()[0]);
				TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
				auto RejectMembership = [&](const TCHAR* Code) {
					FString InvalidText;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), InvalidText, Error)
						|| !FFileHelper::SaveStringToFile(InvalidText, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
					const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
					TArray<uint8> DiskAfter;
					return Test.TestTrue(TEXT("native channel membership rejected before writes"), !Rejected.IsOk() && Rejected.GetErrorJson().Contains(Code)
						&& NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target->GetAllSections()[0]))
						&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore && !Sequence->GetOutermost()->IsDirty());
				};
				ChannelMap->RemoveField(SelectedName);
				if (!RejectMembership(TEXT("keyed_item_remove_unsupported"))) return false;
				ChannelMap->SetObjectField(SelectedName, Original);
				ChannelMap->SetObjectField(TEXT("UnknownChannel"), MakeShared<FJsonObject>());
				if (!RejectMembership(TEXT("keyed_item_create_unsupported"))) return false;
				ChannelMap->RemoveField(TEXT("UnknownChannel"));
			}
			Test.TestEqual(TEXT("initial double decimal lexeme preserves precision"), Keys->GetObjectField(TEXT("Key1"))->GetStringField(TEXT("value_lexeme")), FString(TEXT("1.0000000000000002")));
			Keys->GetObjectField(TEXT("Key1"))->SetStringField(TEXT("value_lexeme"), TEXT("1.0000000000000004"));
			Keys->GetObjectField(TEXT("Key1"))->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("12000"));
			Keys->RemoveField(TEXT("Key2")); Keys->SetObjectField(TEXT("Added"), NewKey(TEXT("3000"), TEXT("2.0000000000000004")));
			ExpectedValues.SetNum(1); ExpectedValues[0].Value = 1.0000000000000004;
			ExpectedValues.Insert(FMovieSceneDoubleValue(2.0000000000000004), 0);
			ExpectedTimes = {FFrameNumber(3000), FFrameNumber(12000)};
		}
		else if (Phase == 1) { Keys->Values.Reset(); ExpectedValues.Reset(); ExpectedTimes.Reset(); }
		else if (Phase == 2)
		{
			Keys->SetObjectField(TEXT("AfterEmpty"), NewKey(TEXT("4500"), TEXT("3.1415926535897931")));
			ExpectedValues = {FMovieSceneDoubleValue(3.1415926535897931)}; ExpectedTimes = {FFrameNumber(4500)};
		}
		else
		{
			TSharedPtr<FJsonObject> Added; FString TemplateText;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(SectionTemplate.ToSharedRef(), TemplateText, Error)
				|| !FUeAgentStructuredHjsonParser::ParseObject(TemplateText, TEXT("exported section template"), Added, Error)) return false;
			Added->GetObjectField(TEXT("channels"))->GetObjectField(SelectedName)->GetObjectField(TEXT("keys"))
				->SetObjectField(TEXT("PreciseKey"), NewKey(TEXT("45"), TEXT("1.0000000000000002")));
			Sections->SetObjectField(TEXT("AddedSection"), Added);
		}
		FString Text;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Target = nullptr; InitialSection = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { Test.AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		Test.TestFalse(TEXT("mixed numeric old object independently exits"), Old.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Target = Sequence ? FindTarget(Sequence) : nullptr;
		if (!Test.TestTrue(TEXT("Double track retains expected sections"), Target && Target->GetAllSections().Num() == (Phase == 3 ? 2 : 1))) return false;
		auto* Section = Target->GetAllSections()[0];
		if (!Section) return false;
		TArray<FFrameNumber> Times; Times.Append((*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetTimes());
		TArray<FMovieSceneDoubleValue> Values; Values.Append((*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues());
		Test.TestTrue(TEXT("double times values full tangents interpolation and default persist exactly"),
			Times == ExpectedTimes && Values == ExpectedValues && (*Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault() == Default);
		if (bSettings)
		{
			const auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0);
			Test.TestTrue(TEXT("both extrapolation modes persist exactly"), Channel->PreInfinityExtrap == RCCE_CycleWithOffset && Channel->PostInfinityExtrap == RCCE_CycleWithOffset);
			double Result = 0;
			if (Phase == 0)
			{
				Test.TestTrue(TEXT("pre range cycle offset evaluates native curve"), Channel->Evaluate(FFrameTime(-9000), Result)
					&& Result == ExpectedValues[1].Value + (ExpectedValues[1].Value - ExpectedValues[0].Value) * -2);
				Test.TestTrue(TEXT("post range cycle offset evaluates native curve"), Channel->Evaluate(FFrameTime(18000), Result)
					&& Result == ExpectedValues[0].Value + (ExpectedValues[1].Value - ExpectedValues[0].Value) * 2);
			}
			else Test.TestTrue(TEXT("empty channel distinguishes precise default from no value"),
				Channel->Evaluate(FFrameTime(4500), Result) == Default.IsSet() && (!Default.IsSet() || Result == Default.GetValue()));
		}
		if (bProperties && Phase == 0)
		{
			double CurveAfter = 0; const bool bEvaluated = Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)->Evaluate(FFrameTime(4500), CurveAfter);
			Test.TestTrue(TEXT("native intermediate curve evaluation reflects edited interpolation"), bEvaluated && CurveAfter == ExpectedValues[0].Value && CurveAfter != CurveBefore);
		}
		Test.TestTrue(TEXT("original Double section remains unbounded"), Section->GetRange().GetLowerBound().IsOpen() && Section->GetRange().GetUpperBound().IsOpen());
		if (Phase == 3)
		{
			auto* Added = Target->GetAllSections()[1];
			auto* DefaultTrack = NewObject<UMovieSceneTrack>(GetTransientPackage(), Target->GetClass());
			if (bVector) CastChecked<UMovieSceneDoubleVectorTrack>(DefaultTrack)->SetNumChannelsUsed(3);
			auto* NativeDefault = DefaultTrack->CreateNewSection();
			Test.TestTrue(TEXT("new section preserves native default channels without cloning user keys"), NativeDefault
				&& Added->GetClass() == NativeDefault->GetClass()
				&& CaptureOtherChannels(*Added, SelectedName).OrderIndependentCompareEqual(CaptureOtherChannels(*NativeDefault, SelectedName)));
			if (bTransform) Test.TestTrue(TEXT("new Transform section keeps native default mask"),
				CastChecked<UMovieScene3DTransformSection>(Added)->GetMask().GetChannels() == CastChecked<UMovieScene3DTransformSection>(NativeDefault)->GetMask().GetChannels());
			Test.TestTrue(TEXT("new section inherits actual Double type and preserves precise new key"), Added
				&& (*Added->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues().Num() == 1
				&& (*Added->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetData().GetValues()[0].Value == 1.0000000000000002
				&& (bTransform ? (*Added->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault().GetValue() == 0.0 : !(*Added->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(0)).GetDefault().IsSet()));
		}
		Test.TestTrue(TEXT("all unselected native channels stay intact"), OtherChannels.OrderIndependentCompareEqual(CaptureOtherChannels(*Section, SelectedName)));
		if (bVector) for (auto* Track : Sequence->GetMovieScene()->GetTracks())
		{
			const FString Label = Track->GetDisplayName().ToString();
			if (Label != TEXT("Color") && Label != TEXT("Vector2") && Label != TEXT("Precise")) continue;
			const int32 Kind = Label == TEXT("Color") ? 0 : Label == TEXT("Vector2") ? 1 : 2;
			for (auto* NativeSection : Track->GetAllSections()) Test.TestTrue(TEXT("native class dimension and channel metadata persist exactly"), HasVectorColorLayout(*Track, *NativeSection, Kind));
			if (Kind == 2) continue;
			auto* NativeSection = Track->GetAllSections()[0]; const auto* Channel = NativeSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
			FString FullChannel; FMovieSceneFloatChannel::StaticStruct()->ExportText(FullChannel, Channel, nullptr, nullptr, PPF_None, nullptr);
			Test.TestEqual(TEXT("complete target Float channel including untouched key properties stays exact"), FullChannel, ExpectedAuxiliarySelected[Label]);
			const auto AuxiliaryValues = Channel->GetData().GetValues(); const auto AuxiliaryTimes = Channel->GetData().GetTimes();
			Test.TestTrue(TEXT("Color and FloatVector value time default and reflected key fields persist"), AuxiliaryValues.Num() == 2
				&& AuxiliaryValues[0].Value == 0.375f && AuxiliaryTimes[0] == FFrameNumber(1200) && AuxiliaryValues[0].InterpMode == RCIM_Constant
				&& AuxiliaryValues[0].TangentMode == RCTM_Break && AuxiliaryValues[0].Tangent.LeaveTangent == 4.5f && AuxiliaryValues[0].Tangent.ArriveTangent == 12.5f
				&& AuxiliaryValues[1].Value == static_cast<float>(9007199254740991.0) && AuxiliaryTimes[1] == FFrameNumber(9000)
				&& Channel->GetDefault().IsSet() && Channel->GetDefault().GetValue() == 0.625f
				&& AuxiliaryChannels[Label].OrderIndependentCompareEqual(CaptureOtherChannels(*NativeSection, Kind == 0 ? TEXT("Color.R") : TEXT("Vector.X"))));
			if (Phase == 3) { const auto All = CaptureOtherChannels(*Track->GetAllSections()[1], TEXT(""));
				auto* DefaultTrack = NewObject<UMovieSceneTrack>(GetTransientPackage(), Track->GetClass());
				if (Kind == 1) CastChecked<UMovieSceneFloatVectorTrack>(DefaultTrack)->SetNumChannelsUsed(2);
				Test.TestTrue(TEXT("Color and FloatVector new sections exactly match native defaults"), All.OrderIndependentCompareEqual(CaptureOtherChannels(*DefaultTrack->CreateNewSection(), TEXT("")))); }
		}
		if (bTransform) Test.TestTrue(TEXT("independent ten-channel native layout and Transform mask preserved"), HasNativeTransformChannels(*Section) && CastChecked<UMovieScene3DTransformSection>(Section)->GetMask().GetChannels() == Mask);
		Owned.Reset(); GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
		int32 Count = 0;
		for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target))
		{
			if (bVector && (Object->GetTypedOuter<UMovieSceneColorTrack>() || Object->GetTypedOuter<UMovieSceneFloatVectorTrack>()
				|| Object->IsA<UMovieSceneColorTrack>() || Object->IsA<UMovieSceneFloatVectorTrack>())) continue;
			++Count; const auto* Before = NonTarget.Find(Object->GetPathName(Sequence->GetMovieScene()));
			Test.TestTrue(TEXT("complete non-target Float Double and bound owned state preserved"), Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object)));
		}
		Owned.Reset(); Test.TestEqual(TEXT("non-target object count unchanged"), Count, NonTarget.Num());
		Test.TestTrue(TEXT("mixed numeric edits preserve original real bindings"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component)
			&& HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		Test.TestFalse(TEXT("mixed numeric independent readback clean"), Sequence->GetOutermost()->IsDirty());
		if (Test.HasAnyErrors()) return false;
	}
	Test.AddInfo(TEXT("NumericChannelsPublic precision=double phases=value_time_collection,empty,readd,exported_track_section_template native_proxy_inventory_independent=1 full_non_target=1 real_binding=1 independent_reload=1"));
	return !Test.HasAnyErrors();
}

bool FUeAgentT85SequenceDoubleNumericTest::RunTest(const FString&) { return RunNumericChannelsPublic(*this, false); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceTransformChannelsTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.TransformPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceTransformChannelsTest::RunTest(const FString&) { return RunNumericChannelsPublic(*this, true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceColorVectorTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ColorVectorPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceColorVectorTest::RunTest(const FString&) {
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	auto* Invalid = NewObject<ULevelSequence>(GetTransientPackage()); Invalid->Initialize();
	Invalid->GetMovieScene()->AddTrack<UMovieSceneFloatVectorTrack>();
	TSharedPtr<FJsonObject> Read; FUeAgentTimelineAnimationExecutionError Error;
	if (!TestFalse(TEXT("missing Vector dimensions reject before native section creation"), TryReadLevelSequenceStructure(*Invalid, TEXT("invalid-context"), Read, Error))) return false;
	return RunNumericChannelsPublic(*this, false, false, false, true);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceVectorRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.VectorLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceVectorRecoveryTest::RunTest(const FString&) {
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, false, true);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceKeyPropertiesTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.KeyPropertiesPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceKeyPropertiesTest::RunTest(const FString&) { return RunNumericChannelsPublic(*this, true, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectPropertiesRecoveryTest,
	"UAI.Root.T85.LevelSequence.ObjectProperties.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceObjectPropertiesRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceGroupConditionRecoveryTest,
	"UAI.Root.T85.LevelSequence.ObjectProperties.GroupConditionLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceGroupConditionRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectPropertiesPublicTest,
	"UAI.Root.T85.LevelSequence.ObjectProperties.Public",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceObjectPropertiesPublicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	const FString Name = TEXT("LS_Properties_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceProperties/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/Properties") / Name;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!Component) return false;
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional); Sequence->Initialize();
	FTimelineAnimationStructure Initial; FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	auto* Target = Sequence->GetMovieScene()->GetTracks()[0]; CastChecked<UMovieSceneFloatTrack>(Target)->SetDisplayName(FText::FromString(TEXT("Editable")));
	Target->SetColorTint(FColor(13, 27, 41, 255)); Target->EvalOptions.bEvaluateInPostroll = true;
	Target->GetAllSections()[0]->Easing.bManualEaseIn = true;
	Target->GetAllSections()[0]->Easing.ManualEaseInDuration = 21;
	Target->GetAllSections()[0]->TimecodeSource.Timecode.Frames = 7;
	FGuid ComponentGuid, ActorGuid;
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true); ActorGuid = Sequencer->GetHandleToObject(Actor, false); }
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs)) return false;
	FString Error; Sequence = nullptr; Target = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false;
	Target = Sequence->GetMovieScene()->GetTracks()[0];
	auto TrackBefore = CapturePersistentObject(*Target); TrackBefore.Remove(TEXT("EvalOptions")); TrackBefore.Remove(TEXT("TrackTint")); TrackBefore.Remove(TEXT("Sections"));
	TrackBefore.Remove(TEXT("DisplayName"));
	auto SectionBefore = CapturePersistentObject(*Target->GetAllSections()[0]); SectionBefore.Remove(TEXT("bIsActive"));
	FString TrackSignature = TrackBefore.FindAndRemoveChecked(TEXT("Signature"));
	FString SectionSignature = SectionBefore.FindAndRemoveChecked(TEXT("Signature"));
	TMap<FString, TMap<FString, FString>> NonTarget;
	TArray<UObject*> Owned; GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
	for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) NonTarget.Add(Object->GetPathName(Sequence->GetMovieScene()), CapturePersistentObject(*Object)); Owned.Reset();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	for (int32 Phase = 0; Phase < 2; ++Phase)
	{
		if (Phase == 1)
		{
			// Phase 0's real Sequencer binding read has already rebuilt its evaluation cache.
			// New-section preservation starts from that current native state, not the pre-edit cache.
			TrackBefore = CapturePersistentObject(*Target);
			TrackBefore.Remove(TEXT("EvalOptions")); TrackBefore.Remove(TEXT("TrackTint")); TrackBefore.Remove(TEXT("Sections"));
			TrackBefore.Remove(TEXT("DisplayName"));
			TrackBefore.Remove(TEXT("SectionToKey"));
			TrackSignature = TrackBefore.FindAndRemoveChecked(TEXT("Signature"));
			SectionSignature = CapturePersistentObject(*Target->GetAllSections()[0]).FindRef(TEXT("Signature"));
		}
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportArgs, Name,
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		TMap<FString, TSharedPtr<FJsonObject>> Templates;
		FString TargetFile, SectionRef; TSharedPtr<FJsonObject> Document, Track, Sections;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Read)};
			while (!Pending.IsEmpty()) { const auto Item = Pending.Pop(EAllowShrinking::No);
				if (Item->Type == EJson::Array) Pending.Append(Item->AsArray());
				else if (Item->Type == EJson::Object) { const auto Object = Item->AsObject(); FString DefinitionName, Ref;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName) && DefinitionName == TEXT("LevelSequenceScalarSection") && Object->TryGetStringField(TEXT("ref"), Ref))
						Templates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
					for (const auto& Pair : Object->Values) Pending.Add(Pair.Value); } }
			const FString TrackName = Phase == 0 ? TEXT("Editable") : TEXT("RenamedProperties");
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks")); if (!Tracks || !Tracks->HasField(TrackName)) continue;
			Document = Read; TargetFile = File; Track = Tracks->GetObjectField(TrackName); Sections = Track->GetObjectField(TEXT("sections"));
			SectionRef = FindObjectFieldRecursive(Read, TEXT("section_definitions"))->GetStringField(TrackName);
		}
		if (!Track || !Templates.Contains(SectionRef)) { AddError(TEXT("actual public property track/template missing")); return false; }
		const auto TrackProperties = Track->GetObjectField(TEXT("properties"));
		TestFalse(TEXT("display_name has no reflected DisplayName second owner"), FindObjectFieldRecursive(TrackProperties, TEXT("DisplayName")).IsValid());
		TestTrue(TEXT("independent TrackRowDisplayNames remains qualified"), FindObjectFieldRecursive(TrackProperties, TEXT("TrackRowDisplayNames")).IsValid());
		const auto SectionProperties = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("properties"));
		TestFalse(TEXT("empty fixed Easing and its category leave ordinary authoring"), SectionProperties->HasField(TEXT("Easing")));
		TestFalse(TEXT("nested empty TimecodeSource leaves ordinary authoring"), FindObjectFieldRecursive(SectionProperties, TEXT("TimecodeSource")).IsValid());
		TestTrue(TEXT("null owned Condition remains actionable"), FindObjectFieldRecursive(SectionProperties, TEXT("ConditionContainer")).IsValid());
		for (const auto& Category : SectionProperties->Values)
			TestTrue(TEXT("ordinary section categories contain actionable properties"), Category.Value->AsObject()->Values.Num() > 0);
		const auto Eval = FindObjectFieldRecursive(TrackProperties, TEXT("EvalOptions"));
		const auto Tint = FindObjectFieldRecursive(TrackProperties, TEXT("TrackTint"));
		const auto Active = FindObjectFieldRecursive(SectionProperties, TEXT("bIsActive"));
		if (!TestTrue(TEXT("public categorized typed track and section properties exist"), Eval && Tint && Active
			&& Eval->HasField(TEXT("type")) && FindObjectFieldRecursive(Document, TEXT("object_property_definitions")))) return false;
		TestEqual(TEXT("nested reflected boolean remains a direct public boolean"),
			Eval->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->TryGetField(TEXT("bEvaluateInPreroll"))->Type, EJson::Boolean);
		const auto SectionEval = FindObjectFieldRecursive(SectionProperties, TEXT("EvalOptions"));
		TestEqual(TEXT("nested enum remains its direct readable name"),
			SectionEval->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->TryGetField(TEXT("CompletionMode"))->Type, EJson::String);
		const auto Condition = FindObjectFieldRecursive(SectionProperties, TEXT("ConditionContainer"));
		TestEqual(TEXT("nested null reference remains direct null"),
			Condition->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->TryGetField(TEXT("Condition"))->Type, EJson::Null);
		const auto PreRoll = FindObjectFieldRecursive(SectionProperties, TEXT("PreRollFrames"));
		TestEqual(TEXT("nested numeric value remains a direct public number"),
			PreRoll->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->TryGetField(TEXT("Value"))->Type, EJson::Number);
		TestFalse(TEXT("range has no reflected second owner"), FindObjectFieldRecursive(SectionProperties, TEXT("SectionRange")).IsValid());
		TestFalse(TEXT("noneditable row index excluded"), FindObjectFieldRecursive(SectionProperties, TEXT("RowIndex")).IsValid());
		auto SaveDocument = [&]() { FString Text; return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); };
		if (Phase == 0)
		{
			TArray<uint8> BeforeDisk; FFileHelper::LoadFileToArray(BeforeDisk, *Filename);
			const auto NativeBefore = CapturePersistentObject(*Target->GetAllSections()[0]);
			const auto NativeTrackBefore = CapturePersistentObject(*Target);
			for (const FString InvalidName : {FString(TEXT("NotAProperty")), FString(TEXT("SectionRange")), FString(TEXT("DisplayName"))})
			{
				const auto InvalidCategory = MakeShared<FJsonObject>(); InvalidCategory->SetObjectField(InvalidName, Active);
				if (InvalidName == TEXT("DisplayName"))
				{
					const auto Duplicate = MakeShared<FJsonObject>(); Duplicate->SetStringField(TEXT("type"), TEXT("text")); Duplicate->SetStringField(TEXT("value"), TEXT("ForbiddenDuplicate"));
					TrackProperties->GetObjectField(TEXT("Track"))->SetObjectField(InvalidName, Duplicate);
				}
				else SectionProperties->SetObjectField(TEXT("InvalidCategory"), InvalidCategory);
				if (!SaveDocument()) return false;
				const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
				TestFalse(TEXT("unknown and structural duplicate properties reject before writes"), Rejected.IsOk());
				if (!Rejected.IsOk()) AddInfo(Rejected.GetErrorJson());
				// Reflection's rejected transaction may replace the native instance during recovery.
				Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
				if (!Sequence || Sequence->GetMovieScene()->GetTracks().IsEmpty()) return false;
				Target = Sequence->GetMovieScene()->GetTracks()[0];
				TArray<uint8> AfterDisk; FFileHelper::LoadFileToArray(AfterDisk, *Filename);
				TestTrue(TEXT("rejected property leaves disk and native section identical"), BeforeDisk == AfterDisk && NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target->GetAllSections()[0])));
				TestTrue(TEXT("rejected duplicate owner leaves complete native track unchanged"), NativeTrackBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target)));
				if (InvalidName == TEXT("DisplayName")) TrackProperties->GetObjectField(TEXT("Track"))->RemoveField(InvalidName);
				else SectionProperties->RemoveField(TEXT("InvalidCategory"));
			}
			const auto EvalValue = MakeShared<FJsonObject>(); EvalValue->SetBoolField(TEXT("bEvaluateInPreroll"), true);
			const auto EvalStruct = MakeShared<FJsonObject>(); EvalStruct->SetObjectField(TEXT("fields"), EvalValue); Eval->SetObjectField(TEXT("value"), EvalStruct);
			auto TintValue = Tint->GetArrayField(TEXT("value"));
			if (!TestEqual(TEXT("public color8 codec has four components"), TintValue.Num(), 4)) return false;
			TintValue[0] = MakeShared<FJsonValueNumber>(91); Tint->SetArrayField(TEXT("value"), TintValue);
			Active->SetBoolField(TEXT("value"), false);
			Track->SetStringField(TEXT("display_name"), TEXT("RenamedProperties"));
		}
		else { FString Text; TSharedPtr<FJsonObject> Added;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Templates[SectionRef].ToSharedRef(), Text, Error)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, TEXT("native property section template"), Added, Error)) return false;
			Sections->SetObjectField(TEXT("Added"), Added); }
		if (!SaveDocument()) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Target = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		TestFalse(TEXT("old property owner exits before independent reload"), Old.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false; Target = Sequence->GetMovieScene()->GetTracks()[0];
		TestTrue(TEXT("recursive track fields persist with omitted fields preserved"), Target->EvalOptions.bEvaluateInPreroll && Target->EvalOptions.bEvaluateInPostroll && Target->GetColorTint() == FColor(91,27,41,255));
		TestFalse(TEXT("section active property persisted"), Target->GetAllSections()[0]->IsActive());
		TestEqual(TEXT("structural display_name rename persists with property edits and new section"), Target->GetDisplayName().ToString(), FString(TEXT("RenamedProperties")));
		auto ActualTrack = CapturePersistentObject(*Target); ActualTrack.Remove(TEXT("EvalOptions")); ActualTrack.Remove(TEXT("TrackTint")); ActualTrack.Remove(TEXT("Sections"));
		ActualTrack.Remove(TEXT("DisplayName"));
		auto ActualSection = CapturePersistentObject(*Target->GetAllSections()[0]); ActualSection.Remove(TEXT("bIsActive"));
		const FString NewTrackSignature = ActualTrack.FindAndRemoveChecked(TEXT("Signature"));
		const FString NewSectionSignature = ActualSection.FindAndRemoveChecked(TEXT("Signature"));
		TestTrue(TEXT("native notifications update only the edited owners' signatures"), TrackSignature != NewTrackSignature
			&& (Phase == 0 ? SectionSignature != NewSectionSignature : SectionSignature == NewSectionSignature));
		if (Phase == 1)
		{
			ActualTrack.Remove(TEXT("SectionToKey"));
			TestTrue(TEXT("native AddSection selects the newly added section for keying"),
				CastChecked<UMovieSceneFloatTrack>(Target)->GetSectionToKey() == Target->GetAllSections()[1]);
		}
		for (const auto& Field : TrackBefore) if (ActualTrack.FindRef(Field.Key) != Field.Value)
			AddError(TEXT("untouched track field=") + Field.Key + TEXT(" before=") + Field.Value + TEXT(" after=") + ActualTrack.FindRef(Field.Key));
		for (const auto& Field : SectionBefore) if (ActualSection.FindRef(Field.Key) != Field.Value)
			AddError(TEXT("untouched section field=") + Field.Key + TEXT(" before=") + Field.Value + TEXT(" after=") + ActualSection.FindRef(Field.Key));
		TestTrue(TEXT("complete untouched native properties and channels retained"), TrackBefore.OrderIndependentCompareEqual(ActualTrack) && SectionBefore.OrderIndependentCompareEqual(ActualSection));
		if (Phase == 1) { auto* NativeTrack = NewObject<UMovieSceneFloatTrack>(); auto* NativeDefault = NativeTrack->CreateNewSection();
			TestTrue(TEXT("new section uses native properties and empty native channel defaults"), Target->GetAllSections().Num() == 2 && Target->GetAllSections()[1]->IsActive()
				&& CaptureOtherChannels(*Target->GetAllSections()[1], TEXT("")).OrderIndependentCompareEqual(CaptureOtherChannels(*NativeDefault, TEXT(""))));
			TSharedPtr<FJsonObject> NativeReadback; FString ActualProperties, DefaultProperties;
			if (!TryReadLevelSequenceStructure(*Sequence, ObjectPath, NativeReadback, NativeError)) return false;
			const auto AddedProperties = NativeReadback->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[1]->AsObject()->GetObjectField(TEXT("properties"));
			if (!CommandContract::Json::TryCanonicalize(AddedProperties, ActualProperties, Error)
				|| !CommandContract::Json::TryCanonicalize(Templates[SectionRef]->GetObjectField(TEXT("properties")), DefaultProperties, Error)) return false;
			TestEqual(TEXT("all new-section public properties equal the exported native default template"), ActualProperties, DefaultProperties); }
		Owned.Reset(); GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true); int32 Count = 0;
		for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) { ++Count; const auto* Before = NonTarget.Find(Object->GetPathName(Sequence->GetMovieScene()));
			TestTrue(TEXT("complete non-target owned state retained"), Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object))); } Owned.Reset();
		TestEqual(TEXT("non-target owned count retained"), Count, NonTarget.Num());
		TestTrue(TEXT("real bindings retained"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		TestFalse(TEXT("property persistent readback clean"), Sequence->GetOutermost()->IsDirty()); if (HasAnyErrors()) return false;
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceStringPublicTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.StringPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceGroupConditionPublicTest,
	"UAI.Root.T85.LevelSequence.ObjectProperties.GroupConditionPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceGroupConditionPublicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	const FString Name = TEXT("LS_Group_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceGroup/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/Group") / Name;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	auto* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	auto* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr; if (!Component) return false;
	UPackage* Package = CreatePackage(*PackageName);
	auto* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional); Sequence->Initialize();
	FTimelineAnimationStructure Initial; FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	auto* Track = CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0]); Track->SetDisplayName(FText::FromString(TEXT("ConditionOwner")));
	AddGroupConditionFixture(*Track);
	FGuid ComponentGuid, ActorGuid;
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true); ActorGuid = Sequencer->GetHandleToObject(Actor, false); }
	FAssetRegistryModule::AssetCreated(Sequence); FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Sequence, *Filename, Save)) return false;
	FString Error; Sequence = nullptr; Track = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath); ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	for (int32 Phase = 0; Phase < 2; ++Phase) {
		TestTrue(TEXT("actual preview bindings resolve before baseline"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportArgs, Name,
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
		FString TargetFile, AllText; TSharedPtr<FJsonObject> Document, PublicTrack;
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			AllText += Text;
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks"));
			if (Tracks && Tracks->HasField(TEXT("ConditionOwner"))) { Document = Read; TargetFile = File; PublicTrack = Tracks->GetObjectField(TEXT("ConditionOwner")); }
		}
		if (!PublicTrack) { AddError(TEXT("group public track missing")); return false; }
		const auto Row = FindObjectFieldRecursive(PublicTrack->GetObjectField(TEXT("properties")), TEXT("ConditionContainer"));
		const TSharedPtr<FJsonObject> *RowValue = nullptr, *Fields = nullptr, *ConditionValue = nullptr, *OwnedPtr = nullptr;
		const TSharedPtr<FJsonObject>* OwnedProperties = nullptr;
		if (!TestTrue(TEXT("existing condition has the public owned shape"), Row && Row->TryGetObjectField(TEXT("value"), RowValue)
			&& (*RowValue)->TryGetObjectField(TEXT("fields"), Fields) && (*Fields)->TryGetObjectField(TEXT("Condition"), ConditionValue)
			&& (*ConditionValue)->TryGetObjectField(TEXT("owned_object"), OwnedPtr) && (*OwnedPtr)->TryGetObjectField(TEXT("properties"), OwnedProperties))) return false;
		const auto Owned = *OwnedPtr;
		TestTrue(TEXT("owned enum definition and real candidates are public"), AllText.Contains(TEXT("EMovieSceneGroupConditionOperator")) && AllText.Contains(TEXT("Xor")));
		bool bOperator = false, bChildren = false;
		for (const auto& Category : (*OwnedProperties)->Values)
			for (const auto& Item : Category.Value->AsObject()->Values) {
				const auto Property = Item.Value->AsObject();
				const auto Value = Property->TryGetField(TEXT("value"));
				FString Type;
				if (!TestTrue(TEXT("owned property uses the same typed row as outer UObject properties"), Property->Values.Num() == 2
					&& Property->TryGetStringField(TEXT("type"), Type) && !Type.IsEmpty() && Value)) return false;
				bOperator |= Item.Key == TEXT("Operator") && Type == TEXT("EMovieSceneGroupConditionOperator") && Value->Type == EJson::String;
				bChildren |= Item.Key == TEXT("SubConditions") && Value->Type == EJson::Array;
				if (Item.Key == TEXT("SubConditions")) {
					const auto ChildProperties = FindObjectFieldRecursive(Value->AsArray()[0]->AsObject(), TEXT("properties"));
					const auto ChildOperator = FindObjectFieldRecursive(ChildProperties, TEXT("Operator"));
					if (!TestTrue(TEXT("nested child group has directly readable type and value"), ChildOperator
						&& ChildOperator->GetStringField(TEXT("type")) == TEXT("EMovieSceneGroupConditionOperator")
						&& ChildOperator->GetStringField(TEXT("value")) == TEXT("Or"))) return false;
				}
			}
		if (!TestTrue(TEXT("existing group has readable operator and nested owned children"), bOperator && bChildren)) return false;
		Track = CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0]);
		auto* Group = CastChecked<UMovieSceneGroupCondition>(Track->ConditionContainer.Condition);
		auto* Child = CastChecked<UMovieSceneGroupCondition>(Group->SubConditions[0].Condition);
		const FString GroupPath = Group->GetPathName(Sequence), ChildPath = Child->GetPathName(Sequence), TrackPath = Track->GetPathName(Sequence), MovieScenePath = Sequence->GetMovieScene()->GetPathName(Sequence);
		auto CaptureGraph = [](ULevelSequence* Asset) {
			TMap<FString, TMap<FString, FString>> Graph; Graph.Add(TEXT("$root"), CapturePersistentObject(*Asset));
			TArray<UObject*> Objects; GetObjectsWithOuter(Asset, Objects, true);
			for (auto* Object : Objects) Graph.Add(Object->GetPathName(Asset), CapturePersistentObject(*Object)); return Graph;
		};
		auto Before = CaptureGraph(Sequence);
		TMap<FString, FString> Signatures;
		for (const FString Path : {FString(TEXT("$root")), MovieScenePath, TrackPath}) Signatures.Add(Path, Before[Path].FindAndRemoveChecked(TEXT("Signature")));
		if (Phase == 0) { Signatures.Add(GroupPath, Before[GroupPath].FindAndRemoveChecked(TEXT("Signature"))); Before[GroupPath].Remove(TEXT("Operator")); }
		else { Before.Remove(GroupPath); Before.Remove(ChildPath); Before[TrackPath].Remove(TEXT("ConditionContainer")); }
		if (Phase == 0) { if (!SetSparseGroupOperator(Row, TEXT("Xor"))) return false; }
		else Row->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetField(TEXT("Condition"), MakeShared<FJsonValueNull>());
		FString Text; if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; TWeakObjectPtr<UMovieSceneGroupCondition> OldGroup = Group, OldChild = Child;
		Sequence = nullptr; Track = nullptr; Group = nullptr; Child = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		TestTrue(TEXT("old complete owned graph exits before independent readback"), !Old.IsValid(true, true) && !OldGroup.IsValid(true, true) && !OldChild.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false;
		Track = CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0]);
		if (Phase == 0) {
			Group = Cast<UMovieSceneGroupCondition>(Track->ConditionContainer.Condition);
			if (!TestTrue(TEXT("selected operator persists on original owned identity"), Group && Group->GetPathName(Sequence) == GroupPath && Group->Operator == EMovieSceneGroupConditionOperator::Xor && Group->SubConditions.Num() == 1)) return false;
			Child = Cast<UMovieSceneGroupCondition>(Group->SubConditions[0].Condition);
			TestTrue(TEXT("nested child identity and operator remain intact"), Child && Child->GetPathName(Sequence) == ChildPath && Child->Operator == EMovieSceneGroupConditionOperator::Or);
		} else TestNull(TEXT("explicit null clear persists"), Track->ConditionContainer.Condition.Get());
		auto After = CaptureGraph(Sequence);
		for (const auto& Pair : Signatures) {
			const FString Signature = After[Pair.Key].FindAndRemoveChecked(TEXT("Signature"));
			TestTrue(TEXT("native edited owner and signed ancestors receive notification"), Signature != Pair.Value);
		}
		if (Phase == 0) After[GroupPath].Remove(TEXT("Operator")); else After[TrackPath].Remove(TEXT("ConditionContainer"));
		TestEqual(TEXT("exact owned graph inventory persists or clears"), After.Num(), Before.Num());
		for (const auto& Pair : Before) {
			const auto* Actual = After.Find(Pair.Key);
			if (!Actual || !Pair.Value.OrderIndependentCompareEqual(*Actual)) AddError(TEXT("untouched owned native state differs: ") + Pair.Key);
		}
		TestFalse(TEXT("independent owned graph readback is clean"), Sequence->GetOutermost()->IsDirty());
		TestTrue(TEXT("real binding remains after owned edit"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		if (HasAnyErrors()) return false;
	}
	return !HasAnyErrors();
}
bool FUeAgentT85SequenceStringPublicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	const FString Name = TEXT("LS_String_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceString/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/String") / Name;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!Component) return false;
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional); Sequence->Initialize();
	FTimelineAnimationStructure Initial; FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	AddStringFixtureTrack(*Sequence->GetMovieScene()); AddBoolFixtureTrack(*Sequence->GetMovieScene(), TEXT("OtherBool"));
	{
		auto* Section = Sequence->GetMovieScene()->GetTracks()[1]->GetAllSections()[0];
		auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
		const auto Before = CapturePersistentObject(*Section);
		FTimelineAnimationChannel Invalid; Invalid.DefaultIsSet = true; Invalid.DefaultValueLexeme = TEXT("must not apply");
		Invalid.Properties = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
		ScalarChannel::FChannelEdits Edits;
		TestFalse(TEXT("String rejects invented channel properties before applying prepared default"), Edits.PrepareSettings(*Channel, *Section, Invalid));
		FTimelineAnimationKey InvalidKey; InvalidKey.Properties = Invalid.Properties; FString Value = TEXT("original");
		TestFalse(TEXT("String rejects invented key tangent properties"), ScalarChannel::DecodeProperties(InvalidKey, Value, Section));
		TestTrue(TEXT("rejected String property preparation leaves full native section unchanged"), Before.OrderIndependentCompareEqual(CapturePersistentObject(*Section)));
	}
	FGuid ComponentGuid, ActorGuid;
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true); ActorGuid = Sequencer->GetHandleToObject(Actor, false); }
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs)) return false;
	FString Error; Sequence = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false;
	auto FindTarget = [](ULevelSequence* Asset) -> UMovieSceneTrack* {
		for (auto* Track : Asset->GetMovieScene()->GetTracks()) if (Track->GetDisplayName().ToString() == TEXT("Precise")) return Track; return nullptr; };
	auto* Target = FindTarget(Sequence); if (!Target) return false;
	TMap<FString, TMap<FString, FString>> NonTarget;
	TArray<UObject*> Owned; GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
	for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) NonTarget.Add(Object->GetPathName(Sequence->GetMovieScene()), CapturePersistentObject(*Object));
	Owned.Reset();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	TArray<FString> ExpectedValues; TArray<FFrameNumber> ExpectedTimes; TOptional<FString> ExpectedDefault;
	for (int32 Phase = 0; Phase < 5; ++Phase)
	{
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportArgs, Name,
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		TMap<FString, TSharedPtr<FJsonObject>> Templates, KeyTemplates;
		FString TargetFile, SectionRef, KeyRef; TSharedPtr<FJsonObject> Document, Sections, Channel;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Read)};
			while (!Pending.IsEmpty())
			{
				const auto Item = Pending.Pop(EAllowShrinking::No);
				if (Item->Type == EJson::Array) Pending.Append(Item->AsArray());
				else if (Item->Type == EJson::Object) { const auto Object = Item->AsObject(); FString DefinitionName, Ref;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName) && Object->TryGetStringField(TEXT("ref"), Ref))
					{
						if (DefinitionName == TEXT("LevelSequenceScalarSection")) Templates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
						if (DefinitionName == TEXT("LevelSequenceScalarKey")) KeyTemplates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
					}
					for (const auto& Pair : Object->Values) Pending.Add(Pair.Value); }
			}
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks"));
			if (!Tracks || !Tracks->HasField(TEXT("Precise"))) continue;
			Document = Read; TargetFile = File; Sections = Tracks->GetObjectField(TEXT("Precise"))->GetObjectField(TEXT("sections"));
			Channel = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("Value"));
			SectionRef = FindObjectFieldRecursive(Read, TEXT("section_definitions"))->GetStringField(TEXT("Precise"));
			KeyRef = FindObjectFieldRecursive(Read, TEXT("key_definitions"))->GetObjectField(TEXT("Precise"))->GetStringField(TEXT("Value"));
		}
		if (!Channel || !Templates.Contains(SectionRef) || !KeyTemplates.Contains(KeyRef)) return false;
		const auto Keys = Channel->GetObjectField(TEXT("keys")); const auto KeyTemplate = KeyTemplates[KeyRef];
		if (!TestTrue(TEXT("String native template has empty text and no invented properties"), KeyTemplate->GetStringField(TEXT("value_type")) == TEXT("string")
			&& KeyTemplate->GetStringField(TEXT("value_lexeme")).IsEmpty() && !KeyTemplate->HasField(TEXT("properties")) && !Channel->HasField(TEXT("properties")))) return false;
		auto NewKey = [&](const TCHAR* Tick, const TCHAR* Text) { const auto Key = MakeShared<FJsonObject>();
			const auto Time = MakeShared<FJsonObject>(); Time->SetStringField(TEXT("tick"), Tick); Key->SetObjectField(TEXT("time"), Time);
			Key->SetStringField(TEXT("value_type"), KeyTemplate->GetStringField(TEXT("value_type"))); Key->SetStringField(TEXT("value_lexeme"), Text); return Key; };
		if (Phase == 0)
		{
			TestEqual(TEXT("Unicode quote and newline export exactly"), Keys->GetObjectField(TEXT("Key1"))->GetStringField(TEXT("value_lexeme")), FString(TEXT("原文\"quoted\"\n下一行")));
			Keys->GetObjectField(TEXT("Key1"))->SetStringField(TEXT("value_lexeme"), TEXT("改写\"quoted\"\n第二行\\path"));
			Keys->GetObjectField(TEXT("Key1"))->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
			Keys->RemoveField(TEXT("Key2")); Keys->SetObjectField(TEXT("Empty"), NewKey(TEXT("3000"), TEXT("")));
			ExpectedValues = {TEXT(""), TEXT("改写\"quoted\"\n第二行\\path")}; ExpectedTimes = {FFrameNumber(3000), FFrameNumber(6000)}; ExpectedDefault = TEXT("默认\"text\"\n换行");
		}
		else if (Phase == 1) { Keys->Values.Reset(); ExpectedValues.Reset(); ExpectedTimes.Reset(); ExpectedDefault = FString(); }
		else if (Phase == 2) ExpectedDefault.Reset();
		else if (Phase == 3) { Keys->SetObjectField(TEXT("Again"), NewKey(TEXT("4500"), TEXT(""))); ExpectedValues = {FString()}; ExpectedTimes = {FFrameNumber(4500)}; }
		else
		{
			FString Text; TSharedPtr<FJsonObject> Added;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Templates[SectionRef].ToSharedRef(), Text, Error)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, TEXT("native String template"), Added, Error)) return false;
			const auto AddedChannel = Added->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("Value"));
			TestTrue(TEXT("String section template has no copied keys or default"), AddedChannel->GetObjectField(TEXT("keys"))->Values.IsEmpty()
				&& !AddedChannel->GetObjectField(TEXT("default"))->GetBoolField(TEXT("is_set")));
			AddedChannel->GetObjectField(TEXT("keys"))->SetObjectField(TEXT("Text"), NewKey(TEXT("123"), TEXT("模板新文本"))); Sections->SetObjectField(TEXT("AddedSection"), Added);
		}
		const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), ExpectedDefault.IsSet());
		if (ExpectedDefault.IsSet()) Default->SetStringField(TEXT("value_lexeme"), ExpectedDefault.GetValue()); Channel->SetObjectField(TEXT("default"), Default);
		FString Text; if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Target = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		TestFalse(TEXT("String old instance exits before independent reload"), Old.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Target = Sequence ? FindTarget(Sequence) : nullptr; if (!Target) return false;
		const auto* Native = Target->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
		TArray<FString> Values; Values.Append(Native->GetData().GetValues()); TArray<FFrameNumber> Times; Times.Append(Native->GetData().GetTimes());
		TestTrue(TEXT("String full values times and default presence persist exactly"), Values == ExpectedValues && Times == ExpectedTimes && Native->GetDefault() == ExpectedDefault);
		if (Phase == 1 || Phase == 2) { const FString* Evaluated = Native->Evaluate(FFrameTime(1500));
			TestTrue(TEXT("native String evaluation distinguishes empty default and absent default"), Phase == 1 ? Evaluated && Evaluated->IsEmpty() : !Evaluated); }
		TestEqual(TEXT("String section count after template insertion"), Target->GetAllSections().Num(), Phase == 4 ? 2 : 1);
		if (Phase == 4) { const auto* Added = Target->GetAllSections()[1]->GetChannelProxy().GetChannel<FMovieSceneStringChannel>(0);
			TestTrue(TEXT("new native String section contains only explicit new text"), Added && !Added->GetDefault().IsSet() && Added->GetData().GetValues().Num() == 1
				&& Added->GetData().GetValues()[0] == TEXT("模板新文本") && Added->GetData().GetTimes()[0] == FFrameNumber(123)); }
		Owned.Reset(); GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true); int32 Count = 0;
		for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) { ++Count; const auto* Before = NonTarget.Find(Object->GetPathName(Sequence->GetMovieScene()));
			TestTrue(TEXT("all other scalar and owned binding state preserved"), Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object))); }
		Owned.Reset(); TestEqual(TEXT("non-target owned count unchanged"), Count, NonTarget.Num());
		TestTrue(TEXT("String preserves actual component and actor bindings"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		TestFalse(TEXT("String independent readback clean"), Sequence->GetOutermost()->IsDirty()); if (HasAnyErrors()) return false;
	}
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceStringRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.StringLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceStringRecoveryTest::RunTest(const FString&) {
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectPathPublicTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ObjectPathPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
static bool RunObjectPathPublic(FAutomationTestBase& Test, const bool bClass)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	const FString Name = TEXT("LS_Object_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceObject/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/ObjectPath") / Name;
	TArray<FString> References;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		if (!bClass) for (const auto& Path : References) DeleteAssetIfPresent(Path);
		if (!bClass && !References.IsEmpty()) T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, FPackageName::ObjectPathToPackageName(References[0]));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	if (bClass) References = {USceneComponent::StaticClass()->GetPathName(), UPointLightComponent::StaticClass()->GetPathName()};
	else if (!CreateReferenceAssets(Test, Name, References)) return false;
	UClass* AllowedClass = bClass ? USceneComponent::StaticClass() : UDataAsset::StaticClass();
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr; if (!Component) return false;
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional); Sequence->Initialize();
	FTimelineAnimationStructure Initial; FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	AddObjectFixtureTrack(*Sequence->GetMovieScene(), References[0], bClass); AddBoolFixtureTrack(*Sequence->GetMovieScene(), TEXT("OtherBool"));
	FGuid ComponentGuid, ActorGuid;
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true); ActorGuid = Sequencer->GetHandleToObject(Actor, false); }
	FAssetRegistryModule::AssetCreated(Sequence); FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs)) return false;
	FString Error; Sequence = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); if (!Sequence) return false;
	auto FindTarget = [](ULevelSequence* Asset) -> UMovieSceneObjectPropertyTrack* {
		for (auto* Track : Asset->GetMovieScene()->GetTracks()) if (auto* Object = Cast<UMovieSceneObjectPropertyTrack>(Track)) return Object; return nullptr; };
	auto* Target = FindTarget(Sequence); if (!Target) return false;
	TMap<FString, TMap<FString, FString>> NonTarget; TArray<UObject*> Owned; GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
	for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) NonTarget.Add(Object->GetPathName(Sequence->GetMovieScene()), CapturePersistentObject(*Object));
	Owned.Reset();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath); ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	auto Ref = [](const FString& Path) { return UeAgentPropertyValueCodec::EncodeReferencePath(Path); };
	TArray<FString> ExpectedPaths; TArray<FFrameNumber> ExpectedTimes; FString ExpectedDefault;
	for (int32 Phase = 0; Phase < 5; ++Phase)
	{
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportArgs, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { Test.AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		TMap<FString, TSharedPtr<FJsonObject>> Templates, KeyTemplates; FString TargetFile, SectionRef, KeyRef;
		TSharedPtr<FJsonObject> Document, Sections, Channel; bool bReferenceDefinition = false;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			bReferenceDefinition |= Text.Contains(TEXT("value_definition")) && Text.Contains(TEXT("allowed_class")) && Text.Contains(AllowedClass->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Read)};
			while (!Pending.IsEmpty()) { const auto Item = Pending.Pop(EAllowShrinking::No);
				if (Item->Type == EJson::Array) Pending.Append(Item->AsArray());
				else if (Item->Type == EJson::Object) { const auto Object = Item->AsObject(); FString DefinitionName, DefinitionRef;
					if (Object->TryGetStringField(TEXT("name"), DefinitionName) && Object->TryGetStringField(TEXT("ref"), DefinitionRef)) {
						if (DefinitionName == TEXT("LevelSequenceScalarSection")) Templates.Add(DefinitionRef, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template")));
						if (DefinitionName == TEXT("LevelSequenceScalarKey")) KeyTemplates.Add(DefinitionRef, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template"))); }
					for (const auto& Pair : Object->Values) Pending.Add(Pair.Value); } }
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks")); if (!Tracks || !Tracks->HasField(TEXT("Precise"))) continue;
			Document = Read; TargetFile = File; Sections = Tracks->GetObjectField(TEXT("Precise"))->GetObjectField(TEXT("sections")); Channel = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("Value"));
			SectionRef = FindObjectFieldRecursive(Read, TEXT("section_definitions"))->GetStringField(TEXT("Precise")); KeyRef = FindObjectFieldRecursive(Read, TEXT("key_definitions"))->GetObjectField(TEXT("Precise"))->GetStringField(TEXT("Value"));
		}
		if (!Channel || !Templates.Contains(SectionRef) || !KeyTemplates.Contains(KeyRef)) return false;
		const auto Keys = Channel->GetObjectField(TEXT("keys")); const auto KeyTemplate = KeyTemplates[KeyRef];
		if (!Test.TestTrue(TEXT("Object reference template and centralized class constraint have one value owner"), bReferenceDefinition
			&& KeyTemplate->GetStringField(TEXT("value_type")) == (bClass ? TEXT("class_ref") : TEXT("object_ref")) && KeyTemplate->HasField(TEXT("value"))
			&& !KeyTemplate->HasField(TEXT("value_lexeme")) && !KeyTemplate->HasField(TEXT("properties")) && !Channel->HasField(TEXT("properties")))) return false;
		auto NewKey = [&](const TCHAR* Tick, const FString& Path) { const auto Key = MakeShared<FJsonObject>(); const auto Time = MakeShared<FJsonObject>(); Time->SetStringField(TEXT("tick"), Tick); Key->SetObjectField(TEXT("time"), Time); Key->SetStringField(TEXT("value_type"), KeyTemplate->GetStringField(TEXT("value_type"))); Key->SetField(TEXT("value"), Ref(Path)); return Key; };
		if (Phase == 0)
		{
			const auto OriginalValue = Keys->GetObjectField(TEXT("Key1"))->TryGetField(TEXT("value")); const auto OriginalDefault = Channel->GetObjectField(TEXT("default"));
			const auto NativeBefore = CapturePersistentObject(*Target->GetAllSections()[0]); TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
			for (int32 Invalid = 0; Invalid < 4; ++Invalid)
			{
				const auto Key = Keys->GetObjectField(TEXT("Key1"));
				if (Invalid == 0) Key->SetField(TEXT("value"), Ref(TEXT("/Game/UAI/Transient/MissingNewReference.MissingNewReference")));
				if (Invalid == 1) Key->SetField(TEXT("value"), Ref(UObject::StaticClass()->GetPathName()));
				if (Invalid == 2) { const auto BadDefault = MakeShared<FJsonObject>(); BadDefault->SetBoolField(TEXT("is_set"), true); BadDefault->SetField(TEXT("value"), Ref(TEXT(""))); Channel->SetObjectField(TEXT("default"), BadDefault); }
				if (Invalid == 3) Key->SetStringField(TEXT("value_lexeme"), TEXT(""));
				FString InvalidText; if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), InvalidText, Error) || !FFileHelper::SaveStringToFile(InvalidText, *TargetFile)) return false;
				const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
				Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Target = Sequence ? FindTarget(Sequence) : nullptr; TArray<uint8> DiskAfter;
				if (!Test.TestTrue(TEXT("missing wrong-class null-default and dual value owners reject before native writes"), !Rejected.IsOk() && Target && NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Target->GetAllSections()[0])) && FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
				Key->SetField(TEXT("value"), OriginalValue); Key->RemoveField(TEXT("value_lexeme")); Channel->SetObjectField(TEXT("default"), OriginalDefault);
			}
			Keys->GetObjectField(TEXT("Key1"))->SetField(TEXT("value"), Ref(References[1])); Keys->GetObjectField(TEXT("Key1"))->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
			Keys->GetObjectField(TEXT("Key2"))->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("3000"));
			Keys->GetObjectField(TEXT("Key3"))->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("19000"));
			Keys->SetObjectField(TEXT("Added"), NewKey(TEXT("12000"), References[0]));
			ExpectedPaths = {TEXT(""), References[1], References[0], TEXT("/Game/UAI/Transient/AbsentObject.AbsentObject")}; ExpectedTimes = {FFrameNumber(3000), FFrameNumber(6000), FFrameNumber(12000), FFrameNumber(19000)}; ExpectedDefault = References[1];
		}
		else if (Phase == 1) { Keys->Values.Reset(); ExpectedPaths.Reset(); ExpectedTimes.Reset(); ExpectedDefault = References[0]; }
		else if (Phase == 2) ExpectedDefault.Reset();
		else if (Phase == 3) { Keys->SetObjectField(TEXT("Null"), NewKey(TEXT("4500"), TEXT(""))); ExpectedPaths = {FString()}; ExpectedTimes = {FFrameNumber(4500)}; }
		else { FString Text; TSharedPtr<FJsonObject> Added;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Templates[SectionRef].ToSharedRef(), Text, Error) || !FUeAgentStructuredHjsonParser::ParseObject(Text, TEXT("native Object template"), Added, Error)) return false;
			Added->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("Value"))->GetObjectField(TEXT("keys"))->SetObjectField(TEXT("Object"), NewKey(TEXT("123"), References[1])); Sections->SetObjectField(TEXT("AddedSection"), Added); }
		const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), !ExpectedDefault.IsEmpty()); if (!ExpectedDefault.IsEmpty()) Default->SetField(TEXT("value"), Ref(ExpectedDefault)); Channel->SetObjectField(TEXT("default"), Default);
		FString Text; if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Target = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name); if (!Imported.IsOk()) { Test.AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false; Test.TestFalse(TEXT("Object old instance exits before independent reload"), Old.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Target = Sequence ? FindTarget(Sequence) : nullptr; if (!Target) return false;
		const auto* Native = Target->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0); TArray<FString> Paths; for (const auto& Value : Native->GetData().GetValues()) Paths.Add(Value.GetSoftPtr().ToString());
		TArray<FFrameNumber> Times; Times.Append(Native->GetData().GetTimes());
		Test.TestTrue(TEXT("Object paths null keys dangling old value times default and type context persist"), Paths == ExpectedPaths && Times == ExpectedTimes && Native->GetDefault().GetSoftPtr().ToString() == ExpectedDefault && Target->PropertyClass == AllowedClass && Native->GetPropertyClass() == AllowedClass && Target->bClassProperty == bClass);
		if (Phase == 1 || Phase == 2) { UObject* Value = nullptr; const bool bEvaluated = Native->Evaluate(FFrameTime(1500), Value); Test.TestTrue(TEXT("native object default presence evaluates correctly"), Phase == 1 ? bEvaluated && Value && Value->GetPathName() == References[0] : !bEvaluated); }
		Test.TestEqual(TEXT("Object section count"), Target->GetAllSections().Num(), Phase == 4 ? 2 : 1);
		if (Phase == 4) { const auto* Added = Target->GetAllSections()[1]->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0); Test.TestTrue(TEXT("native new Object section preserves constraint and explicit value only"), Added && Added->GetPropertyClass() == AllowedClass && Added->GetDefault().GetSoftPtr().IsNull() && Added->GetData().GetValues().Num() == 1 && Added->GetData().GetValues()[0].GetSoftPtr().ToString() == References[1]); }
		Owned.Reset(); GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true); int32 Count = 0;
		for (auto* Object : Owned) if (Object != Target && !Object->IsIn(Target)) { ++Count; const auto* Before = NonTarget.Find(Object->GetPathName(Sequence->GetMovieScene())); Test.TestTrue(TEXT("all non-target native owned state preserved"), Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object))); }
		Owned.Reset(); Test.TestEqual(TEXT("non-target count preserved"), Count, NonTarget.Num()); Test.TestTrue(TEXT("real component and actor binding preserved"), HasFixtureBinding(Sequence, Preview.GetWorld(), ComponentGuid, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorGuid, Actor));
		Test.TestFalse(TEXT("Object independent readback clean"), Sequence->GetOutermost()->IsDirty()); if (Test.HasAnyErrors()) return false;
	}
	return !Test.HasAnyErrors();
}
bool FUeAgentT85SequenceObjectPathPublicTest::RunTest(const FString&) { return RunObjectPathPublic(*this, false); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectClassPublicTest,
    "UAI.Root.T85.LevelSequence.ScalarChannels.ObjectClassPublic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceObjectClassPublicTest::RunTest(const FString&) { return RunObjectPathPublic(*this, true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectPathRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ObjectPathLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceObjectPathRecoveryTest::RunTest(const FString&) {
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceObjectClassTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ObjectClassConstraint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceObjectClassTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	auto* Track = NewObject<UMovieSceneObjectPropertyTrack>(); Track->PropertyClass = USceneComponent::StaticClass(); Track->bClassProperty = true;
	auto* Section = Track->CreateNewSection(); auto* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
	FTimelineAnimationKey Key; Key.KeyRef = TEXT("Class"); Key.ValueType = TEXT("class_ref"); Key.Time.Tick = TEXT("15");
	Key.Value = UeAgentPropertyValueCodec::EncodeReferencePath(UPointLightComponent::StaticClass()->GetPathName());
	FTimelineAnimationChannel Desired; Desired.DefaultIsSet = true; Desired.DefaultValue = Key.Value;
	ScalarChannel::FChannelEdits Valid;
	if (!TestTrue(TEXT("class picker uses shared IsChildOf constraint before apply"), Valid.PrepareSettings(*Channel, *Section, Desired) && Valid.Prepare(*Channel, *Section, {}, {Key}))) return false;
	Valid.Apply();
	TestTrue(TEXT("class key and default use native reference construction"), Channel->GetData().GetValues()[0].GetSoftPtr().ToString() == UPointLightComponent::StaticClass()->GetPathName() && Channel->GetDefault().GetSoftPtr().ToString() == UPointLightComponent::StaticClass()->GetPathName());
	const auto Before = CapturePersistentObject(*Section);
	Key.Value = UeAgentPropertyValueCodec::EncodeReferencePath(UDataAsset::StaticClass()->GetPathName()); Desired.DefaultValue = Key.Value;
	ScalarChannel::FChannelEdits Invalid;
	TestFalse(TEXT("unrelated class default rejects before mutation"), Invalid.PrepareSettings(*Channel, *Section, Desired));
	FTimelineAnimationKey Original = Key; Original.Time.Tick = TEXT("15");
	TestFalse(TEXT("unrelated class key rejects before mutation"), Invalid.Prepare(*Channel, *Section, {Original}, {Key}));
	TestTrue(TEXT("invalid class selections preserve complete native section"), Before.OrderIndependentCompareEqual(CapturePersistentObject(*Section)));
	auto* DefaultTrack = Cast<UMovieSceneObjectPropertyTrack>(ScalarChannel::MakeSectionTemplateTrack(*Track));
	if (!TestNotNull(TEXT("native class template context copies exactly"), DefaultTrack)) return false;
	auto* DefaultSection = DefaultTrack->CreateNewSection(); auto* DefaultChannel = DefaultSection->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
	TestTrue(TEXT("new class section inherits constraint without cloned key or default"), DefaultTrack->bClassProperty && DefaultTrack->PropertyClass == USceneComponent::StaticClass() && DefaultChannel->GetPropertyClass() == USceneComponent::StaticClass() && DefaultChannel->GetData().GetValues().IsEmpty() && DefaultChannel->GetDefault().GetSoftPtr().IsNull());
	const auto Definition = ScalarChannel::CaptureValueDefinition(*DefaultChannel, DefaultSection);
	TestTrue(TEXT("class reference definition uses common native type descriptor"), Definition->GetObjectField(TEXT("type"))->GetStringField(TEXT("kind")) == TEXT("class_ref"));
	return !HasAnyErrors();
}


bool FUeAgentT85LevelSequenceRootWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85LevelSequenceRoot/LS_") + Token;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/LevelSequenceRoot") / Token;
	const FString Readback = Workspace + TEXT("-readback");
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("owned Level Sequence disk absent after cleanup"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
	};

	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	if (!TestNotNull(TEXT("native initialized Level Sequence MovieScene"), Sequence->GetMovieScene())) return false;
	if (!TestNotNull(TEXT("native non-float track added"),
		Sequence->GetMovieScene()->AddTrack<UMovieSceneAudioTrack>())) return false;
	TestTrue(TEXT("owned Level Sequence starts with empty AssetUserData"),
		Sequence->GetAssetUserDataArray() && Sequence->GetAssetUserDataArray()->IsEmpty());
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("native Level Sequence saved"),
		UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))) return false;
	Package->SetDirtyFlag(false);
	FString Error;
	Sequence = nullptr;
	Package = nullptr;
	if (!TestTrue(TEXT("baseline Level Sequence unload"),
		T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)))
	{
		AddError(Error);
		return false;
	}
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("baseline Level Sequence disk reload"), Sequence)) return false;
	const TMap<FString, FString> MovieSceneBefore = CaptureMovieScene(*Sequence->GetMovieScene());

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{
		AddError(Error);
		return false;
	}
	auto ExportTo = [&](const FString& Path)
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Path);
		Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(
			Registry, Dispatch, TEXT("content_graph.export_workspace"), Args,
			TEXT("t85-level-sequence-root-export"),
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	if (!TestTrue(TEXT("public Level Sequence root export"), ExportTo(Workspace))) return false;
	TArray<FString> ExportedHjsonFiles;
	IFileManager::Get().FindFilesRecursive(
		ExportedHjsonFiles, *Workspace, TEXT("*.hjson"), true, false);
	bool bContainsInternalPersistentState = false;
	for (const FString& ExportedFile : ExportedHjsonFiles)
	{
		FString Text;
		bContainsInternalPersistentState |= FFileHelper::LoadFileToString(Text, *ExportedFile)
			&& (Text.Contains(TEXT("persistent_bytes"))
				|| Text.Contains(TEXT("level_sequence_persistent_owned_objects_v1")));
	}
	TestFalse(TEXT("public workspace omits internal persistent comparison state"),
		bContainsInternalPersistentState);
	if (!TestTrue(TEXT("ordinary AssetUserData starts as an empty editable array"),
		EditOrReadAssetUserData(Workspace, false, false, Error)))
	{
		AddError(Error);
		return false;
	}
	auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), ObjectPath);
	Import->SetObjectField(TEXT("source"), Source);
	const auto NoChange = Tests::PublicCommand::SubmitActiveCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), Import,
		TEXT("t85-level-sequence-root-nochange"));
	if (!TestTrue(TEXT("unedited public workspace retains common no-change rejection"),
		!NoChange.IsOk() && NoChange.GetErrorJson().Contains(TEXT("hjson_import_no_explicit_changes"))))
	{
		AddError(NoChange.GetErrorJson());
		return false;
	}
	if (!TestTrue(TEXT("ordinary AssetUserData edit adds one null slot"),
		EditOrReadAssetUserData(Workspace, true, true, Error)))
	{
		AddError(Error);
		return false;
	}
	Sequence = nullptr;
	Package = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), Import,
		TEXT("t85-level-sequence-root-import"));
	if (!TestTrue(TEXT("public Level Sequence root import"), Imported.IsOk()))
	{
		AddError(Imported.GetErrorJson());
		return false;
	}
	if (!TestTrue(TEXT("independent Level Sequence unload"),
		T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)))
	{
		AddError(Error);
		return false;
	}
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent Level Sequence disk reload"), Sequence)) return false;
	TestTrue(TEXT("one legal null AssetUserData slot persisted"),
		Sequence->GetAssetUserDataArray() && Sequence->GetAssetUserDataArray()->Num() == 1
			&& (*Sequence->GetAssetUserDataArray())[0] == nullptr);
	TestFalse(TEXT("persisted Level Sequence clean"), Sequence->GetOutermost()->IsDirty());
	TestTrue(TEXT("non-float track persists unchanged"),
		Sequence->GetMovieScene()->GetTracks().ContainsByPredicate(
			[](const UMovieSceneTrack* Track)
			{
				return Track && Track->IsA<UMovieSceneAudioTrack>();
			}));
	TestTrue(TEXT("all persistent MovieScene root properties remain unchanged"),
		Sequence->GetMovieScene()
			&& MovieSceneBefore.OrderIndependentCompareEqual(CaptureMovieScene(*Sequence->GetMovieScene())));
	if (!TestTrue(TEXT("independent public Level Sequence reexport"), ExportTo(Readback))
		|| !TestTrue(TEXT("ordinary reexport contains one null AssetUserData slot"),
			EditOrReadAssetUserData(Readback, true, false, Error)))
	{
		AddError(Error);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceChannelSettingsTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.ChannelSettingsPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceChannelSettingsTest::RunTest(const FString&) { return RunNumericChannelsPublic(*this, true, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceChannelSettingsRecoveryTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.ChannelSettingsLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceChannelSettingsRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceFloatChannelSettingsTest,
	"UAI.Root.T85.LevelSequence.NumericChannels.FloatChannelSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceFloatChannelSettingsTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	auto* Section = NewObject<UMovieSceneFloatSection>(); auto& Channel = Section->GetChannel();
	FTimelineAnimationChannel Desired; Desired.DefaultIsSet = true; Desired.DefaultValueLexeme = TEXT("1.23456788");
	ScalarChannel::FChannelEdits Set;
	if (!TestTrue(TEXT("Float default prepares without mutation"), Set.PrepareSettings(Channel, *Section, Desired) && !Channel.GetDefault().IsSet())) return false;
	Set.Apply(); float Value = 0;
	TestTrue(TEXT("Float default uses typed precision and native Evaluate"), Channel.Evaluate(FFrameTime(0), Value) && Value == 1.23456788f);
	Desired.DefaultIsSet = false; Desired.DefaultValueLexeme.Reset(); ScalarChannel::FChannelEdits Clear;
	if (!TestTrue(TEXT("Float clear prepares before mutation"), Clear.PrepareSettings(Channel, *Section, Desired) && Channel.GetDefault().IsSet())) return false;
	Clear.Apply(); TestFalse(TEXT("Float absent default is not zero"), Channel.Evaluate(FFrameTime(0), Value));
	FMovieSceneFloatValue Key(7.75f); Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_Break;
	Key.Tangent.ArriveTangent = 13.5f; Key.Tangent.LeaveTangent = -17.25f;
	const int32 Index = Channel.GetData().AddKey(FFrameNumber(123), Key);
	const FKeyHandle Handle = Channel.GetData().GetHandle(Index);
	Desired.DefaultIsSet = true; Desired.DefaultValueLexeme = TEXT("9.25");
	const auto Fields = MakeShared<FJsonObject>(); const auto Enum = MakeShared<FJsonObject>(); Enum->SetStringField(TEXT("selected"), TEXT("RCCE_Linear"));
	Fields->SetObjectField(TEXT("PreInfinityExtrap"), Enum); const auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("fields"), Fields);
	Desired.Properties = MakeShared<FJsonValueObject>(Properties); ScalarChannel::FChannelEdits Settings;
	if (!Settings.PrepareSettings(Channel, *Section, Desired)) return false;
	Settings.Apply();
	TestTrue(TEXT("settings preserve original native handle time and complete Float key"), Channel.GetData().GetIndex(Handle) == 0
		&& Channel.GetData().GetTimes()[0] == FFrameNumber(123) && Channel.GetData().GetValues()[0] == Key && Channel.PreInfinityExtrap == RCCE_Linear);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceBoolChannelTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.BoolPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
template <int32 Kind>
static bool RunPrimitiveChannelPublic(FAutomationTestBase& Test)
{
	using namespace UE::UAI;
	constexpr bool bInteger = Kind == 1, bByte = Kind >= 2, bEnum = Kind == 3;
	using TrackType = std::conditional_t<bByte, UMovieSceneByteTrack, std::conditional_t<bInteger, UMovieSceneIntegerTrack, UMovieSceneBoolTrack>>;
	using SectionType = std::conditional_t<bByte, UMovieSceneByteSection, std::conditional_t<bInteger, UMovieSceneIntegerSection, UMovieSceneBoolSection>>;
	using ValueType = std::conditional_t<bByte, uint8, std::conditional_t<bInteger, int32, bool>>;
	const FString Name = TEXT("LS_Primitive_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequencePrimitive/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/Primitive") / Name;
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	auto* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	auto* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!Component) return false;
	auto* Package = CreatePackage(*PackageName);
	auto* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional); Sequence->Initialize();
	DomainAdapters::TimelineAnimation::FTimelineAnimationStructure Initial;
	DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
	if (!DomainAdapters::TimelineAnimation::TryParseTimelineAnimationStructure(MakeSequenceStructure(ObjectPath), Initial, NativeError)
		|| !DomainAdapters::TimelineAnimation::TryApplyLevelSequenceStructure(*Sequence, Initial, ObjectPath, NativeError)) return false;
	auto* Track = Sequence->GetMovieScene()->template AddTrack<TrackType>(); Track->SetDisplayName(FText::FromString(TEXT("Toggle")));
	if constexpr (bEnum) Track->SetEnum(ByteFixtureEnum());
	auto* Section = CastChecked<SectionType>(Track->CreateNewSection()); Track->AddSection(*Section); Section->SetRange(TRange<FFrameNumber>::All());
	FixtureChannel(*Section).GetData().AddKey(FFrameNumber(0), static_cast<ValueType>(bInteger ? MIN_int32 : false)); FixtureChannel(*Section).GetData().AddKey(FFrameNumber(9000), static_cast<ValueType>(bByte ? 255 : bInteger ? MAX_int32 : true));
	if constexpr (bEnum) { FixtureChannel(*Section).GetData().AddKey(FFrameNumber(18000), 201); FixtureChannel(*Section).SetDefault(201); }
	if constexpr (bByte) { auto* Other = Sequence->GetMovieScene()->AddTrack<UMovieSceneByteTrack>(); Other->SetDisplayName(FText::FromString(TEXT("OtherByte")));
		if constexpr (!bEnum) Other->SetEnum(ByteFixtureEnum());
		auto* OtherSection = CastChecked<UMovieSceneByteSection>(Other->CreateNewSection()); Other->AddSection(*OtherSection);
		OtherSection->ByteCurve.SetDefault(7); OtherSection->ByteCurve.GetData().AddKey(FFrameNumber(15), 1); }
	if constexpr (bInteger) AddBoolFixtureTrack(*Sequence->GetMovieScene(), TEXT("OtherBool"));
	FGuid Binding, ActorBinding;
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); ON_SCOPE_EXIT { Sequencer->Close(); };
		Binding = Sequencer->GetHandleToObject(Component, true); ActorBinding = Sequencer->GetHandleToObject(Actor, false); }
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Sequence, *Filename, Save)) return false;
	FString Error; Sequence = nullptr; Track = nullptr; Section = nullptr; Package = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	auto FindTrack = [](ULevelSequence* Asset) { for (auto* Item : Asset->GetMovieScene()->GetTracks())
		if (auto* Primitive = Cast<TrackType>(Item)) return Primitive; return static_cast<TrackType*>(nullptr); };
	Track = FindTrack(Sequence); if (!Track) return false;
	TMap<FString, TMap<FString, FString>> NonTarget;
	TArray<UObject*> Owned; GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true);
	for (auto* Object : Owned) if (Object != Track && !Object->IsIn(Track)) NonTarget.Add(Object->GetPathName(Sequence->GetMovieScene()), CapturePersistentObject(*Object));
	Owned.Reset();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	TArray<ValueType> ExpectedValues; TArray<FFrameNumber> ExpectedTimes; TOptional<ValueType> ExpectedDefault;
	for (int32 Phase = 0; Phase < 4; ++Phase)
	{
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportArgs, Name,
			{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { Test.AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		FString TargetFile, SectionRef, KeyRef; TSharedPtr<FJsonObject> Document, Sections, Channel;
		TMap<FString, TSharedPtr<FJsonObject>> Templates;
		TMap<FString, TSharedPtr<FJsonObject>> Contracts; TSharedPtr<FJsonObject> EnumCatalog;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Read;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Read, Error)) return false;
			if (const auto Enums = FindObjectFieldRecursive(Read, TEXT("enums"))) EnumCatalog = Enums;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Read)};
			while (!Pending.IsEmpty()) { const auto Item = Pending.Pop(EAllowShrinking::No);
				if (Item->Type == EJson::Array) Pending.Append(Item->AsArray());
				else if (Item->Type == EJson::Object) { const auto Object = Item->AsObject(); FString Ref, DefinitionName;
					if (Object->TryGetStringField(TEXT("ref"), Ref) && Object->TryGetStringField(TEXT("name"), DefinitionName)
						&& (DefinitionName == TEXT("LevelSequenceScalarSection") || DefinitionName == TEXT("LevelSequenceScalarKey")))
						{ Contracts.Add(Ref, Object->GetObjectField(TEXT("value_definition"))); Templates.Add(Ref, Object->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("template"))); }
					for (const auto& Pair : Object->Values) Pending.Add(Pair.Value); } }
			const auto Tracks = FindObjectFieldRecursive(Read, TEXT("root_tracks")); if (!Tracks || !Tracks->HasField(TEXT("Toggle"))) continue;
			Document = Read; TargetFile = File; Sections = Tracks->GetObjectField(TEXT("Toggle"))->GetObjectField(TEXT("sections"));
			Channel = Sections->GetObjectField(TEXT("Section1"))->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("Value"));
			SectionRef = FindObjectFieldRecursive(Read, TEXT("section_definitions"))->GetStringField(TEXT("Toggle"));
			KeyRef = FindObjectFieldRecursive(Read, TEXT("key_definitions"))->GetObjectField(TEXT("Toggle"))->GetStringField(TEXT("Value"));
		}
		const auto KeyTemplate = Templates.FindRef(KeyRef); const auto SectionTemplate = Templates.FindRef(SectionRef);
		if (!Test.TestTrue(TEXT("primitive templates use actual primitive type without invented key properties"), Channel && KeyTemplate && SectionTemplate
			&& KeyTemplate->GetStringField(TEXT("value_type")) == (bEnum ? TEXT("enum") : bByte || bInteger ? TEXT("integer") : TEXT("boolean")) && KeyTemplate->GetStringField(TEXT("value_lexeme")) == (bEnum ? TEXT("") : bByte || bInteger ? TEXT("0") : TEXT("false"))
			&& !KeyTemplate->HasField(TEXT("properties")))) return false;
		if constexpr (bEnum) { const auto Contract = Contracts.FindRef(KeyRef);
			if (!Test.TestTrue(TEXT("enum definitions publish native candidates and require explicit selection for unnamed zero"), Contract && Contract->GetBoolField(TEXT("value_lexeme_requires_selection"))
				&& Contract->HasField(TEXT("value_lexeme_definition")) && EnumCatalog && EnumCatalog->HasField(TEXT("EHLODLevelExclusion")))) return false;
			const auto Candidates = EnumCatalog->GetArrayField(TEXT("EHLODLevelExclusion")); TSet<FString> Names; for (const auto& Item : Candidates) Names.Add(Item->AsString());
			Test.TestTrue(TEXT("sparse native enum values expose names not ordinal guesses"), Names.Contains(TEXT("HLOD0")) && Names.Contains(TEXT("HLOD2")) && Names.Contains(TEXT("HLOD7"))); }
		const auto Keys = Channel->GetObjectField(TEXT("keys"));
		auto Persist = [&] { FString Text; return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); };
		auto Selected = [](const TCHAR* Value) { const auto Object = MakeShared<FJsonObject>(); Object->SetStringField(TEXT("selected"), Value); return Object; };
		auto Clone = [&](const TSharedPtr<FJsonObject>& Value) { FString Text; TSharedPtr<FJsonObject> Result;
			if (FUeAgentStructuredHjsonFormatter::TryWriteObject(Value.ToSharedRef(), Text, Error)) FUeAgentStructuredHjsonParser::ParseObject(Text, TEXT("public template"), Result, Error); return Result; };
		if (Phase == 0)
		{
			const auto Key = Keys->GetObjectField(TEXT("Key1")); const auto NativeBefore = CapturePersistentObject(*Track->GetAllSections()[0]);
			TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
			if constexpr (bInteger) Test.TestTrue(TEXT("int32 key boundaries export exactly"), Key->GetStringField(TEXT("value_lexeme")) == TEXT("-2147483648")
				&& Keys->GetObjectField(TEXT("Key2"))->GetStringField(TEXT("value_lexeme")) == TEXT("2147483647"));
			const auto OriginalDefault = Channel->GetObjectField(TEXT("default"));
			const TArray<FString> InvalidIntegers{TEXT("1.5"), TEXT("1e2"), TEXT("true"), TEXT("2147483648"), TEXT("-2147483649"), TEXT("999999999999999999999999")};
			for (int32 Invalid = 0; Invalid < (bInteger ? 8 : bByte ? 4 : 3); ++Invalid) {
				Channel->SetObjectField(TEXT("default"), OriginalDefault);
				if constexpr (bByte) {
					Key->SetStringField(TEXT("value_lexeme"), Invalid == 0 ? (bEnum ? TEXT("UnknownName") : TEXT("256")) : (bEnum ? TEXT("HLOD0") : TEXT("0")));
					if (Invalid == 1) Key->SetStringField(TEXT("value_lexeme"), bEnum ? TEXT("7") : TEXT("-1"));
					if (Invalid == 2) { const auto Bad = MakeShared<FJsonObject>(); Bad->SetBoolField(TEXT("is_set"), true); Bad->SetStringField(TEXT("value_lexeme"), bEnum ? TEXT("UnknownName") : TEXT("256")); Channel->SetObjectField(TEXT("default"), Bad); }
					if (Invalid == 3) { if constexpr (bEnum) Keys->SetObjectField(TEXT("MissingSelection"), Clone(KeyTemplate)); else Key->SetStringField(TEXT("value_lexeme"), TEXT("1.5")); }
				} else {

				Key->SetStringField(TEXT("value_lexeme"), bInteger ? (Invalid < 6 ? InvalidIntegers[Invalid] : TEXT("0")) : (Invalid == 0 ? TEXT("1") : TEXT("false")));
				if (Invalid == (bInteger ? 6 : 1)) Key->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
				if (Invalid == (bInteger ? 7 : 2)) { const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), true); Default->SetStringField(TEXT("value_lexeme"), bInteger ? TEXT("2147483648") : TEXT("0")); Channel->SetObjectField(TEXT("default"), Default); }
				}
				if (!Persist()) return false;
				const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
				Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Track = Sequence ? FindTrack(Sequence) : nullptr; TArray<uint8> DiskAfter;
				if (!Test.TestTrue(TEXT("primitive rejects numeric coercion and invented key properties before writes"), !Rejected.IsOk() && Track
					&& NativeBefore.OrderIndependentCompareEqual(CapturePersistentObject(*Track->GetAllSections()[0]))
					&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
				Key->RemoveField(TEXT("properties")); Keys->RemoveField(TEXT("MissingSelection")); }
			Key->SetStringField(TEXT("value_lexeme"), bEnum ? TEXT("HLOD0") : bByte ? TEXT("255") : bInteger ? TEXT("-3") : TEXT("true")); Key->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("6000"));
			Keys->RemoveField(TEXT("Key2")); auto Added = Clone(KeyTemplate); Added->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("3000")); if constexpr (bEnum) Added->SetStringField(TEXT("value_lexeme"), TEXT("HLOD7")); Keys->SetObjectField(TEXT("Added"), Added);
			ExpectedValues = {static_cast<ValueType>(bEnum ? 128 : 0), static_cast<ValueType>(bEnum ? 1 : bByte ? 255 : bInteger ? -3 : true)}; ExpectedTimes = {FFrameNumber(3000), FFrameNumber(6000)}; ExpectedDefault = static_cast<ValueType>(bEnum ? 201 : bByte ? 255 : bInteger ? MAX_int32 : true);
			if constexpr (bEnum) { ExpectedValues.Add(201); ExpectedTimes.Add(FFrameNumber(18000));
				Keys->GetObjectField(TEXT("Key3"))->RemoveField(TEXT("value_lexeme")); }
			const auto Fields = Channel->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"));
			if constexpr (bInteger) { const auto Literal = MakeShared<FJsonObject>(); Literal->SetBoolField(TEXT("literal"), true); Fields->SetObjectField(TEXT("bInterpolateLinearKeys"), Literal); }
			Fields->SetObjectField(TEXT("PreInfinityExtrap"), Selected(TEXT("RCCE_Linear"))); Fields->SetObjectField(TEXT("PostInfinityExtrap"), Selected(TEXT("RCCE_CycleWithOffset")));
		}
		else if (Phase == 1) { Keys->Values.Reset(); ExpectedValues.Reset(); ExpectedTimes.Reset(); ExpectedDefault = static_cast<ValueType>(bEnum ? 1 : bInteger ? MIN_int32 : false);
			if constexpr (bInteger) Channel->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("fields"))->RemoveField(TEXT("bInterpolateLinearKeys")); }
		else if (Phase == 2) ExpectedDefault.Reset();
		else {
			auto AddedKey = Clone(KeyTemplate); AddedKey->SetStringField(TEXT("value_lexeme"), bEnum ? TEXT("HLOD7") : bByte ? TEXT("255") : bInteger ? TEXT("2147483647") : TEXT("true")); AddedKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("4500")); Keys->SetObjectField(TEXT("AfterEmpty"), AddedKey);
			ExpectedValues = {static_cast<ValueType>(bEnum ? 128 : bByte ? 255 : bInteger ? MAX_int32 : true)}; ExpectedTimes = {FFrameNumber(4500)};
			auto AddedSection = Clone(SectionTemplate); Sections->SetObjectField(TEXT("AddedSection"), AddedSection);
		}
		const auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), ExpectedDefault.IsSet());
		if (ExpectedDefault.IsSet()) Default->SetStringField(TEXT("value_lexeme"), bEnum ? (ExpectedDefault.GetValue() == 1 ? FString(TEXT("HLOD0")) : FString()) : bByte || bInteger ? LexToString(static_cast<int32>(ExpectedDefault.GetValue())) : (ExpectedDefault.GetValue() ? FString(TEXT("true")) : FString(TEXT("false")))); Channel->SetObjectField(TEXT("default"), Default);
		if constexpr (bEnum) { if (Phase == 0) Channel->RemoveField(TEXT("default")); }
		if (!Persist()) return false;
		TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Track = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { Test.AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		Test.TestFalse(TEXT("primitive old instance exits before independent disk read"), Old.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Track = Sequence ? FindTrack(Sequence) : nullptr;
		if (!Test.TestTrue(TEXT("primitive native track and section membership persist"), Track && Track->GetAllSections().Num() == (Phase == 3 ? 2 : 1))) return false;
		Section = CastChecked<SectionType>(Track->GetAllSections()[0]); auto& Native = FixtureChannel(*Section);
		if constexpr (bEnum) Test.TestTrue(TEXT("Byte native enum context persists"), Native.GetEnum() == Track->GetEnum() && Native.GetEnum()->GetValueByNameString(TEXT("HLOD0")) == 1 && Native.GetEnum()->GetValueByNameString(TEXT("HLOD7")) == 128);
		TArray<ValueType> Values; Values.Append(Native.GetData().GetValues()); TArray<FFrameNumber> Times; Times.Append(Native.GetData().GetTimes());
		Test.TestTrue(TEXT("primitive full values times default and extrapolation persist"), Values == ExpectedValues && Times == ExpectedTimes && Native.GetDefault() == ExpectedDefault
			&& Native.PreInfinityExtrap == RCCE_Linear && Native.PostInfinityExtrap == RCCE_CycleWithOffset);
		if constexpr (bInteger) Test.TestTrue(TEXT("integer interpolation flag survives all unrelated edits"), Native.bInterpolateLinearKeys);
		ValueType Evaluated = 0;
		if (Phase == 0) {
			Test.TestTrue(TEXT("primitive Linear extrapolation is endpoint value"), Native.Evaluate(FFrameTime(0), Evaluated) && Evaluated == static_cast<ValueType>(bEnum ? 128 : 0));
			if constexpr (!bByte) Test.TestTrue(TEXT("primitive cycle offset follows native Evaluate"), Native.Evaluate(FFrameTime(7500), Evaluated) && Evaluated == static_cast<ValueType>(bInteger ? -4 : false));
			if constexpr (bInteger) Test.TestTrue(TEXT("integer interpolation uses native Lerp plus half then FloorToInt"), Native.bInterpolateLinearKeys
				&& Native.Evaluate(FFrameTime(4500), Evaluated) && Evaluated == -1);
		}
		else if (Phase < 3) Test.TestTrue(TEXT("primitive false default differs from absent default"), Native.Evaluate(FFrameTime(0), Evaluated) == ExpectedDefault.IsSet() && (!ExpectedDefault.IsSet() || Evaluated == ExpectedDefault.GetValue()));
		if (Phase == 3) { auto* Added = CastChecked<SectionType>(Track->GetAllSections()[1]);
			auto* TemplateTrack = NewObject<TrackType>(); if constexpr (bEnum) TemplateTrack->SetEnum(Track->GetEnum()); auto* NativeTemplate = CastChecked<SectionType>(TemplateTrack->CreateNewSection());
			if constexpr (bEnum) Test.TestTrue(TEXT("new Byte section inherits native track enum context"), FixtureChannel(*Added).GetEnum() == Track->GetEnum());
			Test.TestTrue(TEXT("new primitive section uses native default not old keys"), FixtureChannel(*Added).GetData().GetTimes().IsEmpty()
				&& FixtureChannel(*Added).GetDefault() == FixtureChannel(*NativeTemplate).GetDefault()
				&& FixtureChannel(*Added).PreInfinityExtrap == FixtureChannel(*NativeTemplate).PreInfinityExtrap && FixtureChannel(*Added).PostInfinityExtrap == FixtureChannel(*NativeTemplate).PostInfinityExtrap); }
		Owned.Reset(); GetObjectsWithOuter(Sequence->GetMovieScene(), Owned, true); int32 Count = 0;
		for (auto* Object : Owned) if (Object != Track && !Object->IsIn(Track)) { ++Count; const auto* Before = NonTarget.Find(Object->GetPathName(Sequence->GetMovieScene()));
			Test.TestTrue(TEXT("all non-target Float channels and owned fields remain intact"), Before && Before->OrderIndependentCompareEqual(CapturePersistentObject(*Object))); }
		Owned.Reset(); Test.TestEqual(TEXT("all non-target objects remain"), Count, NonTarget.Num());
		Test.TestTrue(TEXT("primitive editing preserves real component actor bindings"), HasFixtureBinding(Sequence, Preview.GetWorld(), Binding, Component) && HasFixtureBinding(Sequence, Preview.GetWorld(), ActorBinding, Actor));
		Test.TestFalse(TEXT("primitive independent readback clean"), Sequence->GetOutermost()->IsDirty()); if (Test.HasAnyErrors()) return false;
	}
	return !Test.HasAnyErrors();
}

bool FUeAgentT85SequenceBoolChannelTest::RunTest(const FString&) { return RunPrimitiveChannelPublic<false>(*this); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceIntegerChannelTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.IntegerPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceIntegerChannelTest::RunTest(const FString&) { return RunPrimitiveChannelPublic<true>(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceBoolRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.BoolLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceBoolRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceIntegerRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.IntegerLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceIntegerRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceByteEnumTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ByteEnumPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceByteEnumTest::RunTest(const FString&) { return RunPrimitiveChannelPublic<3>(*this); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceByteTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.BytePublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceByteTest::RunTest(const FString&) { return RunPrimitiveChannelPublic<2>(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceByteEnumRecoveryTest,
	"UAI.Root.T85.LevelSequence.ScalarChannels.ByteEnumLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT85SequenceByteEnumRecoveryTest::RunTest(const FString&)
{
	return RunLevelSequenceRootRecovery(*this, false, true, false, false, false, false, false, false, false, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSequenceBindingIdentityTest,
	"UAI.Root.T208.LevelSequence.BindingIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSequenceBindingIdentityTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	const FString Name = TEXT("LS_BindingIdentity_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceBindingIdentity/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/SequenceBindingIdentity") / Name;
	const FString CreationWorkspace = Workspace + TEXT("_creation_reject");
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("owned binding fixture package absent on disk"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*CreationWorkspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	const auto Created = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)), Name);
	if (!Created.IsOk()) { AddError(Created.GetErrorJson()); return false; }
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !Sequence->GetMovieScene()) return false;
	UMovieScene* Scene = Sequence->GetMovieScene();
	const FGuid OriginalGuid = Scene->GetBindings()[0].GetObjectGuid();
	TArray<APointLight*> Actors; TArray<UPointLightComponent*> Components; TArray<FGuid> ActorGuids, Guids;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
		auto* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
		if (!Component) return false;
		Actors.Add(Actor); Components.Add(Component);
		{
			const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld());
			Guids.Add(Sequencer->GetHandleToObject(Component, true));
			ActorGuids.Add(Sequencer->GetHandleToObject(Actor, false));
			Sequencer->Close();
		}
		Scene->FindBinding(Guids[Index])->SetName(Index == 2 ? TEXT("Shared2") : TEXT("Shared"));
		auto* Track = Scene->AddTrack<UMovieSceneFloatTrack>(Guids[Index]);
		Track->SetPropertyNameAndPath(TEXT("Intensity"), TEXT("Intensity"));
		Track->SetDisplayName(FText::FromString(FString::Printf(TEXT("Track%d"), Index)));
		auto* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>(0, 24000)); Track->AddSection(*Section);
		Section->GetChannel().SetDefault(1000.5f + Index);
		FMovieSceneFloatValue Value(17.25f + Index); Value.InterpMode = RCIM_Cubic; Value.TangentMode = RCTM_Break;
		Value.Tangent.ArriveTangent = 31.5f + Index; Value.Tangent.LeaveTangent = -42.75f - Index;
		Section->GetChannel().GetData().AddKey(FFrameNumber(9000 + Index), Value);
	}
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); Sequencer->Close(); }
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	Sequence = nullptr; Scene = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!Scene) return false;
	TArray<TMap<FString, FString>> TrackStates, SectionStates;
	for (const FGuid& Guid : Guids) {
		const auto* Binding = Scene->FindBinding(Guid); if (!Binding || Binding->GetTracks().Num() != 1) return false;
		TrackStates.Add(CapturePersistentObject(*Binding->GetTracks()[0]));
		SectionStates.Add(CapturePersistentObject(*Binding->GetTracks()[0]->GetAllSections()[0]));
	}
	const auto RootState = CapturePersistentObject(*Scene->GetTracks()[0]);
	const auto RootSectionState = CapturePersistentObject(*Scene->GetTracks()[0]->GetAllSections()[0]);
	const auto OriginalTrackState = CapturePersistentObject(*Scene->FindBinding(OriginalGuid)->GetTracks()[0]);
	const auto OriginalSectionState = CapturePersistentObject(*Scene->FindBinding(OriginalGuid)->GetTracks()[0]->GetAllSections()[0]);
	const int32 BindingCount = Scene->GetBindings().Num();
	TArray<TWeakObjectPtr<UMovieSceneTrack>> OriginalBindingTracks;
	for (const FGuid& Guid : Guids) OriginalBindingTracks.Add(Scene->FindBinding(Guid)->GetTracks()[0]);
	auto VerifyNative = [&](ULevelSequence* Actual, uint8 RemovedBindingMask) {
		if (!Actual || !Actual->GetMovieScene()) return false;
		auto* CurrentScene = Actual->GetMovieScene();
		bool Same = TestEqual(TEXT("only requested native binding removed"), CurrentScene->GetBindings().Num(),
			BindingCount - FMath::CountBits(RemovedBindingMask));
		for (int32 Index = 0; Index < 3; ++Index) {
			const auto* Binding = CurrentScene->FindBinding(Guids[Index]);
			if ((RemovedBindingMask & (1 << Index)) != 0) {
				Same &= TestNull(TEXT("target binding GUID removed"), Binding);
				Same &= TestTrue(TEXT("removing possessable leaves actor alive"), Actors[Index] && IsValid(Actors[Index]));
				Same &= TestTrue(TEXT("removing possessable leaves component alive"), Components[Index] && IsValid(Components[Index]));
				Same &= TestFalse(TEXT("removed component no longer resolves from sequence"),
					HasFixtureBinding(Actual, Preview.GetWorld(), Guids[Index], Components[Index]));
				continue;
			}
			if (!TestNotNull(TEXT("exact binding GUID retained"), Binding)) return false;
			Same &= TestEqual(TEXT("duplicate binding name retained"), Binding->GetName(), FString(Index == 2 ? TEXT("Shared2") : TEXT("Shared")));
			const int32 ExpectedTracks = 1;
			if (!TestEqual(TEXT("only targeted bound track removed"), Binding->GetTracks().Num(), ExpectedTracks)) return false;
			if (ExpectedTracks) {
				Same &= CheckPersistentObject(*this, TEXT("complete surviving bound track"), TrackStates[Index], *Binding->GetTracks()[0]);
				Same &= CheckPersistentObject(*this, TEXT("complete unique bound channel"), SectionStates[Index], *Binding->GetTracks()[0]->GetAllSections()[0]);
			}
			Same &= TestTrue(TEXT("component resolves to original object"), HasFixtureBinding(Actual, Preview.GetWorld(), Guids[Index], Components[Index]));
			Same &= TestTrue(TEXT("actor resolves to original object"), HasFixtureBinding(Actual, Preview.GetWorld(), ActorGuids[Index], Actors[Index]));
		}
		const auto* Original = CurrentScene->FindBinding(OriginalGuid);
		if (!Original || Original->GetTracks().Num() != 1 || CurrentScene->GetTracks().Num() != 1) return false;
		Same &= CheckPersistentObject(*this, TEXT("unrelated initial bound track"), OriginalTrackState, *Original->GetTracks()[0]);
		Same &= CheckPersistentObject(*this, TEXT("unrelated initial bound section"), OriginalSectionState, *Original->GetTracks()[0]->GetAllSections()[0]);
		Same &= CheckPersistentObject(*this, TEXT("unrelated root track"), RootState, *CurrentScene->GetTracks()[0]);
		Same &= CheckPersistentObject(*this, TEXT("unrelated root section"), RootSectionState, *CurrentScene->GetTracks()[0]->GetAllSections()[0]);
		return Same;
	};
	if (!VerifyNative(Sequence, 0)) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		ExportArgs, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	FString TargetFile, RootTracksFile; TSharedPtr<FJsonObject> TargetDocument, Bindings, RootTracksDocument, RootTracks; FString Text;
	for (const FString& File : Files) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		for (const FGuid& Guid : Guids) TestFalse(TEXT("binding GUID absent from ordinary files"), Text.Contains(Guid.ToString(EGuidFormats::Digits)));
		const auto Found = FindObjectFieldRecursive(Document, TEXT("bindings"));
		if (Found && Found->Values.Num() == BindingCount) { TargetFile = File; TargetDocument = Document; Bindings = Found; }
		const auto FoundRootTracks = FindObjectFieldRecursive(Document, TEXT("root_tracks"));
		if (FoundRootTracks && !FoundRootTracks->Values.IsEmpty()) { RootTracksFile = File; RootTracksDocument = Document; RootTracks = FoundRootTracks; }
	}
	if (!TestTrue(TEXT("ordinary bindings are one keyed collection"), Bindings.IsValid())) return false;
	TArray<FString> SharedAliases;
	for (const auto& Pair : Bindings->Values) {
		const auto Record = Pair.Value->AsObject(); TestFalse(TEXT("ordinary binding omits native identity"), Record->HasField(TEXT("binding_ref")));
		if (Pair.Key.StartsWith(TEXT("Shared"))) SharedAliases.Add(Pair.Key);
	}
	SharedAliases.Sort();
	if (!TestEqual(TEXT("duplicate and literal suffix names receive three readable aliases"), SharedAliases.Num(), 3)) return false;
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	if (!TestTrue(TEXT("ordinary root tracks available for document-stage rejection"), RootTracks.IsValid())) return false;
	TArray<uint8> PreflightDisk;
	if (!FFileHelper::LoadFileToArray(PreflightDisk, *Filename)) return false;
	for (const bool bRootCollection : {false, true})
	{
		const auto Document = bRootCollection ? RootTracksDocument : TargetDocument;
		const auto Records = bRootCollection ? RootTracks : Bindings;
		const FString File = bRootCollection ? RootTracksFile : TargetFile;
		const auto Record = Records->Values.CreateConstIterator().Value()->AsObject();
		for (const TCHAR* Operation : {TEXT("remove"), TEXT("update")})
		{
			Record->SetStringField(TEXT("operation"), Operation);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
				TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-forbidden-operation"));
			TArray<uint8> DiskAfter;
			if (!TestTrue(TEXT("ordinary operation rejects before native writes in both resolver stages"),
				!Rejected.IsOk() && Rejected.GetErrorJson().Contains(TEXT("keyed_item_machine_field_forbidden"))
				&& Sequence == LoadObject<ULevelSequence>(nullptr, *ObjectPath)
				&& !Sequence->GetOutermost()->IsDirty() && VerifyNative(Sequence, 0)
				&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == PreflightDisk))
			{ AddError(Rejected.GetErrorJson()); return false; }
		}
		Record->RemoveField(TEXT("operation"));
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonValue>&)> FindBindingRow;
	FindBindingRow = [&](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonObject> {
		if (!Value) return nullptr;
		if (Value->Type == EJson::Object) {
			const auto Object = Value->AsObject(); const TSharedPtr<FJsonObject>* Collection = nullptr;
			if (Object->TryGetObjectField(TEXT("bindings"), Collection) && *Collection == Bindings) return Object;
			for (const auto& Field : Object->Values) if (auto Found = FindBindingRow(Field.Value)) return Found;
		}
		else if (Value->Type == EJson::Array)
			for (const auto& Item : Value->AsArray()) if (auto Found = FindBindingRow(Item)) return Found;
		return nullptr;
	};
	const auto BindingRow = FindBindingRow(MakeShared<FJsonValueObject>(TargetDocument));
	if (!TestTrue(TEXT("binding ordinary row found without machine inventory"), BindingRow.IsValid())) return false;
	const auto OriginalRowFields = BindingRow->Values;
	const auto WrappedValues = MakeShared<FJsonObject>(); WrappedValues->Values = OriginalRowFields;
	WrappedValues->RemoveField(TEXT("ref"));
	// Supply untouched definitions through the production typed resolver so the
	// adversarial values input reaches the operation boundary, not a shape guard.
	auto WorkspaceResources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FResolvedSemanticWorkspacePtr TypedWorkspace;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!HjsonWorkspace::TryResolveEditedSemanticWorkspace(HjsonWorkspace::FWorkspaceSource::Directory(Workspace),
		HjsonWorkspace::ESemanticWorkspaceMode::State, WorkspaceResources, TypedWorkspace, WorkspaceError))
	{ AddError(WorkspaceError.GetCode()); return false; }
	bool bFoundTypedBindings = false;
	for (const auto& Unit : TypedWorkspace->GetModel()->GetUnits())
	{
		if (!Unit.GetValues().Contains(TEXT("bindings"))) continue;
		if (!TestFalse(TEXT("binding typed unit is unique"), bFoundTypedBindings)) return false;
		bFoundTypedBindings = true;
		for (const auto& Field : Unit.GetValues())
		{
			if (Field.Key == TEXT("bindings")) continue;
			TSharedPtr<FJsonValue> Json;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(Field.Value.Get(), Json, Error)) return false;
			WrappedValues->SetField(Field.Key, Json);
		}
	}
	if (!TestTrue(TEXT("binding typed definitions retained for wrapper probe"), bFoundTypedBindings)) return false;
	BindingRow->Values.Reset();
	if (const auto* Ref = OriginalRowFields.Find(TEXT("ref"))) BindingRow->SetField(TEXT("ref"), *Ref);
	BindingRow->SetObjectField(TEXT("values"), WrappedValues);
	const auto WrappedRecord = Bindings->Values.CreateConstIterator().Value()->AsObject();
	for (const TCHAR* Operation : {TEXT("remove"), TEXT("update")})
	{
		WrappedRecord->SetStringField(TEXT("operation"), Operation);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		const auto Rejected = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
			TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-wrapped-forbidden-operation"));
		TArray<uint8> DiskAfter;
		if (!TestTrue(TEXT("values wrapper cannot claim resolved operation provenance"), !Rejected.IsOk()
			&& Rejected.GetErrorJson().Contains(TEXT("keyed_item_machine_field_forbidden"))
			&& Sequence == LoadObject<ULevelSequence>(nullptr, *ObjectPath) && !Sequence->GetOutermost()->IsDirty()
			&& VerifyNative(Sequence, 0) && FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == PreflightDisk))
		{ AddError(Rejected.GetErrorJson()); return false; }
	}
	WrappedRecord->RemoveField(TEXT("operation"));
	BindingRow->Values = OriginalRowFields;
	Bindings->RemoveField(SharedAliases.Last());
	if (!VerifyNative(Sequence, 0)) return false;
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Scene = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-edit"));
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	TestFalse(TEXT("old sequence unloaded before independent readback"), Old.IsValid(true, true));
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("edited sequence independently reloads"), Sequence)) return false;
	int32 RemovedIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Guids.Num(); ++Index)
		if (!Sequence->GetMovieScene()->FindBinding(Guids[Index]))
			RemovedIndex = RemovedIndex == INDEX_NONE ? Index : -2;
	if (!TestTrue(TEXT("exactly one requested binding identity was removed"), RemovedIndex >= 0)) return false;
	TestFalse(TEXT("removed binding-owned track is destroyed after unload"), OriginalBindingTracks[RemovedIndex].IsValid(true, true));
	const uint8 PersistedRemovalMask = 1 << RemovedIndex;
	if (!VerifyNative(Sequence, PersistedRemovalMask)) return false;
	TestFalse(TEXT("binding deletion persisted clean"), Sequence->GetOutermost()->IsDirty());
	// The same native binding edit owner must restore exact persisted state when
	// rejection occurs after save/unload/reload, using the established transaction.
	TArray<uint8> BeforeBytes; if (!FFileHelper::LoadFileToArray(BeforeBytes, *Filename)) return false;
	Sequence->MarkPackageDirty();
	TestTrue(TEXT("late attempt starts from dirty in-memory baseline"), Sequence->GetOutermost()->IsDirty());
	TSharedPtr<FJsonObject> Desired; DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
	if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, Desired, NativeError)) return false;
	auto NativeBindings = Desired->GetArrayField(TEXT("bindings")); Algo::Reverse(NativeBindings);
	const int32 LateRemovedIndex = RemovedIndex == 0 ? 1 : 0;
	NativeBindings.RemoveAll([&](const TSharedPtr<FJsonValue>& Binding) {
		return Binding->AsObject()->GetStringField(TEXT("binding_ref")) == Guids[LateRemovedIndex].ToString(EGuidFormats::Digits);
	});
	Desired->SetArrayField(TEXT("bindings"), NativeBindings);
	bool LateObserved = false;
	UeAgentReflectionAuthoring::FAssetApplyRequest Request;
	Request.TypeId = TEXT("sequence");
	Request.Operation = UeAgentApplyInputContract::EOperation::Update;
	Request.AssetPath = PackageName;
	Request.bSave = true;
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetObjectField(TEXT("movie_scene"), Desired);
	Request.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure) {
		ULevelSequence* Reloaded = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		LateObserved = Reloaded && !Reloaded->GetOutermost()->IsDirty()
			&& VerifyNative(Reloaded, PersistedRemovalMask | (1 << LateRemovedIndex));
		Failure.Diagnostic.Code = TEXT("t208_binding_identity_late_rejected"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Old = Sequence; Sequence = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult Outcome;
	TestFalse(TEXT("late binding edit rejects"), UeAgentReflectionAuthoring::ApplyAsset(Request, Outcome, Name + TEXT("-late")));
	TestTrue(TEXT("late binding recovery confirmed"), Outcome.bRollbackAttempted && Outcome.bRollbackSucceeded);
	TestEqual(TEXT("late failure remains explicit"), Outcome.Diagnostic.Code, FString(TEXT("t208_binding_identity_late_rejected")));
	TestTrue(TEXT("late readback observed exact targeted binding change"), LateObserved);
	TestFalse(TEXT("late edit unloaded original sequence"), Old.IsValid(true, true));
	TArray<uint8> AfterBytes; TestTrue(TEXT("restored package can be read"), FFileHelper::LoadFileToArray(AfterBytes, *Filename));
	TestTrue(TEXT("late recovery restores exact disk bytes"), BeforeBytes == AfterBytes);
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	TestTrue(TEXT("late recovery restores baseline dirty state and native bindings"), Sequence
		&& Sequence->GetOutermost()->IsDirty() && VerifyNative(Sequence, PersistedRemovalMask));
	// The fixture's dirty marker carries no unsaved content; discard only that
	// marker after observing restoration so the independent disk read can unload.
	if (!Sequence) return false;
	Sequence->GetOutermost()->SetDirtyFlag(false);
	Sequence = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	TestTrue(TEXT("late recovery restores pre-attempt binding state"), VerifyNative(Sequence, PersistedRemovalMask));
	// Use a fresh export so the failed create attempt cannot contaminate the
	// successful deletion workspace with derived operation records.
	ExportArgs->SetStringField(TEXT("workspace_path"), CreationWorkspace);
	const auto CreationExport = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
		TEXT("content_graph.export_workspace"), ExportArgs, Name + TEXT("-creation-export"),
		{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!CreationExport.IsOk()) { AddError(CreationExport.GetErrorJson()); return false; }
	Files.Reset(); IFileManager::Get().FindFilesRecursive(Files, *CreationWorkspace, TEXT("*.hjson"), true, false);
	TSharedPtr<FJsonObject> CreationDocument, CreationBindings; FString CreationFile;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		const auto Found = FindObjectFieldRecursive(Document, TEXT("bindings"));
		if (Found && Found->Values.Num() == BindingCount - 1)
		{ CreationFile = File; CreationDocument = Document; CreationBindings = Found; }
	}
	if (!CreationBindings || CreationBindings->Values.IsEmpty()) return false;
	CreationBindings->SetField(TEXT("NewBinding"), CreationBindings->Values.CreateConstIterator().Value());
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(CreationDocument.ToSharedRef(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *CreationFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Source->SetStringField(TEXT("path"), CreationWorkspace);
	const auto AddedBinding = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-reject-binding-creation"));
	TestFalse(TEXT("binding creation remains unsupported"), AddedBinding.IsOk());
	TestTrue(TEXT("binding creation has explicit common rejection"),
		AddedBinding.GetErrorJson().Contains(TEXT("keyed_item_create_unsupported")));
	TestTrue(TEXT("creation rejection preserves persisted binding state"), VerifyNative(Sequence, PersistedRemovalMask));
	return !HasAnyErrors();
}

static bool RunSequenceBindingDependencies(FAutomationTestBase& Test, bool bMetadata, bool bTags = false, bool bFolders = false)
{
	using namespace UE::UAI;
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	const FString Name = (bFolders ? TEXT("LS_Folders_") : bTags ? TEXT("LS_BindingTags_") : bMetadata ? TEXT("LS_BindingMetadata_") : TEXT("LS_BindingDependencies_")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString(bFolders ? TEXT("/Game/UAI/Transient/T208SequenceFolders/") : bTags ? TEXT("/Game/UAI/Transient/T208SequenceBindingTags/") : bMetadata ? TEXT("/Game/UAI/Transient/T208SequenceBindingMetadata/") : TEXT("/Game/UAI/Transient/T208SequenceBindingDependencies/")) + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/SequenceBindingDependencies") / Name;
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		Test.TestFalse(TEXT("joint binding fixture disk absent"), IFileManager::Get().FileExists(*Filename));
		FString CleanupError;
		if (!TestSupport::TryRemoveEmptyFixturePackageDirectory(TEXT("/Game/UAI/Transient"),
			FPackageName::GetLongPackagePath(PackageName), CleanupError)) Test.AddError(CleanupError);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	const auto Created = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		MakeSequenceCreateImport(ObjectPath, MakeSequenceStructure(ObjectPath)), Name);
	if (!Created.IsOk()) { Test.AddError(Created.GetErrorJson()); return false; }
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence || !Sequence->GetMovieScene()) return false;
	UMovieScene* Scene = Sequence->GetMovieScene();
	const FGuid OriginalGuid = Scene->GetBindings()[0].GetObjectGuid();
	TArray<APointLight*> Actors; TArray<UPointLightComponent*> Components;
	TArray<FGuid> ParentGuids, ChildGuids;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
		auto* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
		if (!Component) return false;
		Actors.Add(Actor); Components.Add(Component);
		const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld());
		ChildGuids.Add(Sequencer->GetHandleToObject(Component, true));
		ParentGuids.Add(Sequencer->GetHandleToObject(Actor, false));
		Sequencer->Close();
		if (!Test.TestTrue(TEXT("native component has actor possessable parent"),
			Scene->FindPossessable(ChildGuids[Index])->GetParent() == ParentGuids[Index])) return false;
		Scene->FindBinding(ParentGuids[Index])->SetName(FString::Printf(TEXT("Parent%d"), Index));
		Scene->FindBinding(ChildGuids[Index])->SetName(FString::Printf(TEXT("Child%d"), Index));
		for (const bool bParent : {false, true})
		{
			auto* Track = Scene->AddTrack<UMovieSceneFloatTrack>(bParent ? ParentGuids[Index] : ChildGuids[Index]);
			const FName Property = bParent ? TEXT("InitialLifeSpan") : TEXT("Intensity");
			Track->SetPropertyNameAndPath(Property, Property.ToString());
			auto* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>(0, 24000)); Track->AddSection(*Section);
			Section->GetChannel().SetDefault(123.5f + Index);
			Section->GetChannel().GetData().AddKey(FFrameNumber(9000), FMovieSceneFloatValue(21.5f + Index));
		}
	}
	// Settle tracks before adding the custom reference: no playback touches that reference.
	{ const auto Sequencer = OpenFixtureSequencer(Sequence, Preview.GetWorld()); Sequencer->Close(); }
	// A native custom spawnable reference without resolving or spawning any object.
	const FGuid CustomGuid = Scene->AddPossessable(TEXT("Custom"), AActor::StaticClass());
	static_cast<UMovieSceneSequence*>(Sequence)->GetBindingReferences()->AddOrReplaceBinding(CustomGuid,
		NewObject<UMovieSceneSpawnableActorBinding>(Scene), 0);
	const FMovieSceneSequenceID ForeignSequence(91);
	if (bFolders)
	{
		auto* Track = Scene->AddTrack<UMovieSceneFloatTrack>();
		Track->SetDisplayName(FText::FromString(TEXT("CascadeRoot")));
		Track->SetPropertyNameAndPath(TEXT("CascadeValue"), TEXT("CascadeValue"));
		auto* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>(0, 24000)); Section->GetChannel().SetDefault(67.25f); Track->AddSection(*Section);
	}
	if (bMetadata)
	{
		auto* RootFolder = NewObject<UMovieSceneFolder>(Scene, TEXT("MetadataRoot"));
		auto* NestedFolder = NewObject<UMovieSceneFolder>(Scene, TEXT("MetadataNested"));
		auto* EmptyFolder = NewObject<UMovieSceneFolder>(Scene, TEXT("MetadataEmpty"));
		RootFolder->SetFolderName(TEXT("Root")); NestedFolder->SetFolderName(TEXT("Nested")); EmptyFolder->SetFolderName(TEXT("Empty"));
		Scene->AddRootFolder(RootFolder); RootFolder->AddChildFolder(NestedFolder); RootFolder->AddChildFolder(EmptyFolder);
		RootFolder->AddChildTrack(Scene->GetTracks()[0]); RootFolder->AddChildObjectBinding(OriginalGuid);
		for (int32 Index = 0; Index < 2; ++Index)
			for (const FGuid& Guid : {ParentGuids[Index], ChildGuids[Index]})
			{
				NestedFolder->AddChildObjectBinding(Guid);
				Scene->TagBinding(TEXT("Shared"), UE::MovieScene::FFixedObjectBindingID(Guid, MovieSceneSequenceID::Root));
				Scene->TagBinding(TEXT("Shared"), UE::MovieScene::FFixedObjectBindingID(Guid, ForeignSequence));
			}
		Scene->TagBinding(TEXT("Shared"), UE::MovieScene::FFixedObjectBindingID(OriginalGuid, MovieSceneSequenceID::Root));
		Scene->TagBinding(TEXT("OnlyDeleted"), UE::MovieScene::FFixedObjectBindingID(ChildGuids[0], MovieSceneSequenceID::Root));
	}
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	Sequence = nullptr; Scene = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!Scene) return false;
	const int32 BindingCount = Scene->GetBindings().Num();
	TMap<FString, TMap<FString, FString>> BaselineOwned;
	TArray<UObject*> Owned; GetObjectsWithOuter(Scene, Owned, true);
	for (UObject* Object : Owned) BaselineOwned.Add(Object->GetPathName(Scene), CapturePersistentObject(*Object));
	Owned.Reset();
	TArray<TWeakObjectPtr<UMovieSceneTrack>> DeletedTracks;
	for (int32 Index = 0; Index < 2; ++Index)
		for (const FGuid& Guid : {ParentGuids[Index], ChildGuids[Index]}) DeletedTracks.Add(Scene->FindBinding(Guid)->GetTracks()[0]);
	const FString OriginalRootName = Scene->GetTracks()[0]->GetDisplayName().ToString();
	const TWeakObjectPtr<UMovieSceneTrack> CascadeRootTrack = bFolders ? Scene->GetTracks()[1] : nullptr;
	TMap<FString, TMap<FString, FString>> BaselineFolderProperties;
	if (bMetadata)
	{
		TArray<UMovieSceneFolder*> Folders; GetMovieSceneFoldersRecursive(Scene->GetRootFolders(), Folders);
		for (UMovieSceneFolder* Folder : Folders)
		{
			auto Properties = CapturePersistentObject(*Folder); Properties.Remove(TEXT("ChildObjectBindingStrings"));
			BaselineFolderProperties.Add(Folder->GetName(), MoveTemp(Properties));
		}
	}
	auto HasOrdinaryBinding = [](ULevelSequence* Actual, const FGuid& Guid, UObject* Context, UObject* Expected) {
		TArray<UObject*, TInlineAllocator<1>> Objects;
		static_cast<const UMovieSceneSequence*>(Actual)->LocateBoundObjects(Guid,
			UE::UniversalObjectLocator::FResolveParams(Context), nullptr, Objects);
		return Objects.Num() == 1 && Objects[0] == Expected;
	};
	if (bFolders)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Documents;
		TSharedPtr<FJsonObject> Folders, PublicBindings, RootTracks;
		auto Export = [&]() {
			auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
			Args->SetStringField(TEXT("workspace_path"), Workspace); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
			const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"),
				Args, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
			if (!Result.IsOk()) { Test.AddError(Result.GetErrorJson()); return false; }
			Documents.Reset(); Folders.Reset(); PublicBindings.Reset(); RootTracks.Reset();
			TArray<FString> Paths; IFileManager::Get().FindFilesRecursive(Paths, *Workspace, TEXT("*.hjson"), true, false);
			for (const FString& Path : Paths)
			{
				if (Path.Contains(TEXT("/.uai/")) || Path.Contains(TEXT("\\.uai\\"))) continue;
				FString Text; TSharedPtr<FJsonObject> Document;
				if (!FFileHelper::LoadFileToString(Text, *Path) || !FUeAgentStructuredHjsonParser::ParseObject(Text, Path, Document, Error)) return false;
				if (!Test.TestTrue(TEXT("ordinary folders hide machine identities and operations"), !Text.Contains(TEXT("folder_ref"))
					&& !Text.Contains(TEXT("persistent_bytes")) && !Text.Contains(TEXT("operation:")) && !Text.Contains(ParentGuids[0].ToString(EGuidFormats::Digits)))) return false;
				Documents.Add(Path, Document);
				if (auto Field = FindObjectFieldRecursive(Document, TEXT("folders"))) Folders = Field;
				if (auto Field = FindObjectFieldRecursive(Document, TEXT("bindings"))) PublicBindings = Field;
				if (auto Field = FindObjectFieldRecursive(Document, TEXT("root_tracks"))) RootTracks = Field;
			}
			return Test.TestTrue(TEXT("public readable folder and target collections exported"), Folders && PublicBindings && RootTracks);
		};
		auto Import = [&]() {
			for (const auto& Pair : Documents)
			{
				FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
				if (!FJsonSerializer::Serialize(Pair.Value.ToSharedRef(), Writer) || !FFileHelper::SaveStringToFile(Text, *Pair.Key)) return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("invalid"), MakeShared<FJsonObject>(), Name);
			}
			auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
			auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
			return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, Name);
		};
		auto Reload = [&]() {
			Sequence = nullptr; Scene = nullptr;
			if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
			Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
			return Scene && !Sequence->GetOutermost()->IsDirty();
		};
		auto FindFolder = [&](const TCHAR* NativeName) -> UMovieSceneFolder* {
			TArray<UMovieSceneFolder*> All; GetMovieSceneFoldersRecursive(Scene->GetRootFolders(), All);
			for (auto* Folder : All) if (Folder->GetName() == NativeName) return Folder;
			return nullptr;
		};
		auto Snapshot = [&]() {
			TMap<FString, TMap<FString, FString>> State;
			TArray<UObject*> Objects; GetObjectsWithOuter(Scene, Objects, true);
			Objects.Add(Scene); Objects.Add(Sequence);
			for (auto* Object : Objects) State.Add(Object->GetPathName(Sequence), CapturePersistentObject(*Object));
			return State;
		};
		FString DirtyRootBusinessText;
		auto SameSnapshot = [&](const auto& Expected, bool bCompareDirtyRootDisplayText = false) {
			const auto Actual = Snapshot();
			if (Actual.Num() != Expected.Num()) { Test.AddError(FString::Printf(TEXT("folder snapshot object count: actual=%d expected=%d"), Actual.Num(), Expected.Num())); return false; }
			for (const auto& Pair : Expected)
			{
				const auto* Properties = Actual.Find(Pair.Key);
				if (!Properties) { Test.AddError(TEXT("folder snapshot missing object: ") + Pair.Key); return false; }
				auto ActualProperties = *Properties;
				auto ExpectedProperties = Pair.Value;
				if (bCompareDirtyRootDisplayText && Pair.Key == Scene->GetTracks()[0]->GetPathName(Sequence))
				{
					// Only this explicitly dirty FromString value can acquire a native
					// localization identity during save. All other fields stay exact.
					if (DirtyRootBusinessText.IsEmpty() || Scene->GetTracks()[0]->GetDisplayName().ToString() != DirtyRootBusinessText) return false;
					ActualProperties.Remove(TEXT("DisplayName")); ExpectedProperties.Remove(TEXT("DisplayName"));
				}
				if (!ActualProperties.OrderIndependentCompareEqual(ExpectedProperties))
				{
					FString ChangedFields;
					if (Properties) for (const auto& Field : Pair.Value)
						if (!Properties->Contains(Field.Key) || Properties->FindChecked(Field.Key) != Field.Value)
						{
							ChangedFields += Field.Key + TEXT(" ");
							if (Field.Key == TEXT("DisplayName")) Test.AddInfo(TEXT("display text serialization form before=") + Field.Value.Left(10)
								+ TEXT(" after=") + Properties->FindRef(Field.Key).Left(10));
						}
					Test.AddError(TEXT("folder snapshot mismatch: ") + Pair.Key + TEXT(" fields=") + ChangedFields); return false;
				}
			}
			return true;
		};
		auto PreservedObjects = [&](uint8 RemovedPairs) {
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const bool Removed = (RemovedPairs & (1 << Index)) != 0;
				if (!IsValid(Actors[Index]) || !IsValid(Components[Index])
					|| HasOrdinaryBinding(Sequence, ParentGuids[Index], Preview.GetWorld(), Actors[Index]) == Removed
					|| HasOrdinaryBinding(Sequence, ChildGuids[Index], Actors[Index], Components[Index]) == Removed) return false;
				for (const auto& Guid : {ParentGuids[Index], ChildGuids[Index]})
				{
					const auto* Binding = Scene->FindBinding(Guid);
					if (Removed) { if (Binding) return false; continue; }
					if (!Binding || Binding->GetTracks().Num() != 1) return false;
					for (auto* Object : {static_cast<UObject*>(Binding->GetTracks()[0]), static_cast<UObject*>(Binding->GetTracks()[0]->GetAllSections()[0])})
						if (!CheckPersistentObject(Test, TEXT("surviving complete track properties"), BaselineOwned.FindChecked(Object->GetPathName(Scene)), *Object)) return false;
				}
			}
			if (!Scene->FindBinding(OriginalGuid) || !Scene->FindBinding(CustomGuid) || Scene->GetTracks().Num() != ((RemovedPairs & 1) ? 1 : 2)) return false;
			for (auto* Track : Scene->GetTracks())
				for (auto* Section : Track->GetAllSections())
					if (!CheckPersistentObject(Test, TEXT("root section full non-target data preserved"), BaselineOwned.FindChecked(Section->GetPathName(Scene)), *Section)) return false;
			for (auto* Track : Scene->FindBinding(OriginalGuid)->GetTracks())
			{
				if (!CheckPersistentObject(Test, TEXT("unrelated original binding track preserved"), BaselineOwned.FindChecked(Track->GetPathName(Scene)), *Track)) return false;
				for (auto* Section : Track->GetAllSections())
					if (!CheckPersistentObject(Test, TEXT("unrelated original binding section preserved"), BaselineOwned.FindChecked(Section->GetPathName(Scene)), *Section)) return false;
			}
			TArray<UMovieSceneFolder*> NativeFolders; GetMovieSceneFoldersRecursive(Scene->GetRootFolders(), NativeFolders);
			for (auto* Folder : NativeFolders)
				if (const auto* Before = BaselineFolderProperties.Find(Folder->GetName()))
				{
					auto Expected = *Before; auto Actual = CapturePersistentObject(*Folder);
					for (const TCHAR* Field : {TEXT("FolderName"), TEXT("ChildFolders"), TEXT("ChildTracks"), TEXT("ChildObjectBindingStrings")})
					{ Expected.Remove(Field); Actual.Remove(Field); }
					if (!Expected.OrderIndependentCompareEqual(Actual)) return false;
				}
			const auto* SharedTag = Scene->AllTaggedBindings().Find(TEXT("Shared"));
			if (!SharedTag || SharedTag->IDs.Num() != 9 - 2 * FMath::CountBits(RemovedPairs)
				|| Scene->AllTaggedBindings().Num() != ((RemovedPairs & 1) ? 1 : 2)) return false;
			if (!SharedTag->IDs.Contains(UE::MovieScene::FFixedObjectBindingID(OriginalGuid, MovieSceneSequenceID::Root))) return false;
			for (int32 Index = 0; Index < 2; ++Index) for (const auto& Guid : {ParentGuids[Index], ChildGuids[Index]})
				if (!SharedTag->IDs.Contains(UE::MovieScene::FFixedObjectBindingID(Guid, ForeignSequence))
					|| SharedTag->IDs.Contains(UE::MovieScene::FFixedObjectBindingID(Guid, MovieSceneSequenceID::Root)) == ((RemovedPairs & (1 << Index)) != 0)) return false;
			return true;
		};
		auto SetMembers = [](const TSharedPtr<FJsonObject>& Row, const TCHAR* Field, TArray<FString> Keys) {
			TArray<TSharedPtr<FJsonValue>> Values; for (const auto& Key : Keys) Values.Add(MakeShared<FJsonValueString>(Key)); Row->SetArrayField(Field, Values);
		};
		if (!Export()) return false;
		const auto Baseline = Snapshot(); TArray<uint8> DiskBefore; FFileHelper::LoadFileToArray(DiskBefore, *Filename);
		// Observe native qualification directly before any public checkpoint recovery.
		// The public negatives below exercise the same owner through workspace import.
		{
			TSharedPtr<FJsonObject> NativeDocument;
			DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
			DomainAdapters::TimelineAnimation::FTimelineAnimationStructure Invalid;
			if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, NativeDocument, NativeError)
				|| !DomainAdapters::TimelineAnimation::TryParseTimelineAnimationStructure(NativeDocument, Invalid, NativeError)) return false;
			Invalid.Folders->FindByPredicate([](const auto& Row) { return Row.Name == TEXT("Root"); })->Parent =
				Invalid.Folders->FindByPredicate([](const auto& Row) { return Row.Name == TEXT("Nested"); })->Ref;
			const auto* OriginalSequence = Sequence;
			if (!Test.TestFalse(TEXT("native folder qualification rejects before checkpoint or writes"), DomainAdapters::TimelineAnimation::TryUpdateLevelSequenceValues(*Sequence, Invalid, ObjectPath, NativeError))
				|| !Test.TestTrue(TEXT("direct rejected native call retains instances complete values and clean disk"), Sequence == OriginalSequence
					&& NativeError.Code == TEXT("timeline_animation_folder_plan_invalid") && SameSnapshot(Baseline)
					&& PreservedObjects(0) && !Sequence->GetOutermost()->IsDirty() && DeletedTracks[0].IsValid() && DeletedTracks[1].IsValid())) return false;
		}
		for (int32 Negative = 0; Negative < 4; ++Negative)
		{
			if (Negative == 0) Folders->GetObjectField(TEXT("Root"))->SetStringField(TEXT("parent"), TEXT("Nested"));
			if (Negative == 1) Folders->GetObjectField(TEXT("Nested"))->SetStringField(TEXT("parent"), TEXT("Missing"));
			if (Negative == 2) { Folders->RemoveField(TEXT("Root")); Folders->GetObjectField(TEXT("Nested"))->SetStringField(TEXT("name"), TEXT("ConflictingEdit")); }
			if (Negative == 3) { auto Added = MakeShared<FJsonObject>(); Added->SetStringField(TEXT("name"), TEXT("New")); Added->SetStringField(TEXT("parent"), TEXT("Missing")); Folders->SetObjectField(TEXT("New"), Added); }
			const auto Result = Import(); Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence->GetMovieScene();
			TArray<uint8> DiskAfter;
			if (!Test.TestTrue(TEXT("invalid folder reference cycle or edited cascade rejected before native writes"), !Result.IsOk()
				&& (Result.GetErrorJson().Contains(TEXT("folder_plan_invalid")) || Result.GetErrorJson().Contains(TEXT("keyed_reference_"))))
				|| !Test.TestTrue(TEXT("negative restores complete business state and exact disk"), SameSnapshot(Baseline)
					&& !Sequence->GetOutermost()->IsDirty() && PreservedObjects(0) && FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
			if (!Export()) return false;
		}
		TArray<FString> TrackKeys; RootTracks->Values.GetKeys(TrackKeys);
		if (!TrackKeys.Remove(TEXT("CascadeRoot")) || TrackKeys.Num() != 1) return false;
		auto Parent = MakeShared<FJsonObject>(); Parent->SetStringField(TEXT("name"), TEXT("CreatedParent")); Folders->SetObjectField(TEXT("CreatedParent"), Parent);
		auto Child = MakeShared<FJsonObject>(); Child->SetStringField(TEXT("name"), TEXT("CreatedChild")); Child->SetStringField(TEXT("parent"), TEXT("CreatedParent"));
		SetMembers(Child, TEXT("bindings"), {TEXT("Parent0"), TEXT("Child0")}); SetMembers(Child, TEXT("root_tracks"), TrackKeys); Folders->SetObjectField(TEXT("CreatedChild"), Child);
		auto TrackFolder = MakeShared<FJsonObject>(); TrackFolder->SetStringField(TEXT("name"), TEXT("CreatedTrackFolder")); TrackFolder->SetStringField(TEXT("parent"), TEXT("CreatedParent"));
		SetMembers(TrackFolder, TEXT("root_tracks"), {TEXT("CascadeRoot")}); Folders->SetObjectField(TEXT("CreatedTrackFolder"), TrackFolder);
		Folders->GetObjectField(TEXT("Empty"))->SetStringField(TEXT("name"), TEXT("Moved")); Folders->GetObjectField(TEXT("Empty"))->SetStringField(TEXT("parent"), TEXT("CreatedParent"));
		const auto CreatedFolders = Import(); if (!CreatedFolders.IsOk()) { Test.AddError(CreatedFolders.GetErrorJson()); return false; }
		if (!Reload() || !Export()) return false;
		auto* Moved = FindFolder(TEXT("MetadataEmpty")); auto* Nested = FindFolder(TEXT("MetadataNested"));
		if (!Test.TestTrue(TEXT("native identity retained through rename and move; member ownership transferred"), Moved && Moved->GetFolderName() == TEXT("Moved")
			&& Nested && Nested->GetChildObjectBindings().Num() == 2 && PreservedObjects(0)
			&& Folders->GetObjectField(TEXT("Moved"))->GetStringField(TEXT("parent")) == TEXT("CreatedParent")
			&& Folders->GetObjectField(TEXT("CreatedChild"))->GetArrayField(TEXT("bindings")).Num() == 2)) return false;
		// Move the root track back with native unique ownership; explicit empty clears.
		SetMembers(Folders->GetObjectField(TEXT("Root")), TEXT("root_tracks"), TrackKeys);
		SetMembers(Folders->GetObjectField(TEXT("CreatedChild")), TEXT("root_tracks"), {});
		const auto Members = Import(); if (!Members.IsOk()) { Test.AddError(Members.GetErrorJson()); return false; }
		if (!Reload() || !Export()) return false;
		Folders->RemoveField(TEXT("CreatedParent")); // Captured child rows deliberately remain unchanged.
		const auto Deleted = Import(); if (!Deleted.IsOk()) { Test.AddError(Deleted.GetErrorJson()); return false; }
		if (!Reload() || !Export()) return false;
		if (!Test.TestTrue(TEXT("only removed parent key recursively deletes folders and real binding contents"), Folders->Values.Num() == 2
			&& !FindFolder(TEXT("MetadataEmpty")) && PreservedObjects(1) && !DeletedTracks[0].IsValid(true, true) && !DeletedTracks[1].IsValid(true, true)
			&& !CascadeRootTrack.IsValid(true, true) && !RootTracks->HasField(TEXT("CascadeRoot")))) return false;
		TArray<uint8> DiskAfter;
		FFileHelper::LoadFileToArray(DiskBefore, *Filename);
		CastChecked<UMovieSceneFloatTrack>(Scene->GetTracks()[0])->SetDisplayName(FText::FromString(TEXT("UnsavedRootName"))); Sequence->MarkPackageDirty();
		DirtyRootBusinessText = Scene->GetTracks()[0]->GetDisplayName().ToString();
		if (!Test.TestTrue(TEXT("folder late baseline has actual unsaved business difference"), Sequence->GetOutermost()->IsDirty() && DirtyRootBusinessText != OriginalRootName)) return false;
		const auto DirtyBaseline = Snapshot();
		TSharedPtr<FJsonObject> Desired; DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
		if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, Desired, NativeError)) return false;
		auto Rows = Desired->GetArrayField(TEXT("folders")); Rows.RemoveAll([](const auto& Row) { return Row->AsObject()->GetStringField(TEXT("name")) == TEXT("Nested"); }); Desired->SetArrayField(TEXT("folders"), Rows);
		bool Observed = false, Called = false; TWeakObjectPtr<ULevelSequence> Old = Sequence;
		UeAgentReflectionAuthoring::FAssetApplyRequest Request; Request.TypeId = TEXT("sequence"); Request.AssetPath = PackageName;
		Request.bSave = true; Request.bCompactValues = true; Request.ProviderExtension = MakeShared<FJsonObject>(); Request.ProviderExtension->SetObjectField(TEXT("movie_scene"), Desired);
		Request.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure) {
			Called = true;
			Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence->GetMovieScene(); TArray<uint8> Written;
			Observed = Test.TestTrue(TEXT("late folder apply unloaded original asset"), !Old.IsValid(true, true))
				& Test.TestFalse(TEXT("late folder persisted asset is clean"), Sequence->GetOutermost()->IsDirty())
				& Test.TestTrue(TEXT("late folder recursive contents removed with real objects retained"), !FindFolder(TEXT("MetadataNested")) && PreservedObjects(3))
				& Test.TestEqual(TEXT("late persisted real formerly dirty business value"), Scene->GetTracks()[0]->GetDisplayName().ToString(), FString(TEXT("UnsavedRootName")))
				& Test.TestTrue(TEXT("late folder disk actually changed"), FFileHelper::LoadFileToArray(Written, *Filename) && Written != DiskBefore);
			Failure.Diagnostic.Code = TEXT("t208_folder_late_rejected"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
		};
		UeAgentReflectionAuthoring::FAssetApplyResult Outcome;
		Test.TestFalse(TEXT("folder late verification rejects"), UeAgentReflectionAuthoring::ApplyAsset(Request, Outcome, Name));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); Scene = Sequence->GetMovieScene();
		Test.TestTrue(TEXT("late callback reached after successful native qualification and persistence"), Called);
		Test.TestEqual(TEXT("late rejection is injected outcome"), Outcome.Diagnostic.Code, FString(TEXT("t208_folder_late_rejected")));
		Test.TestTrue(TEXT("folder rollback succeeds"), Outcome.bRollbackSucceeded);
		Test.TestEqual(TEXT("folder rollback restored actual unsaved root business text"), Scene->GetTracks()[0]->GetDisplayName().ToString(), FString(TEXT("UnsavedRootName")));
		Test.TestTrue(TEXT("folder rollback restores actual dirty business memory and complete metadata"), Sequence->GetOutermost()->IsDirty() && SameSnapshot(DirtyBaseline, true) && PreservedObjects(1));
		Test.TestTrue(TEXT("folder rollback independently restores original disk bytes"), FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		if (!Observed || Test.HasAnyErrors()) return false;
		CastChecked<UMovieSceneFloatTrack>(Scene->GetTracks()[0])->SetDisplayName(FText::FromString(OriginalRootName)); Sequence->GetOutermost()->SetDirtyFlag(false);
		return Reload() && Test.TestTrue(TEXT("original independent disk business value recovered"), Scene->GetTracks()[0]->GetDisplayName().ToString() == OriginalRootName && PreservedObjects(1)) && !Test.HasAnyErrors();
	}
	bool bTagsEdited = false;
	auto Verify = [&](ULevelSequence* Actual, uint8 RemovedPairs, bool bDirtyValue, bool bLateTags = false) {
		if (!Actual || !Actual->GetMovieScene()) return false;
		auto* Current = Actual->GetMovieScene();
		if (bMetadata)
		{
			const auto& Tags = Current->AllTaggedBindings();
			TMap<FName, TArray<FMovieSceneObjectBindingID>> ExpectedTags;
			auto AddTag = [&](FName Tag, const FGuid& Guid, FMovieSceneSequenceID SequenceId) {
				ExpectedTags.FindOrAdd(Tag).Add(UE::MovieScene::FFixedObjectBindingID(Guid, SequenceId));
			};
			AddTag(bLateTags ? TEXT("LateOriginal") : TEXT("Shared"), OriginalGuid, MovieSceneSequenceID::Root);
			for (int32 Index = 0; Index < 2; ++Index)
				for (const FGuid& Guid : {ParentGuids[Index], ChildGuids[Index]})
				{
					AddTag(TEXT("Shared"), Guid, ForeignSequence);
					if (RemovedPairs & (1 << Index)) continue;
					if (!bTagsEdited || Index == 1) AddTag(TEXT("Shared"), Guid, MovieSceneSequenceID::Root);
					if (bTagsEdited && Guid == ParentGuids[0]) AddTag(TEXT("New"), Guid, MovieSceneSequenceID::Root);
					if (bTagsEdited && Guid == ChildGuids[1]) AddTag(TEXT("AddedKeep"), Guid, MovieSceneSequenceID::Root);
				}
			if (!bTagsEdited && !(RemovedPairs & 1)) AddTag(TEXT("OnlyDeleted"), ChildGuids[0], MovieSceneSequenceID::Root);
			if (!Test.TestEqual(TEXT("exact tag group count"), Tags.Num(), ExpectedTags.Num())) return false;
			for (const auto& Expected : ExpectedTags)
			{
				const auto* ActualTag = Tags.Find(Expected.Key);
				if (!Test.TestTrue(TEXT("exact tag members retain non-local identities and non-target bindings"), ActualTag && ActualTag->IDs.Num() == Expected.Value.Num())) return false;
				for (const auto& Id : Expected.Value) if (!Test.TestTrue(TEXT("expected complete fixed binding identity exists"), ActualTag->IDs.Contains(Id))) return false;
			}
			TArray<UMovieSceneFolder*> Folders; GetMovieSceneFoldersRecursive(Current->GetRootFolders(), Folders);
			if (!Test.TestTrue(TEXT("folder objects and nesting survive"), Current->GetRootFolders().Num() == 1 && Folders.Num() == 3
				&& Current->GetRootFolders()[0]->GetName() == TEXT("MetadataRoot"))) return false;
			UMovieSceneFolder* Nested = nullptr;
			for (UMovieSceneFolder* Folder : Folders)
			{
				auto Properties = CapturePersistentObject(*Folder); Properties.Remove(TEXT("ChildObjectBindingStrings"));
				if (!Test.TestTrue(TEXT("complete non-membership folder properties remain unchanged"),
					BaselineFolderProperties.Contains(Folder->GetName()) && Properties.OrderIndependentCompareEqual(BaselineFolderProperties.FindChecked(Folder->GetName())))) return false;
				if (Folder->GetName() == TEXT("MetadataNested")) Nested = Folder;
				else if (!Test.TestTrue(TEXT("non-target folder membership retained"), Folder->GetChildObjectBindings().Num() == (Folder->GetName() == TEXT("MetadataRoot") ? 1 : 0)
					&& (Folder->GetName() != TEXT("MetadataRoot") || Folder->GetChildObjectBindings().Contains(OriginalGuid)))) return false;
			}
			if (!Nested || !Test.TestEqual(TEXT("only explicit folder members removed"), Nested->GetChildObjectBindings().Num(), 4 - 2 * FMath::CountBits(RemovedPairs))) return false;
			for (int32 Index = 0; Index < 2; ++Index)
				for (const FGuid& Guid : {ParentGuids[Index], ChildGuids[Index]})
				{
					const bool bRemoved = (RemovedPairs & (1 << Index)) != 0;
					if (!Test.TestTrue(TEXT("folder identity follows only explicit deletion"), Nested->GetChildObjectBindings().Contains(Guid) != bRemoved)) return false;
				}
		}
		bool Same = Test.TestEqual(TEXT("only explicit parent and child bindings removed"), Current->GetBindings().Num(),
			BindingCount - 2 * FMath::CountBits(RemovedPairs));
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const bool bRemoved = (RemovedPairs & (1 << Index)) != 0;
			for (const FGuid& Guid : {ParentGuids[Index], ChildGuids[Index]})
			{
				const auto* Binding = Current->FindBinding(Guid);
				if (bRemoved) { Same &= Test.TestNull(TEXT("explicit binding GUID gone"), Binding); continue; }
				if (!Test.TestNotNull(TEXT("non-target binding retained"), Binding) || Binding->GetTracks().Num() != 1) return false;
				for (UObject* Object : {static_cast<UObject*>(Binding->GetTracks()[0]), static_cast<UObject*>(Binding->GetTracks()[0]->GetAllSections()[0])})
					Same &= CheckPersistentObject(Test, TEXT("non-target complete track and section"), BaselineOwned.FindChecked(Object->GetPathName(Current)), *Object);
			}
			Same &= Test.TestTrue(TEXT("actual parent actor and component remain alive"), IsValid(Actors[Index]) && IsValid(Components[Index]));
			Same &= Test.TestTrue(TEXT("actor binding resolution matches deletion"), HasOrdinaryBinding(Actual, ParentGuids[Index], Preview.GetWorld(), Actors[Index]) != bRemoved);
			Same &= Test.TestTrue(TEXT("component binding resolution matches deletion"), HasOrdinaryBinding(Actual, ChildGuids[Index], Actors[Index], Components[Index]) != bRemoved);
		}
		if (!Current->FindBinding(OriginalGuid) || !Current->FindBinding(CustomGuid) || Current->GetTracks().Num() != 1) return false;
		Same &= Test.TestTrue(TEXT("untouched custom binding retained without spawning"), Actual->GetBindingReferences()->GetReferences(CustomGuid).Num() == 1);
		Same &= Test.TestEqual(TEXT("root track retains expected independent memory/disk value"), Current->GetTracks()[0]->GetDisplayName().ToString(),
			bDirtyValue ? FString(TEXT("UnsavedRootName")) : OriginalRootName);
		for (UObject* Object : {static_cast<UObject*>(Current->GetTracks()[0]->GetAllSections()[0]),
			static_cast<UObject*>(Current->FindBinding(OriginalGuid)->GetTracks()[0]), static_cast<UObject*>(Current->FindBinding(OriginalGuid)->GetTracks()[0]->GetAllSections()[0])})
			Same &= CheckPersistentObject(Test, TEXT("unrelated root channel and original binding unchanged"), BaselineOwned.FindChecked(Object->GetPathName(Current)), *Object);
		return Same;
	};
	if (!Verify(Sequence, 0, false)) return false;
	if (!Test.TestFalse(TEXT("native readback preserves clean baseline"), Sequence->GetOutermost()->IsDirty())) return false;
	auto ExportArgs = MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"), ObjectPath);
	ExportArgs->SetStringField(TEXT("workspace_path"), Workspace); ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		ExportArgs, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Exported.IsOk()) { Test.AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	FString File, Text; TSharedPtr<FJsonObject> Document, Bindings;
	for (const FString& Candidate : Files) {
		if (Candidate.Contains(TEXT("/.uai/")) || Candidate.Contains(TEXT("\\.uai\\"))) continue;
		TSharedPtr<FJsonObject> Parsed;
		if (!FFileHelper::LoadFileToString(Text, *Candidate) || !FUeAgentStructuredHjsonParser::ParseObject(Text, Candidate, Parsed, Error)) return false;
		if (bMetadata && !Test.TestFalse(TEXT("ordinary export excludes internal persistent bytes"), Text.Contains(TEXT("persistent_bytes")))) return false;
		if (bTags && !Test.TestTrue(TEXT("ordinary binding tags expose no captured identity or machine operation"),
			!Text.Contains(TEXT("binding_ref")) && !Text.Contains(TEXT("\"operation\""))
			&& !Text.Contains(ParentGuids[0].ToString(EGuidFormats::Digits)))) return false;
		const auto Found = FindObjectFieldRecursive(Parsed, TEXT("bindings"));
		if (Found && Found->HasField(TEXT("Parent0")) && Found->HasField(TEXT("Child0"))) { File = Candidate; Document = Parsed; Bindings = Found; }
	}
	if (!Test.TestTrue(TEXT("public readable parent and child keys found"), Bindings.IsValid())) return false;
	auto Persist = [&]() { return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
		&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); };
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
	if (bTags)
	{
		auto SetTags = [&](const TCHAR* Key, const TArray<FString>& Names) {
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Tag : Names) Values.Add(MakeShared<FJsonValueString>(Tag));
			Bindings->GetObjectField(Key)->SetArrayField(TEXT("tags"), MoveTemp(Values));
		};
		SetTags(TEXT("Parent0"), {TEXT("New")});
		SetTags(TEXT("Child0"), {});
		SetTags(TEXT("Child1"), {TEXT("Shared"), TEXT("AddedKeep")});
		Bindings->GetObjectField(TEXT("Parent1"))->RemoveField(TEXT("tags"));
		if (!Persist()) return false;
		TWeakObjectPtr<ULevelSequence> BeforeTags = Sequence; Sequence = nullptr; Scene = nullptr;
		const auto Changed = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-tags"));
		if (!Changed.IsOk()) { Test.AddError(Changed.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); bTagsEdited = true;
		if (!Test.TestTrue(TEXT("public tag addition removal explicit empty and omission persisted independently"),
			!BeforeTags.IsValid(true, true) && Verify(Sequence, 0, false) && !Sequence->GetOutermost()->IsDirty())) return false;
		// The next edit uses a fresh public base after the successful tag mutation.
		const auto Refreshed = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"),
			ExportArgs, Name + TEXT("-tags-readback"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Refreshed.IsOk()) { Test.AddError(Refreshed.GetErrorJson()); return false; }
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		Bindings = FindObjectFieldRecursive(Document, TEXT("bindings"));
		if (!Test.TestTrue(TEXT("fresh public export retains readable local tags"), Bindings.IsValid()
			&& Bindings->GetObjectField(TEXT("Parent0"))->GetArrayField(TEXT("tags")).Num() == 1
			&& Bindings->GetObjectField(TEXT("Child0"))->GetArrayField(TEXT("tags")).IsEmpty())) return false;
	}
	TArray<uint8> DiskBefore;
	Bindings->RemoveField(TEXT("Parent0")); Bindings->RemoveField(TEXT("Child0"));
	if (!Persist()) return false;
	TWeakObjectPtr<ULevelSequence> Old = Sequence; Sequence = nullptr; Scene = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name + TEXT("-joint-delete"));
	if (!Imported.IsOk()) { Test.AddError(Imported.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Test.TestTrue(TEXT("public joint deletion persisted after independent reload"), !Old.IsValid(true, true)
		&& Verify(Sequence, 1, false) && !Sequence->GetOutermost()->IsDirty()
		&& !DeletedTracks[0].IsValid(true, true) && !DeletedTracks[1].IsValid(true, true))) return false;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0])->SetDisplayName(FText::FromString(TEXT("UnsavedRootName")));
	Sequence->MarkPackageDirty();
	if (!Test.TestTrue(TEXT("late baseline contains a real unsaved value"), Verify(Sequence, 1, true) && Sequence->GetOutermost()->IsDirty())) return false;
	TSharedPtr<FJsonObject> Desired; DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError NativeError;
	if (!DomainAdapters::TimelineAnimation::TryReadLevelSequenceStructure(*Sequence, ObjectPath, Desired, NativeError)) return false;
	auto NativeBindings = Desired->GetArrayField(TEXT("bindings"));
	if (bTags) for (const auto& Binding : NativeBindings)
		if (Binding->AsObject()->GetStringField(TEXT("binding_ref")) == OriginalGuid.ToString(EGuidFormats::Digits))
			Binding->AsObject()->SetArrayField(TEXT("tags"), {MakeShared<FJsonValueString>(TEXT("LateOriginal"))});
	NativeBindings.RemoveAll([&](const TSharedPtr<FJsonValue>& Binding) {
		const FString Ref = Binding->AsObject()->GetStringField(TEXT("binding_ref"));
		return Ref == ParentGuids[1].ToString(EGuidFormats::Digits) || Ref == ChildGuids[1].ToString(EGuidFormats::Digits);
	});
	Desired->SetArrayField(TEXT("bindings"), NativeBindings);
	bool bLateObserved = false;
	UeAgentReflectionAuthoring::FAssetApplyRequest Request; Request.TypeId = TEXT("sequence");
	Request.AssetPath = PackageName; Request.bSave = true; Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>(); Request.ProviderExtension->SetObjectField(TEXT("movie_scene"), Desired);
	Request.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure) {
		auto* Reloaded = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		TArray<uint8> DeletedDisk;
		bLateObserved = !Old.IsValid(true, true) && Reloaded && !Reloaded->GetOutermost()->IsDirty()
			&& Verify(Reloaded, 3, true, bTags) && FFileHelper::LoadFileToArray(DeletedDisk, *Filename) && DeletedDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_joint_binding_late_rejected"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Old = Sequence; Sequence = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult Outcome;
	Test.TestFalse(TEXT("late joint deletion rejects"), UeAgentReflectionAuthoring::ApplyAsset(Request, Outcome, Name + TEXT("-late")));
	if (!Test.TestTrue(TEXT("joint deletion observed persisted before exact recovery"), bLateObserved && !Old.IsValid(true, true)
		&& Outcome.bRollbackAttempted && Outcome.bRollbackSucceeded && Outcome.Diagnostic.Code == TEXT("t208_joint_binding_late_rejected"))) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath); TArray<uint8> DiskAfter;
	if (!Test.TestTrue(TEXT("rollback restores unsaved native value dirty flag and original disk independently"),
		Verify(Sequence, 1, true) && Sequence->GetOutermost()->IsDirty()
		&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
	// The unsaved fixture value has been verified. Discard it only for a separate disk read.
	CastChecked<UMovieSceneFloatTrack>(Sequence->GetMovieScene()->GetTracks()[0])->SetDisplayName(FText::FromString(OriginalRootName));
	Sequence->GetOutermost()->SetDirtyFlag(false); Sequence = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) return false;
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	Test.TestTrue(TEXT("independent disk read restores original value and binding pair"), Verify(Sequence, 1, false) && !Sequence->GetOutermost()->IsDirty());
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSequenceBindingDependenciesTest,
    "UAI.Root.T208.LevelSequence.BindingDependencies", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSequenceBindingDependenciesTest::RunTest(const FString&) { return RunSequenceBindingDependencies(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSequenceBindingMetadataTest,
    "UAI.Root.T208.LevelSequence.BindingMetadata", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSequenceBindingMetadataTest::RunTest(const FString&) { return RunSequenceBindingDependencies(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSequenceBindingTagsTest,
    "UAI.Root.T208.LevelSequence.BindingTags", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSequenceBindingTagsTest::RunTest(const FString&) { return RunSequenceBindingDependencies(*this, true, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSequenceFoldersTest,
    "UAI.Root.T208.LevelSequence.Folders", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSequenceFoldersTest::RunTest(const FString&) { return RunSequenceBindingDependencies(*this, true, false, true); }
