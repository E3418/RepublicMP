// SPDX-License-Identifier: GPL-3.0-only
// See workshop_panel.h.
#include "workshop_panel.h"
#include "../session/workshop.h"
#include "../session/save_sync.h"
#include "imgui.h"
#include <stdio.h>
#include <vector>

bool WorkshopPanel_IsOpen() { return Workshop_Phase() != WORKSHOP_CLOSED; }

static const char* NameOf(const WorkshopOffer& o, char* buf, size_t size)
{
    if (o.item.name[0]) return o.item.name;
    _snprintf_s(buf, size, _TRUNCATE, "Workshop item %llu", o.item.id);
    return buf;
}

static double Mb(unsigned long long bytes) { return bytes / 1048576.0; }

// The same host again; `without`: what is not installed is declined.
static void JoinAgain(bool without)
{
    if (without) Workshop_DeclineOffered();
    Workshop_Close();
    SaveSync_StartClient(nullptr);
}

static void List(const std::vector<WorkshopOffer>& offers, int phase, float height)
{
    ImGui::BeginChild("##workshopItems", ImVec2(-1, height), ImGuiChildFlags_Borders);
    for (const WorkshopOffer& o : offers) {
        ImGui::PushID((void*)(uintptr_t)o.item.id);
        char buf[48];
        const char* name = NameOf(o, buf, sizeof(buf));
        if (phase == WORKSHOP_CHOOSE) {
            bool on = o.selected;
            char label[160];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%s  (%.1f MB)", name, Mb(o.item.size));
            if (ImGui::Checkbox(label, &on)) Workshop_Select(o.item.id, on);
        } else {
            ImGui::TextUnformatted(name);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
            float w = ImGui::GetContentRegionAvail().x;
            if (o.state == WORKSHOP_RUNNING) {
                float f = o.total ? (float)((double)o.done / o.total) : 0.0f;
                char overlay[48];
                _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%.1f of %.1f MB", Mb(o.done), Mb(o.total));
                ImGui::ProgressBar(f, ImVec2(w, 0), overlay);
            } else if (o.state == WORKSHOP_INSTALLED) {
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "installed");
            } else if (o.state == WORKSHOP_FAILED) {
                ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.45f, 1), "could not be downloaded (private, removed or not on Steam)");
            } else {
                ImGui::TextDisabled("not chosen");
            }
        }
        ImGui::TextDisabled("    %llu", o.item.id);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void WorkshopPanel_Draw()
{
    int phase = Workshop_Phase();
    if (phase == WORKSHOP_CLOSED) return;
    std::vector<WorkshopOffer> offers;
    size_t extra = 0;
    Workshop_Snapshot(offers, &extra);

    ImGuiIO& io = ImGui::GetIO();
    // The dimmer, as the join screen's; it takes the clicks around the screen
    // (the MULTIPLAYER page under it gets none).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.85f));
    ImGui::Begin("##workshopDim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImGui::End();
    ImGui::PopStyleColor();

    float w = io.DisplaySize.x * 0.5f, h = io.DisplaySize.y * 0.66f;
    if (w < 640) w = 640;
    if (h < 460) h = 460;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Multiplayer - Workshop items of the host", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings)) {
        float line = ImGui::GetFrameHeightWithSpacing();
        int selected = 0, installed = 0, failed = 0;
        unsigned long long selectedSize = 0, done = 0, total = 0;
        for (const WorkshopOffer& o : offers) {
            if (o.selected) { selected++; selectedSize += o.item.size; }
            if (o.state == WORKSHOP_INSTALLED) installed++;
            if (o.state == WORKSHOP_FAILED) failed++;
            if (o.state == WORKSHOP_RUNNING || o.state == WORKSHOP_INSTALLED) { done += o.done; total += o.total; }
        }
        if (phase == WORKSHOP_CHOOSE) {
            ImGui::TextWrapped("The host plays with %zu Workshop item(s) you do not have, or have in an older version. "
                               "Choose what to download, then you join.", offers.size());
            if (extra)
                ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "You also have %zu item(s) the host has not - they may change the game.",
                                   extra);
            if (ImGui::SmallButton("All")) Workshop_SelectAll(true);
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) Workshop_SelectAll(false);
            List(offers, phase, ImGui::GetContentRegionAvail().y - line * 2.4f);
            ImGui::Text("Chosen: %d of %zu, %.1f MB", selected, offers.size(), Mb(selectedSize));
            ImGui::BeginDisabled(selected == 0);
            if (ImGui::Button("Download chosen", ImVec2(w * 0.3f, 0))) Workshop_DownloadSelected();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Join without them", ImVec2(w * 0.3f, 0))) JoinAgain(true);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(w * 0.2f, 0))) Workshop_Close();
        } else {
            if (phase == WORKSHOP_DOWNLOADING)
                ImGui::TextWrapped("Downloading %d item(s) through Steam.", selected);
            else
                ImGui::TextWrapped("Done: %d installed%s.", installed, failed ? ", some could not be downloaded" : "");
            char overlay[64];
            _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%.1f of %.1f MB", Mb(done), Mb(total));
            ImGui::ProgressBar(total ? (float)((double)done / total) : (phase == WORKSHOP_DONE ? 1.0f : 0.0f), ImVec2(-1, 0), overlay);
            List(offers, phase, ImGui::GetContentRegionAvail().y - line * 1.4f);
            if (phase == WORKSHOP_DONE) {
                // What could not be downloaded would only be offered again.
                if (ImGui::Button(failed ? "Join without the rest" : "Join", ImVec2(w * 0.3f, 0))) JoinAgain(failed > 0);
                ImGui::SameLine();
            }
            if (ImGui::Button(phase == WORKSHOP_DONE ? "Close" : "Hide", ImVec2(w * 0.2f, 0))) Workshop_Close();
        }
    }
    ImGui::End();
}
