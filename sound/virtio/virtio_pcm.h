/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * virtio-snd: Virtio sound device
 * Copyright (C) 2021 OpenSynergy GmbH
 */
#ifndef VIRTIO_SND_PCM_H
#define VIRTIO_SND_PCM_H

#include <linux/atomic.h>
#include <linux/virtio_config.h>
#include <sound/pcm.h>

struct virtio_pcm;
struct virtio_pcm_msg;

/**
 * struct virtio_pcm_substream - VirtIO PCM substream.
 * @snd: VirtIO sound device.
 * @nid: Function group node identifier.
 * @sid: Stream identifier.
 * @direction: Stream data flow direction (SNDRV_PCM_STREAM_XXX).
 * @features: Stream VirtIO feature bit map (1 << VIRTIO_SND_PCM_F_XXX).
 * @substream: Kernel ALSA substream.
 * @hw: Kernel ALSA substream hardware descriptor.
 */
struct virtio_pcm_substream {
	struct virtio_snd *snd;
	unsigned int nid;
	unsigned int sid;
	u32 direction;
	u32 features;
	struct snd_pcm_substream *substream;
	struct snd_pcm_hardware hw;
};

/**
 * struct virtio_pcm_stream - VirtIO PCM stream.
 * @substreams: VirtIO substreams belonging to the stream.
 * @nsubstreams: Number of substreams.
 */
struct virtio_pcm_stream {
	struct virtio_pcm_substream **substreams;
	unsigned int nsubstreams;
};

/**
 * struct virtio_pcm - VirtIO PCM device.
 * @list: VirtIO PCM list entry.
 * @nid: Function group node identifier.
 * @pcm: Kernel PCM device.
 * @streams: VirtIO PCM streams (playback and capture).
 */
struct virtio_pcm {
	struct list_head list;
	unsigned int nid;
	struct snd_pcm *pcm;
	struct virtio_pcm_stream streams[SNDRV_PCM_STREAM_LAST + 1];
};

int virtsnd_pcm_validate(struct virtio_device *vdev);

int virtsnd_pcm_parse_cfg(struct virtio_snd *snd);

int virtsnd_pcm_build_devs(struct virtio_snd *snd);

struct virtio_pcm *virtsnd_pcm_find(struct virtio_snd *snd, unsigned int nid);

struct virtio_pcm *virtsnd_pcm_find_or_create(struct virtio_snd *snd,
					      unsigned int nid);

#endif /* VIRTIO_SND_PCM_H */
