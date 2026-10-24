#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include <initializer_list>

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCoreAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectEnhancedInputMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperTileMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"

#include "AssetToolsModule.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Factories/Factory.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "Particles/SubUVAnimation.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	bool CanCreateNativeFactoryObject(const UFactory* Factory)
	{
		if (!Factory || !Factory->GetSupportedClass() || Factory->bEditorImport) return false;
		// UE 5.6 gates only the Atlas menu and Details visibility on its experimental
		// setting. Explicit authoring uses the same native factory without changing config.
		return Factory->CanCreateNew()
			|| Factory->GetClass()->GetPathName() == TEXT("/Script/Paper2DEditor.PaperSpriteAtlasFactory");
	}

	UObject* CreateNativeFactoryObject(UPackage* Package, FName ObjectName, UClass* Class,
		UFactory* Factory, bool bUnpublishedTemplate, ExecutionMutation::FExecutionMutationError& OutError)
	{
		auto Reject = [&](const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) -> UObject*
		{
			OutError = ExecutionMutation::FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			if (Package) OutError.InternalContext = Package->GetName() + TEXT(".") + ObjectName.ToString();
			if (Package && !bUnpublishedTemplate) OutError.TargetPath = Package->GetName() + TEXT(".") + ObjectName.ToString();
			if (Class) OutError.CandidateName = Class->GetPathName();
			return nullptr;
		};
		if (!Package || !Class)
			return Reject(TEXT("asset_data_native_factory_unavailable"),
				!Package ? TEXT("Native asset creation requires an owning package.") : TEXT("Native asset creation requires a concrete class."),
				!Package ? TEXT("/package") : TEXT("/class"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (!CanCreateNativeFactoryObject(Factory))
		{
			if (!Factory || !Factory->GetSupportedClass())
				return Reject(TEXT("asset_data_native_factory_unavailable"),
					!Factory ? TEXT("The native factory object is unavailable.") : TEXT("The native factory declares no supported class."),
					!Factory ? TEXT("/factory") : TEXT("/factory/supported_class"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (Factory->bEditorImport)
				return Reject(TEXT("asset_data_native_factory_unavailable"),
					TEXT("A factory marked for editor import is not accepted for new-asset creation through this path."),
					TEXT("/factory/editor_import"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			return Reject(TEXT("asset_data_native_factory_unavailable"),
				TEXT("The selected native factory does not support creating new assets."),
				TEXT("/factory/can_create_new"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}
		if (!Class->IsChildOf(Factory->GetSupportedClass()))
			return Reject(TEXT("asset_data_native_factory_unavailable"),
				TEXT("The requested class does not derive from the selected factory's supported class."),
				TEXT("/class"), MakeShared<FJsonValueString>(Class->GetPathName()),
				MakeShared<FJsonValueString>(Factory->GetSupportedClass()->GetPathName()));
		if (bUnpublishedTemplate)
		{
			if (Package != GetTransientPackage())
				return Reject(TEXT("asset_data_factory_template_outer_invalid"),
					TEXT("An unpublished factory template must be owned by the transient package."),
					TEXT("/template_outer_is_transient"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			UObject* Created = Factory->FactoryCreateNew(Class, Package, ObjectName, RF_Transient, nullptr, GWarn);
			if (!Created) return Reject(TEXT("asset_data_native_factory_create_failed"),
				TEXT("The native factory returned no object for the requested unpublished template."),
				TEXT("/created_object"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return Created;
		}
		FAssetToolsModule* AssetToolsModule = FModuleManager::LoadModulePtr<FAssetToolsModule>(TEXT("AssetTools"));
		const FString PackagePath = FPackageName::GetLongPackagePath(Package->GetName());
		if (!AssetToolsModule || PackagePath.IsEmpty())
			return Reject(TEXT("asset_data_native_factory_unavailable"),
				!AssetToolsModule ? TEXT("The AssetTools module is unavailable for native asset creation.") : TEXT("The owning package has no valid asset folder path."),
				!AssetToolsModule ? TEXT("/asset_tools") : TEXT("/package_path"),
				!AssetToolsModule ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(PackagePath)),
				!AssetToolsModule ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("asset folder path"))));
		UObject* Created = AssetToolsModule->Get().CreateAsset(ObjectName.ToString(), PackagePath, Class, Factory,
			FName(TEXT("UeAgentInterface.AssetSimple")));
		if (!Created) return Reject(TEXT("asset_data_native_factory_create_failed"),
			TEXT("AssetTools returned no object for the requested native asset creation."),
			TEXT("/created_object"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return Created;
	}

	using namespace ContextResource;
	using namespace Discovery;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{

        FExecutionMutationError NativeFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = TEXT("native_result");
            Error.Source = {};
            Error.SemanticPointer.Reset();
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
        }

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic,
            const FString& PropertyPath = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = PropertyPath;
            return false;
        }

        void MarkNativeFailure(FExecutionMutationError& Error)
        {
            Error.SourceKind = TEXT("native_result");
            if (Error.Source.InternalJsonPath.IsEmpty())
                Error.Source.InternalJsonPath = Error.SemanticPointer.IsEmpty() ? Error.Source.JsonPath : Error.SemanticPointer;
            static_cast<Contract::FPublicSourceLocation&>(Error.Source) = {};
            Error.SemanticPointer.Reset();
            for (auto& Child : Error.Diagnostics) MarkNativeFailure(Child);
            if (Error.PublicCause)
            {
                auto Cause = MakeShared<FExecutionMutationError>(*Error.PublicCause);
                MarkNativeFailure(*Cause);
                Error.PublicCause = MoveTemp(Cause);
            }
        }

        FExecutionMutationError ValueFailure(const FString& Code, const FString& Reason, const FString& Field,
            const FUeAgentCanonicalValue* Actual, const FString& Expected, const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
                MakeShared<FJsonValueString>(Expected), nullptr, TEXT("execution"), Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual != nullptr;
            Error.ExpectedValuePresent = true;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
            {
                MarkNativeFailure(ProjectionError);
                Error.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            if (Error.SourceKind == TEXT("native_result")) MarkNativeFailure(Error);
            return Error;
        }

        TSharedPtr<FJsonValue> NativeFacts(std::initializer_list<TPair<FString, TSharedPtr<FJsonValue>>> Fields)
        {
            const auto Object = MakeShared<FJsonObject>();
            for (const auto& Field : Fields) Object->SetField(Field.Key, Field.Value);
            return MakeShared<FJsonValueObject>(Object);
        }

        TSharedPtr<FJsonValue> NativeNumberFact(const double Value)
        {
            return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
        }

        FString ChildPath(const FString& Parent, const FString& Field)
        {
            return Parent + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
        }

        bool WithInputField(FExecutionMutationError& Error, const FString& Field)
        {
            if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Field;
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

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			return Object.IsValid()
				? FindObjectField(*Object, Field)
				: nullptr;
		}

        bool TryReadString(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
            FString& OutValue, FExecutionMutationError& OutError)
        {
            const auto* Value = FindObjectField(Object, Field);
            if (!Value || Value->GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_native_string_invalid"),
                    Value ? TEXT("The planned operation field is not a string.") : TEXT("The planned operation is missing a required string field."),
                    Field, Value, TEXT("string"), TEXT("native_result")));
            OutValue = Value->GetText();
            return true;
        }

        bool TryReadNumber(const FUeAgentCanonicalValue* Value, double& OutNumber,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal))
                return Fail(OutError, ValueFailure(TEXT("asset_data_number_invalid"),
                    TEXT("The field requires a number."), Field, Value, TEXT("finite number")));
            if (!TryParseFiniteDecimal(Value->GetLexeme(), OutNumber))
                return Fail(OutError, ValueFailure(TEXT("asset_data_number_invalid"),
                    TEXT("The number cannot be represented as a finite double."), Field, Value, TEXT("finite double")));
            return true;
        }

        bool TryReadInteger(const FUeAgentCanonicalValuePtr& Object, const FString& Field, int64& OutValue,
            FExecutionMutationError& OutError, const FString& Path, const TCHAR* SourceKind)
        {
            const auto* Value = FindObjectField(Object, Field);
            if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value->GetLexeme(), OutValue))
                return Fail(OutError, ValueFailure(TEXT("asset_data_integer_invalid"),
                    TEXT("The field requires an integer representable as int64."), Path, Value, TEXT("int64 integer"), SourceKind));
            return true;
        }

        bool TryReadBoolean(const FUeAgentCanonicalValue* Value, bool& bOutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!Value || Value->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_boolean_invalid"),
                    TEXT("The field requires a boolean."), Field, Value, TEXT("boolean")));
            bOutValue = Value->GetBoolean();
            return true;
        }

        bool TryReadIntegralPixel(const FUeAgentCanonicalValue* Value, int32& OutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            int64 Number = 0;
            if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value->GetLexeme(), Number) || Number < MIN_int32 || Number > MAX_int32)
                return Fail(OutError, ValueFailure(TEXT("asset_data_pixel_invalid"),
                    TEXT("The pixel or frame count must be an int32 integer."), Field, Value, TEXT("integer in [-2147483648, 2147483647]")));
            OutValue = static_cast<int32>(Number);
            return true;
        }

        bool TryRequireObjectFields(const FUeAgentCanonicalValue* Value, std::initializer_list<const TCHAR*> Fields,
            const FString& Path, FExecutionMutationError& OutError, bool bExact)
        {
            if (!Value || Value->GetKind() != EUeAgentValueKind::Object
                || (bExact && Value->GetObjectItems().Num() != static_cast<int32>(Fields.size())))
            {
                auto Error = ValueFailure(TEXT("asset_data_object_invalid"),
                    bExact ? TEXT("The field must be an object with exactly the required members.")
                        : TEXT("The field must be an object with the required members."), Path, Value, TEXT("object"));
                TArray<TSharedPtr<FJsonValue>> Expected;
                for (const TCHAR* Field : Fields) Expected.Add(MakeShared<FJsonValueString>(Field));
                Error.ExpectedValue = MakeShared<FJsonValueArray>(Expected);
                return Fail(OutError, MoveTemp(Error));
            }
            for (const TCHAR* Field : Fields)
                if (!FindObjectField(*Value, Field))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_field_missing"), TEXT("The required object member is missing."),
                        ChildPath(Path, Field), nullptr, TEXT("present member")));
            return true;
        }

        bool TryReadPaperSpriteRegion(const FUeAgentCanonicalValue* Value, FIntPoint& OutOffset, FIntPoint& OutDimension,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height")}, Field, OutError, true)) return false;
            if (!TryReadIntegralPixel(FindObjectField(*Value, TEXT("x")), OutOffset.X, ChildPath(Field, TEXT("x")), OutError)
                || !TryReadIntegralPixel(FindObjectField(*Value, TEXT("y")), OutOffset.Y, ChildPath(Field, TEXT("y")), OutError)
                || !TryReadIntegralPixel(FindObjectField(*Value, TEXT("width")), OutDimension.X, ChildPath(Field, TEXT("width")), OutError)
                || !TryReadIntegralPixel(FindObjectField(*Value, TEXT("height")), OutDimension.Y, ChildPath(Field, TEXT("height")), OutError)) return false;
            const TCHAR* Invalid = OutOffset.X < 0 ? TEXT("x") : OutOffset.Y < 0 ? TEXT("y")
                : OutDimension.X <= 0 ? TEXT("width") : OutDimension.Y <= 0 ? TEXT("height") : nullptr;
            if (Invalid)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_sprite_source_region_invalid"),
                    TEXT("Sprite region offsets must be nonnegative and dimensions must be positive."), ChildPath(Field, Invalid),
                    FindObjectField(*Value, Invalid), (OutOffset.X < 0 || OutOffset.Y < 0) ? TEXT("integer >= 0") : TEXT("integer > 0")));
            return true;
        }

        bool TryReadPaperSpritePivot(const FUeAgentCanonicalValue* Value, FVector2D& OutPivot,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("x"), TEXT("y")}, Field, OutError, true)) return false;
            int32 X = 0, Y = 0;
            if (!TryReadIntegralPixel(FindObjectField(*Value, TEXT("x")), X, ChildPath(Field, TEXT("x")), OutError)
                || !TryReadIntegralPixel(FindObjectField(*Value, TEXT("y")), Y, ChildPath(Field, TEXT("y")), OutError)) return false;
            OutPivot = FVector2D(static_cast<double>(X), static_cast<double>(Y));
            return true;
        }

		struct FPaperFlipbookFrameState
		{
			TWeakObjectPtr<UPaperSprite> Sprite;
			int32 FrameRun = 1;
		};

        bool TryReadPaperFlipbookFrames(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue* Value, TArray<FPaperFlipbookFrameState>& OutFrames,
            FExecutionMutationError& OutError, const FString& Field)
        {
            OutFrames.Reset();
            if (!Value || Value->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frames_invalid"),
                    TEXT("Flipbook frames must be an array."), Field, Value, TEXT("array")));
            int64 TotalFrames = 0;
            for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
            {
                const auto& FrameValue = Value->GetArrayItems()[Index].Get();
                const FString Path = ChildPath(Field, ::LexToString(Index));
                if (!TryRequireObjectFields(&FrameValue, {TEXT("sprite_ref"), TEXT("frame_run")}, Path, OutError, true)) return false;
                const auto* SpriteRef = FindObjectField(FrameValue, TEXT("sprite_ref"));
                const auto* FrameRunValue = FindObjectField(FrameValue, TEXT("frame_run"));
                int32 FrameRun = 0;
                if (!TryReadIntegralPixel(FrameRunValue, FrameRun, ChildPath(Path, TEXT("frame_run")), OutError)) return false;
                if (FrameRun < 1)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frame_run_invalid"),
                        TEXT("A flipbook frame run must be positive."), ChildPath(Path, TEXT("frame_run")), FrameRunValue, TEXT("integer >= 1")));
                FPaperFlipbookFrameState Frame;
                Frame.FrameRun = FrameRun;
                if (SpriteRef->GetKind() == EUeAgentValueKind::String)
                {
                    const FString& Ref = SpriteRef->GetText();
                    if (Ref.IsEmpty())
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_sprite_ref_invalid"),
                            TEXT("The sprite reference is empty."), ChildPath(Path, TEXT("sprite_ref")), SpriteRef, TEXT("nonempty sprite reference or null")));
                    UObject* Resolved = nullptr;
                    if (!Resolver.TryResolveExisting(Ref, Resolved, OutError))
                        return WithInputField(OutError, ChildPath(Path, TEXT("sprite_ref")));
                    UPaperSprite* Sprite = Cast<UPaperSprite>(Resolved);
                    if (!Sprite)
                    {
                        OutError = ValueFailure(TEXT("asset_data_paper_flipbook_sprite_class_invalid"),
                            TEXT("The referenced object is not a PaperSprite."), ChildPath(Path, TEXT("sprite_ref")), SpriteRef, TEXT("PaperSprite reference"));
                        OutError.ActualValue = NativeFacts({{TEXT("reference"), MakeShared<FJsonValueString>(Ref)}, {TEXT("class"), ObjectClassValue(Resolved)}});
                        return false;
                    }
                    Frame.Sprite = Sprite;
                }
                else if (SpriteRef->GetKind() != EUeAgentValueKind::Null)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_sprite_ref_invalid"),
                        TEXT("A sprite reference must be a string or null."), ChildPath(Path, TEXT("sprite_ref")), SpriteRef, TEXT("sprite reference string or null")));
                TotalFrames += Frame.FrameRun;
                if (TotalFrames > MAX_int32)
                {
                    OutError = NativeFailure(TEXT("asset_data_paper_flipbook_total_frames_overflow"),
                        TEXT("The total flipbook frame run exceeds int32 capacity."), ChildPath(Path, TEXT("frame_run")),
                        MakeShared<FJsonValueNumberString>(::LexToString(TotalFrames)), MakeShared<FJsonValueNumber>(MAX_int32));
                    OutError.SourceKind = TEXT("authored_value");
                    return false;
                }
                OutFrames.Add(MoveTemp(Frame));
            }
            return true;
        }

        bool TryResolveUniqueTextureArray(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue* Value, TArray<TWeakObjectPtr<UTexture>>& OutTextures,
            FExecutionMutationError& OutError, const FString& Field)
        {
            OutTextures.Reset();
            if (!Value || Value->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_refs_invalid"),
                    TEXT("Texture references must be an array."), Field, Value, TEXT("array of unique texture references")));
            TSet<FString> SeenRefs;
            TSet<UTexture*> SeenTextures;
            for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
            {
                const auto& RefValue = Value->GetArrayItems()[Index].Get();
                const FString Path = ChildPath(Field, ::LexToString(Index));
                if (RefValue.GetKind() != EUeAgentValueKind::String)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_ref_invalid"),
                        TEXT("A texture reference must be a string."), Path, &RefValue, TEXT("nonempty texture reference")));
                const FString& Ref = RefValue.GetText();
                if (Ref.IsEmpty() || SeenRefs.Contains(Ref))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_duplicate_ref"),
                        Ref.IsEmpty() ? TEXT("The texture reference is empty.") : TEXT("The texture reference occurs more than once."),
                        Path, &RefValue, TEXT("nonempty unique texture reference")));
                UObject* Resolved = nullptr;
                if (!Resolver.TryResolveExisting(Ref, Resolved, OutError)) return WithInputField(OutError, Path);
                UTexture* Texture = Cast<UTexture>(Resolved);
                if (!Texture || SeenTextures.Contains(Texture))
                {
                    OutError = ValueFailure(TEXT("asset_data_texture_collection_texture_invalid"),
                        !Texture ? TEXT("The referenced object is not a Texture.") : TEXT("Another reference resolves to the same Texture."),
                        Path, &RefValue, TEXT("unique resolved Texture"));
                    OutError.ActualValue = NativeFacts({{TEXT("reference"), MakeShared<FJsonValueString>(Ref)}, {TEXT("class"), ObjectClassValue(Resolved)}});
                    return false;
                }
                SeenRefs.Add(Ref);
                SeenTextures.Add(Texture);
                OutTextures.Add(Texture);
            }
            return true;
        }

        bool TryReadLinearColor(const FUeAgentCanonicalValue* Value, FLinearColor& OutColor,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}, Field, OutError, false)) return false;
            double R = 0, G = 0, B = 0, A = 0;
            if (!TryReadNumber(FindObjectField(*Value, TEXT("r")), R, ChildPath(Field, TEXT("r")), OutError)
                || !TryReadNumber(FindObjectField(*Value, TEXT("g")), G, ChildPath(Field, TEXT("g")), OutError)
                || !TryReadNumber(FindObjectField(*Value, TEXT("b")), B, ChildPath(Field, TEXT("b")), OutError)
                || !TryReadNumber(FindObjectField(*Value, TEXT("a")), A, ChildPath(Field, TEXT("a")), OutError)) return false;
            OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
            return true;
        }

		bool TryReadRenderTargetFormat(
			const FUeAgentCanonicalValue* Value,
			ETextureRenderTargetFormat& OutFormat, const FString& Field, FExecutionMutationError& OutError)
		{
			if (!Value || Value->GetKind() != EUeAgentValueKind::String)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_render_target_format_invalid"),
                    TEXT("The render target format must be a name."), Field, Value, TEXT("render target format string")));
			}
			const FString Format =
				Value->GetText()
					.TrimStartAndEnd()
					.ToLower();
			const TPair<
				const TCHAR*,
				ETextureRenderTargetFormat> Formats[] = {
				{TEXT("r8"), RTF_R8},
				{TEXT("rg8"), RTF_RG8},
				{TEXT("rgba8"), RTF_RGBA8},
				{TEXT("rgba8_srgb"), RTF_RGBA8_SRGB},
				{TEXT("r16f"), RTF_R16f},
				{TEXT("rg16f"), RTF_RG16f},
				{TEXT("rgba16f"), RTF_RGBA16f},
				{TEXT("r32f"), RTF_R32f},
				{TEXT("rg32f"), RTF_RG32f},
				{TEXT("rgba32f"), RTF_RGBA32f},
				{TEXT("rgb10a2"), RTF_RGB10A2},
			};
			for (const TPair<
				const TCHAR*,
				ETextureRenderTargetFormat>& Candidate
				: Formats)
			{
				if (Format == Candidate.Key)
				{
					OutFormat = Candidate.Value;
					return true;
				}
			}
            TArray<TSharedPtr<FJsonValue>> Expected;
            for (const auto& Candidate : Formats) Expected.Add(MakeShared<FJsonValueString>(Candidate.Key));
            auto Error = ValueFailure(TEXT("asset_data_render_target_format_invalid"), TEXT("The render target format name is unsupported."),
                Field, Value, TEXT("supported render target format"));
            Error.ExpectedValue = MakeShared<FJsonValueArray>(Expected);
            return Fail(OutError, MoveTemp(Error));
		}



		struct FTypedChange
		{
			FString FieldId;
			FString Presence;
			FString FieldBindingId;
			FString TypeId;
			FUeAgentCanonicalValuePtr CanonicalValue;
			bool bHasValue = false;
		};

		bool TryReadTypedChanges(
			const FUeAgentCanonicalValuePtr& After,
			TArray<FTypedChange>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			const FUeAgentCanonicalValue* Changes =
				FindObjectField(After, TEXT("typed_changes"));
			if (!Changes
				|| Changes->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, ValueFailure(TEXT("asset_data_native_typed_changes_missing"), TEXT("The planned operation is missing its typed changes array."), TEXT("typed_changes"), Changes, TEXT("array"), TEXT("native_result")));
			}
            for (int32 Index = 0; Index < Changes->GetArrayItems().Num(); ++Index)
            {
                const auto& Unit = Changes->GetArrayItems()[Index];
                const FString Path = TEXT("typed_changes/") + ::LexToString(Index);
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, ValueFailure(TEXT("asset_data_native_change_invalid"), TEXT("The planned change entry must be an object."), Path, &Unit.Get(), TEXT("object"), TEXT("native_result")));
				}
				FTypedChange Change;
				for (const TPair<
					FString,
					FUeAgentCanonicalValueRef>& Item
						: Unit->GetObjectItems())
				{
					if (Item.Key == TEXT("field_id")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.FieldId = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("presence")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.Presence = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("field_binding_id")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.FieldBindingId =
							Item.Value->GetText();
					}
					else if (Item.Key == TEXT("value")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::Object)
					{
						for (const TPair<
							FString,
							FUeAgentCanonicalValueRef>& ValueItem
								: Item.Value->GetObjectItems())
						{
							if (ValueItem.Key == TEXT("type_id")
								&& ValueItem.Value->GetKind()
									== EUeAgentValueKind::String)
							{
								Change.TypeId =
									ValueItem.Value->GetText();
							}
							else if (ValueItem.Key
									== TEXT("canonical_value"))
							{
								Change.CanonicalValue =
									FUeAgentCanonicalValuePtr(
										ValueItem.Value);
								Change.bHasValue = true;
							}
						}
					}
				}
				if (Change.FieldId.IsEmpty()
					|| Change.Presence.IsEmpty()
					|| Change.FieldBindingId.IsEmpty())
				{
					const FString Missing = Change.FieldId.IsEmpty() ? TEXT("field_id")
                        : Change.Presence.IsEmpty() ? TEXT("presence") : TEXT("field_binding_id");
                    return Fail(OutError, ValueFailure(TEXT("asset_data_native_change_invalid"),
                        TEXT("The planned field change requires a nonempty string member."), ChildPath(Path, Missing),
                        FindObjectField(Unit.Get(), Missing), TEXT("nonempty string"), TEXT("native_result")));
				}
				OutChanges.Add(MoveTemp(Change));
			}
			return true;
		}

		enum class EActionKind : uint8
		{
			CreateObject,
			CloneObject,
			UpdateRenderTarget,
			UpdateStructuredAsset,
			UpdateMediaPlayer,
			UpdateMediaTexture,
			UpdateSubUVAnimation,
			UpdateTextureCollection,
			ReplaceTextureArray,
			ReplaceTextureCubeArray,
			ReplaceTextureCube,
			ReplaceVolumeTexture,
			UpdateInputAction,
			UpdateInputMappingContext,
			UpdatePaperSprite,
			ReplacePaperFlipbook,
			ReplacePaperTileMap,
			ReplacePaperTileSet,
			UpdateCoreAsset,
		};

		struct FAssetDataAction
		{
			EActionKind Kind = EActionKind::CloneObject;
			FString CanonicalKey;
			FString TargetRef;
			FString SourceRef;
			FString ProfileId;
			FString NativeBindingId;
			FString NativeMechanicId;
			TArray<FTypedChange> Changes;
			TWeakObjectPtr<UObject> Target;
			TWeakObjectPtr<UObject> Source;
			TWeakObjectPtr<UClass> CreateClass;
			TWeakObjectPtr<UFactory> CreateFactory;
			TWeakObjectPtr<UPackage> Package;
			FName ObjectName;
			bool bPackageWasDirty = false;
			bool bCreated = false;

			int32 OriginalSizeX = 0;
			int32 OriginalSizeY = 0;
			ETextureRenderTargetFormat OriginalRenderTargetFormat =
				RTF_RGBA8;
			FLinearColor OriginalClearColor = FLinearColor::Black;
			bool bOriginalAutoGenerateMips = false;
			bool bChangeRenderTargetSize = false;
			int32 NewSizeX = 0;
			int32 NewSizeY = 0;
			bool bChangeRenderTargetFormat = false;
			ETextureRenderTargetFormat NewRenderTargetFormat =
				RTF_RGBA8;
			bool bChangeRenderTargetClearColor = false;
			FLinearColor NewRenderTargetClearColor =
				FLinearColor::Black;
			bool bChangeRenderTargetAutoGenerateMips = false;
			bool bNewAutoGenerateMips = false;

			bool bOriginalMediaPlayerPlayOnOpen = false;
			bool bOriginalMediaPlayerLoop = false;
			bool bChangeMediaPlayerPlayOnOpen = false;
			bool bNewMediaPlayerPlayOnOpen = false;
			bool bChangeMediaPlayerLoop = false;
			bool bNewMediaPlayerLoop = false;

			TWeakObjectPtr<UMediaPlayer> OriginalMediaTexturePlayer;
			bool bOriginalMediaTextureAutoClear = false;
			FLinearColor OriginalMediaTextureClearColor =
				FLinearColor::Black;
			bool bChangeMediaTexturePlayer = false;
			TWeakObjectPtr<UMediaPlayer> NewMediaTexturePlayer;
			bool bChangeMediaTextureAutoClear = false;
			bool bNewMediaTextureAutoClear = false;
			bool bChangeMediaTextureClearColor = false;
			FLinearColor NewMediaTextureClearColor =
				FLinearColor::Black;

			TWeakObjectPtr<UTexture2D> OriginalSubUVTexture;
			int32 OriginalSubUVHorizontal = 0;
			int32 OriginalSubUVVertical = 0;
			TEnumAsByte<ESubUVBoundingVertexCount>
				OriginalSubUVBoundingMode = BVC_EightVertices;
			bool bChangeSubUVTexture = false;
			TWeakObjectPtr<UTexture2D> NewSubUVTexture;
			bool bChangeSubUVGrid = false;
			int32 NewSubUVHorizontal = 0;
			int32 NewSubUVVertical = 0;
			bool bChangeSubUVBoundingMode = false;
			TEnumAsByte<ESubUVBoundingVertexCount>
				NewSubUVBoundingMode = BVC_EightVertices;

			TArray<TWeakObjectPtr<UTexture>>
				OriginalTextureCollectionTextures;
			bool bChangeTextureCollectionTextures = false;
			TArray<TWeakObjectPtr<UTexture>>
				NewTextureCollectionTextures;

			FEnhancedInputActionPreparedState InputActionState;
			FEnhancedInputMappingContextPreparedState
				InputMappingContextState;
			FTextureSourcePreparedState TextureSourceState;

			TWeakObjectPtr<UTexture2D>
				OriginalPaperSpriteSourceTexture;
			FIntPoint OriginalPaperSpriteOffset =
				FIntPoint::ZeroValue;
			FIntPoint OriginalPaperSpriteDimension =
				FIntPoint::ZeroValue;
			TEnumAsByte<ESpritePivotMode::Type>
				OriginalPaperSpritePivotMode =
					ESpritePivotMode::Center_Center;
			FVector2D OriginalPaperSpriteCustomPivot =
				FVector2D::ZeroVector;
			TArray<TWeakObjectPtr<UTexture>>
				OriginalPaperSpriteAdditionalTextures;
			TWeakObjectPtr<UTexture2D>
				NewPaperSpriteSourceTexture;
			FIntPoint NewPaperSpriteOffset =
				FIntPoint::ZeroValue;
			FIntPoint NewPaperSpriteDimension =
				FIntPoint::ZeroValue;
			TEnumAsByte<ESpritePivotMode::Type>
				NewPaperSpritePivotMode =
					ESpritePivotMode::Center_Center;
			FVector2D NewPaperSpriteCustomPivot =
				FVector2D::ZeroVector;
			TArray<TWeakObjectPtr<UTexture>>
				NewPaperSpriteAdditionalTextures;

			TArray<FPaperFlipbookFrameState>
				OriginalPaperFlipbookFrames;
			float OriginalPaperFlipbookFramesPerSecond = 0.0f;
			TArray<FPaperFlipbookFrameState>
				NewPaperFlipbookFrames;
			float NewPaperFlipbookFramesPerSecond = 0.0f;

			FPaperTileMapPreparedState PaperTileMapState;
			FPaperTileSetPreparedState PaperTileSetState;
			FStructuredAssetPreparedState StructuredAssetState;
			FCoreAssetPreparedState CoreAssetState;
		};

		const FTypedChange* FindChange(
			const FAssetDataAction& Action,
			const FString& FieldId)
		{
			for (const FTypedChange& Change : Action.Changes)
			{
				if (Change.FieldId == FieldId)
				{
					return &Change;
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValue* FindCanonicalChangeValue(
			const FAssetDataAction& Action,
			const FString& FieldId)
		{
			const FTypedChange* Change = FindChange(Action, FieldId);
			return Change
				&& Change->bHasValue
				&& Change->CanonicalValue.IsValid()
					? &*Change->CanonicalValue
					: nullptr;
		}

		FEnhancedInputChangeView MakeEnhancedInputChangeView(
			const FAssetDataAction& Action,
			const FString& FieldId)
		{
			FEnhancedInputChangeView View;
			if (const FTypedChange* Change =
				FindChange(Action, FieldId))
			{
				View.bSpecified = true;
				View.Presence = Change->Presence;
				View.CanonicalValue =
					Change->bHasValue
						&& Change->CanonicalValue.IsValid()
							? &*Change->CanonicalValue
							: nullptr;
			}
			return View;
		}

		FTextureSourceChangeView MakeTextureSourceChangeView(
			const FAssetDataAction& Action,
			const FString& FieldId)
		{
			FTextureSourceChangeView View;
			if (const FTypedChange* Change =
				FindChange(Action, FieldId))
			{
				View.bSpecified = true;
				View.Presence = Change->Presence;
				View.CanonicalValue =
					Change->bHasValue
						&& Change->CanonicalValue.IsValid()
							? &*Change->CanonicalValue
							: nullptr;
			}
			return View;
		}

		TArray<FCoreAssetChangeView> MakeCoreAssetChangeViews(
			const FAssetDataAction& Action)
		{
			TArray<FCoreAssetChangeView> Views;
			Views.Reserve(Action.Changes.Num());
			for (const FTypedChange& Change : Action.Changes)
			{
				FCoreAssetChangeView View;
				View.FieldId = Change.FieldId;
				View.Presence = Change.Presence;
				View.CanonicalValue =
					Change.bHasValue
						&& Change.CanonicalValue.IsValid()
							? &*Change.CanonicalValue
							: nullptr;
				Views.Add(MoveTemp(View));
			}
			return Views;
		}

		TArray<FStructuredAssetChangeView>
		MakeStructuredAssetChangeViews(
			const FAssetDataAction& Action)
		{
			TArray<FStructuredAssetChangeView> Views;
			Views.Reserve(Action.Changes.Num());
			for (const FTypedChange& Change : Action.Changes)
			{
				FStructuredAssetChangeView View;
				View.FieldId = Change.FieldId;
				View.Presence = Change.Presence;
				View.CanonicalValue =
					Change.bHasValue
						&& Change.CanonicalValue.IsValid()
							? &*Change.CanonicalValue
							: nullptr;
				Views.Add(MoveTemp(View));
			}
			return Views;
		}

		bool PrepareAction(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver&
				FactoryResolver,
			const FPlannedChange& Change,
			FAssetDataAction& OutAction,
			FExecutionMutationError& OutError)
		{
			OutAction = FAssetDataAction();
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			if (!TryReadString(
					Change.GetAfter(),
					TEXT("profile_id"),
					OutAction.ProfileId, OutError)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("native_binding_id"),
					OutAction.NativeBindingId, OutError)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("target_ref"),
					OutAction.TargetRef, OutError)
				|| !TryReadTypedChanges(
					Change.GetAfter(),
					OutAction.Changes,
					OutError))
			{
				return false;
			}

			const FString& TargetKind =
				Change.GetIdentity().GetTargetKind();
			FString BoundNativeOperationType;
			FString BoundProfileId;
			FString BoundNativeMechanicId;
			FString StaticCreateClassPath;
			UE::UAI::Contract::FDiagnostic BindingError;
            if (!TryResolveAssetDataSimpleEffectNativeBinding(OutAction.NativeBindingId, BoundNativeOperationType,
                BoundProfileId, BoundNativeMechanicId, StaticCreateClassPath, BindingError))
                return Fail(OutError, MoveTemp(BindingError));
            if (BoundNativeOperationType != TargetKind || BoundProfileId != OutAction.ProfileId)
            {
                OutError = NativeFailure(TEXT("asset_data_native_binding_mismatch"),
                    BoundNativeOperationType != TargetKind ? TEXT("The planned native binding targets a different operation.")
                        : TEXT("The planned profile differs from the registered native binding profile."), FString(), nullptr, nullptr);
                OutError.ActualEvidence = MakeShared<FJsonValueString>(BoundNativeOperationType != TargetKind ? BoundNativeOperationType : BoundProfileId);
                OutError.ExpectedEvidence = MakeShared<FJsonValueString>(BoundNativeOperationType != TargetKind ? TargetKind : OutAction.ProfileId);
                OutError.TargetPath = OutAction.TargetRef;
                return false;
            }
			OutAction.NativeMechanicId =
				BoundNativeMechanicId;
			for (const FTypedChange& TypedChange
				: OutAction.Changes)
			{
				UE::UAI::Contract::FDiagnostic FieldError;
				if (!TryValidateAssetDataSimpleEffectFieldBinding(
						OutAction.NativeBindingId,
						TypedChange.FieldBindingId,
						TypedChange.FieldId,
						TypedChange.Presence,
						TypedChange.TypeId,
						TypedChange.bHasValue,
						TypedChange.CanonicalValue.IsValid()
							? TypedChange.CanonicalValue->GetKind()
							: EUeAgentValueKind::Null,
						FieldError))
				{
					return Fail(OutError, FieldError, TypedChange.FieldId);
				}
			}

			if (BoundNativeMechanicId
				== TEXT("uai.asset_data.mechanic:create_object:v1"))
			{
				if (StaticCreateClassPath.IsEmpty())
				{
					return Fail(OutError, NativeFailure(TEXT("asset_data_factory_candidate_required"), TEXT("The creation operation has neither a static class nor a selected factory."), TEXT("class"), nullptr, MakeShared<FJsonValueString>(TEXT("static class or selected factory"))));
				}
				UClass* CreateClass =
					LoadObject<UClass>(
						nullptr,
						*StaticCreateClassPath);
				if (!CreateClass
					|| CreateClass->HasAnyClassFlags(
						CLASS_Abstract
							| CLASS_Deprecated
							| CLASS_NewerVersionExists))
				{
					return Fail(OutError, NativeFailure(TEXT("asset_data_create_class_invalid"),
                        !CreateClass ? TEXT("The creation class could not be loaded.")
                            : CreateClass->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The creation class is abstract.")
                            : CreateClass->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The creation class is deprecated.")
                            : TEXT("The creation class has been superseded."),
                        TEXT("class"), MakeShared<FJsonValueString>(StaticCreateClassPath), MakeShared<FJsonValueString>(TEXT("loaded concrete current class"))));
				}
				OutAction.Kind = EActionKind::CreateObject;
				OutAction.CreateClass = CreateClass;
			}
			else if (BoundNativeMechanicId
					== TEXT(
						"uai.asset_data.mechanic:"
						"create_candidate_object:v1")
				|| BoundNativeMechanicId
					== TEXT(
						"uai.asset_data.mechanic:"
						"create_native_factory_asset:v1"))
			{
				FString FactoryBindingId;
				FString CatalogSourceId;
				FString CatalogGeneration;
				int64 CatalogSequence = -1;
				if (!TryReadString(
						Change.GetAfter(),
						TEXT("factory_binding_id"),
						FactoryBindingId, OutError)
					|| !TryReadString(
						Change.GetAfter(),
						TEXT("factory_catalog_source_id"),
						CatalogSourceId, OutError)
					|| !TryReadString(
						Change.GetAfter(),
						TEXT("factory_catalog_generation"),
						CatalogGeneration, OutError)
					|| !TryReadInteger(
						Change.GetAfter(),
						TEXT("factory_catalog_sequence"),
						CatalogSequence, OutError, TEXT("factory_catalog_sequence"), TEXT("native_result")))
				{
					return false;
				}
				const FUeAgentCatalogRevision ExpectedRevision(
					CatalogSourceId,
					CatalogGeneration,
					CatalogSequence);
				FUeAgentAssetDataSimpleEffectFactoryBinding
					FactoryBinding;
                if (!ExpectedRevision.IsValid())
                {
                    OutError = *ExpectedRevision.GetValidationDiagnostic();
                    MarkNativeFailure(OutError);
                    return false;
                }
                if (!FactoryResolver.TryResolveFrozenBinding(FactoryBindingId, ExpectedRevision,
                    OutAction.ProfileId, FactoryBinding, OutError)) return false;
				UE::UAI::Contract::FDiagnostic FactoryError;
				if (!TryValidateAssetDataSimpleEffectFactoryBinding(
						OutAction.NativeBindingId,
						FactoryBindingId,
						ExpectedRevision,
						FactoryBinding,
						FactoryError))
				{
					return Fail(OutError, FactoryError, OutAction.ProfileId);
				}
				OutAction.Kind = EActionKind::CreateObject;
				OutAction.CreateClass =
					FactoryBinding.GetConcreteClass();
				const bool bNativeFactory =
					FactoryBinding.GetFactoryMechanicId()
						== TEXT(
							"uai.asset_data.factory:"
							"asset_tools_create:v1");
				if (bNativeFactory)
				{
					if (!FactoryBinding.IsGenericReady()
						|| !FactoryBinding.GetFactory())
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_factory_interactive_required"),
                            !FactoryBinding.IsGenericReady() ? TEXT("The selected factory is not ready for noninteractive creation.")
                                : TEXT("The captured native factory object is no longer available."),
                            TEXT("factory"), MakeShared<FJsonValueBoolean>(FactoryBinding.IsGenericReady()),
                            MakeShared<FJsonValueString>(TEXT("ready factory with a live native factory object"))));
					}
					OutAction.CreateFactory =
						FactoryBinding.GetFactory();
				}
			}
			else if (BoundNativeMechanicId
				== TEXT("uai.asset_data.mechanic:clone_object:v1"))
			{
				OutAction.Kind = EActionKind::CloneObject;
				UObject* Source = nullptr;
				if (!TryReadString(
						Change.GetAfter(),
						TEXT("source_ref"),
						OutAction.SourceRef, OutError)
					|| !Resolver.TryResolveCloneSource(
						OutAction.SourceRef,
						Source,
						OutError))
				{
					return false;
				}
				OutAction.Source = Source;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:update_render_target:v1"))
			{
				OutAction.Kind = EActionKind::UpdateRenderTarget;
			}
			else if (IsStructuredAssetMechanicId(
				BoundNativeMechanicId))
			{
				OutAction.Kind = EActionKind::UpdateStructuredAsset;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:update_media_player:v1"))
			{
				OutAction.Kind = EActionKind::UpdateMediaPlayer;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:update_media_texture:v1"))
			{
				OutAction.Kind = EActionKind::UpdateMediaTexture;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_subuv_animation:v1"))
			{
				OutAction.Kind = EActionKind::UpdateSubUVAnimation;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_texture_collection:v1"))
			{
				OutAction.Kind =
					EActionKind::UpdateTextureCollection;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_array:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplaceTextureArray;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_cube_array:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplaceTextureCubeArray;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_cube:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplaceTextureCube;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_volume_texture:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplaceVolumeTexture;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_input_action:v1"))
			{
				OutAction.Kind = EActionKind::UpdateInputAction;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_input_mapping_context:v1"))
			{
				OutAction.Kind =
					EActionKind::UpdateInputMappingContext;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_paper_sprite:v1"))
			{
				OutAction.Kind =
					EActionKind::UpdatePaperSprite;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_flipbook:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplacePaperFlipbook;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_tilemap:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplacePaperTileMap;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_tileset:v1"))
			{
				OutAction.Kind =
					EActionKind::ReplacePaperTileSet;
			}
			else if (IsCoreAssetMechanicId(BoundNativeMechanicId))
			{
				OutAction.Kind = EActionKind::UpdateCoreAsset;
			}
			else
			{
                OutError = NativeFailure(TEXT("asset_data_native_profile_unsupported"),
                    TEXT("The registered asset operation has no supported execution mechanic."), FString(), nullptr, nullptr);
                OutError.ActualEvidence = MakeShared<FJsonValueString>(BoundNativeMechanicId);
                OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("supported asset execution mechanic"));
                OutError.InternalContext = OutAction.ProfileId;
                return false;
			}

			if (OutAction.Kind == EActionKind::CreateObject
				|| OutAction.Kind == EActionKind::CloneObject)
			{
				UPackage* Package = nullptr;
                if (!Resolver.TryResolveCreationTarget(OutAction.TargetRef, Package, OutAction.ObjectName, OutError)) return false;
                if (!Package || OutAction.ObjectName.IsNone() || FindObject<UObject>(Package, *OutAction.ObjectName.ToString()))
                {
                    OutError = NativeFailure(TEXT("asset_data_creation_target_invalid"),
                        !Package ? TEXT("The creation target has no owning package.")
                            : OutAction.ObjectName.IsNone() ? TEXT("The creation target has no object name.")
                            : TEXT("An object already exists at the creation target."),
                        TEXT("target_ref"), MakeShared<FJsonValueString>(OutAction.TargetRef),
                        MakeShared<FJsonValueString>(TEXT("unused object name in a valid package")));
                    OutError.TargetPath = OutAction.TargetRef;
                    return false;
                }
				OutAction.Package = Package;
				OutAction.bPackageWasDirty = Package->IsDirty();
			}
			else
			{
				UObject* Target = nullptr;
                if (!Resolver.TryResolveExisting(OutAction.TargetRef, Target, OutError)) return false;
                if (!IsValid(Target))
                {
                    OutError = NativeFailure(TEXT("asset_data_target_invalid"), TEXT("The resolved target is no longer a valid object."),
                        TEXT("target_ref"), MakeShared<FJsonValueString>(OutAction.TargetRef), MakeShared<FJsonValueString>(TEXT("live asset")));
                    OutError.TargetPath = OutAction.TargetRef;
                    return false;
                }
				OutAction.Target = Target;
				OutAction.Package = Target->GetOutermost();
				OutAction.bPackageWasDirty =
					OutAction.Package.IsValid()
						&& OutAction.Package->IsDirty();
			}

			switch (OutAction.Kind)
			{
			case EActionKind::UpdateRenderTarget:
				{
					UTextureRenderTarget2D* RenderTarget =
						Cast<UTextureRenderTarget2D>(
							OutAction.Target.Get());
					const UTextureRenderTarget2D* Default =
						GetDefault<UTextureRenderTarget2D>();
					if (!RenderTarget || !Default)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_render_target_invalid"),
                            !RenderTarget ? TEXT("The target is not a live TextureRenderTarget2D.") : TEXT("The render target class default object is unavailable."),
                            TEXT("target"), ObjectClassValue(!RenderTarget ? OutAction.Target.Get() : Default),
                            MakeShared<FJsonValueString>(UTextureRenderTarget2D::StaticClass()->GetPathName())));
					}
					OutAction.OriginalSizeX = RenderTarget->SizeX;
					OutAction.OriginalSizeY = RenderTarget->SizeY;
					OutAction.OriginalRenderTargetFormat =
						RenderTarget->RenderTargetFormat;
					OutAction.OriginalClearColor =
						RenderTarget->ClearColor;
					OutAction.bOriginalAutoGenerateMips =
						RenderTarget->bAutoGenerateMips;
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId == TEXT("size"))
						{
							int32 Width = Default->SizeX;
							int32 Height = Default->SizeY;
							if (TypedChange.Presence == TEXT("set"))
							{
								const FUeAgentCanonicalValue* WidthValue =
									TypedChange.CanonicalValue.IsValid()
										? FindObjectField(
											*TypedChange.CanonicalValue,
											TEXT("x"))
										: nullptr;
								const FUeAgentCanonicalValue* HeightValue =
									TypedChange.CanonicalValue.IsValid()
										? FindObjectField(
											*TypedChange.CanonicalValue,
											TEXT("y"))
										: nullptr;
								int32 WidthNumber = 0;
								int32 HeightNumber = 0;
                                if (!WidthValue || !HeightValue)
                                    return Fail(OutError, ValueFailure(TEXT("asset_data_render_target_size_invalid"),
                                        TEXT("The render target size requires both dimensions."), TypedChange.FieldId + (!WidthValue ? TEXT("/x") : TEXT("/y")),
                                        nullptr, TEXT("positive integer dimension")));
                                if (!TryReadIntegralPixel(WidthValue, WidthNumber, TypedChange.FieldId + TEXT("/x"), OutError)
                                    || !TryReadIntegralPixel(HeightValue, HeightNumber, TypedChange.FieldId + TEXT("/y"), OutError)) return false;
                                if (WidthNumber < 1 || HeightNumber < 1 || WidthNumber > GTextureRenderTarget2DMaxSizeX || HeightNumber > GTextureRenderTarget2DMaxSizeY)
                                {
                                    const bool bX = WidthNumber < 1 || (HeightNumber >= 1 && WidthNumber > GTextureRenderTarget2DMaxSizeX);
                                    return Fail(OutError, ValueFailure(TEXT("asset_data_render_target_size_invalid"),
                                        TEXT("The render target dimension is outside the supported range."), TypedChange.FieldId + (bX ? TEXT("/x") : TEXT("/y")),
                                        bX ? WidthValue : HeightValue, FString::Printf(TEXT("integer in [1, %d]"), bX ? GTextureRenderTarget2DMaxSizeX : GTextureRenderTarget2DMaxSizeY)));
                                }
								Width = static_cast<int32>(WidthNumber);
								Height = static_cast<int32>(HeightNumber);
							}
							if (Width < 1 || Height < 1)
							{
								return Fail(OutError, NativeFailure(TEXT("asset_data_render_target_default_size_invalid"),
                                    TEXT("The effective default render target dimension is not positive."),
                                    Width < 1 ? TEXT("size/x") : TEXT("size/y"), MakeShared<FJsonValueNumber>(Width < 1 ? Width : Height),
                                    MakeShared<FJsonValueString>(TEXT("integer >= 1"))));
							}
							OutAction.bChangeRenderTargetSize = true;
							OutAction.NewSizeX = Width;
							OutAction.NewSizeY = Height;
						}
						else if (TypedChange.FieldId
							== TEXT("format"))
						{
							ETextureRenderTargetFormat Format =
								Default->RenderTargetFormat;
                            if (TypedChange.Presence == TEXT("set")
                                && !TryReadRenderTargetFormat(TypedChange.CanonicalValue.Get(), Format, TypedChange.FieldId, OutError)) return false;
							OutAction.bChangeRenderTargetFormat = true;
							OutAction.NewRenderTargetFormat = Format;
						}
						else if (TypedChange.FieldId
							== TEXT("clear_color"))
						{
							FLinearColor Color = Default->ClearColor;
                            if (TypedChange.Presence == TEXT("set")
                                && !TryReadLinearColor(TypedChange.CanonicalValue.Get(), Color, TypedChange.FieldId, OutError)) return false;
							OutAction.bChangeRenderTargetClearColor =
								true;
							OutAction.NewRenderTargetClearColor = Color;
						}
						else if (TypedChange.FieldId
							== TEXT("auto_generate_mips"))
						{
							bool bAutoGenerateMips =
								Default->bAutoGenerateMips;
                            if (TypedChange.Presence == TEXT("set")
                                && !TryReadBoolean(TypedChange.CanonicalValue.Get(), bAutoGenerateMips, TypedChange.FieldId, OutError)) return false;
							OutAction.
								bChangeRenderTargetAutoGenerateMips =
									true;
							OutAction.bNewAutoGenerateMips =
								bAutoGenerateMips;
						}
					}
					break;
				}
			case EActionKind::UpdateStructuredAsset:
				{
					return TryPrepareStructuredAssetUpdate(
						Resolver,
						OutAction.NativeMechanicId,
						OutAction.Target.Get(),
						MakeStructuredAssetChangeViews(OutAction),
						OutAction.StructuredAssetState,
						OutError);
				}
			case EActionKind::UpdateMediaPlayer:
				{
					UMediaPlayer* MediaPlayer =
						Cast<UMediaPlayer>(OutAction.Target.Get());
					if (!MediaPlayer)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_media_player_invalid"), TEXT("The target is not a live MediaPlayer."), TEXT("target"), ObjectClassValue(OutAction.Target.Get()), MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName())));
					}
					OutAction.bOriginalMediaPlayerPlayOnOpen =
						MediaPlayer->PlayOnOpen;
					OutAction.bOriginalMediaPlayerLoop =
						MediaPlayer->IsLooping();
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						bool bValue = false;
                        if (!TryReadBoolean(TypedChange.CanonicalValue.Get(), bValue, TypedChange.FieldId, OutError)) return false;
						if (TypedChange.FieldId
							== TEXT("play_on_open"))
						{
							OutAction.
								bChangeMediaPlayerPlayOnOpen = true;
							OutAction.bNewMediaPlayerPlayOnOpen =
								bValue;
						}
						else if (TypedChange.FieldId == TEXT("loop"))
						{
							OutAction.bChangeMediaPlayerLoop = true;
							OutAction.bNewMediaPlayerLoop = bValue;
						}
					}
					break;
				}
			case EActionKind::UpdateMediaTexture:
				{
					UMediaTexture* MediaTexture =
						Cast<UMediaTexture>(OutAction.Target.Get());
					const UMediaTexture* Default =
						GetDefault<UMediaTexture>();
					if (!MediaTexture || !Default)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_media_texture_invalid"),
                            !MediaTexture ? TEXT("The target is not a live MediaTexture.") : TEXT("The MediaTexture class default object is unavailable."),
                            TEXT("target"), ObjectClassValue(!MediaTexture ? OutAction.Target.Get() : Default),
                            MakeShared<FJsonValueString>(UMediaTexture::StaticClass()->GetPathName())));
					}
					OutAction.OriginalMediaTexturePlayer =
						MediaTexture->GetMediaPlayer();
					OutAction.bOriginalMediaTextureAutoClear =
						MediaTexture->AutoClear;
					OutAction.OriginalMediaTextureClearColor =
						MediaTexture->ClearColor;
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId
							== TEXT("media_player_ref"))
						{
							UMediaPlayer* NewMediaPlayer = nullptr;
							if (TypedChange.Presence == TEXT("set"))
							{
								UObject* Resolved = nullptr;
                                if (!TypedChange.CanonicalValue.IsValid())
                                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_texture_player_invalid"),
                                        TEXT("The media player reference is missing."), TypedChange.FieldId, nullptr, TEXT("MediaPlayer reference")));
                                if (!Resolver.TryResolveExisting(TypedChange.CanonicalValue->GetText(), Resolved, OutError))
                                    return WithInputField(OutError, TypedChange.FieldId);
								NewMediaPlayer =
									Cast<UMediaPlayer>(Resolved);
								if (!NewMediaPlayer)
								{
									OutError = ValueFailure(TEXT("asset_data_media_texture_player_invalid"),
                                        TEXT("The referenced object is not a MediaPlayer."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("MediaPlayer reference"));
                                    OutError.ActualValue = NativeFacts({{TEXT("reference"), MakeShared<FJsonValueString>(TypedChange.CanonicalValue->GetText())},
                                        {TEXT("class"), ObjectClassValue(Resolved)}});
                                    return false;
								}
							}
							OutAction.bChangeMediaTexturePlayer = true;
							OutAction.NewMediaTexturePlayer =
								NewMediaPlayer;
						}
						else if (TypedChange.FieldId
							== TEXT("auto_clear"))
						{
							bool bAutoClear = Default->AutoClear;
                            if (TypedChange.Presence == TEXT("set")
                                && !TryReadBoolean(TypedChange.CanonicalValue.Get(), bAutoClear, TypedChange.FieldId, OutError)) return false;
							OutAction.bChangeMediaTextureAutoClear =
								true;
							OutAction.bNewMediaTextureAutoClear =
								bAutoClear;
						}
						else if (TypedChange.FieldId
							== TEXT("clear_color"))
						{
							FLinearColor Color = Default->ClearColor;
                            if (TypedChange.Presence == TEXT("set")
                                && !TryReadLinearColor(TypedChange.CanonicalValue.Get(), Color, TypedChange.FieldId, OutError)) return false;
							OutAction.bChangeMediaTextureClearColor =
								true;
							OutAction.NewMediaTextureClearColor =
								Color;
						}
					}
					break;
				}
			case EActionKind::UpdateSubUVAnimation:
				{
					USubUVAnimation* SubUV =
						Cast<USubUVAnimation>(
							OutAction.Target.Get());
					const USubUVAnimation* Default =
						GetDefault<USubUVAnimation>();
					if (!SubUV || !Default)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_subuv_invalid"),
                            !SubUV ? TEXT("The target is not a live SubUVAnimation.") : TEXT("The SubUVAnimation class default object is unavailable."),
                            TEXT("target"), ObjectClassValue(!SubUV ? OutAction.Target.Get() : Default),
                            MakeShared<FJsonValueString>(USubUVAnimation::StaticClass()->GetPathName())));
					}
					OutAction.OriginalSubUVTexture =
						SubUV->SubUVTexture;
					OutAction.OriginalSubUVHorizontal =
						SubUV->SubImages_Horizontal;
					OutAction.OriginalSubUVVertical =
						SubUV->SubImages_Vertical;
					OutAction.OriginalSubUVBoundingMode =
						SubUV->BoundingMode;
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId
							== TEXT("source_texture_ref"))
						{
							UTexture2D* Texture =
								Default->SubUVTexture;
							if (TypedChange.Presence
								== TEXT("set"))
							{
								UObject* Resolved = nullptr;
                                if (!TypedChange.CanonicalValue.IsValid())
                                    return Fail(OutError, ValueFailure(TEXT("asset_data_subuv_texture_invalid"),
                                        TEXT("The source texture reference is missing."), TypedChange.FieldId, nullptr, TEXT("Texture2D reference")));
                                if (!Resolver.TryResolveExisting(TypedChange.CanonicalValue->GetText(), Resolved, OutError))
                                    return WithInputField(OutError, TypedChange.FieldId);
								Texture = Cast<UTexture2D>(Resolved);
								if (!Texture)
								{
									OutError = ValueFailure(TEXT("asset_data_subuv_texture_invalid"), TEXT("The referenced object is not a Texture2D."),
                                        TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("Texture2D reference"));
                                    OutError.ActualValue = NativeFacts({{TEXT("reference"), MakeShared<FJsonValueString>(TypedChange.CanonicalValue->GetText())},
                                        {TEXT("class"), ObjectClassValue(Resolved)}});
                                    return false;
								}
							}
							OutAction.bChangeSubUVTexture = true;
							OutAction.NewSubUVTexture = Texture;
						}
						else if (TypedChange.FieldId
							== TEXT("grid"))
						{
							int64 Horizontal =
								Default->SubImages_Horizontal;
							int64 Vertical =
								Default->SubImages_Vertical;
                            if (TypedChange.Presence == TEXT("set")
                                && (!TryReadInteger(TypedChange.CanonicalValue, TEXT("x"), Horizontal, OutError, TypedChange.FieldId + TEXT("/x"), TEXT("authored_value"))
                                    || !TryReadInteger(TypedChange.CanonicalValue, TEXT("y"), Vertical, OutError, TypedChange.FieldId + TEXT("/y"), TEXT("authored_value")))) return false;
							if (Horizontal < 1
								|| Vertical < 1
								|| Horizontal > MAX_int32
								|| Vertical > MAX_int32
								|| Horizontal * Vertical
									> MAX_int32 / 8)
							{
								const bool bX = Horizontal < 1 || (Vertical >= 1 && Horizontal > MAX_int32);
                                const bool bY = !bX && (Vertical < 1 || Vertical > MAX_int32);
                                OutError = NativeFailure(TEXT("asset_data_subuv_grid_invalid"),
                                    bX || bY ? TEXT("A SubUV grid dimension is outside the supported range.")
                                        : TEXT("The SubUV grid cell count exceeds the supported capacity."),
                                    bX ? TEXT("grid/x") : bY ? TEXT("grid/y") : TEXT("grid"),
                                    bX || bY ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumberString>(::LexToString(bX ? Horizontal : Vertical)))
                                        : NativeFacts({{TEXT("x"), MakeShared<FJsonValueNumberString>(::LexToString(Horizontal))},
                                            {TEXT("y"), MakeShared<FJsonValueNumberString>(::LexToString(Vertical))}}),
                                    MakeShared<FJsonValueString>(bX || bY ? TEXT("integer in [1, 2147483647]") : TEXT("x * y <= 268435455")));
                                if (TypedChange.Presence == TEXT("set")) OutError.SourceKind = TEXT("authored_value");
                                return false;
							}
							OutAction.bChangeSubUVGrid = true;
							OutAction.NewSubUVHorizontal =
								static_cast<int32>(Horizontal);
							OutAction.NewSubUVVertical =
								static_cast<int32>(Vertical);
						}
						else if (TypedChange.FieldId
							== TEXT("bounding_geometry"))
						{
							TEnumAsByte<
								ESubUVBoundingVertexCount> Mode =
									Default->BoundingMode;
							if (TypedChange.Presence
								== TEXT("set"))
							{
								if (!TypedChange.CanonicalValue.IsValid()
									|| TypedChange.CanonicalValue
											->GetKind()
										!= EUeAgentValueKind::String)
								{
									return Fail(OutError, ValueFailure(TEXT("asset_data_subuv_bounding_geometry_invalid"), TEXT("SubUV bounding geometry must be a supported name."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("four_vertices or eight_vertices")));
								}
								const FString Name =
									TypedChange.CanonicalValue
										->GetText()
										.TrimStartAndEnd()
										.ToLower();
								if (Name == TEXT("four_vertices"))
								{
									Mode = BVC_FourVertices;
								}
								else if (
									Name == TEXT("eight_vertices"))
								{
									Mode = BVC_EightVertices;
								}
								else
								{
									return Fail(OutError, ValueFailure(TEXT("asset_data_subuv_bounding_geometry_invalid"), TEXT("The SubUV bounding geometry name is unsupported."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("four_vertices or eight_vertices")));
								}
							}
							OutAction.bChangeSubUVBoundingMode = true;
							OutAction.NewSubUVBoundingMode = Mode;
						}
					}
					const int32 EffectiveHorizontal =
						OutAction.bChangeSubUVGrid
							? OutAction.NewSubUVHorizontal
							: SubUV->SubImages_Horizontal;
					const int32 EffectiveVertical =
						OutAction.bChangeSubUVGrid
							? OutAction.NewSubUVVertical
							: SubUV->SubImages_Vertical;
					if (EffectiveHorizontal < 1
						|| EffectiveVertical < 1
						|| static_cast<int64>(EffectiveHorizontal)
							* EffectiveVertical
							> MAX_int32 / 8)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_subuv_grid_invalid"), TEXT("The effective SubUV grid has a nonpositive dimension or exceeds the supported cell capacity."), TEXT("grid"), NativeFacts({{TEXT("x"), MakeShared<FJsonValueNumber>(EffectiveHorizontal)}, {TEXT("y"), MakeShared<FJsonValueNumber>(EffectiveVertical)}}), MakeShared<FJsonValueString>(TEXT("x >= 1, y >= 1, x * y <= 268435455"))));
					}
					UTexture2D* EffectiveTexture =
						OutAction.bChangeSubUVTexture
							? OutAction.NewSubUVTexture.Get()
							: SubUV->SubUVTexture.Get();
					if (EffectiveTexture
						&& EffectiveTexture->Source.IsValid()
						&& (EffectiveHorizontal
								> EffectiveTexture->Source.GetSizeX()
							|| EffectiveVertical
								> EffectiveTexture->Source.GetSizeY()))
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_subuv_grid_exceeds_texture"),
                            TEXT("The effective SubUV grid has more cells along an axis than the source texture has pixels."),
                            TEXT("grid"), NativeFacts({{TEXT("x"), MakeShared<FJsonValueNumber>(EffectiveHorizontal)}, {TEXT("y"), MakeShared<FJsonValueNumber>(EffectiveVertical)}}),
                            NativeFacts({{TEXT("max_x"), MakeShared<FJsonValueNumber>(EffectiveTexture->Source.GetSizeX())},
                                {TEXT("max_y"), MakeShared<FJsonValueNumber>(EffectiveTexture->Source.GetSizeY())}})));
					}
					break;
				}
			case EActionKind::UpdateTextureCollection:
				{
					UTextureCollection* Collection =
						Cast<UTextureCollection>(
							OutAction.Target.Get());
					if (!Collection)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_texture_collection_invalid"),
                            TEXT("The target is not a live TextureCollection."), TEXT("target"), ObjectClassValue(OutAction.Target.Get()),
                            MakeShared<FJsonValueString>(UTextureCollection::StaticClass()->GetPathName())));
					}
					OutAction.OriginalTextureCollectionTextures
						.Reserve(Collection->Textures.Num());
					for (UTexture* Texture : Collection->Textures)
					{
						if (!Texture)
						{
							return Fail(OutError, NativeFailure(TEXT("asset_data_texture_collection_existing_texture_invalid"),
                                TEXT("The current texture collection contains a null texture."),
                                TEXT("texture_refs/") + ::LexToString(OutAction.OriginalTextureCollectionTextures.Num()),
                                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live Texture"))));
						}
						OutAction.OriginalTextureCollectionTextures
							.Add(Texture);
					}
					OutAction.NewTextureCollectionTextures =
						OutAction.OriginalTextureCollectionTextures;
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId
							!= TEXT("texture_refs"))
						{
							continue;
						}
						OutAction.bChangeTextureCollectionTextures =
							true;
						if (TypedChange.Presence
							== TEXT("clear"))
						{
							OutAction.NewTextureCollectionTextures
								.Reset();
							continue;
						}
                        TArray<TWeakObjectPtr<UTexture>> Requested;
                        if (!TryResolveUniqueTextureArray(Resolver, TypedChange.CanonicalValue.Get(), Requested, OutError, TypedChange.FieldId)) return false;
						if (TypedChange.Presence
							== TEXT("set"))
						{
							for (const TWeakObjectPtr<UTexture>&
								Texture : Requested)
							{
								if (!OutAction
									.NewTextureCollectionTextures
									.Contains(Texture))
								{
									OutAction
										.NewTextureCollectionTextures
										.Add(Texture);
								}
							}
						}
						else if (TypedChange.Presence
							== TEXT("remove"))
						{
							OutAction.NewTextureCollectionTextures
								.RemoveAll(
									[&Requested](
										const TWeakObjectPtr<UTexture>&
											Texture)
									{
										return Requested.Contains(
											Texture);
									});
						}
						else if (TypedChange.Presence
							== TEXT("replace"))
						{
							OutAction.NewTextureCollectionTextures =
								MoveTemp(Requested);
						}
					}
					if (!OutAction
						.bChangeTextureCollectionTextures)
					{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_changes_missing"),
                            TEXT("The texture collection update does not contain a texture_refs change."),
                            TEXT("texture_refs"), nullptr, TEXT("texture_refs change")));
					}
					break;
				}
			case EActionKind::ReplaceTextureArray:
				{
					return TryPrepareTexture2DArrayReplacement(
						Resolver,
						OutAction.Target.Get(),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("source_texture_refs")),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("format_settings")),
						OutAction.TextureSourceState,
						OutError);
				}
			case EActionKind::ReplaceTextureCubeArray:
				{
					return TryPrepareTextureCubeArrayReplacement(
						Resolver,
						OutAction.Target.Get(),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("source_cube_refs")),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("format_settings")),
						OutAction.TextureSourceState,
						OutError);
				}
			case EActionKind::ReplaceTextureCube:
				{
					return TryPrepareTextureCubeReplacement(
						Resolver,
						OutAction.Target.Get(),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("face_texture_refs")),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("format_settings")),
						OutAction.TextureSourceState,
						OutError);
				}
			case EActionKind::ReplaceVolumeTexture:
				{
					return TryPrepareVolumeTextureReplacement(
						Resolver,
						OutAction.Target.Get(),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("slice_texture_refs")),
						MakeTextureSourceChangeView(
							OutAction,
							TEXT("format_settings")),
						OutAction.TextureSourceState,
						OutError);
				}
			case EActionKind::UpdateInputAction:
				{
					return TryPrepareEnhancedInputActionUpdate(
						OutAction.Target.Get(),
						MakeEnhancedInputChangeView(
							OutAction,
							TEXT("value_type")),
						MakeEnhancedInputChangeView(
							OutAction,
							TEXT("consume_input")),
						MakeEnhancedInputChangeView(
							OutAction,
							TEXT("triggers")),
						MakeEnhancedInputChangeView(
							OutAction,
							TEXT("modifiers")),
						OutAction.InputActionState,
						OutError);
				}
			case EActionKind::UpdateInputMappingContext:
				{
					return
						TryPrepareEnhancedInputMappingContextUpdate(
							Resolver,
							OutAction.Target.Get(),
							MakeEnhancedInputChangeView(
								OutAction,
								TEXT("mappings")),
							OutAction.InputMappingContextState,
							OutError);
				}
			case EActionKind::UpdatePaperSprite:
				{
					UPaperSprite* Sprite =
						Cast<UPaperSprite>(
							OutAction.Target.Get());
					const UPaperSprite* Default =
						GetDefault<UPaperSprite>();
					if (!Sprite || !Default)
					{
                        return Fail(OutError, NativeFailure(TEXT("asset_data_paper_sprite_invalid"),
                            !Sprite ? TEXT("The target is not a live PaperSprite.") : TEXT("The PaperSprite class default object is unavailable."),
                            TEXT("target"), ObjectClassValue(!Sprite ? OutAction.Target.Get() : Default),
                            MakeShared<FJsonValueString>(UPaperSprite::StaticClass()->GetPathName())));
					}

					const FVector2D OriginalUV =
						Sprite->GetSourceUV();
					const FVector2D OriginalSize =
						Sprite->GetSourceSize();
					if (!FMath::IsFinite(OriginalUV.X)
						|| !FMath::IsFinite(OriginalUV.Y)
						|| !FMath::IsFinite(OriginalSize.X)
						|| !FMath::IsFinite(OriginalSize.Y)
						|| FMath::FloorToDouble(OriginalUV.X)
							!= OriginalUV.X
						|| FMath::FloorToDouble(OriginalUV.Y)
							!= OriginalUV.Y
						|| FMath::FloorToDouble(OriginalSize.X)
							!= OriginalSize.X
						|| FMath::FloorToDouble(OriginalSize.Y)
							!= OriginalSize.Y
						|| OriginalUV.X < MIN_int32
						|| OriginalUV.X > MAX_int32
						|| OriginalUV.Y < MIN_int32
						|| OriginalUV.Y > MAX_int32
						|| OriginalSize.X < MIN_int32
						|| OriginalSize.X > MAX_int32
						|| OriginalSize.Y < MIN_int32
						|| OriginalSize.Y > MAX_int32)
					{
                        const double Values[] = {OriginalUV.X, OriginalUV.Y, OriginalSize.X, OriginalSize.Y};
                        const TCHAR* Fields[] = {TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height")};
                        int32 Invalid = INDEX_NONE;
                        for (int32 Index = 0; Index < 4; ++Index)
                            if (!FMath::IsFinite(Values[Index])) { Invalid = Index; break; }
                        const TCHAR* Reason = TEXT("The current sprite region component is not finite.");
                        if (Invalid == INDEX_NONE)
                        {
                            Reason = TEXT("The current sprite region component is not an integer pixel value.");
                            for (int32 Index = 0; Index < 4; ++Index)
                                if (FMath::FloorToDouble(Values[Index]) != Values[Index]) { Invalid = Index; break; }
                        }
                        if (Invalid == INDEX_NONE)
                        {
                            Reason = TEXT("The current sprite region component is outside int32 range.");
                            for (int32 Index = 0; Index < 4; ++Index)
                                if (Values[Index] < MIN_int32 || Values[Index] > MAX_int32) { Invalid = Index; break; }
                        }
                        return Fail(OutError, NativeFailure(TEXT("asset_data_paper_sprite_existing_region_invalid"), Reason,
                            ChildPath(TEXT("source_region"), Fields[Invalid]), NativeNumberFact(Values[Invalid]),
                            MakeShared<FJsonValueString>(TEXT("finite int32 pixel value"))));
					}
					OutAction.OriginalPaperSpriteSourceTexture =
						Sprite->GetSourceTexture();
					OutAction.OriginalPaperSpriteOffset =
						FIntPoint(
							static_cast<int32>(OriginalUV.X),
							static_cast<int32>(OriginalUV.Y));
					OutAction.OriginalPaperSpriteDimension =
						FIntPoint(
							static_cast<int32>(OriginalSize.X),
							static_cast<int32>(OriginalSize.Y));
					OutAction.OriginalPaperSpritePivotMode =
						Sprite->GetPivotMode(
							OutAction
								.OriginalPaperSpriteCustomPivot);
					FAdditionalSpriteTextureArray
						AdditionalTextures;
					Sprite->GetBakedAdditionalSourceTextures(
						AdditionalTextures);
					for (UTexture* Texture : AdditionalTextures)
					{
						OutAction
							.OriginalPaperSpriteAdditionalTextures
							.Add(Texture);
					}

					OutAction.NewPaperSpriteSourceTexture =
						OutAction
							.OriginalPaperSpriteSourceTexture;
					OutAction.NewPaperSpriteOffset =
						OutAction.OriginalPaperSpriteOffset;
					OutAction.NewPaperSpriteDimension =
						OutAction.OriginalPaperSpriteDimension;
					OutAction.NewPaperSpritePivotMode =
						OutAction.OriginalPaperSpritePivotMode;
					OutAction.NewPaperSpriteCustomPivot =
						OutAction
							.OriginalPaperSpriteCustomPivot;
					OutAction.NewPaperSpriteAdditionalTextures =
						OutAction
							.OriginalPaperSpriteAdditionalTextures;

					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId
							== TEXT("source_texture_ref"))
						{
							UTexture2D* Texture = nullptr;
							if (TypedChange.Presence
								== TEXT("set"))
							{
								UObject* Resolved = nullptr;
                                if (!TypedChange.CanonicalValue.IsValid() || TypedChange.CanonicalValue->GetKind() != EUeAgentValueKind::String)
                                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_sprite_source_texture_invalid"),
                                        TEXT("The source texture reference must be a string."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("Texture2D reference")));
                                if (!Resolver.TryResolveExisting(TypedChange.CanonicalValue->GetText(), Resolved, OutError))
                                    return WithInputField(OutError, TypedChange.FieldId);
								Texture = Cast<UTexture2D>(Resolved);
								if (!Texture)
								{
									OutError = ValueFailure(TEXT("asset_data_paper_sprite_source_texture_invalid"),
                                        TEXT("The referenced object is not a Texture2D."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("Texture2D reference"));
                                    OutError.ActualValue = NativeFacts({{TEXT("reference"), MakeShared<FJsonValueString>(TypedChange.CanonicalValue->GetText())},
                                        {TEXT("class"), ObjectClassValue(Resolved)}});
                                    return false;
								}
							}
							else if (TypedChange.Presence
								== TEXT("clear"))
							{
								Texture =
									Default->GetSourceTexture();
							}
							OutAction.NewPaperSpriteSourceTexture =
								Texture;
						}
						else if (TypedChange.FieldId
							== TEXT("source_region"))
						{
							if (TypedChange.Presence
								== TEXT("clear"))
							{
								const FVector2D DefaultUV =
									Default->GetSourceUV();
								const FVector2D DefaultSize =
									Default->GetSourceSize();
								OutAction.NewPaperSpriteOffset =
									FIntPoint(
										static_cast<int32>(
											DefaultUV.X),
										static_cast<int32>(
											DefaultUV.Y));
								OutAction.NewPaperSpriteDimension =
									FIntPoint(
										static_cast<int32>(
											DefaultSize.X),
										static_cast<int32>(
											DefaultSize.Y));
							}
                            else if (!TryReadPaperSpriteRegion(TypedChange.CanonicalValue.Get(), OutAction.NewPaperSpriteOffset,
                                OutAction.NewPaperSpriteDimension, TypedChange.FieldId, OutError)) return false;
						}
						else if (TypedChange.FieldId
							== TEXT("pivot"))
						{
							if (TypedChange.Presence
								== TEXT("clear"))
							{
								OutAction.NewPaperSpritePivotMode =
									Default->GetPivotMode(
										OutAction
											.NewPaperSpriteCustomPivot);
							}
                            else if (!TryReadPaperSpritePivot(TypedChange.CanonicalValue.Get(),
                                OutAction.NewPaperSpriteCustomPivot, TypedChange.FieldId, OutError)) return false;
							else
							{
								OutAction.NewPaperSpritePivotMode =
									ESpritePivotMode::Custom;
							}
						}
					}

					const FIntPoint& EffectiveOffset =
						OutAction.NewPaperSpriteOffset;
					const FIntPoint& EffectiveDimension =
						OutAction.NewPaperSpriteDimension;
					const bool bZeroRegion =
						EffectiveDimension == FIntPoint::ZeroValue;
					if (EffectiveOffset.X < 0
						|| EffectiveOffset.Y < 0
						|| EffectiveDimension.X < 0
						|| EffectiveDimension.Y < 0
						|| ((EffectiveDimension.X == 0)
							!= (EffectiveDimension.Y == 0)))
					{
						const TCHAR* Invalid = EffectiveOffset.X < 0 ? TEXT("x") : EffectiveOffset.Y < 0 ? TEXT("y")
                            : EffectiveDimension.X < 0 ? TEXT("width") : EffectiveDimension.Y < 0 ? TEXT("height") : nullptr;
                        return Fail(OutError, NativeFailure(TEXT("asset_data_paper_sprite_effective_region_invalid"),
                            Invalid ? TEXT("The effective sprite region has a negative component.")
                                : TEXT("A zero-size sprite region requires both dimensions to be zero."),
                            Invalid ? ChildPath(TEXT("source_region"), Invalid) : TEXT("source_region"),
                            NativeFacts({{TEXT("x"), MakeShared<FJsonValueNumber>(EffectiveOffset.X)}, {TEXT("y"), MakeShared<FJsonValueNumber>(EffectiveOffset.Y)},
                                {TEXT("width"), MakeShared<FJsonValueNumber>(EffectiveDimension.X)}, {TEXT("height"), MakeShared<FJsonValueNumber>(EffectiveDimension.Y)}}),
                            MakeShared<FJsonValueString>(TEXT("nonnegative offsets; dimensions both zero or both positive"))));
					}
					UTexture2D* EffectiveTexture =
						OutAction.NewPaperSpriteSourceTexture.Get();
					if (EffectiveTexture && !bZeroRegion)
					{
						const FIntPoint ImportedSize =
							EffectiveTexture->GetImportedSize();
						if (ImportedSize.X <= 0
							|| ImportedSize.Y <= 0
							|| static_cast<int64>(
								EffectiveOffset.X)
								+ EffectiveDimension.X
								> ImportedSize.X
							|| static_cast<int64>(
								EffectiveOffset.Y)
								+ EffectiveDimension.Y
								> ImportedSize.Y)
						{
							const bool bInvalidTextureSize = ImportedSize.X <= 0 || ImportedSize.Y <= 0;
                            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_sprite_region_exceeds_texture"),
                                bInvalidTextureSize ? TEXT("The source texture has an invalid imported size.") : TEXT("The sprite region extends beyond the imported source texture."),
                                bInvalidTextureSize ? TEXT("source_texture_ref") : TEXT("source_region"),
                                bInvalidTextureSize ? NativeFacts({{TEXT("x"), MakeShared<FJsonValueNumber>(ImportedSize.X)}, {TEXT("y"), MakeShared<FJsonValueNumber>(ImportedSize.Y)}})
                                    : NativeFacts({{TEXT("end_x"), MakeShared<FJsonValueNumber>(static_cast<int64>(EffectiveOffset.X) + EffectiveDimension.X)},
                                        {TEXT("end_y"), MakeShared<FJsonValueNumber>(static_cast<int64>(EffectiveOffset.Y) + EffectiveDimension.Y)}}),
                                bInvalidTextureSize ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("positive imported texture size")))
                                    : NativeFacts({{TEXT("max_x"), MakeShared<FJsonValueNumber>(ImportedSize.X)}, {TEXT("max_y"), MakeShared<FJsonValueNumber>(ImportedSize.Y)}})));
						}
					}
					break;
				}
			case EActionKind::ReplacePaperFlipbook:
				{
					UPaperFlipbook* Flipbook =
						Cast<UPaperFlipbook>(
							OutAction.Target.Get());
					if (!Flipbook)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_paper_flipbook_invalid"),
                            TEXT("The target is not a live PaperFlipbook."), TEXT("target"), ObjectClassValue(OutAction.Target.Get()),
                            MakeShared<FJsonValueString>(UPaperFlipbook::StaticClass()->GetPathName())));
					}
					OutAction
						.OriginalPaperFlipbookFramesPerSecond =
							Flipbook->GetFramesPerSecond();
					if (!FMath::IsFinite(
							OutAction
								.OriginalPaperFlipbookFramesPerSecond)
						|| OutAction
								.OriginalPaperFlipbookFramesPerSecond
							< 0.0f)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_paper_flipbook_existing_fps_invalid"),
                            TEXT("The current flipbook frame rate is nonfinite or negative."), TEXT("frames_per_second"),
                            NativeNumberFact(OutAction.OriginalPaperFlipbookFramesPerSecond), MakeShared<FJsonValueString>(TEXT("finite number >= 0"))));
					}
					int64 OriginalTotalFrames = 0;
					OutAction.OriginalPaperFlipbookFrames.Reserve(
						Flipbook->GetNumKeyFrames());
					for (int32 Index = 0;
						Index < Flipbook->GetNumKeyFrames();
						++Index)
					{
						const FPaperFlipbookKeyFrame& KeyFrame =
							Flipbook->GetKeyFrameChecked(Index);
						if (KeyFrame.FrameRun < 1)
						{
							return Fail(OutError, NativeFailure(TEXT("asset_data_paper_flipbook_existing_frame_invalid"),
                                TEXT("The current flipbook frame run is not positive."), TEXT("frames/") + ::LexToString(Index) + TEXT("/frame_run"),
                                MakeShared<FJsonValueNumber>(KeyFrame.FrameRun), MakeShared<FJsonValueString>(TEXT("integer >= 1"))));
						}
						OriginalTotalFrames += KeyFrame.FrameRun;
						if (OriginalTotalFrames > MAX_int32)
						{
							return Fail(OutError, NativeFailure(TEXT("asset_data_paper_flipbook_existing_total_overflow"),
                                TEXT("The current flipbook total frame run exceeds int32 capacity."), TEXT("frames/") + ::LexToString(Index) + TEXT("/frame_run"),
                                MakeShared<FJsonValueNumberString>(::LexToString(OriginalTotalFrames)), MakeShared<FJsonValueNumber>(MAX_int32)));
						}
						FPaperFlipbookFrameState Frame;
						Frame.Sprite = KeyFrame.Sprite;
						Frame.FrameRun = KeyFrame.FrameRun;
						OutAction.OriginalPaperFlipbookFrames.Add(
							MoveTemp(Frame));
					}
					OutAction.NewPaperFlipbookFrames =
						OutAction.OriginalPaperFlipbookFrames;
					OutAction.NewPaperFlipbookFramesPerSecond =
						OutAction
							.OriginalPaperFlipbookFramesPerSecond;
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						if (TypedChange.FieldId == TEXT("frames"))
						{
                            if (!TryReadPaperFlipbookFrames(Resolver, TypedChange.CanonicalValue.Get(),
                                OutAction.NewPaperFlipbookFrames, OutError, TypedChange.FieldId)) return false;
						}
						else if (TypedChange.FieldId
							== TEXT("frames_per_second"))
						{
							double FramesPerSecond = 0.0;
                            if (!TryReadNumber(TypedChange.CanonicalValue.Get(), FramesPerSecond, TypedChange.FieldId, OutError)) return false;
                            if (!FMath::IsFinite(FramesPerSecond) || FramesPerSecond < 0.0)
                                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_fps_invalid"),
                                    TEXT("The flipbook frame rate must be nonnegative and finite."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("finite number >= 0")));
							const float NativeFramesPerSecond =
								static_cast<float>(
									FramesPerSecond);
							if (!FMath::IsFinite(
								NativeFramesPerSecond))
							{
								return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_fps_invalid"),
                                    TEXT("The frame rate cannot be represented as a finite float."), TypedChange.FieldId, TypedChange.CanonicalValue.Get(), TEXT("finite float >= 0")));
							}
							OutAction
								.NewPaperFlipbookFramesPerSecond =
									NativeFramesPerSecond;
						}
					}
					break;
				}
			case EActionKind::ReplacePaperTileMap:
				{
					return TryPreparePaperTileMapReplacement(
						Resolver,
						OutAction.Target.Get(),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("map_size")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("tile_size")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("layers")),
						OutAction.PaperTileMapState,
						OutError);
				}
			case EActionKind::ReplacePaperTileSet:
				{
					return TryPreparePaperTileSetReplacement(
						Resolver,
						OutAction.Target.Get(),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("source_texture_ref")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("tile_size")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("margin")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("spacing")),
						FindCanonicalChangeValue(
							OutAction,
							TEXT("terrain")),
						OutAction.PaperTileSetState,
						OutError);
				}
			case EActionKind::UpdateCoreAsset:
				{
					return TryPrepareCoreAssetUpdate(
						Resolver,
						OutAction.NativeMechanicId,
						OutAction.Target.Get(),
						MakeCoreAssetChangeViews(OutAction),
						OutAction.CoreAssetState,
						OutError);
				}
			default:
				break;
			}
			return true;
		}

		bool TryWritePaperSpriteState(
			UObject* Target,
			const TWeakObjectPtr<UTexture2D>& SourceTextureRef,
			const FIntPoint& Offset,
			const FIntPoint& Dimension,
			const TEnumAsByte<ESpritePivotMode::Type> PivotMode,
			const FVector2D& CustomPivot,
			const TArray<TWeakObjectPtr<UTexture>>&
				AdditionalTextureRefs,
			const bool bCallModify, FExecutionMutationError& OutError)
		{
            UPaperSprite* Sprite = Cast<UPaperSprite>(Target);
			UTexture2D* SourceTexture = SourceTextureRef.Get();
			if (!Sprite
				|| (!SourceTexture
					&& !SourceTextureRef.IsExplicitlyNull()))
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_sprite_resolved_state_expired"),
                    !Sprite ? TEXT("Sprite writing requires a PaperSprite target.") : TEXT("The prepared source texture is no longer available."),
                    !Sprite ? TEXT("target") : TEXT("source_texture_ref"),
                    !Sprite ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("expired"))),
                    MakeShared<FJsonValueString>(!Sprite ? UPaperSprite::StaticClass()->GetPathName() : UTexture2D::StaticClass()->GetPathName())));
			}
			TStrongObjectPtr<UTexture2D> SourcePin(SourceTexture);
			TArray<TStrongObjectPtr<UTexture>> AdditionalPins;
			TArray<UTexture*> AdditionalTextures;
			AdditionalPins.Reserve(AdditionalTextureRefs.Num());
			AdditionalTextures.Reserve(
				AdditionalTextureRefs.Num());
			for (int32 Index = 0; Index < AdditionalTextureRefs.Num(); ++Index)
			{
                const TWeakObjectPtr<UTexture>& TextureRef = AdditionalTextureRefs[Index];
				UTexture* Texture = TextureRef.Get();
				if (!Texture && !TextureRef.IsExplicitlyNull())
				{
                    OutError = NativeFailure(TEXT("asset_data_paper_sprite_resolved_state_expired"),
                        TEXT("A prepared additional sprite texture is no longer available."),
                        TEXT("additional_texture_refs/") + FString::FromInt(Index), MakeShared<FJsonValueString>(TEXT("expired")),
                        MakeShared<FJsonValueString>(UTexture::StaticClass()->GetPathName()));
                    OutError.OperationIndex = Index;
                    return false;
				}
				if (Texture)
				{
					AdditionalPins.Emplace(Texture);
				}
				AdditionalTextures.Add(Texture);
			}
			FSpriteAssetInitParameters Init;
			Init.Texture = SourceTexture;
			Init.AdditionalTextures =
				MoveTemp(AdditionalTextures);
			Init.Offset = Offset;
			Init.Dimension = Dimension;
			if (bCallModify)
			{
				Sprite->Modify();
			}
			Sprite->InitializeSprite(Init, false);
			Sprite->SetPivotMode(
				PivotMode,
				CustomPivot,
				false);
			Sprite->PostEditChange();
			return true;
		}

		bool TryWritePaperFlipbookState(
			UObject* Target,
			const TArray<FPaperFlipbookFrameState>& FrameStates,
			const float FramesPerSecond,
			const bool bCallModify, FExecutionMutationError& OutError)
		{
            UPaperFlipbook* Flipbook = Cast<UPaperFlipbook>(Target);
			if (!Flipbook
				|| !FMath::IsFinite(FramesPerSecond)
				|| FramesPerSecond < 0.0f)
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_flipbook_resolved_state_expired"),
                    !Flipbook ? TEXT("Flipbook writing requires a PaperFlipbook target.") : TEXT("Flipbook frames per second must be finite and nonnegative."),
                    !Flipbook ? TEXT("target") : TEXT("frames_per_second"),
                    !Flipbook ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::SanitizeFloat(FramesPerSecond))),
                    MakeShared<FJsonValueString>(!Flipbook ? UPaperFlipbook::StaticClass()->GetPathName() : TEXT("finite number >= 0"))));
			}
			TArray<TStrongObjectPtr<UPaperSprite>> SpritePins;
			TArray<FPaperFlipbookKeyFrame> Frames;
			SpritePins.Reserve(FrameStates.Num());
			Frames.Reserve(FrameStates.Num());
			int64 TotalFrames = 0;
			for (int32 Index = 0; Index < FrameStates.Num(); ++Index)
			{
                const FPaperFlipbookFrameState& FrameState = FrameStates[Index];
				UPaperSprite* Sprite = FrameState.Sprite.Get();
				if ((!Sprite
						&& !FrameState.Sprite.IsExplicitlyNull())
					|| FrameState.FrameRun < 1)
				{
                    const bool bExpired = !Sprite && !FrameState.Sprite.IsExplicitlyNull();
                    OutError = NativeFailure(TEXT("asset_data_paper_flipbook_resolved_state_expired"),
                        bExpired ? TEXT("A prepared flipbook sprite is no longer available.") : TEXT("A flipbook frame duration must be at least one."),
                        TEXT("frames/") + FString::FromInt(Index) + (bExpired ? TEXT("/sprite_ref") : TEXT("/frame_run")),
                        bExpired ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("expired")))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(FrameState.FrameRun)),
                        MakeShared<FJsonValueString>(bExpired ? TEXT("live PaperSprite or explicit null") : TEXT("integer >= 1")));
                    OutError.OperationIndex = Index;
                    return false;
				}
				if (Sprite)
				{
					SpritePins.Emplace(Sprite);
				}
				TotalFrames += FrameState.FrameRun;
				if (TotalFrames > MAX_int32)
				{
                    OutError = NativeFailure(TEXT("asset_data_paper_flipbook_resolved_state_expired"),
                        TEXT("The accumulated flipbook frame duration exceeds the supported range."),
                        TEXT("frames/") + FString::FromInt(Index) + TEXT("/frame_run"),
                        MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), TotalFrames)),
                        MakeShared<FJsonValueNumber>(MAX_int32));
                    OutError.OperationIndex = Index;
                    return false;
				}
				FPaperFlipbookKeyFrame Frame;
				Frame.Sprite = Sprite;
				Frame.FrameRun = FrameState.FrameRun;
				Frames.Add(MoveTemp(Frame));
			}
			if (bCallModify)
			{
				Flipbook->Modify();
			}
			{
				FScopedFlipbookMutator Mutator(Flipbook);
				Mutator.KeyFrames = MoveTemp(Frames);
				Mutator.FramesPerSecond = FramesPerSecond;
			}
			Flipbook->PostEditChange();
			return true;
		}

		bool ApplyAction(
			FAssetDataAction& Action,
			FExecutionMutationError& OutError)
		{
			UPackage* Package = Action.Package.Get();
			switch (Action.Kind)
			{
			case EActionKind::CreateObject:
				{
					UClass* CreateClass =
						Action.CreateClass.Get();
					UObject* Created = nullptr;
					if (UFactory* Factory = Action.CreateFactory.Get())
					{
						Created = CreateNativeFactoryObject(Package, Action.ObjectName, CreateClass, Factory, false, OutError);
                        if (!Created) return false;
					}
					else if (CreateClass)
					{
						Created = NewObject<UObject>(
							Package,
							CreateClass,
							Action.ObjectName,
							RF_Public
								| RF_Standalone
								| RF_Transactional);
					}
					if (!Created)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_create_failed"),
                            !CreateClass ? TEXT("The captured creation class is no longer available.") : TEXT("NewObject did not return the requested asset."),
                            TEXT("class"), CreateClass ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CreateClass->GetPathName())) : nullptr,
                            MakeShared<FJsonValueString>(TEXT("live creation class and successfully created asset"))));
					}
					Action.Target = Created;
					Action.Package = Created->GetOutermost();
					Action.bCreated = true;
					break;
				}
			case EActionKind::CloneObject:
				{
					UObject* Source = Action.Source.Get();
					UObject* Created =
						Source
							? StaticDuplicateObject(
								Source,
								Package,
								Action.ObjectName)
							: nullptr;
					if (!Created)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_clone_failed"),
                            !Source ? TEXT("The captured clone source is no longer available.") : TEXT("StaticDuplicateObject did not return the requested clone."),
                            TEXT("source_ref"), MakeShared<FJsonValueString>(Action.SourceRef), MakeShared<FJsonValueString>(TEXT("live source and successfully duplicated asset"))));
					}
					Created->SetFlags(
						RF_Public
							| RF_Standalone
							| RF_Transactional);
					Action.Target = Created;
					Action.bCreated = true;
					break;
				}
			case EActionKind::UpdateRenderTarget:
				{
					UTextureRenderTarget2D* RenderTarget =
						Cast<UTextureRenderTarget2D>(
							Action.Target.Get());
					if (!RenderTarget)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_render_target_invalid"), TEXT("The target is unavailable or has the wrong class when the change is applied."), TEXT("target"), ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UTextureRenderTarget2D::StaticClass()->GetPathName())));
					}
					RenderTarget->Modify();
					if (Action.bChangeRenderTargetFormat)
					{
						RenderTarget->RenderTargetFormat =
							Action.NewRenderTargetFormat;
					}
					if (Action.bChangeRenderTargetClearColor)
					{
						RenderTarget->ClearColor =
							Action.NewRenderTargetClearColor;
					}
					if (Action.bChangeRenderTargetAutoGenerateMips)
					{
						RenderTarget->bAutoGenerateMips =
							Action.bNewAutoGenerateMips;
					}
					if (Action.bChangeRenderTargetSize)
					{
						RenderTarget->ResizeTarget(
							Action.NewSizeX,
							Action.NewSizeY);
					}
					else
					{
						RenderTarget->UpdateResourceImmediate(true);
					}
					RenderTarget->PostEditChange();
					break;
				}
			case EActionKind::UpdateStructuredAsset:
				{
					if (!TryApplyStructuredAssetUpdate(
							Action.Target.Get(),
							Action.StructuredAssetState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::UpdateMediaPlayer:
				{
					UMediaPlayer* MediaPlayer =
						Cast<UMediaPlayer>(Action.Target.Get());
					if (!MediaPlayer)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_media_player_invalid"), TEXT("The target is unavailable or has the wrong class when the change is applied."), TEXT("target"), ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName())));
					}
					MediaPlayer->Modify();
					if (Action.bChangeMediaPlayerPlayOnOpen)
					{
						MediaPlayer->PlayOnOpen =
							Action.bNewMediaPlayerPlayOnOpen;
					}
					if (Action.bChangeMediaPlayerLoop)
					{
						MediaPlayer->SetLooping(
							Action.bNewMediaPlayerLoop);
					}
					MediaPlayer->PostEditChange();
					break;
				}
			case EActionKind::UpdateMediaTexture:
				{
					UMediaTexture* MediaTexture =
						Cast<UMediaTexture>(Action.Target.Get());
					if (!MediaTexture)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_media_texture_invalid"), TEXT("The target is unavailable or has the wrong class when the change is applied."), TEXT("target"), ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UMediaTexture::StaticClass()->GetPathName())));
					}
					MediaTexture->Modify();
					if (Action.bChangeMediaTexturePlayer)
					{
						MediaTexture->SetMediaPlayer(
							Action.NewMediaTexturePlayer.Get());
					}
					if (Action.bChangeMediaTextureAutoClear)
					{
						MediaTexture->AutoClear =
							Action.bNewMediaTextureAutoClear;
					}
					if (Action.bChangeMediaTextureClearColor)
					{
						MediaTexture->ClearColor =
							Action.NewMediaTextureClearColor;
					}
					MediaTexture->PostEditChange();
					break;
				}
			case EActionKind::UpdateSubUVAnimation:
				{
					USubUVAnimation* SubUV =
						Cast<USubUVAnimation>(
							Action.Target.Get());
					if (!SubUV)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_subuv_invalid"), TEXT("The target is unavailable or has the wrong class when the change is applied."), TEXT("target"), ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(USubUVAnimation::StaticClass()->GetPathName())));
					}
					SubUV->Modify();
					if (Action.bChangeSubUVTexture)
					{
						SubUV->SubUVTexture =
							Action.NewSubUVTexture.Get();
					}
					if (Action.bChangeSubUVGrid)
					{
						SubUV->SubImages_Horizontal =
							Action.NewSubUVHorizontal;
						SubUV->SubImages_Vertical =
							Action.NewSubUVVertical;
					}
					if (Action.bChangeSubUVBoundingMode)
					{
						SubUV->BoundingMode =
							Action.NewSubUVBoundingMode;
					}
					SubUV->PostEditChange();
					break;
				}
			case EActionKind::UpdateTextureCollection:
				{
					UTextureCollection* Collection =
						Cast<UTextureCollection>(
							Action.Target.Get());
					if (!Collection)
					{
						return Fail(OutError, NativeFailure(TEXT("asset_data_texture_collection_invalid"),
                            TEXT("The TextureCollection target is unavailable when the change is applied."), TEXT("target"),
                            ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UTextureCollection::StaticClass()->GetPathName())));
					}
					TArray<UTexture*> Textures;
					Textures.Reserve(
						Action.NewTextureCollectionTextures.Num());
					for (const TWeakObjectPtr<UTexture>& Texture
						: Action.NewTextureCollectionTextures)
					{
						if (!Texture.IsValid())
						{
							return Fail(OutError, NativeFailure(TEXT("asset_data_texture_collection_resolved_texture_expired"),
                                TEXT("A captured texture is no longer available when the collection is applied."),
                                TEXT("texture_refs/") + ::LexToString(Textures.Num()), nullptr, MakeShared<FJsonValueString>(TEXT("live Texture"))));
						}
						Textures.Add(Texture.Get());
					}
					Collection->Modify();
					Collection->Textures.Reset(Textures.Num());
					for (UTexture* Texture : Textures)
					{
						Collection->Textures.Add(Texture);
					}
					Collection->UpdateResource();
					Collection->PostEditChange();
					break;
				}
			case EActionKind::ReplaceTextureArray:
			case EActionKind::ReplaceTextureCubeArray:
			case EActionKind::ReplaceTextureCube:
			case EActionKind::ReplaceVolumeTexture:
				{
					if (!TryApplyTextureSourceReplacement(
							Action.Target.Get(),
							Action.TextureSourceState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::UpdateInputAction:
				{
					if (!TryApplyEnhancedInputActionUpdate(
							Action.Target.Get(),
							Action.InputActionState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::UpdateInputMappingContext:
				{
					if (!TryApplyEnhancedInputMappingContextUpdate(
							Action.Target.Get(),
							Action.InputMappingContextState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::UpdatePaperSprite:
				{
					if (!TryWritePaperSpriteState(
							Action.Target.Get(),
							Action.NewPaperSpriteSourceTexture,
							Action.NewPaperSpriteOffset,
							Action.NewPaperSpriteDimension,
							Action.NewPaperSpritePivotMode,
							Action.NewPaperSpriteCustomPivot,
							Action
								.NewPaperSpriteAdditionalTextures,
							true, OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::ReplacePaperFlipbook:
				{
					if (!TryWritePaperFlipbookState(
							Action.Target.Get(),
							Action.NewPaperFlipbookFrames,
							Action
								.NewPaperFlipbookFramesPerSecond,
							true, OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::ReplacePaperTileMap:
				{
					if (!TryApplyPaperTileMapReplacement(
							Action.Target.Get(),
							Action.PaperTileMapState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::ReplacePaperTileSet:
				{
					if (!TryApplyPaperTileSetReplacement(
							Action.Target.Get(),
							Action.PaperTileSetState,
							OutError))
					{
						return false;
					}
					break;
				}
			case EActionKind::UpdateCoreAsset:
				{
					if (!TryApplyCoreAssetUpdate(
							Action.Target.Get(),
							Action.CoreAssetState,
							OutError))
					{
						return false;
					}
					break;
				}
			}
			if (Package)
			{
				Package->MarkPackageDirty();
			}
			return true;
		}

		bool RollbackAction(FAssetDataAction& Action, FExecutionMutationError& OutError)
		{
            bool bConfirmed = true;
			UPackage* Package = Action.Package.Get();
			if (Action.bCreated)
			{
				UObject* Created = Action.Target.Get();
				if (Created)
				{
					const bool bRenamed = Created->Rename(
						nullptr,
						GetTransientPackage(),
						REN_DontCreateRedirectors
							| REN_NonTransactional
							| REN_ForceNoResetLoaders);
					Created->MarkAsGarbage();
                    if (!bRenamed)
                    {
                        OutError = NativeFailure(TEXT("asset_data_rollback_created_object_rename_failed"),
                            TEXT("The created object could not be moved out of its asset package during rollback cleanup."),
                            TEXT("target_moved_to_transient"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                        OutError.TargetPath = Action.TargetRef;
                        OutError.MutationMayHaveStarted = true;
                        bConfirmed = false;
                    }
				}
			}
			else
			{
				switch (Action.Kind)
				{
				case EActionKind::UpdateRenderTarget:
					{
						UTextureRenderTarget2D* RenderTarget =
							Cast<UTextureRenderTarget2D>(
								Action.Target.Get());
						if (!RenderTarget)
						{
                            return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_target_invalid"),
                                TEXT("The rollback target does not match its captured asset type."), TEXT("target"),
                                ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UTextureRenderTarget2D::StaticClass()->GetPathName())));
                        }
						RenderTarget->RenderTargetFormat =
							Action.OriginalRenderTargetFormat;
						RenderTarget->ClearColor =
							Action.OriginalClearColor;
						RenderTarget->bAutoGenerateMips =
							Action.bOriginalAutoGenerateMips;
						RenderTarget->ResizeTarget(
							Action.OriginalSizeX,
							Action.OriginalSizeY);
						RenderTarget->PostEditChange();
						break;
					}
				case EActionKind::UpdateStructuredAsset:
					{
						if (!TryRollbackStructuredAssetUpdate(
								Action.Target.Get(),
								Action.StructuredAssetState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateMediaPlayer:
					{
						UMediaPlayer* MediaPlayer =
							Cast<UMediaPlayer>(
								Action.Target.Get());
						if (!MediaPlayer)
						{
                            return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_target_invalid"),
                                TEXT("The rollback target does not match its captured asset type."), TEXT("target"),
                                ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName())));
                        }
						MediaPlayer->PlayOnOpen =
							Action.bOriginalMediaPlayerPlayOnOpen;
						MediaPlayer->SetLooping(
							Action.bOriginalMediaPlayerLoop);
						MediaPlayer->PostEditChange();
						break;
					}
				case EActionKind::UpdateMediaTexture:
					{
						UMediaTexture* MediaTexture =
							Cast<UMediaTexture>(
								Action.Target.Get());
						if (!MediaTexture)
						{
                            return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_target_invalid"),
                                TEXT("The rollback target does not match its captured asset type."), TEXT("target"),
                                ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UMediaTexture::StaticClass()->GetPathName())));
                        }
						MediaTexture->SetMediaPlayer(
							Action.OriginalMediaTexturePlayer.Get());
						MediaTexture->AutoClear =
							Action.bOriginalMediaTextureAutoClear;
						MediaTexture->ClearColor =
							Action.OriginalMediaTextureClearColor;
						MediaTexture->PostEditChange();
						break;
					}
				case EActionKind::UpdateSubUVAnimation:
					{
						USubUVAnimation* SubUV =
							Cast<USubUVAnimation>(
								Action.Target.Get());
						if (!SubUV)
						{
                            return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_target_invalid"),
                                TEXT("The rollback target does not match its captured asset type."), TEXT("target"),
                                ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(USubUVAnimation::StaticClass()->GetPathName())));
                        }
						SubUV->SubUVTexture =
							Action.OriginalSubUVTexture.Get();
						SubUV->SubImages_Horizontal =
							Action.OriginalSubUVHorizontal;
						SubUV->SubImages_Vertical =
							Action.OriginalSubUVVertical;
						SubUV->BoundingMode =
							Action.OriginalSubUVBoundingMode;
						SubUV->PostEditChange();
						break;
					}
				case EActionKind::UpdateTextureCollection:
					{
						UTextureCollection* Collection =
							Cast<UTextureCollection>(
								Action.Target.Get());
						if (!Collection)
						{
                            return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_target_invalid"),
                                TEXT("The rollback target does not match its captured asset type."), TEXT("target"),
                                ObjectClassValue(Action.Target.Get()), MakeShared<FJsonValueString>(UTextureCollection::StaticClass()->GetPathName())));
                        }
						TArray<UTexture*> Textures;
						Textures.Reserve(
							Action
								.OriginalTextureCollectionTextures
								.Num());
                        for (int32 Index = 0; Index < Action.OriginalTextureCollectionTextures.Num(); ++Index)
                        {
                            const TWeakObjectPtr<UTexture>& Texture = Action.OriginalTextureCollectionTextures[Index];
                            if (!Texture.IsValid())
                            {
                                OutError = NativeFailure(TEXT("asset_data_texture_collection_rollback_reference_expired"),
                                    TEXT("A captured collection texture is no longer available."),
                                    TEXT("textures/") + FString::FromInt(Index), MakeShared<FJsonValueString>(TEXT("expired")),
                                    MakeShared<FJsonValueString>(UTexture::StaticClass()->GetPathName()));
                                OutError.OperationIndex = Index;
                                return false;
                            }
							Textures.Add(Texture.Get());
						}
						Collection->Textures.Reset(Textures.Num());
						for (UTexture* Texture : Textures)
						{
							Collection->Textures.Add(Texture);
						}
						Collection->UpdateResource();
						Collection->PostEditChange();
						break;
					}
				case EActionKind::ReplaceTextureArray:
				case EActionKind::ReplaceTextureCubeArray:
				case EActionKind::ReplaceTextureCube:
				case EActionKind::ReplaceVolumeTexture:
					{
						if (!TryRollbackTextureSourceReplacement(
								Action.Target.Get(),
								Action.TextureSourceState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateInputAction:
					{
						if (!TryRollbackEnhancedInputActionUpdate(
								Action.Target.Get(),
								Action.InputActionState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateInputMappingContext:
					{
						if (!TryRollbackEnhancedInputMappingContextUpdate(
								Action.Target.Get(),
								Action.InputMappingContextState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdatePaperSprite:
					{
						if (!TryWritePaperSpriteState(
								Action.Target.Get(),
								Action
									.OriginalPaperSpriteSourceTexture,
								Action.OriginalPaperSpriteOffset,
								Action
									.OriginalPaperSpriteDimension,
								Action
									.OriginalPaperSpritePivotMode,
								Action
									.OriginalPaperSpriteCustomPivot,
								Action
									.OriginalPaperSpriteAdditionalTextures,
								false, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperFlipbook:
					{
						if (!TryWritePaperFlipbookState(
								Action.Target.Get(),
								Action.OriginalPaperFlipbookFrames,
								Action
									.OriginalPaperFlipbookFramesPerSecond,
								false, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperTileMap:
					{
						if (!TryRollbackPaperTileMapReplacement(
								Action.Target.Get(),
								Action.PaperTileMapState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperTileSet:
					{
						if (!TryRollbackPaperTileSetReplacement(
								Action.Target.Get(),
								Action.PaperTileSetState, OutError))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateCoreAsset:
					{
						if (!TryRollbackCoreAssetUpdate(
								Action.Target.Get(),
								Action.CoreAssetState, OutError))
						{
							return false;
						}
						break;
					}
				default:
					break;
				}
			}
			if (Package)
			{
				Package->SetDirtyFlag(Action.bPackageWasDirty);
			}
			return bConfirmed;
		}

        bool RollbackAll(TArray<FAssetDataAction>& Actions, FExecutionMutationError& OutError)
        {
            bool bConfirmed = true;
            for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
            {
                FExecutionMutationError ActionError;
                if (!RollbackAction(Actions[Index], ActionError))
                {
                    if (ActionError.TargetPath.IsEmpty()) ActionError.TargetPath = Actions[Index].TargetRef;
                    ActionError.Stage = TEXT("rollback");
                    ActionError.RollbackAttempted = true;
                    ActionError.RollbackSucceeded = false;
                    OutError.Append(ActionError);
                    bConfirmed = false;
                }
            }
            return bConfirmed;
        }

		class FAssetDataSimpleEffectNativeHook final
			: public IUeAgentAssetDataSimpleEffectNativeHook
		{
		public:
			explicit FAssetDataSimpleEffectNativeHook(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectAssetResolver,
					ESPMode::ThreadSafe> InAssetResolver,
				TSharedRef<
					IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
					ESPMode::ThreadSafe> InFactoryResolver)
				: AssetResolver(MoveTemp(InAssetResolver))
				, FactoryResolver(MoveTemp(InFactoryResolver))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
				OutError = FExecutionMutationError();
                if (!IsInGameThread())
                    return Fail(OutError, NativeFailure(TEXT("asset_data_native_request_invalid"),
                        TEXT("Asset mutation must run on the game thread."), TEXT("game_thread"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
                if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
                if (!Lease.IsValid()) return Fail(OutError, Lease.GetValidationDiagnostic());
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					return Fail(OutError, NativeFailure(TEXT("asset_data_change_binding_mismatch"),
                        Changes.IsEmpty() ? TEXT("Asset mutation requires at least one planned change.")
                            : TEXT("The binding count does not match the planned change count."),
                        Changes.IsEmpty() ? TEXT("change_count") : TEXT("binding_count"),
                        MakeShared<FJsonValueNumber>(Changes.IsEmpty() ? 0 : Bindings.Num()),
                        Changes.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least one change")))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Changes.Num()))));
				}

				TArray<FAssetDataAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FAssetDataAction Action;
					if (!PrepareAction(
							*AssetResolver,
							*FactoryResolver,
							Change,
							Action,
							OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Change.GetIdentity().GetSelector();
						if (Change.GetAfter().IsValid()) LocateAssetDataSimpleEffectInputFailure(OutError, *Change.GetAfter());
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FAssetDataAction> AppliedActions;
				TArray<FString> MutatedKeys;
				OutResult = FUeAgentGenericEffectApplyResult();
				for (FAssetDataAction& Action : Actions)
				{
                    if (!ApplyAction(Action, OutError))
                    {
                        if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Action.TargetRef;
                        FExecutionMutationError RollbackError;
                        const bool bRestored = RollbackAll(AppliedActions, RollbackError);
                        if (!AppliedActions.IsEmpty())
                        {
                            const bool bEarlierRollbackSucceeded = !OutError.RollbackSucceeded.IsSet() || OutError.RollbackSucceeded.GetValue();
                            OutError.RollbackAttempted = true;
                            OutError.RollbackSucceeded = bEarlierRollbackSucceeded && bRestored;
                            OutError.MutationMayHaveStarted = true;
                        }
                        if (!bRestored) OutError.Diagnostics.Add(MoveTemp(RollbackError));
                        return false;
                    }
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				const FString Token = FString::Printf(
					TEXT("asset-data:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, AppliedActions);
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("asset_data.authoring.memory")},
					true,
					Token,
					true);
                if (OutResult.IsValid()) return true;
                OutError = OutResult.GetValidationDiagnostic();
                OutError.MutationMayHaveStarted = true;
                return false;
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				bOutConfirmed = false;
				OutError = FExecutionMutationError();
                if (!IsInGameThread())
                    return Fail(OutError, NativeFailure(TEXT("asset_data_rollback_request_invalid"),
                        TEXT("Asset rollback must run on the game thread."), TEXT("game_thread"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
                if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
                if (!Applied.IsValid()) return Fail(OutError, Applied.GetValidationDiagnostic());
				TArray<FAssetDataAction>* Found = nullptr;
				{
					FScopeLock Lock(&StateMutex);
					Found = RollbackByToken.Find(
						Applied.GetRollbackToken());
					if (!Found)
					{
                        auto Error = NativeFailure(TEXT("asset_data_rollback_token_unknown"),
                            TEXT("No captured rollback state exists for this applied result."), TEXT("rollback_state_available"),
                            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                        Error.InternalContext = Applied.GetRollbackToken();
                        return Fail(OutError, MoveTemp(Error));
					}
					bOutConfirmed = RollbackAll(*Found, OutError);
					if (bOutConfirmed)
					{
						RollbackByToken.Remove(
							Applied.GetRollbackToken());
						TokenByOperation.Remove(
							Request.GetOperationId());
					}
				}
                if (!bOutConfirmed)
                    OutError = Contract::FDiagnostic::WrapFailure(TEXT("asset_data_rollback_unconfirmed"), OutError, TEXT("rollback"));
                return bOutConfirmed;
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				FScopeLock Lock(&StateMutex);
				if (const FString* Token =
					TokenByOperation.Find(OperationId))
				{
					RollbackByToken.Remove(*Token);
				}
				TokenByOperation.Remove(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError();
				if (!Request.IsValid())
				{
					return Fail(OutError, NativeFailure(TEXT("asset_data_barrier_request_invalid"),
                        TEXT("The asset settle request does not satisfy the barrier request contract."), TEXT("barrier_request_valid"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("asset-data:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(OutError, NativeFailure(TEXT("asset_data_barrier_result_invalid"),
                        TEXT("The asset settle result does not satisfy the barrier result contract."), TEXT("barrier_result_valid"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}

		private:
			TSharedRef<
				IUeAgentAssetDataSimpleEffectAssetResolver,
				ESPMode::ThreadSafe> AssetResolver;
			TSharedRef<
				IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
				ESPMode::ThreadSafe> FactoryResolver;
			FCriticalSection StateMutex;
			TMap<FString, TArray<FAssetDataAction>> RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	bool FUeAgentAssetDataSimpleEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> AssetResolver,
		TSharedRef<
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryResolver,
		TSharedPtr<IUeAgentAssetDataSimpleEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook =
			MakeShared<FAssetDataSimpleEffectNativeHook>(
				MoveTemp(AssetResolver),
				MoveTemp(FactoryResolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid()
			|| Fail(OutError, NativeFailure(TEXT("asset_data_native_hook_create_failed"),
                TEXT("The native asset execution hook could not be created."), TEXT("native_hook_available"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
	}
}
