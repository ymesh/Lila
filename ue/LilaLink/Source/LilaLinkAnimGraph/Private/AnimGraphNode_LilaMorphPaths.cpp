#include "AnimGraphNode_LilaMorphPaths.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AnimGraphNode_LilaMorphPaths)

#define LOCTEXT_NAMESPACE "LilaLinkAnimGraph"

FText UAnimGraphNode_LilaMorphPaths::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("Title", "Lila Morph Paths");
}

FText UAnimGraphNode_LilaMorphPaths::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "Drives each Lila morph target's shapes (in-betweens, Smooth, Arc and Pivot paths, response curves) from the curve named "
	                          "after the target, as Lila evaluates them. Reads the paths Lila keeps on the skeletal mesh unless Paths is set.");
}

FString UAnimGraphNode_LilaMorphPaths::GetNodeCategory() const { return TEXT("Lila"); }

#undef LOCTEXT_NAMESPACE
