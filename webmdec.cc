/*
 *  Copyright (c) 2013 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "./webmdec.h"

#include <cstring>
#include <cstdio>

#include "third_party/libwebm/mkvparser.hpp"
#include "third_party/libwebm/mkvreader.hpp"

namespace {

void reset(struct WebmInputContext *const webm_ctx) {
  if (webm_ctx->reader != NULL) {
    mkvparser::MkvReader *const reader =
        reinterpret_cast<mkvparser::MkvReader*>(webm_ctx->reader);
    delete reader;
  }
  if (webm_ctx->segment != NULL) {
    mkvparser::Segment *const segment =
        reinterpret_cast<mkvparser::Segment*>(webm_ctx->segment);
    delete segment;
  }
  if (webm_ctx->buffer != NULL) {
    delete[] webm_ctx->buffer;
  }
  webm_ctx->reader = NULL;
  webm_ctx->segment = NULL;
  webm_ctx->buffer = NULL;
  webm_ctx->cluster = NULL;
  webm_ctx->block_entry = NULL;
  webm_ctx->block = NULL;
  webm_ctx->block_frame_index = 0;
  webm_ctx->video_track_index = 0;
  webm_ctx->timestamp_ns = 0;
}

void get_first_cluster(struct WebmInputContext *const webm_ctx) {
  mkvparser::Segment *const segment =
      reinterpret_cast<mkvparser::Segment*>(webm_ctx->segment);
  const mkvparser::Cluster *const cluster = segment->GetFirst();
  webm_ctx->cluster = cluster;
}

void rewind_and_reset(struct WebmInputContext *const webm_ctx,
                      struct VpxInputContext *const vpx_ctx) {
  rewind(vpx_ctx->file);
  reset(webm_ctx);
}

}  // namespace

int file_is_webm(struct WebmInputContext *webm_ctx,
                 struct VpxInputContext *vpx_ctx) {
  mkvparser::MkvReader *const reader = new mkvparser::MkvReader(vpx_ctx->file);
  webm_ctx->reader = reader;

  mkvparser::EBMLHeader header;
  long long pos = 0;
  if (header.Parse(reader, pos) < 0) {
    rewind_and_reset(webm_ctx, vpx_ctx);
    return 0;
  }

  mkvparser::Segment* segment;
  if (mkvparser::Segment::CreateInstance(reader, pos, segment)) {
    rewind_and_reset(webm_ctx, vpx_ctx);
    return 0;
  }
  webm_ctx->segment = segment;
  if (segment->Load() < 0) {
    rewind_and_reset(webm_ctx, vpx_ctx);
    return 0;
  }

  const mkvparser::Tracks *const tracks = segment->GetTracks();
  const mkvparser::VideoTrack* video_track = NULL;
  for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
    const mkvparser::Track* const track = tracks->GetTrackByIndex(i);
    if (track->GetType() == mkvparser::Track::kVideo) {
      video_track = static_cast<const mkvparser::VideoTrack*>(track);
      webm_ctx->video_track_index = track->GetNumber();
      break;
    }
  }

  if (video_track == NULL) {
    rewind_and_reset(webm_ctx, vpx_ctx);
    return 0;
  }

  if (!strncmp(video_track->GetCodecId(), "V_VP8", 5)) {
    vpx_ctx->fourcc = VP8_FOURCC;
  } else if (!strncmp(video_track->GetCodecId(), "V_VP9", 5)) {
    vpx_ctx->fourcc = VP9_FOURCC;
  } else {
    rewind_and_reset(webm_ctx, vpx_ctx);
    return 0;
  }

  vpx_ctx->framerate.denominator = 0;
  vpx_ctx->framerate.numerator = 0;
  vpx_ctx->width = video_track->GetWidth();
  vpx_ctx->height = video_track->GetHeight();

  get_first_cluster(webm_ctx);

  return 1;
}

int webm_read_frame(struct WebmInputContext *webm_ctx,
                    uint8_t **buffer,
                    size_t *bytes_in_buffer,
                    size_t *buffer_size) {
  mkvparser::Segment *const segment =
      reinterpret_cast<mkvparser::Segment*>(webm_ctx->segment);
  const mkvparser::Cluster* cluster =
      reinterpret_cast<const mkvparser::Cluster*>(webm_ctx->cluster);
  const mkvparser::Block *block =
      reinterpret_cast<const mkvparser::Block*>(webm_ctx->block);
  const mkvparser::BlockEntry *block_entry =
      reinterpret_cast<const mkvparser::BlockEntry*>(webm_ctx->block_entry);
  bool block_entry_eos = false;
  do {
    long status = 0;
    bool get_new_block = false;
    if (block_entry == NULL && !block_entry_eos) {
      status = cluster->GetFirst(block_entry);
      get_new_block = true;
    } else if (block_entry_eos || block_entry->EOS()) {
      cluster = segment->GetNext(cluster);
      if (cluster == NULL || cluster->EOS()) {
        *bytes_in_buffer = 0;
        return 1;
      }
      status = cluster->GetFirst(block_entry);
      block_entry_eos = false;
      get_new_block = true;
    } else if (block == NULL ||
               webm_ctx->block_frame_index == block->GetFrameCount() ||
               block->GetTrackNumber() != webm_ctx->video_track_index) {
      status = cluster->GetNext(block_entry, block_entry);
      if (block_entry == NULL || block_entry->EOS()) {
        block_entry_eos = true;
        continue;
      }
      get_new_block = true;
    }
    if (status) {
      return -1;
    }
    if (get_new_block) {
      block = block_entry->GetBlock();
      webm_ctx->block_frame_index = 0;
    }
  } while (block->GetTrackNumber() != webm_ctx->video_track_index ||
           block_entry_eos);

  webm_ctx->cluster = cluster;
  webm_ctx->block_entry = block_entry;
  webm_ctx->block = block;

  const mkvparser::Block::Frame& frame =
      block->GetFrame(webm_ctx->block_frame_index);
  ++webm_ctx->block_frame_index;
  if (frame.len > static_cast<long>(*buffer_size)) {
    delete[] *buffer;
    *buffer = new uint8_t[frame.len];
    if (*buffer == NULL) {
      return -1;
    }
    *buffer_size = frame.len;
    webm_ctx->buffer = *buffer;
  }
  *bytes_in_buffer = frame.len;
  webm_ctx->timestamp_ns = block->GetTime(cluster);

  mkvparser::MkvReader *const reader =
      reinterpret_cast<mkvparser::MkvReader*>(webm_ctx->reader);
  return frame.Read(reader, *buffer) ? -1 : 0;
}

int webm_guess_framerate(struct WebmInputContext *webm_ctx,
                         struct VpxInputContext *vpx_ctx) {
  uint32_t i = 0;
  uint8_t *buffer = NULL;
  size_t bytes_in_buffer = 0;
  size_t buffer_size = 0;
  while (webm_ctx->timestamp_ns < 1000000000 && i < 50) {
    if (webm_read_frame(webm_ctx, &buffer, &bytes_in_buffer, &buffer_size)) {
      break;
    }
    ++i;
  }
  vpx_ctx->framerate.numerator = (i - 1) * 1000000;
  vpx_ctx->framerate.denominator =
      static_cast<int>(webm_ctx->timestamp_ns / 1000);
  delete[] buffer;

  get_first_cluster(webm_ctx);
  webm_ctx->block = NULL;
  webm_ctx->block_entry = NULL;
  webm_ctx->block_frame_index = 0;
  webm_ctx->timestamp_ns = 0;

  return 0;
}

void webm_free(struct WebmInputContext *webm_ctx) {
  reset(webm_ctx);
}
