#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCoreAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectEnhancedInputMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperTileMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"

#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureRenderTarget2D.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "Particles/SubUVAnimation.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace Discovery;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
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

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			return Object.IsValid()
				? FindObjectField(*Object, Field)
				: nullptr;
		}

		bool TryReadString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			if (!Value
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
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
				*Value.GetLexeme());
		}

		bool TryReadInteger(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			int64& OutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			return Value
				&& Value->GetKind() == EUeAgentValueKind::Integer
				&& LexTryParseString(
					OutValue,
					*Value->GetLexeme());
		}

		bool TryReadBoolean(
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

		bool TryReadIntegralPixel(
			const FUeAgentCanonicalValue& Value,
			int32& OutValue)
		{
			double Number = 0.0;
			if (!TryReadNumber(Value, Number)
				|| !FMath::IsFinite(Number)
				|| FMath::FloorToDouble(Number) != Number
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		bool TryReadPaperSpriteRegion(
			const FUeAgentCanonicalValue& Value,
			FIntPoint& OutOffset,
			FIntPoint& OutDimension)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num() != 4)
			{
				return false;
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Value, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Value, TEXT("y"));
			const FUeAgentCanonicalValue* Width =
				FindObjectField(Value, TEXT("width"));
			const FUeAgentCanonicalValue* Height =
				FindObjectField(Value, TEXT("height"));
			return X
				&& Y
				&& Width
				&& Height
				&& TryReadIntegralPixel(*X, OutOffset.X)
				&& TryReadIntegralPixel(*Y, OutOffset.Y)
				&& TryReadIntegralPixel(
					*Width,
					OutDimension.X)
				&& TryReadIntegralPixel(
					*Height,
					OutDimension.Y)
				&& OutOffset.X >= 0
				&& OutOffset.Y >= 0
				&& OutDimension.X > 0
				&& OutDimension.Y > 0;
		}

		bool TryReadPaperSpritePivot(
			const FUeAgentCanonicalValue& Value,
			FVector2D& OutPivot)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num() != 2)
			{
				return false;
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Value, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Value, TEXT("y"));
			int32 PixelX = 0;
			int32 PixelY = 0;
			if (!X
				|| !Y
				|| !TryReadIntegralPixel(*X, PixelX)
				|| !TryReadIntegralPixel(*Y, PixelY))
			{
				return false;
			}
			OutPivot = FVector2D(
				static_cast<double>(PixelX),
				static_cast<double>(PixelY));
			return true;
		}

		struct FPaperFlipbookFrameState
		{
			TWeakObjectPtr<UPaperSprite> Sprite;
			int32 FrameRun = 1;
		};

		bool TryReadPaperFlipbookFrames(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FPaperFlipbookFrameState>& OutFrames,
			FExecutionMutationError& OutError)
		{
			OutFrames.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_flipbook_"
						"frames_invalid"));
			}
			int64 TotalFrames = 0;
			for (int32 Index = 0;
				Index < Value.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& FrameValue =
					Value.GetArrayItems()[Index].Get();
				if (FrameValue.GetKind()
						!= EUeAgentValueKind::Object
					|| FrameValue.GetObjectItems().Num() != 2)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_flipbook_"
							"frame_shape_invalid"),
						FString::FromInt(Index));
				}
				const FUeAgentCanonicalValue* SpriteRef =
					FindObjectField(
						FrameValue,
						TEXT("sprite_ref"));
				const FUeAgentCanonicalValue* FrameRunValue =
					FindObjectField(
						FrameValue,
						TEXT("frame_run"));
				double FrameRunNumber = 0.0;
				if (!SpriteRef
					|| !FrameRunValue
					|| !TryReadNumber(
						*FrameRunValue,
						FrameRunNumber)
					|| !FMath::IsFinite(FrameRunNumber)
					|| FMath::FloorToDouble(FrameRunNumber)
						!= FrameRunNumber
					|| FrameRunNumber < 1.0
					|| FrameRunNumber
						> static_cast<double>(MAX_int32))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_flipbook_"
							"frame_run_invalid"),
						FString::FromInt(Index));
				}

				FPaperFlipbookFrameState Frame;
				Frame.FrameRun =
					static_cast<int32>(FrameRunNumber);
				if (SpriteRef->GetKind()
					== EUeAgentValueKind::String)
				{
					const FString& Ref = SpriteRef->GetText();
					UObject* Resolved = nullptr;
					if (Ref.IsEmpty()
						|| !Resolver.TryResolveExisting(
							Ref,
							Resolved,
							OutError))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_flipbook_"
								"sprite_ref_invalid"),
							FString::FromInt(Index));
					}
					UPaperSprite* Sprite =
						Cast<UPaperSprite>(Resolved);
					if (!Sprite)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_flipbook_"
								"sprite_class_invalid"),
							Ref);
					}
					Frame.Sprite = Sprite;
				}
				else if (SpriteRef->GetKind()
					!= EUeAgentValueKind::Null)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_flipbook_"
							"sprite_ref_invalid"),
						FString::FromInt(Index));
				}
				TotalFrames += Frame.FrameRun;
				if (TotalFrames > MAX_int32)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_flipbook_"
							"total_frames_overflow"));
				}
				OutFrames.Add(MoveTemp(Frame));
			}
			return true;
		}

		bool TryResolveUniqueTextureArray(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<TWeakObjectPtr<UTexture>>& OutTextures,
			FExecutionMutationError& OutError)
		{
			OutTextures.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_collection_"
						"refs_invalid"));
			}
			TSet<FString> SeenRefs;
			TSet<UTexture*> SeenTextures;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue& RefValue = Item.Get();
				if (RefValue.GetKind() != EUeAgentValueKind::String)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_collection_"
							"ref_invalid"));
				}
				const FString& Ref = RefValue.GetText();
				if (Ref.IsEmpty() || SeenRefs.Contains(Ref))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_collection_"
							"duplicate_ref"),
						Ref);
				}
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						Ref,
						Resolved,
						OutError))
				{
					return false;
				}
				UTexture* Texture = Cast<UTexture>(Resolved);
				if (!Texture || SeenTextures.Contains(Texture))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_collection_"
							"texture_invalid"),
						Ref);
				}
				SeenRefs.Add(Ref);
				SeenTextures.Add(Texture);
				OutTextures.Add(Texture);
			}
			return true;
		}

		bool TryReadLinearColor(
			const FUeAgentCanonicalValue& Value,
			FLinearColor& OutColor)
		{
			const FUeAgentCanonicalValue* Red =
				FindObjectField(Value, TEXT("r"));
			const FUeAgentCanonicalValue* Green =
				FindObjectField(Value, TEXT("g"));
			const FUeAgentCanonicalValue* Blue =
				FindObjectField(Value, TEXT("b"));
			const FUeAgentCanonicalValue* Alpha =
				FindObjectField(Value, TEXT("a"));
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 0.0;
			if (!Red
				|| !Green
				|| !Blue
				|| !Alpha
				|| !TryReadNumber(*Red, R)
				|| !TryReadNumber(*Green, G)
				|| !TryReadNumber(*Blue, B)
				|| !TryReadNumber(*Alpha, A)
				|| !FMath::IsFinite(R)
				|| !FMath::IsFinite(G)
				|| !FMath::IsFinite(B)
				|| !FMath::IsFinite(A))
			{
				return false;
			}
			OutColor = FLinearColor(
				static_cast<float>(R),
				static_cast<float>(G),
				static_cast<float>(B),
				static_cast<float>(A));
			return true;
		}

		bool TryReadRenderTargetFormat(
			const FUeAgentCanonicalValue& Value,
			ETextureRenderTargetFormat& OutFormat)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			const FString Format =
				Value.GetText()
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
			return false;
		}

		bool TryReadNumberArray(
			const FUeAgentCanonicalValue& Value,
			const int32 Count,
			TArray<double>& OutNumbers)
		{
			OutNumbers.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() != Count)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				double Number = 0.0;
				if (!TryReadNumber(*Item, Number))
				{
					OutNumbers.Reset();
					return false;
				}
				OutNumbers.Add(Number);
			}
			return true;
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
				return Fail(
					OutError,
					TEXT("asset_data_native_typed_changes_missing"));
			}
			for (const FUeAgentCanonicalValueRef& Unit
				: Changes->GetArrayItems())
			{
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(
						OutError,
						TEXT("asset_data_native_change_invalid"));
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
					return Fail(
						OutError,
						TEXT("asset_data_native_change_invalid"));
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
					OutAction.ProfileId)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("native_binding_id"),
					OutAction.NativeBindingId)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("target_ref"),
					OutAction.TargetRef)
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
			FString BindingError;
			if (!TryResolveAssetDataSimpleEffectNativeBinding(
					OutAction.NativeBindingId,
					BoundNativeOperationType,
					BoundProfileId,
					BoundNativeMechanicId,
					StaticCreateClassPath,
					BindingError)
				|| BoundNativeOperationType != TargetKind
				|| BoundProfileId != OutAction.ProfileId)
			{
				return Fail(
					OutError,
					BindingError.IsEmpty()
						? TEXT("asset_data_native_binding_mismatch")
						: MoveTemp(BindingError),
				OutAction.NativeBindingId);
			}
			OutAction.NativeMechanicId =
				BoundNativeMechanicId;
			for (const FTypedChange& TypedChange
				: OutAction.Changes)
			{
				FString FieldError;
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
					return Fail(
						OutError,
						FieldError,
						TypedChange.FieldId);
				}
			}

			if (BoundNativeMechanicId
				== TEXT("uai.asset_data.mechanic:create_object:v1"))
			{
				if (StaticCreateClassPath.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("asset_data_factory_candidate_required"),
						OutAction.ProfileId);
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
					return Fail(
						OutError,
						TEXT("asset_data_create_class_invalid"),
						StaticCreateClassPath);
				}
				OutAction.Kind = EActionKind::CreateObject;
				OutAction.CreateClass = CreateClass;
			}
			else if (BoundNativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"create_candidate_object:v1"))
			{
				FString FactoryBindingId;
				FString CatalogSourceId;
				FString CatalogGeneration;
				int64 CatalogSequence = -1;
				if (!TryReadString(
						Change.GetAfter(),
						TEXT("factory_binding_id"),
						FactoryBindingId)
					|| !TryReadString(
						Change.GetAfter(),
						TEXT("factory_catalog_source_id"),
						CatalogSourceId)
					|| !TryReadString(
						Change.GetAfter(),
						TEXT("factory_catalog_generation"),
						CatalogGeneration)
					|| !TryReadInteger(
						Change.GetAfter(),
						TEXT("factory_catalog_sequence"),
						CatalogSequence))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_native_factory_binding_missing"),
						OutAction.ProfileId);
				}
				const FUeAgentCatalogRevision ExpectedRevision(
					CatalogSourceId,
					CatalogGeneration,
					CatalogSequence);
				FUeAgentAssetDataSimpleEffectFactoryBinding
					FactoryBinding;
				if (!ExpectedRevision.IsValid()
					|| !FactoryResolver.TryResolveFrozenBinding(
						FactoryBindingId,
						ExpectedRevision,
						OutAction.ProfileId,
						FactoryBinding,
						OutError))
				{
					return false;
				}
				FString FactoryError;
				if (!TryValidateAssetDataSimpleEffectFactoryBinding(
						OutAction.NativeBindingId,
						FactoryBindingId,
						ExpectedRevision,
						FactoryBinding,
						FactoryError))
				{
					return Fail(
						OutError,
						FactoryError,
						OutAction.ProfileId);
				}
				OutAction.Kind = EActionKind::CreateObject;
				OutAction.CreateClass =
					FactoryBinding.GetConcreteClass();
			}
			else if (BoundNativeMechanicId
				== TEXT("uai.asset_data.mechanic:clone_object:v1"))
			{
				OutAction.Kind = EActionKind::CloneObject;
				UObject* Source = nullptr;
				if (!TryReadString(
						Change.GetAfter(),
						TEXT("source_ref"),
						OutAction.SourceRef)
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
				return Fail(
					OutError,
					TEXT("asset_data_native_profile_unsupported"),
					OutAction.ProfileId + TEXT(":") + TargetKind);
			}

			if (OutAction.Kind == EActionKind::CreateObject
				|| OutAction.Kind == EActionKind::CloneObject)
			{
				UPackage* Package = nullptr;
				if (!Resolver.TryResolveCreationTarget(
						OutAction.TargetRef,
						Package,
						OutAction.ObjectName,
						OutError)
					|| !Package
					|| OutAction.ObjectName.IsNone()
					|| FindObject<UObject>(
						Package,
						*OutAction.ObjectName.ToString()))
				{
					return Fail(
						OutError,
						TEXT("asset_data_creation_target_invalid"),
						OutAction.TargetRef);
				}
				OutAction.Package = Package;
				OutAction.bPackageWasDirty = Package->IsDirty();
			}
			else
			{
				UObject* Target = nullptr;
				if (!Resolver.TryResolveExisting(
						OutAction.TargetRef,
						Target,
						OutError)
					|| !IsValid(Target))
				{
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
						return Fail(
							OutError,
							TEXT("asset_data_render_target_invalid"));
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
								double WidthNumber = 0.0;
								double HeightNumber = 0.0;
								if (!WidthValue
									|| !HeightValue
									|| !TryReadNumber(
										*WidthValue,
										WidthNumber)
									|| !TryReadNumber(
										*HeightValue,
										HeightNumber)
									|| WidthNumber < 1.0
									|| HeightNumber < 1.0
									|| WidthNumber
										> GTextureRenderTarget2DMaxSizeX
									|| HeightNumber
										> GTextureRenderTarget2DMaxSizeY
									|| FMath::FloorToDouble(WidthNumber)
										!= WidthNumber
									|| FMath::FloorToDouble(HeightNumber)
										!= HeightNumber)
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_render_target_"
											"size_invalid"));
								}
								Width = static_cast<int32>(WidthNumber);
								Height = static_cast<int32>(HeightNumber);
							}
							if (Width < 1 || Height < 1)
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_render_target_"
										"default_size_invalid"));
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
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadRenderTargetFormat(
										*TypedChange.CanonicalValue,
										Format)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_render_target_"
										"format_invalid"));
							}
							OutAction.bChangeRenderTargetFormat = true;
							OutAction.NewRenderTargetFormat = Format;
						}
						else if (TypedChange.FieldId
							== TEXT("clear_color"))
						{
							FLinearColor Color = Default->ClearColor;
							if (TypedChange.Presence == TEXT("set")
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadLinearColor(
										*TypedChange.CanonicalValue,
										Color)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_render_target_"
										"clear_color_invalid"));
							}
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
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadBoolean(
										*TypedChange.CanonicalValue,
										bAutoGenerateMips)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_render_target_"
										"auto_mips_invalid"));
							}
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
						return Fail(
							OutError,
							TEXT("asset_data_media_player_invalid"));
					}
					OutAction.bOriginalMediaPlayerPlayOnOpen =
						MediaPlayer->PlayOnOpen;
					OutAction.bOriginalMediaPlayerLoop =
						MediaPlayer->IsLooping();
					for (const FTypedChange& TypedChange
						: OutAction.Changes)
					{
						bool bValue = false;
						if (!TypedChange.CanonicalValue.IsValid()
							|| !TryReadBoolean(
								*TypedChange.CanonicalValue,
								bValue))
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_media_player_"
									"field_invalid"),
								TypedChange.FieldId);
						}
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
						return Fail(
							OutError,
							TEXT("asset_data_media_texture_invalid"));
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
								if (!TypedChange.CanonicalValue.IsValid()
									|| !Resolver.TryResolveExisting(
										TypedChange.CanonicalValue
											->GetText(),
										Resolved,
										OutError))
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_media_texture_"
											"player_invalid"));
								}
								NewMediaPlayer =
									Cast<UMediaPlayer>(Resolved);
								if (!NewMediaPlayer)
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_media_texture_"
											"player_invalid"));
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
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadBoolean(
										*TypedChange.CanonicalValue,
										bAutoClear)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_media_texture_"
										"auto_clear_invalid"));
							}
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
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadLinearColor(
										*TypedChange.CanonicalValue,
										Color)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_media_texture_"
										"clear_color_invalid"));
							}
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
						return Fail(
							OutError,
							TEXT("asset_data_subuv_invalid"));
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
								if (!TypedChange.CanonicalValue.IsValid()
									|| !Resolver.TryResolveExisting(
										TypedChange.CanonicalValue
											->GetText(),
										Resolved,
										OutError))
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_subuv_"
											"texture_invalid"));
								}
								Texture = Cast<UTexture2D>(Resolved);
								if (!Texture)
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_subuv_"
											"texture_invalid"));
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
							if (TypedChange.Presence
								== TEXT("set")
								&& (!TypedChange.CanonicalValue.IsValid()
									|| !TryReadInteger(
										TypedChange.CanonicalValue,
										TEXT("x"),
										Horizontal)
									|| !TryReadInteger(
										TypedChange.CanonicalValue,
										TEXT("y"),
										Vertical)))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_subuv_grid_invalid"));
							}
							if (Horizontal < 1
								|| Vertical < 1
								|| Horizontal > MAX_int32
								|| Vertical > MAX_int32
								|| Horizontal * Vertical
									> MAX_int32 / 8)
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_subuv_grid_invalid"));
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
									return Fail(
										OutError,
										TEXT(
											"asset_data_subuv_"
											"bounding_geometry_invalid"));
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
									return Fail(
										OutError,
										TEXT(
											"asset_data_subuv_"
											"bounding_geometry_invalid"));
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
						return Fail(
							OutError,
							TEXT("asset_data_subuv_grid_invalid"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_subuv_grid_exceeds_texture"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_texture_collection_"
								"invalid"));
					}
					OutAction.OriginalTextureCollectionTextures
						.Reserve(Collection->Textures.Num());
					for (UTexture* Texture : Collection->Textures)
					{
						if (!Texture)
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_texture_collection_"
									"existing_texture_invalid"));
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
						if (!TypedChange.CanonicalValue.IsValid())
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_texture_collection_"
									"refs_invalid"));
						}
						TArray<TWeakObjectPtr<UTexture>> Requested;
						if (!TryResolveUniqueTextureArray(
								Resolver,
								*TypedChange.CanonicalValue,
								Requested,
								OutError))
						{
							return false;
						}
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_texture_collection_"
								"changes_missing"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_sprite_invalid"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_sprite_"
								"existing_region_invalid"));
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
								if (!TypedChange.CanonicalValue.IsValid()
									|| TypedChange.CanonicalValue
											->GetKind()
										!= EUeAgentValueKind::String
									|| !Resolver.TryResolveExisting(
										TypedChange.CanonicalValue
											->GetText(),
										Resolved,
										OutError))
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_paper_sprite_"
											"source_texture_invalid"));
								}
								Texture = Cast<UTexture2D>(Resolved);
								if (!Texture)
								{
									return Fail(
										OutError,
										TEXT(
											"asset_data_paper_sprite_"
											"source_texture_invalid"));
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
							else if (!TypedChange.CanonicalValue.IsValid()
								|| !TryReadPaperSpriteRegion(
									*TypedChange.CanonicalValue,
									OutAction.NewPaperSpriteOffset,
									OutAction
										.NewPaperSpriteDimension))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_paper_sprite_"
										"source_region_invalid"));
							}
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
							else if (!TypedChange.CanonicalValue.IsValid()
								|| !TryReadPaperSpritePivot(
									*TypedChange.CanonicalValue,
									OutAction
										.NewPaperSpriteCustomPivot))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_paper_sprite_"
										"pivot_invalid"));
							}
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_sprite_"
								"effective_region_invalid"));
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
							return Fail(
								OutError,
								TEXT(
									"asset_data_paper_sprite_"
									"region_exceeds_texture"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_flipbook_"
								"invalid"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_flipbook_"
								"existing_fps_invalid"));
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
							return Fail(
								OutError,
								TEXT(
									"asset_data_paper_flipbook_"
									"existing_frame_invalid"),
								FString::FromInt(Index));
						}
						OriginalTotalFrames += KeyFrame.FrameRun;
						if (OriginalTotalFrames > MAX_int32)
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_paper_flipbook_"
									"existing_total_overflow"));
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
							if (!TypedChange.CanonicalValue.IsValid()
								|| !TryReadPaperFlipbookFrames(
									Resolver,
									*TypedChange.CanonicalValue,
									OutAction.NewPaperFlipbookFrames,
									OutError))
							{
								return false;
							}
						}
						else if (TypedChange.FieldId
							== TEXT("frames_per_second"))
						{
							double FramesPerSecond = 0.0;
							if (!TypedChange.CanonicalValue.IsValid()
								|| !TryReadNumber(
									*TypedChange.CanonicalValue,
									FramesPerSecond)
								|| !FMath::IsFinite(
									FramesPerSecond)
								|| FramesPerSecond < 0.0
								|| FramesPerSecond > 1000.0)
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_paper_flipbook_"
										"fps_invalid"));
							}
							const float NativeFramesPerSecond =
								static_cast<float>(
									FramesPerSecond);
							if (!FMath::IsFinite(
								NativeFramesPerSecond))
							{
								return Fail(
									OutError,
									TEXT(
										"asset_data_paper_flipbook_"
										"fps_invalid"));
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
			UPaperSprite* Sprite,
			const TWeakObjectPtr<UTexture2D>& SourceTextureRef,
			const FIntPoint& Offset,
			const FIntPoint& Dimension,
			const TEnumAsByte<ESpritePivotMode::Type> PivotMode,
			const FVector2D& CustomPivot,
			const TArray<TWeakObjectPtr<UTexture>>&
				AdditionalTextureRefs,
			const bool bCallModify)
		{
			UTexture2D* SourceTexture = SourceTextureRef.Get();
			if (!Sprite
				|| (!SourceTexture
					&& !SourceTextureRef.IsExplicitlyNull()))
			{
				return false;
			}
			TStrongObjectPtr<UTexture2D> SourcePin(SourceTexture);
			TArray<TStrongObjectPtr<UTexture>> AdditionalPins;
			TArray<UTexture*> AdditionalTextures;
			AdditionalPins.Reserve(AdditionalTextureRefs.Num());
			AdditionalTextures.Reserve(
				AdditionalTextureRefs.Num());
			for (const TWeakObjectPtr<UTexture>& TextureRef
				: AdditionalTextureRefs)
			{
				UTexture* Texture = TextureRef.Get();
				if (!Texture && !TextureRef.IsExplicitlyNull())
				{
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
			UPaperFlipbook* Flipbook,
			const TArray<FPaperFlipbookFrameState>& FrameStates,
			const float FramesPerSecond,
			const bool bCallModify)
		{
			if (!Flipbook
				|| !FMath::IsFinite(FramesPerSecond)
				|| FramesPerSecond < 0.0f)
			{
				return false;
			}
			TArray<TStrongObjectPtr<UPaperSprite>> SpritePins;
			TArray<FPaperFlipbookKeyFrame> Frames;
			SpritePins.Reserve(FrameStates.Num());
			Frames.Reserve(FrameStates.Num());
			int64 TotalFrames = 0;
			for (const FPaperFlipbookFrameState& FrameState
				: FrameStates)
			{
				UPaperSprite* Sprite = FrameState.Sprite.Get();
				if ((!Sprite
						&& !FrameState.Sprite.IsExplicitlyNull())
					|| FrameState.FrameRun < 1)
				{
					return false;
				}
				if (Sprite)
				{
					SpritePins.Emplace(Sprite);
				}
				TotalFrames += FrameState.FrameRun;
				if (TotalFrames > MAX_int32)
				{
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
					UObject* Created =
						CreateClass
							? NewObject<UObject>(
							Package,
							CreateClass,
							Action.ObjectName,
							RF_Public
								| RF_Standalone
								| RF_Transactional)
							: nullptr;
					if (!Created)
					{
						return Fail(
							OutError,
							TEXT("asset_data_create_failed"));
					}
					Action.Target = Created;
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
						return Fail(
							OutError,
							TEXT("asset_data_clone_failed"));
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
						return Fail(
							OutError,
							TEXT("asset_data_render_target_invalid"));
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
						return Fail(
							OutError,
							TEXT("asset_data_media_player_invalid"));
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
						return Fail(
							OutError,
							TEXT("asset_data_media_texture_invalid"));
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
						return Fail(
							OutError,
							TEXT("asset_data_subuv_invalid"));
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
						return Fail(
							OutError,
							TEXT(
								"asset_data_texture_collection_"
								"invalid"));
					}
					TArray<UTexture*> Textures;
					Textures.Reserve(
						Action.NewTextureCollectionTextures.Num());
					for (const TWeakObjectPtr<UTexture>& Texture
						: Action.NewTextureCollectionTextures)
					{
						if (!Texture.IsValid())
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_texture_collection_"
									"resolved_texture_expired"));
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
					UPaperSprite* Sprite =
						Cast<UPaperSprite>(
							Action.Target.Get());
					if (!TryWritePaperSpriteState(
							Sprite,
							Action.NewPaperSpriteSourceTexture,
							Action.NewPaperSpriteOffset,
							Action.NewPaperSpriteDimension,
							Action.NewPaperSpritePivotMode,
							Action.NewPaperSpriteCustomPivot,
							Action
								.NewPaperSpriteAdditionalTextures,
							true))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_sprite_"
								"resolved_state_expired"));
					}
					break;
				}
			case EActionKind::ReplacePaperFlipbook:
				{
					UPaperFlipbook* Flipbook =
						Cast<UPaperFlipbook>(
							Action.Target.Get());
					if (!TryWritePaperFlipbookState(
							Flipbook,
							Action.NewPaperFlipbookFrames,
							Action
								.NewPaperFlipbookFramesPerSecond,
							true))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_flipbook_"
								"resolved_state_expired"));
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

		bool RollbackAction(FAssetDataAction& Action)
		{
			UPackage* Package = Action.Package.Get();
			if (Action.bCreated)
			{
				UObject* Created = Action.Target.Get();
				if (Created)
				{
					Created->Rename(
						nullptr,
						GetTransientPackage(),
						REN_DontCreateRedirectors
							| REN_NonTransactional
							| REN_ForceNoResetLoaders);
					Created->MarkAsGarbage();
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
							return false;
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
								Action.StructuredAssetState))
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
							return false;
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
							return false;
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
							return false;
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
							return false;
						}
						TArray<UTexture*> Textures;
						Textures.Reserve(
							Action
								.OriginalTextureCollectionTextures
								.Num());
						for (const TWeakObjectPtr<UTexture>& Texture
							: Action
								.OriginalTextureCollectionTextures)
						{
							if (!Texture.IsValid())
							{
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
								Action.TextureSourceState))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateInputAction:
					{
						if (!TryRollbackEnhancedInputActionUpdate(
								Action.Target.Get(),
								Action.InputActionState))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateInputMappingContext:
					{
						if (!TryRollbackEnhancedInputMappingContextUpdate(
								Action.Target.Get(),
								Action.InputMappingContextState))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdatePaperSprite:
					{
						UPaperSprite* Sprite =
							Cast<UPaperSprite>(
								Action.Target.Get());
						if (!TryWritePaperSpriteState(
								Sprite,
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
								false))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperFlipbook:
					{
						UPaperFlipbook* Flipbook =
							Cast<UPaperFlipbook>(
								Action.Target.Get());
						if (!TryWritePaperFlipbookState(
								Flipbook,
								Action.OriginalPaperFlipbookFrames,
								Action
									.OriginalPaperFlipbookFramesPerSecond,
								false))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperTileMap:
					{
						if (!TryRollbackPaperTileMapReplacement(
								Action.Target.Get(),
								Action.PaperTileMapState))
						{
							return false;
						}
						break;
					}
				case EActionKind::ReplacePaperTileSet:
					{
						if (!TryRollbackPaperTileSetReplacement(
								Action.Target.Get(),
								Action.PaperTileSetState))
						{
							return false;
						}
						break;
					}
				case EActionKind::UpdateCoreAsset:
					{
						if (!TryRollbackCoreAssetUpdate(
								Action.Target.Get(),
								Action.CoreAssetState))
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
			return true;
		}

		bool RollbackAll(TArray<FAssetDataAction>& Actions)
		{
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1;
				Index >= 0;
				--Index)
			{
				bConfirmed =
					RollbackAction(Actions[Index])
					&& bConfirmed;
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
				OutResult = FUeAgentGenericEffectApplyResult();
				OutError = FExecutionMutationError();
				if (!IsInGameThread()
					|| !Request.IsValid()
					|| !Lease.IsValid())
				{
					return Fail(
						OutError,
						TEXT("asset_data_native_request_invalid"));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					return Fail(
						OutError,
						TEXT("asset_data_change_binding_mismatch"));
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
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FAssetDataAction> AppliedActions;
				TArray<FString> MutatedKeys;
				for (FAssetDataAction& Action : Actions)
				{
					if (!ApplyAction(Action, OutError))
					{
						RollbackAll(AppliedActions);
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
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT("asset_data_apply_result_invalid"));
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
				if (!IsInGameThread()
					|| !Request.IsValid()
					|| !Applied.IsValid())
				{
					return Fail(
						OutError,
						TEXT("asset_data_rollback_request_invalid"));
				}
				TArray<FAssetDataAction>* Found = nullptr;
				{
					FScopeLock Lock(&StateMutex);
					Found = RollbackByToken.Find(
						Applied.GetRollbackToken());
					if (!Found)
					{
						return Fail(
							OutError,
							TEXT("asset_data_rollback_token_unknown"));
					}
					bOutConfirmed = RollbackAll(*Found);
					if (bOutConfirmed)
					{
						RollbackByToken.Remove(
							Applied.GetRollbackToken());
						TokenByOperation.Remove(
							Request.GetOperationId());
					}
				}
				return bOutConfirmed
					|| Fail(
						OutError,
						TEXT("asset_data_rollback_unconfirmed"));
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
					return Fail(
						OutError,
						TEXT("asset_data_barrier_request_invalid"));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("asset-data:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT("asset_data_barrier_result_invalid"));
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
			|| Fail(
				OutError,
				TEXT("asset_data_native_hook_create_failed"));
	}
}
