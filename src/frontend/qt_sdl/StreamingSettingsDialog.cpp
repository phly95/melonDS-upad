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

#ifdef HAVE_GSTREAMER

#include "StreamingSettingsDialog.h"
#include "ui_StreamingSettingsDialog.h"

#include "Platform.h"
#include "Config.h"
#include "main.h"
#include "Window.h"
#include "EmuInstance.h"

StreamingSettingsDialog* StreamingSettingsDialog::currentDlg = nullptr;

StreamingSettingsDialog::StreamingSettingsDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::StreamingSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();
    auto& cfg = emuInstance->getGlobalConfig();

    ui->streaming_enabled_check->setChecked(cfg.GetBool("Streaming.Enabled"));
    ui->streaming_screen_combo->setCurrentIndex(cfg.GetInt("Streaming.Screen"));
    ui->streaming_encoder_combo->setCurrentIndex(cfg.GetInt("Streaming.Encoder"));
    ui->streaming_gpu_edit->setText(QString::fromStdString(cfg.GetString("Streaming.GPUDevice")));
    ui->streaming_custom_res_check->setChecked(cfg.GetBool("Streaming.CustomResolution"));
    ui->streaming_width_spin->setValue(cfg.GetInt("Streaming.Width"));
    ui->streaming_height_spin->setValue(cfg.GetInt("Streaming.Height"));
    ui->target_ip_edit->setText(QString::fromStdString(cfg.GetString("Streaming.TargetIP")));
    ui->target_port_spin->setValue(cfg.GetInt("Streaming.TargetPort"));

    UpdateCustomResEnabled();
}

StreamingSettingsDialog::~StreamingSettingsDialog() { delete ui; }

void StreamingSettingsDialog::UpdateCustomResEnabled()
{
    bool custom = ui->streaming_custom_res_check->isChecked();
    ui->streaming_width_spin->setEnabled(custom);
    ui->streaming_height_spin->setEnabled(custom);
    ui->streaming_width_label->setEnabled(custom);
    ui->streaming_height_label->setEnabled(custom);
}

void StreamingSettingsDialog::on_streaming_custom_res_check_stateChanged(int) { UpdateCustomResEnabled(); }

void StreamingSettingsDialog::on_StreamingSettingsDialog_accepted()
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("Streaming.Enabled", ui->streaming_enabled_check->isChecked());
    cfg.SetInt("Streaming.Screen", ui->streaming_screen_combo->currentIndex());
    cfg.SetInt("Streaming.Encoder", ui->streaming_encoder_combo->currentIndex());
    cfg.SetString("Streaming.GPUDevice", ui->streaming_gpu_edit->text().toStdString());
    cfg.SetBool("Streaming.CustomResolution", ui->streaming_custom_res_check->isChecked());
    cfg.SetInt("Streaming.Width", ui->streaming_width_spin->value());
    cfg.SetInt("Streaming.Height", ui->streaming_height_spin->value());
    cfg.SetString("Streaming.TargetIP", ui->target_ip_edit->text().toStdString());
    cfg.SetInt("Streaming.TargetPort", ui->target_port_spin->value());
    Config::Save();
    emit updateStreamingSettings();
    closeDlg();
}

void StreamingSettingsDialog::on_StreamingSettingsDialog_rejected() { closeDlg(); }

#endif // HAVE_GSTREAMER
