(() => {
  'use strict';

  const maxHeight = 1440;
  const heights = { tiny: 144, small: 240, medium: 360, large: 480, hd720: 720,
    hd1080: 1080, hd1440: 1440, hd2160: 2160, hd2880: 2880, highres: Infinity };
  const startupDelays = [200, 500, 1000, 2500, 5000];
  const switchDelays = [2500, 5000, 10000];
  const offerDelays = [1200, 3500, 8000];

  let player = null;
  let lastRequest = null;
  let timer = null;
  let dueAt = 0;
  let startupAttempt = 0;
  let offerTimer = null;
  let offerAttempt = 0;
  let offerVideoId = null;

  function bestOffer(source) {
    let rows = source.getAvailableQualityData?.();
    if (!rows?.length) {
      rows = (source.getAvailableQualityLevels?.() ?? []).map(quality => ({ quality }));
    }
    let best = null;
    const formats = new Map();
    for (const row of rows) {
      if (!row?.quality || row.quality === 'auto' || row.isPlayable === false) continue;
      const label = row.qualityLabel ?? '';
      const match = /(\d{2,5})\s*p(\d{2,3})?/i.exec(label);
      const height = match ? Number(match[1]) : heights[row.quality];
      if (!(height > 0 && height <= maxHeight)) continue;
      const premium = Boolean(row.paygatedQualityDetails || /premium|enhanced bitrate/i.test(label));
      const fps = Number(match?.[2] ?? 0);
      const offer = { quality: row.quality, formatId: row.formatId, height, premium, fps };
      if (offer.formatId != null) formats.set(String(offer.formatId), offer);
      if (!best || height > best.height || (height === best.height &&
          (premium > best.premium || (premium === best.premium && fps > best.fps)))) {
        best = offer;
      }
    }
    return best && { ...best, formats };
  }

  function schedule(delay) {
    const deadline = performance.now() + delay;
    if (timer !== null && dueAt <= deadline) return;
    clearTimeout(timer);
    dueAt = deadline;
    timer = setTimeout(check, delay);
  }

  function wake() {
    startupAttempt = 0;
    schedule(0);
  }

  function waitForPlayer() {
    if (startupAttempt < startupDelays.length) schedule(startupDelays[startupAttempt++]);
  }

  function resetOfferChecks(videoId) {
    clearTimeout(offerTimer);
    offerTimer = null;
    offerAttempt = 0;
    offerVideoId = videoId;
  }

  // Some videos expose an SD stream before the full list arrives, without another player event.
  function checkLaterOffers(videoId, height) {
    if (offerVideoId !== videoId) resetOfferChecks(videoId);
    if (height >= maxHeight) {
      if (offerTimer !== null) resetOfferChecks(videoId);
    } else if (offerTimer === null && offerAttempt < offerDelays.length) {
      offerTimer = setTimeout(() => {
        offerTimer = null;
        wake();
      }, offerDelays[offerAttempt++]);
    }
  }

  function bind(next) {
    if (next === player) return;
    for (const name of ['onPlaybackQualityChange', 'onApiChange']) {
      player?.removeEventListener(name, wake);
      next?.addEventListener(name, wake);
    }
    player = next;
    lastRequest = null;
    resetOfferChecks(null);
  }

  function alreadySelected(target) {
    const preferred = player.getPreferredQuality?.();
    const pinned = preferred === target.quality;
    // Auto can currently be decoding the target resolution without a manual pin.
    if (preferred && preferred !== 'unknown' && !pinned) return false;
    const checkFormat = target.formatId != null && (target.premium || target.fps > 0);
    if (pinned && !checkFormat) return true;

    const actual = player.getPlaybackQuality?.();
    if (actual !== target.quality) {
      // The preferred setting changes before buffered frames from the old tier finish.
      if (pinned) return true;
      // A legacy player with neither readback can only be requested once per target.
      return Boolean(lastRequest && (!actual || actual === 'unknown'));
    }
    if (checkFormat) {
      const fmt = player.getVideoStats?.()?.fmt;
      if (fmt != null && String(fmt) !== String(target.formatId)) {
        // Several codecs can represent the same resolution, frame rate and Premium choice.
        const current = target.formats.get(String(fmt));
        if (current?.quality === target.quality)
          return current.premium === target.premium && current.fps === target.fps;
        return !target.premium;
      }
    }
    return true;
  }

  function check() {
    timer = null;
    const path = location.pathname;
    const shorts = path.startsWith('/shorts/');
    if (!(shorts || path === '/watch' || path.startsWith('/embed/') || path.startsWith('/live/'))) {
      bind(null);
      return;
    }
    try {
      const candidate = shorts
        ? document.querySelector('ytd-reel-video-renderer[is-active] .html5-video-player')
          ?? document.getElementById('shorts-player')
        : document.getElementById('movie_player');
      bind(typeof candidate?.setPlaybackQualityRange === 'function' ? candidate : null);
      if (!player) return waitForPlayer();
      if (player.classList.contains('ad-showing') || player.classList.contains('ad-interrupting')) return;

      const videoId = player.getVideoData?.()?.video_id;
      const pageId = path === '/watch' ? new URLSearchParams(location.search).get('v')
        : path.split('/')[2];
      // The URL can advance before the reused player finishes switching videos.
      if (!path.startsWith('/embed/') && pageId && videoId && pageId !== videoId) return waitForPlayer();
      const target = bestOffer(player);
      if (!target) return waitForPlayer();
      checkLaterOffers(videoId || pageId, target.height);
      const key = JSON.stringify([videoId || pageId, target.quality, target.formatId]);
      const now = performance.now();
      if (lastRequest?.key !== key) lastRequest = null;
      if (alreadySelected(target)) {
        if (lastRequest) lastRequest.attempts = 0;
        return;
      }
      if (lastRequest) {
        // A switch can buffer for a few seconds. Bound retries if the stream refuses the request.
        if (now < lastRequest.nextTry) {
          if (lastRequest.attempts < switchDelays.length) schedule(lastRequest.nextTry - now);
          return;
        }
      } else {
        lastRequest = { key, attempts: 0, nextTry: 0 };
      }
      // Record the attempt before calling the player. It can emit its change event synchronously.
      const attempt = Math.min(lastRequest.attempts, switchDelays.length - 1);
      lastRequest.attempts = attempt + 1;
      lastRequest.nextTry = now + (lastRequest.attempts === switchDelays.length ? 30000 : switchDelays[attempt]);
      if (target.formatId != null) {
        player.setPlaybackQualityRange(target.quality, target.quality, target.formatId);
      } else {
        player.setPlaybackQualityRange(target.quality, target.quality);
      }
      schedule(switchDelays[attempt]);
    } catch {
      // During navigation the old player can expose methods after its underlying state is gone.
      waitForPlayer();
    }
  }

  document.addEventListener('yt-navigate-finish', wake);
  document.addEventListener('yt-page-data-updated', wake);
  window.addEventListener('pageshow', wake);
  for (const event of ['loadedmetadata', 'playing', 'resize']) {
    document.addEventListener(event, signal => {
      if (signal.target?.tagName === 'VIDEO') wake();
    }, true);
  }
  wake();
})();
