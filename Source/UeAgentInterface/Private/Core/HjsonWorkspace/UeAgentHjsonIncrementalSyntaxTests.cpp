#include "Misc/AutomationTest.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentHjsonPointerText.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "Async/ParallelFor.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
using namespace UE::UAI::HjsonWorkspace;
using namespace UE::UAI::ContextResource;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceIncrementalSyntaxTest,
	"UeAgentInterface.Core.HjsonWorkspace.IncrementalSyntax",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonWorkspaceIncrementalSyntaxTest::RunTest(const FString&)
{
	{
		FString Embedded(TEXT("a")); Embedded.AppendChar(TCHAR(0)); Embedded += TEXT("tail");
		for (const FString& Key : TArray<FString>{TEXT("unit_id"), TEXT("A"), TEXT("a"), TEXT("中文"), Embedded}) {
			const FString Storage = Key + TEXT("unrelated_suffix");
			const FStringView Borrowed(*Storage, Key.Len());
			TestTrue(TEXT("borrowed exact key keeps original equality"), UE::UAI::StateSemantic::FExactStringKeyFuncs::MatchesView(Borrowed, FStringView(Key)));
			TestEqual(TEXT("borrowed key hashes only its actual spelling"), UE::UAI::StateSemantic::FExactStringKeyFuncs::GetViewKeyHash(Borrowed),
				UE::UAI::StateSemantic::FExactStringKeyFuncs::GetKeyHash(Key));
		}
		TestFalse(TEXT("borrowed keys remain case sensitive"), UE::UAI::StateSemantic::FExactStringKeyFuncs::MatchesView(TEXT("A"), TEXT("a")));
	}
	{
		using namespace UE::UAI::StateSemantic;
		FUeAgentResourceTracker Resources(false); FUeAgentStateSemanticError Error;
		FUeAgentCanonicalValuePtr A, B, Object;
		TestTrue(TEXT("numeric factory accepts original exact spelling"), FUeAgentCanonicalValue::TryDecimal(TEXT("1.25e+2"), Resources, A, Error));
		TestTrue(TEXT("same decimal remains valid with optional weak admission"), FUeAgentCanonicalValue::TryDecimal(TEXT("1.25e+2"), Resources, B, Error));
		TestTrue(TEXT("same decimal retains its exact semantic value"), CompareValues(A.ToSharedRef(), B.ToSharedRef(), TEXT("/"), Resources).IsEquivalent());
		AddInfo(FString::Printf(TEXT("Optional immutable numeric leaf reused=%s; admission is not a result requirement."), A == B ? TEXT("true") : TEXT("false")));
		TestFalse(TEXT("another unproven numeric spelling still receives original grammar error"), FUeAgentCanonicalValue::TryDecimal(TEXT("NaN"), Resources, B, Error));
		TestEqual(TEXT("original numeric diagnostic is preserved"), Error.Code, FString(TEXT("decimal_lexeme_not_finite")));
		TestTrue(TEXT("single object member keeps exact original key"), FUeAgentCanonicalValue::TryObject({{TEXT("a"), A.ToSharedRef()}}, Resources, Object, Error));
		TestFalse(TEXT("multi-member duplicate still fails"), FUeAgentCanonicalValue::TryObject({{TEXT("a"), A.ToSharedRef()}, {TEXT("a"), A.ToSharedRef()}}, Resources, Object, Error));
		TestEqual(TEXT("original duplicate diagnostic is preserved"), Error.Code, FString(TEXT("object_key_duplicate")));
		TestFalse(TEXT("single empty key still fails"), FUeAgentCanonicalValue::TryObject({{TEXT(""), A.ToSharedRef()}}, Resources, Object, Error));
		TestEqual(TEXT("original empty-key diagnostic is preserved"), Error.Code, FString(TEXT("object_key_empty")));
	}
	{
		using namespace UE::UAI::StateSemantic;
		FUeAgentResourceTracker Resources(false);
		UE::UAI::Contract::FDiagnostic Error;
		auto Fields = MakeShared<FJsonObject>();
		Fields->SetNumberField(TEXT("visible"), 0);
		Fields->SetBoolField(TEXT("readonly"), true);
		auto Value = MakeShared<FJsonObject>(); Value->SetObjectField(TEXT("fields"), Fields);
		auto Readonly = MakeShared<FJsonObject>(); Readonly->SetBoolField(TEXT("editable"), false);
		auto Descriptors = MakeShared<FJsonObject>();
		Descriptors->SetObjectField(TEXT("visible"), MakeShared<FJsonObject>());
		Descriptors->SetObjectField(TEXT("readonly"), Readonly);
		auto Schema = MakeShared<FJsonObject>(); Schema->SetObjectField(TEXT("fields"), Descriptors);
		auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("kind"), TEXT("struct"));
		FUeAgentCanonicalValuePtr Native, Kept;
		TSharedPtr<FJsonValue> Codec, Projected, ProjectedDefinition;
		if (!TestTrue(TEXT("observed struct enters the public codec"),
			CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Value), Resources, Native, Error)
			&& CanonicalJson::TryToAuthoringCodecJson(*Native, Codec, Error)
			&& UeAgentPropertyValueCodec::ProjectAuthoringValue(Type, Codec, MakeShared<FJsonValueObject>(Schema),
				Projected, ProjectedDefinition))) return false;
		const auto Visible = Projected->AsObject()->GetObjectField(TEXT("fields"));
		TestTrue(TEXT("readonly filtering rebuilds the object"), Projected != Codec && !Visible->HasField(TEXT("readonly")));
		TestTrue(TEXT("the rebuilt public value keeps the native integral decimal kind"),
			CanonicalJson::TryFromJsonValue(Visible->TryGetField(TEXT("visible")), Resources, Kept, Error)
			&& Kept->GetKind() == EUeAgentValueKind::Decimal);
		TSharedPtr<FJsonValue> Declaration;
		if (!CanonicalJson::TryToAuthoringDeclarationJson(*Native, Declaration, Error)) return false;
		const auto DefaultLiteral = Declaration->AsObject()->GetObjectField(TEXT("fields"))->TryGetField(TEXT("visible"));
		FString Spelling;
		TestTrue(TEXT("the same native decimal retains the original nested declaration spelling"),
			!DefaultLiteral->PreferStringRepresentation() && CanonicalJson::TryGetNumberLexeme(*DefaultLiteral, Spelling)
			&& Spelling == TEXT("0") && CanonicalJson::TryFromJsonValue(DefaultLiteral, Resources, Kept, Error)
			&& Kept->GetKind() == EUeAgentValueKind::Decimal);
		FUeAgentStateSemanticError FactoryError;
		for (const TCHAR* Exact : { TEXT("0.0"), TEXT("9007199254740993") }) {
			if (!FUeAgentCanonicalValue::TryDecimal(Exact, Resources, Native, FactoryError)
				|| !CanonicalJson::TryToAuthoringDeclarationJson(*Native, Declaration, Error)) return false;
			TestTrue(TEXT("explicit decimal or nonrepresentable declaration retains the exact number string"),
				Declaration->PreferStringRepresentation() && CanonicalJson::TryGetNumberLexeme(*Declaration, Spelling) && Spelling == Exact);
		}
	}
	{
		// The actual codec reads the same isolated native storage in both modes.
		// No asset, World, setter, or alternate literal implementation is involved.
		const auto CheckLiteral = [&](FProperty* Property, const void* Storage, UObject* Owner,
			UScriptStruct* Struct = nullptr, const FString& Kind = FString()) {
			UE::UAI::Contract::FDiagnostic CodecError;
			UeAgentPropertyValueCodec::FOptions DirectOptions;
			UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Direct;
			DirectOptions.DirectAuthoringLiteral = &Direct;
			DirectOptions.DirectAuthoringLiteralError = &CodecError;
			UeAgentPropertyValueCodec::EAuthoringValueDestination Destination = UeAgentPropertyValueCodec::EAuthoringValueDestination::None;
			DirectOptions.DirectAuthoringDestination = &Destination;
			TSharedPtr<FJsonObject> LegacyDefinition, DirectDefinition;
			const auto Legacy = Struct ? UeAgentPropertyValueCodec::EncodeStructValue(Struct, Storage, Owner, {}, 0, &LegacyDefinition)
				: UeAgentPropertyValueCodec::Encode(Property, Storage, Owner, {}, 0, &LegacyDefinition);
			const auto AbsentJson = Struct ? UeAgentPropertyValueCodec::EncodeStructValue(Struct, Storage, Owner, DirectOptions, 0, &DirectDefinition)
				: UeAgentPropertyValueCodec::Encode(Property, Storage, Owner, DirectOptions, 0, &DirectDefinition);
			if (!TestTrue(TEXT("explicit literal has no JSON producer"), !AbsentJson && Direct.IsValid() && CodecError.IsEmpty())) return false;
			TestTrue(TEXT("original producer reports the selected immutable destination"), Destination == UeAgentPropertyValueCodec::EAuthoringValueDestination::Scalar
				|| Destination == UeAgentPropertyValueCodec::EAuthoringValueDestination::BuiltinShape || Destination == UeAgentPropertyValueCodec::EAuthoringValueDestination::Reference);
			TestFalse(TEXT("closed literal produces no dynamic definition"), DirectDefinition.IsValid());
			FUeAgentResourceTracker OriginalResources(false);
			UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Original;
			UE::UAI::Contract::FDiagnostic OriginalError;
			if (!TestTrue(TEXT("original JSON converter remains the independent oracle"),
				UE::UAI::StateSemantic::CanonicalJson::TryFromJsonValue(Legacy, OriginalResources, Original, OriginalError))) return false;
			TestTrue(TEXT("default JSON remains the same exact canonical value"),
				UE::UAI::StateSemantic::CompareValues(Original.ToSharedRef(), Direct.ToSharedRef(), TEXT("/"), OriginalResources).IsEquivalent());
			const FString DeclaredKind = Struct ? Kind : UeAgentPropertyValueCodec::GetValueKind(Property);
			const auto Projected = UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*Direct);
			TestTrue(TEXT("explicit JSON consumer keeps original value grammar"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(Legacy, Projected));
			UeAgentAuthoringState::FStateEntry LegacyEntry, DirectEntry;
			LegacyEntry.Value = Legacy; DirectEntry.DirectAuthoringValue = Direct;
			LegacyEntry.Type.Kind = DirectEntry.Type.Kind = DeclaredKind;
			FString LegacyMaterial, DirectMaterial;
			UeAgentAuthoringState::VisitStableCanonicalValueMaterialView(LegacyEntry,
				[&](FStringView Text) { LegacyMaterial.AppendChars(Text.GetData(), Text.Len()); });
			UeAgentAuthoringState::VisitStableCanonicalValueMaterialView(DirectEntry,
				[&](FStringView Text) { DirectMaterial.AppendChars(Text.GetData(), Text.Len()); });
			TestEqual(TEXT("binding consumes pure literal with identical stable tokens"), DirectMaterial, LegacyMaterial);
			TestTrue(TEXT("explicit snapshot JSON consumer preserves value"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(
				LegacyEntry.ToJsonObject()->TryGetField(TEXT("value")), DirectEntry.ToJsonObject()->TryGetField(TEXT("value"))));
			return true;
		};
		auto* Component = GetMutableDefault<USceneComponent>();
		auto* Visible = FindFProperty<FBoolProperty>(USceneComponent::StaticClass(), TEXT("bVisible"));
		if (!TestNotNull(TEXT("original reflected bool descriptor"), Visible)
			|| !CheckLiteral(Visible, Visible->ContainerPtrToValuePtr<void>(Component), Component)) return false;
		auto* Mobility = FindFProperty<FProperty>(USceneComponent::StaticClass(), TEXT("Mobility"));
		if (!TestNotNull(TEXT("original reflected non-bitmask enum"), Mobility)
			|| !CheckLiteral(Mobility, Mobility->ContainerPtrToValuePtr<void>(Component), Component)) return false;
		if (auto* ByteEnum = CastField<FByteProperty>(Mobility)) {
			const uint8 Unnamed = MAX_uint8;
			if (!CheckLiteral(ByteEnum, &Unnamed, nullptr)) return false;
		}
		auto* MathLibrary = FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetMathLibrary"));
		if (!TestNotNull(TEXT("registered native int64 parameter owner"), MathLibrary)) return false;
		auto* Convert = MathLibrary->FindFunctionByName(TEXT("Conv_Int64ToInt"));
		if (!TestNotNull(TEXT("native parameter declaration without invoking function"), Convert)) return false;
		auto* Integer = FindFProperty<FInt64Property>(Convert, TEXT("InInt"));
		if (!TestNotNull(TEXT("actual reflected int64 parameter"), Integer)) return false;
		for (int64 Value : {MAX_int64, MIN_int64}) if (!CheckLiteral(Integer, &Value, nullptr)) return false;
		auto* PrimitiveClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.PrimitiveComponent"));
		if (!TestNotNull(TEXT("registered native float/int owner"), PrimitiveClass)) return false;
		auto* Float = FindFProperty<FFloatProperty>(PrimitiveClass, TEXT("BoundsScale"));
		auto* SmallInteger = FindFProperty<FIntProperty>(PrimitiveClass, TEXT("TranslucencySortPriority"));
		if (!TestNotNull(TEXT("original reflected float32 field"), Float)
			|| !TestNotNull(TEXT("original reflected int32 field"), SmallInteger)) return false;
		for (float Value : {0.0f, -0.0f, 0.1f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()})
			if (!CheckLiteral(Float, &Value, nullptr)) return false;
		for (int32 Value : {MIN_int32, 0, MAX_int32}) if (!CheckLiteral(SmallInteger, &Value, nullptr)) return false;
		auto* Location = FindFProperty<FStructProperty>(USceneComponent::StaticClass(), TEXT("RelativeLocation"));
		if (!TestNotNull(TEXT("real vector field"), Location)) return false;
		auto* Double = FindFProperty<FDoubleProperty>(Location->Struct, TEXT("X"));
		if (!TestNotNull(TEXT("original reflected float64 field"), Double)) return false;
		for (double Value : {0.0, -0.0, 0.1, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()})
			if (!CheckLiteral(Double, &Value, nullptr)) return false;

		// Same original codec, actual native closed-shape storage. This tests
		// immutable destination/JSON/stable tokens without assets or setters.
		const FVector Vector(0.1, -0.0, std::numeric_limits<double>::denorm_min());
		const FVector3f VectorFloat(0.1f, -0.0f, std::numeric_limits<float>::denorm_min());
		const FVector2D Vector2(0.25, -2.75);
		const FVector4 Vector4(1.1, -2.2, 3.3, 4.4);
		const FQuat Quat = FRotator(13.5, -27.25, 41.75).Quaternion();
		const FRotator Rotator(13.5, -27.25, 41.75);
		const FColor Color(0, 17, 128, 255);
		const FLinearColor Linear(0.1f, -0.0f, 2.3f, 1.0f);
		const FTransform Transform(Rotator, Vector, FVector(0.5, 1.5, 2.5));
		const FTransform3f TransformFloat(Transform);
		if (!CheckLiteral(Location, &Vector, nullptr)
			|| !CheckLiteral(nullptr, &VectorFloat, nullptr, TVariantStructure<FVector3f>::Get(), TEXT("vector3"))
			|| !CheckLiteral(nullptr, &Vector2, nullptr, TBaseStructure<FVector2D>::Get(), TEXT("vector2"))
			|| !CheckLiteral(nullptr, &Vector4, nullptr, TBaseStructure<FVector4>::Get(), TEXT("vector4"))
			|| !CheckLiteral(nullptr, &Quat, nullptr, TBaseStructure<FQuat>::Get(), TEXT("quat"))
			|| !CheckLiteral(nullptr, &Rotator, nullptr, TBaseStructure<FRotator>::Get(), TEXT("rotator"))
			|| !CheckLiteral(nullptr, &Color, nullptr, TBaseStructure<FColor>::Get(), TEXT("color8"))
			|| !CheckLiteral(nullptr, &Linear, nullptr, TBaseStructure<FLinearColor>::Get(), TEXT("linear_color"))
			|| !CheckLiteral(nullptr, &Transform, nullptr, TBaseStructure<FTransform>::Get(), TEXT("transform"))
			|| !CheckLiteral(nullptr, &TransformFloat, nullptr, TVariantStructure<FTransform3f>::Get(), TEXT("transform"))) return false;

		const FVector Nonfinite(std::numeric_limits<double>::quiet_NaN(), 2, 3);
		UeAgentPropertyValueCodec::FOptions DiagnosticInput;
		UeAgentPropertyValueCodec::EAuthoringValueDestination DiagnosticDestination = UeAgentPropertyValueCodec::EAuthoringValueDestination::None;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr DiagnosticDirect;
		DiagnosticInput.DirectAuthoringLiteral = &DiagnosticDirect;
		DiagnosticInput.DirectAuthoringDestination = &DiagnosticDestination;
		const auto OriginalInvalid = UeAgentPropertyValueCodec::Encode(Location, &Nonfinite, nullptr, {});
		const auto ObservedInvalid = UeAgentPropertyValueCodec::Encode(Location, &Nonfinite, nullptr, DiagnosticInput);
		TestTrue(TEXT("actual nonfinite native read selects original numeric JSON before immutable production"),
			DiagnosticDestination == UeAgentPropertyValueCodec::EAuthoringValueDestination::NumericJson && !DiagnosticDirect);
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr OriginalResult, ObservedResult;
		UE::UAI::Contract::FDiagnostic OriginalInvalidError, ObservedInvalidError;
		FUeAgentResourceTracker InvalidResources(false);
		const bool bOriginalConverted = UE::UAI::StateSemantic::CanonicalJson::TryFromJsonValue(OriginalInvalid, InvalidResources, OriginalResult, OriginalInvalidError);
		const bool bObservedConverted = UE::UAI::StateSemantic::CanonicalJson::TryFromJsonValue(ObservedInvalid, InvalidResources, ObservedResult, ObservedInvalidError);
		TestEqual(TEXT("actual default native value/diagnostic outcome remains unchanged"), bObservedConverted, bOriginalConverted);
		if (bOriginalConverted && bObservedConverted)
			TestTrue(TEXT("observed diagnostic-purpose value matches actual default normalization"),
				UE::UAI::StateSemantic::CompareValues(OriginalResult.ToSharedRef(), ObservedResult.ToSharedRef(), TEXT("/"), InvalidResources).IsEquivalent());
		TestEqual(TEXT("nonfinite diagnostic code unchanged"), ObservedInvalidError.Code, OriginalInvalidError.Code);
		TestEqual(TEXT("nonfinite diagnostic pointer unchanged"), ObservedInvalidError.SemanticPointer, OriginalInvalidError.SemanticPointer);
		const auto FirstNumber = OriginalInvalid->AsObject()->GetArrayField(TEXT("components"))[0]; double NativeJsonNumber = 0;
		const bool bHasNumber = FirstNumber->TryGetNumber(NativeJsonNumber);
		AddInfo(FString::Printf(TEXT("Original native nonfinite input: json_kind=%d has_number=%s number=%.17g finite=%s canonical=%s; observed purpose kept the same result/diagnostic."),
			static_cast<int32>(FirstNumber->Type), bHasNumber ? TEXT("true") : TEXT("false"), NativeJsonNumber,
			FMath::IsFinite(NativeJsonNumber) ? TEXT("true") : TEXT("false"), bOriginalConverted ? TEXT("true") : TEXT("false")));
		auto* FilePathStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.FilePath"));
		if (!TestNotNull(TEXT("registered CoreUObject reflected string owner"), FilePathStruct)) return false;
		auto* StringProperty = FindFProperty<FStrProperty>(FilePathStruct, TEXT("FilePath"));
		const FString Text = TEXT("中文\\\"\n%;{}: 😀");
		if (!TestNotNull(TEXT("original reflected string descriptor"), StringProperty)
			|| !CheckLiteral(StringProperty, &Text, nullptr)) return false;
		auto* Tags = FindFProperty<FArrayProperty>(USceneComponent::StaticClass(), TEXT("ComponentTags"));
		if (!TestNotNull(TEXT("original reflected name descriptor"), Tags)) return false;
		const FName Name(TEXT("C99_中文_%:{}"));
		if (!CheckLiteral(Tags->Inner, &Name, nullptr)) return false;
		TestFalse(TEXT("containers select their original codec before encoding"), UeAgentPropertyValueCodec::SupportsDirectAuthoringLiteral(Tags));

		auto* Parent = FindFProperty<FObjectPropertyBase>(USceneComponent::StaticClass(), TEXT("AttachParent"));
		if (!TestNotNull(TEXT("actual hard-reference descriptor"), Parent)) return false;
		TObjectPtr<USceneComponent> ParentValue = nullptr;
		if (!CheckLiteral(Parent, &ParentValue, Component)) return false;
		ParentValue = Component;
		if (!CheckLiteral(Parent, &ParentValue, Component)) return false;
		const FSoftObjectPath SoftPath(TEXT("/Game/UAI/Tests/Transient/CodecReference.CodecReference"));
		const FSoftObjectPath NullPath;
		if (!CheckLiteral(nullptr, &SoftPath, nullptr, TBaseStructure<FSoftObjectPath>::Get(), TEXT("soft_object_ref"))
			|| !CheckLiteral(nullptr, &NullPath, nullptr, TBaseStructure<FSoftObjectPath>::Get(), TEXT("soft_object_ref"))) return false;
		for (bool bHandled : {false, true}) {
			int32 CallbackCalls = 0;
			UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr CallbackDirect;
			UeAgentPropertyValueCodec::EAuthoringValueDestination CallbackDestination = UeAgentPropertyValueCodec::EAuthoringValueDestination::None;
			UeAgentPropertyValueCodec::FOptions Options;
			Options.DirectAuthoringLiteral = &CallbackDirect; Options.DirectAuthoringDestination = &CallbackDestination;
			Options.ObjectReferenceEncoder = [&](const FObjectPropertyBase*, const void*, UObject*, int32,
				TSharedPtr<FJsonValue>& Out, TSharedPtr<FJsonObject>* Definition) {
				++CallbackCalls; if (!bHandled) return false;
				Out = UeAgentPropertyValueCodec::EncodeReferencePath(TEXT("callback-owned-path"));
				if (Definition) { *Definition = MakeShared<FJsonObject>(); (*Definition)->SetStringField(TEXT("builtin"), TEXT("owned_object")); }
				return true;
			};
			TSharedPtr<FJsonObject> Definition;
			const auto Json = UeAgentPropertyValueCodec::Encode(Parent, &ParentValue, Component, Options, 0, &Definition);
			TestEqual(TEXT("original reference callback runs exactly once"), CallbackCalls, 1);
			if (bHandled) {
				TestTrue(TEXT("handled callback keeps its JSON and actual definition"), Json && Definition && !CallbackDirect
					&& CallbackDestination == UeAgentPropertyValueCodec::EAuthoringValueDestination::CallbackJson);
				TestEqual(TEXT("callback body is not replaced by native path"), Json->AsObject()->GetStringField(TEXT("path")), FString(TEXT("callback-owned-path")));
			} else TestTrue(TEXT("declined callback directly supplies original native path"), !Json && !Definition && CallbackDirect
				&& CallbackDestination == UeAgentPropertyValueCodec::EAuthoringValueDestination::Reference);
		}
	}
	{
		// No native object/asset fixture. Exercise the actual capture reader's
		// failure priority and original array entry using unknown semantics.
		using namespace UeAgentAuthoringState;
		TArray<FReadRequest> Requests;
		Requests.AddDefaulted(2);
		Requests[0].SemanticId = TEXT("uai.c92.unknown-first");
		Requests[1].SemanticId = TEXT("uai.c92.unknown-second");
		FCaptureResult ArrayResult, DirectResult;
		TestFalse(TEXT("original array rejects unknown semantic"), CaptureSnapshot(Requests, ArrayResult));
		int32 Produced = 0;
		const auto Produce = [&](FReadRequestConsumer Consume, UE::UAI::Contract::FDiagnostic&) {
			for (const auto& Request : Requests) { ++Produced; TestFalse(TEXT("failed reader stays stopped"), Consume(Request)); }
			return true;
		};
		TestFalse(TEXT("same direct reader rejects unknown semantic"), CaptureSnapshot(Produce, DirectResult, ECaptureValueMode::AuthoringExport));
		TestEqual(TEXT("producer completed input validation"), Produced, 2);
		TestEqual(TEXT("same first accessor code"), DirectResult.Diagnostic.Code, ArrayResult.Diagnostic.Code);
		TestEqual(TEXT("same first semantic diagnostic"), DirectResult.SemanticId, ArrayResult.SemanticId);
		const auto BadProducer = [&](FReadRequestConsumer Consume, UE::UAI::Contract::FDiagnostic& ProducerError) {
			Consume(Requests[0]);
			ProducerError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("c92_input_fixture_invalid"), TEXT("Original builder failure."), TEXT("request_builder"));
			return false;
		};
		TestFalse(TEXT("builder failure retains original priority"), CaptureSnapshot(BadProducer, DirectResult, ECaptureValueMode::AuthoringExport));
		TestEqual(TEXT("builder diagnostic is not hidden by earlier read failure"), DirectResult.Diagnostic.Code, FString(TEXT("c92_input_fixture_invalid")));
		TestEqual(TEXT("failed input seals no snapshot"), DirectResult.Snapshot.Num(), 0);
		auto OwnedRequests = Requests;
		const auto ProduceOwned = [&](FOwnedReadRequestConsumer Consume, UE::UAI::Contract::FDiagnostic& ProducerError) {
			for (auto& Request : OwnedRequests) Consume(MoveTemp(Request));
			ProducerError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("c92_input_fixture_invalid"), TEXT("Original builder failure."), TEXT("request_builder"));
			return false;
		};
		TestFalse(TEXT("owned producer uses the same failure boundary"), CaptureSnapshotOwned(ProduceOwned, DirectResult, ECaptureValueMode::AuthoringExportLiterals));
		TestEqual(TEXT("owned builder retains original error priority"), DirectResult.Diagnostic.Code, FString(TEXT("c92_input_fixture_invalid")));
		TestEqual(TEXT("owned failed input seals no snapshot"), DirectResult.Snapshot.Num(), 0);
	}
	const FString Source = TEXT("incremental.hjson");
	{
		FUeAgentResourceTracker UnitResources(false);
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Value;
		UE::UAI::StateSemantic::FUeAgentStateSemanticError ValueError;
		if (!TestTrue(TEXT("original canonical property payload"), UE::UAI::StateSemantic::FUeAgentCanonicalValue::TryBoolean(false, UnitResources, Value, ValueError))) return false;
		const auto MakeUnit = [&](auto Values, TArray<FString> Dependencies = {}) {
			return FSemanticUnit(TEXT("property:inline-fixture"), ESemanticUnitKind::PropertySet,
				TEXT("target:inline-fixture"), TEXT("ue.level.component.property#value"), TEXT("properties"), TEXT("bVisible"),
				MoveTemp(Values), {}, {}, FString(), FString(), MoveTemp(Dependencies), {TEXT("llm_authoring")}, TEXT("fixture"));
		};
		TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef> Fields;
		Fields.Add(TEXT("value"), Value.ToSharedRef());
		const auto Legacy = MakeUnit(Fields);
		{
			const FString Target = Legacy.GetOwnerRef();
			const FSemanticUnit Root(Target, ESemanticUnitKind::TargetState, {}, TEXT("uai.asset.target"),
				TEXT("authoring_target"), {}, TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>{},
				{}, {}, {}, {}, {}, {TEXT("llm_authoring")}, TEXT("fixture"));
			const auto Published = FAuthoringModel::MakePropertySourceView(TEXT("fixture"), 1, TEXT("profile"), TEXT("before"), {Root}, {Target}, TEXT("fixture"));
			auto Record = MakeShared<FCommittedWorkspaceDelta>();
			Record->ContractId = Published->GetContractId(); Record->ContractVersion = Published->GetContractVersion();
			Record->ProfileId = Published->GetProfileId(); Record->AfterRevision = TEXT("after");
			Record->BeforeTargets = {Target}; Record->AfterTargets = {Target};
			Record->Changes.Add({Legacy.GetUnitId(), {}, Legacy});
			const TArray<TSharedPtr<const FCommittedWorkspaceDelta>> Records{Record};
			FCommittedWorkspaceDelta Folded; FWorkspaceError FoldError;
			if (!TestTrue(TEXT("a property without a declared dependency may keep its semantic type"),
				TryFoldCommittedWorkspaceDeltas(*Published, Records, UnitResources, Folded, FoldError))) return false;
			Record->Changes[0].After = MakeUnit(Fields, {TEXT("property_definition:missing")});
			TestFalse(TEXT("a property explicitly requiring an absent declaration cannot publish as generic"),
				TryFoldCommittedWorkspaceDeltas(*Published, Records, UnitResources, Folded, FoldError));
			TestEqual(TEXT("original unresolved reference contract"), FoldError.GetCode(), FString(TEXT("unresolved_semantic_unit_reference")));
			TestEqual(TEXT("the error identifies its actual consumer"), FoldError.GetSourceUnit(), Legacy.GetUnitId());
		}
		const auto Inline = MakeUnit(FSemanticUnit::FSinglePropertyValue{Value.ToSharedRef()});
		TestTrue(TEXT("same original unit validation"), Legacy.IsValid() && Inline.IsValid());
		TestEqual(TEXT("same declaration shape"), Inline.GetDeclaration().GetValueCount(), Legacy.GetDeclaration().GetValueCount());
		TestEqual(TEXT("same original case lookup"), Inline.FindValue(TEXT("VALUE")) != nullptr, Legacy.GetValues().Find(TEXT("VALUE")) != nullptr);
		FString EmbeddedKey(TEXT("value")); EmbeddedKey.AppendChar(TCHAR(0)); EmbeddedKey += TEXT("tail");
		TestEqual(TEXT("same original embedded-NUL lookup"), Inline.FindValue(EmbeddedKey) != nullptr, Legacy.GetValues().Find(EmbeddedKey) != nullptr);
		TestTrue(TEXT("single field reference belongs to immutable storage"), &Inline.FindValue(TEXT("value"))->Get() == Value.Get());
		TArray<uint8> A, B; FWorkspaceError UnitError; FString AH, BH;
		if (!TestTrue(TEXT("original row writer consumes both inputs"), TrySerializeSemanticUnitRow(Legacy, A, UnitError) && TrySerializeSemanticUnitRow(Inline, B, UnitError))) return false;
		TestTrue(TEXT("single field preserves exact row bytes"), A == B);
		if (!TestTrue(TEXT("original row SHA consumes both inputs"), TryHashSemanticUnitRow(Legacy, AH, UnitError) && TryHashSemanticUnitRow(Inline, BH, UnitError))) return false;
		TestEqual(TEXT("same original row SHA"), AH, BH);
		TSharedPtr<FJsonObject> Json;
		TestTrue(TEXT("explicit JSON projection remains available"), TryBuildSemanticModelUnitRow(Inline, Json, UnitError));
		TestFalse(TEXT("field and writer readers never request a map"), Inline.HasExpandedInlineValueMap());
		TestTrue(TEXT("unchanged WithValues preserves storage without map"), Inline.WithValues(Fields).SharesStorageWith(Inline));
		TestEqual(TEXT("explicit concrete map reader gets its field"), Inline.GetValues().Num(), 1);
		TestTrue(TEXT("explicit map is owned by this unit"), Inline.HasExpandedInlineValueMap());
		const auto FirstParallel = MakeUnit(FSemanticUnit::FSinglePropertyValue{Value.ToSharedRef()});
		const auto SecondParallel = MakeUnit(FSemanticUnit::FSinglePropertyValue{Value.ToSharedRef()});
		const FSemanticUnit* ParallelUnits[] = {&FirstParallel, &SecondParallel};
		bool Prepared[2] = {};
		ParallelFor(2, [&](int32 Index) { FWorkspaceError Failure; Prepared[Index] = TryPrepareSemanticUnitRowHash(*ParallelUnits[Index], Failure); });
		TestTrue(TEXT("same original hash can borrow a shared immutable leaf in tasks"), Prepared[0] && Prepared[1]);
		TestEqual(TEXT("task prepared row keeps original SHA"), FirstParallel.GetPublicRowHash(), AH);
		TestEqual(TEXT("both task rows keep original SHA"), SecondParallel.GetPublicRowHash(), AH);
		for (const auto Field : {FSemanticUnit::FSinglePropertyValue::EField::Literal, FSemanticUnit::FSinglePropertyValue::EField::Selected}) {
			const auto& Key = FSemanticUnit::FSinglePropertyValue::GetFieldName(Field);
			TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef> OriginalFields;
			OriginalFields.Add(Key, Value.ToSharedRef());
			const auto OriginalUnit = MakeUnit(MoveTemp(OriginalFields));
			const auto CapturedUnit = MakeUnit(FSemanticUnit::FSinglePropertyValue{Value.ToSharedRef(), Field});
			TestTrue(TEXT("actual captured key is directly readable"), CapturedUnit.FindValue(Key) && !CapturedUnit.FindValue(TEXT("value")));
			if (!TestTrue(TEXT("native single field uses original row writer"), TrySerializeSemanticUnitRow(OriginalUnit, A, UnitError)
				&& TrySerializeSemanticUnitRow(CapturedUnit, B, UnitError))) return false;
			TestTrue(TEXT("native single field exact row bytes"), A == B);
			if (!TestTrue(TEXT("native single field uses original row hash"), TryHashSemanticUnitRow(OriginalUnit, AH, UnitError)
				&& TryHashSemanticUnitRow(CapturedUnit, BH, UnitError))) return false;
			TestEqual(TEXT("native single field exact row SHA"), AH, BH);
			TestFalse(TEXT("native single field never materializes a map"), CapturedUnit.HasExpandedInlineValueMap());
		}
	}
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		for (int32 Fields = 0; Fields <= 4; ++Fields) {
			if (Fields == 1) Identity.ModelId = TEXT("model");
			if (Fields == 2) Identity.TargetKind = TEXT("kind");
			if (Fields == 3) Identity.KeyPattern = TEXT("pattern");
			if (Fields == 4) Identity.CanonicalKey = TEXT("nonempty-key");
			TestEqual(TEXT("identity presence result keeps original diagnostic contract"), Identity.IsValid(), Identity.GetValidationDiagnostic().IsEmpty());
		}
	}
	const FString Original = TEXT("{\r\nA: { value: 100 }\r\nB: { text: \"中文\", nested: [1, 2] }\r\nC: { value: 3 }\r\n}\r\n");
	FUeAgentResourceTracker Resources;
	FHjsonDocumentPtr Previous; FWorkspaceError Error;
	if (!TestTrue(TEXT("original source parses"), TryParseHjsonDocument(Original, Source, Resources, Previous, Error))) return false;
	{
		const FString Plain = FString::ChrN(3073, TEXT('a'));
		const FString Text = TEXT("{ key: \"") + Plain + TEXT("\\t\\\"中文\" }\r\n");
		FHjsonDocumentPtr Parsed;
		if (!TestTrue(TEXT("ASCII runs cross checkpoints without a second scanner"), TryParseHjsonDocument(Text, Source, Resources, Parsed, Error))) return false;
		const auto* Node = Parsed->Find(TEXT("/key"));
		if (!TestNotNull(TEXT("ASCII run member exists"), Node)) return false;
		TestEqual(TEXT("ASCII run preserves escaped and Unicode values"), Node->GetText(), Plain + TEXT("\t\"中文"));
		TestEqual(TEXT("ASCII run token starts at actual byte"), Node->GetProvenance().GetByteStart(), int64(7));
		TestEqual(TEXT("ASCII run token ends at actual byte"), Node->GetProvenance().GetByteEnd(), int64(FTCHARToUTF8(*Text).Length() - 4));
		TestFalse(TEXT("bad escape after long run still fails"), TryParseHjsonDocument(TEXT("{ key: \"") + Plain + TEXT("\\q\" }"), Source, Resources, Parsed, Error));
		TestEqual(TEXT("original bad escape diagnostic"), Error.GetCode(), FString(TEXT("hjson_escape_invalid")));
		TestEqual(TEXT("original bad escape column after run"), Error.GetColumn(), 3083);
	}
	{
		FTargetCompositionRegistry Registry; FTargetCompositionContract RouteContract;
		RouteContract.SchemaId = GetTargetCompositionContractSchemaId(); RouteContract.TargetKind = TEXT("c98.local.routes");
		RouteContract.Surfaces = {{TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet},
			{TEXT("properties"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet},
			{TEXT("graph"), EAuthoringSurfaceKind::Structure, ESemanticUnitKind::StructureState}};
		RouteContract.SemanticRoutes = {
			{ECompositionSemanticRouteMatch::Exact, TEXT("x.exact"), TEXT("target_settings"), ESemanticUnitKind::TargetState, ECompositionSemanticRouteRole::TargetValues, 30},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".tail"), TEXT("properties"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 20},
			{ECompositionSemanticRouteMatch::Contains, TEXT(".mid."), TEXT("graph"), ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 10},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("properties"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0}};
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!TestTrue(TEXT("local route owner accepts original contract"), Registry.RegisterContract(RouteContract, RegistrationError))) return false;
		for (const auto& Example : TArray<TPair<FString, FString>>{{TEXT("x.exact"), TEXT("target_settings")},
			{TEXT("x.mid.tail"), TEXT("properties")}, {TEXT("x.mid.y"), TEXT("graph")}, {TEXT("unknown"), TEXT("properties")}}) {
			FCompositionSemanticRouteContract Route;
			if (!TestTrue(TEXT("original route rule resolves without complete copy"), Registry.ResolveSemanticRoute(RouteContract.TargetKind, Example.Key, Route))) return false;
			TestEqual(TEXT("same route priority and match"), Route.SurfaceId, Example.Value);
		}
		FTargetCompositionContract Copy;
		TestTrue(TEXT("complete mutable-copy API remains"), Registry.FindContract(RouteContract.TargetKind, Copy));
		Copy.SemanticRoutes.Empty(); FCompositionSemanticRouteContract Route;
		TestTrue(TEXT("mutable consumer cannot change registered routes"), Registry.ResolveSemanticRoute(RouteContract.TargetKind, TEXT("x.exact"), Route));
		TestEqual(TEXT("original registered route remains"), Route.SurfaceId, FString(TEXT("target_settings")));
		TestFalse(TEXT("unknown target has no implicit route"), Registry.ResolveSemanticRoute(TEXT("missing"), TEXT("x.exact"), Route));
		TestTrue(TEXT("failed route output reset"), Route.SurfaceId.IsEmpty());
	}
	// The scanner's byte coordinates must agree with the actual UTF-8 source,
	// independently of the incremental parser serving as its own oracle.
	const FString Unicode = TEXT("{\r\ns: \"中文😀\"\r\na: [true, 1]\r\n}");
	FHjsonDocumentPtr UnicodeDocument;
	if (!TestTrue(TEXT("single scan accepts CRLF and a supplementary scalar"),
		TryParseHjsonDocument(Unicode, TEXT("unicode.hjson"), Resources, UnicodeDocument, Error))) return false;
	const auto* UnicodeValue = UnicodeDocument->Find(TEXT("/s"));
	const auto* ArrayValue = UnicodeDocument->Find(TEXT("/a/0"));
	if (!TestNotNull(TEXT("unicode member"), UnicodeValue) || !TestNotNull(TEXT("array member"), ArrayValue)) return false;
	TestEqual(TEXT("decoded unicode text"), UnicodeValue->GetText(), FString(TEXT("中文😀")));
	TestEqual(TEXT("unicode source byte begin"), UnicodeValue->GetProvenance().GetByteStart(), int64(6));
	TestEqual(TEXT("unicode source byte end"), UnicodeValue->GetProvenance().GetByteEnd(), int64(18));
	TestEqual(TEXT("next line array byte begin"), ArrayValue->GetProvenance().GetByteStart(), int64(24));
	TestEqual(TEXT("next line array byte end"), ArrayValue->GetProvenance().GetByteEnd(), int64(28));
	TestEqual(TEXT("CRLF line"), ArrayValue->GetProvenance().GetLine(), 3);
	const FString Escaped = TEXT("{ text: \"\\u4e2d\\n\\\"\", quoted: \"False\" }");
	if (!TestTrue(TEXT("escape decoder remains distinct from the direct string span"),
		TryParseHjsonDocument(Escaped, TEXT("escaped.hjson"), Resources, UnicodeDocument, Error))) return false;
	TestEqual(TEXT("escaped decoded text"), UnicodeDocument->Find(TEXT("/text"))->GetText(), FString(TEXT("中\n\"")));
	TestEqual(TEXT("quoted non-literal"), UnicodeDocument->Find(TEXT("/quoted"))->GetText(), FString(TEXT("False")));
	FHjsonNodePtr OneUseRoot;
	if (!TestTrue(TEXT("one-use complete text uses the original grammar"), TryParseHjsonSelectedNodes(
		Unicode, TEXT("unicode.hjson"), Resources, OneUseRoot, Error,
		[](const FString&, const FString&) { return true; }))) return false;
	const auto* OneUseArrayValue = OneUseRoot->Find(TEXT("/a/0"));
	if (!TestNotNull(TEXT("one-use complete root field"), OneUseArrayValue)) return false;
	TestEqual(TEXT("complete input has original byte location"), OneUseArrayValue->GetProvenance().GetByteStart(), int64(24));
	TestEqual(TEXT("complete input has original line"), OneUseArrayValue->GetProvenance().GetLine(), 3);
	TestEqual(TEXT("complete input has original key spelling"), OneUseRoot->Find(TEXT("/s"))->GetProvenance().GetOriginalKey(), FString(TEXT("s")));
	FHjsonDocumentPtr DuplicateDocument;
	TestFalse(TEXT("single Add key check preserves duplicate diagnostics"), TryParseHjsonDocument(
		TEXT("{ A: 1, A: 2 }"), TEXT("duplicate.hjson"), Resources, DuplicateDocument, Error));
	TestEqual(TEXT("duplicate diagnostic code"), Error.GetCode(), FString(TEXT("hjson_duplicate_key")));
	TestFalse(TEXT("delimiter still identifies a missing value"), TryParseHjsonDocument(
		TEXT("{ a: }"), TEXT("delimiter-error.hjson"), Resources, DuplicateDocument, Error));
	TestEqual(TEXT("delimiter diagnostic code"), Error.GetCode(), FString(TEXT("hjson_value_missing")));
	if (!TestTrue(TEXT("delimiter diagnostic has actual spelling"), Error.GetDiagnostic().ActualValue.IsValid())) return false;
	TestEqual(TEXT("delimiter exact spelling"), Error.GetDiagnostic().ActualValue->AsString(), FString(TEXT("}")));
	FHjsonDocumentPtr PointerDocument;
	if (!TestTrue(TEXT("moved parser provenance retains escaped path"), TryParseHjsonDocument(
		TEXT("{ \"a/b~c\": { value: 1 }, A: 2, a: 3 }"), TEXT("pointer-move.hjson"), Resources, PointerDocument, Error))) return false;
	const auto* PointerValue = PointerDocument->Find(TEXT("/a~1b~0c/value"));
	if (!TestNotNull(TEXT("escaped child identity"), PointerValue)) return false;
	TestEqual(TEXT("exact escaped provenance"), PointerValue->GetProvenance().GetJsonPointer(), FString(TEXT("/a~1b~0c/value")));
	TestEqual(TEXT("source key survives the move"), PointerValue->GetProvenance().GetOriginalKey(), FString(TEXT("value")));
	TestEqual(TEXT("case-distinct parser keys remain distinct"), PointerDocument->GetRoot()->GetEntries().Num(), 3);
	TestFalse(TEXT("nonfinite rejection retains pre-move path"), TryParseHjsonDocument(
		TEXT("{ \"a/b~c\": +Infinity }"), TEXT("pointer-error.hjson"), Resources, PointerDocument, Error));
	TestEqual(TEXT("nonfinite code unchanged"), Error.GetCode(), FString(TEXT("hjson_nonfinite_number")));
	TestEqual(TEXT("nonfinite exact source path"), Error.GetJsonPointer(), FString(TEXT("/a~1b~0c")));
	FHjsonNodePtr OmittedRoot;
	TestFalse(TEXT("omitted subtree still diagnoses duplicate keys"), TryParseHjsonSelectedNodes(
		TEXT("{ drop: { a: 1, a: 2 }, keep: true }"), TEXT("omitted-keys.hjson"), Resources, OmittedRoot, Error,
		[](const FString& Pointer, const FString&) { return !Pointer.StartsWith(TEXT("/drop")); }));
	TestEqual(TEXT("omitted duplicate diagnosis"), Error.GetCode(), FString(TEXT("hjson_duplicate_key")));
	// UE's FString operators already accept these literals ignoring case.
	// Preserve that current behavior; do not silently introduce a new syntax rule.
	if (!TestTrue(TEXT("bare keyword comparison compatibility"), TryParseHjsonDocument(
		TEXT("{ upper: TRUE, lower: true, text: trueish\n}"), TEXT("keyword-compat.hjson"), Resources, PointerDocument, Error))) { AddError(Error.GetCode()); return false; }
	TestTrue(TEXT("upper bool actual value"), PointerDocument->Find(TEXT("/upper"))->GetBoolean());
	TestEqual(TEXT("ordinary bare text stays text"), PointerDocument->Find(TEXT("/text"))->GetText(), FString(TEXT("trueish")));
	const TCHAR NullPointerKey[] {TEXT('a'), TCHAR(0), TEXT('b')};
	TestEqual(TEXT("public pointer escape preserves unmodified NUL extent"), EscapeJsonPointerSegment(FString(3, NullPointerKey)).Len(), 3);
	const TCHAR NullThenEscape[] {TEXT('a'), TCHAR(0), TEXT('/')};
	TestEqual(TEXT("public pointer escape preserves old replacement boundary"), EscapeJsonPointerSegment(FString(3, NullThenEscape)), FString(TEXT("a")));
	TestEqual(TEXT("writer pointer keeps its old omitted-NUL policy"), PointerText::Child(TEXT("/"), FString(3, NullPointerKey), PointerText::ENulMode::Omit), FString(TEXT("/ab")));
	const TCHAR NullRoot[] {TEXT('/'), TCHAR(0), TEXT('x')};
	TestEqual(TEXT("pointer root preserves FString literal comparison"), PointerText::Child(FString(3, NullRoot), TEXT("child"), PointerText::ENulMode::Omit), FString(TEXT("/child")));
	FUeAgentResourceTracker FixedCharges; FUeAgentResourceChargeError ChargeError;
	TestTrue(TEXT("fixed charges retain map last-value semantics"), FixedCharges.TryChargeMany(
		{{EUeAgentResourceDimension::Nodes, -1}, {EUeAgentResourceDimension::Items, 4}, {EUeAgentResourceDimension::Nodes, 3}}, TEXT("fixed"), ChargeError));
	TestEqual(TEXT("last node amount"), FixedCharges.Snapshot().Consumed(EUeAgentResourceDimension::Nodes), int64(3));
	TestFalse(TEXT("negative fixed charge is rejected atomically"), FixedCharges.TryChargeMany(
		{{EUeAgentResourceDimension::Nodes, 1}, {EUeAgentResourceDimension::Items, -1}}, TEXT("fixed"), ChargeError));
	TestEqual(TEXT("failed request did not change nodes"), FixedCharges.Snapshot().Consumed(EUeAgentResourceDimension::Nodes), int64(3));
	// Exercise the formal identity owner, using an isolated registry and fixed original grammar.
	{
		using namespace UeAgentAuthoringIdentity;
		FRegistry Registry;
		FModelDescriptor Model;
		Model.ModelId = TEXT("escape_model"); Model.TargetKind = TEXT("target");
		Model.Selectors = {{TEXT("value"), 0, true, false}};
		Model.KeyPatterns = {{TEXT("pattern"), 0, {TEXT("value")}}};
		ERegistrationResult Registration;
		UE::UAI::Contract::FDiagnostic IdentityError;
		if (!TestTrue(TEXT("local identity registration"), Registry.RegisterModel(Model, Registration, IdentityError))) return false;
		FCanonicalIdentity Identity, Parsed;
		const FString Value = TEXT("A%|&=\r\n\t中%25");
		const TMap<FString, FString> PublicSelectors = {{TEXT("value"), Value}};
		if (!TestTrue(TEXT("identity encodes all original delimiters"), BuildCanonicalIdentity(
			Model.ModelId, Model.TargetKind, PublicSelectors, Identity, IdentityError, &Registry))) return false;
		TestEqual(TEXT("public const selector input is not consumed"), PublicSelectors.FindChecked(TEXT("value")), Value);
		TestEqual(TEXT("identity exact original escaped spelling"), Identity.CanonicalKey,
			FString(TEXT("escape_model|target|pattern|value=A%25%7C%26%3D%0D%0A%09中%2525")));
		if (!TestTrue(TEXT("identity decoder does not decode introduced percent twice"),
			ParseCanonicalKey(Identity.CanonicalKey, Parsed, IdentityError, &Registry))) return false;
		TestEqual(TEXT("identity selectors round-trip"), Parsed.GetSelector(TEXT("value")), Value);
		TestFalse(TEXT("unknown escapes do not become decoded values"), ParseCanonicalKey(
			TEXT("escape_model|target|pattern|value=%GG"), Parsed, IdentityError, &Registry));
		TestEqual(TEXT("unknown escape retains canonical diagnostic"), IdentityError.Code,
			FString(TEXT("canonical_identity_key_noncanonical")));
		if (!TestTrue(TEXT("unknown percent is represented literally"), ParseCanonicalKey(
			TEXT("escape_model|target|pattern|value=%25GG"), Parsed, IdentityError, &Registry))) return false;
		TestEqual(TEXT("unknown decoded text"), Parsed.GetSelector(TEXT("value")), FString(TEXT("%GG")));
		TestFalse(TEXT("normalized selector conflict still rejected"), BuildCanonicalIdentity(
			Model.ModelId, Model.TargetKind, {{TEXT("value"), TEXT("a")}, {TEXT(" VALUE "), TEXT("b")}},
			Parsed, IdentityError, &Registry));
		TestEqual(TEXT("selector conflict code"), IdentityError.Code, FString(TEXT("identity_selector_conflict")));
		const TCHAR EmbeddedNull[] {TEXT('a'), TEXT('\0'), TEXT('b')};
		const FString NullValue(3, EmbeddedNull);
		if (!TestTrue(TEXT("identity preserves unescaped FString extent"), BuildCanonicalIdentity(
			Model.ModelId, Model.TargetKind, {{TEXT("value"), NullValue}}, Identity, IdentityError, &Registry))) return false;
		TestEqual(TEXT("unescaped embedded NUL has original extent"), Identity.CanonicalKey.Len(),
			FString(TEXT("escape_model|target|pattern|value=")).Len() + 3);
		const TCHAR NullBeforeEscape[] {TEXT('a'), TEXT('\0'), TEXT('%')};
		if (!TestTrue(TEXT("identity keeps original replacement NUL behavior"), BuildCanonicalIdentity(
			Model.ModelId, Model.TargetKind, {{TEXT("value"), FString(3, NullBeforeEscape)}}, Identity, IdentityError, &Registry))) return false;
		TestEqual(TEXT("replacement truncates at original NUL boundary"), Identity.CanonicalKey,
			FString(TEXT("escape_model|target|pattern|value=a")));
		Model.ModelId = TEXT("fold_model"); Model.Selectors[0].bCaseSensitive = false;
		if (!TestTrue(TEXT("local folded identity registration"), Registry.RegisterModel(Model, Registration, IdentityError))
			|| !TestTrue(TEXT("folded identity"), BuildCanonicalIdentity(Model.ModelId, Model.TargetKind,
				{{TEXT("value"), TEXT("Ab%|中")}}, Identity, IdentityError, &Registry))) return false;
		TestEqual(TEXT("selector fold precedes exact escapes"), Identity.CanonicalKey,
			FString(TEXT("fold_model|target|pattern|value=ab%25%7C中")));
		const FCanonicalIdentity Valid = Identity;
		// The default full parser remains the owning-output oracle. The resolver
		// and diagnostic-only receiver both choose validation for key-only input.
		const auto CheckDirectIdentity = [&](const FCanonicalIdentity& Candidate) {
			FCanonicalIdentity Full;
			FDiagnostic FullError, ValidationError;
			const bool bFull = ParseCanonicalKey(Candidate.CanonicalKey, Full, FullError, &Registry);
			FString HeaderModel, HeaderKind;
			const bool bValidated = ValidateCanonicalKey(Candidate.CanonicalKey, Registry,
				Registry.FindModelView(TEXT("fold_model")), HeaderModel, HeaderKind, ValidationError);
			if (!TestEqual(TEXT("validation uses the full parser grammar"), bValidated, bFull)
				|| !TestEqual(TEXT("validation preserves complete original parser diagnostics"),
					FUeAgentStructuredHjsonFormatter::WriteObject(ValidationError.ToPublicJson()),
					FUeAgentStructuredHjsonFormatter::WriteObject(FullError.ToPublicJson()))) return false;
			if (bFull && (!TestEqual(TEXT("validation model is the actual parsed model"), HeaderModel, Full.ModelId)
				|| !TestEqual(TEXT("validation kind is the actual parsed kind"), HeaderKind, Full.TargetKind))) return false;
			UeAgentAuthoringTargetLocator::FTargetQuery Query;
			Query.ModelId = Candidate.ModelId; Query.TargetKind = Candidate.TargetKind; Query.CanonicalKey = Candidate.CanonicalKey;
			UeAgentAuthoringTargetLocator::FResolveResult Original;
			const bool Expected = UeAgentAuthoringTargetLocator::ResolveExact(Query, {Candidate}, Original, -1, &Registry);
			FDiagnostic Actual;
			const bool Got = UeAgentAuthoringTargetLocator::ValidateIdentity(Candidate, Actual, &Registry);
			return TestEqual(TEXT("borrowed identity keeps original resolution outcome"), Got, Expected)
				&& TestEqual(TEXT("borrowed identity keeps full original public diagnostic"),
					FUeAgentStructuredHjsonFormatter::WriteObject(Actual.ToPublicJson()),
					FUeAgentStructuredHjsonFormatter::WriteObject(Original.Diagnostic.ToPublicJson()));
		};
		if (!CheckDirectIdentity(Valid)) return false;
		for (int32 Mutation = 0; Mutation < 8; ++Mutation) {
			FCanonicalIdentity Candidate = Valid;
			switch (Mutation) {
			case 0: Candidate.CanonicalKey.Reset(); break;
			case 1: Candidate.ModelId = TEXT("unknown"); break;
			case 2: Candidate.TargetKind = TEXT("different"); break;
			case 3: Candidate.CanonicalKey += TEXT("|extra"); break;
			case 4: Candidate.CanonicalKey = TEXT("fold_model|target|wrong|value=x"); break;
			case 5: Candidate.CanonicalKey = TEXT("fold_model|target|pattern|=x"); break;
			case 6: Candidate.CanonicalKey = TEXT("fold_model|target|pattern|value=%GG"); break;
			case 7: Candidate.Selectors.Add(TEXT("unused"), TEXT("not a self-query selector")); break;
			}
			if (!CheckDirectIdentity(Candidate)) return false;
		}
		for (const FString& ExtraValue : {NullValue, FString::ChrN(300, TEXT('A')) + TEXT("%|中"),
			FString(3, NullBeforeEscape)}) {
			if (!BuildCanonicalIdentity(TEXT("fold_model"), TEXT("target"), {{TEXT("value"), ExtraValue}},
				Identity, IdentityError, &Registry) || !CheckDirectIdentity(Identity)) return false;
		}
		FCanonicalIdentity Aliased = Valid;
		if (!TestTrue(TEXT("original parser can consume its own output key without dangling views"),
			ParseCanonicalKey(Aliased.CanonicalKey, Aliased, IdentityError, &Registry))) return false;
		TestEqual(TEXT("aliased parse preserves exact original key"), Aliased.CanonicalKey, Valid.CanonicalKey);
		// A non-trimming model can legitimately keep trailing selector space;
		// query trimming must still produce the original not-found diagnostic.
		if (!BuildCanonicalIdentity(TEXT("escape_model"), TEXT("target"), {{TEXT("value"), TEXT("x ")}}, Identity, IdentityError, &Registry)
			|| !CheckDirectIdentity(Identity)) return false;

	}
	// The new writer input borrows the same actual values; it must preserve the
	// original canonical envelope's bytes, including array strings and numbers.
	using namespace UE::UAI::StateSemantic;
	FUeAgentResourceTracker Uncounted(false), Counted;
	FUeAgentStateSemanticError SemanticError;
	FUeAgentCanonicalValuePtr LabelValue, RefValue, RefsValue, NumberValue, DecimalValue, ValuesValue, EnvelopeValue;
	const FString Label = TEXT("中文\"value\n😀");
	TestEqual(TEXT("quoted empty text"), FUeAgentStructuredHjsonFormatter::EscapeQuotedString(FString()), FString(TEXT("\"\"")));
	TestEqual(TEXT("quoted ordinary run"), FUeAgentStructuredHjsonFormatter::EscapeQuotedString(TEXT("abc/中文😀")), FString(TEXT("\"abc/中文😀\"")));
	TestEqual(TEXT("quoted escaped runs retain original grammar"), FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Label),
		FString(TEXT("\"中文\\\"value\\n😀\"")));
	TestEqual(TEXT("quoted backslash"), FUeAgentStructuredHjsonFormatter::EscapeQuotedString(TEXT("a\\b")), FString(TEXT("\"a\\\\b\"")));
	TCHAR Controls[32];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Controls); ++Index) Controls[Index] = static_cast<TCHAR>(Index);
	const FString ControlText = FString::ConstructFromPtrSize(Controls, UE_ARRAY_COUNT(Controls));
	TestEqual(TEXT("control fixture retains all code units"), ControlText.Len(), 32);
	TestEqual(TEXT("all control spellings including embedded NUL"), FUeAgentStructuredHjsonFormatter::EscapeQuotedString(ControlText),
		FString(TEXT("\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f\"")));
	FHjsonDocumentPtr QuotedDocument;
	if (!TestTrue(TEXT("quoted output roundtrips through the original parser"), TryParseHjsonDocument(
		TEXT("{ value: ") + FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Label) + TEXT(" }"),
		TEXT("quoted-output.hjson"), Resources, QuotedDocument, Error))) return false;
	TestEqual(TEXT("quoted decoded value is exact"), QuotedDocument->Find(TEXT("/value"))->GetText(), Label);
	{
		// Exercise the actual composer's conversion boundary, before route/model
		// construction. Missing storage still precedes scalar conversion errors.
		FCompositionRequest Input;
		Input.Entries.AddDefaulted();
		auto Invalid = MakeShared<FJsonObject>();
		Invalid->SetNumberField(TEXT("a"), std::numeric_limits<double>::quiet_NaN());
		Invalid->Values.Add(TEXT("z/x~"), nullptr);
		Input.Entries[0].Value = MakeShared<FJsonValueObject>(Invalid);
		FAuthoringModelPtr UnusedModel;
		FWorkspaceError ConversionError;
		TestFalse(TEXT("missing child is rejected before numeric conversion"), TryComposeAuthoringModel(Input, Uncounted, UnusedModel, ConversionError));
		TestEqual(TEXT("same missing-child priority"), ConversionError.GetCode(), FString(TEXT("composition_json_value_invalid")));
		TestEqual(TEXT("lazy path retains pointer escapes"), ConversionError.GetJsonPointer(), FString(TEXT("/z~1x~0")));
		Invalid->Values.Remove(TEXT("z/x~"));
		ConversionError.Reset();
		TestFalse(TEXT("finite exact numeric grammar remains required"), TryComposeAuthoringModel(Input, Uncounted, UnusedModel, ConversionError));
		TestEqual(TEXT("same numeric failure"), ConversionError.GetCode(), FString(TEXT("canonical_json_number_lexeme_invalid")));
		TestEqual(TEXT("numeric factory keeps its original diagnostic root"), ConversionError.GetJsonPointer(), FString(TEXT("/")));
		auto Cycle = MakeShared<FJsonObject>();
		Cycle->SetObjectField(TEXT("self"), Cycle);
		Input.Entries[0].Value = MakeShared<FJsonValueObject>(Cycle);
		ConversionError.Reset();
		const bool bComposedCycle = TryComposeAuthoringModel(Input, Uncounted, UnusedModel, ConversionError);
		Cycle->RemoveField(TEXT("self"));
		TestFalse(TEXT("conversion never substitutes an alternate route for cycles"), bComposedCycle);
		TestEqual(TEXT("same unavailable child diagnostic"), ConversionError.GetCode(), FString(TEXT("composition_json_child_missing")));
		TestEqual(TEXT("cycle retains first provenance and child path"), ConversionError.GetJsonPointer(), FString(TEXT("/self/self")));
	}
	const TArray<FString> Refs{TEXT("a"), TEXT("b/中文")};
	TestTrue(TEXT("counted string factory"), FUeAgentCanonicalValue::TryString(Label, Counted, LabelValue, SemanticError));
	TestEqual(TEXT("counted UTF-8 bytes retained"), Counted.Snapshot().Consumed(EUeAgentResourceDimension::StringBytes), int64(FTCHARToUTF8(*Label).Length()));
	TestTrue(TEXT("uncounted string retains actual text"), FUeAgentCanonicalValue::TryString(Label, Uncounted, LabelValue, SemanticError));
	TArray<FUeAgentCanonicalValueRef> RefValues;
	for (const auto& Ref : Refs) {
		if (!TestTrue(TEXT("reference string"), FUeAgentCanonicalValue::TryString(Ref, Uncounted, RefValue, SemanticError))) return false;
		RefValues.Add(RefValue.ToSharedRef());
	}
	if (!TestTrue(TEXT("reference array"), FUeAgentCanonicalValue::TryArray(RefValues, Uncounted, RefsValue, SemanticError))
		|| !TestTrue(TEXT("exact large integer"), FUeAgentCanonicalValue::TryInteger(TEXT("9007199254740993"), Uncounted, NumberValue, SemanticError))
		|| !TestTrue(TEXT("decimal spelling"), FUeAgentCanonicalValue::TryDecimal(TEXT("0.0"), Uncounted, DecimalValue, SemanticError))) return false;
	const TMap<FString, FUeAgentCanonicalValueRef> ActualValues{{TEXT("large"), NumberValue.ToSharedRef()}, {TEXT("decimal"), DecimalValue.ToSharedRef()}};
	if (!TestTrue(TEXT("old values envelope"), FUeAgentCanonicalValue::TryObject(
		{{TEXT("large"), NumberValue.ToSharedRef()}, {TEXT("decimal"), DecimalValue.ToSharedRef()}}, Uncounted, ValuesValue, SemanticError))
		|| !TestTrue(TEXT("old row envelope"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("label"), LabelValue.ToSharedRef()}, {TEXT("refs"), RefsValue.ToSharedRef()}, {TEXT("values"), ValuesValue.ToSharedRef()}}, Uncounted, EnvelopeValue, SemanticError))) return false;
	const FString LabelKey = TEXT("label"), RefsKey = TEXT("refs"), ValuesKey = TEXT("values");
	const FUeAgentStructuredHjsonObjectFieldView BorrowedFields[]{{LabelKey, Label}, {RefsKey, Refs}, {ValuesKey, ActualValues}};
	const FUeAgentStructuredHjsonObjectView BorrowedObject{MakeArrayView(BorrowedFields)};
	FUeAgentStructuredHjsonWriteOptions WriteOptions; WriteOptions.bRequireJsonCompatibleObjectKeys = true;
	UE::UAI::Contract::FDiagnostic WriteError;
	FString CanonicalText, BorrowedText, VisitedText;
	if (!TestTrue(TEXT("original envelope formatter"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*EnvelopeValue, WriteOptions, CanonicalText, WriteError))
		|| !TestTrue(TEXT("borrowed envelope formatter"), FUeAgentStructuredHjsonFormatter::TryWriteValue(BorrowedObject, WriteOptions, BorrowedText, WriteError))
		|| !TestTrue(TEXT("borrowed envelope token consumer"), FUeAgentStructuredHjsonFormatter::TryVisitValueText(BorrowedObject, WriteOptions,
			[&](FStringView Token) { VisitedText.AppendChars(Token.GetData(), Token.Len()); }, WriteError))) return false;
	TestEqual(TEXT("borrowed envelope preserves exact formatted text"), BorrowedText, CanonicalText);
	TestEqual(TEXT("digest token path preserves exact formatted text"), VisitedText, CanonicalText);
	TestEqual(TEXT("disabled accounting produces no usage"), Uncounted.Snapshot().Consumed(EUeAgentResourceDimension::StringBytes), int64(0));
	TestFalse(TEXT("disabled accounting does not disable numeric validation"), FUeAgentCanonicalValue::TryInteger(TEXT("01"), Uncounted, NumberValue, SemanticError));
	TestFalse(TEXT("disabled accounting does not disable duplicate validation"), FUeAgentCanonicalValue::TryObject(
		{{TEXT("same"), LabelValue.ToSharedRef()}, {TEXT("same"), LabelValue.ToSharedRef()}}, Uncounted, EnvelopeValue, SemanticError));
	TArray<TPair<FString, FUeAgentCanonicalValueRef>> MutableKeys{
		{TEXT("first"), LabelValue.ToSharedRef()}, {TEXT("second"), LabelValue.ToSharedRef()}};
	if (!TestTrue(TEXT("duplicate check may borrow only within the factory"),
		FUeAgentCanonicalValue::TryObject(MutableKeys, Uncounted, EnvelopeValue, SemanticError))) return false;
	MutableKeys[0].Key = TEXT("changed"); MutableKeys.Reset();
	TestEqual(TEXT("sealed object retains its own exact keys"), EnvelopeValue->GetObjectItems()[0].Key, FString(TEXT("first")));
	{
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>> Entries{
			{TEXT("/z/中文"), LabelValue.ToSharedRef()}, {TEXT("/a"), DecimalValue.ToSharedRef()}};
		FUeAgentResourceTracker OwningResources, BorrowedResources;
		FUeAgentCanonicalValuePtr At, ReadOnly, EditFile, Guide;
		const FString Path = TEXT("targets/灯/属性.hjson");
		if (!TestTrue(TEXT("original guide at object"), FUeAgentCanonicalValue::TryObject(Entries, OwningResources, At, SemanticError))
			|| !TestTrue(TEXT("borrowed guide uses original member validation"), FUeAgentCanonicalValue::TryValidateObjectMembers(Entries, BorrowedResources, SemanticError))
			|| !TestTrue(TEXT("original guide boolean"), FUeAgentCanonicalValue::TryBoolean(true, Uncounted, ReadOnly, SemanticError))
			|| !TestTrue(TEXT("original guide path"), FUeAgentCanonicalValue::TryString(Path, Uncounted, EditFile, SemanticError))
			|| !TestTrue(TEXT("original guide root"), FUeAgentCanonicalValue::TryObject({{TEXT("at"), At.ToSharedRef()},
				{TEXT("read_only"), ReadOnly.ToSharedRef()}, {TEXT("edit_file"), EditFile.ToSharedRef()}}, Uncounted, Guide, SemanticError))) return false;
		for (const auto Dimension : {EUeAgentResourceDimension::Nodes, EUeAgentResourceDimension::Items,
			EUeAgentResourceDimension::StringBytes, EUeAgentResourceDimension::WorkUnits, EUeAgentResourceDimension::EstimatedMemoryBytes})
			TestEqual(TEXT("member-only validation preserves original logical charges"),
				BorrowedResources.Snapshot().Consumed(Dimension), OwningResources.Snapshot().Consumed(Dimension));
		const FString AtKey = TEXT("at"), FileKey = TEXT("edit_file"), ReadOnlyKey = TEXT("read_only");
		TArray<FUeAgentStructuredHjsonObjectFieldView> AtFields;
		for (const auto& Entry : Entries) AtFields.Emplace(Entry.Key, Entry.Value.Get());
		const FUeAgentStructuredHjsonObjectView AtView{MakeArrayView(AtFields)};
		const FUeAgentStructuredHjsonObjectFieldView RootFields[]{{AtKey, AtView}, {FileKey, Path}, {ReadOnlyKey, *ReadOnly}};
		const FUeAgentStructuredHjsonObjectView Root{MakeArrayView(RootFields)};
		TArray<uint8> OldBytes, DirectBytes;
		if (!TestTrue(TEXT("original semantic guide serializer"), TrySerializeSemanticHjsonObject(*Guide, OldBytes, Error, Path))
			|| !TestTrue(TEXT("borrowed guide uses same semantic serializer"), TrySerializeSemanticHjsonObject(Root, DirectBytes, Error, Path))) return false;
		TestTrue(TEXT("guide exact bytes include original leaf projection, sorting, UTF8 and final newline"), OldBytes == DirectBytes);
		for (const FString& InvalidKey : {FString(), FString(TEXT("same"))}) {
			const TArray<TPair<FString, FUeAgentCanonicalValueRef>> Invalid{{InvalidKey, LabelValue.ToSharedRef()}, {InvalidKey, LabelValue.ToSharedRef()}};
			FUeAgentStateSemanticError FullError, BorrowedError;
			TestFalse(TEXT("original invalid object rejected"), FUeAgentCanonicalValue::TryObject(Invalid, Uncounted, At, FullError));
			TestFalse(TEXT("borrowed invalid object rejected"), FUeAgentCanonicalValue::TryValidateObjectMembers(Invalid, Uncounted, BorrowedError));
			TestEqual(TEXT("borrowed member validation keeps complete original diagnostic"),
				FUeAgentStructuredHjsonFormatter::WriteObject(FullError.ToPublicJson()),
				FUeAgentStructuredHjsonFormatter::WriteObject(BorrowedError.ToPublicJson()));
		}
	}
	FHjsonNodePtr UncountedRoot;
	if (!TestTrue(TEXT("uncounted lexer still validates actual source"), TryParseHjsonSelectedNodes(
		Unicode, TEXT("uncounted-lexer.hjson"), Uncounted, UncountedRoot, Error,
		[](const FString&, const FString&) { return true; }))) return false;
	TestEqual(TEXT("uncounted lexer preserves real UTF8 coordinates"), UncountedRoot->Find(TEXT("/s"))->GetProvenance().GetByteEnd(), int64(18));
	TestEqual(TEXT("uncounted lexer has no input accounting"), Uncounted.Snapshot().Consumed(EUeAgentResourceDimension::InputBytes), int64(0));
	FUeAgentCanonicalValuePtr One, Tuple, Ordered, Shared;
	if (!TestTrue(TEXT("small exact numeric input"), FUeAgentCanonicalValue::TryInteger(TEXT("1"), Uncounted, One, SemanticError))
		|| !TestTrue(TEXT("tuple input"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("z"), One.ToSharedRef()}, {TEXT("x"), One.ToSharedRef()}, {TEXT("y"), One.ToSharedRef()}}, Uncounted, Tuple, SemanticError))
		|| !TestTrue(TEXT("preferred-order input"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("c"), One.ToSharedRef()}, {TEXT("a"), One.ToSharedRef()}, {TEXT("b"), One.ToSharedRef()}}, Uncounted, Ordered, SemanticError))) return false;
	FUeAgentStructuredHjsonWriteOptions OrderingOptions;
	OrderingOptions.PreferredKeyOrderByJsonPointer.Add(TEXT("/"), TArray<FString>{TEXT("z"), TEXT("x"), TEXT("z")});
	FString OrderedText;
	TestTrue(TEXT("same tuple formatter"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*Tuple, OrderingOptions, OrderedText, WriteError));
	TestEqual(TEXT("tuple order precedes preferred order"), OrderedText, FString(TEXT("{ x: 1, y: 1, z: 1 }")));
	OrderingOptions.PreferredKeyOrderByJsonPointer[TEXT("/")] = {TEXT("c"), TEXT("a"), TEXT("c"), TEXT("missing")};
	TestTrue(TEXT("same preferred formatter"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*Ordered, OrderingOptions, OrderedText, WriteError));
	TestEqual(TEXT("preferred duplicate/negative selection preserves remaining sort"), OrderedText, FString(TEXT("{ c: 1, a: 1, b: 1 }")));
	const FString BorrowedA = TEXT("a"), BorrowedB = TEXT("b"), BorrowedC = TEXT("c");
	const FUeAgentStructuredHjsonObjectFieldView OrderedBorrowedFields[]{{BorrowedC, ActualValues.FindChecked(TEXT("large")).Get()}, {BorrowedA, *DecimalValue}, {BorrowedB, *One}};
	const FUeAgentStructuredHjsonObjectView OrderedBorrowed{MakeArrayView(OrderedBorrowedFields)};
	TestTrue(TEXT("borrowed field handles survive preferred order"), FUeAgentStructuredHjsonFormatter::TryWriteValue(OrderedBorrowed, OrderingOptions, OrderedText, WriteError));
	TestEqual(TEXT("borrowed preferred values remain paired"), OrderedText, FString(TEXT("{ c: 9007199254740993, a: 0.0, b: 1 }")));
	const FUeAgentStructuredHjsonObjectFieldView DuplicateBorrowedFields[]{{BorrowedA, *One}, {BorrowedA, *DecimalValue}};
	const FUeAgentStructuredHjsonObjectView DuplicateBorrowed{MakeArrayView(DuplicateBorrowedFields)};
	FUeAgentStructuredHjsonWriteOptions DuplicateOptions;
	TestTrue(TEXT("non-JSON borrowed duplicates retain their old first member"), FUeAgentStructuredHjsonFormatter::TryWriteValue(DuplicateBorrowed, DuplicateOptions, OrderedText, WriteError));
	TestEqual(TEXT("duplicate value selection is unchanged"), OrderedText, FString(TEXT("{ a: 1, a: 1 }")));
	DuplicateOptions.bRequireJsonCompatibleObjectKeys = true;
	TestFalse(TEXT("JSON-key mode still rejects borrowed duplicates"), FUeAgentStructuredHjsonFormatter::TryWriteValue(DuplicateBorrowed, DuplicateOptions, OrderedText, WriteError));
	TestEqual(TEXT("duplicate JSON diagnostic remains"), WriteError.Code, FString(TEXT("canonical_json_object_key_collision")));
	const FString BorrowedUpperA = TEXT("A");
	const FUeAgentStructuredHjsonObjectFieldView CaseCollisionFields[]{{BorrowedA, *One}, {BorrowedUpperA, *DecimalValue}};
	const FUeAgentStructuredHjsonObjectView CaseCollision{MakeArrayView(CaseCollisionFields)};
	TestFalse(TEXT("borrowed JSON set retains original FString case collision"), FUeAgentStructuredHjsonFormatter::TryWriteValue(CaseCollision, DuplicateOptions, OrderedText, WriteError));
	TestEqual(TEXT("case collision code unchanged"), WriteError.Code, FString(TEXT("canonical_json_object_key_collision")));
	if (!TestTrue(TEXT("shared object under different source pointers"), FUeAgentCanonicalValue::TryObject(
		{{TEXT("left"), Ordered.ToSharedRef()}, {TEXT("right"), Ordered.ToSharedRef()}}, Uncounted, Shared, SemanticError))) return false;
	OrderingOptions.PreferredKeyOrderByJsonPointer.Reset();
	OrderingOptions.PreferredKeyOrderByJsonPointer.Add(TEXT("/left"), TArray<FString>{TEXT("c"), TEXT("a")});
	OrderingOptions.PreferredKeyOrderByJsonPointer.Add(TEXT("/right"), TArray<FString>{TEXT("b"), TEXT("c")});
	TestTrue(TEXT("shared analysis keeps per-path render order"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*Shared, OrderingOptions, OrderedText, WriteError));
	TestTrue(TEXT("left order"), OrderedText.Contains(TEXT("left: { c: 1, a: 1, b: 1 }")));
	TestTrue(TEXT("right order"), OrderedText.Contains(TEXT("right: { b: 1, c: 1, a: 1 }")));
	// One wider input exercises normal growth beyond the writer's inline storage.
	TArray<FString> WideNames;
	TArray<FUeAgentCanonicalValueRef> WideValues;
	for (int32 Index = 0; Index < 18; ++Index)
	{
		FUeAgentCanonicalValuePtr Leaf, Child;
		if (!TestTrue(TEXT("wide field numeric value"), FUeAgentCanonicalValue::TryInteger(FString::FromInt(Index), Uncounted, Leaf, SemanticError))
			|| !TestTrue(TEXT("wide distinct child object"), FUeAgentCanonicalValue::TryObject({{TEXT("value"), Leaf.ToSharedRef()}}, Uncounted, Child, SemanticError))) return false;
		WideNames.Add(TEXT("field_") + FString::FromInt(Index));
		WideValues.Add(Child.ToSharedRef());
	}
	TArray<FUeAgentStructuredHjsonObjectFieldView> WideFields;
	for (int32 Index = 0; Index < WideNames.Num(); ++Index) WideFields.Emplace(WideNames[Index], WideValues[Index].Get());
	const FUeAgentStructuredHjsonObjectView WideObject{MakeArrayView(WideFields)};
	FHjsonDocumentPtr WideParsed;
	if (!TestTrue(TEXT("writer grows beyond inline slots"), FUeAgentStructuredHjsonFormatter::TryWriteValue(WideObject, WriteOptions, OrderedText, WriteError))
		|| !TestTrue(TEXT("original parser reads wide output"), TryParseHjsonDocument(OrderedText, TEXT("wide-writer.hjson"), Uncounted, WideParsed, Error))) return false;
	TestEqual(TEXT("all wide fields emitted"), WideParsed->GetRoot()->GetEntries().Num(), 18);
	for (int32 Index = 0; Index < WideNames.Num(); ++Index)
	{
		const auto* Actual = WideParsed->Find(TEXT("/") + WideNames[Index] + TEXT("/value"));
		if (!TestNotNull(TEXT("wide field remains correctly paired"), Actual)) return false;
		TestEqual(TEXT("wide numeric content"), Actual->GetLexeme(), FString::FromInt(Index));
	}
	const TMap<FString, FUeAgentCanonicalValueRef> ValidNames{{TEXT(" field "), One.ToSharedRef()}};
	const TMap<FString, FUeAgentCanonicalValueRef> InvalidNames{{TEXT("  "), One.ToSharedRef()}, {TEXT("\t"), One.ToSharedRef()}};
	const FSemanticUnit ValidUnit(FString(TEXT("unit:test")), ESemanticUnitKind::PropertySet,
		TEXT(""), TEXT("bool"), TEXT("test"), TEXT(""), ValidNames, {}, {}, TEXT(""), TEXT(""), {}, {}, TEXT("test"));
	TestTrue(TEXT("valid value name spelling retained"), ValidUnit.IsValid());
	TestTrue(TEXT("validation does not normalize the output key"), ValidUnit.GetValues().Contains(TEXT(" field ")));
	const FSemanticUnit InvalidUnit(FString(TEXT("unit:test")), ESemanticUnitKind::PropertySet,
		TEXT(""), TEXT("bool"), TEXT("test"), TEXT(""), InvalidNames, {}, {}, TEXT(""), TEXT(""), {}, {}, TEXT("test"));
	if (!TestFalse(TEXT("empty trimmed value name still rejected"), InvalidUnit.IsValid())) return false;
	TestEqual(TEXT("same first invalid name as original sorted keys"), InvalidUnit.GetValidationDiagnostic().ActualValue->AsString(), FString(TEXT("\t")));
	const auto InvalidReplacement = ValidUnit.WithValues(InvalidNames);
	if (!TestFalse(TEXT("WithValues uses the same original validation"), InvalidReplacement.IsValid())) return false;
	TestEqual(TEXT("WithValues reports the same bad spelling"), InvalidReplacement.GetValidationDiagnostic().ActualValue->AsString(), FString(TEXT("\t")));
	const auto MutableCycle = MakeShared<FJsonObject>();
	MutableCycle->SetObjectField(TEXT("self"), MutableCycle);
	const bool bCycleWritten = FUeAgentStructuredHjsonFormatter::TryWriteValue(
		MakeShared<FJsonValueObject>(MutableCycle), OrderedText, WriteError);
	MutableCycle->RemoveField(TEXT("self")); // The fixture must not retain its own cycle.
	TestFalse(TEXT("mutable JSON cycle remains rejected"), bCycleWritten);
	TestEqual(TEXT("same JSON cycle diagnostic"), WriteError.Code, FString(TEXT("hjson_value_cycle")));
	FUeAgentCanonicalValuePtr BoundaryText, BoundaryObject, LongObject, ParentObject;
	FUeAgentStructuredHjsonWriteOptions BoundaryOptions;
	const FString FitsText = FString::ChrN(111, TEXT('a'));
	if (!TestTrue(TEXT("inline boundary value"), FUeAgentCanonicalValue::TryString(FitsText, Uncounted, BoundaryText, SemanticError))
		|| !TestTrue(TEXT("inline boundary object"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("a"), BoundaryText.ToSharedRef()}}, Uncounted, BoundaryObject, SemanticError))) return false;
	TestTrue(TEXT("inline boundary writes"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*BoundaryObject, BoundaryOptions, OrderedText, WriteError));
	TestEqual(TEXT("120 characters still inline"), OrderedText, TEXT("{ a: \"") + FitsText + TEXT("\" }"));
	const FString ExceedsText = FitsText + TEXT("a");
	if (!TestTrue(TEXT("noninline boundary value"), FUeAgentCanonicalValue::TryString(ExceedsText, Uncounted, BoundaryText, SemanticError))
		|| !TestTrue(TEXT("noninline boundary object"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("a"), BoundaryText.ToSharedRef()}}, Uncounted, LongObject, SemanticError))
		|| !TestTrue(TEXT("parent of proven noninline child"), FUeAgentCanonicalValue::TryObject(
			{{TEXT("x"), LongObject.ToSharedRef()}}, Uncounted, ParentObject, SemanticError))) return false;
	TestTrue(TEXT("long object writes"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*LongObject, BoundaryOptions, OrderedText, WriteError));
	TestEqual(TEXT("121-character layout keeps original bare multiline output"), OrderedText, TEXT("{\n  a: ") + ExceedsText + TEXT("\n}"));
	TestTrue(TEXT("negative inline result propagates to parent"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*ParentObject, BoundaryOptions, OrderedText, WriteError));
	TestEqual(TEXT("parent preserves exact multiline output"), OrderedText, TEXT("{\n  x: {\n    a: ") + ExceedsText + TEXT("\n  }\n}"));
	TArray<FUeAgentCanonicalValueRef> BoundaryItems;
	FUeAgentCanonicalValuePtr BoundaryArray;
	FString InlineArray(TEXT("[")), MultilineArray(TEXT("["));
	for (int32 Index = 0; Index < 40; ++Index) {
		BoundaryItems.Add(One.ToSharedRef());
		if (Index) InlineArray += TEXT(", ");
		InlineArray += TEXT("1"); MultilineArray += TEXT("\n  1");
		}
	InlineArray += TEXT("]");
	if (!TestTrue(TEXT("120-character array input"), FUeAgentCanonicalValue::TryArray(BoundaryItems, Uncounted, BoundaryArray, SemanticError))) return false;
	TestTrue(TEXT("40-item array writes"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*BoundaryArray, BoundaryOptions, OrderedText, WriteError));
	TestEqual(TEXT("derived lower bound keeps exact inline boundary"), OrderedText, InlineArray);
	BoundaryItems.Add(One.ToSharedRef()); MultilineArray += TEXT("\n  1\n]");
	if (!TestTrue(TEXT("123-character array input"), FUeAgentCanonicalValue::TryArray(BoundaryItems, Uncounted, BoundaryArray, SemanticError))) return false;
	TestTrue(TEXT("41-item array writes"), FUeAgentStructuredHjsonFormatter::TryWriteValue(*BoundaryArray, BoundaryOptions, OrderedText, WriteError));
	TestEqual(TEXT("derived bound preserves exact multiline output"), OrderedText, MultilineArray);
	FString OldText = Original;
	const TArray<FString> Versions{
		Original.Replace(TEXT("100"), TEXT("200")),
		Original.Replace(TEXT("100"), TEXT("200")).Replace(TEXT("B: {"), TEXT("X: { value: 4 }\r\nB: {")),
		Original.Replace(TEXT("100"), TEXT("200")).Replace(TEXT("[1, 2]"), TEXT("[0, 1, 2]")),
		TEXT("{\nA: { value: 200 }\nB: { text: '''\n  中文\n  跨行\n  '''\n}\nC: { value: 3 }\n}\n"),
		TEXT("{\nA: { value: 200 }\nB: { text: '''\n  中文\n  跨行\n '''\n}\nC: { value: 3 }\n}\n"),
		TEXT("{\n# first comment\nA: { value: 200 }\nC: { value: 3 }\n}\n"),
		TEXT("A: { value: 200 }\nC: { value: 3 }\n")
	};
	TArray<FHjsonDocumentRef> Oracles;
	for (int32 Version = 0; Version < Versions.Num(); ++Version) {
		FHjsonDocumentPtr Full;
		if (!TestTrue(FString::Printf(TEXT("full oracle parses version %d"), Version), TryParseHjsonDocument(Versions[Version], Source, Resources, Full, Error))) {
			AddError(Error.GetCode()); return false;
		}
		Oracles.Add(Full.ToSharedRef());
	}
	for (int32 Version = 0; Version < Versions.Num(); ++Version) {
		FHjsonDocumentPtr Updated; const auto& Full = Oracles[Version];
		FHjsonDocumentUpdateMetrics Metrics;
		if (Version == 3 || Version == 6) {
			// Whole-source newline replacement crosses the old root's span;
			// removing root delimiters changes the chosen input contract. Neither
			// is permission to silently select a complete parser after Update.
			TestFalse(TEXT("root-boundary changes stop the selected update"),
				TryUpdateHjsonDocument(OldText, Versions[Version], Previous.ToSharedRef(), Resources, Updated, Error, &Metrics));
			TestEqual(TEXT("strict boundary diagnostic"), Error.GetCode(), FString(TEXT("workspace_incremental_syntax_unavailable")));
			// The test explicitly chooses the independent full input for the next
			// case. This does not add a production retry or downgrade route.
			Previous = Full; OldText = Versions[Version];
			continue;
		}
		if (!TestTrue(FString::Printf(TEXT("updated source parses version %d"), Version), TryUpdateHjsonDocument(OldText, Versions[Version], Previous.ToSharedRef(), Resources, Updated, Error, &Metrics))) {
			AddError(Error.GetCode()); return false;
		}
		TArray<TPair<const FHjsonNode*, const FHjsonNode*>> Pending{{&Updated->GetRoot().Get(), &Full->GetRoot().Get()}};
		while (!Pending.IsEmpty()) {
			const auto Pair = Pending.Pop(EAllowShrinking::No); const auto& A = *Pair.Key; const auto& B = *Pair.Value;
			TestTrue(TEXT("node kind and value equal full parser"), A.GetKind() == B.GetKind() && A.GetLexeme() == B.GetLexeme()
				&& A.GetText() == B.GetText() && A.GetBoolean() == B.GetBoolean());
			const auto& AP = A.GetProvenance(); const auto& BP = B.GetProvenance();
			TestTrue(TEXT("exact current source coordinates equal full parser"), AP.GetJsonPointer() == BP.GetJsonPointer()
				&& AP.GetOriginalKey() == BP.GetOriginalKey() && AP.GetLine() == BP.GetLine() && AP.GetColumn() == BP.GetColumn()
				&& AP.GetByteStart() == BP.GetByteStart() && AP.GetByteEnd() == BP.GetByteEnd());
			if (!TestEqual(TEXT("array count"), A.GetItems().Num(), B.GetItems().Num())
				|| !TestEqual(TEXT("object count"), A.GetEntries().Num(), B.GetEntries().Num())) return false;
			for (int32 I = 0; I < A.GetItems().Num(); ++I) Pending.Emplace(&A.GetItems()[I].Get(), &B.GetItems()[I].Get());
			for (int32 I = 0; I < A.GetEntries().Num(); ++I) {
				TestEqual(TEXT("object key"), A.GetEntries()[I].Key, B.GetEntries()[I].Key);
				Pending.Emplace(&A.GetEntries()[I].Value.Get(), &B.GetEntries()[I].Value.Get());
			}
		}
		if (Version == 0) {
			TestTrue(TEXT("value edit parses a strict subset"), !Metrics.bWholeDocument && Metrics.ReparsedBytes < FTCHARToUTF8(*Versions[Version]).Length());
			TestTrue(TEXT("unaffected B storage is shared"), Updated->Find(TEXT("/B"))->GetValueStorageIdentity() == Previous->Find(TEXT("/B"))->GetValueStorageIdentity());
		}
		Previous = Updated; OldText = Versions[Version];
	}
{
	const FString OldRows = TEXT("{\n untouched: 4\n rows: [\n  { value: 1 }\n ]\n}\n");
	const FString NewRows = OldRows.Replace(TEXT("{ value: 1 }"), TEXT("{ value: \"} {\" }\n  { value: 7 }"));
	FHjsonDocumentPtr OldDoc, NewDoc, FullDoc;
	FHjsonDocumentUpdateMetrics RowMetrics;
	if (!TestTrue(TEXT("original row and full sibling input parse"), TryParseHjsonDocument(OldRows, Source, Resources, OldDoc, Error)
		&& TryParseHjsonDocument(NewRows, Source, Resources, FullDoc, Error))) return false;
	if (!TestTrue(TEXT("changed first row and appended sibling select their containing array"),
		TryUpdateHjsonDocument(OldRows, NewRows, OldDoc.ToSharedRef(), Resources, NewDoc, Error, &RowMetrics))) return false;
	TestFalse(TEXT("sibling insertion stays in its nested array"), RowMetrics.bWholeDocument);
	TestEqual(TEXT("one array was parsed"), RowMetrics.ReparsedContainers, int64(1));
	for (const TCHAR* Path : {TEXT("/rows/0/value"), TEXT("/rows/1/value")}) {
		const auto* Observed = NewDoc->Find(Path);
		const auto* FromFull = FullDoc->Find(Path);
		if (!TestNotNull(TEXT("independently bound sibling"), Observed) || !TestNotNull(TEXT("full sibling"), FromFull)) return false;
		TestEqual(TEXT("actual value lexeme"), Observed->GetLexeme(), FromFull->GetLexeme());
		TestEqual(TEXT("actual byte position"), Observed->GetProvenance().GetByteStart(), FromFull->GetProvenance().GetByteStart());
	}
}
	// Exterior comments are scanned as trivia, not a reason to parse the root.
	FHjsonDocumentPtr TriviaPrevious;
	if (!TestTrue(TEXT("exterior fixture parses"), TryParseHjsonDocument(Original, Source, Resources, TriviaPrevious, Error))) return false;
	for (const FString& TriviaText : {TEXT("// leading comment\r\n\n") + Original.Replace(TEXT("100"), TEXT("200")),
		TEXT("# leading only\n") + Original + TEXT("// trailing comment\n")}) {
		FHjsonDocumentPtr TriviaUpdated, TriviaFull; FHjsonDocumentUpdateMetrics TriviaMetrics;
		if (!TestTrue(TEXT("exterior update uses original scanner and fragments"), TryUpdateHjsonDocument(Original,
			TriviaText, TriviaPrevious.ToSharedRef(), Resources, TriviaUpdated, Error, &TriviaMetrics))
			|| !TestTrue(TEXT("independent whole input oracle"), TryParseHjsonDocument(TriviaText, Source, Resources, TriviaFull, Error))) return false;
		TestFalse(TEXT("exterior trivia does not select whole root parsing"), TriviaMetrics.bWholeDocument);
		TArray<TPair<const FHjsonNode*, const FHjsonNode*>> Pending{{&TriviaUpdated->GetRoot().Get(), &TriviaFull->GetRoot().Get()}};
		while (!Pending.IsEmpty()) {
			const auto Pair = Pending.Pop(EAllowShrinking::No); const auto& A = *Pair.Key; const auto& B = *Pair.Value;
			const auto& AP = A.GetProvenance(); const auto& BP = B.GetProvenance();
			TestTrue(TEXT("trivia relocation retains actual values and positions"), A.GetKind() == B.GetKind()
				&& A.GetLexeme() == B.GetLexeme() && A.GetText() == B.GetText() && A.GetBoolean() == B.GetBoolean()
				&& AP.GetLine() == BP.GetLine() && AP.GetColumn() == BP.GetColumn()
				&& AP.GetByteStart() == BP.GetByteStart() && AP.GetByteEnd() == BP.GetByteEnd());
			if (!TestEqual(TEXT("trivia array membership"), A.GetItems().Num(), B.GetItems().Num())
				|| !TestEqual(TEXT("trivia object membership"), A.GetEntries().Num(), B.GetEntries().Num())) return false;
			for (int32 I = 0; I < A.GetItems().Num(); ++I) Pending.Emplace(&A.GetItems()[I].Get(), &B.GetItems()[I].Get());
			for (int32 I = 0; I < A.GetEntries().Num(); ++I) {
				TestEqual(TEXT("trivia keeps keys"), A.GetEntries()[I].Key, B.GetEntries()[I].Key);
				Pending.Emplace(&A.GetEntries()[I].Value.Get(), &B.GetEntries()[I].Value.Get());
			}
		}
	}
	FHjsonDocumentPtr BadExterior;
	TestFalse(TEXT("line comment cannot swallow root"), TryUpdateHjsonDocument(Original, TEXT("// no newline") + Original,
		TriviaPrevious.ToSharedRef(), Resources, BadExterior, Error));
	TestFalse(TEXT("exterior token is not trivia"), TryUpdateHjsonDocument(Original, TEXT("x: 1\n") + Original,
		TriviaPrevious.ToSharedRef(), Resources, BadExterior, Error));
	// The fragment-diagnostic case has an explicit, unchanged root boundary.
	Previous = Oracles[5]; OldText = Versions[5];
	const FString Broken = OldText.Replace(TEXT("value: 200"), TEXT("value: { broken"));
	FHjsonDocumentPtr Updated, Full; FWorkspaceError UpdateError, FullError;
	TestFalse(TEXT("invalid boundary is not accepted by fragment parsing"), TryUpdateHjsonDocument(OldText, Broken, Previous.ToSharedRef(), Resources, Updated, UpdateError));
	TestFalse(TEXT("same invalid input fails original parser"), TryParseHjsonDocument(Broken, Source, Resources, Full, FullError));
	TestEqual(TEXT("original syntax diagnostic survives boundary loss"), UpdateError.GetCode(), FullError.GetCode());
	return true;
}

#endif
