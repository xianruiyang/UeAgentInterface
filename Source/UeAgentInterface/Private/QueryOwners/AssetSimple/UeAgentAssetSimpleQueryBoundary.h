#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::AssetSimple
{
	/** T37 is an Operation-only inventory slice and owns no Query command. */
	inline int32 GetAssetSimpleOwnedQueryCommandCount()
	{
		return 0;
	}
}
