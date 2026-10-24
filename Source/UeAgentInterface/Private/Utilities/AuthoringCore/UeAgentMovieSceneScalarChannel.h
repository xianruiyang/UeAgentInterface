#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Algo/BinarySearch.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneStringChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Sections/MovieSceneDoubleSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"
#include <limits>
#include <type_traits>

namespace UE::UAI::Authoring::Timeline::ScalarChannel
{
	inline FString Kind(const UMovieSceneTrack* Track)
	{
		return Track ? Track->GetClass()->GetName() : FString();
	}

	// Visit the channels owned by the scalar adapter. Other native channels remain
	// in the section; optionally expose their identity as read-only context.
	template <typename Section, typename Visitor>
	bool VisitSupported(Section& Value, Visitor&& Consume, TArray<TSharedPtr<FJsonValue>>* NativeContext = nullptr,
		UE::UAI::Contract::FDiagnostic* OutError = nullptr)
	{
		auto* MutableSection = const_cast<UMovieSceneSection*>(static_cast<const UMovieSceneSection*>(&Value));
		for (const auto& Entry : Value.GetChannelProxy().GetAllEntries())
		{
			const FName Type = Entry.GetChannelTypeName();
			const bool bDouble = Type == FMovieSceneDoubleChannel::StaticStruct()->GetFName();
			const bool bBool = Type == FMovieSceneBoolChannel::StaticStruct()->GetFName();
			const bool bInteger = Type == FMovieSceneIntegerChannel::StaticStruct()->GetFName();
			const bool bByte = Type == FMovieSceneByteChannel::StaticStruct()->GetFName();
			const bool bString = Type == FMovieSceneStringChannel::StaticStruct()->GetFName();
			const bool bObject = Type == FMovieSceneObjectPathChannel::StaticStruct()->GetFName();
			const bool bScalar = bDouble || bBool || bInteger || bByte || bString || bObject || Type == FMovieSceneFloatChannel::StaticStruct()->GetFName();
			const auto Channels = Entry.GetChannels();
			const auto Metadata = Entry.GetMetaData();
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				const FString Ref = Type.ToString() + TEXT(":") + FString::FromInt(Index);
				const FString Name = Index >= Metadata.Num() || Metadata[Index].Name.IsNone() ? TEXT("Value") : Metadata[Index].Name.ToString();
				if (!Channels[Index]) {
					if (OutError) {
						*OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_native_channel_missing"),
							TEXT("The section channel proxy contains a null channel."), TEXT("native_execution"), Value.GetPathName());
						OutError->CandidateName = Name;
						OutError->PropertyPath = FString::Printf(TEXT("channels[%s][%d]"), *Type.ToString(), Index);
						OutError->ExpectedValuePresent = true; OutError->ActualValuePresent = false;
						OutError->SourceKind = TEXT("native_result");
					}
					return false;
				}
				const auto* ObjectTrack = bObject ? MutableSection->GetTypedOuter<UMovieSceneObjectPropertyTrack>() : nullptr;
				const bool bObjectContext = !bObject || (ObjectTrack && ObjectTrack->PropertyClass &&
					ObjectTrack->PropertyClass == static_cast<FMovieSceneObjectPathChannel*>(Channels[Index])->GetPropertyClass());
				if (!bScalar || !bObjectContext)
				{
					if (NativeContext)
					{
						auto Context = MakeShared<FJsonObject>();
						Context->SetStringField(TEXT("channel_ref"), Ref);
						Context->SetStringField(TEXT("name"), Name);
						Context->SetStringField(TEXT("native_type"), Type.ToString());
						Context->SetNumberField(TEXT("key_count"), Channels[Index]->GetNumKeys());
						NativeContext->Add(MakeShared<FJsonValueObject>(Context));
					}
					continue;
				}
				if (bObject)
				{
					auto* Channel = static_cast<FMovieSceneObjectPathChannel*>(Channels[Index]);
					if (!Consume(*Channel, Ref, Name, FString(TEXT("object_path")))) return false;
				}
				else if (bString)
				{
					if (!Consume(*static_cast<FMovieSceneStringChannel*>(Channels[Index]), Ref, Name, FString(TEXT("string")))) return false;
				}
				else if (bByte)
				{
					if (!Consume(*static_cast<FMovieSceneByteChannel*>(Channels[Index]), Ref, Name, FString(TEXT("byte")))) return false;
				}
				else if (bInteger)
				{
					if (!Consume(*static_cast<FMovieSceneIntegerChannel*>(Channels[Index]), Ref, Name, FString(TEXT("integer")))) return false;
				}
				else if (bBool)
				{
					if (!Consume(*static_cast<FMovieSceneBoolChannel*>(Channels[Index]), Ref, Name, FString(TEXT("bool")))) return false;
				}
				else if (bDouble)
				{
					if (!Consume(*static_cast<FMovieSceneDoubleChannel*>(Channels[Index]), Ref, Name, FString(TEXT("double")))) return false;
				}
				else if (!Consume(*static_cast<FMovieSceneFloatChannel*>(Channels[Index]), Ref, Name, FString(TEXT("float")))) return false;
			}
		}
		return true;
	}

	template <typename Number>
	FString Lexeme(Number Value)
	{
		if constexpr (std::is_same_v<Number, FString>) return Value;
		else if constexpr (std::is_same_v<Number, bool>) return Value ? TEXT("true") : TEXT("false");
		else if constexpr (std::is_integral_v<Number>) return LexToString(Value);
		else return FString::Printf(TEXT("%.*g"), std::numeric_limits<Number>::max_digits10, static_cast<double>(Value));
	}

	template <typename Number>
	bool Parse(const FString& Text, Number& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
[[maybe_unused]] const auto Invalid = [&](const FString& Expected) {
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_scalar_value_invalid"),
				FString(), MakeShared<FJsonValueString>(Expected), MakeShared<FJsonValueString>(Text));
			return false;
		};
		if constexpr (std::is_same_v<Number, FString>) { Out = Text; return true; }
		else if constexpr (std::is_same_v<Number, bool>)
		{
			if (Text != TEXT("true") && Text != TEXT("false")) return Invalid(TEXT("true or false"));
			Out = Text == TEXT("true"); return true;
		}
		else if constexpr (std::is_integral_v<Number>)
		{
			if constexpr (std::is_signed_v<Number>) {
				int64 Value = 0;
				const FString Expected = FString::Printf(TEXT("decimal integer from %s through %s"),
					*LexToString(std::numeric_limits<Number>::min()), *LexToString(std::numeric_limits<Number>::max()));
				if (!StateSemantic::TryParseInt64Decimal(Text, Value) ||
					Value < std::numeric_limits<Number>::min() || Value > std::numeric_limits<Number>::max()) return Invalid(Expected);
				Out = static_cast<Number>(Value);
			} else {
				uint64 Value = 0;
				if (!StateSemantic::TryParseUInt64Decimal(Text, Value) || Value > std::numeric_limits<Number>::max())
					return Invalid(TEXT("decimal integer from 0 through ") + LexToString(std::numeric_limits<Number>::max()));
				Out = static_cast<Number>(Value);
			}
			return true;
		}
		else
		{
			double Value = 0;
			if (!StateSemantic::TryParseFiniteDecimal(Text, Value)) return Invalid(TEXT("finite decimal number"));
			if constexpr (std::is_same_v<Number, float>)
				return UeAgentPropertyValueCodec::TryConvertFloat32(Value, Out, OutError);
			else { Out = static_cast<Number>(Value); return true; }
		}
	}

	template <typename Value>
	UeAgentPropertyValueCodec::FOptions KeyPropertyOptions()
	{
		UeAgentPropertyValueCodec::FOptions Options;
		Options.bAllowPartialStructFields = true;
		Options.bCaptureStructFieldDescriptors = true;
		Options.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
			return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field, Struct)
				&& !(Struct == Value::StaticStruct() && Field->GetFName() == TEXT("Value"));
		};
		return Options;
	}

	template <typename Value>
	inline constexpr bool IsPlainValue = std::is_arithmetic_v<Value> || std::is_same_v<Value, FString> || std::is_same_v<Value, FMovieSceneObjectPathChannelKeyValue>;

	template <typename Value>
	TSharedPtr<FJsonValue> CaptureProperties(const Value& Native, UObject* Owner, TSharedPtr<FJsonObject>* Definition = nullptr)
	{
		if constexpr (IsPlainValue<Value>) return nullptr;
		else return UeAgentPropertyValueCodec::EncodeStructValue(Value::StaticStruct(), &Native, Owner,
			KeyPropertyOptions<Value>(), 0, Definition);
	}

	template <typename Value>
	bool DecodeProperties(const FTimelineAnimationKey& Key, Value& Desired, UObject* Owner, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if constexpr (IsPlainValue<Value>) {
			if (!Key.Properties.IsValid()) return true;
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_key_properties_invalid"),
				FString(), MakeShared<FJsonValueString>(TEXT("no properties for a plain scalar key")), Key.Properties);
			return false;
		} else return !Key.Properties.IsValid() || UeAgentPropertyValueCodec::DecodeStructValue(Value::StaticStruct(),
			Key.Properties, &Desired, Owner, KeyPropertyOptions<Value>(), OutError);
	}

	template <typename Value>
	struct TValueAccess
	{
		static auto Get(const Value& Native) { if constexpr (IsPlainValue<Value>) return Native; else return Native.Value; }
		template <typename Scalar> static void Set(Value& Native, Scalar Desired) { if constexpr (IsPlainValue<Value>) Native = Desired; else Native.Value = Desired; }
		static const TCHAR* Type() { return std::is_same_v<Value, FString> ? TEXT("string") : std::is_same_v<Value, bool> ? TEXT("boolean") : std::is_integral_v<Value> ? TEXT("integer") : TEXT("decimal"); }
		static bool IsValid(const Value& Native) { if constexpr (IsPlainValue<Value>) return true; else return FMath::IsFinite(Native.Value); }
	};
	template <typename Channel>
	using TKeyValue = std::decay_t<decltype(std::declval<Channel&>().GetData().GetValues()[0])>;

	template <typename Channel>
	const UEnum* ValueEnum(const Channel& Native)
	{
		if constexpr (std::is_same_v<Channel, FMovieSceneByteChannel>) return Native.GetEnum();
		else return nullptr;
	}
	template <typename Channel>
	const TCHAR* ValueType(const Channel& Native, const UMovieSceneSection* Section = nullptr) {
		if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>)
		{
			const auto* Track = Section ? Section->GetTypedOuter<UMovieSceneObjectPropertyTrack>() : nullptr;
			return Track && Track->bClassProperty ? TEXT("class_ref") : TEXT("object_ref");
		}
		else return ValueEnum(Native) ? TEXT("enum") : TValueAccess<TKeyValue<Channel>>::Type();
	}
	template <typename Channel>
	const TCHAR* ValueField() { return std::is_same_v<Channel, FMovieSceneObjectPathChannel> ? TEXT("value") : TEXT("value_lexeme"); }
	template <typename Channel>
	FString ValueLexeme(const Channel& Native, typename Channel::CurveValueType Value)
	{
		if constexpr (std::is_integral_v<typename Channel::CurveValueType>)
			if (const UEnum* Enum = ValueEnum(Native)) return UeAgentPropertyValueCodec::EncodeEnumValueName(Enum, static_cast<int64>(Value));
		return Lexeme(Value);
	}
	template <typename Channel>
	bool ParseValue(const Channel& Native, const FString& Text, typename Channel::CurveValueType& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if constexpr (std::is_integral_v<typename Channel::CurveValueType>) if (const UEnum* Enum = ValueEnum(Native))
		{
			int64 Raw = 0;
			if (!UeAgentPropertyValueCodec::DecodeEnumValue(Enum, MakeShared<FJsonValueString>(Text), Raw, OutError)) return false;
			if (Raw < 0 || Raw > MAX_uint8) {
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_scalar_enum_value_out_of_range"),
					FString(), MakeShared<FJsonValueString>(TEXT("enum value from 0 through 255")), MakeShared<FJsonValueString>(LexToString(Raw)));
				OutError.TargetPath = Enum->GetPathName();
				return false;
			}
			Out = static_cast<typename Channel::CurveValueType>(Raw); return true;
		}
		return Parse(Text, Out, OutError);
	}
	template <typename Channel>
	TSharedPtr<FJsonObject> CaptureValueDefinition(const Channel& Native, const UMovieSceneSection* Section = nullptr)
	{
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>)
		{
			Type.Kind = ValueType(Native, Section);
			Type.AllowedClassPath = Native.GetPropertyClass()->GetPathName();
		}
		else if constexpr (std::is_same_v<typename Channel::CurveValueType, FString>) Type.Kind = TEXT("string");
		else Type = UeAgentAuthoringTypeDescriptors::DescribeScalar<typename Channel::CurveValueType>();
		const UEnum* Enum = ValueEnum(Native);
		if (Enum) { Type.Kind = TEXT("enum"); Type.EnumPath = Enum->GetPathName(); }
		const auto Definition = MakeShared<FJsonObject>(); Definition->SetObjectField(TEXT("type"), Type.ToJsonObject());
		if (Enum) if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Enum))
			for (const auto& Field : Candidates->Values) Definition->SetField(Field.Key, Field.Value);
		if constexpr (std::is_arithmetic_v<typename Channel::CurveValueType>)
			if (!Enum) Definition->SetStringField(TEXT("value_encoding"), TEXT("lexeme"));
		return Definition;
	}
	template <typename Channel>
	TSharedPtr<FJsonValue> CaptureValue(const Channel& ChannelContext, const TKeyValue<Channel>& Value)
	{
		if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>)
			return UeAgentPropertyValueCodec::EncodeReferencePath(Value.GetSoftPtr().ToSoftObjectPath().ToString());
		else
		{
			const auto Result = MakeShared<FJsonValueString>(ValueLexeme(ChannelContext, TValueAccess<TKeyValue<Channel>>::Get(Value)));
			const auto Definition = CaptureValueDefinition(ChannelContext);
			UeAgentAuthoringTypeDescriptors::FDescriptor Type;
			Definition->GetObjectField(TEXT("type"))->TryGetStringField(TEXT("kind"), Type.Kind);
			UeAgentAuthoringEncodedTypes::FScope::Record(Result, Type, Definition);
			return Result;
		}
	}
	template <typename Value>
	bool ValuesEqual(const Value& Left, const Value& Right) {
		if constexpr (std::is_same_v<Value, FMovieSceneObjectPathChannelKeyValue>) return Left.GetSoftPtr() == Right.GetSoftPtr();
		else return Left == Right;
	}
	template <typename Channel>
	TOptional<typename Channel::CurveValueType> GetDefault(const Channel& Native) {
		if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>)
			return Native.GetDefault().GetSoftPtr().IsNull() ? TOptional<typename Channel::CurveValueType>() : TOptional<typename Channel::CurveValueType>(Native.GetDefault());
		else return Native.GetDefault();
	}
	template <typename Value>
	bool DefaultsEqual(const TOptional<Value>& Left, const TOptional<Value>& Right) {
		return Left.IsSet() == Right.IsSet() && (!Left.IsSet() || ValuesEqual(Left.GetValue(), Right.GetValue()));
	}
	template <typename Channel>
	TSharedRef<FJsonObject> CaptureKeyDefault(const Channel& ChannelContext, const UMovieSceneSection* Section = nullptr)
	{
		using Value = TKeyValue<Channel>;
		const Value Native{};
		const auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("value_type"), ValueType(ChannelContext, Section));
		Result->SetField(ValueField<Channel>(), CaptureValue(ChannelContext, Native));
		if (const auto Properties = CaptureProperties(Native, nullptr)) Result->SetField(TEXT("properties"), Properties);
		return Result;
	}
	// Native factories that require a property/parameter context must be initialized
	// before section creation. Candidate discovery never calls a section factory.
	UEAGENTINTERFACE_API bool HasRequiredSectionContext(const UMovieSceneTrack& Track);
	// Preserve native track state and owned context; a fresh section contains no
	// captured keys, while the existing section count still informs native defaults.
	UEAGENTINTERFACE_API UMovieSceneTrack* MakeSectionTemplateTrack(const UMovieSceneTrack& Source);

	inline UeAgentPropertyValueCodec::FOptions ChannelPropertyOptions()
	{
		UeAgentPropertyValueCodec::FOptions Options;
		Options.bAllowPartialStructFields = true;
		Options.bCaptureStructFieldDescriptors = true;
		Options.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) {
			return Field->GetFName() == TEXT("PreInfinityExtrap") || Field->GetFName() == TEXT("PostInfinityExtrap")
				|| Field->GetFName() == TEXT("bInterpolateLinearKeys");
		};
		return Options;
	}

	template <typename Channel>
	TSharedPtr<FJsonValue> CaptureChannelProperties(const Channel& Native, UObject* Owner, TSharedPtr<FJsonObject>* Definition = nullptr)
	{
		bool bHasProperties = false;
		for (TFieldIterator<FProperty> It(Channel::StaticStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
			bHasProperties |= ChannelPropertyOptions().StructFieldFilter(Channel::StaticStruct(), *It);
		if (!bHasProperties) return nullptr;
		return UeAgentPropertyValueCodec::EncodeStructValue(Channel::StaticStruct(), &Native, Owner,
			ChannelPropertyOptions(), 0, Definition);
	}

	template <typename Channel>
	TSharedRef<FJsonObject> CaptureDefault(const Channel& Native)
	{
		const auto Result = MakeShared<FJsonObject>();
		const auto Default = GetDefault(Native);
		Result->SetBoolField(TEXT("is_set"), Default.IsSet());
		if (Default.IsSet()) Result->SetField(ValueField<Channel>(), CaptureValue(Native, TKeyValue<Channel>(Default.GetValue())));
		return Result;
	}

	template <typename Channel>
	bool PrepareValue(const Channel& Native, const UMovieSceneSection& Section, const FTimelineAnimationKey& Key,
		TKeyValue<Channel>& Desired, const bool bExisting, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>)
		{
if (!Key.Value.IsValid() || Key.bHasValueLexeme || !Key.ValueLexeme.IsEmpty()) {
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_object_key_value_shape_invalid"), FString(),
					MakeShared<FJsonValueString>(TEXT("reference in value without value_lexeme")), Key.Value, TEXT("input_decode"),
					!Key.Value.IsValid() ? TEXT("An object-path channel key requires value.")
					: TEXT("An object-path channel key cannot also provide value_lexeme."));
				return false;
			}
			TOptional<FString> Path; UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentPropertyValueCodec::DecodeReferencePath(Key.Value, Path, Error)) { OutError = MoveTemp(Error); return false; }
			const auto Existing = Desired.GetSoftPtr().ToSoftObjectPath();
			if (bExisting && (Path.IsSet() ? Path.GetValue() == Existing.ToString() : Existing.IsNull())) return true;
			const auto* Track = Section.GetTypedOuter<UMovieSceneObjectPropertyTrack>();
if (!Track || !Track->PropertyClass || Native.GetPropertyClass() != Track->PropertyClass) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_object_channel_property_class_mismatch"),
					!Track ? TEXT("The object-path channel has no owning object-property track.")
					: !Track->PropertyClass ? TEXT("The object-property track has no property class.")
					: TEXT("The channel and owning track declare different property classes."), TEXT("execution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("/property_class");
				OutError.TargetPath = Section.GetPathName();
if (Native.GetPropertyClass()) OutError.ActualValue = MakeShared<FJsonValueString>(Native.GetPropertyClass()->GetPathName());
				OutError.ExpectedValue = Track && Track->PropertyClass ? MakeShared<FJsonValueString>(Track->PropertyClass->GetPathName())
					: MakeShared<FJsonValueString>(TEXT("an owning object-property track with a matching property class"));
				return false;
			}
			UObject* Object = nullptr;
			if (!UeAgentPropertyValueCodec::DecodeObjectReference(Key.Value,
				Track->bClassProperty ? UClass::StaticClass() : Track->PropertyClass.Get(),
				Track->bClassProperty ? Track->PropertyClass.Get() : nullptr, Object, Error)) { OutError = MoveTemp(Error); return false; }
			Desired = FMovieSceneObjectPathChannelKeyValue(Object); return true;
		}
		else
		{
if (Key.Value.IsValid()) {
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_scalar_key_value_shape_invalid"), FString(),
					MakeShared<FJsonValueString>(TEXT("value_lexeme without value")), Key.Value, TEXT("input_decode"),
					TEXT("This scalar channel requires value_lexeme and cannot accept value."));
				return false;
			}
			auto Value = TValueAccess<TKeyValue<Channel>>::Get(Desired);
			if (!(bExisting && Key.ValueLexeme.IsEmpty() && ValueEnum(Native) && ValueLexeme(Native, Value).IsEmpty())
				&& !ParseValue(Native, Key.ValueLexeme, Value, OutError)) return false;
			TValueAccess<TKeyValue<Channel>>::Set(Desired, Value); return true;
		}
	}

	struct FChannelEdits
	{
		TArray<TFunction<void()>> Removals, Updates, Additions;
		TSet<UMovieSceneSection*> ModifiedSections;
		TArray<TPair<FString, TFunction<bool(UE::UAI::Contract::FDiagnostic&)>>> Verifications;
		UE::UAI::Contract::FDiagnostic ErrorDetail;

		bool FailValue(const FString& Code, const FString& Field, const FString& Expected,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonObject>& Source = nullptr)
		{
			auto Diagnostic = ErrorDetail;
			if (Diagnostic.IsEmpty()) Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code,
				FString::Printf(TEXT("Invalid value for '%s'; expected %s."), *Field, *Expected), TEXT("input_decode"));
			if (!Diagnostic.IsEmpty() && !ErrorDetail.IsEmpty() && Diagnostic.Source.SourceFile.IsEmpty())
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Diagnostic, Field);
			if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = Field;
			if (Diagnostic.SemanticPointer.IsEmpty()) Diagnostic.SemanticPointer = Field;
			if (!Diagnostic.ExpectedValue.IsValid()) Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = Actual;
			if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Source);
			ErrorDetail = MoveTemp(Diagnostic);
			return false;
		}

		static bool VerificationFailure(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Reason,
			const UeAgentPropertyValueCodec::FValueComparison& Difference, const TSharedPtr<FJsonObject>& Source)
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_channel_intent_failed"), Reason, TEXT("intent_verify"));
			Difference.ApplyTo(Error);
			Error.SemanticPointer = Difference.Path;
			Error.PropertyPath = Difference.Path;
			if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source);
			Error.SourceKind = TEXT("native_result");
			return false;
		}
		template<typename Channel>
		static bool VerifyKey(Channel& Native, UMovieSceneSection& Section, FKeyHandle Handle,
			const TKeyValue<Channel>& Desired, FFrameNumber Time, const TSharedPtr<FJsonObject>& Source,
			UE::UAI::Contract::FDiagnostic& Error)
		{
			using Comparison = UeAgentPropertyValueCodec::FValueComparison;
			auto Data = Native.GetData();
			const int32 Index = Data.GetIndex(Handle);
			if (Index == INDEX_NONE)
				return VerificationFailure(Error, TEXT("The requested key is missing after the channel update."),
					Comparison::Different(TEXT("/key_ref"), MakeShared<FJsonValueBoolean>(true), nullptr), Source);
			if (Data.GetTimes()[Index] != Time)
				return VerificationFailure(Error, TEXT("The native key time differs from the requested time."),
					Comparison::Different(TEXT("/time/tick"), MakeShared<FJsonValueString>(FString::FromInt(Time.Value)),
						MakeShared<FJsonValueString>(FString::FromInt(Data.GetTimes()[Index].Value))), Source);
			const auto& Actual = Data.GetValues()[Index];
			if (ValuesEqual(Actual, Desired)) return true;
			if (!ValuesEqual(TValueAccess<TKeyValue<Channel>>::Get(Actual), TValueAccess<TKeyValue<Channel>>::Get(Desired)))
				return VerificationFailure(Error, TEXT("The native key value differs from the requested value."),
					Comparison::Different(FString(TEXT("/")) + ValueField<Channel>(), CaptureValue(Native, Desired), CaptureValue(Native, Actual)), Source);
			if constexpr (!IsPlainValue<TKeyValue<Channel>>)
			{
				const auto Difference = UeAgentPropertyValueCodec::CompareReflectedStructFields(TKeyValue<Channel>::StaticStruct(),
					&Desired, &Actual, &Section, KeyPropertyOptions<TKeyValue<Channel>>());
				if (!Difference.bEquivalent)
					return VerificationFailure(Error, TEXT("The native key properties differ from the requested properties."), Difference.WithPrefix(TEXT("/properties")), Source);
			}
			// Preserve the native equality contract even if an unreflected field differs.
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_channel_intent_failed"),
				TEXT("The native key differs in state outside its exported value and property fields."), TEXT("intent_verify"));
			if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source);
			Error.SourceKind = TEXT("native_result");
			return false;
		}

		template <typename Channel>
		bool PrepareSettings(Channel& Native, UMovieSceneSection& Section, const FTimelineAnimationChannel& After)
		{
			ErrorDetail.Reset();
			using Number = typename Channel::CurveValueType;
			TOptional<Number> Default = GetDefault(Native);
			if (After.DefaultIsSet.IsSet())
			{
				if (After.DefaultIsSet.GetValue())
				{
					TKeyValue<Channel> Value{};
					if (Default.IsSet()) TValueAccess<TKeyValue<Channel>>::Set(Value, Default.GetValue());
					FTimelineAnimationKey Input; Input.Value = After.DefaultValue; Input.ValueLexeme = After.DefaultValueLexeme;
					Input.bHasValueLexeme = !After.DefaultValue.IsValid();
if (!PrepareValue(Native, Section, Input, Value, Default.IsSet(), ErrorDetail))
						return FailValue(TEXT("timeline_channel_default_invalid"), TEXT("/default"), ValueType(Native, &Section),
							After.DefaultValue.IsValid() ? After.DefaultValue : MakeShared<FJsonValueString>(After.DefaultValueLexeme), After.SourceRecord);
					if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>) if (Value.GetSoftPtr().IsNull())
						return FailValue(TEXT("timeline_channel_default_invalid"), TEXT("/default/value"), TEXT("non-null object reference when default.is_set is true"), After.DefaultValue, After.SourceRecord);
					Default = TValueAccess<TKeyValue<Channel>>::Get(Value);
				}
				else Default.Reset();
			}
			// Decode only supported settings into empty native storage; key arrays and handles are never copied.
			const auto Desired = MakeShared<Channel>();
			const auto Options = ChannelPropertyOptions();
			TArray<FProperty*> Settings;
			for (TFieldIterator<FProperty> It(Channel::StaticStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Field = *It;
				if (!Options.StructFieldFilter(Channel::StaticStruct(), Field)) continue;
				Settings.Add(Field);
				Field->CopyCompleteValue(Field->ContainerPtrToValuePtr<void>(&Desired.Get()), Field->ContainerPtrToValuePtr<void>(&Native));
			}
			UE::UAI::Contract::FDiagnostic Error;
			if (Settings.IsEmpty() && After.Properties.IsValid())
				return FailValue(TEXT("timeline_channel_properties_unsupported"), TEXT("/properties"), TEXT("no editable channel settings"), After.Properties, After.SourceRecord);
			if (After.Properties.IsValid() && !UeAgentPropertyValueCodec::DecodeStructValue(Channel::StaticStruct(),
				After.Properties, &Desired.Get(), &Section, Options, Error))
			{ ErrorDetail = Error; return FailValue(TEXT("timeline_channel_properties_invalid"), TEXT("/properties"), TEXT("editable channel settings"), After.Properties, After.SourceRecord); }
			Verifications.Emplace(Section.GetPathName() + TEXT(":") + After.ChannelRef,
				[&Native, &Section, Default, Desired, Settings, Source = After.SourceRecord](UE::UAI::Contract::FDiagnostic& Error) {
					using Comparison = UeAgentPropertyValueCodec::FValueComparison;
					const auto Actual = GetDefault(Native);
					if (Actual.IsSet() != Default.IsSet())
						return VerificationFailure(Error, TEXT("The channel default presence differs from the requested state."),
							Comparison::Different(TEXT("/default/is_set"), MakeShared<FJsonValueBoolean>(Default.IsSet()), MakeShared<FJsonValueBoolean>(Actual.IsSet())), Source);
					if (!DefaultsEqual(Actual, Default))
					{
						TKeyValue<Channel> ExpectedKey{}, ActualKey{};
						TValueAccess<TKeyValue<Channel>>::Set(ExpectedKey, Default.GetValue());
						TValueAccess<TKeyValue<Channel>>::Set(ActualKey, Actual.GetValue());
						return VerificationFailure(Error, TEXT("The channel default differs from the requested value."),
							Comparison::Different(FString(TEXT("/default/")) + ValueField<Channel>(), CaptureValue(Native, ExpectedKey), CaptureValue(Native, ActualKey)), Source);
					}
					for (auto* Field : Settings)
						if (!Field->Identical(Field->ContainerPtrToValuePtr<void>(&Desired.Get()), Field->ContainerPtrToValuePtr<void>(&Native)))
						{
							const auto Difference = UeAgentPropertyValueCodec::ComparePropertyValues(Field,
								Field->ContainerPtrToValuePtr<void>(&Desired.Get()), Field->ContainerPtrToValuePtr<void>(&Native), &Section, ChannelPropertyOptions());
							const FString Path = TEXT("/properties/fields/") + Field->GetName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
							if (!Difference.bEquivalent)
								return VerificationFailure(Error, TEXT("The native channel setting differs from the requested setting."), Difference.WithPrefix(Path), Source);
							Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_channel_intent_failed"), TEXT("The native channel setting differs in state outside its exported fields."), TEXT("intent_verify"));
							Error.PropertyPath = Path; Error.SemanticPointer = Path;
							if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source);
							Error.SourceKind = TEXT("native_result"); return false;
						}
					return true;
				});
			bool bChanged = !DefaultsEqual(GetDefault(Native), Default);
			for (const auto* Field : Settings) bChanged |= !Field->Identical(Field->ContainerPtrToValuePtr<void>(&Desired.Get()), Field->ContainerPtrToValuePtr<void>(&Native));
			if (bChanged)
			{
				ModifiedSections.Add(&Section);
				Updates.Add([&Native, Default, Desired, Settings] {
					if (Default.IsSet()) Native.SetDefault(Default.GetValue()); else Native.RemoveDefault();
					for (const auto* Field : Settings) Field->CopyCompleteValue(Field->ContainerPtrToValuePtr<void>(&Native), Field->ContainerPtrToValuePtr<void>(&Desired.Get()));
				});
			}
			return true;
		}

		template <typename Channel>
		bool Prepare(Channel& Native, UMovieSceneSection& Section,
			const TArray<FTimelineAnimationKey>& Before, const TArray<FTimelineAnimationKey>& After,
			const TSharedPtr<FJsonObject>& Source = nullptr)
		{
			ErrorDetail.Reset();
			using Value = TKeyValue<Channel>;
			if (Native.GetData().GetValues().Num() != Before.Num())
			{
				FailValue(TEXT("timeline_channel_key_count_changed"), TEXT("/keys"), *FString::Printf(TEXT("%d keys from the captured channel"), Before.Num()), MakeShared<FJsonValueNumber>(Native.GetData().GetValues().Num()), Source);
				ErrorDetail.SourceKind = TEXT("native_result"); return false;
			}
			TMap<FString, FKeyHandle> Handles;
			TMap<FString, const FTimelineAnimationKey*> Originals;
			for (int32 Index = 0; Index < Before.Num(); ++Index)
			{
				if (Before[Index].KeyRef.IsEmpty() || Handles.Contains(Before[Index].KeyRef))
				{
					FailValue(TEXT("timeline_channel_captured_key_reference_invalid"), TEXT("/keys/") + FString::FromInt(Index) + TEXT("/key_ref"),
						TEXT("non-empty unique captured key reference"), MakeShared<FJsonValueBoolean>(!Before[Index].KeyRef.IsEmpty()));
					ErrorDetail.InternalContext = Before[Index].KeyRef; ErrorDetail.SourceKind = TEXT("native_result"); return false;
				}
				Handles.Add(Before[Index].KeyRef, Native.GetData().GetHandle(Index));
				Originals.Add(Before[Index].KeyRef, &Before[Index]);
			}
			TSet<FString> DesiredRefs;
			for (const auto& Key : After)
			{
				int64 Tick = 0;
				if (Key.KeyRef.IsEmpty() || DesiredRefs.Contains(Key.KeyRef))
				{
					FailValue(TEXT("timeline_key_reference_invalid"), TEXT("/key_ref"), Key.KeyRef.IsEmpty() ? TEXT("non-empty key reference") : TEXT("key reference used only once in this channel"),
						MakeShared<FJsonValueBoolean>(!Key.KeyRef.IsEmpty()), Key.SourceRecord);
					ErrorDetail.InternalContext = Key.KeyRef; return false;
				}
				if (Key.ValueType != ValueType(Native, &Section))
					return FailValue(TEXT("timeline_key_type_mismatch"), TEXT("/value_type"), ValueType(Native, &Section), MakeShared<FJsonValueString>(Key.ValueType), Key.SourceRecord);
				if (Key.Transform.IsSet())
					return FailValue(TEXT("timeline_scalar_key_transform_unsupported"), TEXT("/transform"), TEXT("no transform on a scalar channel key"), MakeShared<FJsonValueBoolean>(true), Key.SourceRecord);
				if (!StateSemantic::TryParseInt64Decimal(Key.Time.Tick, Tick) || Tick < MIN_int32 || Tick > MAX_int32)
					return FailValue(TEXT("timeline_key_tick_invalid"), TEXT("/time/tick"), TEXT("integer from -2147483648 through 2147483647"), MakeShared<FJsonValueString>(Key.Time.Tick), Key.SourceRecord);
				DesiredRefs.Add(Key.KeyRef);
				const FFrameNumber Time(static_cast<int32>(Tick));
				if (const auto* Original = Originals.Find(Key.KeyRef))
				{
					const FKeyHandle Handle = Handles.FindChecked(Key.KeyRef);
					const int32 Index = Native.GetData().GetIndex(Handle);
					if (Index == INDEX_NONE)
					{
						FailValue(TEXT("timeline_channel_captured_key_missing"), TEXT("/key_ref"), TEXT("captured key still present in the native channel"), nullptr, Key.SourceRecord);
						ErrorDetail.InternalContext = Key.KeyRef; ErrorDetail.SourceKind = TEXT("native_result"); return false;
					}
					Value DesiredValue = Native.GetData().GetValues()[Index];
					if (!PrepareValue(Native, Section, Key, DesiredValue, true, ErrorDetail))
						return FailValue(TEXT("timeline_key_value_invalid"), FString(TEXT("/")) + ValueField<Channel>(), ValueType(Native, &Section),
							Key.Value.IsValid() ? Key.Value : MakeShared<FJsonValueString>(Key.ValueLexeme), Key.SourceRecord);
					if (!DecodeProperties(Key, DesiredValue, &Section, ErrorDetail))
						return FailValue(TEXT("timeline_key_properties_invalid"), TEXT("/properties"), TEXT("key properties"), Key.Properties, Key.SourceRecord);
					Verifications.Emplace(Section.GetPathName() + TEXT(":") + Key.KeyRef,
						[&Native, &Section, Handle, DesiredValue, Time, Source = Key.SourceRecord](UE::UAI::Contract::FDiagnostic& Error) {
						return VerifyKey(Native, Section, Handle, DesiredValue, Time, Source, Error);
					});
					if (!ValuesEqual(Native.GetData().GetValues()[Index], DesiredValue) || (*Original)->Time.Tick != Key.Time.Tick)
					{
						ModifiedSections.Add(&Section);
						Updates.Add([&Native, Handle, DesiredValue, Time] {
							auto Data = Native.GetData();
							const int32 Current = Data.GetIndex(Handle);
							Data.GetValues()[Current] = DesiredValue;
							Data.MoveKey(Current, Time); // Carries interpolation and all tangent fields.
						});
					}
				}
				else
				{
					Value DesiredValue{};
					if (!PrepareValue(Native, Section, Key, DesiredValue, false, ErrorDetail))
						return FailValue(TEXT("timeline_key_value_invalid"), FString(TEXT("/")) + ValueField<Channel>(), ValueType(Native, &Section),
							Key.Value.IsValid() ? Key.Value : MakeShared<FJsonValueString>(Key.ValueLexeme), Key.SourceRecord);
					if (!DecodeProperties(Key, DesiredValue, &Section, ErrorDetail))
						return FailValue(TEXT("timeline_key_properties_invalid"), TEXT("/properties"), TEXT("key properties"), Key.Properties, Key.SourceRecord);
					ModifiedSections.Add(&Section);
					const auto Handle = MakeShared<FKeyHandle>();
					Additions.Add([&Native, DesiredValue, Time, Handle] {
						auto Data = Native.GetData(); *Handle = Data.GetHandle(Data.AddKey(Time, DesiredValue));
					});
					Verifications.Emplace(Section.GetPathName() + TEXT(":") + Key.KeyRef,
						[&Native, &Section, Handle, DesiredValue, Time, Source = Key.SourceRecord](UE::UAI::Contract::FDiagnostic& Error) {
						return VerifyKey(Native, Section, *Handle, DesiredValue, Time, Source, Error);
					});
				}
			}
			for (const auto& Key : Before) if (!DesiredRefs.Contains(Key.KeyRef))
			{
				ModifiedSections.Add(&Section);
				const FKeyHandle Handle = Handles.FindChecked(Key.KeyRef);
				Removals.Add([&Native, Handle] {
					auto Data = Native.GetData();
					const int32 Index = Data.GetIndex(Handle);
					if (Index != INDEX_NONE) Data.RemoveKey(Index);
				});
			}
			Verifications.Emplace(Section.GetPathName() + TEXT(":keys"),
				[&Native, Count = After.Num(), Source](UE::UAI::Contract::FDiagnostic& Error) {
					const int32 ActualCount = Native.GetData().GetValues().Num();
					return ActualCount == Count || VerificationFailure(Error, TEXT("The native channel key count differs from the requested count."),
						UeAgentPropertyValueCodec::FValueComparison::Different(TEXT("/keys"), MakeShared<FJsonValueNumber>(Count), MakeShared<FJsonValueNumber>(ActualCount)), Source);
				});
			return true;
		}

		/** Inline upsert touches one key; native channel times are kept sorted. */
		template <typename Channel>
		bool PrepareUpsert(Channel& Native, UMovieSceneSection& Section, FFrameNumber Time,
			const FTimelineAnimationKey& Key)
		{
			ErrorDetail.Reset();
			auto Data = Native.GetData();
			const int32 Last = Algo::UpperBound(Data.GetTimes(), Time) - 1;
			const bool bExisting = Last >= 0 && Data.GetTimes()[Last] == Time;
			TKeyValue<Channel> DesiredValue = bExisting ? Data.GetValues()[Last] : TKeyValue<Channel>{};
			if (!PrepareValue(Native, Section, Key, DesiredValue, bExisting, ErrorDetail))
				return FailValue(TEXT("timeline_key_value_invalid"), FString(TEXT("/")) + ValueField<Channel>(), ValueType(Native, &Section),
					Key.Value.IsValid() ? Key.Value : MakeShared<FJsonValueString>(Key.ValueLexeme), Key.SourceRecord);
			if (!DecodeProperties(Key, DesiredValue, &Section, ErrorDetail))
				return FailValue(TEXT("timeline_key_properties_invalid"), TEXT("/properties"), TEXT("key properties"), Key.Properties, Key.SourceRecord);
			const auto ResultHandle = MakeShared<FKeyHandle>(bExisting ? Data.GetHandle(Last) : FKeyHandle());
			Verifications.Emplace(Section.GetPathName() + TEXT(":") + Key.KeyRef,
				[&Native, &Section, ResultHandle, DesiredValue, Time, Source = Key.SourceRecord](UE::UAI::Contract::FDiagnostic& Error) {
					return VerifyKey(Native, Section, *ResultHandle, DesiredValue, Time, Source, Error);
				});
			if (bExisting)
			{
				if (ValuesEqual(Data.GetValues()[Last], DesiredValue)) return true;
				const FKeyHandle Handle = Data.GetHandle(Last);
				Updates.Add([&Native, Handle, DesiredValue, Time] {
					auto CurrentData = Native.GetData();
					const int32 Index = CurrentData.GetIndex(Handle);
					CurrentData.GetValues()[Index] = DesiredValue;
					CurrentData.MoveKey(Index, Time);
				});
			}
			else Additions.Add([&Native, DesiredValue, Time, ResultHandle] {
				auto CurrentData = Native.GetData(); *ResultHandle = CurrentData.GetHandle(CurrentData.AddKey(Time, DesiredValue));
			});
			ModifiedSections.Add(&Section);
			return true;
		}

		// These plans borrow native storage only for this synchronous batch. Check
		// the decoded request, not a fresh capture of whatever the writes produced.
		bool Verify(UE::UAI::Contract::FDiagnostic* OutError = nullptr) const
		{
			for (const auto& Verification : Verifications)
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!Verification.Value(Error))
				{
					if (Error.InternalContext.IsEmpty()) Error.InternalContext = Verification.Key;
					if (OutError) *OutError = MoveTemp(Error);
					return false;
				}
			}
			return true;
		}

		bool Apply(UE::UAI::Contract::FDiagnostic* OutError = nullptr)
		{
			for (auto* Section : ModifiedSections) Section->Modify();
			for (const auto& Action : Removals) Action();
			for (const auto& Action : Updates) Action();
			for (const auto& Action : Additions) Action();
			return Verify(OutError);
		}
	};

	/** Inline initialization adapts native values to the same prepared key edit owner. */
	template <typename Channel>
	bool TryUpsertValue(Channel& Native, UMovieSceneSection& Section, FFrameNumber Time,
		const TKeyValue<Channel>& Value, UE::UAI::Contract::FDiagnostic* OutError = nullptr)
	{
		auto Capture = [&](const TKeyValue<Channel>& NativeValue, FFrameNumber Frame, const FString& Ref) {
			FTimelineAnimationKey Key;
			Key.KeyRef = Ref;
			Key.Time.Tick = FString::FromInt(Frame.Value);
			Key.ValueType = ValueType(Native, &Section);
			if constexpr (std::is_same_v<Channel, FMovieSceneObjectPathChannel>) Key.Value = CaptureValue(Native, NativeValue);
			else { Key.ValueLexeme = ValueLexeme(Native, TValueAccess<TKeyValue<Channel>>::Get(NativeValue)); Key.bHasValueLexeme = true; }
			Key.Properties = CaptureProperties(NativeValue, &Section);
			return Key;
		};
		FChannelEdits Edits;
		if (!Edits.PrepareUpsert(Native, Section, Time, Capture(Value, Time, TEXT("upsert"))))
		{ if (OutError) *OutError = Edits.ErrorDetail; return false; }
		return Edits.Apply(OutError);
	}
}
