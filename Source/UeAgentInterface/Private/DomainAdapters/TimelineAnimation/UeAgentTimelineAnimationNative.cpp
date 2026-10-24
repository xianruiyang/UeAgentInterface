#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Animation/Authoring/UeAgentAnimationSequenceStructure.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineScalarChannel.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringObjectMetadata.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "LevelSequence.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneSignedObject.h"
#include "MovieSceneFolder.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneCommonHelpers.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "UObject/UnrealType.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "UniversalObjectLocatorResolveParams.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "PackageTools.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/MetaData.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/LazyObjectPtr.h"
#include "UObject/ObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using Authoring::Timeline::ApplySequenceCurves;
	using Authoring::Timeline::ApplySequenceMetadata;
	
	using Authoring::Timeline::TryPointSeconds;
	using Authoring::Timeline::TryRate;
	using Authoring::Timeline::TryPointTick;
	using Authoring::Timeline::TrySectionRange;
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		using UE::UAI::Contract::FDiagnostic;

		FString TimelineNativePointer(FString Key)
		{
			return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		FDiagnostic TimelineNativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("execution"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.TargetPath = Target;
			return Error;
		}

		FDiagnostic TimelineNativePresence(const FString& Code, const FString& Reason, const FString& Path,
			const bool Actual, const bool Expected, const FString& Target = FString())
		{
			return TimelineNativeFact(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected), Target);
		}

		void LocateTimelineInput(FDiagnostic& Error, const TSharedPtr<FJsonObject>& Record, const FString& Pointer)
		{
			if (!Record || !Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty() || Error.SourceKind == TEXT("command_input")) return;
			const auto PreviousSource = Error.Source;
			const FString PreviousKind = Error.SourceKind, PreviousPointer = Error.SemanticPointer;
			Error.SourceKind.Reset();
			Error.SemanticPointer = Pointer;
			Error.Source.InternalJsonPath = Pointer;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Record);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty() && Error.SourceKind != TEXT("command_input")) {
				Error.Source = PreviousSource;
				Error.SourceKind = PreviousKind;
				Error.SemanticPointer = PreviousPointer;
			} else if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("related_input");
		}

		UeAgentDetailsAuthoringService::FCapturePolicy BindingRecordPolicy()
		{
			UeAgentDetailsAuthoringService::FCapturePolicy Policy;
			Policy.AuthorizeProperty = [](const FProperty* Property) {
				return Property && ((Property->GetOwnerStruct() == FMovieSceneBindingReferences::StaticStruct()
					&& Property->GetFName() == TEXT("SortedReferences"))
					|| (Property->GetOwnerStruct() == FMovieSceneBindingReference::StaticStruct()
						&& Property->GetFName() == TEXT("CustomBinding"))
					|| (Property->GetOwnerStruct() == FMovieSceneSpawnable::StaticStruct()
						&& Property->GetFName() == TEXT("Ownership")));
			};
			Policy.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
				return Struct != FMovieSceneBindingReference::StaticStruct() || Field->GetFName() == TEXT("CustomBinding");
			};
			return Policy;
		}

		FMovieSceneBindingReferences CaptureBindingReferences(const ULevelSequence& Sequence, const FGuid& Guid)
		{
			FMovieSceneBindingReferences Result;
			if (const auto* References = Sequence.GetBindingReferences())
				for (const auto& Reference : References->GetReferences(Guid))
					Result.AddBinding(Guid, FUniversalObjectLocator(Reference.Locator), Reference.ResolveFlags, Reference.CustomBinding);
			return Result;
		}

		TArray<FString> SpawnablePropertyPaths()
		{
			return {TEXT("Ownership"), TEXT("SpawnTransform"), TEXT("Tags"),
				TEXT("bContinuouslyRespawn"), TEXT("bNetAddressableName")};
		}

		TMap<FString, UObject*> BindingTemplates(ULevelSequence& Sequence, const FGuid& Guid)
		{
			TMap<FString, UObject*> Result;
			if (auto* Legacy = Sequence.GetMovieScene()->FindSpawnable(Guid))
			{
				if (UObject* Template = Legacy->GetObjectTemplate()) Result.Add(TEXT("spawnable"), Template);
			}
			else if (const auto* References = Sequence.GetBindingReferences())
			{
				int32 Index = 0;
				for (const auto& Reference : References->GetReferences(Guid))
				{
					if (auto* Spawnable = Cast<UMovieSceneSpawnableBindingBase>(Reference.CustomBinding))
						if (UObject* Template = Spawnable->GetObjectTemplate())
							Result.Add(FString::Printf(TEXT("resolver_%d"), Index + 1), Template);
					++Index;
				}
			}
			return Result;
		}

		bool ApplyBindingTemplateProperties(ULevelSequence& Sequence, const FGuid& Guid,
			const TSharedPtr<FJsonObject>& Desired, const TSharedPtr<FJsonObject>& Record, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!Desired) return true;
			const auto Templates = BindingTemplates(Sequence, Guid);
TArray<TPair<TSharedRef<UeAgentAuthoringObjectApply::FPreparedApply>, FString>> Edits;
			for (const auto& Field : Desired->Values)
			{
				UObject* const* Template = Templates.Find(Field.Key);
				if (!Template)
				{
					TArray<TSharedPtr<FJsonValue>> Names;
					for (const auto& Entry : Templates) Names.Add(MakeShared<FJsonValueString>(Entry.Key));
					Error = TimelineNativeFact(TEXT("timeline_binding_template_unresolved"),
						TEXT("The sequence binding has no template with the requested selector."), TEXT("/template_properties/") + TimelineNativePointer(Field.Key),
						MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueArray>(MoveTemp(Names)), Sequence.GetPathName());
					LocateTimelineInput(Error, Record, TEXT("/template_properties/") + TimelineNativePointer(Field.Key));
					return false;
				}
				if (!Field.Value || Field.Value->Type != EJson::Object)
				{
					Error = TimelineNativeFact(TEXT("timeline_binding_template_unresolved"),
						TEXT("Template properties must be an object."), TEXT("/template_properties/") + TimelineNativePointer(Field.Key),
						Field.Value, MakeShared<FJsonValueString>(TEXT("object")), Sequence.GetPathName());
					LocateTimelineInput(Error, Record, TEXT("/template_properties/") + TimelineNativePointer(Field.Key));
					return false;
				}
				auto Prepared = MakeShared<UeAgentAuthoringObjectApply::FPreparedApply>();
const FString InputPointer = TEXT("/template_properties/") + TimelineNativePointer(Field.Key);
				if (!Composition::HjsonImport::PrepareReflectedObjectRecord(**Template, {}, Field.Value->AsObject(), *Prepared, Error)) {
					LocateTimelineInput(Error, Record, InputPointer);
					return false;
				}
				Edits.Emplace(Prepared, InputPointer);
			}
			for (auto& Prepared : Edits)
			{
				UeAgentAuthoringObjectApply::FApplyResult Result;
if (!UeAgentAuthoringObjectApply::ApplyPrepared(*Prepared.Key, Result)) {
					Error = MoveTemp(Result.Diagnostic);
					LocateTimelineInput(Error, Record, Prepared.Value);
					return false;
				}
			}
			return true;
		}

		bool PrepareLevelSequenceBindingEdit(ULevelSequence& Sequence, const FTimelineAnimationBinding& Binding,
			Authoring::Timeline::FMovieSceneBindingEdit& OutEdit, UE::UAI::Contract::FDiagnostic& Error)
		{
			FGuid Guid;
			if (!FGuid::ParseExact(Binding.BindingRef, EGuidFormats::Digits, Guid))
			{ Error = TimelineNativeFact(TEXT("timeline_binding_identity_invalid"), TEXT("A movie-scene binding reference must be a GUID in 32-digit format."),
				TEXT("/binding_ref"), MakeShared<FJsonValueString>(Binding.BindingRef), MakeShared<FJsonValueString>(TEXT("32 hexadecimal GUID digits")), Sequence.GetPathName());
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/binding_ref")); return false; }
			UMovieScene& Scene = *Sequence.GetMovieScene();
			auto Prepared = MakeShared<UeAgentAuthoringObjectApply::FPreparedApply>();
			const auto Policy = BindingRecordPolicy();
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bAllowDomainAuthorizedOwners = true;
			Options.AuthorizeProperty = Policy.AuthorizeProperty;
			Options.bEmitEditorNotifications = false;
			if (auto* Spawnable = Scene.FindSpawnable(Guid))
			{
				if (!Composition::HjsonImport::PrepareReflectedStructRecord(*FMovieSceneSpawnable::StaticStruct(),
Spawnable, Scene, SpawnablePropertyPaths(), Policy, Binding.Properties, Options, *Prepared, Error)) {
					LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/properties"));
					return false;
				}
				OutEdit = [Prepared, &Sequence, Guid, Templates = Binding.TemplateProperties, Record = Binding.SourceRecord](UE::UAI::Contract::FDiagnostic& ApplyError) {
					UeAgentAuthoringObjectApply::FApplyResult Result;
					if (Prepared->IsReady() && (!UeAgentAuthoringObjectApply::ApplyPrepared(*Prepared, Result)))
{
						ApplyError = MoveTemp(Result.Diagnostic);
						LocateTimelineInput(ApplyError, Record, TEXT("/properties"));
						return false;
					}
					return ApplyBindingTemplateProperties(Sequence, Guid, Templates, Record, ApplyError);
				};
				return true;
			}
			auto References = MakeShared<FMovieSceneBindingReferences>(CaptureBindingReferences(Sequence, Guid));
			// Locators contain native fragment payload that reflection cannot round-trip.
			// Keep that routing state with its resolver while the public record edits
			// only CustomBinding. New resolvers receive an empty native locator.
			auto OriginalReferences = MakeShared<FMovieSceneBindingReferences>(*References);
			if (!Composition::HjsonImport::PrepareReflectedStructRecord(*FMovieSceneBindingReferences::StaticStruct(),
&References.Get(), Scene, {TEXT("SortedReferences")}, Policy, Binding.Properties, Options, *Prepared, Error)) {
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/properties"));
				return false;
			}
			OutEdit = [Prepared, References, OriginalReferences, &Sequence, Guid, Templates = Binding.TemplateProperties, Record = Binding.SourceRecord](UE::UAI::Contract::FDiagnostic& ApplyError) {
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (Prepared->IsReady() && (!UeAgentAuthoringObjectApply::ApplyPrepared(*Prepared, Result)))
{
					ApplyError = MoveTemp(Result.Diagnostic);
					LocateTimelineInput(ApplyError, Record, TEXT("/properties"));
					return false;
				}
				Sequence.Modify();
				auto* Native = static_cast<UMovieSceneSequence&>(Sequence).GetBindingReferences();
				Native->RemoveBinding(Guid);
				const auto Original = OriginalReferences->GetAllReferences();
				int32 Index = 0;
				for (const auto& Reference : References->GetAllReferences())
				{
					const FMovieSceneBindingReference* Source = nullptr;
					if (Reference.CustomBinding)
					{
						for (const auto& Candidate : Original)
							if (Candidate.CustomBinding == Reference.CustomBinding) { Source = &Candidate; break; }
					}
					else if (Index < Original.Num() && !Original[Index].CustomBinding) Source = &Original[Index];
					Native->AddBinding(Guid, Source ? FUniversalObjectLocator(Source->Locator) : FUniversalObjectLocator(),
						Source ? Source->ResolveFlags : Reference.ResolveFlags, Reference.CustomBinding);
					++Index;
				}
				return ApplyBindingTemplateProperties(Sequence, Guid, Templates, Record, ApplyError);
			};
			return true;
		}

		bool ResolveLevelSequenceObjectBinding(const FTimelineAnimationBinding& Binding,
			Authoring::Timeline::FMovieSceneObjectBinding& Out, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (Binding.BindingKind != TEXT("possessable"))
			{ Error = TimelineNativeFact(TEXT("level_sequence_possessable_object_ref_required"), TEXT("Object binding resolution requires a possessable binding."),
				TEXT("/binding_kind"), MakeShared<FJsonValueString>(Binding.BindingKind), MakeShared<FJsonValueString>(TEXT("possessable")), Binding.BindingRef);
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/binding_kind")); return false; }
			if (Binding.ObjectRef.IsEmpty())
			{ Error = TimelineNativeFact(TEXT("level_sequence_possessable_object_ref_required"), TEXT("A possessable binding requires an object reference."),
				TEXT("/object_ref"), MakeShared<FJsonValueString>(Binding.ObjectRef), MakeShared<FJsonValueString>(TEXT("non-empty object reference")), Binding.BindingRef);
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/object_ref")); return false; }
			if (!UeAgentPropertyValueCodec::DecodeObjectReference(
				UeAgentPropertyValueCodec::EncodeReferencePath(Binding.ObjectRef), UObject::StaticClass(),
nullptr, Out.Object, Error)) {
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/object_ref"));
				return false;
			}
			Out.Context = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!Out.Context) { Error = TimelineNativePresence(TEXT("level_sequence_editor_world_unavailable"),
				TEXT("The editor world needed to install the possessable binding is unavailable."), TEXT("/editor/world"), false, true, Binding.ObjectRef); return false; }
			Out.Install = [Object = Out.Object, Context = Out.Context](UMovieSceneSequence& Owner, const FGuid& Guid, UE::UAI::Contract::FDiagnostic& InstallError)
			{
				Owner.BindPossessableObject(Guid, *Object, Context);
				// Native binding is void and can silently omit an unsupported locator.
				// Check the original object before optional explicit binding properties
				// are applied; those may intentionally replace or clear the locator.
				TArray<UObject*, TInlineAllocator<1>> Objects;
				Owner.LocateBoundObjects(Guid, UE::UniversalObjectLocator::FResolveParams(Context), nullptr, Objects);
				if (Objects.Num() != 1 || Objects[0] != Object)
				{
InstallError = TimelineNativeFact(TEXT("level_sequence_binding_object_unresolved"),
						TEXT("The installed possessable binding did not resolve to exactly the requested object."),
						TEXT("/resolved_objects/count"), MakeShared<FJsonValueNumber>(Objects.Num()),
						MakeShared<FJsonValueNumber>(1), Object->GetPathName());
					if (Objects.Num() == 1) {
						InstallError.PropertyPath = TEXT("/resolved_objects/0");
if (Objects[0]) InstallError.ActualValue = MakeShared<FJsonValueString>(Objects[0]->GetPathName());
						else InstallError.ActualValue.Reset();
						InstallError.ExpectedValue = MakeShared<FJsonValueString>(Object->GetPathName());
					}
					return false;
				}
				return true;
			};
			return true;
		}

		bool ResolveLevelSequenceAuthoringBinding(ULevelSequence& Sequence, const FTimelineAnimationBinding& Binding,
			Authoring::Timeline::FMovieSceneObjectBinding& Out, UE::UAI::Contract::FDiagnostic& Error,
			Authoring::Timeline::FMovieSceneCreationState* CreationState)
		{
			FGuid ExistingGuid;
			if (FGuid::ParseExact(Binding.BindingRef, EGuidFormats::Digits, ExistingGuid))
			{
				if (auto* Legacy = Sequence.GetMovieScene()->FindSpawnable(ExistingGuid))
				{ Out.Object = Legacy->GetObjectTemplate(); return Out.Object != nullptr; }
				if (const auto* References = Sequence.GetBindingReferences())
					for (const auto& Reference : References->GetReferences(ExistingGuid))
						if (auto* Spawnable = Cast<UMovieSceneSpawnableBindingBase>(Reference.CustomBinding))
						{ Out.Object = Spawnable->GetObjectTemplate(); return Out.Object != nullptr; }
			}
			if (Binding.BindingKind != TEXT("spawnable")) return ResolveLevelSequenceObjectBinding(Binding, Out, Error);
			if (!UeAgentPropertyValueCodec::DecodeObjectReference(
UeAgentPropertyValueCodec::EncodeReferencePath(Binding.ObjectRef), UObject::StaticClass(), nullptr, Out.Object, Error)) {
				LocateTimelineInput(Error, Binding.SourceRecord, TEXT("/object_ref"));
				return false;
			}
			TArray<const TSubclassOf<UMovieSceneCustomBinding>> Types;
			MovieSceneHelpers::GetPrioritySortedCustomBindingTypes(Types);
			UMovieSceneCustomBinding* Created = nullptr;
			for (const auto& Type : Types)
			{
				if (!Type || !Type->IsChildOf<UMovieSceneSpawnableBindingBase>()) continue;
				auto* Defaults = Type->GetDefaultObject<UMovieSceneCustomBinding>();
				if (Defaults->SupportsBindingCreationFromObject(Out.Object))
				{
					if (auto* SourceActor = Cast<AActor>(Out.Object))
					{
						// The native factory removes Sequencer tags from its source. Authoring
						// borrows that source and must not alter the user's world actor.
						TGuardValue<TArray<FName>> PreserveTags(SourceActor->Tags, SourceActor->Tags);
						Created = Defaults->CreateNewCustomBinding(Out.Object, *Sequence.GetMovieScene());
					}
					else Created = Defaults->CreateNewCustomBinding(Out.Object, *Sequence.GetMovieScene());
				}
				if (Created) break;
			}
			if (!Created) { Error = TimelineNativeFact(TEXT("timeline_spawnable_source_unsupported"),
				TEXT("No registered spawnable binding factory accepted the source object."), TEXT("/spawnable/source_type"),
				MakeShared<FJsonValueString>(IsValid(Out.Object) ? Out.Object->GetClass()->GetPathName() : FString()),
				MakeShared<FJsonValueString>(TEXT("object supported by a spawnable binding factory")), Binding.ObjectRef); return false; }
			if (CreationState)
			{
				if (!CreationState->StabilizeObjectName(*Created, Binding.BindingRef + TEXT(":custom_binding")))
				{ Error = TimelineNativePresence(TEXT("timeline_custom_binding_identity_failed"),
					TEXT("The created custom binding could not receive its frozen object name."), TEXT("/native_creation/custom_binding_name_stable"), false, true, Binding.BindingRef); return false; }
				auto* Spawnable = CastChecked<UMovieSceneSpawnableBindingBase>(Created);
				if (auto* ActorTemplate = Cast<AActor>(Spawnable->GetObjectTemplate()))
				{
					if (!CreationState->StabilizeObjectName(*ActorTemplate, Binding.BindingRef + TEXT(":actor_template")))
					{ Error = TimelineNativePresence(TEXT("timeline_actor_template_identity_failed"),
						TEXT("The created actor template could not receive its frozen object name."), TEXT("/native_creation/actor_template_name_stable"), false, true, Binding.BindingRef); return false; }
					// AActor::Serialize generates a fresh identity for every normal duplicate.
					// Preview and commit are one creation, so retain its first native identity.
					auto* GuidProperty = FindFProperty<FStructProperty>(AActor::StaticClass(), TEXT("ActorGuid"));
					if (!GuidProperty) { Error = TimelineNativePresence(TEXT("timeline_actor_template_identity_unavailable"),
						TEXT("The ActorGuid reflection property is unavailable on the actor template."), TEXT("/native_creation/actor_template/ActorGuid"), false, true, Binding.BindingRef); return false; }
					auto& NativeGuid = *GuidProperty->ContainerPtrToValuePtr<FGuid>(ActorTemplate);
					if (const FGuid* Frozen = CreationState->ActorTemplateIds.Find(Binding.BindingRef)) NativeGuid = *Frozen;
					else CreationState->ActorTemplateIds.Add(Binding.BindingRef, NativeGuid);
				}
			}
			auto KeepAlive = MakeShared<TStrongObjectPtr<UMovieSceneCustomBinding>>(Created);
			Out.BoundClass = Created->GetBoundObjectClass();
			Out.Install = [KeepAlive, CreationState](UMovieSceneSequence& Owner, const FGuid& Guid, UE::UAI::Contract::FDiagnostic& InstallError) {
				Owner.GetBindingReferences()->AddOrReplaceBinding(Guid, KeepAlive->Get(), 0);
				auto* Track = Owner.GetMovieScene()->AddTrack<UMovieSceneSpawnTrack>(Guid);
				if (!Track) { InstallError = TimelineNativePresence(TEXT("timeline_spawn_track_create_failed"),
					TEXT("The movie scene failed to create the spawn track for the new binding."), TEXT("/binding/spawn_track"), false, true, Owner.GetPathName()); return false; }
				// Removed tracks can remain in the live UObject outer until GC, while
				// the preview duplicate omits them. Tie this new track to the already
				// frozen binding identity instead of either outer's allocation counter.
				if (CreationState && !CreationState->StabilizeObjectName(*Track, Guid.ToString(EGuidFormats::Digits) + TEXT(":spawn")))
				{ InstallError = TimelineNativePresence(TEXT("timeline_spawn_track_identity_failed"),
					TEXT("The created spawn track could not receive its frozen object name."), TEXT("/native_creation/spawn_track_name_stable"), false, true, Owner.GetPathName()); return false; }
				Track->AddSection(*Track->CreateNewSection());
				return true;
			};
			return true;
		}



		class FSemanticPersistentObjectWriter final
			: public FObjectAndNameAsStringProxyArchive
		{
		public:
			FSemanticPersistentObjectWriter(
				FArchive& Inner, const UObject& InRoot, const UObject& InAssetRoot)
				: FObjectAndNameAsStringProxyArchive(Inner, false),
				  Root(InRoot), AssetRoot(InAssetRoot)
			{
				SetIsSaving(true);
				SetIsPersistent(true);
				ArNoDelta = true;
			}

			virtual FArchive& operator<<(UObject*& Object) override
			{
				FString Path = StablePath(Object);
				*this << Path;
				return *this;
			}

			virtual bool ShouldSkipProperty(const FProperty* Property) const override
			{
				// Native generation signatures deliberately differ for equivalent content
				// (UMovieSceneSignedObject::MarkAsChanged). They are not authored identity.
				return (Property && Property->GetOwnerStruct() == UMovieSceneSignedObject::StaticClass()
					&& Property->GetFName() == TEXT("Signature"))
					|| FObjectAndNameAsStringProxyArchive::ShouldSkipProperty(Property);
			}

			virtual FArchive& operator<<(FObjectPtr& Object) override
			{
				UObject* Resolved = Object.Get();
				return *this << Resolved;
			}

			virtual FArchive& operator<<(FWeakObjectPtr& Object) override
			{
				UObject* Resolved = Object.Get();
				return *this << Resolved;
			}

			virtual FArchive& operator<<(FSoftObjectPtr& Object) override
			{
				FSoftObjectPath Path = Object.ToSoftObjectPath();
				return *this << Path;
			}

			virtual FArchive& operator<<(FSoftObjectPath& Object) override
			{
				FString Path = NormalizePath(Object.ToString());
				*this << Path;
				return *this;
			}

		private:
			FString StablePath(const UObject* Object) const
			{
				if (!Object) return TEXT("$null");
				if (Object == &Root) return TEXT("$root");
				if (Object == &AssetRoot) return TEXT("$asset");
				if (Object->IsIn(&Root))
				{
					return TEXT("$root/") + Object->GetPathName(&Root);
				}
				return Object->GetPathName();
			}

			FString NormalizePath(FString Path) const
			{
				const FString RootPath = Root.GetPathName();
				if (Path == RootPath) return TEXT("$root");
				if (Path.StartsWith(RootPath + TEXT(".")))
					return TEXT("$root/") + Path.RightChop(RootPath.Len() + 1);
				const FString AssetPath = AssetRoot.GetPathName();
				if (Path == AssetPath) return TEXT("$asset");
				if (Path.StartsWith(AssetPath + TEXT(".")))
					return TEXT("$asset/") + Path.RightChop(AssetPath.Len() + 1);
				return Path;
			}

			const UObject& Root;
			const UObject& AssetRoot;
		};

		template <typename VisitorType>
		bool VisitMovieSceneTracks(const UMovieScene& Scene, VisitorType&& Visitor)
		{
			for (UMovieSceneTrack* Track : Scene.GetTracks())
			{
				if (Visitor(Track)) return true;
			}
			for (const FMovieSceneBinding& Binding : Scene.GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (Visitor(Track)) return true;
				}
			}
			return false;
		}

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }


		void DiscardUnregisteredAsset(UObject*& Asset)
		{
			if (!IsValid(Asset))
			{
				Asset = nullptr;
				return;
			}
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->MarkAsGarbage();
			Asset = nullptr;
		}

		const FUeAgentTimelineAnimationRequest* ProductionRequest(
			const FUeAgentTimelineAnimationTypedRequestRef& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			const TCHAR* Field = Request->GetTypeId().IsEmpty() ? TEXT("type_id")
				: Request->GetFamilyId().IsEmpty() ? TEXT("family_id") : Request->GetRequestKindId().IsEmpty() ? TEXT("request_kind_id") : nullptr;
			if (Field)
			{
				Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_native_typed_request_invalid"),
					TEXT("The typed timeline request has an empty required identity field."), TEXT("/typed_request/") + FString(Field),
					MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty identity"))));
				return nullptr;
			}
			return static_cast<const FUeAgentTimelineAnimationRequest*>(&Request.Get());
		}





		bool TryClosedRangeTicks(const FTimelineAnimationRange& RangeValue,
			const FString& ClockId, const FFrameRate& Rate,
			int64& OutLower, int64& OutUpper,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (RangeValue.ClockId != ClockId)
				return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_c4_exact_time_mismatch"),
					TEXT("The authored range clock differs from the target timeline clock."), TEXT("/range/clock_id"),
					MakeShared<FJsonValueString>(RangeValue.ClockId), MakeShared<FJsonValueString>(ClockId)));
			if (!RangeValue.Lower.Point.IsSet() || !RangeValue.Upper.Point.IsSet())
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_c4_closed_range_required"),
					TEXT("Native sampling requires both range bounds to contain a point."),
					!RangeValue.Lower.Point.IsSet() ? TEXT("/range/lower/point_present") : TEXT("/range/upper/point_present"), false, true));
			if (!TryPointTick(*RangeValue.Lower.Point, ClockId, Rate, OutLower, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/range/lower/point"), OutError);
			if (!TryPointTick(*RangeValue.Upper.Point, ClockId, Rate, OutUpper, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/range/upper/point"), OutError);
			return OutUpper >= OutLower || Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_c4_range_order_invalid"),
				TEXT("The native upper range tick precedes the lower tick."), TEXT("/range/upper/tick"),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), OutUpper)),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("at least %lld"), OutLower))));
		}



		FString ObjectPath(const UObject& Object)
		{
			return Object.GetPathName();
		}

		FString TransformText(const FTransform& Value)
		{
			const FVector P = Value.GetTranslation();
			const FQuat R = Value.GetRotation();
			const FVector S = Value.GetScale3D();
			return FString::Printf(TEXT("p=%.9g,%.9g,%.9g;r=%.9g,%.9g,%.9g,%.9g;s=%.9g,%.9g,%.9g"),
				P.X, P.Y, P.Z, R.X, R.Y, R.Z, R.W, S.X, S.Y, S.Z);
		}

		FString GenericObjectState(UObject* Object)
		{
			if (!IsValid(Object)) return TEXT("absent");
			FString State = Object->GetClass()->GetPathName() + TEXT("|")
				+ Object->GetPathName();
			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
			{
				const IAnimationDataModel* Model = Sequence->GetDataModel();
				State += FString::Printf(TEXT("|frames=%d|rate=%d/%d|tracks=%d|curves=%d|notifies=%d|markers=%d"),
					Model ? Model->GetNumberOfFrames() : -1,
					Model ? Model->GetFrameRate().Numerator : 0,
					Model ? Model->GetFrameRate().Denominator : 0,
					Model ? Model->GetNumBoneTracks() : -1,
					Model ? Model->GetFloatCurves().Num() : -1,
					Sequence->Notifies.Num(), Sequence->AuthoredSyncMarkers.Num());
				if (Model)
				{
					TArray<FName> BoneNames;
					Model->GetBoneTrackNames(BoneNames);
					for (const FName BoneName : BoneNames)
					{
						TArray<FTransform> BoneTransforms;
						Model->GetBoneTrackTransforms(BoneName, BoneTransforms);
						State += TEXT("|bone=") + BoneName.ToString()
							+ FString::Printf(TEXT(":%d"), BoneTransforms.Num());
					}
					for (const FFloatCurve& Curve : Model->GetFloatCurves())
						State += TEXT("|curve=") + Curve.GetName().ToString()
							+ FString::Printf(TEXT(":%d"), Curve.FloatCurve.GetNumKeys());
				}
				for (const FAnimNotifyEvent& Event : Sequence->Notifies)
					State += TEXT("|notify=") + Event.NotifyName.ToString();
				for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
					State += TEXT("|marker=") + Marker.MarkerName.ToString();
				if (UPackage* Package = Sequence->GetOutermost())
				{
					Package->GetMetaData();
					if (const TMap<FName, FString>* Values = FMetaData::GetMapForObject(Sequence))
						for (const TPair<FName, FString>& Pair : *Values)
							State += TEXT("|meta=") + Pair.Key.ToString() + TEXT(":") + Pair.Value;
				}
			}
			else if (const UAnimMontage* Montage = Cast<UAnimMontage>(Object))
			{
				State += FString::Printf(TEXT("|length=%.9g|sections=%d|slots=%d"),
					Montage->GetPlayLength(), Montage->CompositeSections.Num(),
					Montage->SlotAnimTracks.Num());
				for (const FCompositeSection& Section : Montage->CompositeSections)
					State += TEXT("|section=") + Section.SectionName.ToString();
				for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
					State += TEXT("|slot=") + Track.SlotName.ToString();
			}
			else if (const USkeleton* Skeleton = Cast<USkeleton>(Object))
			{
				for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
				{
					State += TEXT("|group=") + Group.GroupName.ToString();
					for (const FName Slot : Group.SlotNames)
						State += TEXT("|slot=") + Slot.ToString();
				}
			}
			else if (const ULevelSequence* LevelTimeline = Cast<ULevelSequence>(Object))
			{
				if (const UMovieScene* Scene = LevelTimeline->GetMovieScene())
				{
					State += FString::Printf(TEXT("|display=%d/%d|tick=%d/%d|start=%d|end=%d|bindings=%d|tracks=%d"),
						Scene->GetDisplayRate().Numerator, Scene->GetDisplayRate().Denominator,
						Scene->GetTickResolution().Numerator, Scene->GetTickResolution().Denominator,
						Scene->GetPlaybackRange().GetLowerBoundValue().Value,
						Scene->GetPlaybackRange().GetUpperBoundValue().Value,
						Scene->GetBindings().Num(), Scene->GetTracks().Num());
					int32 BoundTracks = 0;
					int32 Sections = 0;
					int32 Keys = 0;
					auto CountTrack = [&Sections, &Keys](UMovieSceneTrack* Track)
					{
						if (!Track)
						{
							return;
						}
						Sections += Track->GetAllSections().Num();
						for (UMovieSceneSection* Section : Track->GetAllSections())
						{
							const UMovieSceneFloatSection* FloatSection =
								Cast<UMovieSceneFloatSection>(Section);
							const FMovieSceneFloatChannel* Channel = FloatSection
								? FloatSection->GetChannelProxy().
									GetChannel<FMovieSceneFloatChannel>(0)
								: nullptr;
							if (Channel)
							{
								Keys += Channel->GetData().GetTimes().Num();
							}
						}
					};
					for (UMovieSceneTrack* Track : Scene->GetTracks())
					{
						CountTrack(Track);
					}
					for (const FMovieSceneBinding& Binding : Scene->GetBindings())
					{
						BoundTracks += Binding.GetTracks().Num();
						for (UMovieSceneTrack* Track : Binding.GetTracks())
						{
							CountTrack(Track);
						}
					}
					State += FString::Printf(
						TEXT("|bound_tracks=%d|sections=%d|keys=%d"),
						BoundTracks,
						Sections,
						Keys);
					State += TEXT("|signature=") + Scene->GetSignature().ToString();
				}
			}
			else if (const UWidgetAnimation* WidgetAnimation = Cast<UWidgetAnimation>(Object))
			{
				if (const UMovieScene* Scene = WidgetAnimation->GetMovieScene())
					State += FString::Printf(TEXT("|display=%d/%d|tick=%d/%d|bindings=%d|tracks=%d"),
						Scene->GetDisplayRate().Numerator, Scene->GetDisplayRate().Denominator,
						Scene->GetTickResolution().Numerator, Scene->GetTickResolution().Denominator,
						Scene->GetBindings().Num(), Scene->GetTracks().Num());
			}
			return State;
		}

		class FProductionEnvironment final
			: public IUeAgentTimelineAnimationEnvironment
			, public IUeAgentTimelineAnimationRevisionSource
		{
		public:
			FProductionEnvironment(FString InGeneration, FString InSession,
				TArray<FUeAgentTimelineAnimationSubjectBinding> InSubjects,
				TArray<FUeAgentTimelineAnimationRigCandidateBinding> InRigs)
				: Generation(MoveTemp(InGeneration)), Session(MoveTemp(InSession)),
					Subjects(MoveTemp(InSubjects)), Rigs(MoveTemp(InRigs))
			{
				for (const FUeAgentTimelineAnimationSubjectBinding& Binding : Subjects)
				{
					if (Binding.PoseComponent.IsValid())
						PinnedObjects.Emplace(Binding.PoseComponent.Get());
					if (Binding.MorphComponent.IsValid())
						PinnedObjects.Emplace(Binding.MorphComponent.Get());
				}
				for (const FUeAgentTimelineAnimationRigCandidateBinding& Binding : Rigs)
				{
					if (Binding.Sequence.IsValid()) PinnedObjects.Emplace(Binding.Sequence.Get());
					if (Binding.Rig.IsValid()) PinnedObjects.Emplace(Binding.Rig.Get());
				}
			}

			void GetExplicitSubjectIdentities(TArray<FString>& OutIds) const override
			{
				OutIds.Reset();
				for (const auto& Binding : Subjects) OutIds.Add(Binding.Identity);
			}
			void GetExplicitRigCandidateIdentities(TArray<FString>& OutIds) const override
			{
				OutIds.Reset();
				for (const auto& Binding : Rigs) OutIds.Add(Binding.Identity);
			}

			const FUeAgentTimelineAnimationSubjectBinding* FindSubject(
				const FString& Identity) const
			{
				return Subjects.FindByPredicate([&](const auto& B){ return B.Identity == Identity; });
			}
			const FUeAgentTimelineAnimationRigCandidateBinding* FindRig(
				const FString& Identity) const
			{
				return Rigs.FindByPredicate([&](const auto& B){ return B.Identity == Identity; });
			}

			UObject* Resolve(const FString& ExplicitRef, FUeAgentTimelineAnimationExecutionError& OutError) const
			{
				if (const auto* Subject = FindSubject(ExplicitRef))
					return Subject->PoseComponent.IsValid()
						? static_cast<UObject*>(Subject->PoseComponent.Get())
						: static_cast<UObject*>(Subject->MorphComponent.Get());
				if (const auto* Rig = FindRig(ExplicitRef)) return Rig->Sequence.Get();
				FString ObjectPath = ExplicitRef;
				UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
				if (!ContentDiscovery::TryResolveEditorContentReference(ObjectPath, FString(), ResolveError))
				{
					Fail(OutError, ResolveError, ResolveError.SubjectId);
					return nullptr;
				}
				return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath,
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}

			bool TryReadRevision(const FString& ExplicitRef,
				FTimelineAnimationRevision& OutRevision,
				FUeAgentTimelineAnimationExecutionError& OutError) const override
			{
				OutRevision = {};
				OutError = {};
				UObject* Object = Resolve(ExplicitRef, OutError);
				if (!IsValid(Object)) return OutError.IsValid() ? false : Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_explicit_target_unavailable"),
					TEXT("The explicit timeline target could not be resolved."), TEXT("/target/resolved"), false, true, ExplicitRef));
				FString State = GenericObjectState(Object);
				if (const auto* Subject = FindSubject(ExplicitRef))
				{
					if (UPoseableMeshComponent* Pose = Subject->PoseComponent.Get())
					{
						for (int32 Index = 0; Index < Pose->GetNumBones(); ++Index)
							State += TEXT("|subject_bone=")
								+ Pose->GetBoneName(Index).ToString() + TEXT(":")
								+ TransformText(Pose->GetBoneTransform(Index));
					}
					if (USkeletalMeshComponent* Morph = Subject->MorphComponent.Get())
						for (const TPair<FName, float>& Pair : Morph->GetMorphTargetCurves())
							State += FString::Printf(TEXT("|subject_morph=%s:%.9g"),
								*Pair.Key.ToString(), Pair.Value);
				}
				const uint64 Upper = FCrc::StrCrc32(*State);
				const uint64 Lower = FCrc::StrCrc32(*(Session + TEXT(":") + State));
				OutRevision.SourceId = TEXT("timeline-animation:") + ExplicitRef;
				OutRevision.Generation = Generation;
				OutRevision.Sequence = static_cast<int64>(
					((Upper << 32) | Lower) & ((1ULL << 53) - 1));
if (OutRevision.TryValidate(OutError)) return true;
				if (OutError.Source.SourceFile.IsEmpty()) OutError.SourceKind = TEXT("native_result");
				return false;
			}

			void RecordMutation(const FString& OperationId, UObject* Object)
			{
				FScopeLock Lock(&Mutex);
				MutatedObjects.Add(OperationId, Object);
			}
			void ForgetMutation(const FString& OperationId)
			{
				FScopeLock Lock(&Mutex);
				MutatedObjects.Remove(OperationId);
				VerifiedDocuments.Remove(OperationId);
			}
			void RecordVerifiedDocument(const FString& OperationId, const TSharedPtr<FJsonObject>& Document) {
				FScopeLock Lock(&Mutex);
				VerifiedDocuments.Add(OperationId, Document);
			}
			TSharedPtr<FJsonObject> FindVerifiedDocument(const FString& OperationId) const {
				FScopeLock Lock(&Mutex);
				return VerifiedDocuments.FindRef(OperationId);
			}
			UObject* FindMutation(const FString& OperationId) const
			{
				FScopeLock Lock(&Mutex);
				const TWeakObjectPtr<UObject>* Found = MutatedObjects.Find(OperationId);
				return Found ? Found->Get() : nullptr;
			}
			void PreparePackageUnload(UPackage* Package)
			{
				PinnedObjects.RemoveAllSwap(
					[&](const TStrongObjectPtr<UObject>& Pinned)
					{
						UObject* const Object = Pinned.Get();
						return Object && Object->GetOutermost() == Package;
					});
			}
			bool TryRebindReloadedAsset(const FString& ObjectPath, UObject* Reloaded,
				FUeAgentTimelineAnimationExecutionError& OutError)
			{
				ULevelSequence* const Sequence = Cast<ULevelSequence>(Reloaded);
				for (FUeAgentTimelineAnimationRigCandidateBinding& Binding : Rigs)
				{
					if (Binding.Identity != ObjectPath) continue;
					if (!Sequence) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_reload_sequence_type_mismatch"),
						TEXT("The reloaded rig asset is not a LevelSequence."), TEXT("/reloaded_target/type"),
						MakeShared<FJsonValueString>(IsValid(Reloaded) ? Reloaded->GetClass()->GetPathName() : TEXT("missing")),
						MakeShared<FJsonValueString>(ULevelSequence::StaticClass()->GetPathName()), ObjectPath));
					UControlRig* Rig = nullptr;
					if (UMovieScene* Scene = Sequence->GetMovieScene())
					{
						VisitMovieSceneTracks(*Scene,
							[&](UMovieSceneTrack* SceneTrack)
							{
								auto* Track = Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
								UControlRig* const Candidate = Track ? Track->GetControlRig() : nullptr;
								if (!Candidate || Candidate->GetPathName() != Binding.RigIdentity)
									return false;
								Rig = Candidate;
								return true;
							});
					}
					if (!Rig) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_reload_rig_binding_unavailable"),
						TEXT("No control-rig track in the reloaded sequence resolved the registered rig."),
						TEXT("/reloaded_target/rig_binding_resolved"), false, true, Binding.RigIdentity));
					Binding.Sequence = Sequence;
					Binding.Rig = Rig;
					PinnedObjects.Emplace(Sequence);
					PinnedObjects.Emplace(Rig);
				}
				return true;
			}

		private:
			FString Generation;
			FString Session;
			TArray<FUeAgentTimelineAnimationSubjectBinding> Subjects;
			TArray<FUeAgentTimelineAnimationRigCandidateBinding> Rigs;
			TArray<TStrongObjectPtr<UObject>> PinnedObjects;
			mutable FCriticalSection Mutex;
			TMap<FString, TWeakObjectPtr<UObject>> MutatedObjects;
			TMap<FString, TSharedPtr<FJsonObject>> VerifiedDocuments;
		};

		FString ExplicitRef(const FUeAgentTimelineAnimationRequest& Request,
			const TOptional<FUeAgentCandidateBinding>& Target)
		{
			if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
				|| Request.GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate)
				return Request.GetData().DestinationRef;
			return Target.IsSet() ? Target->GetPublicRef() : FString();
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool TryStringValue(const FString& Text, const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
if (FUeAgentCanonicalValue::TryString(Text, Context.GetResourceTracker(), OutValue, Error)) return true;
			if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			return Fail(OutError, MoveTemp(Error));
		}

		bool TryIntegerValue(const int64 Value, const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
if (FUeAgentCanonicalValue::TryInteger(FString::Printf(TEXT("%lld"), Value), Context.GetResourceTracker(), OutValue, Error)) return true;
			if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			return Fail(OutError, MoveTemp(Error));
		}

		bool TryCanonicalObject(
			const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
if (FUeAgentCanonicalValue::TryObject(Items, Context.GetResourceTracker(), OutValue, Error)) return true;
			if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			return Fail(OutError, MoveTemp(Error));
		}

		bool TryBuildLevelSequenceObserved(
			const ULevelSequence& Sequence,
			const FString& PublicRef,
			const FTimelineAnimationRequestData& Data,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			OutValue.Reset();
			const UMovieScene* Scene = Sequence.GetMovieScene();
if (!Scene) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_movie_scene_unavailable"),
				TEXT("The level sequence has no MovieScene to observe."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));

			auto String = [&](const FString& Value, FUeAgentCanonicalValuePtr& Out)
			{
				return TryStringValue(Value, Context, Out, OutError);
			};
			auto Integer = [&](int64 Value, FUeAgentCanonicalValuePtr& Out)
			{
				return TryIntegerValue(Value, Context, Out, OutError);
			};
			auto Rate = [&](const FFrameRate& Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentCanonicalValuePtr Numerator, Denominator;
				return Integer(Value.Numerator, Numerator) && Integer(Value.Denominator, Denominator)
					&& TryCanonicalObject({
						{TEXT("numerator"), Numerator.ToSharedRef()},
						{TEXT("denominator"), Denominator.ToSharedRef()}}, Context, Out, OutError);
			};
			auto Range = [&](const TRange<FFrameNumber>& Value, FUeAgentCanonicalValuePtr& Out)
			{
				const int64 Lower = Value.HasLowerBound() ? Value.GetLowerBoundValue().Value : 0;
				const int64 Upper = Value.HasUpperBound() ? Value.GetUpperBoundValue().Value : Lower;
				FUeAgentCanonicalValuePtr LowerValue, UpperValue;
				return Integer(Lower, LowerValue) && Integer(Upper, UpperValue)
					&& TryCanonicalObject({
						{TEXT("lower"), LowerValue.ToSharedRef()},
						{TEXT("upper"), UpperValue.ToSharedRef()}}, Context, Out, OutError);
			};

			FUeAgentCanonicalValuePtr SequenceRef, SequenceKind, DisplayRate, TickResolution,
				PlaybackRange, BindingCount, RootTrackCount;
			if (!String(PublicRef, SequenceRef)
				|| !String(Sequence.GetClass()->GetName(), SequenceKind)
				|| !Rate(Scene->GetDisplayRate(), DisplayRate)
				|| !Rate(Scene->GetTickResolution(), TickResolution)
				|| !Range(Scene->GetPlaybackRange(), PlaybackRange)
				|| !Integer(Scene->GetBindings().Num(), BindingCount)
				|| !Integer(Scene->GetTracks().Num(), RootTrackCount)) return false;

			TArray<FUeAgentCanonicalValueRef> Bindings;
			TArray<FUeAgentCanonicalValueRef> RootTracks;

			auto BuildTrack = [&](UMovieSceneTrack* Track, const FString& TrackRef,
				FUeAgentCanonicalValuePtr& OutTrack)
			{
				FUeAgentCanonicalValuePtr Type, SectionCount;
				if (!Track || !String(Track->GetClass()->GetName(), Type)
					|| !Integer(Track->GetAllSections().Num(), SectionCount)) return false;
				TArray<FUeAgentCanonicalValueRef> Sections;
				if (Data.IncludeTracks.IsSet() && *Data.IncludeTracks)
				{
					for (int32 Index = 0; Index < Track->GetAllSections().Num(); ++Index)
					{
						UMovieSceneSection* Section = Track->GetAllSections()[Index];
						if (!Section) continue;
						FUeAgentCanonicalValuePtr Ref, SectionType, KeyCount, SectionValue;
						if (!String(FString::Printf(TEXT("%s:section:%d"), *TrackRef, Index), Ref)
							|| !String(Section->GetClass()->GetName(), SectionType)) return false;
						TArray<TPair<FString, FUeAgentCanonicalValueRef>> SectionItems{
							{TEXT("section_ref"), Ref.ToSharedRef()},
							{TEXT("section_type"), SectionType.ToSharedRef()}};
						if (Data.IncludeKeys.IsSet() && *Data.IncludeKeys)
						{
							int32 KeyTotal = 0;
							TArray<FUeAgentCanonicalValueRef> KeyValues;
							if (const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
								if (const FMovieSceneFloatChannel* Channel = FloatSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0))
								{
									const TArrayView<const FFrameNumber> Times =
										Channel->GetData().GetTimes();
									KeyTotal = Times.Num();
									for (int32 KeyIndex = 0; KeyIndex < Times.Num(); ++KeyIndex)
									{
										FUeAgentCanonicalValuePtr KeyValue;
										if (!Integer(Times[KeyIndex].Value, KeyValue)) return false;
										KeyValues.Add(KeyValue.ToSharedRef());
									}
								}
							if (!Integer(KeyTotal, KeyCount)) return false;
							FUeAgentCanonicalValuePtr KeysValue;
							FUeAgentStateSemanticError KeysError;
							if (!FUeAgentCanonicalValue::TryArray(KeyValues, Context.GetResourceTracker(), KeysValue, KeysError))
								if (KeysError.Source.SourceFile.IsEmpty()) KeysError.SourceKind = TEXT("native_result");
								return Fail(OutError, MoveTemp(KeysError));
							SectionItems.Add({TEXT("key_count"), KeyCount.ToSharedRef()});
							SectionItems.Add({TEXT("keys"), KeysValue.ToSharedRef()});
						}
						if (!TryCanonicalObject(SectionItems, Context, SectionValue, OutError)) return false;
						Sections.Add(SectionValue.ToSharedRef());
					}
				}
				FUeAgentCanonicalValuePtr Ref;
				if (!String(TrackRef, Ref)) return false;
				FUeAgentCanonicalValuePtr SectionsValue;
				FUeAgentStateSemanticError SectionsError;
				if (!FUeAgentCanonicalValue::TryArray(Sections, Context.GetResourceTracker(), SectionsValue, SectionsError))
					if (SectionsError.Source.SourceFile.IsEmpty()) SectionsError.SourceKind = TEXT("native_result");
					return Fail(OutError, MoveTemp(SectionsError));
				return TryCanonicalObject({
					{TEXT("track_ref"), Ref.ToSharedRef()},
					{TEXT("track_type"), Type.ToSharedRef()},
					{TEXT("section_count"), SectionCount.ToSharedRef()},
					{TEXT("sections"), SectionsValue.ToSharedRef()}}, Context, OutTrack, OutError);
			};

			if (Data.IncludeTracks.IsSet() && *Data.IncludeTracks)
			{
				for (int32 Index = 0; Index < Scene->GetTracks().Num(); ++Index)
				{
					FUeAgentCanonicalValuePtr Track;
					if (!BuildTrack(Scene->GetTracks()[Index], FString::Printf(TEXT("root_track:%d"), Index), Track)) return false;
					RootTracks.Add(Track.ToSharedRef());
				}
			}
			if (Data.IncludeBindings.IsSet() && *Data.IncludeBindings)
			{
				for (int32 Index = 0; Index < Scene->GetBindings().Num(); ++Index)
				{
					const FMovieSceneBinding& Binding = Scene->GetBindings()[Index];
					FUeAgentCanonicalValuePtr Ref, Name, TrackCount, BindingValue;
					if (!String(FString::Printf(TEXT("binding:%d"), Index), Ref)
						|| !String(FString::Printf(TEXT("binding_%d"), Index), Name)
						|| !Integer(Binding.GetTracks().Num(), TrackCount)) return false;
					TArray<FUeAgentCanonicalValueRef> Tracks;
					if (Data.IncludeTracks.IsSet() && *Data.IncludeTracks)
						for (int32 TrackIndex = 0; TrackIndex < Binding.GetTracks().Num(); ++TrackIndex)
						{
							FUeAgentCanonicalValuePtr Track;
							if (!BuildTrack(Binding.GetTracks()[TrackIndex], FString::Printf(TEXT("binding:%d:track:%d"), Index, TrackIndex), Track)) return false;
							Tracks.Add(Track.ToSharedRef());
						}
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> BindingItems{
						{TEXT("binding_ref"), Ref.ToSharedRef()},
						{TEXT("name"), Name.ToSharedRef()},
						{TEXT("track_count"), TrackCount.ToSharedRef()}};
					if (Data.IncludeTracks.IsSet() && *Data.IncludeTracks)
					{
						FUeAgentCanonicalValuePtr TrackArray;
						FUeAgentStateSemanticError ArrayError;
						if (!FUeAgentCanonicalValue::TryArray(
								Tracks,
								Context.GetResourceTracker(),
								TrackArray,
								ArrayError))
						{
							if (ArrayError.Source.SourceFile.IsEmpty()) ArrayError.SourceKind = TEXT("native_result");
							return Fail(OutError, MoveTemp(ArrayError));
						}
						BindingItems.Add({TEXT("tracks"), TrackArray.ToSharedRef()});
					}
					if (!TryCanonicalObject(
							BindingItems,
							Context,
							BindingValue,
							OutError)) return false;
					Bindings.Add(BindingValue.ToSharedRef());
				}
			}

			FUeAgentCanonicalValuePtr SequenceValue, StructureValue;
			if (!TryCanonicalObject({{TEXT("asset_ref"), SequenceRef.ToSharedRef()},
				{TEXT("asset_kind"), SequenceKind.ToSharedRef()}}, Context, SequenceValue, OutError)) return false;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> StructureItems{
				{TEXT("display_rate"), DisplayRate.ToSharedRef()},
				{TEXT("tick_resolution"), TickResolution.ToSharedRef()},
				{TEXT("playback_range"), PlaybackRange.ToSharedRef()},
				{TEXT("binding_count"), BindingCount.ToSharedRef()},
				{TEXT("root_track_count"), RootTrackCount.ToSharedRef()}};
			FUeAgentStateSemanticError ArrayError;
			if (Data.IncludeBindings.IsSet() && *Data.IncludeBindings)
			{
				FUeAgentCanonicalValuePtr BindingArray;
				if (!FUeAgentCanonicalValue::TryArray(
						Bindings,
						Context.GetResourceTracker(),
						BindingArray,
						ArrayError))
				{
					if (ArrayError.Source.SourceFile.IsEmpty()) ArrayError.SourceKind = TEXT("native_result");
					return Fail(OutError, MoveTemp(ArrayError));
				}
				StructureItems.Add({TEXT("bindings"), BindingArray.ToSharedRef()});
			}
			if (Data.IncludeTracks.IsSet() && *Data.IncludeTracks)
			{
				FUeAgentCanonicalValuePtr RootTrackArray;
				if (!FUeAgentCanonicalValue::TryArray(
						RootTracks,
						Context.GetResourceTracker(),
						RootTrackArray,
						ArrayError))
				{
					if (ArrayError.Source.SourceFile.IsEmpty()) ArrayError.SourceKind = TEXT("native_result");
					return Fail(OutError, MoveTemp(ArrayError));
				}
				StructureItems.Add({TEXT("root_tracks"), RootTrackArray.ToSharedRef()});
			}
			if (!TryCanonicalObject(
					StructureItems,
					Context,
					StructureValue,
					OutError)) return false;
			return TryCanonicalObject({{TEXT("sequence"), SequenceValue.ToSharedRef()},
				{TEXT("structure"), StructureValue.ToSharedRef()}}, Context, OutValue, OutError);
		}

		bool TrySnapshot(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			EUeAgentTimelineAnimationReadPhase Phase, const FString& PublicRef,
			const FString& State, bool bTargetPresent,
			const FUeAgentStateRevision* NativeRevision,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentTimelineAnimationExecutionError& OutError,
			const FUeAgentCanonicalValuePtr& ObservedValue = nullptr)
		{
			FUeAgentCanonicalValuePtr RefValue, CapabilityValue, StateValue, ObjectValue;
			FUeAgentStateSemanticError Error;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError)
				|| !TryStringValue(Contract.GetCapabilityId(), Context, CapabilityValue, OutError)
				|| (!ObservedValue.IsValid()
					&& !TryStringValue(State, Context, StateValue, OutError)))
			{
				return false;
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision DerivedRevision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & ((1ULL << 53) - 1)));
			const FUeAgentStateRevision& Revision = NativeRevision
				? *NativeRevision : DerivedRevision;
			TArray<FUeAgentStateEntry> Entries;
			if (bTargetPresent)
			{
				if (ObservedValue.IsValid())
				{
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
						: ObservedValue->GetObjectItems())
					{
						Entries.Emplace(
							FUeAgentCanonicalIdentity(
								TEXT("timeline-animation:")
									+ Contract.GetCapabilityId()
									+ TEXT(":") + Pair.Key,
								TEXT("timeline_animation_target"),
								PublicRef,
								Pair.Key,
								TEXT("timeline_sequence.") + Pair.Key,
								TEXT("native-observation-v1")),
							FUeAgentCanonicalType(
								Contract.GetTypedResultId() + TEXT(".") + Pair.Key,
								Pair.Value->GetKind(),
								false,
								TEXT("object")),
							Pair.Value,
							FUeAgentDefaultValue(
								EUeAgentDefaultSourceKind::DomainNative,
								Contract.GetReadbackId(),
								TEXT("independent-native-readback"),
								Pair.Value),
							FUeAgentStateDetails(
								Pair.Key,
								TEXT("independent TimelineAnimation native observation"),
								{TEXT("TimelineAnimation"), Contract.GetFamilyId()},
								false,
								TEXT("available"),
								{}),
							Revision);
					}
				}
				else
				{
					if (!FUeAgentCanonicalValue::TryObject({
							{TEXT("public_ref"), RefValue.ToSharedRef()},
							{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
							{TEXT("observed_state"), StateValue.ToSharedRef()}},
						Context.GetResourceTracker(), ObjectValue, Error))
					{
						if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
						return Fail(OutError, MoveTemp(Error));
					}
					Entries.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("timeline-animation:") + Contract.GetCapabilityId(),
							TEXT("timeline_animation_target"), PublicRef, PublicRef,
							TEXT("result"), TEXT("native-observation-v1")),
						FUeAgentCanonicalType(Contract.GetTypedResultId(),
							EUeAgentValueKind::Object, false, TEXT("object")),
						ObjectValue.ToSharedRef(),
						FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
							Contract.GetReadbackId(), TEXT("independent-native-readback"),
							ObjectValue.ToSharedRef()),
						FUeAgentStateDetails(TEXT("result"),
							TEXT("independent TimelineAnimation native observation"),
							{TEXT("TimelineAnimation"), Contract.GetFamilyId()}, false,
							TEXT("available"), {}), Revision);
				}
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(TEXT("timeline-animation:%s:%d:%s"),
						*Contract.GetCapabilityId(), static_cast<int32>(Phase),
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(), Revision,
					FUeAgentTemporalPoint(TEXT("editor-native"),
						FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
					FUeAgentStateProvenance(TEXT("independent_readback"),
						Contract.GetReadbackId(),
						TEXT("native-state:") + Contract.GetCapabilityId()),
					Entries, Context.GetResourceTracker(), OutSnapshot, Error))
			{
				if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			const FUeAgentCanonicalValueRef& Root = Snapshot.GetEntries()[0].GetValue();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair : Root->GetObjectItems())
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			return FString();
		}

		FUeAgentCanonicalValuePtr SnapshotResultField(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& Field)
		{
			if (Snapshot.GetEntries().IsEmpty())
			{
				return nullptr;
			}
			const FUeAgentCanonicalValueRef& Root =
				Snapshot.GetEntries()[0].GetValue();
			if (Root->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Root->GetObjectItems())
			{
				if (Pair.Key == Field)
				{
					return Pair.Value;
				}
			}
			return nullptr;
		}

		FString QueryState(UObject* Object,
			const FUeAgentTimelineAnimationRequest& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FString State = GenericObjectState(Object);
			const FTimelineAnimationRequestData& Data = Request.GetData();
			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
			{
				const IAnimationDataModel* Model = Sequence->GetDataModel();
				const FFrameRate NativeRate = Model ? Model->GetFrameRate()
					: FFrameRate(0, 0);
				const FString NativeClock = TEXT("animation_sequence:")
					+ Sequence->GetPathName();
				int64 LowerTick = 0, UpperTick = 0, PointTick = 0;
				if ((Data.TemporalRange.IsSet()
						&& !TryClosedRangeTicks(*Data.TemporalRange, NativeClock,
							NativeRate, LowerTick, UpperTick, OutError))
					|| (Data.TimePoint.IsSet()
						&& !TryPointTick(*Data.TimePoint, NativeClock, NativeRate,
							PointTick, OutError))) return FString();
				if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze)
				{
					double Start = 0.0, End = 0.0;
					if (!Data.TemporalRange.IsSet() || !Data.TemporalRange->Lower.Point.IsSet() || !Data.TemporalRange->Upper.Point.IsSet())
						return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_root_motion_range_required"),
							TEXT("Root-motion sampling requires a range with both endpoint points."),
							!Data.TemporalRange.IsSet() ? TEXT("/temporal_range/present") : !Data.TemporalRange->Lower.Point.IsSet()
								? TEXT("/temporal_range/lower/point_present") : TEXT("/temporal_range/upper/point_present"), false, true, Sequence->GetPathName())), FString();
					if (!TryPointSeconds(*Data.TemporalRange->Lower.Point, Start, OutError)
						|| !TryPointSeconds(*Data.TemporalRange->Upper.Point, End, OutError)) return FString();
					FAnimExtractContext Extract(Start);
					State += TEXT("|root_motion=") + TransformText(
						Sequence->ExtractRootMotionFromRange(Start, End, Extract));
				}
				else if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::SequencePoseSample)
				{
					double Seconds = 0.0;
					if (!Data.TimePoint.IsSet())
						return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_pose_time_required"),
							TEXT("Pose sampling requires a time point."), TEXT("/time_point/present"), false, true, Sequence->GetPathName())), FString();
					if (!TryPointSeconds(*Data.TimePoint, Seconds, OutError)) return FString();
					const USkeleton* Skeleton = Sequence->GetSkeleton();
					for (int32 BoneIndex = 0; BoneIndex < Data.BoneRefs.Num(); ++BoneIndex)
					{
						const FString& BoneRef = Data.BoneRefs[BoneIndex];
						const int32 Index = Skeleton
							? Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneRef))
							: INDEX_NONE;
						if (Index == INDEX_NONE) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_bone_unavailable"),
							Skeleton ? TEXT("The requested bone was not found in the target skeleton.") : TEXT("The target animation sequence has no skeleton."),
							TEXT("/bone_refs/") + FString::FromInt(BoneIndex), MakeShared<FJsonValueString>(BoneRef),
							MakeShared<FJsonValueString>(TEXT("bone present in the target skeleton")), Sequence->GetPathName())), FString();
						FTransform Bone;
						const FAnimExtractContext Extraction(Seconds);
						Sequence->GetBoneTransform(Bone,
							FSkeletonPoseBoneIndex(Index), Extraction, true);
						State += TEXT("|pose=") + BoneRef + TEXT(":") + TransformText(Bone);
					}
				}
			}
			else if (const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Object))
			{
				FVector Input = FVector::ZeroVector;
				TSet<int32> AssignedAxes;
				for (int32 ParameterIndex = 0; ParameterIndex < Data.BlendParameters.Num(); ++ParameterIndex)
				{
					const auto& Parameter = Data.BlendParameters[ParameterIndex];
					const FString ParameterPath = TEXT("/blend_parameters/") + FString::FromInt(ParameterIndex) + TEXT("/axis_ref");
					int32 AxisIndex = INDEX_NONE;
					const TCHAR* CoordinateNames[] = {TEXT("x"), TEXT("y"), TEXT("z")};
					for (int32 Index = 0; Index < 3; ++Index)
						if (Parameter.AxisRef.Equals(CoordinateNames[Index], ESearchCase::IgnoreCase)
							|| Parameter.AxisRef == FString::FromInt(Index)) AxisIndex = Index;
					if (AxisIndex == INDEX_NONE)
					{
						for (int32 Index = 0; Index < 3; ++Index)
							if (BlendSpace->GetBlendParameter(Index).DisplayName.Equals(Parameter.AxisRef, ESearchCase::IgnoreCase))
							{
								if (AxisIndex != INDEX_NONE)
								{
									auto Matches = MakeShared<FJsonObject>();
									Matches->SetStringField(TEXT("axis_ref"), Parameter.AxisRef);
									Matches->SetArrayField(TEXT("matched_indices"), {MakeShared<FJsonValueNumber>(AxisIndex), MakeShared<FJsonValueNumber>(Index)});
									return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_blend_axis_ambiguous"),
										TEXT("The blend-axis display name matches multiple native axes."), ParameterPath,
										MakeShared<FJsonValueObject>(Matches), MakeShared<FJsonValueString>(TEXT("one native axis; use x/y/z or index to disambiguate")), ObjectPath(*BlendSpace))), FString();
								}
								AxisIndex = Index;
							}
					}
					if (AxisIndex == INDEX_NONE)
						return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_blend_axis_unavailable"),
							TEXT("The blend-axis reference matches no coordinate name, index, or native display name."), ParameterPath,
							MakeShared<FJsonValueString>(Parameter.AxisRef), MakeShared<FJsonValueString>(TEXT("x/y/z, 0/1/2, or a native blend-axis display name")), ObjectPath(*BlendSpace))), FString();
					if (AssignedAxes.Contains(AxisIndex))
						return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_blend_axis_duplicate"),
							TEXT("Multiple blend parameters assign the same native axis."), ParameterPath,
							MakeShared<FJsonValueNumber>(AxisIndex), MakeShared<FJsonValueString>(TEXT("an axis not assigned by an earlier parameter")), ObjectPath(*BlendSpace))), FString();
					AssignedAxes.Add(AxisIndex);
					Input[AxisIndex] = Parameter.Value;
				}
				TArray<FBlendSampleData> Samples;
				int32 Triangulation = INDEX_NONE;
				if (!BlendSpace->GetSamplesFromBlendInput(Input, Samples, Triangulation, true))
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_blend_sample_failed"),
						TEXT("The blend space failed to produce samples for the requested input."), TEXT("/blend_input"),
						MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Input.X), MakeShared<FJsonValueNumber>(Input.Y), MakeShared<FJsonValueNumber>(Input.Z)}),
						MakeShared<FJsonValueString>(TEXT("input accepted by the native blend sampler")), ObjectPath(*BlendSpace))), FString();
				for (const FBlendSampleData& Sample : Samples)
					State += FString::Printf(TEXT("|sample=%d:%.9g"),
						Sample.SampleDataIndex, Sample.TotalWeight);
			}
			return State;
		}

		bool CheckRevision(FProductionEnvironment& Environment,
			const FString& ExplicitRef, const FTimelineAnimationRevision& Expected,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FTimelineAnimationRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			if (Actual.SourceId == Expected.SourceId && Actual.Generation == Expected.Generation && Actual.Sequence == Expected.Sequence) return true;
			const FString Field = Actual.SourceId != Expected.SourceId ? TEXT("source_id")
				: Actual.Generation != Expected.Generation ? TEXT("generation") : TEXT("sequence");
			auto Error = TimelineNativePresence(TEXT("timeline_animation_expected_revision_mismatch"),
				TEXT("The target revision differs from the requested baseline."), TEXT("/expected_revision/") + Field + TEXT("_matches"), false, true, ExplicitRef);
			if (Field == TEXT("source_id"))
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.SourceId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.SourceId);
			}
			else if (Field == TEXT("generation"))
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.Generation);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.Generation);
			}
			else
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Actual.Sequence));
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Expected.Sequence));
			}
			return Fail(OutError, MoveTemp(Error));
		}

		bool SplitAssetRef(const FString& Ref, FString& OutPackage, FString& OutName)
		{
			OutPackage = Ref;
			int32 Dot = INDEX_NONE;
			if (OutPackage.FindLastChar(TEXT('.'), Dot)) OutPackage.LeftInline(Dot);
			int32 Slash = INDEX_NONE;
			if (!OutPackage.FindLastChar(TEXT('/'), Slash) || Slash + 1 >= OutPackage.Len())
				return false;
			OutName = OutPackage.Mid(Slash + 1);
			return OutPackage.StartsWith(TEXT("/Game/")) && !OutName.IsEmpty();
		}

		bool ApplySequenceTracks(UAnimSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FFrameRate Rate;
			if (!Data.SampleRate.IsSet()) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_sample_rate_required"),
				TEXT("Replacing bone tracks requires a sample rate."), TEXT("/sample_rate/present"), false, true, Sequence.GetPathName()));
			if (!TryRate(*Data.SampleRate, Rate, OutError)) return false;
			const FString NativeClock = TEXT("animation_sequence:") + Sequence.GetPathName();
			int64 LowerTick = 0, UpperTick = 0;
			if (!Data.TemporalRange.IsSet()) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_authored_range_required"),
				TEXT("Replacing bone tracks requires an authored range."), TEXT("/authored_range/present"), false, true, Sequence.GetPathName()));
			if (!TryClosedRangeTicks(*Data.TemporalRange, NativeClock, Rate, LowerTick, UpperTick, OutError)) return false;
			IAnimationDataController& Controller = Sequence.GetController();
			Controller.OpenBracket(FText::FromString(TEXT("UAI replace bone tracks")), false);
			Controller.RemoveAllBoneTracks(false);
			Controller.SetFrameRate(Rate, false);
			int32 KeyCount = 0;
			for (const auto& Track : Data.BoneTracks) KeyCount = FMath::Max(KeyCount, Track.Keys.Num());
			Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(0, KeyCount - 1)), false);
			bool bOk = true;
			for (int32 TrackIndex = 0; TrackIndex < Data.BoneTracks.Num(); ++TrackIndex)
			{
				const auto& Track = Data.BoneTracks[TrackIndex];
				const FString TrackPath = TEXT("/bone_tracks/") + FString::FromInt(TrackIndex);
				if (Track.Keys.Num() != KeyCount || UpperTick - LowerTick != KeyCount)
				{
					bOk = Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_bone_track_sampling_mismatch"),
						Track.Keys.Num() != KeyCount ? TEXT("The bone track key count differs from the largest track.")
							: TEXT("The authored tick range length differs from the bone track key count."),
						Track.Keys.Num() != KeyCount ? TrackPath + TEXT("/keys/count") : TEXT("/authored_range/tick_count"),
						MakeShared<FJsonValueString>(::LexToString(Track.Keys.Num() != KeyCount ? static_cast<int64>(Track.Keys.Num()) : UpperTick - LowerTick)),
						MakeShared<FJsonValueString>(::LexToString(KeyCount)), Track.BoneRef));
					break;
				}
				TArray<FVector> Positions;
				TArray<FQuat> Rotations;
				TArray<FVector> Scales;
				const FString NativeFrame = TEXT("animation_sequence:")
					+ Sequence.GetPathName() + TEXT(":bone:") + Track.BoneRef;
				for (int32 KeyIndex = 0; KeyIndex < Track.Keys.Num(); ++KeyIndex)
				{
					const FTimelineAnimationBoneKey& Key = Track.Keys[KeyIndex];
					int64 KeyTick = 0;
					if (!TryPointTick(Key.Time, NativeClock, Rate, KeyTick, OutError)
						|| KeyTick != LowerTick + KeyIndex)
					{
						bOk = OutError.IsValid() ? false : Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_bone_track_key_time_mismatch"),
							TEXT("The bone key tick does not match the authored sampling grid."), TrackPath + TEXT("/keys/") + FString::FromInt(KeyIndex) + TEXT("/time/tick"),
							MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), KeyTick)),
							MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), LowerTick + KeyIndex)), Track.BoneRef));
						break;
					}
					FTransform Transform;
					if (!Key.Transform.TryToNativeTransform(NativeFrame, Transform, OutError))
					{
						bOk = false;
						break;
					}
					Positions.Add(Transform.GetTranslation());
					Rotations.Add(Transform.GetRotation());
					Scales.Add(Transform.GetScale3D());
				}
				if (!bOk) break;
				const FName BoneName(*Track.BoneRef);
				if (!Controller.AddBoneCurve(BoneName, false)
					&& !Sequence.GetDataModel()->IsValidBoneTrackName(BoneName))
				{
					bOk = false;
					Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_bone_track_add_failed"),
						TEXT("The animation controller failed to add the bone track and no matching track exists."), TrackPath + TEXT("/native_track_present"), false, true, Track.BoneRef));
					break;
				}
				bOk = Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, false);
				if (!bOk) Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_bone_track_keys_failed"),
					TEXT("The animation controller rejected the sampled bone track keys."), TrackPath + TEXT("/keys_applied"), false, true, Track.BoneRef));
			}
			Controller.CloseBracket(false);
			return bOk;
		}

		bool ApplyMontageStructure(UAnimMontage& Montage,
			const FTimelineAnimationStructure& Structure,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			const FString NativeClock = TEXT("animation_montage:") + Montage.GetPathName();
			if (Structure.PlaybackRange.ClockId != NativeClock) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_c4_exact_time_mismatch"), TEXT("The montage playback range must use the montage native clock."), TEXT("/playback_range/clock_id"), MakeShared<FJsonValueString>(Structure.PlaybackRange.ClockId), MakeShared<FJsonValueString>(NativeClock), Montage.GetPathName()));
			double Start = 0.0, End = 0.0;
			if (!Structure.PlaybackRange.Lower.Point.IsSet() || !Structure.PlaybackRange.Upper.Point.IsSet())
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_playback_range_closed_required"),
					TEXT("Montage playback requires both range endpoints."),
					!Structure.PlaybackRange.Lower.Point.IsSet() ? TEXT("/playback_range/lower/point") : TEXT("/playback_range/upper/point"), false, true, Montage.GetPathName()));
			if (!TryPointSeconds(*Structure.PlaybackRange.Lower.Point, Start, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/playback_range/lower/point"), OutError);
			if (!TryPointSeconds(*Structure.PlaybackRange.Upper.Point, End, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/playback_range/upper/point"), OutError);
			Montage.SetCompositeLength(static_cast<float>(FMath::Max(0.0, End - Start)));
			Montage.CompositeSections.Reset();
			Montage.SlotAnimTracks.Reset();
			for (const auto& Binding : Structure.Bindings)
			{
				FSlotAnimationTrack Slot;
				Slot.SlotName = FName(*Binding.BindingRef);
				Montage.SlotAnimTracks.Add(MoveTemp(Slot));
				for (const auto& Track : Binding.Tracks)
					for (const auto& Section : Track.Sections)
					{
						double SectionStart = 0.0;
						if (!Section.Range.Lower.Point.IsSet()) return Authoring::Timeline::Json::ValueFailure(Section.SourceRecord, TEXT("/range/lower/point"), TEXT("timeline_animation_montage_section_start_required"), TEXT("a lower endpoint for the montage section"), nullptr, OutError);
						if (!TryPointSeconds(*Section.Range.Lower.Point, SectionStart, OutError))
							return Authoring::Timeline::Json::PrefixChildFailure(Section.SourceRecord, TEXT("/range/lower/point"), OutError);
						if (Montage.AddAnimCompositeSection(
								FName(*Section.SectionRef), static_cast<float>(SectionStart))
							== INDEX_NONE)
							return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_montage_section_add_failed"), TEXT("Unreal could not add the montage section at the requested start time."), TEXT("/range/lower/point"), MakeShared<FJsonValueNumber>(SectionStart), MakeShared<FJsonValueString>(TEXT("a start time accepted by AddAnimCompositeSection")), Section.SectionRef));
					}
			}
			return true;
		}

		bool AddLevelSequenceFloatTrack(
			UMovieScene& Scene,
			const TOptional<FGuid>& BindingGuid,
			const FTimelineAnimationTrack& Track,
			const FString& NativeClock,
			const FFrameRate& TickResolution,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (Track.TrackKind != TEXT("float")
				&& Track.TrackKind != UMovieSceneFloatTrack::StaticClass()->GetName()
				&& Track.TrackKind
					!= TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack"))
			{
				return Authoring::Timeline::Json::ValueFailure(Track.SourceRecord, TEXT("/track_kind"), TEXT("timeline_animation_level_sequence_track_kind_unsupported"), TEXT("float, MovieSceneFloatTrack, or /Script/MovieSceneTracks.MovieSceneFloatTrack"), MakeShared<FJsonValueString>(Track.TrackKind), OutError);
			}
			UMovieSceneFloatTrack* NativeTrack = BindingGuid.IsSet()
				? Scene.AddTrack<UMovieSceneFloatTrack>(*BindingGuid)
				: Scene.AddTrack<UMovieSceneFloatTrack>();
			if (!NativeTrack)
			{
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_track_create_failed"), TEXT("Unreal did not create the requested float track."), TEXT("/native_track"), false, true, Track.TrackRef));
			}
			NativeTrack->SetDisplayName(FText::FromString(
				Track.DisplayName.IsEmpty() ? Track.TrackRef : Track.DisplayName));
			if (BindingGuid.IsSet() && !Track.PropertyPath.IsEmpty())
			{
				NativeTrack->SetPropertyNameAndPath(
					FName(*Track.PropertyPath), Track.PropertyPath);
			}
			for (const FTimelineAnimationSection& Section : Track.Sections)
			{
				if (Section.SectionKind != TEXT("float")
					&& Section.SectionKind != UMovieSceneFloatTrack::StaticClass()->GetName()
					&& Section.SectionKind
						!= TEXT("/Script/MovieSceneTracks.MovieSceneFloatSection"))
				{
					return Authoring::Timeline::Json::ValueFailure(Section.SourceRecord, TEXT("/section_kind"), TEXT("timeline_animation_level_sequence_section_kind_unsupported"), TEXT("float, MovieSceneFloatTrack, or /Script/MovieSceneTracks.MovieSceneFloatSection"), MakeShared<FJsonValueString>(Section.SectionKind), OutError);
				}
				TRange<FFrameNumber> NativeRange;
				if (!TrySectionRange(Section.Range, NativeClock, TickResolution, NativeRange, OutError))
					return Authoring::Timeline::Json::PrefixChildFailure(Section.SourceRecord, TEXT("/range"), OutError);
				UMovieSceneFloatSection* NativeSection =
					Cast<UMovieSceneFloatSection>(NativeTrack->CreateNewSection());
				if (!NativeSection)
				{
					return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_section_create_failed"), TEXT("Unreal did not create a float section for the track."), TEXT("/native_section"), false, true, Section.SectionRef));
				}
				UE::UAI::Contract::FDiagnostic RangeError;
				if (!Authoring::Timeline::TrySetMovieSceneSectionRange(*NativeSection, NativeRange, RangeError))
					return Fail(OutError, RangeError);
				NativeTrack->AddSection(*NativeSection);
				FMovieSceneFloatChannel* Channel = NativeSection->
					GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
				if (!Channel)
				{
					return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_channel_missing"), TEXT("The new float section has no float channel at index 0."), TEXT("/native_channels/0"), false, true, Section.SectionRef));
				}
				Channel->Reset();
				for (const FTimelineAnimationKey& Key : Section.Keys)
				{
					if (Key.ValueType != TEXT("decimal")
						&& Key.ValueType != TEXT("integer"))
					{
						return Authoring::Timeline::Json::ValueFailure(Key.SourceRecord, TEXT("/value_type"), TEXT("timeline_animation_level_sequence_key_type_unsupported"), TEXT("decimal or integer"), MakeShared<FJsonValueString>(Key.ValueType), OutError);
					}
					int64 Tick = 0;
					if (!TryPointTick(Key.Time, NativeClock, TickResolution, Tick, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(Key.SourceRecord, TEXT("/time"), OutError);
					if (Tick < MIN_int32 || Tick > MAX_int32)
						return Authoring::Timeline::Json::ValueFailure(Key.SourceRecord, TEXT("/time/tick"), TEXT("timeline_animation_key_tick_out_of_range"), TEXT("tick from -2147483648 through 2147483647"), MakeShared<FJsonValueString>(::LexToString(Tick)), OutError);
					float Value = 0;
					if (!ScalarChannel::Parse(Key.ValueLexeme, Value, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(Key.SourceRecord, TEXT("/value_lexeme"), OutError);
					FMovieSceneFloatValue NativeValue(Value);
					if (!Key.Properties.IsValid()) NativeValue.InterpMode = RCIM_Linear;
					if (!ScalarChannel::DecodeProperties(Key, NativeValue, NativeSection, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(Key.SourceRecord, TEXT("/properties"), OutError);
					Channel->GetData().AddKey(FFrameNumber(static_cast<int32>(Tick)), NativeValue);
				}
			}
			return true;
		}

		bool ApplyLevelSequenceStructure(ULevelSequence& Sequence,
			const FTimelineAnimationStructure& Structure,
			const FString& NativeClock,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (Structure.Folders.IsSet() && !Structure.Folders->IsEmpty())
				return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_folders_require_existing_sequence"), TEXT("Initial structure creation cannot apply folders; folder edits require an existing sequence."), TEXT("/folders/count"), MakeShared<FJsonValueNumber>(Structure.Folders->Num()), MakeShared<FJsonValueNumber>(0), Sequence.GetPathName()));
			for (const auto& Binding : Structure.Bindings)
				if (Binding.Tags.IsSet() && !Binding.Tags.GetValue().IsEmpty())
					return Authoring::Timeline::Json::ValueFailure(Binding.SourceRecord, TEXT("/tags"), TEXT("timeline_animation_binding_tags_require_existing_binding"), TEXT("no tags while creating the initial binding"), Binding.SourceRecord ? Binding.SourceRecord->TryGetField(TEXT("tags")) : nullptr, OutError);
			UMovieScene* Scene = Sequence.GetMovieScene();
			FFrameRate Display, Tick;
			if (!Scene) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_movie_scene_unavailable"), TEXT("The level sequence has no MovieScene."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));
			if (Structure.PlaybackRange.ClockId != NativeClock) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_c4_exact_time_mismatch"), TEXT("The playback range must use the level sequence native clock."), TEXT("/playback_range/clock_id"), MakeShared<FJsonValueString>(Structure.PlaybackRange.ClockId), MakeShared<FJsonValueString>(NativeClock), Sequence.GetPathName()));
			if (!TryRate(Structure.DisplayRate, Display, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/display_rate"), OutError);
			if (!TryRate(Structure.TickResolution, Tick, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/tick_resolution"), OutError);
			if (!Structure.PlaybackRange.Lower.Point.IsSet() || !Structure.PlaybackRange.Upper.Point.IsSet())
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_playback_range_closed_required"),
					TEXT("Level sequence playback requires both range endpoints."),
					!Structure.PlaybackRange.Lower.Point.IsSet() ? TEXT("/playback_range/lower/point") : TEXT("/playback_range/upper/point"), false, true, Sequence.GetPathName()));
			int64 Lower = 0, Upper = 0;
			if (!TryPointTick(*Structure.PlaybackRange.Lower.Point, Structure.PlaybackRange.ClockId, Tick, Lower, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/playback_range/lower/point"), OutError);
			if (!TryPointTick(*Structure.PlaybackRange.Upper.Point, Structure.PlaybackRange.ClockId, Tick, Upper, OutError))
				return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/playback_range/upper/point"), OutError);
			if (Lower < MIN_int32 || Upper > MAX_int32 || Upper < Lower) {
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("lower_tick"), ::LexToString(Lower));
				Actual->SetStringField(TEXT("upper_tick"), ::LexToString(Upper));
				return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_playback_range_invalid"), TEXT("The playback interval must fit the accepted frame bounds and have upper >= lower."), TEXT("/playback_range"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("lower >= -2147483648; upper <= 2147483647; upper >= lower")), Sequence.GetPathName()));
			}
			Scene->Modify();
			Scene->SetDisplayRate(Display);
			Scene->SetTickResolutionDirectly(Tick);
			Scene->SetPlaybackRange(FFrameNumber(static_cast<int32>(Lower)),
				static_cast<int32>(Upper - Lower));
			TArray<UMovieSceneTrack*> ExistingRootTracks;
			for (UMovieSceneTrack* Existing : Scene->GetTracks())
			{
				ExistingRootTracks.Add(Existing);
			}
			for (UMovieSceneTrack* Existing : ExistingRootTracks)
			{
				if (Existing)
				{
					Scene->RemoveTrack(*Existing);
				}
			}
			TArray<FGuid> BindingIds;
			for (const FMovieSceneBinding& Existing : Scene->GetBindings())
			{
				BindingIds.Add(Existing.GetObjectGuid());
			}
			for (const FGuid& BindingId : BindingIds)
			{
				Scene->RemovePossessable(BindingId);
				Scene->RemoveSpawnable(BindingId);
			}
			for (const FTimelineAnimationBinding& Binding : Structure.Bindings)
			{
				if (Binding.BindingKind != TEXT("possessable"))
				{
					return Authoring::Timeline::Json::ValueFailure(Binding.SourceRecord, TEXT("/binding_kind"), TEXT("timeline_animation_level_sequence_binding_kind_unsupported"), TEXT("possessable"), MakeShared<FJsonValueString>(Binding.BindingKind), OutError);
				}
				const FGuid BindingGuid = Scene->AddPossessable(
					Binding.BindingRef, UObject::StaticClass());
				if (!BindingGuid.IsValid())
				{
					return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_binding_create_failed"), TEXT("Unreal did not create a valid possessable binding."), TEXT("/native_binding"), false, true, Binding.BindingRef));
				}
				for (const FTimelineAnimationTrack& Track : Binding.Tracks)
				{
					if (!AddLevelSequenceFloatTrack(
							*Scene,
							TOptional<FGuid>(BindingGuid),
							Track,
							NativeClock,
							Tick,
							OutError))
					{
						return false;
					}
				}
			}
			for (const FTimelineAnimationTrack& Track : Structure.RootTracks)
			{
				if (!AddLevelSequenceFloatTrack(
						*Scene,
						TOptional<FGuid>(),
						Track,
						NativeClock,
						Tick,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool ApplyLevelSequenceSettings(
			ULevelSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			UMovieScene* Scene = Sequence.GetMovieScene();
			if (!Scene)
			{
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_movie_scene_unavailable"), TEXT("The level sequence has no MovieScene."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));
			}

			Authoring::Timeline::FPreparedMovieSceneSettings Settings;
			if (!Authoring::Timeline::TryPrepareMovieSceneSettings(*Scene,
				TEXT("level_sequence:") + Sequence.GetPathName(), Data.DisplayRate, Data.TemporalRange, Settings, OutError)) return false;
			if (!Settings.HasChanges())
				return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_settings_no_change"), TEXT("The requested display rate and playback range produce no native settings change."), TEXT("/settings/has_changes"), false, true, Sequence.GetPathName()));
			Settings.Apply(*Scene);
			return true;
		}

		UObject* CreateAsset(const FUeAgentTimelineAnimationRequest& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FString PackageName, AssetName;
			if (!SplitAssetRef(Request.GetData().DestinationRef, PackageName, AssetName))
			{
				Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_destination_ref_invalid"), TEXT("The destination must identify an asset package and object name."), TEXT("/destination_ref"), MakeShared<FJsonValueString>(Request.GetData().DestinationRef), MakeShared<FJsonValueString>(TEXT("asset path under /Game/ with a nonempty asset name")), Request.GetData().DestinationRef));
				return nullptr;
			}
			if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName)))
			{
				Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_destination_exists"), TEXT("An object already exists at the requested destination."), TEXT("/destination_exists"), true, false, Request.GetData().DestinationRef));
				return nullptr;
			}
			UPackage* Package = nullptr;
			UObject* Asset = nullptr;
			if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate)
			{
				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr,
					*Request.GetData().SkeletonRef, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Skeleton) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_montage_create_dependency_missing"), TEXT("The montage skeleton reference could not be loaded as a Skeleton."), TEXT("/skeleton_ref"), MakeShared<FJsonValueString>(Request.GetData().SkeletonRef), MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName()), Request.GetData().SkeletonRef)), nullptr;
				Package = CreatePackage(*PackageName);
				UAnimMontage* Montage = NewObject<UAnimMontage>(Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Montage) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_montage_create_failed"), TEXT("Unreal did not create the montage object in the destination package."), TEXT("/created_object"), false, true, Request.GetData().DestinationRef)), nullptr;
				Montage->SetSkeleton(Skeleton);
				if (!ApplyMontageStructure(*Montage, *Request.GetData().Structure, OutError))
				{
					Asset = Montage;
					DiscardUnregisteredAsset(Asset);
					return nullptr;
				}
				Asset = Montage;
			}
			else
			{
				Package = CreatePackage(*PackageName);
				ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Sequence) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_create_failed"), TEXT("Unreal did not create the level sequence object in the destination package."), TEXT("/created_object"), false, true, Request.GetData().DestinationRef)), nullptr;
				Sequence->Initialize();
				if (!ApplyLevelSequenceStructure(
						*Sequence,
						*Request.GetData().Structure,
						TEXT("level_sequence:") + Sequence->GetPathName(),
						OutError))
				{
					Asset = Sequence;
					DiscardUnregisteredAsset(Asset);
					return nullptr;
				}
				Asset = Sequence;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Asset;
		}

		class FReadback final : public IUeAgentTimelineAnimationReadbackPort
		{
		public:
			explicit FReadback(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TryRead(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentTimelineAnimationReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutSnapshot.Reset(); OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString Ref = ExplicitRef(*Typed, Target);
				if (Ref.IsEmpty()) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_explicit_target_required"), TEXT("The operation requires an explicit public target reference."), TEXT("/target_ref"), MakeShared<FJsonValueString>(Ref), MakeShared<FJsonValueString>(TEXT("a nonempty public target reference")), Contract.GetCapabilityId()));
				if (Phase == EUeAgentTimelineAnimationReadPhase::Preflight
					&& (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
						|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate))
				{
					// StaticLoadObject can create an empty package even when loading fails.
					// Creation preflight must inspect absence without resolving that path.
					bool bPresent = false;
					if (!HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(Ref, bPresent))
						return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_destination_ref_invalid"), TEXT("The creation destination cannot be interpreted as an asset object reference."), TEXT("/destination_ref"), MakeShared<FJsonValueString>(Ref), MakeShared<FJsonValueString>(TEXT("asset path under /Game/ with a nonempty asset name")), Ref));
					if (!bPresent)
						return TrySnapshot(Contract, Phase, Ref, TEXT("absent:") + Ref, false, nullptr,
							Context, OutSnapshot, OutError);
				}
				UObject* Object = Environment->Resolve(Ref, OutError);
				if (!IsValid(Object)) return OutError.IsValid() ? false : Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_explicit_target_unavailable"), TEXT("The explicit target could not be resolved to a live Unreal object."), TEXT("/native_target"), false, true, Ref));
				const EUeAgentTimelineAnimationRequestKind Kind = Typed->GetKind();
				if ((Kind == EUeAgentTimelineAnimationRequestKind::SequenceRead
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequencePoseSample)
					&& !Object->IsA<UAnimSequence>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_sequence_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UAnimSequence::StaticClass()->GetPathName()), Ref));
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::BlendSpaceSample
					&& !Object->IsA<UBlendSpace>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_blend_space_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UBlendSpace::StaticClass()->GetPathName()), Ref));
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::MontageRead
					&& !Object->IsA<UAnimMontage>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_montage_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UAnimMontage::StaticClass()->GetPathName()), Ref));
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead
					&& !Object->IsA<USkeleton>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_skeleton_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName()), Ref));
				}
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceRead
					&& !Object->IsA<ULevelSequence>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_level_sequence_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ULevelSequence::StaticClass()->GetPathName()), Ref));
				}
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead
					&& !Object->IsA<UWidgetAnimation>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_widget_animation_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UWidgetAnimation::StaticClass()->GetPathName()), Ref));
				}

				FString State;
				if (const auto* Subject = Environment->FindSubject(Ref))
				{
					FFrameRate EvaluationRate;
					int64 EvaluationTick = 0;
					if (!Typed->GetData().EvaluationPoint.IsSet()) return Fail(OutError,
						TimelineNativePresence(TEXT("timeline_animation_pose_time_required"), TEXT("Subject readback requires an evaluation point."), TEXT("/evaluation_point"), false, true, Ref));
					if (!TryRate({Subject->EvaluationRateNumerator, Subject->EvaluationRateDenominator}, EvaluationRate, OutError)) return false;
					if (!TryPointTick(*Typed->GetData().EvaluationPoint, Subject->EvaluationClockId, EvaluationRate, EvaluationTick, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/evaluation_point"), OutError);
					State = GenericObjectState(Object);
					if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate)
					{
						UPoseableMeshComponent* Component = Subject->PoseComponent.Get();
						if (!Component) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_pose_component_unavailable"), TEXT("The subject binding no longer has its required pose component."), TEXT("/subject/pose_component"), false, true, Ref));
						for (const auto& Item : Typed->GetData().Pose)
							State += TEXT("|pose=") + Item.BoneRef + TEXT(":")
								+ TransformText(Component->GetBoneTransformByName(
									FName(*Item.BoneRef), EBoneSpaces::ComponentSpace));
					}
					else
					{
						USkeletalMeshComponent* Component = Subject->MorphComponent.Get();
						if (!Component) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_morph_component_unavailable"), TEXT("The subject binding no longer has its required morph component."), TEXT("/subject/morph_component"), false, true, Ref));
						for (const auto& Item : Typed->GetData().MorphWeights)
							State += FString::Printf(TEXT("|morph=%s:%.9g"),
								*Item.MorphRef, Component->GetMorphTarget(FName(*Item.MorphRef)));
					}
				}
				else if (const auto* Rig = Environment->FindRig(Ref))
				{
					int64 Tick = 0;
					FFrameRate Rate;
					if (!TryRate({Rig->AuthoredRateNumerator, Rig->AuthoredRateDenominator}, Rate, OutError)) return false;
					if (!Typed->GetData().AuthoredTime.IsSet()) return Fail(OutError,
						TimelineNativePresence(TEXT("timeline_animation_rig_authored_time_required"), TEXT("Rig readback requires an authored time."), TEXT("/authored_time"), false, true, Ref));
					if (!TryPointTick(*Typed->GetData().AuthoredTime, Rig->AuthoredClockId, Rate, Tick, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/authored_time"), OutError);
					FTransform Value = FTransform::Identity;
					bool bObservedKey = false;
					if (const UMovieScene* Scene = Rig->Sequence->GetMovieScene())
					{
						const FFrameTime TickTime = FFrameRate::TransformTime(
							FFrameTime(FFrameNumber(static_cast<int32>(Tick))),
							Rate, Scene->GetTickResolution());
						VisitMovieSceneTracks(*Scene,
							[&](UMovieSceneTrack* SceneTrack)
							{
							const UMovieSceneControlRigParameterTrack* Track =
								Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
							if (!Track || Track->GetControlRig() != Rig->Rig.Get()) return false;
							for (UMovieSceneSection* SceneSection : Track->GetAllSections())
							{
								UMovieSceneControlRigParameterSection* Section =
									Cast<UMovieSceneControlRigParameterSection>(SceneSection);
								if (!Section) continue;
								const FTransformParameterNameAndCurves* Curves =
									Section->GetTransformParameterNamesAndCurves().FindByPredicate(
										[&](const FTransformParameterNameAndCurves& Item)
										{
											return Item.ParameterName == Rig->ControlName;
										});
								if (Curves)
								{
									float Translation[3] = {0.0f, 0.0f, 0.0f};
									float Rotation[3] = {0.0f, 0.0f, 0.0f};
									float Scale[3] = {1.0f, 1.0f, 1.0f};
									bObservedKey = Curves->Translation[0].Evaluate(TickTime, Translation[0])
										&& Curves->Translation[1].Evaluate(TickTime, Translation[1])
										&& Curves->Translation[2].Evaluate(TickTime, Translation[2])
										&& Curves->Rotation[0].Evaluate(TickTime, Rotation[0])
										&& Curves->Rotation[1].Evaluate(TickTime, Rotation[1])
										&& Curves->Rotation[2].Evaluate(TickTime, Rotation[2])
										&& Curves->Scale[0].Evaluate(TickTime, Scale[0])
										&& Curves->Scale[1].Evaluate(TickTime, Scale[1])
										&& Curves->Scale[2].Evaluate(TickTime, Scale[2]);
									if (bObservedKey)
									{
										Value = FTransform(
											FRotator(Rotation[1], Rotation[2], Rotation[0]),
											FVector(Translation[0], Translation[1], Translation[2]),
											FVector(Scale[0], Scale[1], Scale[2]));
										break;
									}
								}
							}
							return bObservedKey;
						});
					}
					if (!bObservedKey
						&& Phase == EUeAgentTimelineAnimationReadPhase::Preflight)
					{
						Value = Rig->Rig->GetControlLocalTransform(Rig->ControlName);
					}
					else if (!bObservedKey)
					{
						Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_rig_key_readback_unavailable"), TEXT("No readable transform curves were found for the rig control at the requested time."), TEXT("/goal_ref"), MakeShared<FJsonValueString>(Rig->ControlName.ToString()), MakeShared<FJsonValueString>(TEXT("a control with readable translation, rotation, and scale curves")), Ref));
						return false;
					}
					State = GenericObjectState(Rig->Sequence.Get()) + TEXT("|rig=")
						+ Rig->Rig->GetPathName() + TEXT("|goal=")
						+ Rig->ControlName.ToString() + TEXT(":") + TransformText(Value);
				}
				else State = QueryState(Object, *Typed, OutError);
				FTimelineAnimationRevision NativeRevision;
				if (State.IsEmpty()) {
					if (OutError.IsEmpty()) OutError = TimelineNativePresence(TEXT("timeline_animation_native_state_unavailable"), TEXT("Native readback returned no observable state."), TEXT("/observed_state"), false, true, Ref);
					return false;
				}
				if (!Environment->TryReadRevision(Ref, NativeRevision, OutError)) return false;
				const FUeAgentStateRevision SharedRevision(
					NativeRevision.SourceId, NativeRevision.Generation,
					NativeRevision.Sequence);
				FUeAgentCanonicalValuePtr ObservedValue;
				if (Kind == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate ||
					Kind == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate ||
					Kind == EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate) {
					UE::UAI::Contract::FDiagnostic Error;
					const auto Document = UeAgentAnimationAuthoring::ReadSequenceStructure(*CastChecked<UAnimSequence>(Object), Error);
					if (!Document) return Fail(OutError, Error, Ref);
					const auto Observation = MakeShared<FJsonObject>();
					Observation->SetStringField(TEXT("observed_state"), State);
					Observation->SetStringField(TEXT("public_ref"), Ref);
					Observation->SetObjectField(TEXT("sequence_document"), Document);
					const auto Envelope = MakeShared<FJsonObject>();
					Envelope->SetObjectField(TEXT("result"), Observation);
					if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Envelope),
						Context.GetResourceTracker(), ObservedValue, Error))
						return Fail(OutError, Error, Ref);
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::LevelSequenceRead
					&& !TryBuildLevelSequenceObserved(*CastChecked<ULevelSequence>(Object), Ref,
						Typed->GetData(), Context, ObservedValue, OutError)) return false;
				const FString ObservedRef =
					(Kind == EUeAgentTimelineAnimationRequestKind::MontageCreate
						|| Kind == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate)
					? Object->GetPathName()
					: Ref;
				return !State.IsEmpty() && TrySnapshot(Contract, Phase, ObservedRef, State, true,
					&SharedRevision, Context, OutSnapshot, OutError, ObservedValue);
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FMutation final : public IUeAgentTimelineAnimationMutationPort
		{
		public:
			explicit FMutation(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TryMutate(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentTimelineAnimationMutationResult& OutResult,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutResult = {}; OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				if (!Typed->IsOperation()) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_mutation_request_required"), TEXT("The mutation port requires an operation request."), TEXT("/is_operation"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId()));
				const FString Ref = ExplicitRef(*Typed, FirstTarget(Targets));
				if (Ref.IsEmpty()) return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_explicit_target_required"), TEXT("The operation requires an explicit public target reference."), TEXT("/target_ref"), MakeShared<FJsonValueString>(Ref), MakeShared<FJsonValueString>(TEXT("a nonempty public target reference")), Contract.GetCapabilityId()));
				UObject* Object = nullptr;
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
					|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate)
					Object = CreateAsset(*Typed, OutError);
				else Object = Environment->Resolve(Ref, OutError);
				const auto& Data = Typed->GetData();
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate
					|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate)
				{
					const auto* Binding = Environment->FindSubject(Ref);
					FFrameRate EvaluationRate;
					int64 EvaluationTick = 0;
					if (!Binding) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_subject_binding_unavailable"), TEXT("No subject binding exists for the requested target."), TEXT("/subject_binding"), false, true, Ref));
					if (!Data.EvaluationPoint.IsSet()) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_pose_time_required"), TEXT("Subject mutation requires an evaluation point."), TEXT("/evaluation_point"), false, true, Ref));
					if (!TryRate({Binding->EvaluationRateNumerator, Binding->EvaluationRateDenominator}, EvaluationRate, OutError)) return false;
					if (!TryPointTick(*Data.EvaluationPoint, Binding->EvaluationClockId, EvaluationRate, EvaluationTick, OutError))
						return Authoring::Timeline::Json::PrefixChildFailure(nullptr, TEXT("/evaluation_point"), OutError);
					Object = Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate
						? static_cast<UObject*>(Binding->PoseComponent.Get())
						: static_cast<UObject*>(Binding->MorphComponent.Get());
				}
				if (!IsValid(Object)) return OutError.IsValid() ? false : Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_explicit_target_unavailable"), TEXT("The explicit target could not be resolved to a live Unreal object."), TEXT("/native_target"), false, true, Ref));
				const EUeAgentTimelineAnimationRequestKind Kind = Typed->GetKind();
				if ((Kind == EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate)
					&& !Object->IsA<UAnimSequence>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_sequence_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UAnimSequence::StaticClass()->GetPathName()), Ref));
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::MontageStructureReplace
					&& !Object->IsA<UAnimMontage>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_montage_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UAnimMontage::StaticClass()->GetPathName()), Ref));
				}
				if ((Typed->GetKind()
						== EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace
					|| Typed->GetKind()
						== EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate)
					&& !Object->IsA<ULevelSequence>())
				{
					return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_level_sequence_type_mismatch"), TEXT("The resolved target has the wrong native class for this operation."), TEXT("/target_class"), MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ULevelSequence::StaticClass()->GetPathName()), Ref));
				}

				if (Data.ExpectedRevision.IsSet()
					&& !CheckRevision(*Environment, Ref, *Data.ExpectedRevision, OutError)) return false;
				if (Kind == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate ||
					Kind == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate ||
					Kind == EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate) {
					using namespace UeAgentAnimationAuthoring;
					TSharedPtr<FJsonValue> Input;
					UE::UAI::Contract::FDiagnostic Error;
					if (!CanonicalJson::TryToJsonValue(*Typed->GetIntentPayload(), Input, Error) ||
						!RegisterAuthoring(Error))
						return Fail(OutError, Error, Ref);
					FAssetApplyRequest Apply;
					Apply.Kind = EAssetKind::AnimSequence;
					Apply.AssetPath = Object->GetOutermost()->GetName();
					Apply.bCompile = true;
					Apply.bSave = true;
					Apply.Document = MakeShared<FJsonObject>();
					const TCHAR* Field = Kind == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate
						? TEXT("curve_changes") : Kind == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate
						? TEXT("metadata_changes") : TEXT("event_changes");
					Apply.Document->SetField(Field, Input->AsObject()->TryGetField(Field));
					TSharedPtr<FJsonObject> VerifiedDocument;
					Apply.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
						auto* Settled = Cast<UAnimSequence>(LoadAsset(EAssetKind::AnimSequence, Apply.AssetPath));
						if (Settled) VerifiedDocument = ReadSequenceStructure(*Settled, Failure.Diagnostic);
						if (!VerifiedDocument && Failure.Diagnostic.IsEmpty()) Failure.Diagnostic = TimelineNativePresence(TEXT("timeline_animation_document_readback_failed"), TEXT("The settled animation sequence did not produce a readable sequence document."), TEXT("/sequence_document"), false, true, Apply.AssetPath);
						return VerifiedDocument.IsValid();
					};
					Environment->PreparePackageUnload(Object->GetOutermost());
					FAssetApplyResult Applied;
					if (!ApplyAsset(Apply, Applied, Context.GetRequestId()) || !Applied.IsApplied()) {
						if (!Applied.Diagnostic.IsEmpty()) return Fail(OutError, Applied.Diagnostic, Ref);
						return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_authoring_apply_failed"), TEXT("The animation authoring provider did not apply the requested document and returned no diagnostic."), TEXT("/authoring/applied"), false, true, Ref));
					}
					Object = LoadAsset(EAssetKind::AnimSequence, Apply.AssetPath);
					const FString OperationId = Context.GetRequestId() + TEXT(":") + Contract.GetCapabilityId();
					Environment->RecordMutation(OperationId, Object);
					Environment->RecordVerifiedDocument(OperationId, VerifiedDocument);
					OutResult = FUeAgentTimelineAnimationMutationResult(OperationId, TEXT("atomic-authoring:") + Apply.AssetPath);
					if (!OutResult.IsValid()) return Fail(OutError, OutResult.GetValidationDiagnostic());
					return true;
				}
				Object->Modify();
				bool bApplied = true;
				switch (Typed->GetKind())
				{
				case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
					bApplied = ApplySequenceTracks(*CastChecked<UAnimSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
					bApplied = ApplyMontageStructure(*CastChecked<UAnimMontage>(Object), *Data.Structure, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
					bApplied = ApplyLevelSequenceStructure(
						*CastChecked<ULevelSequence>(Object),
						*Data.Structure,
						TEXT("level_sequence:") + Object->GetPathName(),
						OutError); break;
				case EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate:
					bApplied = ApplyLevelSequenceSettings(
						*CastChecked<ULevelSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
				{
					const auto* Binding = Environment->FindSubject(Ref);
					UPoseableMeshComponent* Component = Binding ? Binding->PoseComponent.Get() : nullptr;
					if (!Component) { bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_pose_component_unavailable"), TEXT("The subject binding no longer has its required pose component."), TEXT("/subject/pose_component"), false, true, Ref)); break; }
					for (const auto& Item : Data.Pose)
					{
						FTransform Value;
						if (!Item.Transform.TryToNativeTransform(Binding->NativeFrameId, Value, OutError)) { bApplied = false; break; }
						if (Data.Weight.IsSet())
						{
							const FTransform Current = Component->GetBoneTransformByName(
								FName(*Item.BoneRef), EBoneSpaces::ComponentSpace);
							FTransform Blended;
							Blended.Blend(Current, Value, *Data.Weight);
							Value = Blended;
						}
						Component->SetBoneTransformByName(FName(*Item.BoneRef), Value,
							EBoneSpaces::ComponentSpace);
					}
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
				{
					const auto* Binding = Environment->FindSubject(Ref);
					USkeletalMeshComponent* Component = Binding ? Binding->MorphComponent.Get() : nullptr;
					if (!Component) { bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_morph_component_unavailable"), TEXT("The subject binding no longer has its required morph component."), TEXT("/subject/morph_component"), false, true, Ref)); break; }
					for (const auto& Item : Data.MorphWeights)
						Component->SetMorphTarget(FName(*Item.MorphRef), Item.Weight, false);
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
				{
					const auto* Binding = Environment->FindRig(Ref);
					if (!Binding) { bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_rig_binding_unavailable"), TEXT("No rig binding exists for the requested target."), TEXT("/rig_binding"), false, true, Ref)); break; }
					if (!Data.Candidate.IsSet()) { bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_rig_candidate_required"), TEXT("Applying a rig probe requires a candidate transform."), TEXT("/candidate"), false, true, Ref)); break; }
					if (!Data.AuthoredTime.IsSet()) { bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_rig_authored_time_required"), TEXT("Applying a rig probe requires an authored time."), TEXT("/authored_time"), false, true, Ref)); break; }
					FTransform Value;
					FFrameRate Rate;
					int64 Tick = 0;
					if (!Data.Candidate->TryToNativeTransform(Binding->NativeFrameId, Value, OutError)
						|| !TryRate({Binding->AuthoredRateNumerator,
							Binding->AuthoredRateDenominator}, Rate, OutError)
						|| !TryPointTick(*Data.AuthoredTime, Binding->AuthoredClockId,
							Rate, Tick, OutError)) { bApplied = false; break; }
					UMovieScene* Scene = Binding->Sequence->GetMovieScene();
					if (!Scene)
					{
						bApplied = Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_rig_movie_scene_unavailable"), TEXT("The rig sequence has no MovieScene."), TEXT("/rig_sequence/movie_scene"), false, true, Ref));
						break;
					}
					const FFrameNumber KeyFrame = FFrameRate::TransformTime(
						FFrameTime(FFrameNumber(static_cast<int32>(Tick))), Rate,
						Scene->GetTickResolution()).RoundToFrame();
					UMovieSceneControlRigParameterSection* BoundSection = nullptr;
					VisitMovieSceneTracks(*Scene,
						[&](UMovieSceneTrack* SceneTrack)
						{
						UMovieSceneControlRigParameterTrack* Track =
							Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
						if (!Track || Track->GetControlRig() != Binding->Rig.Get()) return false;
						for (UMovieSceneSection* SceneSection : Track->GetAllSections())
						{
							BoundSection = Cast<UMovieSceneControlRigParameterSection>(SceneSection);
							if (BoundSection) break;
						}
						return BoundSection != nullptr;
					});
					if (!BoundSection)
					{
						bApplied = Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_rig_parameter_section_unavailable"), TEXT("The rig sequence has no parameter section bound to the requested control rig."), TEXT("/rig_ref"), MakeShared<FJsonValueString>(Binding->Rig->GetPathName()), MakeShared<FJsonValueString>(TEXT("a bound MovieSceneControlRigParameterSection")), Ref));
						break;
					}
					BoundSection->Modify();
					BoundSection->AddTransformParameterKey(Binding->ControlName, KeyFrame, Value);
					Binding->Rig->SetControlLocalTransform(Binding->ControlName, Value, true);
					Scene->Modify();
					Binding->Sequence->Modify();
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::MontageCreate:
				case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
					break;
				default:
					bApplied = Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_native_operation_unsupported"), TEXT("The native mutation port has no implementation for this capability."), TEXT("/capability_id"), MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("a supported timeline mutation capability")), Ref));
					break;
				}
				if (!bApplied) return false;
				Object->MarkPackageDirty();
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, Object);
				OutResult = FUeAgentTimelineAnimationMutationResult(OperationId,
					TEXT("native-mutation:") + Object->GetPathName());
				if (!OutResult.IsValid()) return Fail(OutError, OutResult.GetValidationDiagnostic());
				return true;
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FSettle final : public IUeAgentTimelineAnimationSettlePort
		{
		public:
			explicit FSettle(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TrySettle(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutError = {};
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				// These requests already saved/reloaded/verified inside the shared transaction.
				if (Environment->FindVerifiedDocument(Mutation.GetOperationId()).IsValid()) return true;
				if (!IsValid(Object)) {
					auto Error = TimelineNativePresence(TEXT("timeline_animation_settle_target_unavailable"), TEXT("The recorded mutation target is no longer a live Unreal object."), TEXT("/mutation/native_target"), false, true, Contract.GetCapabilityId());
					Error.SubjectId = Mutation.GetOperationId();
					return Fail(OutError, MoveTemp(Error));
				}
				Object->PostEditChange();
				if (UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
					Sequence->RefreshCacheData();
				else if (UAnimMontage* Montage = Cast<UAnimMontage>(Object))
				{
					Montage->RefreshCacheData();
				}
				else if (ULevelSequence* LevelSequence = Cast<ULevelSequence>(Object))
				{
					UMovieSceneCompiledDataManager::GetPrecompiledData()
						->Compile(LevelSequence);
				}
				else if (UPoseableMeshComponent* Pose = Cast<UPoseableMeshComponent>(Object))
					Pose->RefreshBoneTransforms();
				else if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Object))
					Mesh->RefreshBoneTransforms();
				if (Object->IsAsset())
				{
					UPackage* Package = Object->GetOutermost();
					const FString ObjectPath = Object->GetPathName();
					UClass* const ExpectedClass = Object->GetClass();
					if (!UE::UAI::EditorResources::IsPersistentPackage(Package))
						return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_package_not_persistent"), TEXT("The mutated asset has no persistent package to save."), TEXT("/package/is_persistent"), false, true, ObjectPath));
					if (!UE::UAI::EditorResources::SavePackages({Package}, false, OutError)) {
						if (OutError.IsEmpty()) OutError = TimelineNativePresence(TEXT("timeline_animation_package_save_failed"), TEXT("The package save provider failed without a diagnostic."), TEXT("/package/saved"), false, true, ObjectPath);
						return false;
					}
					const TWeakObjectPtr<UObject> Original(Object);
					Environment->ForgetMutation(Mutation.GetOperationId());
					Environment->PreparePackageUnload(Package);
					Object = nullptr;
					FText UnloadError;
					const bool bUnloaded = UPackageTools::UnloadPackages(
						{Package}, UnloadError, true);
					CollectGarbage(RF_NoFlags, true);
					if (!bUnloaded || Original.IsValid())
					{
						auto Error = TimelineNativePresence(TEXT("timeline_animation_package_unload_failed"),
							UnloadError.IsEmpty() ? TEXT("The package did not fully unload before independent readback.") : UnloadError.ToString(),
							TEXT("/package/unloaded"), false, true, ObjectPath);
						const auto Facts = MakeShared<FJsonObject>();
						Facts->SetBoolField(TEXT("unload_returned_success"), bUnloaded);
						Facts->SetBoolField(TEXT("original_object_still_valid"), Original.IsValid());
						Error.ActualValue = MakeShared<FJsonValueObject>(Facts);
						return Fail(OutError, MoveTemp(Error));
					}
					UObject* const Reloaded = StaticLoadObject(
						ExpectedClass, nullptr, *ObjectPath, nullptr,
						LOAD_NoWarn | LOAD_Quiet);
					if (!Reloaded)
					{
						return Fail(OutError, TimelineNativeFact(TEXT("timeline_animation_package_reload_failed"), TEXT("Independent readback could not reload the saved asset with its original class."), TEXT("/reloaded_object_class"), nullptr, MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), ObjectPath));
					}
					if (ULevelSequence* const ReloadedLevelSequence =
							Cast<ULevelSequence>(Reloaded))
					{
						UMovieSceneCompiledDataManager::GetPrecompiledData()
							->Compile(ReloadedLevelSequence);
					}
					if (!Environment->TryRebindReloadedAsset(ObjectPath, Reloaded, OutError))
						return false;
					Environment->RecordMutation(Mutation.GetOperationId(), Reloaded);
				}
				return true;
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FComparator final : public IUeAgentTimelineAnimationResultComparator
		{
		public:
			explicit FComparator(TSharedRef<FProductionEnvironment> InEnvironment) : Environment(InEnvironment) {}
			bool TryCompare(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				const bool bCreate = Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
					|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate;
				FString ObservedTarget;
				const auto CaptureTarget = [&](const FUeAgentCanonicalSnapshot& Snapshot) {
					for (const auto& Entry : Snapshot.GetEntries()) {
						const auto& Identity = Entry.GetIdentity();
						if (Identity.GetTargetKind() == TEXT("timeline_animation_target") && !Identity.GetOwnerId().IsEmpty()) {
							ObservedTarget = Identity.GetOwnerId(); return;
						}
					}
				};
				CaptureTarget(After);
				if (ObservedTarget.IsEmpty()) CaptureTarget(Before);
				FDiagnostic Mismatch;
				const auto Explain = [&](const FString& Reason, const FString& Path,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
					if (!Mismatch.IsEmpty()) return;
					Mismatch = TimelineNativeFact(TEXT("timeline_animation_native_state_mismatch"), Reason, Path,
						MoveTemp(Actual), MoveTemp(Expected));
Mismatch.Stage = TEXT("verification");
					Mismatch.TargetPath = ObservedTarget;
				};
				const bool bBeforeMatches = bCreate ? Before.GetEntries().IsEmpty() : !BeforeState.IsEmpty();
				bool bMatches = bBeforeMatches && !AfterState.IsEmpty()
					&& (bCreate || BeforeState != AfterState) && !AfterState.StartsWith(TEXT("absent:"));
				if (!bBeforeMatches) {
					if (bCreate) Explain(TEXT("Creation preflight unexpectedly contains an existing target."),
						TEXT("/before/entries/count"), MakeShared<FJsonValueNumber>(Before.GetEntries().Num()), MakeShared<FJsonValueNumber>(0));
					else Explain(TEXT("Mutation preflight contains no native observed state."), TEXT("/before/observed_state/present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				} else if (AfterState.IsEmpty()) Explain(TEXT("Independent readback contains no native observed state."),
					TEXT("/after/observed_state/present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else if (!bCreate && BeforeState == AfterState) {
					Explain(TEXT("The native observed state did not change after mutation."), TEXT("/observed_state/unchanged"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
Mismatch.ActualEvidence = MakeShared<FJsonValueString>(AfterState);
				} else if (AfterState.StartsWith(TEXT("absent:"))) Explain(TEXT("The target is absent after mutation."),
					TEXT("/after/target_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				const auto ContainsAll = [&](const auto& Items, const TCHAR* Prefix, const TCHAR* Field, auto GetRef) {
					for (int32 Index = 0; Index < Items.Num(); ++Index) {
						const FString Ref = GetRef(Items[Index]);
						if (!AfterState.Contains(FString(Prefix) + Ref)) {
							Explain(TEXT("Independent readback omits the requested item: ") + Ref,
								FString::Printf(TEXT("/%s/%d"), Field, Index),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(Ref));
							return false;
						}
					}
					return true;
				};
				const auto& Data = Typed->GetData();
				switch (Typed->GetKind()) {
				case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
					bMatches &= ContainsAll(Data.BoneTracks, TEXT("|bone="), TEXT("bone_tracks"), [](const auto& V){ return V.BoneRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
					bMatches &= ContainsAll(Data.Pose, TEXT("|pose="), TEXT("pose"), [](const auto& V){ return V.BoneRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
					bMatches &= ContainsAll(Data.MorphWeights, TEXT("|morph="), TEXT("morph_weights"), [](const auto& V){ return V.MorphRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply: {
					const bool bGoalPresent = AfterState.Contains(TEXT("|goal=") + Data.GoalRef + TEXT(":"));
					bMatches &= bGoalPresent;
					if (!bGoalPresent) Explain(TEXT("Independent rig readback omits the requested goal."), TEXT("/goal_ref"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(Data.GoalRef));
					break;
				}
				default: break;
				}
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate ||
					Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate ||
					Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate) {
					const FString OperationId = Context.GetRequestId() + TEXT(":") + Contract.GetCapabilityId();
					const auto Expected = Environment->FindVerifiedDocument(OperationId);
					const auto Actual = SnapshotResultField(After, TEXT("sequence_document"));
					TSharedPtr<FJsonValue> ActualJson;
					Mismatch.Reset();
					bMatches = false;
					if (!Expected.IsValid()) Explain(TEXT("The authoring transaction retained no verified sequence document."),
						TEXT("/sequence_document/expected_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else if (!Actual.IsValid()) Explain(TEXT("Independent readback omitted the sequence document."),
						TEXT("/sequence_document/actual_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else if (!CanonicalJson::TryToJsonValue(*Actual, ActualJson, Mismatch)) {
						if (Mismatch.Source.SourceFile.IsEmpty()) Mismatch.SourceKind = TEXT("native_result");
					} else {
						const auto Difference = CanonicalJson::CompareJsonValues(MakeShared<FJsonValueObject>(Expected), ActualJson, TEXT("/sequence_document"));
						bMatches = Difference.bEquivalent;
						if (!bMatches) {
							Explain(TEXT("Independent sequence readback differs from the document verified by authoring."),
								Difference.Path, Difference.ActualValue, Difference.ExpectedValue);
							Difference.ApplyTo(Mismatch);
						}
					}
					Environment->ForgetMutation(OperationId);
				}
				FUeAgentCanonicalValuePtr PublicResult;
				if (bMatches && bCreate) {
					const auto TargetRef = SnapshotResultField(After, TEXT("public_ref"));
					if (!TargetRef.IsValid() || TargetRef->GetKind() != EUeAgentValueKind::String || TargetRef->GetText().IsEmpty()) {
						const auto Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("present"), TargetRef.IsValid());
						if (TargetRef.IsValid()) {
const auto KindName = [](EUeAgentValueKind Kind) -> const TCHAR* {
								switch (Kind) {
								case EUeAgentValueKind::Null: return TEXT("null");
								case EUeAgentValueKind::Boolean: return TEXT("boolean");
								case EUeAgentValueKind::String: return TEXT("string");
								case EUeAgentValueKind::Integer: return TEXT("integer");
								case EUeAgentValueKind::Decimal: return TEXT("decimal");
								case EUeAgentValueKind::Array: return TEXT("array");
								case EUeAgentValueKind::Object: return TEXT("object");
								case EUeAgentValueKind::Temporal: return TEXT("temporal");
								case EUeAgentValueKind::Spatial: return TEXT("spatial");
								default: return TEXT("unknown");
								}
							};
							Actual->SetStringField(TEXT("value_kind"), KindName(TargetRef->GetKind()));
							if (TargetRef->GetKind() == EUeAgentValueKind::String) Actual->SetStringField(TEXT("value"), TargetRef->GetText());
						}
						auto Error = TimelineNativeFact(TEXT("timeline_animation_created_target_ref_invalid"),
							TEXT("Created-asset readback must provide a nonempty string public_ref."), TEXT("/public_ref"),
							MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty string")));
Error.Stage = TEXT("verification");
						Error.TargetPath = ObservedTarget;
						return Fail(OutError, MoveTemp(Error));
					}
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryObject({{TEXT("target_ref"), TargetRef.ToSharedRef()}},
						Context.GetResourceTracker(), PublicResult, StateError)) return Fail(OutError, StateError);
				}
				if (!Mismatch.IsEmpty() && Mismatch.TargetPath.IsEmpty()) Mismatch.TargetPath = ObservedTarget;
				OutOutcome = FVerificationOutcome(TEXT("timeline-animation-verify:") + Contract.GetCapabilityId(),
					bMatches ? EVerificationStatus::Verified : EVerificationStatus::Mismatch,
					bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
					MoveTemp(Mismatch), false, {}, {TEXT("independent-state:") + After.GetSnapshotId()}, {}, MoveTemp(PublicResult));
				return true;
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};
	}

	namespace
	{
		bool TimelineBindingFailure(Contract::FDiagnostic& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, const FString& Identity, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_binding"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.TargetPath = Identity;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}
	}

	bool FUeAgentTimelineAnimationSubjectBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [&](const TCHAR* Reason, const TCHAR* Path, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			return TimelineBindingFailure(OutError, TEXT("timeline_subject_binding_invalid"), Reason, Path, Identity, MoveTemp(Actual), Expected);
		};
		if (Identity.IsEmpty()) return Reject(TEXT("The animation subject binding requires a target reference."),
			TEXT("identity"), MakeShared<FJsonValueString>(Identity), TEXT("non-empty subject reference"));
		if (!PoseComponent.IsValid() && !MorphComponent.IsValid()) return Reject(TEXT("The animation subject has no available poseable or skeletal mesh component."),
			TEXT("component"), nullptr, TEXT("poseable or skeletal mesh component"));
		if (NativeFrameId.IsEmpty()) return Reject(TEXT("The animation subject binding requires a native frame identifier."),
			TEXT("native_frame_id"), MakeShared<FJsonValueString>(NativeFrameId), TEXT("non-empty frame identifier"));
		if (EvaluationClockId.IsEmpty()) return Reject(TEXT("The animation subject binding requires an evaluation clock identifier."),
			TEXT("evaluation_clock_id"), MakeShared<FJsonValueString>(EvaluationClockId), TEXT("non-empty clock identifier"));
		const FUeAgentTemporalRate Rate(EvaluationRateNumerator, EvaluationRateDenominator);
		if (!Rate.IsValid())
		{
			OutError = Rate.GetValidationDiagnostic();
			OutError.PropertyPath = TEXT("evaluation_rate.") + OutError.PropertyPath;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity;
			return false;
		}
		return true;
	}

	bool FUeAgentTimelineAnimationSubjectBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentTimelineAnimationRigCandidateBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [&](const TCHAR* Reason, const TCHAR* Path, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			return TimelineBindingFailure(OutError, TEXT("timeline_rig_candidate_binding_invalid"), Reason, Path, Identity, MoveTemp(Actual), Expected);
		};
		if (Identity.IsEmpty()) return Reject(TEXT("The rig candidate binding requires a target sequence reference."),
			TEXT("identity"), MakeShared<FJsonValueString>(Identity), TEXT("non-empty sequence reference"));
		if (!Sequence.IsValid()) return Reject(TEXT("The rig candidate sequence is unavailable."), TEXT("sequence"), nullptr, TEXT("valid level sequence"));
		if (!Rig.IsValid()) return Reject(TEXT("The rig candidate Control Rig is unavailable."), TEXT("rig"), nullptr, TEXT("valid Control Rig"));
		if (RigIdentity.IsEmpty()) return Reject(TEXT("The rig candidate binding requires a Control Rig reference."),
			TEXT("rig_identity"), MakeShared<FJsonValueString>(RigIdentity), TEXT("non-empty Control Rig reference"));
		if (ControlName.IsNone()) return Reject(TEXT("The rig candidate control name must not be None."),
			TEXT("control_name"), MakeShared<FJsonValueString>(ControlName.ToString()), TEXT("control name other than None"));
		if (NativeFrameId.IsEmpty()) return Reject(TEXT("The rig candidate binding requires a native frame identifier."),
			TEXT("native_frame_id"), MakeShared<FJsonValueString>(NativeFrameId), TEXT("non-empty frame identifier"));
		if (AuthoredClockId.IsEmpty()) return Reject(TEXT("The rig candidate binding requires an authored clock identifier."),
			TEXT("authored_clock_id"), MakeShared<FJsonValueString>(AuthoredClockId), TEXT("non-empty clock identifier"));
		const FUeAgentTemporalRate Rate(AuthoredRateNumerator, AuthoredRateDenominator);
		if (!Rate.IsValid())
		{
			OutError = Rate.GetValidationDiagnostic();
			OutError.PropertyPath = TEXT("authored_rate.") + OutError.PropertyPath;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity;
			return false;
		}
		return true;
	}

	bool FUeAgentTimelineAnimationRigCandidateBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	namespace
	{
		bool ValidateNativeDependencies(const FUeAgentTimelineAnimationNativeDependencies& Dependencies,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (!Dependencies.Environment.IsValid())
				return TimelineBindingFailure(OutError, TEXT("timeline_animation_native_dependencies_invalid"),
					TEXT("The animation native environment is unavailable."), TEXT("environment"), FString(), nullptr, TEXT("valid native environment"));
			if (!Dependencies.RevisionSource.IsValid())
				return TimelineBindingFailure(OutError, TEXT("timeline_animation_native_dependencies_invalid"),
					TEXT("The animation revision source is unavailable."), TEXT("revision_source"), FString(), nullptr, TEXT("valid revision source"));
			return true;
		}

		bool ValidateNativePorts(const FUeAgentTimelineAnimationNativePorts& Ports,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			const TCHAR* Missing = !Ports.Readback.IsValid() ? TEXT("readback")
				: !Ports.Mutation.IsValid() ? TEXT("mutation")
				: !Ports.Settle.IsValid() ? TEXT("settle")
				: !Ports.Comparator.IsValid() ? TEXT("comparator") : nullptr;
			if (Missing)
				return TimelineBindingFailure(OutError, TEXT("timeline_animation_native_port_missing"),
					TEXT("A required animation native port is unavailable."), Missing, FString(), nullptr, TEXT("valid native port"));
			return true;
		}
	}

	bool FUeAgentTimelineAnimationNativeDependencies::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return ValidateNativeDependencies(*this, Error);
	}

	bool FUeAgentTimelineAnimationNativePorts::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return ValidateNativePorts(*this, Error);
	}

	bool TryCreateTimelineAnimationProductionDependencies(
		const FString& RuntimeGeneration, const FString& ServerSessionId,
		const TArray<FUeAgentTimelineAnimationSubjectBinding>& SubjectBindings,
		const TArray<FUeAgentTimelineAnimationRigCandidateBinding>& RigBindings,
		FUeAgentTimelineAnimationNativeDependencies& OutDependencies,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutDependencies = {}; OutError = {};
		if (RuntimeGeneration.IsEmpty() || ServerSessionId.IsEmpty())
		{
			TimelineBindingFailure(OutError, TEXT("timeline_animation_native_dependencies_invalid"),
				RuntimeGeneration.IsEmpty() ? TEXT("The animation runtime generation is empty.") : TEXT("The animation server session identifier is empty."),
				RuntimeGeneration.IsEmpty() ? TEXT("runtime_generation") : TEXT("server_session_id"), FString(), nullptr, TEXT("non-empty string"));
			OutError.ActualValuePresent = true;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(RuntimeGeneration.IsEmpty() ? RuntimeGeneration : ServerSessionId);
			return false;
		}
		const auto ValidateBindings = [&](const auto& Bindings, const TCHAR* Collection)
		{
			for (int32 Index = 0; Index < Bindings.Num(); ++Index)
			{
				if (!Bindings[Index].TryValidate(OutError))
				{
					const FString Path = FString::Printf(TEXT("%s[%d]"), Collection, Index);
					OutError.PropertyPath = OutError.PropertyPath.IsEmpty() ? Path : Path + TEXT(".") + OutError.PropertyPath;
					return false;
				}
			}
			return true;
		};
		if (!ValidateBindings(SubjectBindings, TEXT("subject_bindings"))
			|| !ValidateBindings(RigBindings, TEXT("rig_candidate_bindings"))) return false;
		TSet<FString> Ids;
		const auto RegisterIdentities = [&](const auto& Bindings, const TCHAR* Collection)
		{
			for (int32 Index = 0; Index < Bindings.Num(); ++Index)
			{
				const auto& Binding = Bindings[Index];
				if (Ids.Contains(Binding.Identity))
					return TimelineBindingFailure(OutError, TEXT("timeline_animation_explicit_binding_duplicate"),
						TEXT("An explicit subject or rig candidate reference is registered more than once."),
						FString::Printf(TEXT("%s[%d].identity"), Collection, Index), Binding.Identity,
						MakeShared<FJsonValueString>(Binding.Identity), TEXT("unique reference across subject and rig candidate bindings"));
				Ids.Add(Binding.Identity);
			}
			return true;
		};
		if (!RegisterIdentities(SubjectBindings, TEXT("subject_bindings"))
			|| !RegisterIdentities(RigBindings, TEXT("rig_candidate_bindings"))) return false;
		const TSharedRef<FProductionEnvironment> Environment =
			MakeShared<FProductionEnvironment>(RuntimeGeneration, ServerSessionId,
				SubjectBindings, RigBindings);
		OutDependencies.Environment = Environment;
		OutDependencies.RevisionSource = Environment;
		return ValidateNativeDependencies(OutDependencies, OutError);
	}

	bool TryCreateTimelineAnimationNativePorts(
		const FUeAgentTimelineAnimationNativeDependencies& Dependencies,
		FUeAgentTimelineAnimationNativePorts& OutPorts,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutPorts = {}; OutError = {};
		if (!ValidateNativeDependencies(Dependencies, OutError)) return false;
		const TSharedPtr<FProductionEnvironment> Environment =
			StaticCastSharedPtr<FProductionEnvironment>(Dependencies.Environment);
		if (!Environment.IsValid())
			return TimelineBindingFailure(OutError, TEXT("timeline_animation_native_environment_type_invalid"),
				TEXT("The animation production environment is unavailable."), TEXT("environment"), FString(), nullptr, TEXT("valid production environment"));
		OutPorts.Readback = MakeShared<FReadback>(Environment.ToSharedRef());
		OutPorts.Mutation = MakeShared<FMutation>(Environment.ToSharedRef());
		OutPorts.Settle = MakeShared<FSettle>(Environment.ToSharedRef());
		OutPorts.Comparator = MakeShared<FComparator>(Environment.ToSharedRef());
		return ValidateNativePorts(OutPorts, OutError);
	}

	bool TryApplyLevelSequenceStructure(
		ULevelSequence& Sequence,
		const FTimelineAnimationStructure& Structure,
		const FString& AuthoredObjectPath,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		FString ObjectPath = AuthoredObjectPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
			ObjectPath += TEXT(".") + AssetName;
		}
		return ApplyLevelSequenceStructure(
			Sequence,
			Structure,
			TEXT("level_sequence:") + ObjectPath,
			OutError);
	}

	bool TryUpdateLevelSequenceValues(
		ULevelSequence& Sequence, const FTimelineAnimationStructure& Structure,
		const FString& AuthoredObjectPath, FUeAgentTimelineAnimationExecutionError& OutError,
		Authoring::Timeline::FMovieSceneCreationState* CreationState)
	{
		TSharedPtr<FJsonObject> CurrentDocument;
		FTimelineAnimationStructure Current;
		if (!TryReadLevelSequenceStructure(Sequence, AuthoredObjectPath, CurrentDocument, OutError)
			|| !TryParseTimelineAnimationStructure(CurrentDocument, Current, OutError)) return false;
		return Authoring::Timeline::TryUpdateMovieSceneValues(*Sequence.GetMovieScene(), Current, Structure, OutError,
			&Sequence, [&Sequence, CreationState](const FTimelineAnimationBinding& Binding, Authoring::Timeline::FMovieSceneObjectBinding& Out, UE::UAI::Contract::FDiagnostic& Error) {
				return ResolveLevelSequenceAuthoringBinding(Sequence, Binding, Out, Error, CreationState);
			}, CreationState,
			[&Sequence](const FTimelineAnimationBinding& Binding, Authoring::Timeline::FMovieSceneBindingEdit& Edit, UE::UAI::Contract::FDiagnostic& Error) {
				return PrepareLevelSequenceBindingEdit(Sequence, Binding, Edit, Error);
			});
	}

	bool TryQueryLevelSequenceTrackCandidates(const FString& Target, const FString& Query,
		const FString& ObjectRef, const FString& PropertyPath, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError)
	{
		ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *Target);
		if (!Sequence) return Fail(OutError, TimelineNativeFact(TEXT("timeline_track_candidate_target_unresolved"), TEXT("The candidate query target could not be loaded as a LevelSequence."), TEXT("/target_ref"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(ULevelSequence::StaticClass()->GetPathName()), Target));
		if (!Sequence->GetMovieScene()) return Fail(OutError, TimelineNativePresence(TEXT("timeline_track_candidate_movie_scene_unavailable"), TEXT("The candidate query target has no MovieScene."), TEXT("/movie_scene"), false, true, Target));
		Authoring::Timeline::FMovieSceneObjectBinding Bound;
		if (!PropertyPath.IsEmpty())
		{
			FTimelineAnimationBinding Binding;
			Binding.BindingKind = TEXT("possessable"); Binding.ObjectRef = ObjectRef;
			if (!ResolveLevelSequenceObjectBinding(Binding, Bound, OutError)) return false;
		}
		return Authoring::Timeline::TryQueryMovieSceneTrackCandidates(*Sequence->GetMovieScene(), Query,
			OutPayload, OutError, Bound.Object, PropertyPath);
	}

	bool TryReadLevelSequenceStructure(const ULevelSequence& Sequence,
		const FString& AuthoredObjectPath, TSharedPtr<FJsonObject>& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		const UMovieScene* Scene = Sequence.GetMovieScene();
		if (!Scene) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_movie_scene_unavailable"), TEXT("The level sequence has no MovieScene to read."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));
		FString ObjectPath = AuthoredObjectPath;
		if (!ObjectPath.Contains(TEXT("."))) ObjectPath += TEXT(".") + FPackageName::GetLongPackageAssetName(ObjectPath);
		TMap<FGuid, Authoring::Timeline::FMovieSceneBindingIdentity> Identities;
		for (const FMovieSceneBinding& Binding : Scene->GetBindings())
		{
			FString ObjectRef;
			if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			{
				TArray<UObject*, TInlineAllocator<1>> Objects;
				Sequence.LocateBoundObjects(Binding.GetObjectGuid(), UE::UniversalObjectLocator::FResolveParams(World), nullptr, Objects);
				if (Objects.Num() == 1 && IsValid(Objects[0])) ObjectRef = Objects[0]->GetPathName();
			}
			Authoring::Timeline::FMovieSceneBindingIdentity Identity{TEXT("possessable"), MoveTemp(ObjectRef)};
			UE::UAI::Contract::FDiagnostic PropertyError;
			UMovieScene& Owner = *const_cast<UMovieScene*>(Scene);
			const auto Policy = BindingRecordPolicy();
			if (const auto* Spawnable = Owner.FindSpawnable(Binding.GetObjectGuid()))
			{
				Identity.Kind = TEXT("spawnable");
				Identity.ObjectRef.Reset();
				if (!Composition::HjsonImport::CaptureReflectedStructRecord(*FMovieSceneSpawnable::StaticStruct(), Spawnable,
					Owner, SpawnablePropertyPaths(), Policy, Identity.Properties, Identity.PropertyDefinitions, PropertyError))
					return Fail(OutError, PropertyError);
			}
			else
			{
				const auto References = CaptureBindingReferences(Sequence, Binding.GetObjectGuid());
				for (const auto& Reference : References.GetAllReferences())
					if (Cast<UMovieSceneSpawnableBindingBase>(Reference.CustomBinding))
					{ Identity.Kind = TEXT("spawnable"); Identity.ObjectRef.Reset(); break; }
				if (!Composition::HjsonImport::CaptureReflectedStructRecord(*FMovieSceneBindingReferences::StaticStruct(), &References,
					Owner, {TEXT("SortedReferences")}, Policy, Identity.Properties, Identity.PropertyDefinitions, PropertyError))
					return Fail(OutError, PropertyError);
			}
			Identity.TemplateProperties = MakeShared<FJsonObject>();
			Identity.TemplateDefinitions = MakeShared<FJsonObject>();
			for (const auto& Template : BindingTemplates(*const_cast<ULevelSequence*>(&Sequence), Binding.GetObjectGuid()))
			{
				TSharedPtr<FJsonObject> Properties, Definitions;
				if (!Composition::HjsonImport::CaptureReflectedObjectRecord(*Template.Value, {}, Properties, Definitions, PropertyError))
					return Fail(OutError, PropertyError);
				Identity.TemplateProperties->SetObjectField(Template.Key, Properties);
				Identity.TemplateDefinitions->SetObjectField(Template.Key, Definitions);
			}
			Identities.Add(Binding.GetObjectGuid(), MoveTemp(Identity));
		}
		return Authoring::Timeline::TryReadMovieSceneStructure(*Scene, TEXT("level_sequence:") + ObjectPath, Identities, OutStructure, OutError, true);
	}

	TSharedRef<FJsonObject> ReadLevelSequenceBindingResolverSummary(const ULevelSequence& Sequence)
	{
		const auto Result = MakeShared<FJsonObject>();
		int32 ReferenceCount = 0;
		int32 CustomCount = 0;
		TArray<FString> Types;
		if (const FMovieSceneBindingReferences* References = Sequence.GetBindingReferences())
		{
			for (const FMovieSceneBindingReference& Reference : References->GetAllReferences())
			{
				++ReferenceCount;
				if (!Reference.CustomBinding) continue;
				++CustomCount;
				FString Name = Reference.CustomBinding->GetBindingTypePrettyName().ToString();
				if (Name.IsEmpty()) Name = Reference.CustomBinding->GetClass()->GetName();
				Types.AddUnique(MoveTemp(Name));
			}
		}
		Types.Sort();
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FString& Type : Types) Names.Add(MakeShared<FJsonValueString>(Type));
		Result->SetNumberField(TEXT("reference_count"), ReferenceCount);
		Result->SetNumberField(TEXT("custom_reference_count"), CustomCount);
		Result->SetArrayField(TEXT("custom_types"), MoveTemp(Names));
		return Result;
	}

	bool TryReadLevelSequenceBindingMetadataState(const ULevelSequence& Sequence,
		TSharedPtr<FJsonObject>& OutState, FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutState.Reset(); OutError = {};
		const UMovieScene* Scene = Sequence.GetMovieScene();
		if (!Scene) return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_movie_scene_unavailable"), TEXT("The level sequence has no MovieScene to read."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));
		OutState = MakeShared<FJsonObject>();
		TArray<FName> TagNames; Scene->AllTaggedBindings().GetKeys(TagNames);
		TagNames.Sort(FNameLexicalLess());
		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& Tag : TagNames)
		{
			TArray<uint8> Bytes; FMemoryWriter MemoryWriter(Bytes, true);
			FSemanticPersistentObjectWriter Writer(MemoryWriter, *Scene, Sequence);
			FMovieSceneObjectBindingIDs::StaticStruct()->SerializeItem(Writer,
				const_cast<FMovieSceneObjectBindingIDs*>(&Scene->AllTaggedBindings().FindChecked(Tag)), nullptr);
			Writer.Close(); MemoryWriter.Close();
			auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Tag.ToString());
			Item->SetStringField(TEXT("persistent_bytes"), BytesToHex(Bytes.GetData(), Bytes.Num()));
			Tags.Add(MakeShared<FJsonValueObject>(Item));
		}
		OutState->SetArrayField(TEXT("tags"), MoveTemp(Tags));
		TArray<TSharedPtr<FJsonValue>> Roots;
		// UE 5.6 exposes only a non-const accessor; this capture does not mutate its view.
		const auto RootFolders = const_cast<UMovieScene*>(Scene)->GetRootFolders();
		for (const UMovieSceneFolder* Folder : RootFolders)
			Roots.Add(MakeShared<FJsonValueString>(Folder ? Folder->GetPathName(Scene) : FString()));
		OutState->SetArrayField(TEXT("root_folders"), MoveTemp(Roots));
		TArray<UMovieSceneFolder*> Folders; GetMovieSceneFoldersRecursive(RootFolders, Folders);
		Folders.Sort([Scene](const UMovieSceneFolder& A, const UMovieSceneFolder& B) { return A.GetPathName(Scene) < B.GetPathName(Scene); });
		TArray<TSharedPtr<FJsonValue>> FolderStates;
		for (UMovieSceneFolder* Folder : Folders)
		{
			TArray<uint8> Bytes; FMemoryWriter MemoryWriter(Bytes, true);
			FSemanticPersistentObjectWriter Writer(MemoryWriter, *Scene, Sequence);
			Folder->Serialize(Writer); Writer.Close(); MemoryWriter.Close();
			auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("object_ref"), Folder->GetPathName(Scene));
			Item->SetStringField(TEXT("class_path"), Folder->GetClass()->GetPathName());
			Item->SetStringField(TEXT("persistent_bytes"), BytesToHex(Bytes.GetData(), Bytes.Num()));
			FolderStates.Add(MakeShared<FJsonValueObject>(Item));
		}
		OutState->SetArrayField(TEXT("folders"), MoveTemp(FolderStates));
		return true;
	}

	bool TryReadLevelSequencePersistentState(
		const ULevelSequence& Sequence,
		TSharedPtr<FJsonObject>& OutState,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutState.Reset();
		OutError = {};
		const UMovieScene* Scene = Sequence.GetMovieScene();
		if (!Scene)
		{
			return Fail(OutError, TimelineNativePresence(TEXT("timeline_animation_level_sequence_movie_scene_unavailable"), TEXT("The level sequence has no MovieScene to read."), TEXT("/movie_scene"), false, true, Sequence.GetPathName()));
		}
		TArray<UObject*> Objects;
		Objects.Add(const_cast<UMovieScene*>(Scene));
		GetObjectsWithOuter(Scene, Objects, true, RF_Transient);
		Objects.Sort([Scene](const UObject& Left, const UObject& Right)
		{
			return Left.GetPathName(Scene) < Right.GetPathName(Scene);
		});

		TArray<TSharedPtr<FJsonValue>> SerializedObjects;
		SerializedObjects.Reserve(Objects.Num());
		for (UObject* Object : Objects)
		{
			if (!Object) continue;
			TArray<uint8> Bytes;
			FMemoryWriter MemoryWriter(Bytes, true);
			FSemanticPersistentObjectWriter Writer(
				MemoryWriter, *Scene, Sequence);
			Object->Serialize(Writer);
			Writer.Close();
			MemoryWriter.Close();

			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("object_ref"),
				Object == Scene ? TEXT("$root")
					: TEXT("$root/") + Object->GetPathName(Scene));
			Item->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
			Item->SetStringField(TEXT("persistent_bytes"),
				BytesToHex(Bytes.GetData(), Bytes.Num()));
			SerializedObjects.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutState = MakeShared<FJsonObject>();
		OutState->SetStringField(TEXT("state_kind"),
			TEXT("level_sequence_persistent_owned_objects_v1"));
		OutState->SetArrayField(TEXT("objects"), MoveTemp(SerializedObjects));
		return true;
	}
}
