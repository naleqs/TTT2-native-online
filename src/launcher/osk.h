#pragma once

// On-screen keyboard for entering text with a controller (D-pad to move, A to type,
// X backspace, Start/Y done, B cancel).

#include <functional>
#include <string>

namespace tt2::launcher
{
    class Osk
    {
    public:
        enum class Layout
        {
            Text,
            Address, // digits, letters for host names, '.', ':', '-'
        };

        void open(std::string title, std::string initial, Layout layout, std::function<void(const std::string&)> done);
        bool active() const { return active_; }
        void draw();

    private:
        bool active_ = false;
        bool first_frame_ = false;
        std::string title_, text_;
        Layout layout_ = Layout::Text;
        std::function<void(const std::string&)> done_;
    };
}
