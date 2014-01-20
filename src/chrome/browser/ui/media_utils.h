// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_MEDIA_UTILS_H_
#define CHROME_BROWSER_UI_MEDIA_UTILS_H_

#include "content/public/common/media_stream_request.h"

class Profile;

namespace content {
class WebContents;
}

void RequestMediaAccessPermission(
    content::WebContents* web_contents,
    Profile* profile,
    const content::MediaStreamRequest& request,
    const content::MediaResponseCallback& callback);

#endif  // CHROME_BROWSER_UI_MEDIA_UTILS_H_
