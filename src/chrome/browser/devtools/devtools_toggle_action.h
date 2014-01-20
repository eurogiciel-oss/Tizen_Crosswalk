// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_DEVTOOLS_TOGGLE_ACTION_H_
#define CHROME_BROWSER_DEVTOOLS_DEVTOOLS_TOGGLE_ACTION_H_

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string16.h"

struct DevToolsToggleAction {
 public:
  enum Type {
    kShow,
    kShowConsole,
    kInspect,
    kToggle,
    kReveal
  };

  struct RevealParams {
    RevealParams(const base::string16& url,
                 size_t line_number,
                 size_t column_number);
    ~RevealParams();

    base::string16 url;
    size_t line_number;
    size_t column_number;
  };

  void operator=(const DevToolsToggleAction& rhs);
  DevToolsToggleAction(const DevToolsToggleAction& rhs);
  ~DevToolsToggleAction();

  static DevToolsToggleAction Show();
  static DevToolsToggleAction ShowConsole();
  static DevToolsToggleAction Inspect();
  static DevToolsToggleAction Toggle();
  static DevToolsToggleAction Reveal(const base::string16& url,
                                     size_t line_number,
                                     size_t column_number);

  Type type() const { return type_; }
  const RevealParams* params() const { return params_.get(); }

 private:
  explicit DevToolsToggleAction(Type type);
  explicit DevToolsToggleAction(RevealParams* reveal_params);

  // The type of action.
  Type type_;

  // Additional parameters for the Reveal action; NULL if of any other type.
  scoped_ptr<RevealParams> params_;
};

#endif  // CHROME_BROWSER_DEVTOOLS_DEVTOOLS_TOGGLE_ACTION_H_
