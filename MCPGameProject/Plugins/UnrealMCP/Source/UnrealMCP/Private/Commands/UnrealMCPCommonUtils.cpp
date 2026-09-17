#include "Commands/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "Kismet/GameplayStatics.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"

// Reasonable-area guard rails for node placement. Deliberately generous: they exist to
// catch astronomical gaps (runaway agents / bad plan coordinates), not to police layout.
static constexpr float GMaxNodeCoordinate = 100000.0f;  // absolute |x|,|y| backstop
static constexpr float GMaxPlacementDrift = 20000.0f;   // max distance beyond the current graph box

// Actor utilities
UWorld* FUnrealMCPCommonUtils::GetEditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FUnrealMCPCommonUtils::ResolveActor(const FString& Identifier)
{
    UWorld* World = GetEditorWorld();
    if (!World || Identifier.IsEmpty())
    {
        return nullptr;
    }
    TArray<AActor*> Actors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), Actors);
    // Path first (uniquely identifies), then label, then raw object name.
    for (AActor* Actor : Actors)
    {
        if (Actor && Actor->GetPathName() == Identifier) { return Actor; }
    }
    for (AActor* Actor : Actors)
    {
        if (Actor && Actor->GetActorLabel() == Identifier) { return Actor; }
    }
    for (AActor* Actor : Actors)
    {
        if (Actor && Actor->GetName() == Identifier) { return Actor; }
    }
    return nullptr;
}

// JSON Utilities
TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    
    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }
    
    return ResponseObject;
}

void FUnrealMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FUnrealMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

FVector2D FUnrealMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }
    
    return Result;
}

FVector FUnrealMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

FRotator FUnrealMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

// Engine state utilities
bool FUnrealMCPCommonUtils::IsObjectLookupSafe(FString& OutReason)
{
    // StaticFindObjectFast() fatal-asserts in the engine (UObjectGlobals.cpp:
    // "Illegal call to StaticFindObjectFast() while serializing object data or
    // garbage collecting!") when either of these globals is set. Match the engine's
    // own conditions so callers refuse the lookup instead of crashing the editor.
    if (GIsSavingPackage)
    {
        OutReason = TEXT("editor is saving a package (GIsSavingPackage); asset lookups are unsafe until the save completes");
        return false;
    }
    if (GIsGarbageCollecting)
    {
        OutReason = TEXT("game thread is garbage collecting; asset lookups are unsafe until GC completes");
        return false;
    }
    return true;
}

// Blueprint Utilities
UBlueprint* FUnrealMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FUnrealMCPCommonUtils::FindBlueprintByName(const FString& InBlueprintName)
{
    if (InBlueprintName.IsEmpty())
    {
        return nullptr;
    }

    FString BlueprintName = InBlueprintName.TrimStartAndEnd();

    // Clean up any duplicate slashes right away
    while (BlueprintName.Contains(TEXT("//")))
    {
        BlueprintName = BlueprintName.Replace(TEXT("//"), TEXT("/"));
    }

    // 1. If caller passed a package path or asset path (starts with "/")
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        // Try UEditorAssetLibrary::LoadAsset first as it loads unloaded packages into memory properly
        UObject* LoadedObj = UEditorAssetLibrary::LoadAsset(BlueprintName);
        if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
        {
            return BP;
        }

        // Try LoadObject with full dot path
        FString FullAssetPath = BlueprintName;
        if (!FullAssetPath.Contains(TEXT(".")))
        {
            FullAssetPath = FullAssetPath + TEXT(".") + FPaths::GetBaseFilename(FullAssetPath);
        }
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FullAssetPath))
        {
            return BP;
        }

        // If it starts with "/" and direct load failed, do NOT prepend /Game/Blueprints/!
        // Instead, search Asset Registry by base asset name:
        FString BaseName = FPaths::GetBaseFilename(BlueprintName);
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssets(Filter, AssetDataList);

        for (const FAssetData& AssetData : AssetDataList)
        {
            if (AssetData.AssetName.ToString().Equals(BaseName, ESearchCase::IgnoreCase))
            {
                return Cast<UBlueprint>(AssetData.GetAsset());
            }
        }

        return nullptr;
    }

    // 2. Simple name passed (no leading slash):
    // First try standard /Game/Blueprints/<Name>
    FString DefaultPath = TEXT("/Game/Blueprints/") + BlueprintName;
    UObject* LoadedObj = UEditorAssetLibrary::LoadAsset(DefaultPath);
    if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
    {
        return BP;
    }

    FString FullDefaultPath = DefaultPath + TEXT(".") + BlueprintName;
    if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FullDefaultPath))
    {
        return BP;
    }

    // 3. Fall back to searching across the entire Asset Registry for this name anywhere under /Game
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);

    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Equals(BlueprintName, ESearchCase::IgnoreCase))
        {
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
    }

    return nullptr;
}

UEdGraph* FUnrealMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Try to find the event graph
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph->GetName().Contains(TEXT("EventGraph")))
        {
            return Graph;
        }
    }
    
    // Create a new event graph if none exists
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    return NewGraph;
}

UEdGraph* FUnrealMCPCommonUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        return FindOrCreateEventGraph(Blueprint);
    }

    if (GraphName.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase) ||
        GraphName.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
        GraphName.Equals(TEXT("Construction"), ESearchCase::IgnoreCase))
    {
        UEdGraph* ConstructionGraph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint);
        if (ConstructionGraph)
        {
            return ConstructionGraph;
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName().Contains(TEXT("Construction")))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    // Exact match first
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
        {
            return Graph;
        }
    }

    // Substring match next
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && Graph->GetName().Contains(GraphName))
        {
            return Graph;
        }
    }

    return nullptr;
}

UEdGraphNode* FUnrealMCPCommonUtils::FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidStr, UEdGraph* PreferredGraph)
{
    if (NodeGuidStr.IsEmpty())
    {
        return nullptr;
    }

    if (PreferredGraph)
    {
        for (UEdGraphNode* Node : PreferredGraph->Nodes)
        {
            if (Node && (Node->NodeGuid.ToString().Equals(NodeGuidStr, ESearchCase::IgnoreCase) || Node->GetName().Equals(NodeGuidStr, ESearchCase::IgnoreCase)))
            {
                return Node;
            }
        }
    }

    if (Blueprint)
    {
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph == PreferredGraph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && (Node->NodeGuid.ToString().Equals(NodeGuidStr, ESearchCase::IgnoreCase) || Node->GetName().Equals(NodeGuidStr, ESearchCase::IgnoreCase)))
                {
                    return Node;
                }
            }
        }
    }

    return nullptr;
}

// Blueprint node utilities
UK2Node_Event* FUnrealMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Check for existing event node with this exact name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Using existing event node with name %s (ID: %s)"), 
                *EventName, *EventNode->NodeGuid.ToString());
            return EventNode;
        }
    }

    // No existing node found, create a new one
    UK2Node_Event* EventNode = nullptr;
    
    // Find the function to create the event
    UClass* BlueprintClass = Blueprint->GeneratedClass;
    UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));
    
    if (EventFunction)
    {
        EventNode = NewObject<UK2Node_Event>(Graph);
        EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
        EventNode->NodePosX = Position.X;
        EventNode->NodePosY = Position.Y;
        Graph->AddNode(EventNode, true);
        EventNode->PostPlacedNewNode();
        EventNode->AllocateDefaultPins();
        UE_LOG(LogTemp, Display, TEXT("Created new event node with name %s (ID: %s)"), 
            *EventName, *EventNode->NodeGuid.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to find function for event name: %s"), *EventName);
    }
    
    return EventNode;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
{
    if (!Graph || !Function)
    {
        return nullptr;
    }
    
    UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
    FunctionNode->SetFromFunction(Function);
    FunctionNode->NodePosX = Position.X;
    FunctionNode->NodePosY = Position.Y;
    Graph->AddNode(FunctionNode, true);
    FunctionNode->CreateNewGuid();
    FunctionNode->PostPlacedNewNode();
    FunctionNode->AllocateDefaultPins();
    
    return FunctionNode;
}

UK2Node_BreakStruct* FUnrealMCPCommonUtils::CreateBreakStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position)
{
    if (!Graph || !StructType)
    {
        return nullptr;
    }

    UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph);
    BreakNode->StructType = StructType;
    BreakNode->NodePosX = Position.X;
    BreakNode->NodePosY = Position.Y;
    Graph->AddNode(BreakNode, true);
    BreakNode->CreateNewGuid();
    BreakNode->PostPlacedNewNode();
    BreakNode->AllocateDefaultPins();

    return BreakNode;
}

UK2Node_MakeStruct* FUnrealMCPCommonUtils::CreateMakeStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position)
{
    if (!Graph || !StructType)
    {
        return nullptr;
    }

    UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph);
    MakeNode->StructType = StructType;
    MakeNode->NodePosX = Position.X;
    MakeNode->NodePosY = Position.Y;
    Graph->AddNode(MakeNode, true);
    MakeNode->CreateNewGuid();
    MakeNode->PostPlacedNewNode();
    MakeNode->AllocateDefaultPins();

    return MakeNode;
}

UK2Node_VariableGet* FUnrealMCPCommonUtils::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableGetNode->VariableReference.SetSelfMember(VarName);
        VariableGetNode->NodePosX = Position.X;
        VariableGetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableGetNode, true);
        VariableGetNode->CreateNewGuid();
        VariableGetNode->PostPlacedNewNode();
        VariableGetNode->AllocateDefaultPins();
        
        return VariableGetNode;
    }
    
    return nullptr;
}

UK2Node_VariableSet* FUnrealMCPCommonUtils::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableSetNode->VariableReference.SetSelfMember(VarName);
        VariableSetNode->NodePosX = Position.X;
        VariableSetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableSetNode, true);
        VariableSetNode->CreateNewGuid();
        VariableSetNode->PostPlacedNewNode();
        VariableSetNode->AllocateDefaultPins();
        
        return VariableSetNode;
    }
    
    return nullptr;
}

UK2Node_InputAction* FUnrealMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
    InputActionNode->InputActionName = FName(*ActionName);
    InputActionNode->NodePosX = Position.X;
    InputActionNode->NodePosY = Position.Y;
    Graph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();
    
    return InputActionNode;
}

UK2Node_Self* FUnrealMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
    SelfNode->NodePosX = Position.X;
    SelfNode->NodePosY = Position.Y;
    Graph->AddNode(SelfNode, true);
    SelfNode->CreateNewGuid();
    SelfNode->PostPlacedNewNode();
    SelfNode->AllocateDefaultPins();
    
    return SelfNode;
}

UK2Node_Knot* FUnrealMCPCommonUtils::CreateKnotNode(UEdGraph* Graph, const FVector2D& Position)
{
    // A reroute (knot) node is a tiny pass-through used to bend a wire around an obstacle.
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
    KnotNode->NodePosX = Position.X;
    KnotNode->NodePosY = Position.Y;
    Graph->AddNode(KnotNode, true);
    KnotNode->CreateNewGuid();
    KnotNode->PostPlacedNewNode();
    KnotNode->AllocateDefaultPins();

    return KnotNode;
}

UK2Node_IfThenElse* FUnrealMCPCommonUtils::CreateBranchNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Graph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_ExecutionSequence* FUnrealMCPCommonUtils::CreateSequenceNode(UEdGraph* Graph, int32 NumOutputs, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    UK2Node_ExecutionSequence* Node = NewObject<UK2Node_ExecutionSequence>(Graph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    // Count existing 'then_' execution outputs, then add pins until NumOutputs is reached.
    int32 Existing = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().StartsWith(TEXT("then_")))
        {
            ++Existing;
        }
    }
    while (NumOutputs > 0 && Existing < NumOutputs)
    {
        Node->AddInputPin();
        ++Existing;
    }

    return Node;
}

UK2Node_DynamicCast* FUnrealMCPCommonUtils::CreateCastNode(UEdGraph* Graph, UClass* TargetClass, const FVector2D& Position)
{
    if (!Graph || !TargetClass) { return nullptr; }

    UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
    Node->TargetType = TargetClass;
    Node->SetPurity(false); // impure => exec pins, so the cast can gate success/failure
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_CustomEvent* FUnrealMCPCommonUtils::CreateCustomEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    // UK2Node_CustomEvent::CreateFromFunction requires a non-null UFunction, so build the
    // node directly instead (a brand new custom event has no backing function).
    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
    Node->CustomFunctionName = FName(*EventName);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_MacroInstance* FUnrealMCPCommonUtils::CreateMacroNode(UEdGraph* Graph, const FString& MacroName, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    const FString MacroPath = FString::Printf(
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:%s"), *MacroName);
    UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *MacroPath);
    if (!MacroGraph)
    {
        UE_LOG(LogTemp, Error, TEXT("Could not load standard macro graph: %s"), *MacroPath);
        return nullptr;
    }

    UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
    Node->SetMacroGraph(MacroGraph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateSpawnActorNode(UEdGraph* Graph, UClass* ActorClass, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    // The dedicated UK2Node_SpawnActorFromClass is a composite node that asserts when built
    // headless, so expose spawning through the underlying gameplay-statics function instead.
    UFunction* SpawnFunction = UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("BeginDeferredActorSpawnFromClass"));
    if (!SpawnFunction) { return nullptr; }

    UK2Node_CallFunction* Node = CreateFunctionCallNode(Graph, SpawnFunction, Position);
    if (Node && ActorClass)
    {
        if (UEdGraphPin* ClassPin = FindPin(Node, TEXT("ActorClass"), EGPD_Input))
        {
            ClassPin->DefaultObject = ActorClass;
        }
    }

    return Node;
}

bool FUnrealMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph || !SourceNode || !TargetNode)
    {
        return false;
    }
    
    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    if (!SourcePin)
    {
        // Not a top-level pin: maybe a split struct member ("X", "Min", or "ReturnValue_X").
        SourcePin = FindPinOrSplitMember(SourceNode, SourcePinName, EGPD_Output);
    }
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    if (!TargetPin)
    {
        TargetPin = FindPinOrSplitMember(TargetNode, TargetPinName, EGPD_Input);
    }
    
    if (SourcePin && TargetPin)
    {
        // Route through the graph schema so wildcard pins (Cast.Object, ForEach.Array, ...)
        // propagate their type and the link survives recompiles. A raw MakeLinkTo skips the
        // schema's connection notifications and leaves those pins untyped ("undetermined").
        const UEdGraphSchema* Schema = Graph->GetSchema();
        if (Schema && Schema->TryCreateConnection(SourcePin, TargetPin))
        {
            return true;
        }
        return false;
    }
    
    return false;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    
    // Log all pins for debugging
    UE_LOG(LogTemp, Display, TEXT("FindPin: Looking for pin '%s' (Direction: %d) in node '%s'"), 
           *PinName, (int32)Direction, *Node->GetName());
    
    for (UEdGraphPin* Pin : Node->Pins)
    {
        UE_LOG(LogTemp, Display, TEXT("  - Available pin: '%s', Direction: %d, Category: %s"), 
               *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
    }
    
    // First try exact match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found exact matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If no exact match and we're looking for a component reference, try case-insensitive match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) && 
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If we're looking for a VariableGet output and didn't find it by name, fall back
    // only when there is exactly ONE data output pin - otherwise the choice is ambiguous
    // and guessing could wire the wrong pin.
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        UEdGraphPin* SoleOutput = nullptr;
        int32 DataOutputCount = 0;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                SoleOutput = Pin;
                ++DataOutputCount;
            }
        }
        if (DataOutputCount == 1)
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found sole fallback data output pin: '%s'"), *SoleOutput->PinName.ToString());
            return SoleOutput;
        }
        if (DataOutputCount > 1)
        {
            UE_LOG(LogTemp, Warning, TEXT("  - Pin '%s' not found on VariableGet '%s' and %d data outputs are ambiguous; not guessing"), *PinName, *Node->GetName(), DataOutputCount);
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("  - No matching pin found for '%s'"), *PinName);
    return nullptr;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPinOrSplitMember(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }

    // A caller may pass "Parent.Member" to disambiguate; a bare name has no explicit parent.
    FString ParentPart;
    FString MemberPart = PinName;
    if (PinName.Contains(TEXT(".")))
    {
        PinName.Split(TEXT("."), &ParentPart, &MemberPart);
    }

    // 1) Already-split struct pins keep their children in <ParentPin>->SubPins (NOT Node->Pins),
    //    named "<ParentPinName>_<Member>". Match by full name or by the trailing member segment.
    for (UEdGraphPin* ParentPin : Node->Pins)
    {
        if (!ParentPin || (Direction != EGPD_MAX && ParentPin->Direction != Direction) || ParentPin->SubPins.Num() == 0)
        {
            continue;
        }
        if (!ParentPart.IsEmpty() && !ParentPin->PinName.ToString().Equals(ParentPart, ESearchCase::IgnoreCase))
        {
            continue;
        }
        for (UEdGraphPin* SubPin : ParentPin->SubPins)
        {
            if (!SubPin)
            {
                continue;
            }
            const FString SubName = SubPin->PinName.ToString();
            if (SubName.Equals(PinName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
            FString SubParent, SubMember;
            if (SubName.Split(TEXT("_"), &SubParent, &SubMember, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
                && SubMember.Equals(MemberPart, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
        }
    }

    // 2) Not split yet: find a struct pin whose struct defines the requested member, split it
    //    through the schema, then resolve the freshly created child pin from its SubPins.
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || (Direction != EGPD_MAX && Pin->Direction != Direction) || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
        {
            continue;
        }
        if (!ParentPart.IsEmpty() && !Pin->PinName.ToString().Equals(ParentPart, ESearchCase::IgnoreCase))
        {
            continue;
        }
        UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
        if (!Struct)
        {
            continue;
        }

        // The requested pin may be a bare member ("Max") or the full split name the schema
        // generates ("ReturnValue_Max" = "<ParentPinName>_<Member>").
        const FString ParentPinName = Pin->PinName.ToString();
        FString MemberName = MemberPart;
        if (PinName.StartsWith(ParentPinName + TEXT("_")))
        {
            MemberName = PinName.RightChop(ParentPinName.Len() + 1);
        }
        if (!Struct->FindPropertyByName(FName(*MemberName)))
        {
            continue;
        }

        const UEdGraphSchema* Schema = Node->GetSchema();
        if (!Schema)
        {
            continue;
        }
        Schema->SplitPin(Pin);
        for (UEdGraphPin* SubPin : Pin->SubPins)
        {
            if (!SubPin)
            {
                continue;
            }
            const FString SubName = SubPin->PinName.ToString();
            if (SubName.Equals(PinName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
            FString SubParent, SubMember;
            if (SubName.Split(TEXT("_"), &SubParent, &SubMember, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
                && SubMember.Equals(MemberName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
        }
    }

    return nullptr;
}

bool FUnrealMCPCommonUtils::SetNodePinDefault(UEdGraphNode* Node, const FString& PinName, const FString& Value)
{
    if (!Node)
    {
        return false;
    }

    // Exact top-level pin first; otherwise a split struct member (accepts "X" or "Vector.X").
    UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_MAX);
    if (!Pin)
    {
        Pin = FindPinOrSplitMember(Node, PinName, EGPD_MAX);
    }
    if (!Pin)
    {
        return false;
    }

    const UEdGraphSchema* Schema = Node->GetSchema();
    if (!Schema)
    {
        return false;
    }

    // TrySetDefaultValue validates/converts per pin type and marks the graph modified.
    Schema->TrySetDefaultValue(*Pin, Value);
    return true;
}

// Actor utilities
TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ActorToJsonObject(AActor* Actor, bool bDetailed)
{
    if (!Actor)
    {
        return nullptr;
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return ActorObject;
}

UK2Node_Event* FUnrealMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    // Look for existing event nodes
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Found existing event node with name: %s"), *EventName);
            return EventNode;
        }
    }

    return nullptr;
}

namespace
{
    // Accept a bool either as a JSON boolean or as the string "true"/"false"; reject
    // anything else so a wrong-typed value fails loudly instead of coercing to false.
    static bool MCPJsonToBool(const TSharedPtr<FJsonValue>& Value, bool& Out)
    {
        if (Value->Type == EJson::Boolean) { Out = Value->AsBool(); return true; }
        if (Value->Type == EJson::String)
        {
            const FString S = Value->AsString();
            if (S.Equals(TEXT("true"), ESearchCase::IgnoreCase)) { Out = true; return true; }
            if (S.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { Out = false; return true; }
        }
        return false;
    }

    // Accept a number as a JSON number or a numeric string; reject anything else.
    static bool MCPJsonToDouble(const TSharedPtr<FJsonValue>& Value, double& Out)
    {
        if (Value->Type == EJson::Number) { Out = Value->AsNumber(); return true; }
        if (Value->Type == EJson::String && Value->AsString().IsNumeric()) { Out = FCString::Atod(*Value->AsString()); return true; }
        return false;
    }
}

bool FUnrealMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);
    
    // Handle different property types
    if (Property->IsA<FBoolProperty>())
    {
        bool BoolValue = false;
        if (!MCPJsonToBool(Value, BoolValue))
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a boolean value (true/false)"), *PropertyName);
            return false;
        }
        ((FBoolProperty*)Property)->SetPropertyValue(PropertyAddr, BoolValue);
        return true;
    }
    else if (Property->IsA<FIntProperty>())
    {
        double NumValue = 0.0;
        if (!MCPJsonToDouble(Value, NumValue))
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a number value"), *PropertyName);
            return false;
        }
        FIntProperty* IntProperty = CastField<FIntProperty>(Property);
        if (IntProperty)
        {
            IntProperty->SetPropertyValue_InContainer(Object, static_cast<int32>(NumValue));
            return true;
        }
    }
    else if (Property->IsA<FFloatProperty>())
    {
        double NumValue = 0.0;
        if (!MCPJsonToDouble(Value, NumValue))
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a number value"), *PropertyName);
            return false;
        }
        ((FFloatProperty*)Property)->SetPropertyValue(PropertyAddr, (float)NumValue);
        return true;
    }
    else if (Property->IsA<FDoubleProperty>())
    {
        double NumValue = 0.0;
        if (!MCPJsonToDouble(Value, NumValue))
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a number value"), *PropertyName);
            return false;
        }
        ((FDoubleProperty*)Property)->SetPropertyValue(PropertyAddr, NumValue);
        return true;
    }
    else if (Property->IsA<FStrProperty>())
    {
        if (Value->Type != EJson::String)
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a string value"), *PropertyName);
            return false;
        }
        ((FStrProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsString());
        return true;
    }
    else if (Property->IsA<FNameProperty>())
    {
        if (Value->Type != EJson::String)
        {
            OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a string value"), *PropertyName);
            return false;
        }
        ((FNameProperty*)Property)->SetPropertyValue(PropertyAddr, FName(*Value->AsString()));
        return true;
    }
    else if (Property->IsA<FByteProperty>())
    {
        FByteProperty* ByteProp = CastField<FByteProperty>(Property);
        UEnum* EnumDef = ByteProp ? ByteProp->GetIntPropertyEnum() : nullptr;
        
        // If this is a TEnumAsByte property (has associated enum)
        if (EnumDef)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
                ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %d"), 
                      *PropertyName, ByteValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    uint8 ByteValue = FCString::Atoi(*EnumValueName);
                    ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %d"), 
                          *PropertyName, *EnumValueName, ByteValue);
                    return true;
                }
                
                // Handle qualified enum names (e.g., "Player0" or "EAutoReceiveInput::Player0")
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
        else
        {
            // Regular (non-enum) byte property.
            double NumValue = 0.0;
            if (!MCPJsonToDouble(Value, NumValue))
            {
                OutErrorMessage = FString::Printf(TEXT("Property '%s' expects a number value"), *PropertyName);
                return false;
            }
            ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(NumValue));
            return true;
        }
    }
    else if (Property->IsA<FEnumProperty>())
    {
        FEnumProperty* EnumProp = CastField<FEnumProperty>(Property);
        UEnum* EnumDef = EnumProp ? EnumProp->GetEnum() : nullptr;
        FNumericProperty* UnderlyingNumericProp = EnumProp ? EnumProp->GetUnderlyingProperty() : nullptr;
        
        if (EnumDef && UnderlyingNumericProp)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                int64 EnumValue = static_cast<int64>(Value->AsNumber());
                UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %lld"), 
                      *PropertyName, EnumValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    int64 EnumValue = FCString::Atoi64(*EnumValueName);
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                
                // Handle qualified enum names
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
    }
    
    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"), 
                                    *Property->GetClass()->GetName(), *PropertyName);
    return false;
}

// ---------------------------------------------------------------------------
// Node layout / placement validation (headless): estimate node boxes and reject
// placements that would overlap an existing node, so the tools never stack nodes.
// ---------------------------------------------------------------------------

FVector2D FUnrealMCPCommonUtils::EstimateNodeSize(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return FVector2D(160.0f, 80.0f);
    }

    // Resizable nodes that Slate has laid out report a real size.
    if (Node->GetWidth() > 0.0f && Node->GetHeight() > 0.0f)
    {
        return FVector2D(Node->GetWidth(), Node->GetHeight());
    }

    // Headless: estimate from the title and the longest input/output pin labels.
    const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
    int32 MaxInputLen = 0;
    int32 MaxOutputLen = 0;
    int32 NumPins = 0;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin)
        {
            continue;
        }
        ++NumPins;
        const int32 Len = Pin->PinName.ToString().Len();
        if (Pin->Direction == EGPD_Input)
        {
            MaxInputLen = FMath::Max(MaxInputLen, Len);
        }
        else
        {
            MaxOutputLen = FMath::Max(MaxOutputLen, Len);
        }
    }

    const float TitleWidth = Title.Len() * 8.0f;
    const float PinsWidth = (MaxInputLen + MaxOutputLen) * 7.0f + 60.0f;
    const float Width = FMath::Clamp(FMath::Max(TitleWidth, PinsWidth) + 40.0f, 160.0f, 640.0f);
    const float Height = FMath::Clamp(30.0f + NumPins * 22.0f + 20.0f, 80.0f, 800.0f);
    return FVector2D(Width, Height);
}

float FUnrealMCPCommonUtils::NodeGap(const UEdGraphNode* A, const UEdGraphNode* B)
{
    if (!A || !B)
    {
        return 0.0f;
    }
    const FVector2D APos(A->NodePosX, A->NodePosY);
    const FVector2D BPos(B->NodePosX, B->NodePosY);
    const FVector2D ASize = EstimateNodeSize(A);
    const FVector2D BSize = EstimateNodeSize(B);

    // Axis-aligned edge-to-edge gap (0 when the boxes touch or overlap).
    const float GapX = FMath::Max(0.0f, FMath::Max(APos.X, BPos.X) - FMath::Min(APos.X + ASize.X, BPos.X + BSize.X));
    const float GapY = FMath::Max(0.0f, FMath::Max(APos.Y, BPos.Y) - FMath::Min(APos.Y + ASize.Y, BPos.Y + BSize.Y));
    return FMath::Sqrt(GapX * GapX + GapY * GapY);
}

void FUnrealMCPCommonUtils::ComputeGraphBounds(UEdGraph* Graph, FVector2D& OutMin, FVector2D& OutMax, int32& OutCount, bool bIncludeStructural)
{
    OutMin = FVector2D(0.0f, 0.0f);
    OutMax = FVector2D(0.0f, 0.0f);
    OutCount = 0;
    if (!Graph)
    {
        return;
    }

    OutMin = FVector2D(FLT_MAX, FLT_MAX);
    OutMax = FVector2D(-FLT_MAX, -FLT_MAX);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) { continue; }
        if (!bIncludeStructural && IsStructuralNode(Node)) { continue; }
        const FVector2D Pos(Node->NodePosX, Node->NodePosY);
        const FVector2D Size = EstimateNodeSize(Node);
        OutMin.X = FMath::Min(OutMin.X, Pos.X);
        OutMin.Y = FMath::Min(OutMin.Y, Pos.Y);
        OutMax.X = FMath::Max(OutMax.X, Pos.X + Size.X);
        OutMax.Y = FMath::Max(OutMax.Y, Pos.Y + Size.Y);
        ++OutCount;
    }
    if (OutCount == 0)
    {
        OutMin = FVector2D(0.0f, 0.0f);
        OutMax = FVector2D(0.0f, 0.0f);
    }
}

bool FUnrealMCPCommonUtils::ValidateNodeBounds(UEdGraph* Graph, UEdGraphNode* NewNode, FString& OutErrorMessage)
{
    if (!Graph || !NewNode)
    {
        return true;
    }

    const FString Title = NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
    const int32 X = NewNode->NodePosX;
    const int32 Y = NewNode->NodePosY;

    // Absolute backstop: reject anything absurdly far from the origin.
    if (FMath::Abs((float)X) > GMaxNodeCoordinate || FMath::Abs((float)Y) > GMaxNodeCoordinate)
    {
        OutErrorMessage = FString::Printf(
            TEXT("Node '%s' at (%d, %d) is outside the allowed area (+/-%.0f on each axis). Use get_blueprint_node_bounds and place within the area."),
            *Title, X, Y, GMaxNodeCoordinate);
        return false;
    }

    // Drift cap: the new node must land within the box of the OTHER nodes plus a
    // generous margin, so a single astronomical jump is rejected while the box grows
    // normally. Structural nodes (knots/comments) are ignored so a stray reroute can't
    // define -- or be judged against -- the layout area.
    FVector2D Min(FLT_MAX, FLT_MAX);
    FVector2D Max(-FLT_MAX, -FLT_MAX);
    int32 OtherCount = 0;
    for (UEdGraphNode* Other : Graph->Nodes)
    {
        if (!Other || Other == NewNode || IsStructuralNode(Other)) { continue; }
        const FVector2D Pos(Other->NodePosX, Other->NodePosY);
        const FVector2D Size = EstimateNodeSize(Other);
        Min.X = FMath::Min(Min.X, Pos.X);
        Min.Y = FMath::Min(Min.Y, Pos.Y);
        Max.X = FMath::Max(Max.X, Pos.X + Size.X);
        Max.Y = FMath::Max(Max.Y, Pos.Y + Size.Y);
        ++OtherCount;
    }
    if (OtherCount > 0)
    {
        const float LoX = Min.X - GMaxPlacementDrift;
        const float HiX = Max.X + GMaxPlacementDrift;
        const float LoY = Min.Y - GMaxPlacementDrift;
        const float HiY = Max.Y + GMaxPlacementDrift;
        if (X < LoX || X > HiX || Y < LoY || Y > HiY)
        {
            OutErrorMessage = FString::Printf(
                TEXT("Node '%s' at (%d, %d) is too far from the existing graph. Current area X[%.0f..%.0f] Y[%.0f..%.0f] (max drift %.0f). Use get_blueprint_node_bounds and place within/near this area."),
                *Title, X, Y, Min.X, Max.X, Min.Y, Max.Y, GMaxPlacementDrift);
            return false;
        }
    }
    return true;
}

bool FUnrealMCPCommonUtils::ValidatePlacement(UEdGraph* Graph, UEdGraphNode* NewNode, FString& OutErrorMessage)
{
    if (!Graph || !NewNode)
    {
        return true;
    }

    if (!ValidateNodeBounds(Graph, NewNode, OutErrorMessage))
    {
        return false;
    }

    for (UEdGraphNode* Other : Graph->Nodes)
    {
        if (!Other || Other == NewNode)
        {
            continue;
        }
        if (NodesOverlap(Other, NewNode))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Node would overlap existing node '%s' (id %s) at (%d, %d). Pass a different node_position."),
                *Other->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                *Other->NodeGuid.ToString(),
                Other->NodePosX, Other->NodePosY);
            return false;
        }
    }
    return true;
}

bool FUnrealMCPCommonUtils::FinalizePlacedNode(UEdGraph* Graph, UEdGraphNode* Node, FString& OutErrorMessage)
{
    if (!Graph || !Node)
    {
        return true;
    }
    if (ValidatePlacement(Graph, Node, OutErrorMessage))
    {
        return true;
    }

    // Rejected: remove the just-created node so the graph is left unchanged.
    if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
    }
    else
    {
        Graph->RemoveNode(Node);
    }
    return false;
}

// Liang-Barsky: does segment P0->P1 intersect the axis-aligned box [Min, Max]?
static bool SegmentIntersectsAABB(const FVector2D& P0, const FVector2D& P1, const FVector2D& Min, const FVector2D& Max)
{
    const FVector2D D = P1 - P0;
    float T0 = 0.0f;
    float T1 = 1.0f;
    for (int32 Axis = 0; Axis < 2; ++Axis)
    {
        const float P = (Axis == 0) ? P0.X : P0.Y;
        const float Dir = (Axis == 0) ? D.X : D.Y;
        const float Lo = (Axis == 0) ? Min.X : Min.Y;
        const float Hi = (Axis == 0) ? Max.X : Max.Y;
        if (FMath::IsNearlyZero(Dir))
        {
            if (P < Lo || P > Hi)
            {
                return false; // parallel to this slab and outside it
            }
        }
        else
        {
            float TA = (Lo - P) / Dir;
            float TB = (Hi - P) / Dir;
            if (TA > TB) { Swap(TA, TB); }
            T0 = FMath::Max(T0, TA);
            T1 = FMath::Min(T1, TB);
            if (T0 > T1)
            {
                return false;
            }
        }
    }
    return true;
}

UEdGraphNode* FUnrealMCPCommonUtils::FindWireOverlap(UEdGraphNode* Source, UEdGraphNode* Target, FString& OutErrorMessage)
{
    if (!Source || !Target)
    {
        return nullptr;
    }
    UEdGraph* Graph = Source->GetGraph();
    if (!Graph || Target->GetGraph() != Graph)
    {
        return nullptr;
    }

    // Wire approx: source right-center -> target left-center (exact pin anchors need Slate).
    const FVector2D SSize = EstimateNodeSize(Source);
    const FVector2D TSize = EstimateNodeSize(Target);
    const FVector2D P0(Source->NodePosX + SSize.X, Source->NodePosY + SSize.Y * 0.5f);
    const FVector2D P1(Target->NodePosX, Target->NodePosY + TSize.Y * 0.5f);

    for (UEdGraphNode* Other : Graph->Nodes)
    {
        if (!Other || Other == Source || Other == Target || IsStructuralNode(Other))
        {
            continue;
        }
        const FVector2D OMin(Other->NodePosX, Other->NodePosY);
        const FVector2D OMax = OMin + EstimateNodeSize(Other);
        if (SegmentIntersectsAABB(P0, P1, OMin, OMax))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Connection would cross node '%s' (id %s) at (%d, %d). Re-route so the wire does not pass over another node."),
                *Other->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                *Other->NodeGuid.ToString(),
                Other->NodePosX, Other->NodePosY);
            return Other;
        }
    }
    return nullptr;
}

bool FUnrealMCPCommonUtils::IsStructuralNode(const UEdGraphNode* Node)
{
    // Comment nodes are containers (they are meant to enclose other nodes) and reroute/knot
    // nodes are tiny pass-through dots whose true size the estimate grossly overstates. Both
    // are excluded from overlap and wire-crossing checks so they do not swamp the results.
    return Node && (Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>());
}

bool FUnrealMCPCommonUtils::NodesOverlap(const UEdGraphNode* A, const UEdGraphNode* B)
{
    if (IsStructuralNode(A) || IsStructuralNode(B))
    {
        return false;
    }
    if (!A || !B || A == B)
    {
        return false;
    }
    const FVector2D APos(A->NodePosX, A->NodePosY);
    const FVector2D BPos(B->NodePosX, B->NodePosY);
    const FVector2D ASize = EstimateNodeSize(A);
    const FVector2D BSize = EstimateNodeSize(B);
    return (APos.X < BPos.X + BSize.X) && (BPos.X < APos.X + ASize.X) &&
           (APos.Y < BPos.Y + BSize.Y) && (BPos.Y < APos.Y + ASize.Y);
}

void FUnrealMCPCommonUtils::CollectGraphEdges(UEdGraph* Graph, TArray<TPair<UEdGraphNode*, UEdGraphNode*>>& OutEdges)
{
    if (!Graph)
    {
        return;
    }
    TSet<FString> Seen;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Other || Other == Node)
                {
                    continue;
                }
                const FString Key = Node->NodeGuid.ToString() + TEXT("->") + Other->NodeGuid.ToString();
                if (Seen.Contains(Key))
                {
                    continue;
                }
                Seen.Add(Key);
                OutEdges.Add(TPair<UEdGraphNode*, UEdGraphNode*>(Node, Other));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Graph auto-layout (layered, left -> right)
// ---------------------------------------------------------------------------

void FUnrealMCPCommonUtils::LayeredLayout(const FLayoutInput& In, float ColGap, float RowGap, const FVector2D& Origin, FLayoutOutput& Out)
{
    const int32 N = In.NodeCount;
    Out.Positions.Init(FVector2D::ZeroVector, FMath::Max(0, N));
    Out.EdgeKnots.Reset();
    Out.EdgeKnots.SetNum(In.Edges.Num());
    if (N <= 0)
    {
        return;
    }

    auto SizeOf = [&In](int32 i) -> FVector2D
    {
        if (In.Sizes.IsValidIndex(i) && In.Sizes[i].X > 0.0f && In.Sizes[i].Y > 0.0f)
        {
            return In.Sizes[i];
        }
        return FVector2D(220.0f, 100.0f);
    };

    TArray<TArray<int32>> Succs; Succs.SetNum(N);
    TArray<TArray<int32>> Preds; Preds.SetNum(N);
    for (const TPair<int32, int32>& E : In.Edges)
    {
        if (!Succs.IsValidIndex(E.Key) || !Succs.IsValidIndex(E.Value) || E.Key == E.Value) { continue; }
        Succs[E.Key].Add(E.Value);
        Preds[E.Value].Add(E.Key);
    }

    // Longest-path ranking from sources; a back-edge (cycle) is treated as a source
    // boundary so ranking terminates.
    TArray<int32> Rank; Rank.Init(-1, N);
    TArray<bool> OnStack; OnStack.Init(false, N);
    TFunction<int32(int32)> RankOf;
    RankOf = [&](int32 i) -> int32
    {
        if (Rank[i] >= 0) { return Rank[i]; }
        if (OnStack[i]) { return 0; }
        OnStack[i] = true;
        int32 R = 0;
        for (int32 P : Preds[i]) { R = FMath::Max(R, RankOf(P) + 1); }
        OnStack[i] = false;
        Rank[i] = R;
        return R;
    };
    for (int32 i = 0; i < N; ++i) { RankOf(i); }

    int32 MaxRank = 0;
    for (int32 i = 0; i < N; ++i) { MaxRank = FMath::Max(MaxRank, Rank[i]); }

    TArray<TArray<int32>> RankNodes; RankNodes.SetNum(MaxRank + 1);
    for (int32 i = 0; i < N; ++i) { RankNodes[Rank[i]].Add(i); }

    // Order within each rank to reduce crossings (barycenter sweeps).
    TArray<int32> OrderIndex; OrderIndex.Init(0, N);
    for (int32 r = 0; r <= MaxRank; ++r)
    {
        for (int32 k = 0; k < RankNodes[r].Num(); ++k) { OrderIndex[RankNodes[r][k]] = k; }
    }
    auto Barycenter = [&](int32 Node, const TArray<TArray<int32>>& Neighbors) -> float
    {
        float Sum = 0.0f;
        int32 Cnt = 0;
        for (int32 Nb : Neighbors[Node]) { Sum += (float)OrderIndex[Nb]; ++Cnt; }
        return Cnt > 0 ? (Sum / (float)Cnt) : (float)OrderIndex[Node];
    };
    for (int32 Iter = 0; Iter < 4; ++Iter)
    {
        for (int32 r = 1; r <= MaxRank; ++r)
        {
            RankNodes[r].Sort([&](int32 a, int32 b) { return Barycenter(a, Preds) < Barycenter(b, Preds); });
            for (int32 k = 0; k < RankNodes[r].Num(); ++k) { OrderIndex[RankNodes[r][k]] = k; }
        }
        for (int32 r = MaxRank - 1; r >= 0; --r)
        {
            RankNodes[r].Sort([&](int32 a, int32 b) { return Barycenter(a, Succs) < Barycenter(b, Succs); });
            for (int32 k = 0; k < RankNodes[r].Num(); ++k) { OrderIndex[RankNodes[r][k]] = k; }
        }
    }

    // Column X: each column starts after the previous column's widest node + ColGap, so
    // every left->right wire is roughly ColGap long (< the connect-length limit).
    TArray<float> ColX; ColX.SetNum(MaxRank + 1);
    float X = Origin.X;
    for (int32 r = 0; r <= MaxRank; ++r)
    {
        ColX[r] = X;
        float W = 0.0f;
        for (int32 i : RankNodes[r]) { W = FMath::Max(W, (float)SizeOf(i).X); }
        X += W + ColGap;
    }

    // Vertical stacking, centred on Origin.Y, as a seed for the straightening pass below.
    for (int32 r = 0; r <= MaxRank; ++r)
    {
        float TotalH = 0.0f;
        for (int32 i : RankNodes[r]) { TotalH += SizeOf(i).Y + RowGap; }
        if (RankNodes[r].Num() > 0) { TotalH -= RowGap; }
        float Y = Origin.Y - TotalH * 0.5f;
        for (int32 i : RankNodes[r])
        {
            Out.Positions[i] = FVector2D(ColX[r], Y);
            Y += SizeOf(i).Y + RowGap;
        }
    }

    // Coordinate assignment / straightening: pull each node toward the mean y of its
    // neighbours -- this is what minimises connection length and removes the diagonal
    // swoop -- then resolve within-column overlap by pushing nodes apart. Alternating
    // left->right (predecessors) and right->left (successors) sweeps converge to straight,
    // short wires.
    auto AlignRank = [&](int32 r, const TArray<TArray<int32>>& Neighbors)
    {
        if (RankNodes[r].Num() == 0) { return; }
        TArray<TPair<float, int32>> SortedByWant;
        SortedByWant.Reserve(RankNodes[r].Num());
        for (int32 i : RankNodes[r])
        {
            float Sum = 0.0f;
            int32 Cnt = 0;
            for (int32 Nb : Neighbors[i]) { Sum += Out.Positions[Nb].Y; ++Cnt; }
            const float Want = (Cnt > 0) ? (Sum / (float)Cnt) : Out.Positions[i].Y;
            SortedByWant.Add(TPair<float, int32>(Want, i));
        }
        SortedByWant.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });

        // Anchor the block at the topmost desired y and stack downward. Anchoring (rather
        // than re-centring on the mean) keeps the pass drift-free: each rank is pulled
        // straight toward its already-placed predecessors, mirroring them, with no
        // feedback loop to walk the whole layout off-origin.
        float Y = SortedByWant[0].Key;
        for (const TPair<float, int32>& D : SortedByWant)
        {
            const FVector2D Sz = SizeOf(D.Value);
            Y = FMath::Max(Y, D.Key); // sit at the aligned y, but never ride up into the node above
            Out.Positions[D.Value] = FVector2D(ColX[r], Y);
            Y += Sz.Y + RowGap;
        }
    };
    // One left->right pass suffices (every rank's predecessors are already final); a second
    // pass smooths second-order effects without introducing drift.
    for (int32 Iter = 0; Iter < 2; ++Iter)
    {
        for (int32 r = 1; r <= MaxRank; ++r) { AlignRank(r, Preds); }
    }

    // Occupied vertical span of each column, used to nudge knots clear of the nodes so a
    // wire routed through a knot does not cut across them.
    TArray<float> ColNodeMinY; ColNodeMinY.Init(0.0f, MaxRank + 1);
    TArray<float> ColNodeMaxY; ColNodeMaxY.Init(0.0f, MaxRank + 1);
    for (int32 r = 0; r <= MaxRank; ++r)
    {
        float Mn = FLT_MAX;
        float Mx = -FLT_MAX;
        for (int32 i : RankNodes[r])
        {
            Mn = FMath::Min(Mn, Out.Positions[i].Y);
            Mx = FMath::Max(Mx, Out.Positions[i].Y + (float)SizeOf(i).Y);
        }
        ColNodeMinY[r] = (Mn == FLT_MAX) ? 0.0f : Mn;
        ColNodeMaxY[r] = (Mx == -FLT_MAX) ? 0.0f : Mx;
    }

    // Knot chains for edges spanning more than one column.
    for (int32 e = 0; e < In.Edges.Num(); ++e)
    {
        const int32 u = In.Edges[e].Key;
        const int32 v = In.Edges[e].Value;
        if (!Out.Positions.IsValidIndex(u) || !Out.Positions.IsValidIndex(v)) { continue; }
        const int32 ru = Rank[u];
        const int32 rv = Rank[v];
        if (rv - ru <= 1) { continue; } // adjacent (or back edge): a direct wire is fine
        for (int32 r = ru + 1; r < rv; ++r)
        {
            const float T = (float)(r - ru) / (float)(rv - ru);
            float Y = FMath::Lerp(Out.Positions[u].Y, Out.Positions[v].Y, T);
            // If the knot would sit within (or beside) the column's node band, lift it just
            // above or drop it just below -- whichever is nearer -- so the through-wire
            // clears the nodes instead of cutting across them.
            if (RankNodes[r].Num() > 0)
            {
                const float KnotClearance = 60.0f;
                const float Band = 40.0f;
                if (Y + Band > ColNodeMinY[r] && Y < ColNodeMaxY[r] + Band)
                {
                    const float Above = ColNodeMinY[r] - KnotClearance;
                    const float Below = ColNodeMaxY[r] + KnotClearance;
                    Y = (FMath::Abs(Y - Above) <= FMath::Abs(Y - Below)) ? Above : Below;
                }
            }
            Out.EdgeKnots[e].Add(FVector2D(ColX[r], Y));
        }
    }
}

void FUnrealMCPCommonUtils::CollectLogicEdges(UEdGraph* Graph, TArray<FGraphEdge>& Out)
{
    Out.Reset();
    if (!Graph)
    {
        return;
    }
    TSet<FString> Seen;

    // Resolve a linked pin to the real (non-knot) downstream endpoints, following any
    // chain of reroute knots in between.
    TFunction<void(UEdGraphNode*, UEdGraphPin*, TArray<TPair<UEdGraphNode*, FString>>&, TSet<const UEdGraphNode*>&)> Walk;
    Walk = [&](UEdGraphNode* N, UEdGraphPin* Pin, TArray<TPair<UEdGraphNode*, FString>>& Targets, TSet<const UEdGraphNode*>& Visited)
    {
        if (!N) { return; }
        if (IsStructuralNode(N))
        {
            if (Visited.Contains(N)) { return; }
            Visited.Add(N);
            for (UEdGraphPin* OP : N->Pins)
            {
                if (!OP || OP->Direction != EGPD_Output) { continue; }
                for (UEdGraphPin* L : OP->LinkedTo)
                {
                    Walk(L ? L->GetOwningNode() : nullptr, L, Targets, Visited);
                }
            }
            return;
        }
        Targets.Add(TPair<UEdGraphNode*, FString>(N, Pin ? Pin->PinName.ToString() : FString()));
    };

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || IsStructuralNode(Node)) { continue; }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output) { continue; }
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Other) { continue; }
                TArray<TPair<UEdGraphNode*, FString>> Targets;
                TSet<const UEdGraphNode*> Visited;
                Walk(Other, Linked, Targets, Visited);
                for (const TPair<UEdGraphNode*, FString>& T : Targets)
                {
                    if (!T.Key || T.Key == Node) { continue; }
                    const FString Key = Node->NodeGuid.ToString() + TEXT(":") + Pin->PinName.ToString()
                        + TEXT("->") + T.Key->NodeGuid.ToString() + TEXT(":") + T.Value;
                    if (Seen.Contains(Key)) { continue; }
                    Seen.Add(Key);
                    FGraphEdge Edge;
                    Edge.Src = Node;
                    Edge.SrcPin = Pin->PinName.ToString();
                    Edge.Dst = T.Key;
                    Edge.DstPin = T.Value;
                    Out.Add(Edge);
                }
            }
        }
    }
}

void FUnrealMCPCommonUtils::RemoveStructuralKnots(UEdGraph* Graph, UBlueprint* Blueprint)
{
    if (!Graph)
    {
        return;
    }
    TArray<UEdGraphNode*> Knots;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->IsA<UK2Node_Knot>()) { Knots.Add(Node); }
    }
    for (UEdGraphNode* Knot : Knots)
    {
        if (Blueprint)
        {
            FBlueprintEditorUtils::RemoveNode(Blueprint, Knot, /*bDontRecompile*/ true);
        }
        else
        {
            Graph->RemoveNode(Knot);
        }
    }
}

bool FUnrealMCPCommonUtils::ConnectWithKnots(UEdGraph* Graph, UEdGraphNode* Src, const FString& SrcPin,
                                             UEdGraphNode* Dst, const FString& DstPin, const TArray<FVector2D>& KnotPath)
{
    if (!Graph || !Src || !Dst)
    {
        return false;
    }

    UEdGraphNode* PrevNode = Src;
    FString PrevPin = SrcPin;
    for (const FVector2D& KnotPos : KnotPath)
    {
        UK2Node_Knot* Knot = CreateKnotNode(Graph, KnotPos);
        if (!Knot)
        {
            return false;
        }
        // Knot pins are a single input/output pair; resolve their names by direction.
        FString KnotIn = TEXT("InputPin");
        FString KnotOut = TEXT("OutputPin");
        for (UEdGraphPin* P : Knot->Pins)
        {
            if (!P) { continue; }
            if (P->Direction == EGPD_Input) { KnotIn = P->PinName.ToString(); }
            else if (P->Direction == EGPD_Output) { KnotOut = P->PinName.ToString(); }
        }
        if (!ConnectGraphNodes(Graph, PrevNode, PrevPin, Knot, KnotIn))
        {
            return false;
        }
        PrevNode = Knot;
        PrevPin = KnotOut;
    }

    return ConnectGraphNodes(Graph, PrevNode, PrevPin, Dst, DstPin);
} 