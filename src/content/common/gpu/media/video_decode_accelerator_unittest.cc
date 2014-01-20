// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The bulk of this file is support code; sorry about that.  Here's an overview
// to hopefully help readers of this code:
// - RenderingHelper is charged with interacting with X11/{EGL/GLES2,GLX/GL} or
//   Win/EGL.
// - ClientState is an enum for the state of the decode client used by the test.
// - ClientStateNotification is a barrier abstraction that allows the test code
//   to be written sequentially and wait for the decode client to see certain
//   state transitions.
// - GLRenderingVDAClient is a VideoDecodeAccelerator::Client implementation
// - Finally actual TEST cases are at the bottom of this file, using the above
//   infrastructure.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <deque>

// Include gtest.h out of order because <X11/X.h> #define's Bool & None, which
// gtest uses as struct names (inside a namespace).  This means that
// #include'ing gtest after anything that pulls in X.h fails to compile.
// This is http://code.google.com/p/googletest/issues/detail?id=371
#include "testing/gtest/include/gtest/gtest.h"

#include "base/at_exit.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/file_util.h"
#include "base/format_macros.h"
#include "base/md5.h"
#include "base/message_loop/message_loop_proxy.h"
#include "base/platform_file.h"
#include "base/process/process.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringize_macros.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "content/common/gpu/media/rendering_helper.h"
#include "content/common/gpu/media/video_accelerator_unittest_helpers.h"
#include "content/public/common/content_switches.h"
#include "ui/gfx/codec/png_codec.h"

#if defined(OS_WIN)
#include "content/common/gpu/media/dxva_video_decode_accelerator.h"
#elif defined(OS_CHROMEOS)
#if defined(ARCH_CPU_ARMEL)
#include "content/common/gpu/media/exynos_video_decode_accelerator.h"
#elif defined(ARCH_CPU_X86_FAMILY)
#include "content/common/gpu/media/vaapi_video_decode_accelerator.h"
#include "content/common/gpu/media/vaapi_wrapper.h"
#endif  // ARCH_CPU_ARMEL
#else
#error The VideoAccelerator tests are not supported on this platform.
#endif  // OS_WIN

using media::VideoDecodeAccelerator;

namespace content {
namespace {

// Values optionally filled in from flags; see main() below.
// The syntax of multiple test videos is:
//  test-video1;test-video2;test-video3
// where only the first video is required and other optional videos would be
// decoded by concurrent decoders.
// The syntax of each test-video is:
//  filename:width:height:numframes:numfragments:minFPSwithRender:minFPSnoRender
// where only the first field is required.  Value details:
// - |filename| must be an h264 Annex B (NAL) stream or an IVF VP8 stream.
// - |width| and |height| are in pixels.
// - |numframes| is the number of picture frames in the file.
// - |numfragments| NALU (h264) or frame (VP8) count in the stream.
// - |minFPSwithRender| and |minFPSnoRender| are minimum frames/second speeds
//   expected to be achieved with and without rendering to the screen, resp.
//   (the latter tests just decode speed).
// - |profile| is the media::VideoCodecProfile set during Initialization.
// An empty value for a numeric field means "ignore".
const base::FilePath::CharType* g_test_video_data =
    // FILE_PATH_LITERAL("test-25fps.vp8:320:240:250:250:50:175:11");
    FILE_PATH_LITERAL("test-25fps.h264:320:240:250:258:50:175:1");

// The path of the frame delivery time log. We can enable the log and specify
// the filename by the "--frame_delivery_log" switch.
const base::FilePath::CharType* g_frame_delivery_log = NULL;

// The value is set by the switch "--rendering_fps".
double g_rendering_fps = 0;

// Disable rendering, the value is set by the switch "--disable_rendering".
bool g_disable_rendering = false;

// Magic constants for differentiating the reasons for NotifyResetDone being
// called.
enum ResetPoint {
  START_OF_STREAM_RESET = -3,
  MID_STREAM_RESET = -2,
  END_OF_STREAM_RESET = -1
};

const int kMaxResetAfterFrameNum = 100;
const int kMaxFramesToDelayReuse = 64;
const base::TimeDelta kReuseDelay = base::TimeDelta::FromSeconds(1);

struct TestVideoFile {
  explicit TestVideoFile(base::FilePath::StringType file_name)
      : file_name(file_name),
        width(-1),
        height(-1),
        num_frames(-1),
        num_fragments(-1),
        min_fps_render(-1),
        min_fps_no_render(-1),
        profile(-1),
        reset_after_frame_num(END_OF_STREAM_RESET) {
  }

  base::FilePath::StringType file_name;
  int width;
  int height;
  int num_frames;
  int num_fragments;
  int min_fps_render;
  int min_fps_no_render;
  int profile;
  int reset_after_frame_num;
  std::string data_str;
};

// Presumed minimal display size.
// We subtract one pixel from the width because some ARM chromebooks do not
// support two fullscreen app running at the same time. See crbug.com/270064.
const gfx::Size kThumbnailsDisplaySize(1366 - 1, 768);
const gfx::Size kThumbnailsPageSize(1600, 1200);
const gfx::Size kThumbnailSize(160, 120);
const int kMD5StringLength = 32;

// Parse |data| into its constituent parts, set the various output fields
// accordingly, and read in video stream. CHECK-fails on unexpected or
// missing required data. Unspecified optional fields are set to -1.
void ParseAndReadTestVideoData(base::FilePath::StringType data,
                               size_t num_concurrent_decoders,
                               int reset_point,
                               std::vector<TestVideoFile*>* test_video_files) {
  std::vector<base::FilePath::StringType> entries;
  base::SplitString(data, ';', &entries);
  CHECK_GE(entries.size(), 1U) << data;
  for (size_t index = 0; index < entries.size(); ++index) {
    std::vector<base::FilePath::StringType> fields;
    base::SplitString(entries[index], ':', &fields);
    CHECK_GE(fields.size(), 1U) << entries[index];
    CHECK_LE(fields.size(), 8U) << entries[index];
    TestVideoFile* video_file = new TestVideoFile(fields[0]);
    if (!fields[1].empty())
      CHECK(base::StringToInt(fields[1], &video_file->width));
    if (!fields[2].empty())
      CHECK(base::StringToInt(fields[2], &video_file->height));
    if (!fields[3].empty()) {
      CHECK(base::StringToInt(fields[3], &video_file->num_frames));
      // If we reset mid-stream and start playback over, account for frames
      // that are decoded twice in our expectations.
      if (video_file->num_frames > 0 && reset_point == MID_STREAM_RESET) {
        // Reset should not go beyond the last frame; reset after the first
        // frame for short videos.
        video_file->reset_after_frame_num = kMaxResetAfterFrameNum;
        if (video_file->num_frames <= kMaxResetAfterFrameNum)
          video_file->reset_after_frame_num = 1;
        video_file->num_frames += video_file->reset_after_frame_num;
      } else {
        video_file->reset_after_frame_num = reset_point;
      }
    }
    if (!fields[4].empty())
      CHECK(base::StringToInt(fields[4], &video_file->num_fragments));
    if (!fields[5].empty()) {
      CHECK(base::StringToInt(fields[5], &video_file->min_fps_render));
      video_file->min_fps_render /= num_concurrent_decoders;
    }
    if (!fields[6].empty()) {
      CHECK(base::StringToInt(fields[6], &video_file->min_fps_no_render));
      video_file->min_fps_no_render /= num_concurrent_decoders;
    }
    if (!fields[7].empty())
      CHECK(base::StringToInt(fields[7], &video_file->profile));

    // Read in the video data.
    base::FilePath filepath(video_file->file_name);
    CHECK(base::ReadFileToString(filepath, &video_file->data_str))
        << "test_video_file: " << filepath.MaybeAsASCII();

    test_video_files->push_back(video_file);
  }
}

// Read in golden MD5s for the thumbnailed rendering of this video
void ReadGoldenThumbnailMD5s(const TestVideoFile* video_file,
                             std::vector<std::string>* md5_strings) {
  base::FilePath filepath(video_file->file_name);
  filepath = filepath.AddExtension(FILE_PATH_LITERAL(".md5"));
  std::string all_md5s;
  base::ReadFileToString(filepath, &all_md5s);
  base::SplitString(all_md5s, '\n', md5_strings);
  // Check these are legitimate MD5s.
  for (std::vector<std::string>::iterator md5_string = md5_strings->begin();
      md5_string != md5_strings->end(); ++md5_string) {
      // Ignore the empty string added by SplitString
      if (!md5_string->length())
        continue;

      CHECK_EQ(static_cast<int>(md5_string->length()),
               kMD5StringLength) << *md5_string;
      bool hex_only = std::count_if(md5_string->begin(),
                                    md5_string->end(), isxdigit) ==
                                    kMD5StringLength;
      CHECK(hex_only) << *md5_string;
  }
  CHECK_GE(md5_strings->size(), 1U) << all_md5s;
}

// State of the GLRenderingVDAClient below.  Order matters here as the test
// makes assumptions about it.
enum ClientState {
  CS_CREATED = 0,
  CS_DECODER_SET = 1,
  CS_INITIALIZED = 2,
  CS_FLUSHING = 3,
  CS_FLUSHED = 4,
  CS_RESETTING = 5,
  CS_RESET = 6,
  CS_ERROR = 7,
  CS_DESTROYED = 8,
  CS_MAX,  // Must be last entry.
};

// A wrapper client that throttles the PictureReady callbacks to a given rate.
// It may drops or queues frame to deliver them on time.
class ThrottlingVDAClient : public VideoDecodeAccelerator::Client,
                            public base::SupportsWeakPtr<ThrottlingVDAClient> {
 public:
  // Callback invoked whan the picture is dropped and should be reused for
  // the decoder again.
  typedef base::Callback<void(int32 picture_buffer_id)> ReusePictureCB;

  ThrottlingVDAClient(VideoDecodeAccelerator::Client* client,
                      double fps,
                      ReusePictureCB reuse_picture_cb);
  virtual ~ThrottlingVDAClient();

  // VideoDecodeAccelerator::Client implementation
  virtual void ProvidePictureBuffers(uint32 requested_num_of_buffers,
                                     const gfx::Size& dimensions,
                                     uint32 texture_target) OVERRIDE;
  virtual void DismissPictureBuffer(int32 picture_buffer_id) OVERRIDE;
  virtual void PictureReady(const media::Picture& picture) OVERRIDE;
  virtual void NotifyInitializeDone() OVERRIDE;
  virtual void NotifyEndOfBitstreamBuffer(int32 bitstream_buffer_id) OVERRIDE;
  virtual void NotifyFlushDone() OVERRIDE;
  virtual void NotifyResetDone() OVERRIDE;
  virtual void NotifyError(VideoDecodeAccelerator::Error error) OVERRIDE;

  int num_decoded_frames() { return num_decoded_frames_; }

 private:

  void CallClientPictureReady(int version);

  VideoDecodeAccelerator::Client* client_;
  ReusePictureCB reuse_picture_cb_;
  base::TimeTicks next_frame_delivered_time_;
  base::TimeDelta frame_duration_;

  int num_decoded_frames_;
  int stream_version_;
  std::deque<media::Picture> pending_pictures_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(ThrottlingVDAClient);
};

ThrottlingVDAClient::ThrottlingVDAClient(VideoDecodeAccelerator::Client* client,
                                         double fps,
                                         ReusePictureCB reuse_picture_cb)
    : client_(client),
      reuse_picture_cb_(reuse_picture_cb),
      num_decoded_frames_(0),
      stream_version_(0) {
  CHECK(client_);
  CHECK_GT(fps, 0);
  frame_duration_ = base::TimeDelta::FromSeconds(1) / fps;
}

ThrottlingVDAClient::~ThrottlingVDAClient() {}

void ThrottlingVDAClient::ProvidePictureBuffers(uint32 requested_num_of_buffers,
                                                const gfx::Size& dimensions,
                                                uint32 texture_target) {
  client_->ProvidePictureBuffers(
      requested_num_of_buffers, dimensions, texture_target);
}

void ThrottlingVDAClient::DismissPictureBuffer(int32 picture_buffer_id) {
  client_->DismissPictureBuffer(picture_buffer_id);
}

void ThrottlingVDAClient::PictureReady(const media::Picture& picture) {
  ++num_decoded_frames_;

  if (pending_pictures_.empty()) {
    base::TimeDelta delay =
        next_frame_delivered_time_.is_null()
            ? base::TimeDelta()
            : next_frame_delivered_time_ - base::TimeTicks::Now();
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ThrottlingVDAClient::CallClientPictureReady,
                   AsWeakPtr(),
                   stream_version_),
        delay);
  }
  pending_pictures_.push_back(picture);
}

void ThrottlingVDAClient::CallClientPictureReady(int version) {
  // Just return if we have reset the decoder
  if (version != stream_version_)
    return;

  base::TimeTicks now = base::TimeTicks::Now();

  if (next_frame_delivered_time_.is_null())
    next_frame_delivered_time_ = now;

  if (next_frame_delivered_time_ + frame_duration_ < now) {
    // Too late, drop the frame
    reuse_picture_cb_.Run(pending_pictures_.front().picture_buffer_id());
  } else {
    client_->PictureReady(pending_pictures_.front());
  }

  pending_pictures_.pop_front();
  next_frame_delivered_time_ += frame_duration_;
  if (!pending_pictures_.empty()) {
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ThrottlingVDAClient::CallClientPictureReady,
                   AsWeakPtr(),
                   stream_version_),
        next_frame_delivered_time_ - base::TimeTicks::Now());
  }
}

void ThrottlingVDAClient::NotifyInitializeDone() {
  client_->NotifyInitializeDone();
}

void ThrottlingVDAClient::NotifyEndOfBitstreamBuffer(
    int32 bitstream_buffer_id) {
  client_->NotifyEndOfBitstreamBuffer(bitstream_buffer_id);
}

void ThrottlingVDAClient::NotifyFlushDone() {
  if (!pending_pictures_.empty()) {
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ThrottlingVDAClient::NotifyFlushDone,
                   base::Unretained(this)),
        next_frame_delivered_time_ - base::TimeTicks::Now());
    return;
  }
  client_->NotifyFlushDone();
}

void ThrottlingVDAClient::NotifyResetDone() {
  ++stream_version_;
  while (!pending_pictures_.empty()) {
    reuse_picture_cb_.Run(pending_pictures_.front().picture_buffer_id());
    pending_pictures_.pop_front();
  }
  next_frame_delivered_time_ = base::TimeTicks();
  client_->NotifyResetDone();
}

void ThrottlingVDAClient::NotifyError(VideoDecodeAccelerator::Error error) {
  client_->NotifyError(error);
}

// Client that can accept callbacks from a VideoDecodeAccelerator and is used by
// the TESTs below.
class GLRenderingVDAClient
    : public VideoDecodeAccelerator::Client,
      public base::SupportsWeakPtr<GLRenderingVDAClient> {
 public:
  // Doesn't take ownership of |rendering_helper| or |note|, which must outlive
  // |*this|.
  // |num_play_throughs| indicates how many times to play through the video.
  // |reset_after_frame_num| can be a frame number >=0 indicating a mid-stream
  // Reset() should be done after that frame number is delivered, or
  // END_OF_STREAM_RESET to indicate no mid-stream Reset().
  // |delete_decoder_state| indicates when the underlying decoder should be
  // Destroy()'d and deleted and can take values: N<0: delete after -N Decode()
  // calls have been made, N>=0 means interpret as ClientState.
  // Both |reset_after_frame_num| & |delete_decoder_state| apply only to the
  // last play-through (governed by |num_play_throughs|).
  // |rendering_fps| indicates the target rendering fps. 0 means no target fps
  // and it would render as fast as possible.
  // |suppress_rendering| indicates GL rendering is suppressed or not.
  // After |delay_reuse_after_frame_num| frame has been delivered, the client
  // will start delaying the call to ReusePictureBuffer() for kReuseDelay.
  GLRenderingVDAClient(RenderingHelper* rendering_helper,
                       int rendering_window_id,
                       ClientStateNotification<ClientState>* note,
                       const std::string& encoded_data,
                       int num_in_flight_decodes,
                       int num_play_throughs,
                       int reset_after_frame_num,
                       int delete_decoder_state,
                       int frame_width,
                       int frame_height,
                       int profile,
                       double rendering_fps,
                       bool suppress_rendering,
                       int delay_reuse_after_frame_num);
  virtual ~GLRenderingVDAClient();
  void CreateDecoder();

  // VideoDecodeAccelerator::Client implementation.
  // The heart of the Client.
  virtual void ProvidePictureBuffers(uint32 requested_num_of_buffers,
                                     const gfx::Size& dimensions,
                                     uint32 texture_target) OVERRIDE;
  virtual void DismissPictureBuffer(int32 picture_buffer_id) OVERRIDE;
  virtual void PictureReady(const media::Picture& picture) OVERRIDE;
  // Simple state changes.
  virtual void NotifyInitializeDone() OVERRIDE;
  virtual void NotifyEndOfBitstreamBuffer(int32 bitstream_buffer_id) OVERRIDE;
  virtual void NotifyFlushDone() OVERRIDE;
  virtual void NotifyResetDone() OVERRIDE;
  virtual void NotifyError(VideoDecodeAccelerator::Error error) OVERRIDE;

  void OutputFrameDeliveryTimes(base::PlatformFile output);

  void NotifyFrameDropped(int32 picture_buffer_id);

  // Simple getters for inspecting the state of the Client.
  int num_done_bitstream_buffers() { return num_done_bitstream_buffers_; }
  int num_skipped_fragments() { return num_skipped_fragments_; }
  int num_queued_fragments() { return num_queued_fragments_; }
  int num_decoded_frames();
  double frames_per_second();
  bool decoder_deleted() { return !decoder_.get(); }

 private:
  typedef std::map<int, media::PictureBuffer*> PictureBufferById;

  void SetState(ClientState new_state);

  // Delete the associated decoder helper.
  void DeleteDecoder();

  // Compute & return the first encoded bytes (including a start frame) to send
  // to the decoder, starting at |start_pos| and returning one fragment. Skips
  // to the first decodable position.
  std::string GetBytesForFirstFragment(size_t start_pos, size_t* end_pos);
  // Compute & return the encoded bytes of next fragment to send to the decoder
  // (based on |start_pos|).
  std::string GetBytesForNextFragment(size_t start_pos, size_t* end_pos);
  // Helpers for GetBytesForNextFragment above.
  void GetBytesForNextNALU(size_t start_pos, size_t* end_pos);  // For h.264.
  std::string GetBytesForNextFrame(
      size_t start_pos, size_t* end_pos);  // For VP8.

  // Request decode of the next fragment in the encoded data.
  void DecodeNextFragment();

  RenderingHelper* rendering_helper_;
  int rendering_window_id_;
  std::string encoded_data_;
  const int num_in_flight_decodes_;
  int outstanding_decodes_;
  size_t encoded_data_next_pos_to_decode_;
  int next_bitstream_buffer_id_;
  ClientStateNotification<ClientState>* note_;
  scoped_ptr<VideoDecodeAccelerator> decoder_;
  std::set<int> outstanding_texture_ids_;
  int remaining_play_throughs_;
  int reset_after_frame_num_;
  int delete_decoder_state_;
  ClientState state_;
  int num_skipped_fragments_;
  int num_queued_fragments_;
  int num_decoded_frames_;
  int num_done_bitstream_buffers_;
  PictureBufferById picture_buffers_by_id_;
  base::TimeTicks initialize_done_ticks_;
  int profile_;
  bool suppress_rendering_;
  std::vector<base::TimeTicks> frame_delivery_times_;
  int delay_reuse_after_frame_num_;
  scoped_ptr<ThrottlingVDAClient> throttling_client_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(GLRenderingVDAClient);
};

GLRenderingVDAClient::GLRenderingVDAClient(
    RenderingHelper* rendering_helper,
    int rendering_window_id,
    ClientStateNotification<ClientState>* note,
    const std::string& encoded_data,
    int num_in_flight_decodes,
    int num_play_throughs,
    int reset_after_frame_num,
    int delete_decoder_state,
    int frame_width,
    int frame_height,
    int profile,
    double rendering_fps,
    bool suppress_rendering,
    int delay_reuse_after_frame_num)
    : rendering_helper_(rendering_helper),
      rendering_window_id_(rendering_window_id),
      encoded_data_(encoded_data),
      num_in_flight_decodes_(num_in_flight_decodes),
      outstanding_decodes_(0),
      encoded_data_next_pos_to_decode_(0),
      next_bitstream_buffer_id_(0),
      note_(note),
      remaining_play_throughs_(num_play_throughs),
      reset_after_frame_num_(reset_after_frame_num),
      delete_decoder_state_(delete_decoder_state),
      state_(CS_CREATED),
      num_skipped_fragments_(0),
      num_queued_fragments_(0),
      num_decoded_frames_(0),
      num_done_bitstream_buffers_(0),
      profile_(profile),
      suppress_rendering_(suppress_rendering),
      delay_reuse_after_frame_num_(delay_reuse_after_frame_num) {
  CHECK_GT(num_in_flight_decodes, 0);
  CHECK_GT(num_play_throughs, 0);
  CHECK_GE(rendering_fps, 0);
  if (rendering_fps > 0)
    throttling_client_.reset(new ThrottlingVDAClient(
        this,
        rendering_fps,
        base::Bind(&GLRenderingVDAClient::NotifyFrameDropped,
                   base::Unretained(this))));
}

GLRenderingVDAClient::~GLRenderingVDAClient() {
  DeleteDecoder();  // Clean up in case of expected error.
  CHECK(decoder_deleted());
  STLDeleteValues(&picture_buffers_by_id_);
  SetState(CS_DESTROYED);
}

static bool DoNothingReturnTrue() { return true; }

void GLRenderingVDAClient::CreateDecoder() {
  CHECK(decoder_deleted());
  CHECK(!decoder_.get());

  VideoDecodeAccelerator::Client* client = this;
  base::WeakPtr<VideoDecodeAccelerator::Client> weak_client = AsWeakPtr();
  if (throttling_client_) {
    client = throttling_client_.get();
    weak_client = throttling_client_->AsWeakPtr();
  }
#if defined(OS_WIN)
  decoder_.reset(
      new DXVAVideoDecodeAccelerator(client, base::Bind(&DoNothingReturnTrue)));
#elif defined(OS_CHROMEOS)
#if defined(ARCH_CPU_ARMEL)
  decoder_.reset(new ExynosVideoDecodeAccelerator(
      static_cast<EGLDisplay>(rendering_helper_->GetGLDisplay()),
      static_cast<EGLContext>(rendering_helper_->GetGLContext()),
      client,
      weak_client,
      base::Bind(&DoNothingReturnTrue),
      base::MessageLoopProxy::current()));
#elif defined(ARCH_CPU_X86_FAMILY)
  decoder_.reset(new VaapiVideoDecodeAccelerator(
      static_cast<Display*>(rendering_helper_->GetGLDisplay()),
      static_cast<GLXContext>(rendering_helper_->GetGLContext()),
      client,
      base::Bind(&DoNothingReturnTrue)));
#endif  // ARCH_CPU_ARMEL
#endif  // OS_WIN
  CHECK(decoder_.get());
  SetState(CS_DECODER_SET);
  if (decoder_deleted())
    return;

  // Configure the decoder.
  media::VideoCodecProfile profile = media::H264PROFILE_BASELINE;
  if (profile_ != -1)
    profile = static_cast<media::VideoCodecProfile>(profile_);
  CHECK(decoder_->Initialize(profile));
}

void GLRenderingVDAClient::ProvidePictureBuffers(
    uint32 requested_num_of_buffers,
    const gfx::Size& dimensions,
    uint32 texture_target) {
  if (decoder_deleted())
    return;
  std::vector<media::PictureBuffer> buffers;

  for (uint32 i = 0; i < requested_num_of_buffers; ++i) {
    uint32 id = picture_buffers_by_id_.size();
    uint32 texture_id;
    base::WaitableEvent done(false, false);
    rendering_helper_->CreateTexture(
        rendering_window_id_, texture_target, &texture_id, &done);
    done.Wait();
    CHECK(outstanding_texture_ids_.insert(texture_id).second);
    media::PictureBuffer* buffer =
        new media::PictureBuffer(id, dimensions, texture_id);
    CHECK(picture_buffers_by_id_.insert(std::make_pair(id, buffer)).second);
    buffers.push_back(*buffer);
  }
  decoder_->AssignPictureBuffers(buffers);
}

void GLRenderingVDAClient::DismissPictureBuffer(int32 picture_buffer_id) {
  PictureBufferById::iterator it =
      picture_buffers_by_id_.find(picture_buffer_id);
  CHECK(it != picture_buffers_by_id_.end());
  CHECK_EQ(outstanding_texture_ids_.erase(it->second->texture_id()), 1U);
  rendering_helper_->DeleteTexture(it->second->texture_id());
  delete it->second;
  picture_buffers_by_id_.erase(it);
}

void GLRenderingVDAClient::PictureReady(const media::Picture& picture) {
  // We shouldn't be getting pictures delivered after Reset has completed.
  CHECK_LT(state_, CS_RESET);

  if (decoder_deleted())
    return;

  frame_delivery_times_.push_back(base::TimeTicks::Now());

  CHECK_LE(picture.bitstream_buffer_id(), next_bitstream_buffer_id_);
  ++num_decoded_frames_;

  // Mid-stream reset applies only to the last play-through per constructor
  // comment.
  if (remaining_play_throughs_ == 1 &&
      reset_after_frame_num_ == num_decoded_frames()) {
    reset_after_frame_num_ = MID_STREAM_RESET;
    decoder_->Reset();
    // Re-start decoding from the beginning of the stream to avoid needing to
    // know how to find I-frames and so on in this test.
    encoded_data_next_pos_to_decode_ = 0;
  }

  media::PictureBuffer* picture_buffer =
      picture_buffers_by_id_[picture.picture_buffer_id()];
  CHECK(picture_buffer);
  if (!suppress_rendering_) {
    rendering_helper_->RenderTexture(picture_buffer->texture_id());
  }

  if (num_decoded_frames() > delay_reuse_after_frame_num_) {
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&VideoDecodeAccelerator::ReusePictureBuffer,
                   decoder_->AsWeakPtr(),
                   picture.picture_buffer_id()),
        kReuseDelay);
  } else {
    decoder_->ReusePictureBuffer(picture.picture_buffer_id());
  }
}

void GLRenderingVDAClient::NotifyInitializeDone() {
  SetState(CS_INITIALIZED);
  initialize_done_ticks_ = base::TimeTicks::Now();

  if (reset_after_frame_num_ == START_OF_STREAM_RESET) {
    decoder_->Reset();
    return;
  }

  for (int i = 0; i < num_in_flight_decodes_; ++i)
    DecodeNextFragment();
  DCHECK_EQ(outstanding_decodes_, num_in_flight_decodes_);
}

void GLRenderingVDAClient::NotifyEndOfBitstreamBuffer(
    int32 bitstream_buffer_id) {
  // TODO(fischman): this test currently relies on this notification to make
  // forward progress during a Reset().  But the VDA::Reset() API doesn't
  // guarantee this, so stop relying on it (and remove the notifications from
  // VaapiVideoDecodeAccelerator::FinishReset()).
  ++num_done_bitstream_buffers_;
  --outstanding_decodes_;
  DecodeNextFragment();
}

void GLRenderingVDAClient::NotifyFlushDone() {
  if (decoder_deleted())
    return;
  SetState(CS_FLUSHED);
  --remaining_play_throughs_;
  DCHECK_GE(remaining_play_throughs_, 0);
  if (decoder_deleted())
    return;
  decoder_->Reset();
  SetState(CS_RESETTING);
}

void GLRenderingVDAClient::NotifyResetDone() {
  if (decoder_deleted())
    return;

  if (reset_after_frame_num_ == MID_STREAM_RESET) {
    reset_after_frame_num_ = END_OF_STREAM_RESET;
    DecodeNextFragment();
    return;
  } else if (reset_after_frame_num_ == START_OF_STREAM_RESET) {
    reset_after_frame_num_ = END_OF_STREAM_RESET;
    for (int i = 0; i < num_in_flight_decodes_; ++i)
      DecodeNextFragment();
    return;
  }

  if (remaining_play_throughs_) {
    encoded_data_next_pos_to_decode_ = 0;
    NotifyInitializeDone();
    return;
  }

  SetState(CS_RESET);
  if (!decoder_deleted())
    DeleteDecoder();
}

void GLRenderingVDAClient::NotifyError(VideoDecodeAccelerator::Error error) {
  SetState(CS_ERROR);
}

void GLRenderingVDAClient::OutputFrameDeliveryTimes(base::PlatformFile output) {
  std::string s = base::StringPrintf("frame count: %" PRIuS "\n",
                                     frame_delivery_times_.size());
  base::WritePlatformFileAtCurrentPos(output, s.data(), s.length());
  base::TimeTicks t0 = initialize_done_ticks_;
  for (size_t i = 0; i < frame_delivery_times_.size(); ++i) {
    s = base::StringPrintf("frame %04" PRIuS ": %" PRId64 " us\n",
                           i,
                           (frame_delivery_times_[i] - t0).InMicroseconds());
    t0 = frame_delivery_times_[i];
    base::WritePlatformFileAtCurrentPos(output, s.data(), s.length());
  }
}

void GLRenderingVDAClient::NotifyFrameDropped(int32 picture_buffer_id) {
  decoder_->ReusePictureBuffer(picture_buffer_id);
}

static bool LookingAtNAL(const std::string& encoded, size_t pos) {
  return encoded[pos] == 0 && encoded[pos + 1] == 0 &&
      encoded[pos + 2] == 0 && encoded[pos + 3] == 1;
}

void GLRenderingVDAClient::SetState(ClientState new_state) {
  note_->Notify(new_state);
  state_ = new_state;
  if (!remaining_play_throughs_ && new_state == delete_decoder_state_) {
    CHECK(!decoder_deleted());
    DeleteDecoder();
  }
}

void GLRenderingVDAClient::DeleteDecoder() {
  if (decoder_deleted())
    return;
  decoder_.release()->Destroy();
  STLClearObject(&encoded_data_);
  for (std::set<int>::iterator it = outstanding_texture_ids_.begin();
       it != outstanding_texture_ids_.end(); ++it) {
    rendering_helper_->DeleteTexture(*it);
  }
  outstanding_texture_ids_.clear();
  // Cascade through the rest of the states to simplify test code below.
  for (int i = state_ + 1; i < CS_MAX; ++i)
    SetState(static_cast<ClientState>(i));
}

std::string GLRenderingVDAClient::GetBytesForFirstFragment(
    size_t start_pos, size_t* end_pos) {
  if (profile_ < media::H264PROFILE_MAX) {
    *end_pos = start_pos;
    while (*end_pos + 4 < encoded_data_.size()) {
      if ((encoded_data_[*end_pos + 4] & 0x1f) == 0x7) // SPS start frame
        return GetBytesForNextFragment(*end_pos, end_pos);
      GetBytesForNextNALU(*end_pos, end_pos);
      num_skipped_fragments_++;
    }
    *end_pos = start_pos;
    return std::string();
  }
  DCHECK_LE(profile_, media::VP8PROFILE_MAX);
  return GetBytesForNextFragment(start_pos, end_pos);
}

std::string GLRenderingVDAClient::GetBytesForNextFragment(
    size_t start_pos, size_t* end_pos) {
  if (profile_ < media::H264PROFILE_MAX) {
    *end_pos = start_pos;
    GetBytesForNextNALU(*end_pos, end_pos);
    if (start_pos != *end_pos) {
      num_queued_fragments_++;
    }
    return encoded_data_.substr(start_pos, *end_pos - start_pos);
  }
  DCHECK_LE(profile_, media::VP8PROFILE_MAX);
  return GetBytesForNextFrame(start_pos, end_pos);
}

void GLRenderingVDAClient::GetBytesForNextNALU(
    size_t start_pos, size_t* end_pos) {
  *end_pos = start_pos;
  if (*end_pos + 4 > encoded_data_.size())
    return;
  CHECK(LookingAtNAL(encoded_data_, start_pos));
  *end_pos += 4;
  while (*end_pos + 4 <= encoded_data_.size() &&
         !LookingAtNAL(encoded_data_, *end_pos)) {
    ++*end_pos;
  }
  if (*end_pos + 3 >= encoded_data_.size())
    *end_pos = encoded_data_.size();
}

std::string GLRenderingVDAClient::GetBytesForNextFrame(
    size_t start_pos, size_t* end_pos) {
  // Helpful description: http://wiki.multimedia.cx/index.php?title=IVF
  std::string bytes;
  if (start_pos == 0)
    start_pos = 32;  // Skip IVF header.
  *end_pos = start_pos;
  uint32 frame_size = *reinterpret_cast<uint32*>(&encoded_data_[*end_pos]);
  *end_pos += 12;  // Skip frame header.
  bytes.append(encoded_data_.substr(*end_pos, frame_size));
  *end_pos += frame_size;
  num_queued_fragments_++;
  return bytes;
}

void GLRenderingVDAClient::DecodeNextFragment() {
  if (decoder_deleted())
    return;
  if (encoded_data_next_pos_to_decode_ == encoded_data_.size()) {
    if (outstanding_decodes_ == 0) {
      decoder_->Flush();
      SetState(CS_FLUSHING);
    }
    return;
  }
  size_t end_pos;
  std::string next_fragment_bytes;
  if (encoded_data_next_pos_to_decode_ == 0) {
    next_fragment_bytes = GetBytesForFirstFragment(0, &end_pos);
  } else {
    next_fragment_bytes =
        GetBytesForNextFragment(encoded_data_next_pos_to_decode_, &end_pos);
  }
  size_t next_fragment_size = next_fragment_bytes.size();

  // Populate the shared memory buffer w/ the fragment, duplicate its handle,
  // and hand it off to the decoder.
  base::SharedMemory shm;
  CHECK(shm.CreateAndMapAnonymous(next_fragment_size));
  memcpy(shm.memory(), next_fragment_bytes.data(), next_fragment_size);
  base::SharedMemoryHandle dup_handle;
  CHECK(shm.ShareToProcess(base::Process::Current().handle(), &dup_handle));
  media::BitstreamBuffer bitstream_buffer(
      next_bitstream_buffer_id_, dup_handle, next_fragment_size);
  // Mask against 30 bits, to avoid (undefined) wraparound on signed integer.
  next_bitstream_buffer_id_ = (next_bitstream_buffer_id_ + 1) & 0x3FFFFFFF;
  decoder_->Decode(bitstream_buffer);
  ++outstanding_decodes_;
  encoded_data_next_pos_to_decode_ = end_pos;

  if (!remaining_play_throughs_ &&
      -delete_decoder_state_ == next_bitstream_buffer_id_) {
    DeleteDecoder();
  }
}

int GLRenderingVDAClient::num_decoded_frames() {
  return throttling_client_ ? throttling_client_->num_decoded_frames()
                            : num_decoded_frames_;
}

double GLRenderingVDAClient::frames_per_second() {
  base::TimeDelta delta = frame_delivery_times_.back() - initialize_done_ticks_;
  if (delta.InSecondsF() == 0)
    return 0;
  return num_decoded_frames() / delta.InSecondsF();
}

// Test parameters:
// - Number of concurrent decoders.
// - Number of concurrent in-flight Decode() calls per decoder.
// - Number of play-throughs.
// - reset_after_frame_num: see GLRenderingVDAClient ctor.
// - delete_decoder_phase: see GLRenderingVDAClient ctor.
// - whether to test slow rendering by delaying ReusePictureBuffer().
// - whether the video frames are rendered as thumbnails.
class VideoDecodeAcceleratorTest
    : public ::testing::TestWithParam<
  Tuple7<int, int, int, ResetPoint, ClientState, bool, bool> > {
};

// Helper so that gtest failures emit a more readable version of the tuple than
// its byte representation.
::std::ostream& operator<<(
    ::std::ostream& os,
    const Tuple7<int, int, int, ResetPoint, ClientState, bool, bool>& t) {
  return os << t.a << ", " << t.b << ", " << t.c << ", " << t.d << ", " << t.e
            << ", " << t.f << ", " << t.g;
}

// Wait for |note| to report a state and if it's not |expected_state| then
// assert |client| has deleted its decoder.
static void AssertWaitForStateOrDeleted(
    ClientStateNotification<ClientState>* note,
    GLRenderingVDAClient* client,
    ClientState expected_state) {
  ClientState state = note->Wait();
  if (state == expected_state) return;
  ASSERT_TRUE(client->decoder_deleted())
      << "Decoder not deleted but Wait() returned " << state
      << ", instead of " << expected_state;
}

// We assert a minimal number of concurrent decoders we expect to succeed.
// Different platforms can support more concurrent decoders, so we don't assert
// failure above this.
enum { kMinSupportedNumConcurrentDecoders = 3 };

// Test the most straightforward case possible: data is decoded from a single
// chunk and rendered to the screen.
TEST_P(VideoDecodeAcceleratorTest, TestSimpleDecode) {
  // Required for Thread to work.  Not used otherwise.
  base::ShadowingAtExitManager at_exit_manager;

  const size_t num_concurrent_decoders = GetParam().a;
  const size_t num_in_flight_decodes = GetParam().b;
  const int num_play_throughs = GetParam().c;
  const int reset_point = GetParam().d;
  const int delete_decoder_state = GetParam().e;
  bool test_reuse_delay = GetParam().f;
  const bool render_as_thumbnails = GetParam().g;

  std::vector<TestVideoFile*> test_video_files;
  ParseAndReadTestVideoData(g_test_video_data,
                            num_concurrent_decoders,
                            reset_point,
                            &test_video_files);

  // Suppress GL rendering for all tests when the "--disable_rendering" is set.
  const bool suppress_rendering = g_disable_rendering;

  std::vector<ClientStateNotification<ClientState>*>
      notes(num_concurrent_decoders, NULL);
  std::vector<GLRenderingVDAClient*> clients(num_concurrent_decoders, NULL);

  // Initialize the rendering helper.
  base::Thread rendering_thread("GLRenderingVDAClientThread");
  base::Thread::Options options;
  options.message_loop_type = base::MessageLoop::TYPE_DEFAULT;
#if defined(OS_WIN)
  // For windows the decoding thread initializes the media foundation decoder
  // which uses COM. We need the thread to be a UI thread.
  options.message_loop_type = base::MessageLoop::TYPE_UI;
#endif  // OS_WIN

  rendering_thread.StartWithOptions(options);
  RenderingHelper rendering_helper;

  base::WaitableEvent done(false, false);
  RenderingHelperParams helper_params;
  helper_params.num_windows = num_concurrent_decoders;
  helper_params.render_as_thumbnails = render_as_thumbnails;
  if (render_as_thumbnails) {
    // Only one decoder is supported with thumbnail rendering
    CHECK_EQ(num_concurrent_decoders, 1U);
    gfx::Size frame_size(test_video_files[0]->width,
                         test_video_files[0]->height);
    helper_params.frame_dimensions.push_back(frame_size);
    helper_params.window_dimensions.push_back(kThumbnailsDisplaySize);
    helper_params.thumbnails_page_size = kThumbnailsPageSize;
    helper_params.thumbnail_size = kThumbnailSize;
  } else {
    for (size_t index = 0; index < test_video_files.size(); ++index) {
      gfx::Size frame_size(test_video_files[index]->width,
                           test_video_files[index]->height);
      helper_params.frame_dimensions.push_back(frame_size);
      helper_params.window_dimensions.push_back(frame_size);
    }
  }
  rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&RenderingHelper::Initialize,
                 base::Unretained(&rendering_helper),
                 helper_params,
                 &done));
  done.Wait();

  // First kick off all the decoders.
  for (size_t index = 0; index < num_concurrent_decoders; ++index) {
    TestVideoFile* video_file =
        test_video_files[index % test_video_files.size()];
    ClientStateNotification<ClientState>* note =
        new ClientStateNotification<ClientState>();
    notes[index] = note;

    int delay_after_frame_num = std::numeric_limits<int>::max();
    if (test_reuse_delay &&
        kMaxFramesToDelayReuse * 2 < video_file->num_frames) {
      delay_after_frame_num = video_file->num_frames - kMaxFramesToDelayReuse;
    }

    GLRenderingVDAClient* client =
        new GLRenderingVDAClient(&rendering_helper,
                                 index,
                                 note,
                                 video_file->data_str,
                                 num_in_flight_decodes,
                                 num_play_throughs,
                                 video_file->reset_after_frame_num,
                                 delete_decoder_state,
                                 video_file->width,
                                 video_file->height,
                                 video_file->profile,
                                 g_rendering_fps,
                                 suppress_rendering,
                                 delay_after_frame_num);
    clients[index] = client;

    rendering_thread.message_loop()->PostTask(
        FROM_HERE,
        base::Bind(&GLRenderingVDAClient::CreateDecoder,
                   base::Unretained(client)));

    ASSERT_EQ(note->Wait(), CS_DECODER_SET);
  }
  // Then wait for all the decodes to finish.
  // Only check performance & correctness later if we play through only once.
  bool skip_performance_and_correctness_checks = num_play_throughs > 1;
  for (size_t i = 0; i < num_concurrent_decoders; ++i) {
    ClientStateNotification<ClientState>* note = notes[i];
    ClientState state = note->Wait();
    if (state != CS_INITIALIZED) {
      skip_performance_and_correctness_checks = true;
      // We expect initialization to fail only when more than the supported
      // number of decoders is instantiated.  Assert here that something else
      // didn't trigger failure.
      ASSERT_GT(num_concurrent_decoders,
                static_cast<size_t>(kMinSupportedNumConcurrentDecoders));
      continue;
    }
    ASSERT_EQ(state, CS_INITIALIZED);
    for (int n = 0; n < num_play_throughs; ++n) {
      // For play-throughs other than the first, we expect initialization to
      // succeed unconditionally.
      if (n > 0) {
        ASSERT_NO_FATAL_FAILURE(
            AssertWaitForStateOrDeleted(note, clients[i], CS_INITIALIZED));
      }
      // InitializeDone kicks off decoding inside the client, so we just need to
      // wait for Flush.
      ASSERT_NO_FATAL_FAILURE(
          AssertWaitForStateOrDeleted(note, clients[i], CS_FLUSHING));
      ASSERT_NO_FATAL_FAILURE(
          AssertWaitForStateOrDeleted(note, clients[i], CS_FLUSHED));
      // FlushDone requests Reset().
      ASSERT_NO_FATAL_FAILURE(
          AssertWaitForStateOrDeleted(note, clients[i], CS_RESETTING));
    }
    ASSERT_NO_FATAL_FAILURE(
        AssertWaitForStateOrDeleted(note, clients[i], CS_RESET));
    // ResetDone requests Destroy().
    ASSERT_NO_FATAL_FAILURE(
        AssertWaitForStateOrDeleted(note, clients[i], CS_DESTROYED));
  }
  // Finally assert that decoding went as expected.
  for (size_t i = 0; i < num_concurrent_decoders &&
           !skip_performance_and_correctness_checks; ++i) {
    // We can only make performance/correctness assertions if the decoder was
    // allowed to finish.
    if (delete_decoder_state < CS_FLUSHED)
      continue;
    GLRenderingVDAClient* client = clients[i];
    TestVideoFile* video_file = test_video_files[i % test_video_files.size()];
    if (video_file->num_frames > 0) {
      // Expect the decoded frames may be more than the video frames as frames
      // could still be returned until resetting done.
      if (video_file->reset_after_frame_num > 0)
        EXPECT_GE(client->num_decoded_frames(), video_file->num_frames);
      else
        EXPECT_EQ(client->num_decoded_frames(), video_file->num_frames);
    }
    if (reset_point == END_OF_STREAM_RESET) {
      EXPECT_EQ(video_file->num_fragments, client->num_skipped_fragments() +
                client->num_queued_fragments());
      EXPECT_EQ(client->num_done_bitstream_buffers(),
                client->num_queued_fragments());
    }
    LOG(INFO) << "Decoder " << i << " fps: " << client->frames_per_second();
    if (!render_as_thumbnails) {
      int min_fps = suppress_rendering ?
          video_file->min_fps_no_render : video_file->min_fps_render;
      if (min_fps > 0 && !test_reuse_delay)
        EXPECT_GT(client->frames_per_second(), min_fps);
    }
  }

  if (render_as_thumbnails) {
    std::vector<unsigned char> rgb;
    bool alpha_solid;
    rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&RenderingHelper::GetThumbnailsAsRGB,
                 base::Unretained(&rendering_helper),
                 &rgb, &alpha_solid, &done));
    done.Wait();

    std::vector<std::string> golden_md5s;
    std::string md5_string = base::MD5String(
        base::StringPiece(reinterpret_cast<char*>(&rgb[0]), rgb.size()));
    ReadGoldenThumbnailMD5s(test_video_files[0], &golden_md5s);
    std::vector<std::string>::iterator match =
        find(golden_md5s.begin(), golden_md5s.end(), md5_string);
    if (match == golden_md5s.end()) {
      // Convert raw RGB into PNG for export.
      std::vector<unsigned char> png;
      gfx::PNGCodec::Encode(&rgb[0],
                            gfx::PNGCodec::FORMAT_RGB,
                            kThumbnailsPageSize,
                            kThumbnailsPageSize.width() * 3,
                            true,
                            std::vector<gfx::PNGCodec::Comment>(),
                            &png);

      LOG(ERROR) << "Unknown thumbnails MD5: " << md5_string;

      base::FilePath filepath(test_video_files[0]->file_name);
      filepath = filepath.AddExtension(FILE_PATH_LITERAL(".bad_thumbnails"));
      filepath = filepath.AddExtension(FILE_PATH_LITERAL(".png"));
      int num_bytes = file_util::WriteFile(filepath,
                                           reinterpret_cast<char*>(&png[0]),
                                           png.size());
      ASSERT_EQ(num_bytes, static_cast<int>(png.size()));
    }
    ASSERT_NE(match, golden_md5s.end());
    EXPECT_EQ(alpha_solid, true) << "RGBA frame had incorrect alpha";
  }

  // Output the frame delivery time to file
  // We can only make performance/correctness assertions if the decoder was
  // allowed to finish.
  if (g_frame_delivery_log != NULL && delete_decoder_state >= CS_FLUSHED) {
    base::PlatformFile output_file = base::CreatePlatformFile(
        base::FilePath(g_frame_delivery_log),
        base::PLATFORM_FILE_CREATE_ALWAYS | base::PLATFORM_FILE_WRITE,
        NULL,
        NULL);
    for (size_t i = 0; i < num_concurrent_decoders; ++i) {
      clients[i]->OutputFrameDeliveryTimes(output_file);
    }
    base::ClosePlatformFile(output_file);
  }

  rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&STLDeleteElements<std::vector<GLRenderingVDAClient*> >,
                 &clients));
  rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&STLDeleteElements<
          std::vector<ClientStateNotification<ClientState>*> >,
          &notes));
  rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&STLDeleteElements<std::vector<TestVideoFile*> >,
                 &test_video_files));
  rendering_thread.message_loop()->PostTask(
      FROM_HERE,
      base::Bind(&RenderingHelper::UnInitialize,
                 base::Unretained(&rendering_helper),
                 &done));
  done.Wait();
  rendering_thread.Stop();
};

// Test that replay after EOS works fine.
INSTANTIATE_TEST_CASE_P(
    ReplayAfterEOS, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 4, END_OF_STREAM_RESET, CS_RESET, false, false)));

// This hangs on Exynos, preventing further testing and wasting test machine
// time.
// TODO(ihf): Enable again once http://crbug.com/269754 is fixed.
#if defined(ARCH_CPU_X86_FAMILY)
// Test that Reset() before the first Decode() works fine.
INSTANTIATE_TEST_CASE_P(
    ResetBeforeDecode, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, START_OF_STREAM_RESET, CS_RESET, false, false)));
#endif  // ARCH_CPU_X86_FAMILY

// Test that Reset() mid-stream works fine and doesn't affect decoding even when
// Decode() calls are made during the reset.
INSTANTIATE_TEST_CASE_P(
    MidStreamReset, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, MID_STREAM_RESET, CS_RESET, false, false)));

INSTANTIATE_TEST_CASE_P(
    SlowRendering, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_RESET, true, false)));

// Test that Destroy() mid-stream works fine (primarily this is testing that no
// crashes occur).
INSTANTIATE_TEST_CASE_P(
    TearDownTiming, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_DECODER_SET, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_INITIALIZED, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_FLUSHING, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_FLUSHED, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_RESETTING, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_RESET, false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET,
                  static_cast<ClientState>(-1), false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET,
                  static_cast<ClientState>(-10), false, false),
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET,
                  static_cast<ClientState>(-100), false, false)));

// Test that decoding various variation works with multiple in-flight decodes.
INSTANTIATE_TEST_CASE_P(
    DecodeVariations, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_RESET, false, false),
        MakeTuple(1, 10, 1, END_OF_STREAM_RESET, CS_RESET, false, false),
        // Tests queuing.
        MakeTuple(1, 15, 1, END_OF_STREAM_RESET, CS_RESET, false, false)));

// Find out how many concurrent decoders can go before we exhaust system
// resources.
INSTANTIATE_TEST_CASE_P(
    ResourceExhaustion, VideoDecodeAcceleratorTest,
    ::testing::Values(
        // +0 hack below to promote enum to int.
        MakeTuple(kMinSupportedNumConcurrentDecoders + 0, 1, 1,
                  END_OF_STREAM_RESET, CS_RESET, false, false),
        MakeTuple(kMinSupportedNumConcurrentDecoders + 1, 1, 1,
                  END_OF_STREAM_RESET, CS_RESET, false, false)));

// Thumbnailing test
INSTANTIATE_TEST_CASE_P(
    Thumbnail, VideoDecodeAcceleratorTest,
    ::testing::Values(
        MakeTuple(1, 1, 1, END_OF_STREAM_RESET, CS_RESET, false, true)));

// TODO(fischman, vrk): add more tests!  In particular:
// - Test life-cycle: Seek/Stop/Pause/Play for a single decoder.
// - Test alternate configurations
// - Test failure conditions.
// - Test frame size changes mid-stream

}  // namespace
}  // namespace content

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);  // Removes gtest-specific args.
  CommandLine::Init(argc, argv);

  // Needed to enable DVLOG through --vmodule.
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  settings.dcheck_state =
      logging::ENABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS;
  CHECK(logging::InitLogging(settings));

  CommandLine* cmd_line = CommandLine::ForCurrentProcess();
  DCHECK(cmd_line);

  CommandLine::SwitchMap switches = cmd_line->GetSwitches();
  for (CommandLine::SwitchMap::const_iterator it = switches.begin();
       it != switches.end(); ++it) {
    if (it->first == "test_video_data") {
      content::g_test_video_data = it->second.c_str();
      continue;
    }
    if (it->first == "frame_delivery_log") {
      content::g_frame_delivery_log = it->second.c_str();
      continue;
    }
    if (it->first == "rendering_fps") {
      // On Windows, CommandLine::StringType is wstring. We need to convert
      // it to std::string first
      std::string input(it->second.begin(), it->second.end());
      CHECK(base::StringToDouble(input, &content::g_rendering_fps));
      continue;
    }
    if (it->first == "disable_rendering") {
      content::g_disable_rendering = true;
      continue;
    }
    if (it->first == "v" || it->first == "vmodule")
      continue;
    LOG(FATAL) << "Unexpected switch: " << it->first << ":" << it->second;
  }

  base::ShadowingAtExitManager at_exit_manager;

#if defined(OS_WIN)
  content::DXVAVideoDecodeAccelerator::PreSandboxInitialization();
#elif defined(OS_CHROMEOS)
#if defined(ARCH_CPU_ARMEL)
  content::ExynosVideoDecodeAccelerator::PreSandboxInitialization();
#elif defined(ARCH_CPU_X86_FAMILY)
  content::VaapiWrapper::PreSandboxInitialization();
#endif  // ARCH_CPU_ARMEL
#endif  // OS_CHROMEOS

  return RUN_ALL_TESTS();
}