// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file performs actions on media elements.
(function() {
  function seekMedia(selector, seekTime, logSeekTime, seekLabel) {
    // Performs the "Seek" action on media satisfying selector.
    var mediaElements = window.__findMediaElements(selector);
    for (var i = 0; i < mediaElements.length; i++) {
      if (mediaElements[i] instanceof HTMLMediaElement)
        seekHTML5Element(mediaElements[i], seekTime, logSeekTime, seekLabel);
      else
        throw new Error('Can not seek non HTML5 media elements.');
    }
  }

  function seekHTML5Element(element, seekTime, logSeekTime, seekLabel) {
    function readyForSeek() {
      seekHTML5ElementPostLoad(element, seekTime, logSeekTime, seekLabel);
    }
    if (element.readyState == element.HAVE_NOTHING) {
      var onLoadedMetaData = function(e) {
        element.removeEventListener('loadedmetadata', onLoadedMetaData);
        readyForSeek();
      };
      element.addEventListener('loadedmetadata', onLoadedMetaData);
      element.load();
    } else {
      readyForSeek();
    }
  }

  function seekHTML5ElementPostLoad(element, seekTime, logSeekTime, seekLabel) {
    var onSeeked = function(e) {
      element[e.type + '_completed'] = true;
      element.removeEventListener('seeked', onSeeked);
    };
    function onError(e) {
      throw new Error('Error playing media :' + e.type);
    }

    element['seeked_completed'] = false;
    element.addEventListener('error', onError);
    element.addEventListener('abort', onError);
    element.addEventListener('seeked', onSeeked);

    if (logSeekTime) {
      var willSeekEvent = document.createEvent('Event');
      willSeekEvent.initEvent('willSeek', false, false);
      if (seekLabel)
        willSeekEvent.seekLabel = seekLabel;
      else
        willSeekEvent.seekLabel = seekTime;
      element.dispatchEvent(willSeekEvent);
    }
    try {
      element.currentTime = seekTime;
    } catch (err) {
      throw new Error('Cannot seek in network state: ' + element.networkState);
    }
  }

  window.__seekMedia = seekMedia;
})();
