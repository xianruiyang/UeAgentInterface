#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneScalarChannel.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "MovieScene.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieSceneSequence.h"
#include "MovieSceneCommonHelpers.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "MovieScene/Parameters/MovieSceneNiagaraVectorParameterTrack.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneSpawnRegister.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "IMovieScenePlayer.h"
#include "Evaluation/MovieSceneSequenceHierarchy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "MovieSceneNameableTrack.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::Authoring::Timeline
{
	using Json::Fail;
	namespace ScalarChannel
	{
		bool HasRequiredSectionContext(const UMovieSceneTrack& Track)
		{
			if (const auto* Object = Cast<UMovieSceneObjectPropertyTrack>(&Track)) return Object->PropertyClass != nullptr;
			auto ValidVectorSize = [](int32 Channels) { return Channels >= 2 && Channels <= 4; };
			if (const auto* Vector = Cast<UMovieSceneFloatVectorTrack>(&Track)) return ValidVectorSize(Vector->GetNumChannelsUsed());
			if (const auto* Vector = Cast<UMovieSceneDoubleVectorTrack>(&Track)) return ValidVectorSize(Vector->GetNumChannelsUsed());
			if (const auto* Vector = Cast<UMovieSceneNiagaraVectorParameterTrack>(&Track)) return ValidVectorSize(Vector->GetChannelsUsed());
			return true;
		}

		UMovieSceneTrack* MakeSectionTemplateTrack(const UMovieSceneTrack& Source)
		{
			if (!HasRequiredSectionContext(Source)) return nullptr;
			// Copying selected fields loses plugin-specific factory context (for
			// example Niagara parameter dimensions or ControlRig-owned state).
			auto* Template = DuplicateObject<UMovieSceneTrack>(&Source, GetTransientPackage(),
				MakeUniqueObjectName(GetTransientPackage(), Source.GetClass()));
			if (Template)
			{
				Template->ClearFlags(RF_Public | RF_Standalone);
				Template->SetFlags(RF_Transient);
			}
			return Template;
		}
	}
	namespace
	{
		using FDiagnostic = UE::UAI::Contract::FDiagnostic;
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationRate& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationPoint& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationBound& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationRange& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationKey& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationChannel& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationSection& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationTrack& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationBinding& Value);
		TSharedPtr<FJsonValue> BusinessValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		template<typename T> TSharedPtr<FJsonValue> BusinessValue(const TArray<T>& Values) {
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const auto& Value : Values) Result.Add(BusinessValue(Value));
			return MakeShared<FJsonValueArray>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationRate& Value) {
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("numerator"), Value.Numerator); Result->SetStringField(TEXT("denominator"), Value.Denominator);
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationPoint& Value) {
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("tick"), Value.Tick); Result->SetStringField(TEXT("state"), Value.State);
			Result->SetField(TEXT("rate"), BusinessValue(Value.Rate));
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationBound& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("kind"), Value.Kind);
			if (Value.Point.IsSet()) Result->SetField(TEXT("point"), BusinessValue(Value.Point.GetValue()));
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationRange& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("state"), Value.State);
			Result->SetField(TEXT("lower"), BusinessValue(Value.Lower)); Result->SetField(TEXT("upper"), BusinessValue(Value.Upper));
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationKey& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetField(TEXT("time"), BusinessValue(Value.Time));
			Result->SetStringField(TEXT("value_type"), Value.ValueType);
			if (Value.bHasValueLexeme) Result->SetStringField(TEXT("value_lexeme"), Value.ValueLexeme);
			if (Value.Value) Result->SetField(TEXT("value"), Value.Value);
			if (Value.Properties) Result->SetField(TEXT("properties"), Value.Properties);
			if (Value.Transform.IsSet()) {
				auto Transform = MakeShared<FJsonObject>(); Transform->SetStringField(TEXT("quantity"), Value.Transform->Quantity);
				Transform->SetField(TEXT("components"), BusinessValue(Value.Transform->Components));
				auto Frame = MakeShared<FJsonObject>(); Frame->SetNumberField(TEXT("dimension"), Value.Transform->Frame.Dimension);
				Frame->SetStringField(TEXT("unit"), Value.Transform->Frame.Unit); Transform->SetObjectField(TEXT("frame"), Frame);
				Result->SetObjectField(TEXT("transform"), Transform);
			}
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationChannel& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("name"), Value.Name);
			Result->SetStringField(TEXT("channel_kind"), Value.ChannelKind); Result->SetField(TEXT("keys"), BusinessValue(Value.Keys));
			if (Value.Properties) Result->SetField(TEXT("properties"), Value.Properties);
			if (Value.DefaultIsSet.IsSet()) {
				auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("is_set"), Value.DefaultIsSet.GetValue());
				if (Value.DefaultIsSet.GetValue()) {
					if (Value.DefaultValue) Default->SetField(TEXT("value"), Value.DefaultValue);
					else Default->SetStringField(TEXT("value_lexeme"), Value.DefaultValueLexeme);
				}
				Result->SetObjectField(TEXT("default"), Default);
			}
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationSection& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("section_kind"), Value.SectionKind);
			Result->SetField(TEXT("range"), BusinessValue(Value.Range)); Result->SetField(TEXT("keys"), BusinessValue(Value.Keys));
			Result->SetField(TEXT("channels"), BusinessValue(Value.Channels));
			if (Value.Properties) Result->SetObjectField(TEXT("properties"), Value.Properties);
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationTrack& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("track_kind"), Value.TrackKind);
			Result->SetStringField(TEXT("display_name"), Value.DisplayName); Result->SetStringField(TEXT("property_path"), Value.PropertyPath);
			Result->SetField(TEXT("sections"), BusinessValue(Value.Sections));
			if (Value.Properties) Result->SetObjectField(TEXT("properties"), Value.Properties);
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> BusinessValue(const FTimelineAnimationBinding& Value) {
			auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("binding_kind"), Value.BindingKind);
			if (Value.Tags.IsSet()) Result->SetField(TEXT("tags"), BusinessValue(Value.Tags.GetValue()));
			Result->SetField(TEXT("tracks"), BusinessValue(Value.Tracks));
			if (Value.Properties) Result->SetObjectField(TEXT("properties"), Value.Properties);
			if (Value.TemplateProperties) Result->SetObjectField(TEXT("template_properties"), Value.TemplateProperties);
			return MakeShared<FJsonValueObject>(Result);
		}
		// The full-record predicate protects deletion semantics, including additional
		// context fields. Only this source owner chooses public business facts; GUIDs,
		// native paths, source metadata and the raw record never enter that projection.
		template<typename T>
		bool RequireUneditedRecord(const T* Before, const T& After, const TCHAR* Code,
			const TCHAR* Reason, FDiagnostic& Error)
		{
			if (!Before || !Before->SourceRecord || !After.SourceRecord) {
				Error = FDiagnostic::Failure(Code, !Before ? TEXT("The removed descendant has no captured baseline record.")
					: TEXT("The deletion comparison is missing a complete captured or requested record."), TEXT("timeline_prepare"));
				Error.PropertyPath = !Before || !Before->SourceRecord ? TEXT("captured_record") : TEXT("requested_record");
				Error.ExpectedValuePresent = true; Error.ActualValuePresent = false;
				if (After.SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *After.SourceRecord);
				return false;
			}
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalValuePtr Left, Right;
			if (!StateSemantic::CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Before->SourceRecord), Resources, Left, Error)) {
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Before->SourceRecord); return false;
			}
			if (!StateSemantic::CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(After.SourceRecord), Resources, Right, Error)) {
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *After.SourceRecord); return false;
			}
			StateSemantic::FUeAgentSemanticComparison Comparison;
			if (!StateSemantic::TryCompareValues(Left.ToSharedRef(), Right.ToSharedRef(), TEXT("/"), Resources, Comparison, Error)) {
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *After.SourceRecord); return false;
			}
			if (Comparison.IsEquivalent()) return true;
			Error = FDiagnostic::Failure(Code, Reason, TEXT("timeline_prepare"));
			if (!Comparison.GetDifferences().IsEmpty()) {
				const auto& Difference = Comparison.GetDifferences()[0];
				Error.Source.InternalJsonPath = Difference.GetPath();
				// Read the same difference from the source-owned public projection. Never
				// compare again: a second traversal can select a different failing field.
				auto AtPath = [](TSharedPtr<FJsonValue> Value, const FString& Path) {
					if (Path.IsEmpty() || Path == TEXT("/")) return Value;
					TArray<FString> Parts; Path.RightChop(1).ParseIntoArray(Parts, TEXT("/"), false);
					for (FString Part : Parts) {
						Part = Part.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
						if (!Value) break;
						if (Value->Type == EJson::Object) Value = Value->AsObject()->TryGetField(Part);
						else if (Value->Type == EJson::Array) {
							int32 Index = INDEX_NONE;
							Value = LexTryParseString(Index, *Part) && Value->AsArray().IsValidIndex(Index) ? Value->AsArray()[Index] : nullptr;
						} else Value.Reset();
					}
					return Value;
				};
				Error.ExpectedValue = AtPath(BusinessValue(*Before), Difference.GetPath());
				Error.ActualValue = AtPath(BusinessValue(After), Difference.GetPath());
				Error.ExpectedValuePresent = Difference.GetLeft().IsValid();
				Error.ActualValuePresent = Difference.GetRight().IsValid();
				if (Error.ExpectedValue || Error.ActualValue) Error.PropertyPath = Difference.GetPath();
				else Error.PublicMessage += TEXT(" The captured record context changed.");
				FDiagnostic ProjectionError;
				if (Difference.GetLeft() && !StateSemantic::CanonicalJson::TryToJsonValue(*Difference.GetLeft(), Error.ExpectedEvidence, ProjectionError)) Error.Append(ProjectionError);
				if (Difference.GetRight() && !StateSemantic::CanonicalJson::TryToJsonValue(*Difference.GetRight(), Error.ActualEvidence, ProjectionError)) Error.Append(ProjectionError);
			}
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *After.SourceRecord);
			return false;
		}

		void DestroyBindingInstances(UMovieSceneSequence& Sequence, const FGuid& BindingId, bool bLegacySpawnable)
		{
			const FMovieSceneBindingReferences* References = Sequence.GetBindingReferences();
			const TArrayView<const FMovieSceneBindingReference> Bindings = References
				? References->GetReferences(BindingId) : TArrayView<const FMovieSceneBindingReference>();
			if (!bLegacySpawnable && !Bindings.ContainsByPredicate([](const auto& Binding) { return Binding.CustomBinding != nullptr; })) return;
			TArray<IMovieScenePlayer*> Players;
			// Stopped players can retain preview/spawned instances. Use the native
			// registry, including Sequencer, rather than scanning world actors.
			IMovieScenePlayer::Get(Players, false);
			for (IMovieScenePlayer* Player : Players)
			{
				const auto Playback = Player->FindSharedPlaybackState();
				if (!Playback) continue;
				auto DestroyInSequence = [&](FMovieSceneSequenceID SequenceId)
				{
					if (Playback->GetSequence(SequenceId) != &Sequence) return;
					if (bLegacySpawnable)
						Player->GetSpawnRegister().DestroySpawnedObject(BindingId, SequenceId, Playback.ToSharedRef(), 0);
					for (int32 Index = 0; Index < Bindings.Num(); ++Index)
						if (Bindings[Index].CustomBinding && Bindings[Index].CustomBinding->WillSpawnObject(Playback.ToSharedRef()))
							Player->GetSpawnRegister().DestroySpawnedObject(BindingId, SequenceId, Playback.ToSharedRef(), Index);
				};
				DestroyInSequence(MovieSceneSequenceID::Root);
				if (const FMovieSceneSequenceHierarchy* Hierarchy = Playback->GetHierarchy())
					for (const auto& SubSequence : Hierarchy->AllSubSequenceData()) DestroyInSequence(SubSequence.Key);
			}
		}

		// MovieSceneTools initializes this native context from the leaf property.
		// Asset authoring has no ISequencer; it uses the same public track setters.
		bool InitializePropertyContext(UMovieSceneTrack& Track, const FProperty& Property)
		{
			// Property editors match their exact track type. Inheritance alone does
			// not make a global/specialized track a property animator (Fade/Slomo).
			// A property's native override, such as hidden -> Visibility, is explicit.
			const FString& OverrideName = Property.GetMetaData(TEXT("SequencerTrackClass"));
			UClass* Override = nullptr;
			if (!OverrideName.IsEmpty())
			{
				Override = UClass::TryFindTypeSlow<UClass>(OverrideName);
				if (!Override) Override = LoadObject<UClass>(nullptr, *OverrideName);
			}
			if (Override && Track.GetClass() != Override) return false;
			auto Matches = [&](UClass* NativePropertyTrack) {
				return Track.GetClass() == NativePropertyTrack
					|| (Override == Track.GetClass() && Override->IsChildOf(NativePropertyTrack));
			};
			const auto* Struct = CastField<FStructProperty>(&Property);
			const FName Type = Struct ? Struct->Struct->GetFName() : NAME_None;
			if (Matches(UMovieSceneFloatVectorTrack::StaticClass()))
			{
				auto* Vector = CastChecked<UMovieSceneFloatVectorTrack>(&Track);
				const int32 Dimensions = Type == NAME_Vector2f ? 2 : Type == NAME_Vector3f ? 3 : Type == NAME_Vector4f ? 4 : 0;
				if (!Dimensions) return false;
				Vector->SetNumChannelsUsed(Dimensions); return true;
			}
			if (Matches(UMovieSceneDoubleVectorTrack::StaticClass()))
			{
				auto* Vector = CastChecked<UMovieSceneDoubleVectorTrack>(&Track);
				const int32 Dimensions = Type == NAME_Vector2D ? 2
					: (Type == NAME_Vector || Type == NAME_Vector3d) ? 3
					: (Type == NAME_Vector4 || Type == NAME_Vector4d) ? 4 : 0;
				if (!Dimensions) return false;
				Vector->SetNumChannelsUsed(Dimensions); return true;
			}
			if (Matches(UMovieSceneObjectPropertyTrack::StaticClass()))
			{
				auto* ObjectTrack = CastChecked<UMovieSceneObjectPropertyTrack>(&Track);
				const auto* Object = CastField<FObjectPropertyBase>(&Property);
				if (!Object) return false;
				ObjectTrack->PropertyClass = Object->PropertyClass;
				if (const auto* Class = CastField<FClassProperty>(Object); Class && Object->HasAllPropertyFlags(CPF_UObjectWrapper))
				{ ObjectTrack->PropertyClass = Class->MetaClass; ObjectTrack->bClassProperty = true; }
				return true;
			}
			if (Matches(UMovieSceneByteTrack::StaticClass()))
			{
				auto* Byte = CastChecked<UMovieSceneByteTrack>(&Track);
				if (const auto* Enum = CastField<FEnumProperty>(&Property)) { Byte->SetEnum(Enum->GetEnum()); return true; }
				if (const auto* NativeByte = CastField<FByteProperty>(&Property)) { Byte->SetEnum(NativeByte->Enum); return true; }
				return false;
			}
			if (Matches(UMovieSceneFloatTrack::StaticClass())) return Property.IsA<FFloatProperty>();
			if (Matches(UMovieSceneDoubleTrack::StaticClass())) return Property.IsA<FDoubleProperty>();
			if (Matches(UMovieSceneBoolTrack::StaticClass())) return Property.IsA<FBoolProperty>();
			if (Matches(UMovieSceneIntegerTrack::StaticClass())) return Property.IsA<FIntProperty>();
			if (Matches(UMovieSceneStringTrack::StaticClass())) return Property.IsA<FStrProperty>();
			if (Matches(UMovieSceneColorTrack::StaticClass())) return Type == NAME_Color || Type == NAME_LinearColor;
			if (Matches(UMovieScene3DTransformTrack::StaticClass())) return Type == NAME_Transform;
			if (Matches(UMovieScene2DTransformTrack::StaticClass())) return Type == TEXT("WidgetTransform");
			if (Matches(UMovieSceneMarginTrack::StaticClass())) return Type == TEXT("Margin");
			// A native metadata-selected animator need not inherit a scalar track
			// (Visibility derives directly from PropertyTrack). The property owns
			// this association; do not replace it with another UAI class list.
			return Override == Track.GetClass() && ScalarChannel::HasRequiredSectionContext(Track);
		}

		struct FTrackClassCandidate
		{
			ContentDiscovery::FNameSelectionEntry Selection;
			TStrongObjectPtr<UClass> Class;
		};

		bool CollectTrackClasses(const UMovieScene& Scene, const FProperty* Property,
			TArray<FTrackClassCandidate>& Result, UE::UAI::Contract::FDiagnostic& OutError)
		{
			Result.Reset();
			TArray<TStrongObjectPtr<UClass>> LoadedClasses;
			if (!ContentDiscovery::TryLoadDerivedBlueprintClasses(*UMovieSceneTrack::StaticClass(), LoadedClasses, OutError)) return false;
			const auto* Sequence = Scene.GetTypedOuter<UMovieSceneSequence>();
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class->IsChildOf(UMovieSceneTrack::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
					|| !Scene.IsTrackClassAllowed(Class)
					|| (Sequence && Sequence->IsTrackSupported(Class) == ETrackSupport::NotSupported)) continue;
				// Discovery reads native class metadata and the supplied property context.
				// Do not execute every class's section factory as a capability probe.
				if (Property)
				{
					TStrongObjectPtr<UMovieSceneTrack> Template(NewObject<UMovieSceneTrack>(GetTransientPackage(), Class));
					if (!InitializePropertyContext(*Template, *Property)) continue;
				}
				else if (!ScalarChannel::HasRequiredSectionContext(*Class->GetDefaultObject<UMovieSceneTrack>())) continue;
				ContentDiscovery::FNameSelectionEntry Entry;
				Entry.Name = Class->GetDisplayNameText().ToString();
				Entry.Identity = Class->GetPathName();
				Entry.Select.Add(TEXT("class"), Class->GetName());
				Entry.Select.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(Class->GetPathName()));
				Result.Add({MoveTemp(Entry), TStrongObjectPtr<UClass>(Class)});
			}
			Result.Sort([](const auto& A, const auto& B) { return A.Selection.Identity < B.Selection.Identity; });
			return true;
		}

		UClass* ResolveTrackClass(const UMovieScene& Scene, const FJsonObject& Record, UE::UAI::Contract::FDiagnostic& Error, const FProperty* Property = nullptr)
		{
			auto RejectField = [&](const TCHAR* Code, const TCHAR* Reason, const FJsonObject& Object, const FString& Field) {
				Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
					MakeShared<FJsonValueString>(Reason), Object.TryGetField(Field), TEXT("authoring"), Reason);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Object, Field);
			};
			FString Name;
			if (!Record.TryGetStringField(TEXT("name"), Name)) { RejectField(TEXT("timeline_track_candidate_name_required"), TEXT("The track candidate name must be a string."), Record, TEXT("name")); return nullptr; }
			TMap<FString, FString> Select;
			const TSharedPtr<FJsonObject>* Selection = nullptr;
			if (Record.HasField(TEXT("select")))
			{
				if (!Record.TryGetObjectField(TEXT("select"), Selection)) { RejectField(TEXT("timeline_track_candidate_select_invalid"), TEXT("Track candidate discriminators must be an object."), Record, TEXT("select")); return nullptr; }
				for (const auto& Field : (*Selection)->Values)
				{
					FString Value;
					if (!Field.Value || !Field.Value->TryGetString(Value)) {
						RejectField(TEXT("timeline_track_candidate_select_invalid"), TEXT("A track candidate discriminator must be a string."), **Selection, Field.Key);
						if (Error.Source.SourceFile.IsEmpty()) { UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/select")); UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Record); }
						return nullptr;
					}
					Select.Add(Field.Key, Value);
				}
			}
			TArray<FTrackClassCandidate> Candidates;
			if (!CollectTrackClasses(Scene, Property, Candidates, Error))
			{ UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Record, TEXT("name")); return nullptr; }
			TArray<ContentDiscovery::FNameSelectionEntry> Entries;
			for (const auto& Candidate : Candidates) Entries.Add(Candidate.Selection);
			int32 Index = INDEX_NONE;
			if (!ContentDiscovery::TryResolveNameSelection(Entries, Name, Select, Index, Error))
			{ UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Record, TEXT("name")); return nullptr; }
			return Candidates[Index].Class.Get();
		}

		UeAgentDetailsAuthoringService::FCapturePolicy MovieSceneObjectPropertyPolicy()
		{
			UeAgentDetailsAuthoringService::FCapturePolicy Policy;
			// These values have native structural owners in this adapter.
			Policy.ExcludedProperties = {TEXT("DisplayName"), TEXT("SectionRange"), TEXT("TransformMask"), TEXT("SectionToKey"),
				TEXT("PropertyName"), TEXT("PropertyPath"), TEXT("PropertyClass"), TEXT("bClassProperty"),
				TEXT("Enum"), TEXT("NumChannelsUsed"), TEXT("ChannelsUsed")};
			Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) {
				const auto* Struct = CastField<FStructProperty>(Property);
				return !Struct || !Struct->Struct->IsChildOf(FMovieSceneChannel::StaticStruct());
			};
			return Policy;
		}

		bool CaptureMovieSceneObjectProperties(const UObject& Object, FJsonObject& Record, UE::UAI::Contract::FDiagnostic& Error)
		{
			TSharedPtr<FJsonObject> Properties, Definitions;
			if (!Composition::HjsonImport::CaptureReflectedObjectRecord(Object, MovieSceneObjectPropertyPolicy(),
				Properties, Definitions, Error)) return false;
			Record.SetObjectField(TEXT("properties"), Properties);
			Record.SetObjectField(TEXT("property_definitions"), Definitions);
			return true;
		}

		bool ResolveRemovedBindingContext(UMovieScene& Scene, const FTimelineAnimationStructure& Before,
			FTimelineAnimationStructure& After, FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSet<FString> Deleted;
			for (const auto& Row : Before.Bindings)
				if (!After.Bindings.ContainsByPredicate([&](const auto& Item) { return Item.BindingRef == Row.BindingRef; }))
					Deleted.Add(Row.BindingRef);
			bool bExpanded = true;
			while (bExpanded)
			{
				bExpanded = false;
				for (int32 Index = 0; Index < Scene.GetPossessableCount(); ++Index)
				{
					const auto& Possessable = Scene.GetPossessable(Index);
					const FString Ref = Possessable.GetGuid().ToString(EGuidFormats::Digits);
					if (Deleted.Contains(Possessable.GetParent().ToString(EGuidFormats::Digits)) && !Deleted.Contains(Ref))
					{ Deleted.Add(Ref); bExpanded = true; }
				}
			}
			for (const auto& Row : After.Bindings)
				if (Deleted.Contains(Row.BindingRef))
				{
					const auto* Original = Before.Bindings.FindByPredicate([&](const auto& Item) { return Item.BindingRef == Row.BindingRef; });
					if (!RequireUneditedRecord(Original, Row, TEXT("timeline_deleted_binding_has_edited_descendant"),
						TEXT("A descendant binding was edited while its parent binding was removed."), Error)) return false;
				}
			After.Bindings.RemoveAll([&](const auto& Row) { return Deleted.Contains(Row.BindingRef); });
			return true;
		}

		// One native plan owns containment, including the contents of removed folders.
		// References reaching here have already been resolved by the common keyed owner.
		struct FPreparedMovieSceneFolders
		{
			TMap<FString, UMovieSceneFolder*> Existing;
			TArray<UMovieSceneFolder*> Removed;
			TArray<FTimelineAnimationFolder> Desired;
			TArray<int32> AttachmentOrder;
			bool bActive = false;

			bool Prepare(UMovieScene& Scene, const FTimelineAnimationStructure& Before,
				FTimelineAnimationStructure& After, FUeAgentTimelineAnimationExecutionError& Error)
			{
				if (!After.Folders.IsSet()) return true;
				bActive = true;
				if (!Before.Folders.IsSet()) return Fail(Error, FDiagnostic::Failure(TEXT("timeline_animation_folders_unsupported"),
					TEXT("The captured movie scene does not provide folder authoring data."), TEXT("timeline_prepare")), Scene.GetPathName());
				auto Reject = [&](const FTimelineAnimationFolder& Folder, const TCHAR* Reason, const FString& Pointer,
					TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual) {
					Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_folder_change_invalid"), Pointer,
						MoveTemp(Expected), MoveTemp(Actual), TEXT("timeline_prepare"), Reason);
					Error.CandidateName = Folder.Name;
					if (Folder.SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Folder.SourceRecord);
					return false;
				};
				auto RejectReference = [&](const FTimelineAnimationFolder& Folder, const TCHAR* Reason, const FString& Pointer,
					const FTimelineAnimationItemReference& Ref) {
					Reject(Folder, Reason, Pointer, MakeShared<FJsonValueString>(TEXT("an existing, unambiguous reference")),
						MakeShared<FJsonValueBoolean>(!Ref.Key.IsEmpty()));
					Error.ActualEvidence = MakeShared<FJsonValueString>(Ref.Key);
					return false;
				};
				TArray<UMovieSceneFolder*> Native;
				GetMovieSceneFoldersRecursive(Scene.GetRootFolders(), Native);
				for (auto* Folder : Native) Existing.Add(Folder->GetPathName(&Scene), Folder);
				Desired = After.Folders.GetValue();
				auto SameSet = [](const auto& A, const auto& B) {
					if (A.Num() != B.Num()) return false;
					for (const auto& Ref : A) if (!B.Contains(Ref)) return false;
					return true;
				};
				auto RequireUneditedFolder = [&](const FTimelineAnimationFolder& A, const FTimelineAnimationFolder& B) {
					const TCHAR* Reason = TEXT("A descendant folder was edited while an ancestor folder was removed.");
					auto SameReference = [&](const auto& Left, const auto& Right, const TCHAR* Field) {
						if (Left == Right) return true;
						Reject(B, Reason, FString(TEXT("/")) + Field, MakeShared<FJsonValueString>(TEXT("unchanged reference")), nullptr);
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(Left.Key);
						Error.ActualEvidence = MakeShared<FJsonValueString>(Right.Key);
						Error.ExpectedValuePresent = !Left.Key.IsEmpty(); Error.ActualValuePresent = !Right.Key.IsEmpty();
						return false;
					};
					if (!SameReference(A.Ref, B.Ref, TEXT("folder_ref")) || !SameReference(A.Parent, B.Parent, TEXT("parent"))) return false;
					if (A.Name != B.Name) return Reject(B, Reason, TEXT("/name"), MakeShared<FJsonValueString>(A.Name), MakeShared<FJsonValueString>(B.Name));
					auto SameMembers = [&](const auto& Left, const auto& Right, const TCHAR* Field) {
						if (SameSet(Left, Right)) return true;
						for (int32 Index = 0; Index < Right.Num(); ++Index) if (!Left.Contains(Right[Index])) {
							Reject(B, Reason, FString::Printf(TEXT("/%s/%d"), Field, Index), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							Error.ActualEvidence = MakeShared<FJsonValueString>(Right[Index].Key); return false;
						}
						Reject(B, Reason, FString(TEXT("/")) + Field, MakeShared<FJsonValueNumber>(Left.Num()), MakeShared<FJsonValueNumber>(Right.Num()));
						return false;
					};
					return SameMembers(A.Bindings, B.Bindings, TEXT("bindings")) && SameMembers(A.RootTracks, B.RootTracks, TEXT("root_tracks"));
				};
				TSet<FString> SeenExisting, SeenNew, RemovedRefs;
				for (const auto& Folder : Desired)
				{
					auto& Seen = Folder.Ref.bNew ? SeenNew : SeenExisting;
					if (Folder.Ref.Key.IsEmpty()) return RejectReference(Folder, TEXT("The folder reference is empty."), TEXT("/folder_ref"), Folder.Ref);
					if (Seen.Contains(Folder.Ref.Key)) return RejectReference(Folder, TEXT("The same folder reference occurs more than once."), TEXT("/folder_ref"), Folder.Ref);
					if (!Folder.Ref.bNew && !Existing.Contains(Folder.Ref.Key)) return RejectReference(Folder, TEXT("The referenced folder no longer exists in the movie scene."), TEXT("/folder_ref"), Folder.Ref);
					if (Folder.Name.TrimStartAndEnd().IsEmpty() || FName(*Folder.Name).IsNone())
						return Reject(Folder, TEXT("The folder name must be non-empty and must not resolve to the reserved name None."), TEXT("/name"),
							MakeShared<FJsonValueString>(TEXT("non-empty name other than None")), MakeShared<FJsonValueString>(Folder.Name));
					Seen.Add(Folder.Ref.Key);
				}
				for (const auto& Pair : Existing)
					if (!SeenExisting.Contains(Pair.Key)) RemovedRefs.Add(Pair.Key);
				// Follow captured containment, never edited relationships, to freeze deletion.
				TArray<UE::UAI::HjsonWorkspace::FOwnedItemRemovalCandidate> OwnedFolders;
				for (const auto& Folder : Before.Folders.GetValue())
					OwnedFolders.Add({Folder.Ref.Key, Folder.Parent.Key});
				TArray<FString> RemovedDescendants;
				if (!UE::UAI::HjsonWorkspace::TryCollectOwnedItemRemovals(OwnedFolders, RemovedRefs.Array(),
					[&](const FString& Id, bool& bUnchanged) {
						const auto* Original = Before.Folders->FindByPredicate([&](const auto& Row) { return Row.Ref.Key == Id; });
						const auto* Edited = Desired.FindByPredicate([&](const auto& Row) { return Row.Ref == Original->Ref; });
						bUnchanged = !Edited || RequireUneditedFolder(*Original, *Edited);
						return bUnchanged;
					}, RemovedDescendants)) return false;
				for (const FString& Id : RemovedDescendants) RemovedRefs.Add(Id);
				TSet<FString> CascadedBindings, CascadedTracks;
				for (const auto& Folder : Before.Folders.GetValue())
				{
					if (!RemovedRefs.Contains(Folder.Ref.Key)) continue;
					const auto* Edited = Desired.FindByPredicate([&](const auto& Row) { return Row.Ref == Folder.Ref; });
					if (Edited && !RequireUneditedFolder(Folder, *Edited)) return false;
					Removed.Add(Existing.FindChecked(Folder.Ref.Key));
					for (const auto& Ref : Folder.Bindings) CascadedBindings.Add(Ref.Key);
					for (const auto& Ref : Folder.RootTracks) CascadedTracks.Add(Ref.Key);
				}
				for (const auto& Binding : After.Bindings)
					if (CascadedBindings.Contains(Binding.BindingRef))
					{
						const auto* Original = Before.Bindings.FindByPredicate([&](const auto& Row) { return Row.BindingRef == Binding.BindingRef; });
						if (!RequireUneditedRecord(Original, Binding, TEXT("timeline_animation_folder_change_invalid"),
							TEXT("A binding was edited while its containing folder was removed."), Error)) return false;
					}
				for (const auto& Track : After.RootTracks)
					if (CascadedTracks.Contains(Track.TrackRef))
					{
						const auto* Original = Before.RootTracks.FindByPredicate([&](const auto& Row) { return Row.TrackRef == Track.TrackRef; });
						if (!RequireUneditedRecord(Original, Track, TEXT("timeline_animation_folder_change_invalid"),
							TEXT("A root track was edited while its containing folder was removed."), Error)) return false;
					}
				After.Bindings.RemoveAll([&](const auto& Row) { return CascadedBindings.Contains(Row.BindingRef); });
				After.RootTracks.RemoveAll([&](const auto& Row) { return CascadedTracks.Contains(Row.TrackRef); });
				if (!ResolveRemovedBindingContext(Scene, Before, After, Error)) return false;
				Desired.RemoveAll([&](const auto& Row) { return !Row.Ref.bNew && RemovedRefs.Contains(Row.Ref.Key); });
				// Every surviving relation is checked after closure. Containment exceptions
				// in the public owner never turn into surviving dangling references.
				for (const auto& Folder : Desired)
				{
					TArray<FTimelineAnimationItemReference> Ancestors;
					const auto* Cursor = &Folder;
					while (!Cursor->Parent.Key.IsEmpty())
					{
						if (Cursor->Parent == Folder.Ref || Ancestors.Contains(Cursor->Parent)) return RejectReference(*Cursor, TEXT("The folder parent relationship forms a cycle."), TEXT("/parent"), Cursor->Parent);
						Ancestors.Add(Cursor->Parent);
						const auto* Child = Cursor;
						Cursor = Desired.FindByPredicate([&](const auto& Row) { return Row.Ref == Child->Parent; });
						if (!Cursor) return RejectReference(*Child, TEXT("The folder parent is absent from the surviving folder set."), TEXT("/parent"), Child->Parent);
					}
					for (const auto& Other : Desired)
						if (!(Other.Ref == Folder.Ref) && Other.Parent == Folder.Parent && FName(*Other.Name) == FName(*Folder.Name))
							return Reject(Folder, TEXT("Two folders under the same parent have the same native name."), TEXT("/name"),
								MakeShared<FJsonValueString>(TEXT("a unique sibling name")), MakeShared<FJsonValueString>(Folder.Name));
				}
				auto PlanMembers = [&](auto Member, const TSet<FString>& Live, const TCHAR* Field) {
					TMap<FString, int32> ExplicitOwners;
					for (int32 Index = 0; Index < Desired.Num(); ++Index)
					{
						auto& Folder = Desired[Index];
						const auto* Original = Before.Folders->FindByPredicate([&](const auto& Row) { return Row.Ref == Folder.Ref; });
						const bool bChanged = !Original || !SameSet(Original->*Member, Folder.*Member);
						const auto& Members = Folder.*Member;
						for (int32 MemberIndex = 0; MemberIndex < Members.Num(); ++MemberIndex)
						{
							const auto& Ref = Members[MemberIndex];
							const FString Pointer = FString::Printf(TEXT("/%s/%d"), Field, MemberIndex);
							if (Ref.bNew) return RejectReference(Folder, TEXT("Folder members must resolve to existing bindings or root tracks before containment is prepared."), Pointer, Ref);
							if (!Live.Contains(Ref.Key) && (!Original || !(Original->*Member).Contains(Ref)))
								return RejectReference(Folder, TEXT("The folder member does not exist in the surviving binding or root-track set."), Pointer, Ref);
							if (bChanged && Live.Contains(Ref.Key))
							{
								if (ExplicitOwners.Contains(Ref.Key)) return RejectReference(Folder,
									TEXT("The same member was explicitly assigned to more than one folder."), Pointer, Ref);
								ExplicitOwners.Add(Ref.Key, Index);
							}
						}
					}
					TSet<FString> Assigned;
					for (int32 Index = 0; Index < Desired.Num(); ++Index)
					{
						auto& Members = Desired[Index].*Member;
						const auto InputMembers = Members;
						Members.RemoveAll([&](const auto& Ref) {
							const int32* Owner = ExplicitOwners.Find(Ref.Key);
							return !Live.Contains(Ref.Key) || (Owner && *Owner != Index);
						});
						for (const auto& Ref : Members) {
							if (Assigned.Contains(Ref.Key)) return RejectReference(Desired[Index], TEXT("The member remains assigned to more than one folder."),
								FString::Printf(TEXT("/%s/%d"), Field, InputMembers.IndexOfByKey(Ref)), Ref);
							Assigned.Add(Ref.Key);
						}
					}
					return true;
				};
				TSet<FString> LiveBindings, LiveTracks;
				for (const auto& Row : After.Bindings) LiveBindings.Add(Row.BindingRef);
				for (const auto& Row : After.RootTracks) LiveTracks.Add(Row.TrackRef);
				if (!PlanMembers(&FTimelineAnimationFolder::Bindings, LiveBindings, TEXT("bindings"))
					|| !PlanMembers(&FTimelineAnimationFolder::RootTracks, LiveTracks, TEXT("root_tracks"))) return false;
				// Attach ancestors first so a valid move that inverts an old ancestry
				// never creates a transient cycle inside native recursive folder helpers.
				while (AttachmentOrder.Num() != Desired.Num())
				{
					const int32 PreviousCount = AttachmentOrder.Num();
					for (int32 Index = 0; Index < Desired.Num(); ++Index)
					{
						if (AttachmentOrder.Contains(Index)) continue;
						const auto& Parent = Desired[Index].Parent;
						const int32 ParentIndex = Desired.IndexOfByPredicate([&](const auto& Row) { return Row.Ref == Parent; });
						if (Parent.Key.IsEmpty() || AttachmentOrder.Contains(ParentIndex)) AttachmentOrder.Add(Index);
					}
					if (PreviousCount == AttachmentOrder.Num()) {
						Error = FDiagnostic::Failure(TEXT("timeline_animation_folder_change_invalid"), TEXT("The validated folder parents could not be ordered for native attachment."), TEXT("timeline_prepare"));
						Error.ExpectedValue = MakeShared<FJsonValueNumber>(Desired.Num());
						Error.ActualValue = MakeShared<FJsonValueNumber>(AttachmentOrder.Num());
						Error.SourceKind = TEXT("native_result"); return false;
					}
				}
				After.Folders = Desired;
				return true;
			}

			bool Apply(UMovieScene& Scene, FMovieSceneCreationState* CreationState,
				FUeAgentTimelineAnimationExecutionError& Error)
			{
				if (!bActive) return true;
				TMap<FString, UMovieSceneFolder*> Created;
				for (const auto& Row : Desired)
					if (Row.Ref.bNew)
					{
						auto* Folder = NewObject<UMovieSceneFolder>(&Scene, NAME_None, RF_Transactional);
						// Native name counters belong to each outer, not to the prepared
						// operation. Share creation identity with the actual application.
						if (CreationState && !CreationState->StabilizeObjectName(*Folder, TEXT("folder:") + Row.Ref.Key))
							{
							Error = FDiagnostic::Failure(TEXT("timeline_folder_identity_failed"), TEXT("The created folder could not retain its prepared native identity."), TEXT("native_execution"), Row.Ref.Key);
							Error.CandidateName = Row.Name; Error.PropertyPath = TEXT("folder_ref");
							if (Row.SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Row.SourceRecord, TEXT("folder_ref"));
							Error.SourceKind = TEXT("native_result"); return false;
						}
						Created.Add(Row.Ref.Key, Folder);
					}
				auto Resolve = [&](const FTimelineAnimationItemReference& Ref) -> UMovieSceneFolder* {
					return Ref.Key.IsEmpty() ? nullptr : (Ref.bNew ? Created.FindChecked(Ref.Key) : Existing.FindChecked(Ref.Key));
				};
				for (int32 Index = Removed.Num() - 1; Index >= 0; --Index)
				{
					auto* Folder = Removed[Index];
					for (const auto& Pair : Existing)
						if (Pair.Value->GetChildFolders().Contains(Folder)) Pair.Value->RemoveChildFolder(Folder);
					if (Scene.GetRootFolders().Contains(Folder)) Scene.RemoveRootFolder(Folder);
				}
				for (int32 Index : AttachmentOrder)
				{
					const auto& Row = Desired[Index];
					auto* Folder = Resolve(Row.Ref);
					auto* Parent = Resolve(Row.Parent);
					if (Folder->GetFolderName() != FName(*Row.Name)) { Folder->Modify(); Folder->SetFolderName(FName(*Row.Name)); }
					if (Parent)
					{
						if (!Parent->GetChildFolders().Contains(Folder)) Parent->AddChildFolder(Folder);
					}
					else if (!Scene.GetRootFolders().Contains(Folder))
					{
						for (const auto& Pair : Existing)
							if (Pair.Value->GetChildFolders().Contains(Folder)) Pair.Value->RemoveChildFolder(Folder);
						for (const auto& Pair : Created)
							if (Pair.Value->GetChildFolders().Contains(Folder)) Pair.Value->RemoveChildFolder(Folder);
						Scene.AddRootFolder(Folder);
					}
					TArray<FGuid> BindingIds;
					for (const auto& Ref : Row.Bindings) { FGuid Id; FGuid::ParseExact(Ref.Key, EGuidFormats::Digits, Id); BindingIds.Add(Id); }
					const auto OldBindings = Folder->GetChildObjectBindings();
					for (const auto& Id : OldBindings) if (!BindingIds.Contains(Id)) Folder->RemoveChildObjectBinding(Id);
					for (const auto& Id : BindingIds) if (!Folder->GetChildObjectBindings().Contains(Id)) Folder->AddChildObjectBinding(Id);
					TArray<UMovieSceneTrack*> Tracks;
					for (const auto& Ref : Row.RootTracks)
						for (auto* Track : Scene.GetTracks()) if (Ref.Key == TEXT("root_track:") + Track->GetName()) Tracks.Add(Track);
					const auto OldTracks = Folder->GetChildTracks();
					for (auto* Track : OldTracks) if (!Tracks.Contains(Track)) Folder->RemoveChildTrack(Track);
					for (auto* Track : Tracks) if (!Folder->GetChildTracks().Contains(Track)) Folder->AddChildTrack(Track);
				}
				return true;
			}
		};
	}

	bool FMovieSceneCreationState::StabilizeObjectName(UObject& Object, const FString& LogicalKey)
	{
		FName& Name = ObjectNames.FindOrAdd(LogicalKey);
		if (Name.IsNone()) Name = FName(*(TEXT("Authored_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		return Object.GetFName() == Name || Object.Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	}

	bool TryRate(const FTimelineAnimationRate& Rate, FFrameRate& OutRate,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		const StateSemantic::FUeAgentTemporalRate TemporalRate(Rate.Numerator, Rate.Denominator);
		if (!TemporalRate.IsValid()) {
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("timeline_animation_native_rate_invalid"), TemporalRate.GetValidationDiagnostic(), TEXT("native_execution"));
			OutError.SemanticPointer = TEXT("/") + OutError.PropertyPath;
			return false;
		}
		int64 Numerator = 0;
		int64 Denominator = 0;
		const bool bNumeratorInvalid = !StateSemantic::TryParseInt64Decimal(Rate.Numerator, Numerator) || Numerator > MAX_int32;
		if (bNumeratorInvalid || !StateSemantic::TryParseInt64Decimal(Rate.Denominator, Denominator) || Denominator > MAX_int32) {
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_native_rate_invalid"), bNumeratorInvalid ? TEXT("/numerator") : TEXT("/denominator"),
				MakeShared<FJsonValueString>(TEXT("positive integer <= 2147483647")), MakeShared<FJsonValueString>(bNumeratorInvalid ? Rate.Numerator : Rate.Denominator), TEXT("native_execution"), TEXT("The rate component exceeds the native frame-rate integer range."));
			return false;
		}
		OutRate = FFrameRate(static_cast<int32>(Numerator), static_cast<int32>(Denominator));
		return true;
	}

	bool TryPointTick(const FTimelineAnimationPoint& Point,
		const FString& ClockId, const FFrameRate& Rate, int64& OutTick,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		FFrameRate PointRate;
		if (!TryRate(Point.Rate, PointRate, OutError)) return Json::PrefixChildFailure(nullptr, TEXT("/rate"), OutError);
		if (Point.ClockId != ClockId) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_c4_exact_time_mismatch"), TEXT("The point clock does not match the target animation clock."), TEXT("native_execution"));
			OutError.SemanticPointer = TEXT("/clock_id");
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ClockId);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Point.ClockId);
			return false;
		}
		if (PointRate != Rate) {
			auto RateValue = [](const FFrameRate& Value) { auto Json = MakeShared<FJsonObject>(); Json->SetNumberField(TEXT("numerator"), Value.Numerator); Json->SetNumberField(TEXT("denominator"), Value.Denominator); return MakeShared<FJsonValueObject>(Json); };
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_c4_exact_time_mismatch"), TEXT("/rate"), RateValue(Rate), RateValue(PointRate), TEXT("native_execution"), TEXT("The point rate must exactly match the target animation rate."));
			return false;
		}
		if (!StateSemantic::TryParseInt64Decimal(Point.Tick, OutTick)) {
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_c4_exact_time_mismatch"), TEXT("/tick"), MakeShared<FJsonValueString>(TEXT("decimal integer representable as int64")), MakeShared<FJsonValueString>(Point.Tick), TEXT("native_execution"), TEXT("The point tick cannot be represented as a native integer."));
			return false;
		}
		return true;
	}

	bool TrySectionRange(const FTimelineAnimationRange& Value, const FString& ClockId,
		const FFrameRate& Rate, TRange<FFrameNumber>& OutRange,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		if (!Value.IsValid() || Value.ClockId != ClockId)
		{
			const TCHAR* Field = Value.State != TEXT("authored") ? TEXT("/state") : Value.ClockId.TrimStartAndEnd().IsEmpty() || Value.ClockId != ClockId ? TEXT("/clock_id") : nullptr;
			if (Field) {
				const bool bState = Value.State != TEXT("authored");
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_level_sequence_section_range_invalid"), Field,
					MakeShared<FJsonValueString>(bState ? FString(TEXT("authored")) : ClockId), MakeShared<FJsonValueString>(bState ? Value.State : Value.ClockId), TEXT("native_execution"),
					bState ? TEXT("The range must have authored state.") : TEXT("The range clock must match the target movie-scene clock."));
				if (!bState) { OutError.ExpectedEvidence = MoveTemp(OutError.ExpectedValue); OutError.ActualEvidence = MoveTemp(OutError.ActualValue); }
				return false;
			}
			// This layer receives typed ranges as well as parsed inputs. Preserve
			// the invalid bound's concrete fields without encoding/reparsing it.
			const bool bLower = !Value.Lower.IsValid();
			const bool bUpper = !Value.Upper.IsValid();
			if (bLower || bUpper) {
				const auto& Invalid = bLower ? Value.Lower : Value.Upper;
				const FString Path = bLower ? TEXT("/lower") : TEXT("/upper");
				if (Invalid.Kind != TEXT("open") && Invalid.Kind != TEXT("inclusive") && Invalid.Kind != TEXT("exclusive"))
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_level_sequence_section_range_invalid"), Path + TEXT("/kind"), MakeShared<FJsonValueString>(TEXT("open, inclusive or exclusive")), MakeShared<FJsonValueString>(Invalid.Kind), TEXT("native_execution"), TEXT("The range bound kind is unsupported."));
				else if (Invalid.Kind == TEXT("open") || !Invalid.Point.IsSet()) {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_section_range_invalid"), Invalid.Kind == TEXT("open") ? TEXT("An open range bound must omit its point.") : TEXT("A closed range bound requires a point."), TEXT("native_execution"));
					OutError.SemanticPointer = Path + TEXT("/point");
					OutError.ExpectedValuePresent = Invalid.Kind != TEXT("open");
					OutError.ActualValuePresent = Invalid.Point.IsSet();
				} else if (Invalid.Point->State != TEXT("authored"))
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_level_sequence_section_range_invalid"), Path + TEXT("/point/state"), MakeShared<FJsonValueString>(TEXT("authored")), MakeShared<FJsonValueString>(Invalid.Point->State), TEXT("native_execution"), TEXT("The bound point must have authored state."));
				else {
					const StateSemantic::FUeAgentTemporalRate TemporalRate(Invalid.Point->Rate.Numerator, Invalid.Point->Rate.Denominator);
					const auto Point = Invalid.Point->ToTemporalPoint();
					OutError = !TemporalRate.IsValid() ? TemporalRate.GetValidationDiagnostic() : Point.GetValidationDiagnostic();
					OutError.SemanticPointer = Path + TEXT("/point/") + (!TemporalRate.IsValid() ? TEXT("rate/") : TEXT("")) + OutError.PropertyPath;
				}
				return false;
			}
			for (const auto& Item : {TPair<const TCHAR*, const FTimelineAnimationBound*>(TEXT("lower"), &Value.Lower), {TEXT("upper"), &Value.Upper}}) {
				if (Item.Value->Point.IsSet() && Item.Value->Point->ClockId != Value.ClockId) {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_section_range_invalid"), TEXT("The bound point clock does not match the range clock."), TEXT("native_execution"));
					OutError.SemanticPointer = FString(TEXT("/")) + Item.Key + TEXT("/point/clock_id");
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Value.ClockId);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Item.Value->Point->ClockId);
					return false;
				}
			}
			auto PointValue = [](const FTimelineAnimationPoint& Point) { auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("tick"), Point.Tick); Result->SetStringField(TEXT("rate_numerator"), Point.Rate.Numerator); Result->SetStringField(TEXT("rate_denominator"), Point.Rate.Denominator); return MakeShared<FJsonValueObject>(Result); };
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_level_sequence_section_range_invalid"), TEXT("/upper/point"), PointValue(Value.Lower.Point.GetValue()), PointValue(Value.Upper.Point.GetValue()), TEXT("native_execution"), TEXT("The upper bound must not precede the lower bound after exact rate conversion; expected_value gives the lower bound."));
			return false;
		}
		auto Bound = [&](const FTimelineAnimationBound& In, const TCHAR* Name, TRangeBound<FFrameNumber>& Out) {
			if (In.Kind == TEXT("open")) { Out = TRangeBound<FFrameNumber>::Open(); return true; }
			int64 Tick = 0;
			if (!TryPointTick(*In.Point, ClockId, Rate, Tick, OutError)) return Json::PrefixChildFailure(nullptr, FString(TEXT("/")) + Name + TEXT("/point"), OutError);
			if (Tick < MIN_int32 || Tick > MAX_int32) {
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_level_sequence_section_range_invalid"), FString(TEXT("/")) + Name + TEXT("/point/tick"), MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]")), MakeShared<FJsonValueString>(In.Point->Tick), TEXT("native_execution"), TEXT("The bound tick exceeds the native movie-scene frame-number range."));
				return false;
			}
			Out = In.Kind == TEXT("inclusive") ? TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(static_cast<int32>(Tick)))
				: TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(static_cast<int32>(Tick)));
			return true;
		};
		TRangeBound<FFrameNumber> Lower, Upper;
		if (!Bound(Value.Lower, TEXT("lower"), Lower) || !Bound(Value.Upper, TEXT("upper"), Upper)) return false;
		OutRange = TRange<FFrameNumber>(Lower, Upper);
		return true;
	}

	bool TrySetMovieSceneSectionRange(UMovieSceneSection& Section,
		const TRange<FFrameNumber>& Range, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (Section.GetRange() == Range) return true;
		UMovieScene* Scene = Section.GetTypedOuter<UMovieScene>();
		const bool bLocked = Section.IsLocked();
		const bool bSceneReadOnly = Scene && Scene->IsReadOnly();
		// UE's SetRange silently skips writes when TryModify sees an editor lock.
		// Like native retiming, temporarily lift that UI state, then restore it.
		if (bSceneReadOnly) Scene->SetReadOnly(false);
		if (bLocked) Section.SetIsLocked(false);
		ON_SCOPE_EXIT
		{
			if (bLocked) Section.SetIsLocked(true);
			if (bSceneReadOnly) Scene->SetReadOnly(true);
		};
		Section.SetRange(Range);
		if (Section.GetRange() != Range)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_section_range_apply_failed"), TEXT("The native section range differs from the range just applied."), TEXT("authoring"), Section.GetPathName());
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = Section.GetClass()->GetName();
			const auto ActualRange = Section.GetRange();
			const bool bLower = Range.GetLowerBound() != ActualRange.GetLowerBound();
			const auto Expected = bLower ? Range.GetLowerBound() : Range.GetUpperBound();
			const auto Actual = bLower ? ActualRange.GetLowerBound() : ActualRange.GetUpperBound();
			const auto Kind = [](const TRangeBound<FFrameNumber>& Bound) { return Bound.IsOpen() ? FString(TEXT("open")) : Bound.IsInclusive() ? FString(TEXT("inclusive")) : FString(TEXT("exclusive")); };
			OutError.PropertyPath = bLower ? TEXT("range.lower") : TEXT("range.upper");
			if (Kind(Expected) != Kind(Actual)) {
				OutError.PropertyPath += TEXT(".kind");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Kind(Expected));
				OutError.ActualValue = MakeShared<FJsonValueString>(Kind(Actual));
			} else {
				OutError.PropertyPath += TEXT(".tick");
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetValue().Value);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Actual.GetValue().Value);
			}
			return false;
		}
		return true;
	}

	void FPreparedMovieSceneSettings::Apply(UMovieScene& MovieScene) const
	{
		if (!HasChanges()) return;
		MovieScene.Modify();
		if (DisplayRate.IsSet()) MovieScene.SetDisplayRate(*DisplayRate);
		if (PlaybackRange.IsSet()) MovieScene.SetPlaybackRange(*PlaybackRange);
	}

	bool TryPrepareMovieSceneSettings(const UMovieScene& MovieScene, const FString& ClockId,
		const TOptional<FTimelineAnimationRate>& DisplayRate, const TOptional<FTimelineAnimationRange>& PlaybackRange,
		FPreparedMovieSceneSettings& OutSettings, FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutSettings = {};
		if (DisplayRate.IsSet())
		{
			FFrameRate Rate;
			if (!TryRate(*DisplayRate, Rate, OutError)) return Json::PrefixChildFailure(nullptr, TEXT("/display_rate"), OutError);
			if (Rate != MovieScene.GetDisplayRate()) OutSettings.DisplayRate = Rate;
		}
		if (PlaybackRange.IsSet())
		{
			if (PlaybackRange->Lower.Kind == TEXT("open") || PlaybackRange->Upper.Kind == TEXT("open"))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_playback_range_closed_required"), PlaybackRange->Lower.Kind == TEXT("open") ? TEXT("/playback_range/lower/kind") : TEXT("/playback_range/upper/kind"), MakeShared<FJsonValueString>(TEXT("inclusive or exclusive")), MakeShared<FJsonValueString>(TEXT("open")), TEXT("native_execution"), TEXT("A movie-scene playback range requires two closed bounds."));
				return false;
			}
			TRange<FFrameNumber> Range;
			if (!TrySectionRange(*PlaybackRange, ClockId, MovieScene.GetTickResolution(), Range, OutError)) return Json::PrefixChildFailure(nullptr, TEXT("/playback_range"), OutError);
			if (Range.GetUpperBoundValue() < Range.GetLowerBoundValue())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_playback_range_invalid"), TEXT("/playback_range/upper/point/tick"), MakeShared<FJsonValueNumber>(Range.GetLowerBoundValue().Value), MakeShared<FJsonValueNumber>(Range.GetUpperBoundValue().Value), TEXT("native_execution"), TEXT("The playback upper tick must be at least the lower tick; expected_value gives the minimum."));
				return false;
			}
			if (Range.GetUpperBound().IsInclusive())
			{
				if (Range.GetUpperBoundValue().Value == MAX_int32)
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_playback_range_invalid"), TEXT("/playback_range/upper/point/tick"), MakeShared<FJsonValueString>(TEXT("inclusive upper tick <= 2147483646")), MakeShared<FJsonValueNumber>(Range.GetUpperBoundValue().Value), TEXT("native_execution"), TEXT("The inclusive upper tick cannot be converted to a native exclusive bound without overflow."));
					return false;
				}
				Range.SetUpperBound(TRangeBound<FFrameNumber>::Exclusive(Range.GetUpperBoundValue() + 1));
			}
			if (Range != MovieScene.GetPlaybackRange()) OutSettings.PlaybackRange = Range;
		}
		return true;
	}

	bool TryQueryMovieSceneTrackCandidates(const FString& Target, const FString& Query, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UMovieSceneSequence* Sequence = LoadObject<UMovieSceneSequence>(nullptr, *Target);
		if (!Sequence || !Sequence->GetMovieScene()) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_track_candidate_target_unresolved"), !Sequence ? TEXT("The requested target could not be loaded as a movie-scene sequence.") : TEXT("The sequence has no movie scene for track discovery."), TEXT("authoring"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Target;
			return false;
		}
		return TryQueryMovieSceneTrackCandidates(*Sequence->GetMovieScene(), Query, OutPayload, OutError);
	}

	bool TryQueryMovieSceneTrackCandidates(const UMovieScene& Scene, const FString& Query, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError,
		const UObject* PropertyObject, const FString& PropertyPath)
	{
		const FProperty* Property = PropertyObject && !PropertyPath.IsEmpty()
			? FTrackInstancePropertyBindings::FindProperty(PropertyObject, PropertyPath) : nullptr;
		if (!PropertyPath.IsEmpty() && !Property) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_track_property_unresolved"), !PropertyObject ? TEXT("Property track discovery requires a property owner object.") : TEXT("The requested property path does not resolve on the track owner."), TEXT("authoring"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = PropertyPath;
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
			if (PropertyObject) OutError.CandidateName = PropertyObject->GetClass()->GetName();
			return false;
		}
		TArray<ContentDiscovery::FNameSelectionEntry> Entries;
		TArray<FTrackClassCandidate> Candidates;
		if (!CollectTrackClasses(Scene, Property, Candidates, OutError)) return false;
		for (const auto& Candidate : Candidates)
			if (Query.IsEmpty() || Candidate.Selection.Name.Contains(Query) || Candidate.Class->GetName().Contains(Query))
				Entries.Add(Candidate.Selection);
		Entries.StableSort([](const auto& A, const auto& B) { return A.Name < B.Name; });
		return ContentDiscovery::TryBuildCandidateNamesOnlyPayload(Entries, ContentDiscovery::ENameComparison::CaseSensitive, OutPayload, OutError);
	}

	bool TryUpdateMovieSceneValues(
		UMovieScene& MovieScene, const FTimelineAnimationStructure& Current,
		const FTimelineAnimationStructure& RequestedStructure, FUeAgentTimelineAnimationExecutionError& OutError,
		UMovieSceneSequence* OwnerSequence, const FMovieSceneBindingResolver& ResolveBinding,
		FMovieSceneCreationState* CreationState, const FMovieSceneBindingEditPreparer& PrepareBindingEdit)
	{
		FTimelineAnimationStructure Structure = RequestedStructure;
		FPreparedMovieSceneFolders FolderPlan;
		if (!ResolveRemovedBindingContext(MovieScene, Current, Structure, OutError)) return false;
		if (!FolderPlan.Prepare(MovieScene, Current, Structure, OutError)) return false;
		auto SameRate = [](const FTimelineAnimationRate& A, const FTimelineAnimationRate& B) {
			return A.Numerator == B.Numerator && A.Denominator == B.Denominator;
		};
		auto SamePoint = [&](const FTimelineAnimationPoint& A, const FTimelineAnimationPoint& B) {
			return A.ClockId == B.ClockId && A.Tick == B.Tick && A.State == B.State && SameRate(A.Rate, B.Rate);
		};
		auto SameBound = [&](const FTimelineAnimationBound& A, const FTimelineAnimationBound& B) {
			return A.Kind == B.Kind && A.Point.IsSet() == B.Point.IsSet()
				&& (!A.Point.IsSet() || SamePoint(*A.Point, *B.Point));
		};
		auto SameRange = [&](const FTimelineAnimationRange& A, const FTimelineAnimationRange& B) {
			return A.ClockId == B.ClockId && A.State == B.State
				&& SameBound(A.Lower, B.Lower) && SameBound(A.Upper, B.Upper);
		};
		UE::UAI::Contract::FDiagnostic PropertyError;
		TSharedPtr<FJsonObject> FailureSource;
		FString FailureContext;
		auto RejectStructure = [&](const TCHAR* Reason, const FString& Pointer, const FString& Expected,
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonObject>& Source = nullptr) {
			PropertyError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_existing_structure_change_unsupported"), Pointer,
				MakeShared<FJsonValueString>(Expected), Actual, TEXT("timeline_prepare"), Reason);
			const auto Input = Source ? Source : FailureSource;
			if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(PropertyError, *Input);
			return false;
		};
		auto RejectReference = [&](const TCHAR* Reason, const FString& Pointer, const FString& Reference,
			const TSharedPtr<FJsonObject>& Source = nullptr) {
			RejectStructure(Reason, Pointer, TEXT("valid unambiguous reference"), nullptr, Source);
			PropertyError.ActualValuePresent = !Reference.IsEmpty();
			PropertyError.ActualEvidence = MakeShared<FJsonValueString>(Reference);
			return false;
		};
		auto Unsupported = [&]() {
			auto Diagnostic = PropertyError.IsEmpty()
                ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_existing_structure_change_unsupported"),
                    TEXT("The requested change to the existing timeline structure is not supported."), TEXT("timeline_prepare"))
                : PropertyError;
			if (FailureSource) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *FailureSource);
			if (Diagnostic.Source.SourceFile.IsEmpty() && !FailureContext.IsEmpty()) Diagnostic.PropertyPath = FailureContext
				+ (Diagnostic.PropertyPath.IsEmpty() ? FString() : TEXT(".") + Diagnostic.PropertyPath);
			return Fail(OutError, MoveTemp(Diagnostic));
		};
		FFrameRate RequestedTickResolution;
		if (!TryRate(Structure.TickResolution, RequestedTickResolution, OutError)) return false;
		FPreparedMovieSceneSettings Settings;
		if (!TryPrepareMovieSceneSettings(MovieScene, Current.PlaybackRange.ClockId, Structure.DisplayRate,
			Structure.PlaybackRange, Settings, OutError)) return false;
		ScalarChannel::FChannelEdits KeyEdits;
		TArray<ScalarChannel::FChannelEdits> AddedKeyEdits;
		TArray<UeAgentAuthoringObjectApply::FPreparedApply> PropertyEdits;
		auto PrepareProperties = [&](UObject& Object, const TSharedPtr<FJsonObject>& Properties) {
			if (!Properties) return true;
			return Composition::HjsonImport::PrepareReflectedObjectRecord(Object, MovieSceneObjectPropertyPolicy(),
				Properties, PropertyEdits.AddDefaulted_GetRef(), PropertyError);
		};
		auto NativeFailure = [&](const TCHAR* Code, const TCHAR* Reason, const TSharedPtr<FJsonObject>& Source,
			const FString& Field, const FString& Candidate, const FString& Evidence) {
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"), Evidence);
			OutError.PropertyPath = Field; OutError.CandidateName = Candidate;
			if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Source, Field);
			OutError.SourceKind = TEXT("native_result"); return false;
		};
		struct FSectionChange
		{
			UMovieSceneTrack* Track;
			UMovieSceneSection* Section;
			TRange<FFrameNumber> Range;
			TArray<FTimelineAnimationChannel> Channels;
			TSharedPtr<FJsonObject> Properties;
			const FTimelineAnimationSection* Desired;
		};
		TArray<FSectionChange> SectionAdditions, SectionUpdates;
		TArray<TPair<UMovieSceneTrack*, UMovieSceneSection*>> SectionRemovals;
		struct FTrackRemoval { UMovieSceneTrack* Track; const FTimelineAnimationTrack* Before; };
		TArray<FTrackRemoval> TrackRemovals;
		TArray<FGuid> BindingRemovals;
		TMap<FGuid, const FTimelineAnimationBinding*> RemovedBindingSources;
		struct FBindingAddition { const FTimelineAnimationBinding* Desired; FMovieSceneObjectBinding Binding; };
		TArray<FBindingAddition> BindingAdditions;
		TArray<TPair<UMovieSceneNameableTrack*, FString>> TrackRenames;
		struct FTrackAddition { UClass* Class; FGuid Binding; const FTimelineAnimationTrack* Desired; FProperty* Property; };
		TArray<FTrackAddition> TrackAdditions;
		auto ValidateNativeChannels = [&](UMovieSceneSection& Section, const TSharedPtr<FJsonValue>& Desired) {
			if (!Desired) return true;
			TArray<TSharedPtr<FJsonValue>> Context;
			if (!ScalarChannel::VisitSupported(Section, [](auto&, const FString&, const FString&, const FString&) { return true; }, &Context, &PropertyError)) return false;
			const auto Compared = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(MakeShared<FJsonValueArray>(Context), Desired);
			if (Compared.bEquivalent) return true;
			PropertyError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_native_channels_context_read_only"),
				TEXT("Native channel context is read-only."));
			Compared.WithPrefix(TEXT("/native_channels")).ApplyTo(PropertyError);
			return false;
		};
		auto ParseRange = [&](const FTimelineAnimationRange& Range, TRange<FFrameNumber>& OutRange) {
			const bool bValid = TrySectionRange(Range, Current.PlaybackRange.ClockId,
				(&MovieScene)->GetTickResolution(), OutRange, OutError);
			if (!bValid) PropertyError = OutError;
			return bValid;
		};
		auto PrepareChannels = [&](UMovieSceneSection& Section,
			const TArray<FTimelineAnimationChannel>& Before, const TArray<FTimelineAnimationChannel>& After,
			ScalarChannel::FChannelEdits& Edits) {
			TMap<FString, const FTimelineAnimationChannel*> BeforeByRef, AfterByRef;
			BeforeByRef.Reserve(Before.Num());
			AfterByRef.Reserve(After.Num());
			for (const auto& Channel : Before)
				if (!BeforeByRef.Contains(Channel.ChannelRef)) BeforeByRef.Add(Channel.ChannelRef, &Channel);
			for (const auto& Channel : After)
				if (!AfterByRef.Contains(Channel.ChannelRef)) AfterByRef.Add(Channel.ChannelRef, &Channel);
			TSet<FString> Seen;
			const FString SectionContext = FailureContext;
			const bool bValid = ScalarChannel::VisitSupported(Section, [&](auto& Channel, const FString& Ref, const FString& Name, const FString& Kind) {
				FailureContext = SectionContext + TEXT(".channels.") + Name;
				const auto* DesiredEntry = AfterByRef.Find(Ref);
				const auto* OriginalEntry = BeforeByRef.Find(Ref);
				const auto* Desired = DesiredEntry ? *DesiredEntry : nullptr;
				const auto* Original = OriginalEntry ? *OriginalEntry : nullptr;
				if (!Desired) return RejectReference(TEXT("A native channel is absent from the requested channel list."), TEXT("/channels"), Ref);
				if (Desired->Name != Name) return RejectStructure(TEXT("The channel name does not match its native channel."), TEXT("/name"), Name, MakeShared<FJsonValueString>(Desired->Name), Desired->SourceRecord);
				if (Desired->ChannelKind != Kind) return RejectStructure(TEXT("The channel kind does not match its native channel."), TEXT("/channel_kind"), Kind, MakeShared<FJsonValueString>(Desired->ChannelKind), Desired->SourceRecord);
				if (Seen.Contains(Ref)) return RejectReference(TEXT("More than one native channel uses the same reference."), TEXT("/channel_ref"), Ref, Desired->SourceRecord);
				if (!Before.IsEmpty() && !Original) return RejectReference(TEXT("The native channel was absent from the captured channel list."), TEXT("/channel_ref"), Ref, Desired->SourceRecord);
				Seen.Add(Ref);
				for (const auto& Key : Desired->Keys) {
					if (Key.Time.State != TEXT("authored")) return RejectStructure(TEXT("A timeline key time must be authored."), TEXT("/time/state"), TEXT("authored"), MakeShared<FJsonValueString>(Key.Time.State), Key.SourceRecord);
					if (Key.Time.ClockId != Current.PlaybackRange.ClockId) {
						RejectReference(TEXT("The key time belongs to a different timeline clock."), TEXT("/time/clock_id"), Key.Time.ClockId, Key.SourceRecord);
						PropertyError.ExpectedEvidence = MakeShared<FJsonValueString>(Current.PlaybackRange.ClockId);
						return false;
					}
					if (!SameRate(Key.Time.Rate, Current.TickResolution)) {
						const bool bNumerator = Key.Time.Rate.Numerator != Current.TickResolution.Numerator;
						return RejectStructure(TEXT("The key time rate does not match the captured timeline tick resolution."),
							bNumerator ? TEXT("/time/rate/numerator") : TEXT("/time/rate/denominator"),
							bNumerator ? Current.TickResolution.Numerator : Current.TickResolution.Denominator,
							MakeShared<FJsonValueString>(bNumerator ? Key.Time.Rate.Numerator : Key.Time.Rate.Denominator), Key.SourceRecord);
					}
				}
				const bool bPrepared = Edits.PrepareSettings(Channel, Section, *Desired)
					&& Edits.Prepare(Channel, Section, Original ? Original->Keys : TArray<FTimelineAnimationKey>{}, Desired->Keys, Desired->SourceRecord);
				if (!bPrepared) PropertyError = Edits.ErrorDetail;
				return bPrepared;
			}, nullptr, &PropertyError);
			if (bValid) FailureContext = SectionContext;
			if (!bValid) return false;
			if (Seen.Num() != After.Num()) return RejectStructure(TEXT("The requested channel list contains entries that do not match native channels."), TEXT("/channels"), FString::FromInt(Seen.Num()), MakeShared<FJsonValueNumber>(After.Num()));
			if (!Before.IsEmpty() && Seen.Num() != Before.Num()) return RejectStructure(TEXT("The captured channel count differs from the native channel count."), TEXT("/channels"), FString::FromInt(Before.Num()), MakeShared<FJsonValueNumber>(Seen.Num()));
			return true;
		};
		auto PrepareTracks = [&](const TArray<FTimelineAnimationTrack>& BeforeTracks,
			const TArray<FTimelineAnimationTrack>& AfterTracks, const auto& NativeTracks, const FGuid& Binding,
			UObject* BoundObject) -> bool
		{
			if (BeforeTracks.Num() != NativeTracks.Num()) return RejectStructure(TEXT("The native track count changed since capture."), TEXT("/tracks"), FString::FromInt(BeforeTracks.Num()), MakeShared<FJsonValueNumber>(NativeTracks.Num()));
			TMap<FString, int32> TracksByRef;
			for (int32 TrackIndex = 0; TrackIndex < BeforeTracks.Num(); ++TrackIndex)
			{
				if (BeforeTracks[TrackIndex].TrackRef.IsEmpty()
					|| TracksByRef.Contains(BeforeTracks[TrackIndex].TrackRef)) return RejectReference(TEXT("A captured track reference is empty or repeated."), TEXT("/track_ref"), BeforeTracks[TrackIndex].TrackRef, BeforeTracks[TrackIndex].SourceRecord);
				TracksByRef.Add(BeforeTracks[TrackIndex].TrackRef, TrackIndex);
			}
			TSet<FString> DesiredTrackRefs;
			for (const auto& AfterTrack : AfterTracks)
			{
				FailureSource = AfterTrack.SourceRecord;
				FailureContext = AfterTrack.DisplayName.IsEmpty() ? AfterTrack.TrackKind : AfterTrack.DisplayName;
				const FString TrackContext = FailureContext;
				const int32* TrackIndexPtr = TracksByRef.Find(AfterTrack.TrackRef);
				if (DesiredTrackRefs.Contains(AfterTrack.TrackRef)) return RejectReference(TEXT("The requested track reference is repeated."), TEXT("/track_ref"), AfterTrack.TrackRef);
				DesiredTrackRefs.Add(AfterTrack.TrackRef);
				if (!TrackIndexPtr)
				{
					FProperty* Property = BoundObject && !AfterTrack.PropertyPath.IsEmpty()
						? FTrackInstancePropertyBindings::FindProperty(BoundObject, AfterTrack.PropertyPath) : nullptr;
					if (!AfterTrack.PropertyPath.IsEmpty() && Binding.IsValid() && ResolveBinding && !Property)
					return RejectStructure(TEXT("The bound object has no property matching the requested property track."), TEXT("/property_path"), TEXT("a property of the bound object"), MakeShared<FJsonValueString>(AfterTrack.PropertyPath));
					UClass* Class = AfterTrack.SourceRecord ? ResolveTrackClass(MovieScene, *AfterTrack.SourceRecord, PropertyError, Property) : nullptr;
					if (!Class) {
						if (!PropertyError.IsEmpty()) return false;
						return RejectStructure(TEXT("The new track has no source record from which to resolve its class."), TEXT("/name"), TEXT("track creation record"));
					}
					if (!Class->IsChildOf(UMovieSceneNameableTrack::StaticClass()) && !AfterTrack.DisplayName.IsEmpty())
					return RejectStructure(TEXT("This native track class does not support changing its display name."), TEXT("/display_name"), TEXT("unchanged display name"), MakeShared<FJsonValueString>(AfterTrack.DisplayName));
					if (Class->IsChildOf(UMovieScenePropertyTrack::StaticClass()) && Binding.IsValid() && ResolveBinding && !Property)
					return RejectStructure(TEXT("The bound object has no property matching the requested property track."), TEXT("/property_path"), TEXT("a property of the bound object"), MakeShared<FJsonValueString>(AfterTrack.PropertyPath));
					// Native defaults are exported after creation for subsequent section authoring.
					if (!AfterTrack.Sections.IsEmpty()) return RejectStructure(TEXT("Create and export the new track before authoring its native sections."), TEXT("/sections"), TEXT("empty sections for initial track creation"), MakeShared<FJsonValueNumber>(AfterTrack.Sections.Num()));
					TStrongObjectPtr<UMovieSceneTrack> Template(NewObject<UMovieSceneTrack>(GetTransientPackage(), Class));
					if (!Template) return RejectStructure(TEXT("The native track template could not be created."), TEXT("/name"), Class->GetName());
					if (Property && !InitializePropertyContext(*Template, *Property)) return RejectStructure(TEXT("The track cannot initialize context for the bound property type."), TEXT("/property_path"), Class->GetName(), MakeShared<FJsonValueString>(Property->GetCPPType()));
					UeAgentAuthoringObjectApply::FPreparedApply PropertyValidation;
					if (!Composition::HjsonImport::PrepareReflectedObjectRecord(*Template, MovieSceneObjectPropertyPolicy(),
						AfterTrack.Properties, PropertyValidation, PropertyError)) return false;
					TrackAdditions.Add({Class, Binding, &AfterTrack, Property});
					continue;
				}
				const int32 TrackIndex = *TrackIndexPtr;
				const auto& BeforeTrack = BeforeTracks[TrackIndex];
				UMovieSceneTrack* NativeTrack = NativeTracks[TrackIndex];
				const FString NativeKind = ScalarChannel::Kind(NativeTrack);
				if (!NativeTrack) return RejectReference(TEXT("The captured native track no longer exists."), TEXT("/track_ref"), AfterTrack.TrackRef);
				if (BeforeTrack.TrackKind != NativeKind || AfterTrack.TrackKind != NativeKind)
					return RejectStructure(TEXT("The track kind does not match the native track."), TEXT("/track_kind"), NativeKind, MakeShared<FJsonValueString>(BeforeTrack.TrackKind != NativeKind ? BeforeTrack.TrackKind : AfterTrack.TrackKind));
				if (BeforeTrack.PropertyPath != AfterTrack.PropertyPath)
					return RejectStructure(TEXT("An existing track cannot be rebound to a different property."), TEXT("/property_path"), BeforeTrack.PropertyPath, MakeShared<FJsonValueString>(AfterTrack.PropertyPath));
				if (BeforeTrack.Sections.Num() != NativeTrack->GetAllSections().Num())
					return RejectStructure(TEXT("The native section count changed since track capture."), TEXT("/sections"), FString::FromInt(BeforeTrack.Sections.Num()), MakeShared<FJsonValueNumber>(NativeTrack->GetAllSections().Num()));
				if (AfterTrack.SourceRecord && AfterTrack.SourceRecord->HasField(TEXT("name"))) {
					UClass* RequestedClass = ResolveTrackClass(MovieScene, *AfterTrack.SourceRecord, PropertyError);
					if (!RequestedClass) return false;
					if (RequestedClass != NativeTrack->GetClass()) return RejectStructure(TEXT("An existing track cannot change its native class."), TEXT("/name"), NativeTrack->GetClass()->GetName(), MakeShared<FJsonValueString>(RequestedClass->GetName()));
				}
				if (!PrepareProperties(*NativeTrack, AfterTrack.Properties)) return false;
				if (BeforeTrack.DisplayName != AfterTrack.DisplayName)
				{
					if (AfterTrack.DisplayName.TrimStartAndEnd().IsEmpty()) return RejectStructure(TEXT("The new track display name is empty."), TEXT("/display_name"), TEXT("non-empty string"), MakeShared<FJsonValueString>(AfterTrack.DisplayName));
					auto* Nameable = Cast<UMovieSceneNameableTrack>(NativeTrack);
					if (!Nameable) return RejectStructure(TEXT("This native track class does not support changing its display name."), TEXT("/display_name"), TEXT("unchanged display name"), MakeShared<FJsonValueString>(AfterTrack.DisplayName));
					TrackRenames.Emplace(Nameable, AfterTrack.DisplayName);
				}
				TMap<FString, int32> SectionsByRef;
				for (int32 Index = 0; Index < BeforeTrack.Sections.Num(); ++Index)
				{
					if (SectionsByRef.Contains(BeforeTrack.Sections[Index].SectionRef)) return RejectReference(TEXT("The captured track contains repeated section references."), TEXT("/section_ref"), BeforeTrack.Sections[Index].SectionRef, BeforeTrack.Sections[Index].SourceRecord);
					SectionsByRef.Add(BeforeTrack.Sections[Index].SectionRef, Index);
				}
				TSet<FString> DesiredSections;
				for (const auto& AfterSection : AfterTrack.Sections)
				{
					FailureSource = AfterSection.SourceRecord;
					FailureContext = TrackContext + TEXT(".sections");
					if (AfterSection.SectionRef.IsEmpty() || DesiredSections.Contains(AfterSection.SectionRef)) return RejectReference(TEXT("A requested section reference is empty or repeated."), TEXT("/section_ref"), AfterSection.SectionRef);
					if (AfterSection.SectionKind != NativeKind) return RejectStructure(TEXT("The section kind does not match its native track."), TEXT("/section_kind"), NativeKind, MakeShared<FJsonValueString>(AfterSection.SectionKind));
					if (!AfterSection.SourceRef.IsEmpty()) return RejectReference(TEXT("This scalar timeline section does not accept an external source reference."), TEXT("/source_ref"), AfterSection.SourceRef);
					DesiredSections.Add(AfterSection.SectionRef);
					TRange<FFrameNumber> DesiredRange;
					if (!ParseRange(AfterSection.Range, DesiredRange)) return Json::PrefixChildFailure(AfterSection.SourceRecord, TEXT("/range"), PropertyError);
					const int32* ExistingIndex = SectionsByRef.Find(AfterSection.SectionRef);
					if (!ExistingIndex)
					{
						TStrongObjectPtr<UMovieSceneTrack> DefaultTrack(ScalarChannel::MakeSectionTemplateTrack(*NativeTrack));
						if (!DefaultTrack) return RejectStructure(TEXT("The track cannot provide a section template with the required native context."), TEXT("/section_kind"), TEXT("track supporting a native section factory"), MakeShared<FJsonValueString>(NativeTrack->GetClass()->GetName()));
						auto* DefaultSection = DefaultTrack->CreateNewSection();
						ScalarChannel::FChannelEdits Validation;
						if (!DefaultSection) return RejectStructure(TEXT("The native track factory did not create a default section."), TEXT("/section_kind"), NativeKind);
						if (!ValidateNativeChannels(*DefaultSection, AfterSection.NativeChannels)
							|| !PrepareChannels(*DefaultSection, {}, AfterSection.Channels, Validation)) return false;
						UeAgentAuthoringObjectApply::FPreparedApply PropertyValidation;
						if (!Composition::HjsonImport::PrepareReflectedObjectRecord(*DefaultSection, MovieSceneObjectPropertyPolicy(),
							AfterSection.Properties, PropertyValidation, PropertyError)) return false;
						SectionAdditions.Add({NativeTrack, nullptr, DesiredRange, AfterSection.Channels, AfterSection.Properties, &AfterSection});
						continue;
					}
					const int32 SectionIndex = *ExistingIndex;
					const auto& BeforeSection = BeforeTrack.Sections[SectionIndex];
					UMovieSceneSection* NativeSection = NativeTrack->GetAllSections()[SectionIndex];
					if (!NativeSection) return RejectReference(TEXT("The captured native section no longer exists."), TEXT("/section_ref"), AfterSection.SectionRef);
					if (BeforeSection.SectionRef != AfterSection.SectionRef) return RejectReference(TEXT("The existing section reference does not match the captured section."), TEXT("/section_ref"), AfterSection.SectionRef);
					if (BeforeSection.SectionKind != AfterSection.SectionKind) return RejectStructure(TEXT("An existing section cannot change its kind."), TEXT("/section_kind"), BeforeSection.SectionKind, MakeShared<FJsonValueString>(AfterSection.SectionKind));
					if (BeforeSection.SourceRef != AfterSection.SourceRef) return RejectReference(TEXT("An existing section cannot change its external source reference."), TEXT("/source_ref"), AfterSection.SourceRef);
					if (!ValidateNativeChannels(*NativeSection, AfterSection.NativeChannels)
						|| !PrepareProperties(*NativeSection, AfterSection.Properties)) return false;
					if (!SameRange(BeforeSection.Range, AfterSection.Range))
						SectionUpdates.Add({NativeTrack, NativeSection, DesiredRange, {}, nullptr, &AfterSection});
					if (!PrepareChannels(*NativeSection, BeforeSection.Channels, AfterSection.Channels, KeyEdits)) return false;
				}
				for (int32 Index = 0; Index < BeforeTrack.Sections.Num(); ++Index)
					if (!DesiredSections.Contains(BeforeTrack.Sections[Index].SectionRef))
						SectionRemovals.Emplace(NativeTrack, NativeTrack->GetAllSections()[Index]);
			}
			for (int32 TrackIndex = 0; TrackIndex < BeforeTracks.Num(); ++TrackIndex)
				if (!DesiredTrackRefs.Contains(BeforeTracks[TrackIndex].TrackRef))
					TrackRemovals.Add({NativeTracks[TrackIndex], &BeforeTracks[TrackIndex]});
			return true;
		};
		UMovieScene* Scene = (&MovieScene);
		if (!PrepareTracks(Current.RootTracks, Structure.RootTracks, Scene->GetTracks(), FGuid(), nullptr)) return Unsupported();
		FailureSource.Reset(); FailureContext.Reset();
		TMap<FString, const FTimelineAnimationBinding*> BindingsByRef;
		for (const auto& Before : Current.Bindings)
		{
			if (Before.BindingRef.IsEmpty() || BindingsByRef.Contains(Before.BindingRef)) { RejectReference(TEXT("A captured binding reference is empty or repeated."), TEXT("/binding_ref"), Before.BindingRef, Before.SourceRecord); return Unsupported(); }
			BindingsByRef.Add(Before.BindingRef, &Before);
		}
		TSet<FString> DesiredBindings;
		struct FPreparedBindingEdit { FMovieSceneBindingEdit Edit; const FTimelineAnimationBinding* Desired; };
		TArray<FPreparedBindingEdit> BindingEdits;
		TArray<TPair<FGuid, FName>> TagsToAdd, TagsToRemove;
		for (const auto& After : Structure.Bindings)
		{
			FailureSource = After.SourceRecord; FailureContext = After.BindingKind;
			const auto* Found = BindingsByRef.Find(After.BindingRef);
			FGuid BindingGuid;
			if (DesiredBindings.Contains(After.BindingRef)) { RejectReference(TEXT("The requested binding reference is repeated."), TEXT("/binding_ref"), After.BindingRef); return Unsupported(); }
			DesiredBindings.Add(After.BindingRef);
			if ((After.Properties.IsValid() || After.TemplateProperties.IsValid()) && !PrepareBindingEdit)
				{ RejectStructure(TEXT("This timeline has no binding property editor."), TEXT("/properties"), TEXT("binding property edit support"), nullptr); PropertyError.Code = TEXT("timeline_binding_properties_unsupported"); return Unsupported(); }
			if (After.Tags.IsSet())
				for (const FString& Name : After.Tags.GetValue())
					if (Name.IsEmpty()) { RejectStructure(TEXT("A binding tag name cannot be empty."), TEXT("/tags"), TEXT("non-empty tag name"), MakeShared<FJsonValueString>(Name)); PropertyError.Code = TEXT("timeline_animation_binding_tag_name_invalid"); return Unsupported(); }
			if (!Found)
			{
				FMovieSceneObjectBinding Binding;
				if (!OwnerSequence || !ResolveBinding) { RejectStructure(!OwnerSequence ? TEXT("Binding creation requires an owning sequence.") : TEXT("Binding creation has no object resolver."), TEXT("/binding_kind"), TEXT("sequence with binding support"), MakeShared<FJsonValueString>(After.BindingKind)); return Unsupported(); }
				if (!ResolveBinding(After, Binding, PropertyError)) {
					if (PropertyError.IsEmpty()) RejectStructure(TEXT("The binding resolver reported failure without a diagnostic."), TEXT("/object_ref"), TEXT("resolvable object or class"));
					return Unsupported();
				}
				if (!Binding.Object && !Binding.BoundClass) { RejectStructure(TEXT("The binding resolver returned neither an object nor a class."), TEXT("/object_ref"), TEXT("resolved object or class")); return Unsupported(); }
				if (!After.Tracks.IsEmpty())
					{ RejectStructure(TEXT("Create and export the new binding before authoring its tracks."), TEXT("/tracks"), TEXT("empty tracks for initial binding creation"), MakeShared<FJsonValueNumber>(After.Tracks.Num())); PropertyError.Code = TEXT("timeline_new_binding_export_before_tracks"); return Unsupported(); }
				BindingAdditions.Add({&After, Binding});
				continue;
			}
			if (!FGuid::ParseExact(After.BindingRef, EGuidFormats::Digits, BindingGuid)) { RejectReference(TEXT("The existing binding reference is not a native binding identifier."), TEXT("/binding_ref"), After.BindingRef); return Unsupported(); }
			const auto& Before = **Found;
			if (After.Properties.IsValid() || After.TemplateProperties.IsValid())
			{
				FMovieSceneBindingEdit Edit;
				if (!PrepareBindingEdit(After, Edit, PropertyError)) {
					if (PropertyError.IsEmpty()) RejectStructure(TEXT("The binding editor could not prepare the requested properties and returned no diagnostic."), TEXT("/properties"), TEXT("prepared binding properties"));
					return Unsupported();
				}
				if (Edit) BindingEdits.Add({MoveTemp(Edit), &After});
			}
			const FMovieSceneBinding* NativeBinding = Scene->FindBinding(BindingGuid);
			FMovieSceneObjectBinding BoundObject;
			const bool bNewTrack = After.Tracks.ContainsByPredicate([&](const auto& Track) {
				return !Before.Tracks.ContainsByPredicate([&](const auto& Prior) { return Prior.TrackRef == Track.TrackRef; }); });
			if (bNewTrack && ResolveBinding && !ResolveBinding(Before, BoundObject, PropertyError)) {
				if (PropertyError.IsEmpty()) RejectStructure(TEXT("The binding resolver could not resolve the existing binding and returned no diagnostic."), TEXT("/object_ref"), TEXT("resolved existing binding"));
				return Unsupported();
			}
			if (!NativeBinding) { RejectReference(TEXT("The captured native binding no longer exists."), TEXT("/binding_ref"), After.BindingRef); return Unsupported(); }
			if (Before.BindingKind != After.BindingKind) { RejectStructure(TEXT("An existing binding cannot change its kind."), TEXT("/binding_kind"), Before.BindingKind, MakeShared<FJsonValueString>(After.BindingKind)); return Unsupported(); }
			if (Before.ObjectRef != After.ObjectRef) { RejectReference(TEXT("An existing binding cannot change its bound object reference."), TEXT("/object_ref"), After.ObjectRef); PropertyError.ExpectedEvidence = MakeShared<FJsonValueString>(Before.ObjectRef); return Unsupported(); }
			if (!PrepareTracks(Before.Tracks, After.Tracks, NativeBinding->GetTracks(), BindingGuid, BoundObject.Object)) return Unsupported();
			if (After.Tags.IsSet())
			{
				if (!OwnerSequence) { RejectStructure(TEXT("Binding tags require an owning sequence."), TEXT("/tags"), TEXT("sequence-owned binding"), nullptr, After.SourceRecord); return Unsupported(); }
				TSet<FName> DesiredTags;
				for (const FString& Name : After.Tags.GetValue())
				{
					const FName Tag(*Name);
					DesiredTags.Add(Tag);
				}
				const FMovieSceneObjectBindingID Local = UE::MovieScene::FFixedObjectBindingID(BindingGuid, MovieSceneSequenceID::Root);
				TSet<FName> CurrentTags;
				for (const auto& Tag : Scene->AllTaggedBindings())
					if (Tag.Value.IDs.Contains(Local)) CurrentTags.Add(Tag.Key);
				for (const FName& Tag : CurrentTags)
					if (!DesiredTags.Contains(Tag)) TagsToRemove.Emplace(BindingGuid, Tag);
				for (const FName& Tag : DesiredTags)
					if (!CurrentTags.Contains(Tag)) TagsToAdd.Emplace(BindingGuid, Tag);
			}
		}
		for (const auto& Before : Current.Bindings)
		{
			if (DesiredBindings.Contains(Before.BindingRef)) continue;
			FailureSource = Before.SourceRecord; FailureContext = Before.BindingKind;
			FGuid BindingGuid;
			if (!FGuid::ParseExact(Before.BindingRef, EGuidFormats::Digits, BindingGuid)) { RejectReference(TEXT("The removed binding reference is not a native binding identifier."), TEXT("/binding_ref"), Before.BindingRef); return Unsupported(); }
			const FMovieSceneBinding* NativeBinding = Scene->FindBinding(BindingGuid);
			if (!NativeBinding || (!Scene->FindPossessable(BindingGuid) && !Scene->FindSpawnable(BindingGuid)) || !OwnerSequence) {
				RejectReference(!NativeBinding ? TEXT("The binding to remove no longer exists.") : !OwnerSequence ? TEXT("Binding removal requires an owning sequence.") : TEXT("The binding to remove is neither possessable nor spawnable."), TEXT("/binding_ref"), Before.BindingRef);
				return Unsupported();
			}
			BindingRemovals.Add(BindingGuid);
			RemovedBindingSources.Add(BindingGuid, &Before);
		}
		// Native deletion leaves references in unrelated sections unresolved; it
		// does not delete those sections or prohibit deleting the bound object.
		// Remove descendants before their parents, as Sequencer does.
		TArray<FGuid> PendingBindings = MoveTemp(BindingRemovals);
		while (!PendingBindings.IsEmpty())
		{
			const int32 LeafIndex = PendingBindings.IndexOfByPredicate([&](const FGuid& Candidate) {
				for (const FGuid& Other : PendingBindings)
					if (const auto* Possessable = Scene->FindPossessable(Other))
						if (Possessable->GetParent() == Candidate) return false;
				return true;
			});
			if (LeafIndex == INDEX_NONE) {
				NativeFailure(TEXT("timeline_animation_binding_parent_cycle_unsupported"), TEXT("The native bindings selected for removal contain a parent cycle."), nullptr,
					TEXT("bindings"), MovieScene.GetName(), MovieScene.GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(0);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(PendingBindings.Num());
				TArray<TSharedPtr<FJsonValue>> Ids;
				for (const auto& Id : PendingBindings) Ids.Add(MakeShared<FJsonValueString>(Id.ToString(EGuidFormats::Digits)));
				OutError.ActualEvidence = MakeShared<FJsonValueArray>(Ids); return false;
			}
			BindingRemovals.Add(PendingBindings[LeafIndex]);
			PendingBindings.RemoveAt(LeafIndex);
		}
		// All identity and shape checks precede the first native write. Preserve
		// binding GUIDs, owned objects, key handles, interpolation and tangents.
		if (!TagsToRemove.IsEmpty() || !TagsToAdd.IsEmpty()) Scene->Modify();
		for (const auto& Change : TagsToRemove)
			Scene->UntagBinding(Change.Value, UE::MovieScene::FFixedObjectBindingID(Change.Key, MovieSceneSequenceID::Root));
		for (const auto& Change : TagsToAdd)
			Scene->TagBinding(Change.Value, UE::MovieScene::FFixedObjectBindingID(Change.Key, MovieSceneSequenceID::Root));
		for (auto& Prepared : PropertyEdits)
		{
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))
				return Fail(OutError, Result.Diagnostic);
		}
		for (const auto& Rename : TrackRenames)
		{ Rename.Key->Modify(); Rename.Key->SetDisplayName(FText::FromString(Rename.Value)); }
		Settings.Apply(MovieScene);
		for (auto& Prepared : BindingEdits)
			if (!Prepared.Edit(PropertyError)) {
				if (PropertyError.IsEmpty()) return NativeFailure(TEXT("timeline_binding_edit_failed"), TEXT("The prepared binding property edit failed without a diagnostic."),
					Prepared.Desired->SourceRecord, TEXT("properties"), Prepared.Desired->BindingKind, Prepared.Desired->BindingRef);
				if (Prepared.Desired->SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(PropertyError, *Prepared.Desired->SourceRecord);
				return Fail(OutError, PropertyError);
			}
		for (const auto& Addition : BindingAdditions)
		{
			Scene->Modify();
			OwnerSequence->Modify();
			const FString& Name = Addition.Desired->BindingRef;
			UClass* BoundClass = Addition.Binding.BoundClass
				? Addition.Binding.BoundClass : Addition.Binding.Object->GetClass();
			FGuid Guid;
			if (CreationState)
			{
				FGuid& Frozen = CreationState->BindingIds.FindOrAdd(Name);
				if (!Frozen.IsValid()) Frozen = FGuid::NewGuid();
				Guid = Frozen;
				if (Scene->FindBinding(Guid)) return NativeFailure(TEXT("timeline_binding_identity_collision"), TEXT("The prepared binding identity is already used by another native binding."),
					Addition.Desired->SourceRecord, TEXT("binding_ref"), BoundClass->GetName(), Name);
				FMovieScenePossessable Possessable(Name, BoundClass);
				Possessable.SetGuid(Guid);
				Scene->AddPossessable(Possessable, FMovieSceneBinding(Guid, Name));
			}
			else Guid = Scene->AddPossessable(Name, BoundClass);
			if (!Guid.IsValid()) return NativeFailure(TEXT("timeline_binding_create_failed"), TEXT("The movie scene did not return a valid identity for the new binding."),
				Addition.Desired->SourceRecord, TEXT("binding_ref"), BoundClass->GetName(), Addition.Desired->BindingRef);
			if (Addition.Binding.Install)
			{
				if (!Addition.Binding.Install(*OwnerSequence, Guid, PropertyError)) {
					if (PropertyError.IsEmpty()) return NativeFailure(TEXT("timeline_binding_install_failed"), TEXT("The native binding installer failed without a diagnostic."),
						Addition.Desired->SourceRecord, TEXT("binding_ref"), BoundClass->GetName(), Name);
					if (Addition.Desired->SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(PropertyError, *Addition.Desired->SourceRecord);
					return Fail(OutError, PropertyError);
				}
			}
			else OwnerSequence->BindPossessableObject(Guid, *Addition.Binding.Object, Addition.Binding.Context);
			// A newly installed binding has the same editable record as an existing
			// one. Route its original properties through the domain's shared editor
			// after native creation supplies the actual identity.
			if (Addition.Desired->Properties.IsValid() || Addition.Desired->TemplateProperties.IsValid())
			{
				auto Installed = *Addition.Desired;
				Installed.BindingRef = Guid.ToString(EGuidFormats::Digits);
				FMovieSceneBindingEdit Edit;
				if (!PrepareBindingEdit(Installed, Edit, PropertyError) || (Edit && !Edit(PropertyError))) {
					if (PropertyError.IsEmpty()) return NativeFailure(TEXT("timeline_binding_edit_failed"), TEXT("The newly installed binding could not apply its properties and returned no diagnostic."),
						Addition.Desired->SourceRecord, TEXT("properties"), BoundClass->GetName(), Name);
					if (Addition.Desired->SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(PropertyError, *Addition.Desired->SourceRecord);
					return Fail(OutError, PropertyError);
				}
			}
			if (Addition.Desired->Tags.IsSet())
				for (const FString& Tag : Addition.Desired->Tags.GetValue())
					Scene->TagBinding(FName(*Tag), UE::MovieScene::FFixedObjectBindingID(Guid, MovieSceneSequenceID::Root));
		}
		for (const auto& Addition : TrackAdditions)
		{
			auto* Track = Addition.Binding.IsValid()
				? Scene->AddTrack(Addition.Class, Addition.Binding) : Scene->AddTrack(Addition.Class);
			const auto& Desired = *Addition.Desired;
			if (!Track) return NativeFailure(TEXT("timeline_track_create_failed"), TEXT("The movie scene could not create a track of the selected class."),
				Desired.SourceRecord, TEXT("name"), Addition.Class->GetName(), Desired.TrackRef);
			if (CreationState && !CreationState->StabilizeObjectName(*Track,
				Addition.Binding.ToString(EGuidFormats::Digits) + TEXT(":") + Desired.TrackRef))
				return NativeFailure(TEXT("timeline_track_identity_failed"), TEXT("The created track could not retain its prepared native identity."),
					Desired.SourceRecord, TEXT("track_ref"), Desired.DisplayName, Desired.TrackRef);
			if (auto* Nameable = Cast<UMovieSceneNameableTrack>(Track))
				Nameable->SetDisplayName(FText::FromString(Desired.DisplayName.IsEmpty() ? Desired.TrackRef : Desired.DisplayName));
			auto* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
			if (Addition.Property && PropertyTrack)
			{
				if (!InitializePropertyContext(*Track, *Addition.Property)) return NativeFailure(TEXT("timeline_track_property_context_invalid"), TEXT("The created track could not initialize its property type context."),
					Desired.SourceRecord, TEXT("property_path"), Desired.PropertyPath, Track->GetPathName());
				PropertyTrack->SetPropertyNameAndPath(Addition.Property->GetFName(), Desired.PropertyPath);
			}
			else if (PropertyTrack && !Desired.PropertyPath.IsEmpty()) PropertyTrack->SetPropertyNameAndPath(FName(*Desired.PropertyPath), Desired.PropertyPath);
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
			if (!Composition::HjsonImport::PrepareReflectedObjectRecord(*Track, MovieSceneObjectPropertyPolicy(), Desired.Properties, Prepared, PropertyError))
				return Fail(OutError, PropertyError);
			if (Prepared.IsReady())
			{
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))
					return Fail(OutError, Result.Diagnostic);
				PropertyEdits.Add(MoveTemp(Prepared));
			}
		}
		if (!KeyEdits.Apply(&PropertyError)) return Fail(OutError, PropertyError);
		for (const auto& Update : SectionUpdates)
		{
			if (!TrySetMovieSceneSectionRange(*Update.Section, Update.Range, PropertyError)) {
				if (Update.Desired->SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(PropertyError, *Update.Desired->SourceRecord, TEXT("range"));
				return Fail(OutError, PropertyError);
			}
		}
		for (const auto& Removal : SectionRemovals)
		{ Removal.Key->Modify(); Removal.Value->Modify(); Removal.Key->RemoveSection(*Removal.Value); }
		for (const auto& Addition : SectionAdditions)
		{
			FailureSource = Addition.Desired->SourceRecord;
			FailureContext = Addition.Desired->SectionKind;
			Addition.Track->Modify();
			auto* Section = Addition.Track->CreateNewSection();
			if (!Section) return NativeFailure(TEXT("timeline_animation_level_sequence_section_create_failed"), TEXT("The native track factory could not create the requested section."),
				Addition.Desired->SourceRecord, TEXT("section_kind"), Addition.Desired->SectionKind, Addition.Desired->SectionRef);
			UeAgentAuthoringObjectApply::FPreparedApply NewProperties;
			if (!Composition::HjsonImport::PrepareReflectedObjectRecord(*Section, MovieSceneObjectPropertyPolicy(),
				Addition.Properties, NewProperties, PropertyError)) return Fail(OutError, PropertyError);
			if (NewProperties.IsReady())
			{
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::ApplyPrepared(NewProperties, Result))
					return Fail(OutError, Result.Diagnostic);
				PropertyEdits.Add(MoveTemp(NewProperties));
			}
			if (!TrySetMovieSceneSectionRange(*Section, Addition.Range, PropertyError))
				return Fail(OutError, PropertyError);
			Addition.Track->AddSection(*Section);
			ScalarChannel::FChannelEdits NewKeys;
			if (!PrepareChannels(*Section, {}, Addition.Channels, NewKeys)) return Fail(OutError, PropertyError);
			if (!NewKeys.Apply(&PropertyError)) return Fail(OutError, PropertyError);
			AddedKeyEdits.Add(MoveTemp(NewKeys));
		}
		for (const auto& Removal : TrackRemovals)
		{
			UMovieSceneTrack* Track = Removal.Track;
			TArray<UMovieSceneFolder*> Folders;
			GetMovieSceneFoldersRecursive(Scene->GetRootFolders(), Folders);
			for (auto* Folder : Folders) if (Folder->GetChildTracks().Contains(Track)) Folder->RemoveChildTrack(Track);
			if (!Track || !Scene->RemoveTrack(*Track))
				return NativeFailure(TEXT("timeline_animation_level_sequence_track_remove_failed"), !Track ? TEXT("The native track to remove no longer exists.") : TEXT("The movie scene rejected removal of the track."),
					Removal.Before->SourceRecord, TEXT("track_ref"), Removal.Before->DisplayName, Removal.Before->TrackRef);
		}
		for (const FGuid& BindingGuid : BindingRemovals)
		{
			OwnerSequence->Modify();
			// This asset is the local root of the asset-only operation. A tag with
			// the same GUID in another absolute sequence is a different identity.
			const UE::MovieScene::FFixedObjectBindingID LocalBinding(BindingGuid, MovieSceneSequenceID::Root);
			const FMovieSceneObjectBindingID SerializedLocalBinding = LocalBinding;
			TArray<FName> Tags;
			for (const auto& Tag : Scene->AllTaggedBindings())
				if (Tag.Value.IDs.Contains(SerializedLocalBinding)) Tags.Add(Tag.Key);
			if (!Tags.IsEmpty()) Scene->Modify();
			for (const FName& Tag : Tags) Scene->UntagBinding(Tag, LocalBinding);
			TArray<UMovieSceneFolder*> Folders;
			GetMovieSceneFoldersRecursive(Scene->GetRootFolders(), Folders);
			for (UMovieSceneFolder* Folder : Folders)
				if (Folder->GetChildObjectBindings().Contains(BindingGuid)) Folder->RemoveChildObjectBinding(BindingGuid);
			const bool bLegacySpawnable = Scene->FindSpawnable(BindingGuid) != nullptr;
			const bool bRemoved = bLegacySpawnable
				? Scene->RemoveSpawnable(BindingGuid) : Scene->RemovePossessable(BindingGuid);
			if (!bRemoved) {
				const auto* Before = RemovedBindingSources.FindChecked(BindingGuid);
				return NativeFailure(TEXT("timeline_animation_level_sequence_binding_remove_failed"), bLegacySpawnable
					? TEXT("The movie scene rejected removal of the spawnable binding.") : TEXT("The movie scene rejected removal of the possessable binding."),
					Before->SourceRecord, TEXT("binding_ref"), Before->BindingKind, Before->BindingRef);
			}
			// Native Sequencer ordering: remove the MovieScene binding, tear down
			// registered custom instances while their references still exist, unbind.
			// A proposed duplicate has no matching playback state and touches no world.
			DestroyBindingInstances(*OwnerSequence, BindingGuid, bLegacySpawnable);
			OwnerSequence->UnbindPossessableObjects(BindingGuid);
		}
		if (!FolderPlan.Apply(*Scene, CreationState, OutError)) return false;
		// Structural native setters can affect fields or channel data after an
		// individual property write. Retain the original plans until the batch ends.
		for (auto& Prepared : PropertyEdits)
		{
			UeAgentAuthoringObjectApply::ObservePrepared(Prepared);
		}
		if (!KeyEdits.Verify(&PropertyError))
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("timeline_animation_channel_intent_failed"), PropertyError, TEXT("channel_observation"));
		for (const auto& Added : AddedKeyEdits)
			if (!Added.Verify(&PropertyError))
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("timeline_animation_channel_intent_failed"), PropertyError, TEXT("channel_observation"));
		// Authored points retain the exported clock/rate throughout this edit.
		// Retiming is last so the native owner migrates every surviving channel,
		// section, range and marked frame once, including non-editable data.
		if (RequestedTickResolution != Scene->GetTickResolution())
			UE::MovieScene::TimeHelpers::MigrateFrameTimes(
				Scene->GetTickResolution(), RequestedTickResolution, Scene, false);
		// Native UMovieSceneTrack::PostLoad removes empty sections. Apply that
		// same relationship change before capturing the expected state, so an
		// explicitly empty range is not later mistaken for failed persistence.
		auto RemoveEmptySections = [](UMovieSceneTrack* Track) {
			if (!Track) return;
			for (int32 Index = Track->GetAllSections().Num() - 1; Index >= 0; --Index)
				if (const auto* Section = Track->GetAllSections()[Index]; Section && Section->GetRange().IsEmpty())
				{
					Track->Modify();
					Track->RemoveSectionAt(Index);
				}
		};
		for (auto* Track : Scene->GetTracks()) RemoveEmptySections(Track);
		for (const auto& Binding : Scene->GetBindings())
			for (auto* Track : Binding.GetTracks()) RemoveEmptySections(Track);
		OutError = {};
		return true;
	}

	bool TryReadMovieSceneStructure(const UMovieScene& MovieScene, const FString& ClockId,
		const TMap<FGuid, FMovieSceneBindingIdentity>& Identities, TSharedPtr<FJsonObject>& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError, bool bIncludeSequenceCollections)
	{
		OutStructure.Reset(); OutError = {};
		const UMovieScene* Scene = &MovieScene;
		const FFrameRate TickResolution = Scene->GetTickResolution();
		auto Rate = [](const FFrameRate& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("numerator"), FString::FromInt(Value.Numerator));
			Result->SetStringField(TEXT("denominator"), FString::FromInt(Value.Denominator));
			return Result;
		};
		auto PointContext = [&ClockId, &TickResolution, &Rate]()
		{
			const auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), ClockId);
			Result->SetObjectField(TEXT("rate"), Rate(TickResolution));
			Result->SetStringField(TEXT("state"), TEXT("authored"));
			return Result;
		};
		auto Point = [&PointContext](const int32 Tick)
		{
			const auto Result = PointContext();
			Result->SetStringField(TEXT("tick"), FString::FromInt(Tick));
			return Result;
		};
		auto Range = [&ClockId, &Point](const TRange<FFrameNumber>& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), ClockId);
			Result->SetStringField(TEXT("state"), TEXT("authored"));
			auto Bound = [&Point](const TRangeBound<FFrameNumber>& Native) {
				const auto Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("kind"), Native.IsOpen() ? TEXT("open") : Native.IsInclusive() ? TEXT("inclusive") : TEXT("exclusive"));
				if (!Native.IsOpen()) Object->SetObjectField(TEXT("point"), Point(Native.GetValue().Value));
				return Object;
			};
			Result->SetObjectField(TEXT("lower"), Bound(Value.GetLowerBound()));
			Result->SetObjectField(TEXT("upper"), Bound(Value.GetUpperBound()));
			return Result;
		};

		if (!Scene->GetPlaybackRange().HasLowerBound()
			|| !Scene->GetPlaybackRange().HasUpperBound())
		{
			OutError = FDiagnostic::Failure(TEXT("timeline_animation_playback_range_closed_required"), TEXT("The native movie-scene playback range must have both lower and upper bounds."), TEXT("native_execution"), MovieScene.GetPathName());
			OutError.PropertyPath = TEXT("playback_range"); OutError.SourceKind = TEXT("native_result");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("closed lower and upper bounds"));
			auto Bounds = MakeShared<FJsonObject>();
			Bounds->SetBoolField(TEXT("lower_present"), Scene->GetPlaybackRange().HasLowerBound());
			Bounds->SetBoolField(TEXT("upper_present"), Scene->GetPlaybackRange().HasUpperBound());
			OutError.ActualValue = MakeShared<FJsonValueObject>(Bounds);
			return false;
		}

		auto CaptureChannels = [&](const UMovieSceneSection& NativeSection, TArray<TSharedPtr<FJsonValue>>& Channels,
			TArray<TSharedPtr<FJsonValue>>& NativeContext) {
			return ScalarChannel::VisitSupported(NativeSection, [&](const auto& Channel, const FString& Ref, const FString& Name, const FString& Kind) {
				const auto Record = MakeShared<FJsonObject>();
				Record->SetStringField(TEXT("channel_ref"), Ref);
				Record->SetStringField(TEXT("name"), Name);
				Record->SetStringField(TEXT("channel_kind"), Kind);
				using NativeChannel = std::decay_t<decltype(Channel)>;
				using NativeValue = ScalarChannel::TKeyValue<NativeChannel>;
				Record->SetObjectField(TEXT("default"), ScalarChannel::CaptureDefault(Channel));
				TSharedPtr<FJsonObject> ChannelDefinition;
				if (const auto Properties = ScalarChannel::CaptureChannelProperties(Channel, nullptr, &ChannelDefinition)) Record->SetField(TEXT("properties"), Properties);
				if (ChannelDefinition) Record->SetObjectField(TEXT("properties_definition"), ChannelDefinition);
				TSharedPtr<FJsonObject> PropertiesDefinition;
				const auto DefaultKey = ScalarChannel::CaptureKeyDefault(Channel, &NativeSection);
				DefaultKey->SetObjectField(TEXT("time"), Point(0));
				Record->SetObjectField(TEXT("default_key"), DefaultKey);
				if (const auto Definition = ScalarChannel::CaptureValueDefinition(Channel, &NativeSection)) Record->SetObjectField(TEXT("key_value_definition"), Definition);
				ScalarChannel::CaptureProperties(NativeValue(), nullptr, &PropertiesDefinition);
				if (PropertiesDefinition) Record->SetObjectField(TEXT("key_properties_definition"), PropertiesDefinition);
				TArray<TSharedPtr<FJsonValue>> Keys;
				const auto Times = Channel.GetData().GetTimes(); const auto Values = Channel.GetData().GetValues();
				for (int32 Index = 0; Index < Times.Num(); ++Index)
				{
					if (!ScalarChannel::TValueAccess<NativeValue>::IsValid(Values[Index])) {
						OutError = FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_numeric_channel_unsupported"),
							TEXT("The native channel key contains a non-finite numeric value."), TEXT("native_execution"), Ref);
						OutError.CandidateName = Name;
						OutError.PropertyPath = FString::Printf(TEXT("keys[%d].value"), Index);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite numeric value"));
						if constexpr (!ScalarChannel::IsPlainValue<NativeValue>)
							OutError.ActualValue = MakeShared<FJsonValueString>(ScalarChannel::Lexeme(Values[Index].Value));
						OutError.SourceKind = TEXT("native_result"); return false;
					}
					const auto Key = MakeShared<FJsonObject>();
					Key->SetStringField(TEXT("key_ref"), Ref + TEXT(":key:") + FString::FromInt(Index));
					Key->SetObjectField(TEXT("time"), Point(Times[Index].Value));
					Key->SetStringField(TEXT("value_type"), ScalarChannel::ValueType(Channel, &NativeSection));
					Key->SetField(ScalarChannel::ValueField<NativeChannel>(), ScalarChannel::CaptureValue(Channel, Values[Index]));
					if (const auto Properties = ScalarChannel::CaptureProperties(Values[Index], nullptr)) Key->SetField(TEXT("properties"), Properties);
					Keys.Add(MakeShared<FJsonValueObject>(Key));
				}
				Record->SetArrayField(TEXT("keys"), Keys); Channels.Add(MakeShared<FJsonValueObject>(Record));
				return true;
			}, &NativeContext, &OutError);
		};

		auto BuildTrack = [&](UMovieSceneTrack* Track, const FString& StableRef,
			TSharedPtr<FJsonObject>& OutTrack)
		{
			UMovieScenePropertyTrack* NumericTrack = Cast<UMovieScenePropertyTrack>(Track);
			const FString Kind = ScalarChannel::Kind(Track);
			if (!Track || Kind.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_track_kind_unsupported"),
					!Track ? TEXT("The movie scene contains a null track.") : TEXT("The native track has no authoring kind."), TEXT("native_execution"), StableRef);
				OutError.CandidateName = Track ? Track->GetClass()->GetName() : FString();
				OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = Track != nullptr;
				OutError.SourceKind = TEXT("native_result"); return false;
			}
			OutTrack = MakeShared<FJsonObject>();
			UE::UAI::Contract::FDiagnostic PropertyError;
			if (!CaptureMovieSceneObjectProperties(*Track, *OutTrack, PropertyError))
				return Fail(OutError, PropertyError);
			const FString DisplayName = Track->GetDisplayName().ToString();
			OutTrack->SetStringField(TEXT("track_ref"), StableRef);
			OutTrack->SetStringField(TEXT("display_name"),
				DisplayName.IsEmpty() ? StableRef : DisplayName);
			OutTrack->SetStringField(TEXT("track_kind"), Kind);
			const FString PropertyPath = NumericTrack ? NumericTrack->GetPropertyPath().ToString() : FString();
			if (!PropertyPath.IsEmpty())
			{
				OutTrack->SetStringField(TEXT("property_path"), PropertyPath);
			}
			TArray<TSharedPtr<FJsonValue>> Sections;
			const TArray<UMovieSceneSection*>& NativeSections = Track->GetAllSections();
			for (int32 SectionIndex = 0; SectionIndex < NativeSections.Num(); ++SectionIndex)
			{
				const UMovieSceneSection* NativeSection = NativeSections[SectionIndex];
				if (!NativeSection)
				{
					OutError = FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_section_kind_unsupported"), TEXT("The native track contains a null section."), TEXT("native_execution"), StableRef);
					OutError.CandidateName = DisplayName; OutError.PropertyPath = FString::Printf(TEXT("sections[%d]"), SectionIndex);
					OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
					OutError.SourceKind = TEXT("native_result"); return false;
				}
				const FString SectionRef = FString::Printf(
					TEXT("%s:section:%d"), *StableRef, SectionIndex);
				const TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
				if (!CaptureMovieSceneObjectProperties(*NativeSection, *Section, PropertyError))
					return Fail(OutError, PropertyError);
				Section->SetStringField(TEXT("section_ref"), SectionRef);
				Section->SetStringField(TEXT("section_kind"), Kind);
				Section->SetObjectField(TEXT("range"), Range(NativeSection->GetRange()));
				TArray<TSharedPtr<FJsonValue>> Channels, NativeContext;
				if (!CaptureChannels(*NativeSection, Channels, NativeContext)) {
					OutError.InternalContext = SectionRef;
					OutError.PropertyPath = FString::Printf(TEXT("sections[%d]."), SectionIndex) + OutError.PropertyPath;
					return false;
				}
				Section->SetArrayField(TEXT("channels"), MoveTemp(Channels));
				Section->SetArrayField(TEXT("native_channels"), MoveTemp(NativeContext));
				Sections.Add(MakeShared<FJsonValueObject>(Section));
			}
			// Keep native factory context, but capture only the newly created section.
			TStrongObjectPtr<UMovieSceneTrack> DefaultTrack(ScalarChannel::MakeSectionTemplateTrack(*Track));
			if (!DefaultTrack) {
				OutError = FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_defaults_context_unsupported"),
					ScalarChannel::HasRequiredSectionContext(*Track) ? TEXT("The native track could not be duplicated to capture its section defaults.")
					: TEXT("The native track is missing the property class or vector dimension required by its section factory."), TEXT("native_execution"), StableRef);
				OutError.CandidateName = DisplayName; OutError.PropertyPath = TEXT("default_channels");
				OutError.ActualValue = MakeShared<FJsonValueString>(Track->GetClass()->GetName());
				OutError.SourceKind = TEXT("native_result"); return false;
			}
			auto* DefaultSection = DefaultTrack->CreateNewSection();
			TArray<TSharedPtr<FJsonValue>> DefaultChannels, DefaultNativeContext;
			if (!DefaultSection) {
				OutError = FDiagnostic::Failure(TEXT("timeline_animation_level_sequence_numeric_defaults_unsupported"), TEXT("The native track factory returned no default section."), TEXT("native_execution"), StableRef);
				OutError.CandidateName = DisplayName; OutError.PropertyPath = TEXT("default_channels");
				OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
				OutError.SourceKind = TEXT("native_result"); return false;
			}
			if (!CaptureChannels(*DefaultSection, DefaultChannels, DefaultNativeContext)) {
				OutError.InternalContext = StableRef;
				OutError.PropertyPath = TEXT("default_channels.") + OutError.PropertyPath; return false;
			}
			OutTrack->SetArrayField(TEXT("default_channels"), MoveTemp(DefaultChannels));
			OutTrack->SetArrayField(TEXT("default_native_channels"), MoveTemp(DefaultNativeContext));
			// Playback is not a section factory default. In particular, an empty
			// animation's [0,0) would manufacture sections that native PostLoad deletes.
			OutTrack->SetObjectField(TEXT("default_range"), Range(DefaultSection->GetRange()));
			// A newly bounded open range has a real clock, but no native default tick.
			OutTrack->SetObjectField(TEXT("bound_point_context"), PointContext());
			TSharedPtr<FJsonObject> DefaultProperties, DefaultDefinitions;
			if (!Composition::HjsonImport::CaptureReflectedObjectRecord(*DefaultSection, MovieSceneObjectPropertyPolicy(),
				DefaultProperties, DefaultDefinitions, PropertyError))
				return Fail(OutError, PropertyError);
			OutTrack->SetObjectField(TEXT("default_properties"), DefaultProperties);
			OutTrack->SetObjectField(TEXT("default_property_definitions"), DefaultDefinitions);
			OutTrack->SetArrayField(TEXT("sections"), MoveTemp(Sections));
			return true;
		};

		TArray<TSharedPtr<FJsonValue>> RootTracks;
		for (int32 Index = 0; Index < Scene->GetTracks().Num(); ++Index)
		{
			UMovieSceneTrack* NativeTrack = Scene->GetTracks()[Index];
			TSharedPtr<FJsonObject> Track;
			if (!BuildTrack(NativeTrack,
					TEXT("root_track:") + (NativeTrack ? NativeTrack->GetName() : FString()), Track))
			{
				return false;
			}
			RootTracks.Add(MakeShared<FJsonValueObject>(Track));
		}
		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (int32 Index = 0; Index < Scene->GetBindings().Num(); ++Index)
		{
			const FMovieSceneBinding& NativeBinding = Scene->GetBindings()[Index];
			const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
			Binding->SetStringField(TEXT("binding_ref"), NativeBinding.GetObjectGuid().ToString(EGuidFormats::Digits));
			Binding->SetStringField(TEXT("display_name"), NativeBinding.GetName());
			const auto* Identity = Identities.Find(NativeBinding.GetObjectGuid());
			if (!Identity) {
				OutError = FDiagnostic::Failure(TEXT("movie_scene_binding_identity_missing"), TEXT("The native binding has no corresponding captured identity record."), TEXT("native_execution"), NativeBinding.GetObjectGuid().ToString(EGuidFormats::Digits));
				OutError.CandidateName = NativeBinding.GetName(); OutError.PropertyPath = TEXT("binding_identity");
				OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
				OutError.SourceKind = TEXT("native_result"); return false;
			}
			Binding->SetStringField(TEXT("binding_kind"), Identity->Kind);
			if (!Identity->ObjectRef.IsEmpty()) Binding->SetStringField(TEXT("object_ref"), Identity->ObjectRef);
			if (Identity->Properties.IsValid()) Binding->SetObjectField(TEXT("properties"), Identity->Properties);
			if (Identity->PropertyDefinitions.IsValid()) Binding->SetObjectField(TEXT("property_definitions"), Identity->PropertyDefinitions);
			if (Identity->TemplateProperties.IsValid()) Binding->SetObjectField(TEXT("template_properties"), Identity->TemplateProperties);
			if (Identity->TemplateDefinitions.IsValid()) Binding->SetObjectField(TEXT("template_property_definitions"), Identity->TemplateDefinitions);
			if (bIncludeSequenceCollections)
			{
				const FMovieSceneObjectBindingID Local = UE::MovieScene::FFixedObjectBindingID(NativeBinding.GetObjectGuid(), MovieSceneSequenceID::Root);
				TArray<FName> Names;
				for (const auto& Tag : Scene->AllTaggedBindings()) if (Tag.Value.IDs.Contains(Local)) Names.Add(Tag.Key);
				Names.Sort(FNameLexicalLess());
				TArray<TSharedPtr<FJsonValue>> Tags;
				for (const FName& Name : Names) Tags.Add(MakeShared<FJsonValueString>(Name.ToString()));
				Binding->SetArrayField(TEXT("tags"), MoveTemp(Tags));
			}
			TArray<TSharedPtr<FJsonValue>> Tracks;
			for (int32 TrackIndex = 0; TrackIndex < NativeBinding.GetTracks().Num(); ++TrackIndex)
			{
				UMovieSceneTrack* NativeTrack = NativeBinding.GetTracks()[TrackIndex];
				TSharedPtr<FJsonObject> Track;
				if (!BuildTrack(NativeTrack,
						TEXT("binding:") + NativeBinding.GetObjectGuid().ToString(EGuidFormats::Digits)
							+ TEXT(":track:") + (NativeTrack ? NativeTrack->GetName() : FString()),
						Track))
				{
					return false;
				}
				Tracks.Add(MakeShared<FJsonValueObject>(Track));
			}
			Binding->SetArrayField(TEXT("tracks"), MoveTemp(Tracks));
			Bindings.Add(MakeShared<FJsonValueObject>(Binding));
		}

		OutStructure = MakeShared<FJsonObject>();
		OutStructure->SetObjectField(TEXT("display_rate"), Rate(Scene->GetDisplayRate()));
		OutStructure->SetObjectField(TEXT("tick_resolution"), Rate(TickResolution));
		OutStructure->SetObjectField(TEXT("playback_range"), Range(Scene->GetPlaybackRange()));
		OutStructure->SetArrayField(TEXT("bindings"), MoveTemp(Bindings));
		OutStructure->SetArrayField(TEXT("root_tracks"), MoveTemp(RootTracks));
		if (bIncludeSequenceCollections)
		{
			TArray<TPair<UMovieSceneFolder*, UMovieSceneFolder*>> Pending;
			for (auto* Folder : const_cast<UMovieScene*>(Scene)->GetRootFolders()) Pending.Emplace(Folder, nullptr);
			TSet<UMovieSceneFolder*> Seen; TArray<TSharedPtr<FJsonValue>> Folders;
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				auto* Folder = Pending[Index].Key; auto* Parent = Pending[Index].Value;
				if (!Folder || Folder->GetTypedOuter<UMovieScene>() != Scene || Seen.Contains(Folder))
				{
					auto Diagnostic = FDiagnostic::Failure(TEXT("timeline_animation_folder_ownership_invalid"),
						!Folder ? TEXT("The movie scene contains a null folder.")
							: Folder->GetTypedOuter<UMovieScene>() != Scene ? TEXT("The folder belongs to a different movie scene.")
							: TEXT("The folder occurs more than once in the movie-scene folder hierarchy."),
						TEXT("native_execution"), MovieScene.GetPathName());
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = MovieScene.GetPathName();
					Diagnostic.PropertyPath = TEXT("folders");
					Diagnostic.ActualValuePresent = Folder != nullptr;
					if (Folder) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Folder->GetPathName(Scene));
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-null, unique folder owned by this movie scene"));
					return Fail(OutError, MoveTemp(Diagnostic), MovieScene.GetPathName());
				}
				Seen.Add(Folder);
				auto Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("folder_ref"), Folder->GetPathName(Scene));
				Item->SetStringField(TEXT("name"), Folder->GetFolderName().ToString());
				Item->SetStringField(TEXT("parent"), Parent ? Parent->GetPathName(Scene) : FString());
				TArray<TSharedPtr<FJsonValue>> Members;
				for (const auto& Guid : Folder->GetChildObjectBindings()) Members.Add(MakeShared<FJsonValueString>(Guid.ToString(EGuidFormats::Digits)));
				Item->SetArrayField(TEXT("bindings"), MoveTemp(Members));
				TArray<TSharedPtr<FJsonValue>> Tracks;
				for (auto* Track : Folder->GetChildTracks()) {
					if (!Track || !Scene->GetTracks().Contains(Track))
					{
						auto Diagnostic = FDiagnostic::Failure(TEXT("timeline_animation_folder_track_unsupported"),
							!Track ? TEXT("The folder contains a null track.")
								: TEXT("The folder references a track that is not a root track of this movie scene."),
							TEXT("native_execution"), Folder->GetPathName());
						Diagnostic.SourceKind = TEXT("native_result");
						Diagnostic.TargetPath = MovieScene.GetPathName();
						Diagnostic.PropertyPath = Folder->GetPathName(Scene) + TEXT("/root_tracks");
						Diagnostic.ActualValuePresent = Track != nullptr;
						if (Track) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Track->GetName());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("root track of this movie scene"));
						return Fail(OutError, MoveTemp(Diagnostic), Folder->GetPathName());
					}
					Tracks.Add(MakeShared<FJsonValueString>(TEXT("root_track:") + Track->GetName())); }
				Item->SetArrayField(TEXT("root_tracks"), MoveTemp(Tracks));
				Folders.Add(MakeShared<FJsonValueObject>(Item));
				for (auto* Child : Folder->GetChildFolders()) Pending.Emplace(Child, Folder);
			}
			OutStructure->SetArrayField(TEXT("folders"), MoveTemp(Folders));
		}
		return true;
	}
}
