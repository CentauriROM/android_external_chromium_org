// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/external_protocol_dialog.h"

#include "base/metrics/histogram.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/win/registry.h"
#include "chrome/browser/external_protocol/external_protocol_handler.h"
#include "chrome/browser/tab_contents/tab_util.h"
#include "chrome/browser/ui/views/constrained_window_views.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_view.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/text_elider.h"
#include "ui/views/controls/message_box_view.h"
#include "ui/views/widget/widget.h"

using content::WebContents;

namespace {

const int kMessageWidth = 400;

}  // namespace

///////////////////////////////////////////////////////////////////////////////
// ExternalProtocolHandler

// static
void ExternalProtocolHandler::RunExternalProtocolDialog(
    const GURL& url, int render_process_host_id, int routing_id) {
  std::wstring command =
      ExternalProtocolDialog::GetApplicationForProtocol(url);
  if (command.empty()) {
    // ShellExecute won't do anything. Don't bother warning the user.
    return;
  }
  // Windowing system takes ownership.
  new ExternalProtocolDialog(url, render_process_host_id, routing_id, command);
}

///////////////////////////////////////////////////////////////////////////////
// ExternalProtocolDialog

ExternalProtocolDialog::~ExternalProtocolDialog() {
}

//////////////////////////////////////////////////////////////////////////////
// ExternalProtocolDialog, views::DialogDelegate implementation:

int ExternalProtocolDialog::GetDefaultDialogButton() const {
  return ui::DIALOG_BUTTON_CANCEL;
}

base::string16 ExternalProtocolDialog::GetDialogButtonLabel(
    ui::DialogButton button) const {
  if (button == ui::DIALOG_BUTTON_OK)
    return l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_OK_BUTTON_TEXT);
  else
    return l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_CANCEL_BUTTON_TEXT);
}

base::string16 ExternalProtocolDialog::GetWindowTitle() const {
  return l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_TITLE);
}

void ExternalProtocolDialog::DeleteDelegate() {
  delete this;
}

bool ExternalProtocolDialog::Cancel() {
  // We also get called back here if the user closes the dialog or presses
  // escape. In these cases it would be preferable to ignore the state of the
  // check box but MessageBox doesn't distinguish this from pressing the cancel
  // button.
  if (message_box_view_->IsCheckBoxSelected()) {
    ExternalProtocolHandler::SetBlockState(
        url_.scheme(), ExternalProtocolHandler::BLOCK);
  }

  // Returning true closes the dialog.
  return true;
}

bool ExternalProtocolDialog::Accept() {
  // We record how long it takes the user to accept an external protocol.  If
  // users start accepting these dialogs too quickly, we should worry about
  // clickjacking.
  UMA_HISTOGRAM_LONG_TIMES("clickjacking.launch_url",
                           base::TimeTicks::Now() - creation_time_);

  if (message_box_view_->IsCheckBoxSelected()) {
    ExternalProtocolHandler::SetBlockState(
        url_.scheme(), ExternalProtocolHandler::DONT_BLOCK);
  }

  ExternalProtocolHandler::LaunchUrlWithoutSecurityCheck(
      url_, render_process_host_id_, routing_id_);
  // Returning true closes the dialog.
  return true;
}

views::View* ExternalProtocolDialog::GetContentsView() {
  return message_box_view_;
}

views::Widget* ExternalProtocolDialog::GetWidget() {
  return message_box_view_->GetWidget();
}

const views::Widget* ExternalProtocolDialog::GetWidget() const {
  return message_box_view_->GetWidget();
}

///////////////////////////////////////////////////////////////////////////////
// ExternalProtocolDialog, private:

ExternalProtocolDialog::ExternalProtocolDialog(const GURL& url,
                                               int render_process_host_id,
                                               int routing_id,
                                               const std::wstring& command)
    : url_(url),
      render_process_host_id_(render_process_host_id),
      routing_id_(routing_id),
      creation_time_(base::TimeTicks::Now()) {
  const int kMaxUrlWithoutSchemeSize = 256;
  const int kMaxCommandSize = 256;
  base::string16 elided_url_without_scheme;
  base::string16 elided_command;
  gfx::ElideString(ASCIIToUTF16(url.possibly_invalid_spec()),
                  kMaxUrlWithoutSchemeSize, &elided_url_without_scheme);
  gfx::ElideString(WideToUTF16Hack(command), kMaxCommandSize, &elided_command);

  base::string16 message_text = l10n_util::GetStringFUTF16(
      IDS_EXTERNAL_PROTOCOL_INFORMATION,
      ASCIIToUTF16(url.scheme() + ":"),
      elided_url_without_scheme) + ASCIIToUTF16("\n\n");

  message_text += l10n_util::GetStringFUTF16(
      IDS_EXTERNAL_PROTOCOL_APPLICATION_TO_LAUNCH,
      elided_command) + ASCIIToUTF16("\n\n");

  message_text += l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_WARNING);

  views::MessageBoxView::InitParams params(message_text);
  params.message_width = kMessageWidth;
  message_box_view_ = new views::MessageBoxView(params);
  message_box_view_->SetCheckBoxLabel(
      l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_CHECKBOX_TEXT));

  // Dialog is top level if we don't have a web_contents associated with us.
  WebContents* web_contents = tab_util::GetWebContentsByID(
      render_process_host_id_, routing_id_);
  gfx::NativeWindow parent_window = NULL;
  if (web_contents)
    parent_window = web_contents->GetView()->GetTopLevelNativeWindow();
  CreateBrowserModalDialogViews(this, parent_window)->Show();
}

// static
std::wstring ExternalProtocolDialog::GetApplicationForProtocol(
    const GURL& url) {
  // We shouldn't be accessing the registry from the UI thread, since it can go
  // to disk.  http://crbug.com/61996
  base::ThreadRestrictions::ScopedAllowIO allow_io;

  std::wstring url_spec = ASCIIToWide(url.possibly_invalid_spec());
  std::wstring cmd_key_path =
      ASCIIToWide(url.scheme() + "\\shell\\open\\command");
  base::win::RegKey cmd_key(HKEY_CLASSES_ROOT, cmd_key_path.c_str(), KEY_READ);
  size_t split_offset = url_spec.find(L':');
  if (split_offset == std::wstring::npos)
    return std::wstring();
  std::wstring parameters = url_spec.substr(split_offset + 1,
                                            url_spec.length() - 1);
  std::wstring application_to_launch;
  if (cmd_key.ReadValue(NULL, &application_to_launch) == ERROR_SUCCESS) {
    ReplaceSubstringsAfterOffset(&application_to_launch, 0, L"%1", parameters);
    return application_to_launch;
  } else {
    return std::wstring();
  }
}
