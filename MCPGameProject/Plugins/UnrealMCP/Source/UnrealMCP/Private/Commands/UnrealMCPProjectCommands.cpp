#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "GameFramework/InputSettings.h"

FUnrealMCPProjectCommands::FUnrealMCPProjectCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("create_input_mapping"))
    {
        return HandleCreateInputMapping(Params);
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown project command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    FString Key;
    if (!Params->TryGetStringField(TEXT("key"), Key))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter"));
    }

    // Get the input settings
    UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
    if (!InputSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
    }

    // input_type selects an Action mapping (default) or an Axis mapping.
    FString InputType = TEXT("Action");
    Params->TryGetStringField(TEXT("input_type"), InputType);

    if (InputType.Equals(TEXT("Axis"), ESearchCase::IgnoreCase))
    {
        FInputAxisKeyMapping AxisMapping;
        AxisMapping.AxisName = FName(*ActionName);
        AxisMapping.Key = FKey(*Key);
        AxisMapping.Scale = 1.0f;
        if (Params->HasField(TEXT("scale"))) { AxisMapping.Scale = (float)Params->GetNumberField(TEXT("scale")); }
        InputSettings->AddAxisMapping(AxisMapping);
    }
    else
    {
        FInputActionKeyMapping ActionMapping;
        ActionMapping.ActionName = FName(*ActionName);
        ActionMapping.Key = FKey(*Key);
        if (Params->HasField(TEXT("shift"))) { ActionMapping.bShift = Params->GetBoolField(TEXT("shift")); }
        if (Params->HasField(TEXT("ctrl"))) { ActionMapping.bCtrl = Params->GetBoolField(TEXT("ctrl")); }
        if (Params->HasField(TEXT("alt"))) { ActionMapping.bAlt = Params->GetBoolField(TEXT("alt")); }
        if (Params->HasField(TEXT("cmd"))) { ActionMapping.bCmd = Params->GetBoolField(TEXT("cmd")); }
        InputSettings->AddActionMapping(ActionMapping);
    }
    InputSettings->SaveConfig();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), ActionName);
    ResultObj->SetStringField(TEXT("key"), Key);
    ResultObj->SetStringField(TEXT("input_type"), InputType.Equals(TEXT("Axis"), ESearchCase::IgnoreCase) ? TEXT("Axis") : TEXT("Action"));
    return ResultObj;
} 