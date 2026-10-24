#pragma once

#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include "SDL_compat.h"
#include "settings/streamingpreferences.h"

class QPainter;

namespace Overlay {
class OverlayManager;
}

// In-stream quick menu (Ctrl+Alt+Shift+O or Select+L1+R1+Y) drawn as a
// pre-rendered overlay. It only manages its own state and drawing; the
// Session performs the selected action.
class StreamMenu
{
    Q_DECLARE_TR_FUNCTIONS(StreamMenu)

public:
    enum class Action
    {
        None,
        Close,
        QuitStream,
        ForceQuitApp,
        ApplySettings,
        SaveSettings,
        ToggleStatsOverlay,
        SetVolume,
    };

    struct StreamSettings
    {
        int width;
        int height;
        int fps;
        StreamingPreferences::VideoCodecConfig videoCodecConfig;

        bool operator==(const StreamSettings& other) const
        {
            return width == other.width && height == other.height &&
                   fps == other.fps && videoCodecConfig == other.videoCodecConfig;
        }
    };

    enum SettingsRowMask : unsigned
    {
        RowMaskResolution = 1,
        RowMaskFrameRate = 2,
        RowMaskVideoCodec = 4,
    };

    explicit StreamMenu(Overlay::OverlayManager& overlayManager);

    // appliedRows are the rows already changed by an earlier Apply in this stream.
    // The Stats tile reads the live stats overlay state, so statsOverlayEnabled is unused.
    // volume is the current client playback volume in percent.
    void open(SDL_Window* window, const StreamSettings& current, const StreamSettings& saved,
              bool statsOverlayEnabled, bool fromGamepad, bool swapFaceButtons, unsigned appliedRows,
              int volume);
    void close();
    bool isOpen() const;

    // Redraws the menu, for example after the window was resized
    void refresh();

    // Called after the selected settings were saved without reconnecting
    void setSavedSettings(const StreamSettings& saved);

    // Returns true if the event was consumed by the menu. Any action the
    // user selected is returned in action.
    bool handleEvent(const SDL_Event& event, Action& action);

    StreamSettings getSelectedSettings() const;

    // Volume in percent selected with the Volume tile (Action::SetVolume)
    int getVolume() const;

    // Returns true if the selected settings differ from the current stream
    bool hasSettingsChanges() const;

    // Rows whose selection differs from the current stream, combined with
    // the rows changed by an earlier Apply (SettingsRowMask values)
    unsigned getChangedRows() const;

private:
    enum MenuItem
    {
        ItemQuitStream,
        ItemStreamSettings,
        ItemVolume,
        ItemStatsOverlay,
        ItemForceQuitApp,
        ItemMax
    };

    enum SettingsRow
    {
        RowResolution,
        RowFrameRate,
        RowVideoCodec,
        RowApply,
        RowMax
    };

    enum Button
    {
        ButtonApply,
        ButtonSave,
        ButtonMax
    };

    enum Nav
    {
        NavLeft,
        NavRight,
        NavUp,
        NavDown,
        NavNext,
        NavPrev,
        NavAccept,
        NavBack,
    };

    struct Option
    {
        QString label;
        int a;
        int b;
    };

    struct Layout
    {
        QSize size;
        QRect tiles[ItemMax];
        QRect settingsPanel;
        QRect rows[RowMax];
        QRect tracks[RowVideoCodec + 1];
        QRect buttons[ButtonMax];
        QRect volumePanel;
        QRect volumeRow;
        QRect volumeTrack;
        QRect footer;
    };

    Action navigate(Nav nav);
    Action activateItem(MenuItem item);
    void setSelectedItem(int item);
    void adjustRow(int row, int delta);
    Action adjustVolume(int delta);
    Action setVolumeIndex(int index);
    bool canSaveSettings() const;
    void focusButtonRow();
    Action activateButton();

    bool handleMouseEvent(const SDL_Event& event, Action& action);
    QPoint windowToSurface(int x, int y) const;
    static int trackIndexAt(const QRect& track, int count, int x);

    double scale() const;
    Layout computeLayout(double s, bool settingsOpen, bool volumeOpen) const;
    void render();
    void drawSliderRow(QPainter& painter, double s, const QRect& rowRect, const QRect& track,
                       const QString& title, const QString& value, const QVector<Option>& options,
                       int index, bool focused);
    void drawIcon(QPainter& painter, const QString& resource, const QRect& rect, const QColor& color);
    QString acceptKeyName() const;
    QString backKeyName() const;
    QString hintText() const;

    Overlay::OverlayManager& m_OverlayManager;
    SDL_Window* m_Window;
    bool m_Open;
    bool m_UsingGamepad;
    bool m_SwapFaceButtons;
    unsigned m_AppliedRows;

    int m_SelectedItem;
    bool m_ForceQuitArmed;
    bool m_SettingsOpen;
    int m_SettingsRow;
    int m_SelectedButton;
    StreamSettings m_SavedSettings;
    bool m_JustSaved;
    int m_DraggingRow;
    bool m_VolumeOpen;
    bool m_DraggingVolume;
    QVector<Option> m_VolumeOptions;
    int m_VolumeIndex;

    QVector<Option> m_Options[RowVideoCodec + 1];
    int m_OptionIndex[RowVideoCodec + 1];
    int m_InitialIndex[RowVideoCodec + 1];

    // Left stick direction last used for navigation, per axis
    int m_StickDirection[2];

    Layout m_Layout;

    QHash<QString, QImage> m_IconCache;
    double m_IconCacheScale;
};
