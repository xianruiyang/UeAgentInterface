#include "Core/Projection/UeAgentEditBundleProjection.h"

namespace UE::UAI::Projection
{
	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool NormalizeSet(TArray<FString>& Values)
		{
			for (FString& Value : Values)
			{
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty())
				{
					return false;
				}
			}
			Values.Sort();
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
					return false;
				}
			}
			return true;
		}

		bool IsSha256(const FString& Value)
		{
			if (
				Value.Len() != 71
				|| !Value.StartsWith(TEXT("sha256:")))
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}
	}

	FEditBundleProjection::FEditBundleProjection(
		FString InBundleFingerprint,
		FString InClosureFingerprint,
		TArray<FString> InInlineUnitIds,
		TArray<FString> InInlineFieldIds,
		TOptional<FUeAgentArtifactDescriptor> InArtifact,
		FUeAgentPageInfo InPage)
		: BundleFingerprint(
			Normalize(MoveTemp(InBundleFingerprint)))
		, ClosureFingerprint(
			Normalize(MoveTemp(InClosureFingerprint)))
		, InlineUnitIds(MoveTemp(InInlineUnitIds))
		, InlineFieldIds(MoveTemp(InInlineFieldIds))
		, Artifact(MoveTemp(InArtifact))
		, Page(MoveTemp(InPage))
	{
		bValid =
			IsSha256(BundleFingerprint)
			&& IsSha256(ClosureFingerprint)
			&& NormalizeSet(InlineUnitIds)
			&& NormalizeSet(InlineFieldIds)
			&& Page.IsValid()
			&& (!Artifact.IsSet() || Artifact->IsValid());
	}

	const FString&
	FEditBundleProjection::GetBundleFingerprint() const
	{
		return BundleFingerprint;
	}

	const FString&
	FEditBundleProjection::GetClosureFingerprint() const
	{
		return ClosureFingerprint;
	}

	const TArray<FString>&
	FEditBundleProjection::GetInlineUnitIds() const
	{
		return InlineUnitIds;
	}

	const TArray<FString>&
	FEditBundleProjection::GetInlineFieldIds() const
	{
		return InlineFieldIds;
	}

	const TOptional<FUeAgentArtifactDescriptor>&
	FEditBundleProjection::GetArtifact() const
	{
		return Artifact;
	}

	const FUeAgentPageInfo& FEditBundleProjection::GetPage() const
	{
		return Page;
	}

	bool FEditBundleProjection::IsValid() const
	{
		return bValid;
	}
}
