#ifndef ZELDAONLINEROOMWINDOWH
#define ZELDAONLINEROOMWINDOWH
#include <ship/window/gui/GuiWindow.h>
#include <ship/window/Window.h>
#include <ship/Context.h>
#include "soh/util.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <soh/cvar_prefixes.h>
#include <ship/window/gui/IconsFontAwesome4.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdlib>
#include <SDL2/SDL.h>
#include "PartySettings.hpp"

const char* ResolveSceneID(int sceneID, int roomID);

namespace ZeldaOnline {
struct SkinOption {
    std::string displayName;
    std::string referenceName;
    float pitch = 1.0f;
};

struct PlayerEntry {
    std::string name;
    uint32_t networkID = 0;
    bool isInParty = false;
    uint32_t pendingInvitePartyID = 0;
    int16_t sceneNum = 0;
    uint8_t roomIndex = 0;
    uint8_t age = 0;
};

enum class PlayerListAction {
    None,
    Invite,
    AcceptInvite,
    DeclineInvite,
    CreateParty,
    LeaveParty,
    PartySettingChanged,
    TeleportTo,
    ChangeSkin,
    ToggleSkinPitch,
    SetPvp,
    ChangeName,
    Connect,
    Disconnect,
    UnstuckMe,
    Chat
};

struct PlayerListActionEvent {
    PlayerListAction action = PlayerListAction::None;
};

struct PlayerListActionEventInvite {
    PlayerListActionEvent actionEvent;
    uint32_t networkID = 0;
    uint32_t partyID = 0;
};

struct PlayerListActionEventText {
    PlayerListActionEvent actionEvent;
    std::string text;
};

struct PlayerListActionEventMultiVar {
    PlayerListActionEvent actionEvent;
    std::string text;
    int32_t intValue = 0;
    float floatValue = 0.0f;
};

struct PlayerListActionEventToggle {
    PlayerListActionEvent actionEvent;
    bool enabled = false;
};

struct PlayerListActionEventConnect {
    PlayerListActionEvent actionEvent;
    std::string host;
    uint16_t port = 0;
    std::string nickname;
    std::string fileServer;
};

class ZeldaOnlineRoomWindow : public Ship::GuiWindow {
  private:
    static constexpr float LONG_PRESS_SECONDS = 0.5f;

    ImGuiID m_heldRowId = 0;
    float m_heldRowTime = 0.0f;
    bool m_heldRowFired = false;

    bool m_chatButtonShown = false;
    bool m_chatBoxOpen = false;
    float m_chatBoxAnim = 0.0f;
    bool m_chatBoxFocus = false;
    bool m_applySkinPitch = true;
    bool m_pvpEnabled = false;
    bool m_chatSettingsLoaded = false;
    bool m_chatKeyboardShown = false;
    bool m_chatKeyboardProbed = false;
    bool m_chatButtonPlaced = false;
    float m_chatButtonX = -1.0f;
    float m_chatButtonY = -1.0f;
    char m_chatBuffer[512] = "";

    std::vector<PlayerEntry> m_players;
    std::vector<SkinOption> m_availableSkins;
    std::string m_currentSkin;
    std::function<void(const PlayerListActionEvent*)> m_onAction;
    bool m_hasParty = false;
    bool m_partyAdmin = false;
    uint32_t m_partyAdminID = 0;
    bool m_partySettingsOpen = false;
    PartySettings m_partySettings;
    PartySettings m_partySettingsBackup;
    std::string m_statusText;
    ImVec4 m_statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    char m_nameBuffer[256] = "";
    bool m_recenterNextFrame = false;

    bool m_beginAutoConnect = false;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_fieldsLoaded = false;
    bool m_autoConnect = false;
    char m_hostBuffer[256] = "";
    int m_portValue = 0;
    char m_fileServerBuffer[512] = "";

    PlayerEntry* FindPlayerEntry(uint32_t networkID) {
        for (auto& entry : m_players) {
            if (entry.networkID == networkID)
                return &entry;
        }
        return nullptr;
    }

    bool HasPendingInvite() const {
        for (const auto& entry : m_players) {
            if (entry.pendingInvitePartyID != 0)
                return true;
        }
        return false;
    }

    std::string BuildWindowTitle() const {
        std::string title = "Zelda Online";

        if (m_connected) {
            title += " (" + std::to_string((int)(m_players.size())) + ")";

            if (HasPendingInvite())
                title += " " ICON_FA_ENVELOPE;
        }

        title += "###zo_room";
        return title;
    }

    int PartyMemberCount() const {
        int count = 0;
        for (const auto& entry : m_players) {
            if (entry.isInParty)
                count++;
        }
        return count;
    }

    void DrawMenuBar(PlayerListActionEventText& textEvent, PlayerListActionEvent*& pending) {
        if (!ImGui::BeginMenuBar())
            return;

        if (!m_hasParty) {
            if (ImGui::Button(ICON_FA_USER_PLUS " Create Party")) {
                textEvent.actionEvent.action = PlayerListAction::CreateParty;
                pending = &textEvent.actionEvent;
            }
        } else {
            if (ImGui::Button(ICON_FA_SIGN_OUT " Leave Party")) {
                textEvent.actionEvent.action = PlayerListAction::LeaveParty;
                pending = &textEvent.actionEvent;
            }

            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_COG "##zo_party_settings")) {
                m_partySettingsOpen = !m_partySettingsOpen;

                if (m_partySettingsOpen)
                    m_partySettingsBackup = m_partySettings;
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Party settings");
        }

        ImGui::EndMenuBar();
    }

    void EmitPartySettingChanged() {
        if (!m_onAction)
            return;

        PlayerListActionEvent actionEvent;
        actionEvent.action = PlayerListAction::PartySettingChanged;
        m_onAction(&actionEvent);
    }

    void DrawPartySettings() {
        if (!m_partySettingsOpen)
            return;

        if (!m_hasParty) {
            m_partySettingsOpen = false;
            return;
        }

        bool accept = false;
        bool open = true;

        ImGui::SetNextWindowSize(ImVec2(320.0f, m_partyAdmin ? 480.0f : 550.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, m_partyAdmin ? 460.0f : 530.0f), ImVec2(FLT_MAX, FLT_MAX));

        if (ImGui::Begin("Party Settings###zo_party_settings_window", &open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            if (!m_partyAdmin) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                ImGui::TextWrapped(ICON_FA_LOCK " Only the party admin can change these settings.");
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::BeginDisabled();
            }

            if (ImGui::Checkbox("Private Dungeons", &m_partySettings.privateDungeons)) {
                if (!m_partySettings.privateDungeons) {
                    m_partySettings.extraEnemies = false;
                    m_partySettings.healthMultiplier = false;
                }
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Create a separate dungeon instance for your party.\n"
                                  "Only members of your party can enter the instance.");

            ImGui::Spacing();

            if (ImGui::Checkbox("Story Co-op", &m_partySettings.storyCoop)) {
                if (!m_partySettings.storyCoop)
                    m_partySettings.shareAmmoRupeesHp = false;
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Party members share progression, story flags and the Master Sword time travel.");

            if (!m_partySettings.storyCoop)
                ImGui::BeginDisabled();

            ImGui::Indent();
            ImGui::Checkbox("Share Ammo, Rupees & HP", &m_partySettings.shareAmmoRupeesHp);

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires Story Co-op. Ammo, rupees and health are shared between party members.");

            ImGui::Unindent();

            if (!m_partySettings.storyCoop)
                ImGui::EndDisabled();

            ImGui::Spacing();

            if (!m_partySettings.privateDungeons)
                ImGui::BeginDisabled();

            const float rowHeight = ImGui::GetFrameHeightWithSpacing();
            const float padY = ImGui::GetStyle().WindowPadding.y * 2.0f;
            const float labelHeight = ImGui::GetTextLineHeightWithSpacing();
            const float enemyHeight = (rowHeight * 2.0f) + padY;
            const float healthHeight = (rowHeight * 3.0f) + padY;
            const float modifierHeight =
                (labelHeight * 2.0f) + enemyHeight + healthHeight + (ImGui::GetStyle().ItemSpacing.y * 2.0f) + padY;

            ImGui::TextUnformatted("Dungeon Settings");

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires Private Dungeons.");

            if (ImGui::BeginChild("##zo_party_modifiers", ImVec2(0.0f, modifierHeight), true)) {
                ImGui::TextUnformatted("Extra Enemies");

                if (ImGui::BeginChild("##zo_extra_enemies", ImVec2(0.0f, enemyHeight), true)) {
                    ImGui::Checkbox("Enabled", &m_partySettings.extraEnemies);

                    if (!m_partySettings.extraEnemies)
                        ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##zo_extra_enemy_weight", &m_partySettings.extraEnemyWeight, 0.0f, 5.0f,
                                       "Weight %.1f", ImGuiSliderFlags_AlwaysClamp);

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Spawn weight for extra enemies.");

                    if (!m_partySettings.extraEnemies)
                        ImGui::EndDisabled();
                }

                ImGui::EndChild();

                ImGui::Spacing();
                ImGui::TextUnformatted("Health Multiplier");

                if (ImGui::BeginChild("##zo_health_multiplier", ImVec2(0.0f, healthHeight), true)) {
                    ImGui::Checkbox("Enabled", &m_partySettings.healthMultiplier);

                    if (!m_partySettings.healthMultiplier)
                        ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##zo_enemy_health_weight", &m_partySettings.enemyHealthWeight, 0.0f, 5.0f,
                                       "Enemy Health Weight %.1f", ImGuiSliderFlags_AlwaysClamp);

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(
                            "Health weight for regular enemies.\nHealth increases by 0.25 x Party Size x Weight");

                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##zo_boss_health_weight", &m_partySettings.bossHealthWeight, 0.0f, 5.0f,
                                       "Boss Health Weight %.1f", ImGuiSliderFlags_AlwaysClamp);

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Health weight for bosses.\nHealth increases by 0.25 x Party Size x Weight");

                    if (!m_partySettings.healthMultiplier)
                        ImGui::EndDisabled();
                }

                ImGui::EndChild();
            }

            ImGui::EndChild();

            if (!m_partySettings.privateDungeons)
                ImGui::EndDisabled();

            if (!m_partyAdmin)
                ImGui::EndDisabled();

            ImGui::Separator();

            if (m_partyAdmin) {
                if (ImGui::Button("OK", ImVec2(80.0f, 0.0f)))
                    accept = true;

                ImGui::SameLine();

                if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
                    open = false;
            } else {
                if (ImGui::Button("Close", ImVec2(80.0f, 0.0f)))
                    open = false;
            }
        }

        ImGui::End();

        if (accept) {
            m_partySettingsOpen = false;
            m_partySettingsBackup = m_partySettings;
            EmitPartySettingChanged();
            return;
        }

        if (!open) {
            m_partySettings = m_partySettingsBackup;
            m_partySettingsOpen = false;
            return;
        }

        m_partySettingsOpen = open;
    }

    static std::string LocationLabel(const PlayerEntry& entry) {
        const char* scene = ResolveSceneID(entry.sceneNum, entry.roomIndex);

        if (scene == nullptr)
            return "";

        std::string label = scene;
        size_t paren = label.find('(');

        if (paren != std::string::npos)
            label = label.substr(0, paren);

        while (!label.empty() && label.back() == ' ')
            label.pop_back();

        if (label.empty())
            return "";

        label += (entry.age == 0) ? " (Adult)" : " (Child)";
        return label;
    }

    void DrawPlayerRow(const PlayerEntry& entry, PlayerListActionEventInvite& inviteEvent,
                       PlayerListActionEvent*& pending) {
        ImGui::PushID((int)(entry.networkID));

        if (entry.isInParty)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.55f, 1.00f));
        else if (entry.pendingInvitePartyID != 0)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.85f, 0.40f, 1.00f));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

        std::string label = entry.name;

        if (entry.pendingInvitePartyID != 0)
            label = ICON_FA_ENVELOPE " " + label;

        if (m_partyAdminID != 0 && entry.networkID == m_partyAdminID)
            label = ICON_FA_STAR " " + label;

        ImGui::Selectable(label.c_str());
        ImGui::PopStyleColor();

        const ImGuiID rowId = ImGui::GetItemID();
        bool openContext = false;

        if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (m_heldRowId != rowId) {
                m_heldRowId = rowId;
                m_heldRowTime = 0.0f;
                m_heldRowFired = false;
            }

            m_heldRowTime += ImGui::GetIO().DeltaTime;

            if (!m_heldRowFired && m_heldRowTime >= LONG_PRESS_SECONDS) {
                m_heldRowFired = true;
                openContext = true;
            }
        } else if (m_heldRowId == rowId && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_heldRowId = 0;
            m_heldRowTime = 0.0f;
            m_heldRowFired = false;
        }

        if (openContext)
            ImGui::OpenPopup("##zo_row_context");

        if (!m_heldRowFired && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.pendingInvitePartyID != 0) {
                inviteEvent.actionEvent.action = PlayerListAction::AcceptInvite;
                inviteEvent.networkID = entry.networkID;
                inviteEvent.partyID = entry.pendingInvitePartyID;
                pending = &inviteEvent.actionEvent;
            } else if (!entry.isInParty && m_hasParty) {
                inviteEvent.actionEvent.action = PlayerListAction::Invite;
                inviteEvent.networkID = entry.networkID;
                pending = &inviteEvent.actionEvent;
            }
        }

        if (entry.sceneNum >= 0) {
            std::string location = LocationLabel(entry);

            if (location.empty() && entry.isInParty && m_partySettings.storyCoop)
                location = (entry.age == 0) ? "(Adult)" : "(Child)";

            if (!location.empty()) {
                const float padding = ImGui::GetStyle().FramePadding.x;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const ImVec2 rowStart = ImGui::GetItemRectMin();
                const float rowRight = ImGui::GetItemRectMax().x - padding;
                const float nameEnd = rowStart.x + ImGui::CalcTextSize(label.c_str()).x + spacing;

                const float textWidth = ImGui::CalcTextSize(location.c_str()).x;
                const float available = rowRight - nameEnd;

                if (available > 0.0f) {
                    const float drawX = (textWidth > available) ? nameEnd : (rowRight - textWidth);
                    const ImVec2 drawPos = ImVec2(drawX, rowStart.y + 2);

                    ImGui::PushClipRect(
                        drawPos, ImVec2(rowRight + 1.0f, rowStart.y + ImGui::GetTextLineHeightWithSpacing()), true);
                    ImGui::GetWindowDrawList()->AddText(drawPos, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                        location.c_str());
                    ImGui::PopClipRect();
                }
            }
        }

        if (ImGui::BeginPopupContextItem("##zo_row_context")) {
            ImGui::TextDisabled("%s", entry.name.c_str());
            ImGui::Separator();

            if (entry.pendingInvitePartyID != 0) {
                if (ImGui::MenuItem(ICON_FA_CHECK " Accept party invite")) {
                    inviteEvent.actionEvent.action = PlayerListAction::AcceptInvite;
                    inviteEvent.networkID = entry.networkID;
                    inviteEvent.partyID = entry.pendingInvitePartyID;
                    pending = &inviteEvent.actionEvent;
                }
                if (ImGui::MenuItem(ICON_FA_TIMES " Decline")) {
                    inviteEvent.actionEvent.action = PlayerListAction::DeclineInvite;
                    inviteEvent.networkID = entry.networkID;
                    inviteEvent.partyID = entry.pendingInvitePartyID;
                    pending = &inviteEvent.actionEvent;
                }
            } else if (entry.isInParty) {
                if (ImGui::MenuItem(ICON_FA_MAP_MARKER " Teleport to")) {
                    inviteEvent.actionEvent.action = PlayerListAction::TeleportTo;
                    inviteEvent.networkID = entry.networkID;
                    inviteEvent.partyID = 0;
                    pending = &inviteEvent.actionEvent;
                }
            } else if (m_hasParty) {
                if (ImGui::MenuItem(ICON_FA_USER_PLUS " Invite to party")) {
                    inviteEvent.actionEvent.action = PlayerListAction::Invite;
                    inviteEvent.networkID = entry.networkID;
                    pending = &inviteEvent.actionEvent;
                }
            } else {
                ImGui::TextDisabled("No actions");
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    void LoadChatSettings() {
        if (m_chatSettingsLoaded)
            return;

        m_applySkinPitch = CVarGetInteger("gZeldaOnline.ApplySkinPitch", 1) != 0;
        m_pvpEnabled = CVarGetInteger("gZeldaOnline.Pvp", 0) != 0;
        m_chatButtonShown = CVarGetInteger("gZeldaOnline.ChatButton", 0) != 0;
        m_chatButtonX = CVarGetFloat("gZeldaOnline.ChatButtonX", -1.0f);
        m_chatButtonY = CVarGetFloat("gZeldaOnline.ChatButtonY", -1.0f);
        m_chatSettingsLoaded = true;
    }

    bool OnScreenKeyboardWanted() const {
        if (CVarGetInteger("gZeldaOnline.ForceOnScreenKeyboard", 0) != 0)
            return true;

        return std::getenv("SteamDeck") != nullptr;
    }

    void ProbeOnScreenKeyboard() {
        if (m_chatKeyboardProbed)
            return;

        m_chatKeyboardProbed = true;

        const char* deck = std::getenv("SteamDeck");

        printf("ZeldaOnline: SDL screen keyboard support=%d, text input active=%d, SteamDeck=%s, video=%s\n",
               SDL_HasScreenKeyboardSupport() ? 1 : 0, SDL_IsTextInputActive() ? 1 : 0, deck ? deck : "(unset)",
               SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    }

    void SetOnScreenKeyboard(bool show) {
        if (m_chatKeyboardShown == show)
            return;

        m_chatKeyboardShown = show;

        ProbeOnScreenKeyboard();

        ImGuiViewport* vp = ImGui::GetMainViewport();

        if (show) {
            SDL_Rect rect;
            rect.x = (int)(vp->Pos.x + 24.0f);
            rect.y = (int)(vp->Pos.y + 24.0f);
            rect.w = (int)(vp->Size.x - 48.0f);
            rect.h = (int)(ImGui::GetFrameHeight());

            SDL_SetTextInputRect(&rect);

            if (!SDL_IsTextInputActive())
                SDL_StartTextInput();
        } else if (SDL_IsTextInputActive()) {
            SDL_StopTextInput();
        }

        if (SDL_HasScreenKeyboardSupport() || !OnScreenKeyboardWanted())
            return;

#if defined(__linux__)
        if (show) {
            char command[256];
            snprintf(command, sizeof(command),
                     "xdg-open 'steam://open/keyboard?XPosition=0&YPosition=%d&Width=%d&Height=%d&Mode=0' "
                     ">/dev/null 2>&1 &",
                     (int)(vp->Size.y * 0.5f), (int)(vp->Size.x), (int)(vp->Size.y * 0.5f));
            std::system(command);
        } else {
            std::system("xdg-open 'steam://close/keyboard' >/dev/null 2>&1 &");
        }
#endif
    }

    float CurrentSkinPitch() const {
        if (!m_applySkinPitch)
            return 1.0f;

        const std::string currentKey = m_currentSkin.empty() ? std::string("link") : m_currentSkin;

        for (const auto& skin : m_availableSkins) {
            if (skin.referenceName == currentKey)
                return skin.pitch;
        }

        return 1.0f;
    }

    void SavePvpEnabled() {
        CVarSetInteger("gZeldaOnline.Pvp", m_pvpEnabled ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    void SaveApplySkinPitch() {
        CVarSetInteger("gZeldaOnline.ApplySkinPitch", m_applySkinPitch ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    void SaveChatButtonShown() {
        CVarSetInteger("gZeldaOnline.ChatButton", m_chatButtonShown ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    void SaveChatButtonPos(const ImVec2& pos) {
        if (pos.x == m_chatButtonX && pos.y == m_chatButtonY)
            return;

        m_chatButtonX = pos.x;
        m_chatButtonY = pos.y;
        CVarSetFloat("gZeldaOnline.ChatButtonX", m_chatButtonX);
        CVarSetFloat("gZeldaOnline.ChatButtonY", m_chatButtonY);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    void DrawChatToggleButton() {
        ImGui::SameLine();

        if (m_chatButtonShown) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }

        if (ImGui::Button(ICON_FA_COMMENT "##zo_chat_toggle")) {
            m_chatButtonShown = !m_chatButtonShown;

            if (!m_chatButtonShown) {
                m_chatBoxOpen = false;
                m_chatBoxFocus = false;
            }

            SaveChatButtonShown();
        }

        if (m_chatButtonShown)
            ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_chatButtonShown ? "Hide chat button" : "Show chat button");
    }

    void DrawPvpToggleButton(PlayerListActionEventToggle& toggleEvent, PlayerListActionEvent*& pending) {
        ImGui::SameLine();

        if (m_pvpEnabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }

        if (ImGui::Button(ICON_FA_CROSSHAIRS "##zo_pvp_toggle")) {
            m_pvpEnabled = !m_pvpEnabled;
            SavePvpEnabled();

            toggleEvent.actionEvent.action = PlayerListAction::SetPvp;
            toggleEvent.enabled = m_pvpEnabled;
            pending = &toggleEvent.actionEvent;
        }

        if (m_pvpEnabled)
            ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_pvpEnabled ? "PvP enabled" : "PvP disabled");
    }

    void HandleChatHotkey() {
        if (!m_chatButtonShown)
            return;

        if (ImGui::GetIO().WantTextInput)
            return;

        if (!ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
            return;

        m_chatBoxOpen = !m_chatBoxOpen;

        if (m_chatBoxOpen) {
            m_chatBuffer[0] = '\0';
            m_chatBoxFocus = true;
        }
    }

    void DrawChatButton() {
        if (!m_chatButtonShown) {
            m_chatButtonPlaced = false;
            return;
        }

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        ImGuiViewport* vp = ImGui::GetMainViewport();

        if (!m_chatButtonPlaced) {
            if (m_chatButtonX >= 0.0f && m_chatButtonY >= 0.0f)
                ImGui::SetNextWindowPos(ImVec2(m_chatButtonX, m_chatButtonY), ImGuiCond_Always);

            m_chatButtonPlaced = true;
        }

        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowBgAlpha(0.35f);

        const bool visible = ImGui::Begin("##zo_chat_button", nullptr, flags);

        ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();

        float maxX = vp->Pos.x + vp->Size.x - size.x;
        float maxY = vp->Pos.y + vp->Size.y - size.y;

        if (maxX < vp->Pos.x)
            maxX = vp->Pos.x;
        if (maxY < vp->Pos.y)
            maxY = vp->Pos.y;

        ImVec2 clamped = pos;

        if (clamped.x > maxX)
            clamped.x = maxX;
        if (clamped.y > maxY)
            clamped.y = maxY;
        if (clamped.x < vp->Pos.x)
            clamped.x = vp->Pos.x;
        if (clamped.y < vp->Pos.y)
            clamped.y = vp->Pos.y;

        if (clamped.x != pos.x || clamped.y != pos.y) {
            ImGui::SetWindowPos(clamped);
            pos = clamped;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            SaveChatButtonPos(pos);

        if (visible) {
            if (m_chatBoxOpen) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }

            if (ImGui::Button(ICON_FA_COMMENT, ImVec2(40.0f, 40.0f))) {
                m_chatBoxOpen = !m_chatBoxOpen;

                if (m_chatBoxOpen)
                    m_chatBoxFocus = true;
            }

            if (m_chatBoxOpen)
                ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Chat '~' Key");
        }

        ImGui::End();
    }

    void DrawChatBox() {
        SetOnScreenKeyboard(m_chatBoxOpen);

        const float target = m_chatBoxOpen ? 1.0f : 0.0f;
        const float step = ImGui::GetIO().DeltaTime * 8.0f;

        if (m_chatBoxAnim < target) {
            m_chatBoxAnim += step;

            if (m_chatBoxAnim > target)
                m_chatBoxAnim = target;
        } else if (m_chatBoxAnim > target) {
            m_chatBoxAnim -= step;

            if (m_chatBoxAnim < target)
                m_chatBoxAnim = target;
        }

        if (m_chatBoxAnim <= 0.0f)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();

        const float margin = 24.0f;
        const float maxWidth = vp->Size.x - (margin * 2.0f);
        const float eased = m_chatBoxAnim * m_chatBoxAnim * (3.0f - (2.0f * m_chatBoxAnim));
        const float width = maxWidth * eased;
        const float height = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;

        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + margin, vp->Pos.y + margin));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::SetNextWindowBgAlpha(0.65f);

        if (m_chatBoxFocus)
            ImGui::SetNextWindowFocus();

        if (ImGui::Begin("##zo_chat_box", nullptr, flags)) {
            ImGui::SetNextItemWidth(-FLT_MIN);

            if (m_chatBoxFocus) {
                ImGui::SetKeyboardFocusHere();
                m_chatBoxFocus = false;
            }

            if (ImGui::InputTextWithHint("##zo_chat_input", "Say something...", m_chatBuffer, sizeof(m_chatBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (m_chatBuffer[0] != '\0') {
                    PlayerListActionEventText chatEvent;
                    chatEvent.actionEvent.action = PlayerListAction::Chat;
                    chatEvent.text = m_chatBuffer;

                    if (m_onAction)
                        m_onAction(&chatEvent.actionEvent);
                }

                m_chatBuffer[0] = '\0';

                if (OnScreenKeyboardWanted()) {
                    m_chatBoxFocus = true;
                } else {
                    m_chatBoxOpen = false;
                    m_chatBoxFocus = false;
                }
            }

            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_chatBuffer[0] = '\0';
                m_chatBoxOpen = false;
            }
        }

        ImGui::End();
    }

  public:
    using GuiWindow::GuiWindow;

    inline static ZeldaOnlineRoomWindow* Instance;

    ~ZeldaOnlineRoomWindow() {
        if (Instance == this)
            Instance = nullptr;
    }

    void SetDisplayName(const std::string& name) {
        snprintf(m_nameBuffer, sizeof(m_nameBuffer), "%s", name.c_str());
    }

    void SetActionHandler(std::function<void(const PlayerListActionEvent*)> handler) {
        m_onAction = std::move(handler);
    }

    void SetAvailableSkins(const std::vector<SkinOption>& skins) {
        m_availableSkins = skins;
    }

    void SetCurrentSkin(const std::string& skin) {
        m_currentSkin = skin;
        CVarSetString("gZeldaOnline.Skin", m_currentSkin.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    void SetHasParty(bool hasParty) {
        m_hasParty = hasParty;

        if (!hasParty) {
            for (auto& entry : m_players)
                entry.isInParty = false;

            m_partyAdmin = false;
            m_partyAdminID = 0;
            m_partySettingsOpen = false;
        }
    }

    bool HasParty() const {
        return m_hasParty;
    }

    int GetPartySize() const {
        if (!m_hasParty)
            return 1;

        int size = 1;

        for (const auto& entry : m_players) {
            if (entry.isInParty)
                size++;
        }

        return size;
    }

    PartySettings& GetPartySettings() {
        return m_partySettings;
    }

    void SetPvpEnabled(bool enabled) {
        m_pvpEnabled = enabled;
        SavePvpEnabled();
    }

    bool PvpEnabled() const {
        return m_pvpEnabled;
    }

    void SetPartyAdminID(uint32_t networkID) {
        m_partyAdminID = networkID;
    }

    uint32_t PartyAdminID() const {
        return m_partyAdminID;
    }

    void SetPartyAdmin(bool admin) {
        m_partyAdmin = admin;

        if (!admin)
            m_partySettingsOpen = false;
    }

    bool PartyAdmin() const {
        return m_partyAdmin;
    }

    void AddPlayer(const PlayerEntry& player) {

        if (PlayerEntry* existing = FindPlayerEntry(player.networkID)) {
            existing->name = player.name;
            return;
        }

        m_players.push_back(player);
    }

    void RemovePlayer(uint32_t networkID) {
        for (size_t i = 0; i < m_players.size(); i++) {
            if (m_players[i].networkID == networkID) {
                m_players.erase(m_players.begin() + i);
                return;
            }
        }
    }

    std::string NameOf(uint32_t networkID) {

        if (PlayerEntry* entry = FindPlayerEntry(networkID))
            return entry->name;

        return "";
    }

    void UpdateEntry(uint32_t networkID, const std::string& name, int sceneIndex, int roomIndex, uint8_t age) {
        if (PlayerEntry* entry = FindPlayerEntry(networkID)) {
            entry->name = name;
            entry->sceneNum = sceneIndex;
            entry->roomIndex = roomIndex;
            entry->age = age;
        }
    }

    void SetInParty(uint32_t networkID, bool inParty) {

        if (PlayerEntry* entry = FindPlayerEntry(networkID)) {
            entry->isInParty = inParty;
            if (inParty)
                entry->pendingInvitePartyID = 0;
        }
    }

    void SetPendingPartyInvite(uint32_t networkID, uint32_t partyID) {
        if (PlayerEntry* entry = FindPlayerEntry(networkID))
            entry->pendingInvitePartyID = partyID;
    }

    uint32_t PendingInvitePartyID(uint32_t networkID) {
        if (PlayerEntry* entry = FindPlayerEntry(networkID))
            return entry->pendingInvitePartyID;

        return 0;
    }

    void ClearPendingInvites() {
        for (auto& entry : m_players)
            entry.pendingInvitePartyID = 0;
    }

    void ClearPartyFlags() {

        for (auto& entry : m_players) {
            entry.isInParty = false;
            entry.pendingInvitePartyID = 0;
        }
    }

    void ClearPlayers() {
        m_players.clear();
    }

    void SetConnected(bool connected) {
        m_connected = connected;
        m_connecting = false;

        if (!connected) {
            m_players.clear();
            m_hasParty = false;
            m_fieldsLoaded = false;
            m_chatBoxOpen = false;
            m_chatBoxFocus = false;
            SetOnScreenKeyboard(false);
        } else {
            SetDisplayName("");
        }
    }

    void SetConnecting(bool connecting) {
        m_connecting = connecting;
    }

    bool IsConnected() const {
        return m_connected;
    }

    void CenterAndExpand() {
        m_recenterNextFrame = true;
    }

    void SetStatus(const std::string& text, const ImVec4& color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f)) {
        m_statusText = text;
        m_statusColor = color;
    }

    void ClearStatus() {
        m_statusText.clear();
    }

    void Reset() {
        m_players.clear();
        m_hasParty = false;
        m_partyAdmin = false;
        m_partyAdminID = 0;
        m_partySettingsOpen = false;
        m_partySettings = PartySettings();
        m_connected = false;
        m_connecting = false;
        m_statusText.clear();
        m_statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        m_fieldsLoaded = false;
    }

    void BeginAutoConnect() {
        m_beginAutoConnect = true;
    }

    void InitElement() override {};

    void LoadConnectionFields() {
        if (m_fieldsLoaded)
            return;

        snprintf(m_hostBuffer, sizeof(m_hostBuffer), "%s", CVarGetString("gZeldaOnline.Host", "awu.fks.mybluehost.me"));
        m_portValue = CVarGetInteger("gZeldaOnline.Port", 21050);
        snprintf(m_nameBuffer, sizeof(m_nameBuffer), "%s", CVarGetString("gZeldaOnline.Nickname", "Player"));
        snprintf(m_fileServerBuffer, sizeof(m_fileServerBuffer), "%s",
                 CVarGetString("gZeldaOnline.FileServer", "http://awu.fks.mybluehost.me/"));
        m_autoConnect = CVarGetInteger("gZeldaOnline.AutoConnect", 0) != 0;

        m_fieldsLoaded = true;
    }

    void SaveConnectionFields() {
        CVarSetString("gZeldaOnline.Host", m_hostBuffer);
        CVarSetInteger("gZeldaOnline.Port", m_portValue);
        CVarSetString("gZeldaOnline.Nickname", m_nameBuffer);
        CVarSetString("gZeldaOnline.FileServer", m_fileServerBuffer);
        CVarSetInteger("gZeldaOnline.AutoConnect", m_autoConnect ? 1 : 0);
    }

    void DrawConnectSetup(PlayerListActionEventConnect& connectEvent, PlayerListActionEvent*& pending) {
        LoadConnectionFields();

        ImGui::TextUnformatted("Host");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##zo_host", "127.0.0.1", m_hostBuffer, sizeof(m_hostBuffer));

        ImGui::TextUnformatted("Port");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##zo_port", &m_portValue, 0, 0);

        if (m_portValue < 0)
            m_portValue = 0;
        if (m_portValue > 65535)
            m_portValue = 65535;

        ImGui::TextUnformatted("Nickname");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##zo_nick", "Player", m_nameBuffer, sizeof(m_nameBuffer));

        ImGui::TextUnformatted("Resource Server");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##zo_fileserver", "http://...", m_fileServerBuffer, sizeof(m_fileServerBuffer));

        ImGui::Spacing();

        bool valid = m_hostBuffer[0] != '\0' && m_nameBuffer[0] != '\0' && m_portValue > 1024 && m_portValue < 65535;

        ImGui::BeginDisabled(!valid || m_connecting);

        if (ImGui::Button(m_connecting ? "Connecting..." : "Connect", ImVec2(-FLT_MIN, 0.0f)) || m_beginAutoConnect) {
            connectEvent.actionEvent.action = PlayerListAction::Connect;
            connectEvent.host = m_hostBuffer;
            connectEvent.port = (uint16_t)(m_portValue);
            connectEvent.nickname = m_nameBuffer;
            connectEvent.fileServer = m_fileServerBuffer;
            pending = &connectEvent.actionEvent;
            m_connecting = true;
            m_beginAutoConnect = false;
            SaveConnectionFields();
        }

        ImGui::EndDisabled();

        if (ImGui::Checkbox("Auto-connect", &m_autoConnect)) {
            CVarSetInteger("gZeldaOnline.AutoConnect", m_autoConnect ? 1 : 0);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Connect automatically when the game starts");

        if (m_connecting) {
            if (ImGui::Button(ICON_FA_TIMES " Cancel", ImVec2(-FLT_MIN, 0.0f))) {
                connectEvent.actionEvent.action = PlayerListAction::Disconnect;
                pending = &connectEvent.actionEvent;
                m_connecting = false;
            }
        }
    }

    void DrawElement() override {
        PlayerListActionEventInvite inviteEvent;
        PlayerListActionEventText textEvent;
        PlayerListActionEventConnect connectEvent;
        PlayerListActionEventMultiVar multiVarEvent;
        PlayerListActionEvent actionEvent;
        PlayerListActionEventToggle toggleEvent;
        PlayerListActionEvent* pending = nullptr;

        if (!m_connected) {
            DrawConnectSetup(connectEvent, pending);

            if (!m_statusText.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, m_statusColor);
                ImGui::TextWrapped("%s", m_statusText.c_str());
                ImGui::PopStyleColor();
            }

            if (pending != nullptr && m_onAction)
                m_onAction(pending);

            return;
        }

        {
            DrawMenuBar(textEvent, pending);

            float footerHeight = (ImGui::GetFrameHeightWithSpacing() * 3.0f) + 12.0f;

            if (!m_statusText.empty())
                footerHeight += ImGui::GetTextLineHeightWithSpacing();

            if (m_hasParty) {
                if (m_partyAdmin)
                    ImGui::Text(ICON_FA_STAR " " ICON_FA_USERS " Party: %d others", PartyMemberCount());
                else
                    ImGui::Text(ICON_FA_USERS " Party: %d others", PartyMemberCount());
                ImGui::Separator();

                for (const auto& entry : m_players) {
                    if (entry.isInParty)
                        DrawPlayerRow(entry, inviteEvent, pending);
                }

                ImGui::Spacing();
            }

            int otherCount = (int)(m_players.size()) - PartyMemberCount();

            ImGui::Text(ICON_FA_USER " Players: %d others", otherCount);
            ImGui::Separator();

            if (ImGui::BeginChild("##zo_player_list", ImVec2(0, -footerHeight), false)) {
                if (otherCount == 0)
                    ImGui::TextDisabled("No other players connected");

                for (const auto& entry : m_players) {
                    if (!entry.isInParty)
                        DrawPlayerRow(entry, inviteEvent, pending);
                }
            }
            ImGui::EndChild();

            ImGui::Separator();

            ImGui::TextUnformatted("Skin");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));

            const std::string currentKey = m_currentSkin.empty() ? std::string("link") : m_currentSkin;
            const char* preview = currentKey.c_str();

            for (const auto& skin : m_availableSkins) {
                if (skin.referenceName == currentKey) {
                    preview = skin.displayName.c_str();
                    break;
                }
            }

            if (ImGui::BeginCombo("##zo_skin", preview)) {
                if (m_availableSkins.empty())
                    ImGui::TextDisabled("No skins found");

                for (const auto& skin : m_availableSkins) {
                    bool selected = skin.referenceName == currentKey;
                    ImGui::PushID(skin.referenceName.c_str());

                    if (ImGui::Selectable(skin.displayName.c_str(), selected)) {
                        multiVarEvent.actionEvent.action = PlayerListAction::ChangeSkin;
                        multiVarEvent.text = skin.referenceName;
                        multiVarEvent.intValue = m_applySkinPitch ? 1 : 0;
                        multiVarEvent.floatValue = m_applySkinPitch ? skin.pitch : 1.0f;
                        pending = &multiVarEvent.actionEvent;
                    }

                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", skin.referenceName.c_str());

                    if (selected)
                        ImGui::SetItemDefaultFocus();

                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();

            if (m_applySkinPitch) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }

            if (ImGui::Button(m_applySkinPitch ? ICON_FA_VOLUME_UP "##zo_skin_pitch"
                                               : ICON_FA_VOLUME_OFF "##zo_skin_pitch",
                              ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                m_applySkinPitch = !m_applySkinPitch;
                SaveApplySkinPitch();

                multiVarEvent.actionEvent.action = PlayerListAction::ToggleSkinPitch;
                multiVarEvent.text = m_currentSkin;
                multiVarEvent.intValue = m_applySkinPitch ? 1 : 0;
                multiVarEvent.floatValue = CurrentSkinPitch();
                pending = &multiVarEvent.actionEvent;
            }

            if (m_applySkinPitch)
                ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(m_applySkinPitch ? "Skin voice pitch on" : "Skin voice pitch off");

            ImGui::TextUnformatted("Name");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);

            if (ImGui::InputTextWithHint("##zo_name", "Display name...", m_nameBuffer, sizeof(m_nameBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (m_nameBuffer[0] != '\0') {
                    textEvent.actionEvent.action = PlayerListAction::ChangeName;
                    textEvent.text = m_nameBuffer;
                    pending = &textEvent.actionEvent;
                }
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

            if (ImGui::Button(ICON_FA_SIGN_OUT "##zo_disconnect")) {
                actionEvent.action = PlayerListAction::Disconnect;
                pending = &actionEvent;
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Disconnect");

            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_REFRESH "##zo_unstuck")) {
                actionEvent.action = PlayerListAction::UnstuckMe;
                pending = &actionEvent;
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unstuck me");

            DrawChatToggleButton();
            DrawPvpToggleButton(toggleEvent, pending);

            ImGui::PopStyleVar();

            if (!m_statusText.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, m_statusColor);
                ImGui::TextWrapped("%s", m_statusText.c_str());
                ImGui::PopStyleColor();
            }
        }

        if (pending != nullptr && m_onAction)
            m_onAction(pending);
    }

    void Draw() override {
        LoadChatSettings();

        if (!IsVisible()) {
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_WindowBg,
                              ImVec4(0, 0, 0, CVarGetFloat(CVAR_SETTING("Notifications.BgOpacity"), 0.5f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

        const ImVec2 defaultSize = ImVec2(240.0f, 380.0f);
        auto vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(180.0f, 200.0f), ImVec2(FLT_MAX, FLT_MAX));

        const ImGuiCond placementCond = m_recenterNextFrame ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + (vp->Size.x - defaultSize.x) * 0.5f, vp->Pos.y + (vp->Size.y - defaultSize.y) * 0.5f),
            placementCond, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowCollapsed(false, placementCond);

        if (m_recenterNextFrame) {
            ImGui::SetNextWindowSize(defaultSize, ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            m_recenterNextFrame = false;
        }

        const std::string title = BuildWindowTitle();

        if (ImGui::Begin(title.c_str(), &mIsVisible,
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_MenuBar)) {
            DrawElement();
        }

        ImGui::End();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);

        if (m_connected) {
            DrawPartySettings();
            HandleChatHotkey();
            DrawChatButton();
            DrawChatBox();
        }
    }

    void UpdateElement() override {};
};

} // namespace ZeldaOnline
#endif