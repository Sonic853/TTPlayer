#pragma once

#include <array>
#include <string>
#include <windows.h>
#include <shobjidl.h>

namespace ttplayer::ui {
struct TaskbarPlaybackState {
    bool playing{};
    bool previous_enabled{};
    bool play_pause_enabled{};
    bool next_enabled{};
    bool operator==(const TaskbarPlaybackState&) const = default;
};

struct TaskbarPlaybackLabels {
    std::wstring previous, play, pause, next;
    bool operator==(const TaskbarPlaybackLabels&) const = default;
};

enum class TaskbarPlaybackAction : UINT {
    none = 0, previous = 0xb710, play_pause = 0xb711, next = 0xb712
};

// UI-thread owned. The shell owns the thumbnail and copies our HICONs; no
// custom preview window, global media-key handler or background thread.
class TaskbarPlaybackControls {
public:
    using Factory = HRESULT (*)(ITaskbarList3**);
    explicit TaskbarPlaybackControls(Factory factory = CreateNativeTaskbar)
        : factory_(factory) {}
    ~TaskbarPlaybackControls();
    TaskbarPlaybackControls(const TaskbarPlaybackControls&) = delete;
    TaskbarPlaybackControls& operator=(const TaskbarPlaybackControls&) = delete;

    // Only call after the shell's registered TaskbarButtonCreated message.
    HRESULT OnButtonCreated(HWND window, const TaskbarPlaybackState& state,
                            const TaskbarPlaybackLabels& labels);
    HRESULT Update(const TaskbarPlaybackState& state,
                   const TaskbarPlaybackLabels& labels);
    void Reset(); // Explorer restart or owning HWND destruction
    [[nodiscard]] static TaskbarPlaybackAction DecodeClick(
        WPARAM wparam, const TaskbarPlaybackState& state) noexcept;

private:
    static HRESULT CreateNativeTaskbar(ITaskbarList3** taskbar);
    bool EnsureIcons();
    std::array<THUMBBUTTON, 3> Buttons(const TaskbarPlaybackState& state,
                                      const TaskbarPlaybackLabels& labels) const;
    Factory factory_;
    ITaskbarList3* taskbar_{};
    HWND window_{};
    std::array<HICON, 4> icons_{}; // previous, play, pause, next
    int icon_width_{}, icon_height_{};
    bool added_{};
    bool have_state_{};
    TaskbarPlaybackState state_;
    TaskbarPlaybackLabels labels_;
};
} // namespace ttplayer::ui
