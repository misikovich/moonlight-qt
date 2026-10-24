#include "streammenu.h"
#include "streamutils.h"
#include "video/overlaymanager.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QSvgRenderer>
#include <QtMath>

#include <algorithm>

#if !SDL_VERSION_ATLEAST(2, 26, 0)
#if SDL_VERSION_ATLEAST(2, 0, 6)
#include <SDL_vulkan.h>
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
#include <SDL_metal.h>
#endif
#endif

// Colors follow the Material dark theme used by the rest of the client
static const QColor k_PanelColor(0x30, 0x30, 0x30, 0xF2);
static const QColor k_BorderColor(0x4A, 0x4A, 0x4A);
static const QColor k_TileColor(0x3C, 0x3C, 0x3C);
static const QColor k_SelectedColor(0x61, 0x61, 0x61);
static const QColor k_DangerColor(0xB7, 0x1C, 0x1C);
static const QColor k_RowFocusColor(0x3C, 0x3C, 0x3C);
static const QColor k_TrackColor(0x5A, 0x5A, 0x5A);
static const QColor k_TrackFillColor(0xE0, 0xE0, 0xE0);
static const QColor k_TickColor(0x8A, 0x8A, 0x8A);
static const QColor k_TextColor(0xFF, 0xFF, 0xFF);
static const QColor k_SecondaryTextColor(0xB0, 0xB0, 0xB0);
static const QColor k_DisabledTextColor(0x80, 0x80, 0x80);

// Distance from center before a stick movement counts as navigation
#define STICK_NAV_THRESHOLD 16000

// Space kept free around the menu in pixels, and the smallest scale used to fit it
#define MENU_FIT_MARGIN 8
#define MENU_MIN_SCALE 0.3

static StreamingPreferences::VideoCodecConfig normalizeCodec(StreamingPreferences::VideoCodecConfig codec)
{
    return codec == StreamingPreferences::VCC_FORCE_HEVC_HDR_DEPRECATED ?
               StreamingPreferences::VCC_FORCE_HEVC : codec;
}

static void getDrawableSize(SDL_Window* window, int* width, int* height)
{
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(window, width, height);
#else
    // Window points differ from pixels on HiDPI displays, so ask the
    // graphics API of the renderer for the drawable size
    Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_OPENGL) {
        SDL_GL_GetDrawableSize(window, width, height);
        return;
    }
#if SDL_VERSION_ATLEAST(2, 0, 6)
    if (flags & SDL_WINDOW_VULKAN) {
        SDL_Vulkan_GetDrawableSize(window, width, height);
        return;
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (flags & SDL_WINDOW_METAL) {
        SDL_Metal_GetDrawableSize(window, width, height);
        return;
    }
#endif
    SDL_GetWindowSize(window, width, height);
#endif
}

static unsigned rowMask(int row)
{
    // SettingsRow values map to SettingsRowMask bits
    return 1u << row;
}

static bool isModifierKey(SDL_Keycode key)
{
    switch (key) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
        return true;
    default:
        return false;
    }
}

// Width of text drawn with font on the painter's device. The fractional advance is
// rounded up so a rect of this width really holds the text, since eliding compares
// the unrounded width and would cut text that is a fraction of a pixel wider.
static int textWidth(const QPainter& painter, const QFont& font, const QString& text)
{
    QFontMetricsF metrics(font, painter.device());
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return qCeil(metrics.horizontalAdvance(text));
#else
    return qCeil(metrics.width(text));
#endif
}

// Draws a single line of text inside rect, shrinking it slightly and
// then eliding it if needed so long translations never overflow
static void drawFittedText(QPainter& painter, const QRect& rect, int flags, const QString& text)
{
    const QFont originalFont = painter.font();
    QFont font = originalFont;
    const int minPixelSize = qMax(1, font.pixelSize() * 3 / 4);
    while (font.pixelSize() > minPixelSize && textWidth(painter, font, text) > rect.width()) {
        font.setPixelSize(font.pixelSize() - 1);
    }

    painter.setFont(font);
    if (textWidth(painter, font, text) <= rect.width()) {
        painter.drawText(rect, flags, text);
    }
    else {
        QFontMetricsF metrics(font, painter.device());
        painter.drawText(rect, flags, metrics.elidedText(text, Qt::ElideRight, qMax(0, rect.width())));
    }
    painter.setFont(originalFont);
}

StreamMenu::StreamMenu(Overlay::OverlayManager& overlayManager)
    : m_OverlayManager(overlayManager),
      m_Window(nullptr),
      m_Open(false),
      m_UsingGamepad(false),
      m_SwapFaceButtons(false),
      m_AppliedRows(0),
      m_SelectedItem(ItemQuitStream),
      m_ForceQuitArmed(false),
      m_SettingsOpen(false),
      m_SettingsRow(RowResolution),
      m_SelectedButton(ButtonApply),
      m_SavedSettings(),
      m_JustSaved(false),
      m_DraggingRow(-1),
      m_VolumeOpen(false),
      m_DraggingVolume(false),
      m_VolumeIndex(0),
      m_OptionIndex(),
      m_InitialIndex(),
      m_StickDirection(),
      m_IconCacheScale(0)
{
}

void StreamMenu::open(SDL_Window* window, const StreamSettings& current, const StreamSettings& saved,
                      bool statsOverlayEnabled, bool fromGamepad, bool swapFaceButtons, unsigned appliedRows,
                      int volume)
{
    // The Stats tile reads the live overlay state instead
    Q_UNUSED(statsOverlayEnabled);

    m_Window = window;
    m_AppliedRows = appliedRows;
    m_UsingGamepad = fromGamepad;
    m_SwapFaceButtons = swapFaceButtons;
    m_SelectedItem = ItemQuitStream;
    m_ForceQuitArmed = false;
    m_SettingsOpen = false;
    m_SettingsRow = RowResolution;
    m_SelectedButton = ButtonApply;
    m_SavedSettings = saved;
    m_SavedSettings.videoCodecConfig = normalizeCodec(saved.videoCodecConfig);
    m_JustSaved = false;
    m_DraggingRow = -1;
    m_VolumeOpen = false;
    m_DraggingVolume = false;
    m_StickDirection[0] = m_StickDirection[1] = 0;

    // Volume, in steps of 10%
    m_VolumeOptions.clear();
    for (int percent = 0; percent <= 100; percent += 10) {
        m_VolumeOptions.append({ tr("%1%").arg(percent), percent, 0 });
    }
    m_VolumeIndex = qBound(0, qRound(volume / 10.0), (int)m_VolumeOptions.size() - 1);

    for (int i = 0; i <= RowVideoCodec; i++) {
        m_Options[i].clear();
    }

    int nativeWidth = 0, nativeHeight = 0, nativeFps = 0;
    SDL_DisplayMode desktopMode;
    SDL_Rect safeArea;
    if (StreamUtils::getNativeDesktopMode(SDL_GetWindowDisplayIndex(window), &desktopMode, &safeArea)) {
        nativeWidth = desktopMode.w;
        nativeHeight = desktopMode.h;
        nativeFps = desktopMode.refresh_rate;
    }

    // Resolution
    QVector<Option>& resolutions = m_Options[RowResolution];
    resolutions = {
        { tr("720p"), 1280, 720 },
        { tr("1080p"), 1920, 1080 },
        { tr("1440p"), 2560, 1440 },
        { tr("4K"), 3840, 2160 },
    };
    auto hasResolution = [&resolutions](int width, int height) {
        return std::any_of(resolutions.cbegin(), resolutions.cend(), [=](const Option& option) {
            return option.a == width && option.b == height;
        });
    };
    if (nativeWidth > 0 && !hasResolution(nativeWidth, nativeHeight)) {
        resolutions.append({ tr("Native"), nativeWidth, nativeHeight });
    }
    if (!hasResolution(current.width, current.height)) {
        resolutions.append({ tr("%1×%2").arg(current.width).arg(current.height), current.width, current.height });
    }
    std::sort(resolutions.begin(), resolutions.end(), [](const Option& left, const Option& right) {
        return (qint64)left.a * left.b < (qint64)right.a * right.b;
    });

    // Frame rate
    QVector<int> frameRates = { 30, 60, 90, 120 };
    if (nativeFps > 0 && !frameRates.contains(nativeFps)) {
        frameRates.append(nativeFps);
    }
    if (!frameRates.contains(current.fps)) {
        frameRates.append(current.fps);
    }
    std::sort(frameRates.begin(), frameRates.end());
    for (int fps : frameRates) {
        m_Options[RowFrameRate].append({ tr("%1 FPS").arg(fps), fps, 0 });
    }

    // Video codec
    m_Options[RowVideoCodec] = {
        { tr("Auto"), StreamingPreferences::VCC_AUTO, 0 },
        { tr("H.264"), StreamingPreferences::VCC_FORCE_H264, 0 },
        { tr("HEVC"), StreamingPreferences::VCC_FORCE_HEVC, 0 },
        { tr("AV1"), StreamingPreferences::VCC_FORCE_AV1, 0 },
    };

    StreamingPreferences::VideoCodecConfig currentCodec = normalizeCodec(current.videoCodecConfig);

    for (int row = 0; row <= RowVideoCodec; row++) {
        m_OptionIndex[row] = 0;
        for (int i = 0; i < m_Options[row].size(); i++) {
            const Option& option = m_Options[row][i];
            if ((row == RowResolution && option.a == current.width && option.b == current.height) ||
                    (row == RowFrameRate && option.a == current.fps) ||
                    (row == RowVideoCodec && option.a == currentCodec)) {
                m_OptionIndex[row] = i;
                break;
            }
        }
        m_InitialIndex[row] = m_OptionIndex[row];
    }

    m_Open = true;
    m_OverlayManager.setOverlayState(Overlay::OverlayStreamMenu, true);
    render();
}

void StreamMenu::close()
{
    m_Open = false;
    m_DraggingRow = -1;
    m_DraggingVolume = false;
    m_OverlayManager.setOverlayState(Overlay::OverlayStreamMenu, false);
}

bool StreamMenu::isOpen() const
{
    return m_Open;
}

void StreamMenu::refresh()
{
    if (m_Open) {
        render();
    }
}

void StreamMenu::setSavedSettings(const StreamSettings& saved)
{
    m_SavedSettings = saved;
    m_SavedSettings.videoCodecConfig = normalizeCodec(saved.videoCodecConfig);
    m_JustSaved = true;
    refresh();
}

StreamMenu::StreamSettings StreamMenu::getSelectedSettings() const
{
    StreamSettings settings;
    const Option& resolution = m_Options[RowResolution][m_OptionIndex[RowResolution]];
    settings.width = resolution.a;
    settings.height = resolution.b;
    settings.fps = m_Options[RowFrameRate][m_OptionIndex[RowFrameRate]].a;
    settings.videoCodecConfig = (StreamingPreferences::VideoCodecConfig)m_Options[RowVideoCodec][m_OptionIndex[RowVideoCodec]].a;
    return settings;
}

int StreamMenu::getVolume() const
{
    return m_VolumeOptions.isEmpty() ? 100 : m_VolumeOptions[m_VolumeIndex].a;
}

bool StreamMenu::hasSettingsChanges() const
{
    for (int row = 0; row <= RowVideoCodec; row++) {
        if (m_OptionIndex[row] != m_InitialIndex[row]) {
            return true;
        }
    }
    return false;
}

unsigned StreamMenu::getChangedRows() const
{
    unsigned rows = m_AppliedRows;
    for (int row = 0; row <= RowVideoCodec; row++) {
        if (m_OptionIndex[row] != m_InitialIndex[row]) {
            rows |= rowMask(row);
        }
    }
    return rows;
}

bool StreamMenu::canSaveSettings() const
{
    // Only changed rows are saved, so unchanged rows can't enable Save
    unsigned changedRows = getChangedRows();
    StreamSettings selected = getSelectedSettings();

    if ((changedRows & RowMaskResolution) &&
            (selected.width != m_SavedSettings.width || selected.height != m_SavedSettings.height)) {
        return true;
    }
    if ((changedRows & RowMaskFrameRate) && selected.fps != m_SavedSettings.fps) {
        return true;
    }
    if ((changedRows & RowMaskVideoCodec) && selected.videoCodecConfig != m_SavedSettings.videoCodecConfig) {
        return true;
    }
    return false;
}

void StreamMenu::focusButtonRow()
{
    m_SettingsRow = RowApply;
    m_SelectedButton = (hasSettingsChanges() || !canSaveSettings()) ? ButtonApply : ButtonSave;
}

StreamMenu::Action StreamMenu::activateButton()
{
    if (m_SelectedButton == ButtonApply && hasSettingsChanges()) {
        return Action::ApplySettings;
    }
    else if (m_SelectedButton == ButtonSave && canSaveSettings()) {
        return Action::SaveSettings;
    }

    // Disabled buttons do nothing
    return Action::None;
}

void StreamMenu::setSelectedItem(int item)
{
    if (item == m_SelectedItem) {
        return;
    }

    m_SelectedItem = item;

    // Moving away disarms force quit and collapses the settings
    m_ForceQuitArmed = false;
    if (item != ItemStreamSettings) {
        m_SettingsOpen = false;
    }
    if (item != ItemVolume) {
        m_VolumeOpen = false;
    }
}

void StreamMenu::adjustRow(int row, int delta)
{
    m_OptionIndex[row] = qBound(0, m_OptionIndex[row] + delta, (int)m_Options[row].size() - 1);
}

StreamMenu::Action StreamMenu::adjustVolume(int delta)
{
    return setVolumeIndex(m_VolumeIndex + delta);
}

StreamMenu::Action StreamMenu::setVolumeIndex(int index)
{
    index = qBound(0, index, (int)m_VolumeOptions.size() - 1);
    if (index == m_VolumeIndex) {
        return Action::None;
    }

    // The volume changes right away, the Session redraws the menu
    m_VolumeIndex = index;
    return Action::SetVolume;
}

StreamMenu::Action StreamMenu::activateItem(MenuItem item)
{
    switch (item) {
    case ItemQuitStream:
        return Action::QuitStream;

    case ItemStreamSettings:
        m_SettingsOpen = !m_SettingsOpen;
        m_SettingsRow = RowResolution;
        return Action::None;

    case ItemVolume:
        m_VolumeOpen = !m_VolumeOpen;
        return Action::None;

    case ItemStatsOverlay:
        return Action::ToggleStatsOverlay;

    case ItemForceQuitApp:
        // Require a second confirmation before quitting the app
        if (m_ForceQuitArmed) {
            return Action::ForceQuitApp;
        }
        m_ForceQuitArmed = true;
        return Action::None;

    default:
        Q_UNREACHABLE();
    }

    return Action::None;
}

StreamMenu::Action StreamMenu::navigate(Nav nav)
{
    Action action = Action::None;

    if (m_VolumeOpen) {
        switch (nav) {
        case NavLeft:
            action = adjustVolume(-1);
            break;
        case NavRight:
            action = adjustVolume(1);
            break;
        case NavDown:
        case NavNext:
            break;
        case NavUp:
        case NavPrev:
        case NavAccept:
        case NavBack:
            m_VolumeOpen = false;
            break;
        }
    }
    else if (!m_SettingsOpen) {
        switch (nav) {
        case NavLeft:
        case NavPrev:
            setSelectedItem((m_SelectedItem + ItemMax - 1) % ItemMax);
            break;
        case NavRight:
        case NavNext:
            setSelectedItem((m_SelectedItem + 1) % ItemMax);
            break;
        case NavUp:
            break;
        case NavDown:
            if (m_SelectedItem == ItemStreamSettings || m_SelectedItem == ItemVolume) {
                action = activateItem((MenuItem)m_SelectedItem);
            }
            break;
        case NavAccept:
            action = activateItem((MenuItem)m_SelectedItem);
            break;
        case NavBack:
            action = Action::Close;
            break;
        }
    }
    else {
        // Tab moves through the buttons before leaving the button row
        if (nav == NavNext && m_SettingsRow == RowApply && m_SelectedButton == ButtonApply) {
            nav = NavRight;
        }
        else if (nav == NavPrev && m_SettingsRow == RowApply && m_SelectedButton == ButtonSave) {
            nav = NavLeft;
        }

        switch (nav) {
        case NavUp:
        case NavPrev:
            if (m_SettingsRow == RowResolution) {
                m_SettingsOpen = false;
            }
            else {
                m_SettingsRow--;
            }
            break;
        case NavDown:
        case NavNext:
            if (m_SettingsRow == RowVideoCodec) {
                focusButtonRow();
            }
            else if (m_SettingsRow < RowVideoCodec) {
                m_SettingsRow++;
            }
            break;
        case NavLeft:
        case NavRight:
            if (m_SettingsRow == RowApply) {
                m_SelectedButton = nav == NavLeft ? ButtonApply : ButtonSave;
            }
            else {
                adjustRow(m_SettingsRow, nav == NavLeft ? -1 : 1);
            }
            break;
        case NavAccept:
            if (m_SettingsRow == RowApply) {
                action = activateButton();
            }
            else {
                focusButtonRow();
            }
            break;
        case NavBack:
            m_SettingsOpen = false;
            break;
        }
    }

    if (action == Action::None) {
        render();
    }
    return action;
}

QPoint StreamMenu::windowToSurface(int x, int y) const
{
    int windowWidth, windowHeight, drawableWidth, drawableHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    getDrawableSize(m_Window, &drawableWidth, &drawableHeight);

    // Mouse coordinates are in window points but the overlay is in pixels
    if (windowWidth > 0 && windowHeight > 0) {
        x = x * drawableWidth / windowWidth;
        y = y * drawableHeight / windowHeight;
    }

    // Renderers center the overlay in the drawable area
    return QPoint(x - (drawableWidth - m_Layout.size.width()) / 2,
                  y - (drawableHeight - m_Layout.size.height()) / 2);
}

int StreamMenu::trackIndexAt(const QRect& track, int count, int x)
{
    if (count <= 1 || track.width() <= 0) {
        return 0;
    }

    double position = (double)(x - track.left()) / track.width();
    return qBound(0, (int)qRound(position * (count - 1)), count - 1);
}

bool StreamMenu::handleMouseEvent(const SDL_Event& event, Action& action)
{
    switch (event.type) {
    case SDL_MOUSEMOTION:
    {
        QPoint pos = windowToSurface(event.motion.x, event.motion.y);

        if (m_DraggingVolume) {
            action = setVolumeIndex(trackIndexAt(m_Layout.volumeTrack, m_VolumeOptions.size(), pos.x()));
            return true;
        }

        if (m_DraggingRow >= 0) {
            int index = trackIndexAt(m_Layout.tracks[m_DraggingRow], m_Options[m_DraggingRow].size(), pos.x());
            if (index != m_OptionIndex[m_DraggingRow]) {
                m_OptionIndex[m_DraggingRow] = index;
                render();
            }
            return true;
        }

        bool changed = false;
        if (m_SettingsOpen) {
            // Hovering a row focuses it, without collapsing the settings
            for (int row = 0; row <= RowVideoCodec; row++) {
                if (m_Layout.rows[row].contains(pos) && m_SettingsRow != row) {
                    m_SettingsRow = row;
                    changed = true;
                }
            }
            for (int button = 0; button < ButtonMax; button++) {
                if (m_Layout.buttons[button].contains(pos) &&
                        (m_SettingsRow != RowApply || m_SelectedButton != button)) {
                    m_SettingsRow = RowApply;
                    m_SelectedButton = button;
                    changed = true;
                }
            }
        }
        else {
            for (int item = 0; item < ItemMax; item++) {
                if (m_Layout.tiles[item].contains(pos) && m_SelectedItem != item) {
                    setSelectedItem(item);
                    changed = true;
                }
            }
        }

        if (changed) {
            render();
        }
        return true;
    }

    case SDL_MOUSEBUTTONDOWN:
    {
        QPoint pos = windowToSurface(event.button.x, event.button.y);

        if (event.button.button == SDL_BUTTON_RIGHT) {
            action = navigate(NavBack);
            return true;
        }
        else if (event.button.button != SDL_BUTTON_LEFT) {
            return true;
        }

        for (int item = 0; item < ItemMax; item++) {
            if (m_Layout.tiles[item].contains(pos)) {
                setSelectedItem(item);
                action = activateItem((MenuItem)item);
                if (action == Action::None) {
                    render();
                }
                return true;
            }
        }

        if (m_SettingsOpen) {
            for (int row = 0; row <= RowVideoCodec; row++) {
                if (m_Layout.rows[row].contains(pos)) {
                    m_SettingsRow = row;

                    // Only the slider area below the title changes the value
                    if (pos.y() >= m_Layout.tracks[row].top() - qRound(16 * scale())) {
                        m_DraggingRow = row;
                        m_OptionIndex[row] = trackIndexAt(m_Layout.tracks[row], m_Options[row].size(), pos.x());
                    }
                    render();
                    return true;
                }
            }

            for (int button = 0; button < ButtonMax; button++) {
                if (m_Layout.buttons[button].contains(pos)) {
                    m_SettingsRow = RowApply;
                    m_SelectedButton = button;
                    action = navigate(NavAccept);
                    return true;
                }
            }

            // Clicks between the buttons do nothing
            if (m_Layout.rows[RowApply].contains(pos)) {
                return true;
            }
        }

        if (m_VolumeOpen && m_Layout.volumeRow.contains(pos)) {
            // Only the slider area below the title changes the value
            if (pos.y() >= m_Layout.volumeTrack.top() - qRound(16 * scale())) {
                m_DraggingVolume = true;
                action = setVolumeIndex(trackIndexAt(m_Layout.volumeTrack, m_VolumeOptions.size(), pos.x()));
            }
            return true;
        }

        // Clicking outside of the menu closes it
        if (!QRect(QPoint(0, 0), m_Layout.size).contains(pos)) {
            action = Action::Close;
        }
        return true;
    }

    case SDL_MOUSEBUTTONUP:
        m_DraggingRow = -1;
        m_DraggingVolume = false;
        return true;

    case SDL_MOUSEWHEEL:
        if (m_VolumeOpen && event.wheel.y != 0) {
            action = adjustVolume(event.wheel.y > 0 ? 1 : -1);
        }
        else if (m_SettingsOpen && m_SettingsRow != RowApply && event.wheel.y != 0) {
            adjustRow(m_SettingsRow, event.wheel.y > 0 ? 1 : -1);
            render();
        }
        return true;
    }

    return false;
}

bool StreamMenu::handleEvent(const SDL_Event& event, Action& action)
{
    action = Action::None;

    if (!m_Open) {
        return false;
    }

    bool wasUsingGamepad = m_UsingGamepad;

    switch (event.type) {
    case SDL_KEYDOWN:
    {
        const SDL_Keysym& keysym = event.key.keysym;
        const bool comboModifiers = (keysym.mod & KMOD_CTRL) && (keysym.mod & KMOD_ALT) && (keysym.mod & KMOD_SHIFT);
        if (comboModifiers && (keysym.sym == SDLK_o || keysym.scancode == SDL_SCANCODE_O)) {
            // The shortcut that opened the menu closes it again, but holding
            // it must not close the menu it just opened
            m_UsingGamepad = false;
            if (!event.key.repeat) {
                action = Action::Close;
            }
            return true;
        }
        else if (comboModifiers && !isModifierKey(keysym.sym)) {
            // Let the input handler perform its other key combos while the menu is open
            return false;
        }

        m_UsingGamepad = false;

        switch (keysym.sym) {
        case SDLK_LEFT:
            action = navigate(NavLeft);
            break;
        case SDLK_RIGHT:
            action = navigate(NavRight);
            break;
        case SDLK_UP:
            action = navigate(NavUp);
            break;
        case SDLK_DOWN:
            action = navigate(NavDown);
            break;
        case SDLK_TAB:
            action = navigate((keysym.mod & KMOD_SHIFT) ? NavPrev : NavNext);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!event.key.repeat) {
                action = navigate(NavAccept);
            }
            break;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
            if (!event.key.repeat) {
                action = navigate(NavBack);
            }
            break;
        default:
            break;
        }
        break;
    }

    case SDL_KEYUP:
        // The input handler only forwards releases of keys it saw pressed. Keys were
        // raised when the menu opened, so releases of menu keys are dropped there, while
        // keys passed through above (Ctrl+Alt+Shift combos) are released on the host.
        return false;

    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
        // Keys were raised on the host when the menu opened
        return true;

    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        m_UsingGamepad = false;
        if (!handleMouseEvent(event, action)) {
            return false;
        }
        break;

    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        // SDL synthesizes mouse events from touches, which we use instead
        return true;

    case SDL_CONTROLLERBUTTONDOWN:
    {
        m_UsingGamepad = true;

        Uint8 button = event.cbutton.button;
        if (m_SwapFaceButtons) {
            if (button == SDL_CONTROLLER_BUTTON_A) {
                button = SDL_CONTROLLER_BUTTON_B;
            }
            else if (button == SDL_CONTROLLER_BUTTON_B) {
                button = SDL_CONTROLLER_BUTTON_A;
            }
        }

        switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            action = navigate(NavLeft);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            action = navigate(NavRight);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            action = navigate(NavUp);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            action = navigate(NavDown);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            action = navigate(NavPrev);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            action = navigate(NavNext);
            break;
        case SDL_CONTROLLER_BUTTON_A:
            action = navigate(NavAccept);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            action = navigate(NavBack);
            break;
        case SDL_CONTROLLER_BUTTON_START:
            action = Action::Close;
            break;
        default:
            break;
        }
        break;
    }

    case SDL_CONTROLLERBUTTONUP:
        // Let the input handler see button releases to keep its gamepad state in sync
        return false;

    case SDL_CONTROLLERAXISMOTION:
    {
        int axis;
        if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            axis = 0;
        }
        else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            axis = 1;
        }
        else {
            return true;
        }

        int direction = event.caxis.value > STICK_NAV_THRESHOLD ? 1 :
                        event.caxis.value < -STICK_NAV_THRESHOLD ? -1 : 0;
        if (direction != m_StickDirection[axis]) {
            m_StickDirection[axis] = direction;
            if (direction != 0) {
                m_UsingGamepad = true;
                if (axis == 0) {
                    action = navigate(direction < 0 ? NavLeft : NavRight);
                }
                else {
                    action = navigate(direction < 0 ? NavUp : NavDown);
                }
            }
        }
        return true;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLERSENSORUPDATE:
    case SDL_CONTROLLERTOUCHPADDOWN:
    case SDL_CONTROLLERTOUCHPADUP:
    case SDL_CONTROLLERTOUCHPADMOTION:
        return true;
#endif

    default:
        return false;
    }

    // Update the hints if the input device changed
    if (m_Open && action == Action::None && wasUsingGamepad != m_UsingGamepad) {
        render();
    }

    return true;
}

double StreamMenu::scale() const
{
    int width, height;
    getDrawableSize(m_Window, &width, &height);
    double s = qBound(0.8, SDL_min(height / 1000.0, width / 1600.0), 3.0);

    // Shrink the menu below the usual minimum in small windows, so it always fits
    // inside the drawable with expanded settings. The margin also absorbs rounding.
    const QSize baseSize = computeLayout(1.0, true, false).size;
    double fit = SDL_min((double)(width - 2 * MENU_FIT_MARGIN) / baseSize.width(),
                         (double)(height - 2 * MENU_FIT_MARGIN) / baseSize.height());
    return qMax(MENU_MIN_SCALE, SDL_min(s, fit));
}

StreamMenu::Layout StreamMenu::computeLayout(double s, bool settingsOpen, bool volumeOpen) const
{
    Layout layout = {};

    const int padding = qRound(12 * s);
    const int gap = qRound(8 * s);
    const int tileWidth = qRound(140 * s);
    const int tileHeight = qRound(112 * s);
    const int width = ItemMax * tileWidth + (ItemMax - 1) * gap + 2 * padding;

    int y = padding;
    for (int item = 0; item < ItemMax; item++) {
        layout.tiles[item] = QRect(padding + item * (tileWidth + gap), y, tileWidth, tileHeight);
    }
    y += tileHeight + padding;

    const int rowHeight = qRound(76 * s);
    const int trackInset = qRound(24 * s);

    if (settingsOpen) {
        const int top = y;

        y += gap;
        for (int row = 0; row <= RowVideoCodec; row++) {
            layout.rows[row] = QRect(padding, y, width - 2 * padding, rowHeight);
            layout.tracks[row] = QRect(padding + trackInset, y + qRound(42 * s),
                                       width - 2 * (padding + trackInset), qRound(4 * s));
            y += rowHeight;
        }

        y += gap;
        layout.rows[RowApply] = QRect(padding, y, width - 2 * padding, qRound(40 * s));
        const int buttonWidth = (layout.rows[RowApply].width() - gap) / 2;
        layout.buttons[ButtonApply] = QRect(layout.rows[RowApply].left(), y, buttonWidth, layout.rows[RowApply].height());
        layout.buttons[ButtonSave] = QRect(layout.rows[RowApply].right() + 1 - buttonWidth, y, buttonWidth, layout.rows[RowApply].height());
        y += layout.rows[RowApply].height() + padding;

        layout.settingsPanel = QRect(0, top, width, y - top);
    }
    else if (volumeOpen) {
        const int top = y;

        y += gap;
        layout.volumeRow = QRect(padding, y, width - 2 * padding, rowHeight);
        layout.volumeTrack = QRect(padding + trackInset, y + qRound(42 * s),
                                   width - 2 * (padding + trackInset), qRound(4 * s));
        y += rowHeight + padding;

        layout.volumePanel = QRect(0, top, width, y - top);
    }

    layout.footer = QRect(0, y, width, qRound(30 * s));
    y += layout.footer.height();

    layout.size = QSize(width, y);
    return layout;
}

QString StreamMenu::acceptKeyName() const
{
    // Swapped face buttons accept with the physical B button
    if (m_UsingGamepad) {
        return m_SwapFaceButtons ? QStringLiteral("B") : QStringLiteral("A");
    }
    return tr("Enter");
}

QString StreamMenu::backKeyName() const
{
    if (m_UsingGamepad) {
        return m_SwapFaceButtons ? QStringLiteral("A") : QStringLiteral("B");
    }
    return tr("Esc");
}

QString StreamMenu::hintText() const
{
    const QString accept = acceptKeyName();
    const QString back = backKeyName();

    if (m_ForceQuitArmed) {
        return tr("Press %1 again to quit the app on the host").arg(accept);
    }
    else if (m_VolumeOpen) {
        return tr("← →  Adjust volume     %1  Done     %2  Back").arg(accept, back);
    }
    else if (m_SettingsOpen && m_SettingsRow == RowApply) {
        return tr("↑  Settings     ← →  Apply or Save     %1  Select     %2  Back").arg(accept, back);
    }
    else if (m_SettingsOpen) {
        return tr("↑ ↓  Choose     ← →  Adjust     %1  Continue     %2  Back").arg(accept, back);
    }
    else {
        return tr("← →  Move     %1  Select     %2  Close").arg(accept, back);
    }
}

void StreamMenu::drawIcon(QPainter& painter, const QString& resource, const QRect& rect, const QColor& color)
{
    // The size is part of the key, but clearing on scale changes keeps the cache small
    if (m_IconCacheScale != scale()) {
        m_IconCache.clear();
        m_IconCacheScale = scale();
    }

    QString key = QString("%1|%2x%3|%4").arg(resource).arg(rect.width()).arg(rect.height()).arg(color.rgba());
    auto it = m_IconCache.constFind(key);
    if (it == m_IconCache.constEnd()) {
        QSvgRenderer renderer(resource);
        QImage icon(rect.size(), QImage::Format_ARGB32_Premultiplied);
        icon.fill(Qt::transparent);

        {
            QPainter iconPainter(&icon);
            iconPainter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&iconPainter);

            // Tint the icon
            iconPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            iconPainter.fillRect(icon.rect(), color);
        }

        it = m_IconCache.insert(key, icon);
    }

    painter.drawImage(rect.topLeft(), *it);
}

static QFont menuFont(double pixelSize, bool bold = false)
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    font.setBold(bold);
    return font;
}

void StreamMenu::drawSliderRow(QPainter& painter, double s, const QRect& rowRect, const QRect& track,
                               const QString& title, const QString& value, const QVector<Option>& options,
                               int index, bool focused)
{
    if (focused) {
        painter.fillRect(rowRect, k_RowFocusColor);
    }

    // Row title and value, side by side without overlapping. The value gets
    // the space it needs, but leaves at least 40% of the row to the title.
    const QFont titleFont = menuFont(14 * s, focused);
    const QFont valueFont = menuFont(14 * s, true);
    const int titleSpacing = qRound(12 * s);
    const int available = qMax(0, track.width() - titleSpacing);
    const int titleWidth = textWidth(painter, titleFont, title);
    const int valueWidth = qMin(textWidth(painter, valueFont, value),
                                qMax(available - titleWidth, available * 3 / 5));
    const int textTop = rowRect.top() + qRound(8 * s);
    const int textHeight = qRound(22 * s);

    painter.setFont(titleFont);
    painter.setPen(focused ? k_TextColor : k_SecondaryTextColor);
    drawFittedText(painter, QRect(track.left(), textTop, available - valueWidth, textHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, title);

    painter.setFont(valueFont);
    painter.setPen(k_TextColor);
    drawFittedText(painter, QRect(track.right() + 1 - valueWidth, textTop, valueWidth, textHeight),
                   Qt::AlignRight | Qt::AlignVCenter, value);

    // Track
    auto tickX = [&](int i) {
        if (options.size() <= 1) {
            return track.center().x();
        }
        return track.left() + qRound((double)i * track.width() / (options.size() - 1));
    };

    int selectedX = tickX(index);
    painter.fillRect(track, k_TrackColor);
    painter.fillRect(QRect(track.left(), track.top(), selectedX - track.left(), track.height()), k_TrackFillColor);

    const int tickWidth = qMax(2, qRound(2 * s));
    const int tickHeight = qRound(10 * s);

    // Option labels are centered on their tick, except at the track ends where they
    // are aligned to the track end. Limit their width so neighbors never overlap.
    const int labelGap = qRound(6 * s);
    int labelWidth = qRound(90 * s);
    int endLabelWidth = labelWidth;
    if (options.size() == 1) {
        labelWidth = endLabelWidth = qMin(labelWidth, track.width());
    }
    else if (options.size() == 2) {
        endLabelWidth = qBound(0, (track.width() - labelGap) / 2, labelWidth);
    }
    else {
        const int spacing = track.width() / (options.size() - 1);
        labelWidth = qBound(0, spacing - labelGap, labelWidth);
        endLabelWidth = qBound(0, spacing - labelWidth / 2 - labelGap, qRound(90 * s));
    }

    for (int i = 0; i < options.size(); i++) {
        int x = tickX(i);
        painter.fillRect(QRect(x - tickWidth / 2, track.center().y() - tickHeight / 2, tickWidth, tickHeight),
                         i <= index ? k_TrackFillColor : k_TickColor);

        QRect labelRect(x - labelWidth / 2, track.bottom() + qRound(8 * s), labelWidth, qRound(18 * s));
        Qt::Alignment alignment = Qt::AlignHCenter;
        if (options.size() > 1 && i == 0) {
            labelRect.setWidth(endLabelWidth);
            labelRect.moveLeft(track.left());
            alignment = Qt::AlignLeft;
        }
        else if (options.size() > 1 && i == options.size() - 1) {
            labelRect.setWidth(endLabelWidth);
            labelRect.moveRight(track.right());
            alignment = Qt::AlignRight;
        }
        painter.setFont(menuFont(12 * s, i == index));
        painter.setPen(i == index ? k_TextColor : k_SecondaryTextColor);
        drawFittedText(painter, labelRect, alignment | Qt::AlignVCenter, options[i].label);
    }

    // Square handle, matching the square corners of the menu
    const int handleSize = qRound((focused ? 16 : 14) * s);
    painter.fillRect(QRect(selectedX - handleSize / 2, track.center().y() - handleSize / 2, handleSize, handleSize),
                     focused ? k_TextColor : k_TrackFillColor);
}

void StreamMenu::render()
{
    if (!m_Open || m_Window == nullptr) {
        return;
    }

    const double s = scale();
    m_Layout = computeLayout(s, m_SettingsOpen, m_VolumeOpen);

    QImage image(m_Layout.size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Container
    const QRect bounds(QPoint(0, 0), m_Layout.size);
    painter.fillRect(bounds, k_PanelColor);
    painter.setPen(QPen(k_BorderColor, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(bounds).adjusted(0.5, 0.5, -0.5, -0.5));

    // Tiles
    static const char* const k_Icons[ItemMax] = {
        ":/res/logout.svg",
        ":/res/tune.svg",
        ":/res/volume_up.svg",
        ":/res/speed.svg",
        ":/res/stop_FILL1_wght700_GRAD200_opsz48.svg",
    };
    const QString labels[ItemMax] = {
        tr("Quit stream"),
        tr("Stream settings"),
        tr("Volume: %1%").arg(getVolume()),
        m_OverlayManager.isOverlayEnabled(Overlay::OverlayDebug) ? tr("Stats: On") : tr("Stats: Off"),
        m_ForceQuitArmed ? tr("Press again") : tr("Force quit app"),
    };

    for (int item = 0; item < ItemMax; item++) {
        const QRect& tile = m_Layout.tiles[item];
        bool selected = item == m_SelectedItem;

        QColor background = k_TileColor;
        if (selected) {
            background = (item == ItemForceQuitApp && m_ForceQuitArmed) ? k_DangerColor : k_SelectedColor;
        }
        painter.fillRect(tile, background);

        const int iconSize = qRound(44 * s);
        QRect iconRect(tile.center().x() - iconSize / 2 + 1, tile.top() + qRound(18 * s), iconSize, iconSize);
        drawIcon(painter, k_Icons[item], iconRect, selected ? k_TextColor : k_SecondaryTextColor);

        painter.setFont(menuFont(15 * s, selected));
        painter.setPen(selected ? k_TextColor : k_SecondaryTextColor);
        const int labelInset = qRound(6 * s);
        drawFittedText(painter,
                       QRect(tile.left() + labelInset, iconRect.bottom() + qRound(10 * s),
                             tile.width() - 2 * labelInset, qRound(22 * s)),
                       Qt::AlignHCenter | Qt::AlignVCenter, labels[item]);
    }

    // Stream settings
    if (m_SettingsOpen) {
        painter.setPen(QPen(k_BorderColor, 1));
        painter.drawLine(m_Layout.settingsPanel.left() + 1, m_Layout.settingsPanel.top(),
                         m_Layout.settingsPanel.right() - 1, m_Layout.settingsPanel.top());

        const QString rowLabels[RowVideoCodec + 1] = {
            tr("Resolution"),
            tr("Frame rate"),
            tr("Video codec"),
        };

        for (int row = 0; row <= RowVideoCodec; row++) {
            // Changed rows show the value of the current stream as well
            QString value = m_Options[row][m_OptionIndex[row]].label;
            if (m_OptionIndex[row] != m_InitialIndex[row]) {
                value = tr("%1  →  %2").arg(m_Options[row][m_InitialIndex[row]].label, value);
            }

            drawSliderRow(painter, s, m_Layout.rows[row], m_Layout.tracks[row], rowLabels[row], value,
                          m_Options[row], m_OptionIndex[row], row == m_SettingsRow);
        }

        // Apply and Save buttons
        const bool enabled[ButtonMax] = {
            hasSettingsChanges(),
            canSaveSettings(),
        };
        const QString buttonLabels[ButtonMax] = {
            tr("Apply"),
            (m_JustSaved && !enabled[ButtonSave]) ? tr("Saved") : tr("Save"),
        };
        for (int button = 0; button < ButtonMax; button++) {
            bool focused = m_SettingsRow == RowApply && m_SelectedButton == button;
            painter.fillRect(m_Layout.buttons[button], focused ? k_SelectedColor : k_TileColor);
            painter.setFont(menuFont(14 * s, focused));
            painter.setPen(enabled[button] ? k_TextColor : k_DisabledTextColor);
            const int buttonInset = qRound(8 * s);
            drawFittedText(painter, m_Layout.buttons[button].adjusted(buttonInset, 0, -buttonInset, 0),
                           Qt::AlignCenter, buttonLabels[button]);
        }
    }

    // Volume
    if (m_VolumeOpen) {
        painter.setPen(QPen(k_BorderColor, 1));
        painter.drawLine(m_Layout.volumePanel.left() + 1, m_Layout.volumePanel.top(),
                         m_Layout.volumePanel.right() - 1, m_Layout.volumePanel.top());

        drawSliderRow(painter, s, m_Layout.volumeRow, m_Layout.volumeTrack, tr("Client volume"),
                      m_VolumeOptions[m_VolumeIndex].label, m_VolumeOptions, m_VolumeIndex, true);
    }

    // Footer hints
    painter.setPen(QPen(k_BorderColor, 1));
    painter.drawLine(m_Layout.footer.left() + 1, m_Layout.footer.top(),
                     m_Layout.footer.right() - 1, m_Layout.footer.top());
    painter.setFont(menuFont(12 * s));
    painter.setPen(m_ForceQuitArmed ? QColor(0xEF, 0x9A, 0x9A) : k_SecondaryTextColor);
    const int footerInset = qRound(12 * s);
    drawFittedText(painter, m_Layout.footer.adjusted(footerInset, 0, -footerInset, 0), Qt::AlignCenter, hintText());

    painter.end();

    // Overlays use non-premultiplied ARGB8888 surfaces like SDL_ttf produces
    QImage converted = image.convertToFormat(QImage::Format_ARGB32);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, converted.width(), converted.height(),
                                                          32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateRGBSurfaceWithFormat() failed: %s",
                     SDL_GetError());
        return;
    }

    for (int y = 0; y < converted.height(); y++) {
        memcpy((Uint8*)surface->pixels + y * surface->pitch,
               converted.constScanLine(y),
               converted.width() * 4);
    }

    m_OverlayManager.updateOverlaySurface(Overlay::OverlayStreamMenu, surface);
}
