#include "Tools/SageDialogTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CoreDelegates.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#include <atomic>

namespace sage::tools
{
namespace
{

struct FDialogPolicy
{
    FString              Pattern;
    EAppReturnType::Type Response;
};

// Module-scoped state. ModalMessageDialog fires on the GameThread; every
// MCP handler is GT-marshalled via RunOnGameThread, so policy reads/writes
// are single-threaded by construction. The atomic on bHookInstalled is
// belt-and-braces — module shutdown could in theory race the lazy install.
TArray<FDialogPolicy>& GetPolicies()
{
    static TArray<FDialogPolicy> Policies;
    return Policies;
}

std::atomic<bool> bHookInstalled{false};

EAppReturnType::Type ParseResponse(const FString& In)
{
    const FString L = In.ToLower();
    if (L == TEXT("yes"))      return EAppReturnType::Yes;
    if (L == TEXT("no"))       return EAppReturnType::No;
    if (L == TEXT("ok"))       return EAppReturnType::Ok;
    if (L == TEXT("cancel"))   return EAppReturnType::Cancel;
    if (L == TEXT("retry"))    return EAppReturnType::Retry;
    if (L == TEXT("continue")) return EAppReturnType::Continue;
    if (L == TEXT("yesall"))   return EAppReturnType::YesAll;
    if (L == TEXT("noall"))    return EAppReturnType::NoAll;
    return EAppReturnType::Ok;
}

FString ResponseToString(EAppReturnType::Type R)
{
    switch (R)
    {
    case EAppReturnType::Yes:      return TEXT("yes");
    case EAppReturnType::No:       return TEXT("no");
    case EAppReturnType::Ok:       return TEXT("ok");
    case EAppReturnType::Cancel:   return TEXT("cancel");
    case EAppReturnType::Retry:    return TEXT("retry");
    case EAppReturnType::Continue: return TEXT("continue");
    case EAppReturnType::YesAll:   return TEXT("yesall");
    case EAppReturnType::NoAll:    return TEXT("noall");
    default:                       return TEXT("unknown");
    }
}

// Conservative fall-through: if no policy matches, prefer the "safe" answer
// (no/cancel) so an unattended agent never accidentally accepts a destructive
// dialog. Plain Ok prompts pass through as Ok (no choice to make).
EAppReturnType::Type DefaultResponseFor(EAppMsgType::Type MsgType)
{
    switch (MsgType)
    {
    case EAppMsgType::Ok:                     return EAppReturnType::Ok;
    case EAppMsgType::YesNo:                  return EAppReturnType::No;
    case EAppMsgType::YesNoCancel:            return EAppReturnType::No;
    case EAppMsgType::OkCancel:               return EAppReturnType::Cancel;
    case EAppMsgType::CancelRetryContinue:    return EAppReturnType::Cancel;
    case EAppMsgType::YesNoYesAllNoAll:       return EAppReturnType::No;
    case EAppMsgType::YesNoYesAllNoAllCancel: return EAppReturnType::Cancel;
    case EAppMsgType::YesNoYesAll:            return EAppReturnType::No;
    default:                                  return EAppReturnType::No;
    }
}

EAppReturnType::Type HandleModalDialogV2(EAppMsgCategory /*Category*/,
                                         EAppMsgType::Type MsgType,
                                         const FText& Text,
                                         const FText& Title)
{
    const FString MessageStr = Text.ToString();
    const FString TitleStr   = Title.ToString();

    for (const FDialogPolicy& P : GetPolicies())
    {
        if (MessageStr.Contains(P.Pattern) || TitleStr.Contains(P.Pattern))
        {
            UE_LOG(LogSageBridge, Log,
                   TEXT("Dialog auto-responded: pattern='%s' title='%s' response=%s"),
                   *P.Pattern, *TitleStr, *ResponseToString(P.Response));
            return P.Response;
        }
    }

    const EAppReturnType::Type Default = DefaultResponseFor(MsgType);
    UE_LOG(LogSageBridge, Log,
           TEXT("Dialog (no policy match): title='%s' message='%s' default=%s"),
           *TitleStr, *MessageStr.Left(200), *ResponseToString(Default));
    return Default;
}

// ---- handler implementations --------------------------------------------

FSageToolDispatch::FOutcome SetPolicyImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));

    FString Pattern;
    if (!Args->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'pattern'"));

    FString ResponseStr;
    if (!Args->TryGetStringField(TEXT("response"), ResponseStr) || ResponseStr.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'response'"));

    const EAppReturnType::Type Resp = ParseResponse(ResponseStr);

    auto& Policies = GetPolicies();

    bool bReplaced = false;
    for (FDialogPolicy& P : Policies)
    {
        if (P.Pattern == Pattern)
        {
            P.Response = Resp;
            bReplaced = true;
            break;
        }
    }
    if (!bReplaced)
    {
        Policies.Add({Pattern, Resp});
    }

    InstallDialogHook();  // idempotent

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("pattern"),      Pattern);
    R->SetStringField(TEXT("response"),     ResponseToString(Resp));
    R->SetBoolField  (TEXT("replaced"),     bReplaced);
    R->SetNumberField(TEXT("policy_count"), Policies.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ClearPolicyImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto& Policies = GetPolicies();

    FString Pattern;
    int32   Removed = 0;
    if (Args.IsValid() &&
        Args->TryGetStringField(TEXT("pattern"), Pattern) && !Pattern.IsEmpty())
    {
        Removed = Policies.RemoveAll(
            [&Pattern](const FDialogPolicy& P) { return P.Pattern == Pattern; });
    }
    else
    {
        Removed = Policies.Num();
        Policies.Empty();
    }

    auto R = MakeShared<FJsonObject>();
    if (!Pattern.IsEmpty()) R->SetStringField(TEXT("pattern"), Pattern);
    R->SetNumberField(TEXT("removed"),      Removed);
    R->SetNumberField(TEXT("policy_count"), Policies.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetPolicyImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    const auto& Policies = GetPolicies();

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Policies.Num());
    for (const FDialogPolicy& P : Policies)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("pattern"),  P.Pattern);
        O->SetStringField(TEXT("response"), ResponseToString(P.Response));
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("policies"),       Arr);
    R->SetNumberField(TEXT("count"),          Arr.Num());
    R->SetBoolField  (TEXT("hook_installed"), bHookInstalled.load());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

void TraverseTextsAndButtons(const TSharedRef<SWidget>& W,
                             TArray<FString>& Texts,
                             TArray<FString>& ButtonLabels)
{
    if (W->GetType() == TEXT("STextBlock"))
    {
        const FString S = StaticCastSharedRef<STextBlock>(W)->GetText().ToString();
        if (!S.IsEmpty()) Texts.Add(S);
    }
    if (W->GetType() == TEXT("SButton"))
    {
        if (FChildren* C = W->GetChildren())
        {
            for (int32 i = 0; i < C->Num(); ++i)
            {
                TSharedRef<SWidget> Ch = C->GetChildAt(i);
                if (Ch->GetType() == TEXT("STextBlock"))
                {
                    ButtonLabels.Add(
                        StaticCastSharedRef<STextBlock>(Ch)->GetText().ToString());
                    break;
                }
            }
        }
    }
    if (FChildren* C = W->GetChildren())
    {
        for (int32 i = 0; i < C->Num(); ++i)
            TraverseTextsAndButtons(C->GetChildAt(i), Texts, ButtonLabels);
    }
}

FSageToolDispatch::FOutcome ListDialogsImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    TArray<TSharedPtr<FJsonValue>> Out;

    if (FSlateApplication::IsInitialized())
    {
        TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
        if (Modal.IsValid())
        {
            const FString TitleStr = Modal->GetTitle().ToString();
            TArray<FString> Texts;
            TArray<FString> ButtonLabels;
            TraverseTextsAndButtons(Modal.ToSharedRef(), Texts, ButtonLabels);

            FString Message;
            for (const FString& T : Texts)
            {
                if (T == TitleStr) continue;
                if (!Message.IsEmpty()) Message += TEXT("\n");
                Message += T;
            }

            auto Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("title"),   TitleStr);
            Obj->SetStringField(TEXT("message"), Message);

            TArray<TSharedPtr<FJsonValue>> ButtonsJson;
            for (const FString& B : ButtonLabels)
                ButtonsJson.Add(MakeShared<FJsonValueString>(B));
            Obj->SetArrayField(TEXT("buttons"), ButtonsJson);

            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("dialogs"), Out);
    R->SetNumberField(TEXT("count"),   Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

void CollectButtons(const TSharedRef<SWidget>& W,
                    TArray<TSharedRef<SButton>>& Btns,
                    TArray<FString>& Labels)
{
    if (W->GetType() == TEXT("SButton"))
    {
        Btns.Add(StaticCastSharedRef<SButton>(W));

        FString Label;
        if (FChildren* C = W->GetChildren())
        {
            for (int32 i = 0; i < C->Num(); ++i)
            {
                TSharedRef<SWidget> Ch = C->GetChildAt(i);
                if (Ch->GetType() == TEXT("STextBlock"))
                {
                    Label = StaticCastSharedRef<STextBlock>(Ch)->GetText().ToString();
                    break;
                }
            }
        }
        Labels.Add(Label);
    }
    if (FChildren* C = W->GetChildren())
    {
        for (int32 i = 0; i < C->Num(); ++i)
            CollectButtons(C->GetChildAt(i), Btns, Labels);
    }
}

FSageToolDispatch::FOutcome RespondToDialogImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!FSlateApplication::IsInitialized())
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("Slate not initialized"));

    TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
    if (!Modal.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32004,
            TEXT("no active modal dialog"));

    FString ButtonLabel;
    int32   ButtonIndex = -1;
    FString Action;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("button_label"), ButtonLabel);
        Args->TryGetNumberField(TEXT("button_index"), ButtonIndex);
        Args->TryGetStringField(TEXT("action"),       Action);
    }

    TArray<TSharedRef<SButton>> Btns;
    TArray<FString>             Labels;
    CollectButtons(Modal.ToSharedRef(), Btns, Labels);

    int32 Target = -1;
    if (!ButtonLabel.IsEmpty())
    {
        for (int32 i = 0; i < Labels.Num(); ++i)
        {
            if (Labels[i].Contains(ButtonLabel))
            {
                Target = i;
                break;
            }
        }
    }
    else if (ButtonIndex >= 0 && ButtonIndex < Btns.Num())
    {
        Target = ButtonIndex;
    }

    auto R = MakeShared<FJsonObject>();

    if (Target >= 0)
    {
        TSharedRef<SButton> Btn = Btns[Target];
        FSlateApplication::Get().SetKeyboardFocus(Btn);

        const FGeometry G       = Btn->GetCachedGeometry();
        const FVector2D Center  = G.LocalToAbsolute(G.GetLocalSize() * 0.5f);
        FPointerEvent  Click(0, Center, Center, TSet<FKey>(),
                             EKeys::LeftMouseButton, 0, FModifierKeysState());
        Btn->OnMouseButtonDown(G, Click);
        Btn->OnMouseButtonUp  (G, Click);

        R->SetStringField(TEXT("clicked_button"), Labels[Target]);
        R->SetNumberField(TEXT("button_index"),   Target);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (Action == TEXT("escape"))
    {
        FSlateApplication::Get().ProcessKeyDownEvent(
            FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
        FSlateApplication::Get().ProcessKeyUpEvent(
            FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
        R->SetStringField(TEXT("action"), TEXT("escape"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    TArray<TSharedPtr<FJsonValue>> Available;
    for (const FString& L : Labels)
        Available.Add(MakeShared<FJsonValueString>(L));
    R->SetArrayField(TEXT("available_buttons"), Available);
    R->SetStringField(TEXT("hint"),
        TEXT("provide button_index, button_label (substring), or action='escape'"));
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("button not found"));
}

}  // namespace (anonymous)

void InstallDialogHook()
{
    if (bHookInstalled.exchange(true)) return;
    FCoreDelegates::ModalMessageDialog.BindStatic(&HandleModalDialogV2);
    UE_LOG(LogSageBridge, Log,
           TEXT("Dialog hook installed (FCoreDelegates::ModalMessageDialog bound)"));
}

void RemoveDialogHook()
{
    if (!bHookInstalled.exchange(false)) return;
    FCoreDelegates::ModalMessageDialog.Unbind();
    GetPolicies().Empty();
    UE_LOG(LogSageBridge, Log, TEXT("Dialog hook removed"));
}

void RegisterDialogTools(FSageToolDispatch& Dispatch)
{
    auto GT = [](FSageToolDispatch::FOutcome (*Fn)(const TSharedPtr<FJsonObject>&))
    {
        return [Fn](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
        {
            return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
            {
                return Fn(Args);
            });
        };
    };

    Dispatch.RegisterHandler(TEXT("editor.set_dialog_policy"),   GT(&SetPolicyImpl));
    Dispatch.RegisterHandler(TEXT("editor.clear_dialog_policy"), GT(&ClearPolicyImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_dialog_policy"),   GT(&GetPolicyImpl));
    Dispatch.RegisterHandler(TEXT("editor.list_dialogs"),        GT(&ListDialogsImpl));
    Dispatch.RegisterHandler(TEXT("editor.respond_to_dialog"),   GT(&RespondToDialogImpl));
}

}  // namespace sage::tools
