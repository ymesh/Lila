#pragma once

#if WITH_EDITOR
#include "CoreMinimal.h"
#include "IOptimusNodeAdderPinProvider.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"

/** Building Deformer Graphs in code: the pin lookups and kernel pins the Lila graphs share. */
namespace LilaGraphPins
{
inline UOptimusNodePin* PinNamed(UOptimusNode* Node, const TCHAR* Name)
{
	for (UOptimusNodePin* Pin : Node->GetPins())
	{
		if (Pin->GetName() == Name)
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Adds a pin to the kernel matching Source (its type and domain) and links them, the way dragging onto the kernel's + pin does. */
inline bool AddKernelPin(UOptimusNodeGraph* Graph, UOptimusNode* Kernel, UOptimusNodePin* Source, const EOptimusNodePinDirection Direction, const FName Name, FString& OutError)
{
	IOptimusNodeAdderPinProvider* Adder = Cast<IOptimusNodeAdderPinProvider>(Kernel);
	if (!Adder || !Source)
	{
		OutError = FString::Printf(TEXT("can't add the kernel pin %s"), *Name.ToString());
		return false;
	}
	FString Reason;
	const TArray<IOptimusNodeAdderPinProvider::FAdderPinAction> Actions = Adder->GetAvailableAdderPinActions(Source, Direction, &Reason);
	if (Actions.IsEmpty())
	{
		OutError = FString::Printf(TEXT("the kernel takes no pin from %s: %s"), *Source->GetName(), *Reason);
		return false;
	}
	const TArray<UOptimusNodePin*> Added = Adder->TryAddPinFromPin(Actions[0], Source, Name);
	if (Added.IsEmpty())
	{
		OutError = FString::Printf(TEXT("the kernel didn't add %s"), *Name.ToString());
		return false;
	}
	const bool bLinked = Direction == EOptimusNodePinDirection::Input ? Graph->AddLink(Source, Added[0]) : Graph->AddLink(Added[0], Source);
	if (!bLinked)
	{
		OutError = FString::Printf(TEXT("couldn't link %s"), *Name.ToString());
	}
	return bLinked;
}
} // namespace LilaGraphPins
#endif
