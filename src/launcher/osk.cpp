#include "osk.h"

#include <imgui.h>

namespace tt2::launcher
{
    void Osk::open(std::string title, std::string initial, Layout layout, std::function<void(const std::string&)> done)
    {
        title_ = std::move(title);
        text_ = std::move(initial);
        layout_ = layout;
        done_ = std::move(done);
        active_ = true;
        first_frame_ = true;
    }

    void Osk::draw()
    {
        static const char* text_rows[] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_. " };
        static const char* address_rows[] = { "1234567890", "abcdefghij", "klmnopqrst", "uvwxyz.:- " };
        const char** rows = layout_ == Layout::Text ? text_rows : address_rows;

        ImGui::TextUnformatted(title_.c_str());
        ImGui::Spacing();
        ImGui::Text("> %s_", text_.c_str());
        ImGui::Spacing();

        bool finish = false, cancel = false;
        const float key = 64.0f;
        for (int r = 0; r < 4; r++)
        {
            for (int c = 0; rows[r][c]; c++)
            {
                const char ch = rows[r][c];
                char label[16];
                snprintf(label, sizeof(label), ch == ' ' ? "space##%d%d" : "%c##%d%d", ch, r, c);
                if (c)
                    ImGui::SameLine();
                if (first_frame_ && r == 0 && c == 0)
                {
                    ImGui::SetKeyboardFocusHere();
                    first_frame_ = false;
                }
                if (ImGui::Button(label, ImVec2(ch == ' ' ? key * 2 : key, key)) && text_.size() < 48)
                    text_.push_back(ch);
            }
        }
        if (ImGui::Button("Delete (X)", ImVec2(key * 3, key)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
        {
            if (!text_.empty())
                text_.pop_back();
        }
        ImGui::SameLine();
        if (ImGui::Button("Done (Start)", ImVec2(key * 3, key)) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart) || ImGui::IsKeyPressed(ImGuiKey_Enter))
            finish = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel (B)", ImVec2(key * 3, key)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
            cancel = true;

        // Typing on a physical keyboard also works when one is present.
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters)
            if (c >= 32 && c < 127 && text_.size() < 48)
                text_.push_back(char(c));

        if (finish)
        {
            while (!text_.empty() && text_.back() == ' ')
                text_.pop_back();
            active_ = false;
            if (done_)
                done_(text_);
        }
        else if (cancel)
        {
            active_ = false;
        }
    }
}
