#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::QueryCalibration
{
	enum class EQueryCalibrationCompleteness : uint8
	{
		Complete,
		Paged,
		Partial,
		Degraded,
	};

	struct UEAGENTINTERFACE_API FQueryCalibrationError final
	{
		FString Code;
		FString Detail;
	};

	struct UEAGENTINTERFACE_API FQueryCalibrationRequest final
	{
		FString Category;
		FString SearchText;
		int32 Limit = 50;
		int32 Cursor = 0;
	};

	struct UEAGENTINTERFACE_API FQueryCalibrationItem final
	{
		FString Identity;
		FString DisplayName;
		FString Kind;
		TMap<FString, FString> Fields;
	};

	struct UEAGENTINTERFACE_API FQueryCalibrationPage final
	{
		FString Category;
		FString SourceId;
		FString StateLayer;
		FString Revision;
		bool bReadOnly = true;
		EQueryCalibrationCompleteness Completeness =
			EQueryCalibrationCompleteness::Complete;
		TArray<FQueryCalibrationItem> Items;
		TArray<FString> MissingSources;
		int32 TotalItems = 0;
		int32 NextCursor = -1;
	};

	class UEAGENTINTERFACE_API IUeAgentQueryCalibrationSource
	{
	public:
		virtual ~IUeAgentQueryCalibrationSource() = default;

		virtual bool Read(
			const FQueryCalibrationRequest& Request,
			FQueryCalibrationPage& OutPage,
			FQueryCalibrationError& OutError) const = 0;
	};

	UEAGENTINTERFACE_API const TArray<FString>&
	GetQueryCalibrationCategoryIds();

	UEAGENTINTERFACE_API bool TryCreateProductionQueryCalibrationSource(
		TSharedPtr<IUeAgentQueryCalibrationSource>& OutSource,
		FString& OutError);
}
