#pragma once

#include "AnimGraphNode_Base.h"
#include "LilaMorphPaths.h"

#include "AnimGraphNode_LilaMorphPaths.generated.h"

/** Lila Morph Paths in the anim graph: drives each Lila morph target's shapes from its weight curve. */
UCLASS()
class LILALINKANIMGRAPH_API UAnimGraphNode_LilaMorphPaths : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_LilaMorphPaths Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FString GetNodeCategory() const override;
};
