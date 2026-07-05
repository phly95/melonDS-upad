/*
    Copyright 2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef STREAMINGSETTINGSDIALOG_H
#define STREAMINGSETTINGSDIALOG_H

#ifdef HAVE_GSTREAMER

#include <QDialog>

namespace Ui { class StreamingSettingsDialog; }
class EmuInstance;

class StreamingSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StreamingSettingsDialog(QWidget* parent);
    ~StreamingSettingsDialog();

    static StreamingSettingsDialog* currentDlg;
    static StreamingSettingsDialog* openDlg(QWidget* parent)
    {
        if (currentDlg) { currentDlg->activateWindow(); return currentDlg; }
        currentDlg = new StreamingSettingsDialog(parent);
        currentDlg->show();
        return currentDlg;
    }
    static void closeDlg() { currentDlg = nullptr; }

signals:
    void updateStreamingSettings();

private slots:
    void on_StreamingSettingsDialog_accepted();
    void on_StreamingSettingsDialog_rejected();
    void on_streaming_custom_res_check_stateChanged(int state);

private:
    void UpdateCustomResEnabled();
    Ui::StreamingSettingsDialog* ui;
    EmuInstance* emuInstance;
};

#endif // HAVE_GSTREAMER
#endif // STREAMINGSETTINGSDIALOG_H
