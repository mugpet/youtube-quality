(() => {
  'use strict';

  const maxHeight = 1440;
  const heights = { tiny: 144, small: 240, medium: 360, large: 480, hd720: 720,
    hd1080: 1080, hd1440: 1440, hd2160: 2160, hd2880: 2880, highres: Infinity };
  const startupDelays = [200, 500, 1000, 2500, 5000];
  const switchDelays = [2500, 5000, 10000];
  const offerDelays = [1200, 3500, 8000];
  const settleDelay = 4000;

  let player = null;
  let selection = null;
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
    const levels = new Set();
    for (const row of rows) {
      if (!row?.quality || row.quality === 'auto' || row.isPlayable === false) continue;
      const label = row.qualityLabel ?? '';
      const match = /(\d{2,5})\s*p(\d{2,3})?/i.exec(label);
      const height = match ? Number(match[1]) : heights[row.quality];
      if (!(height > 0)) continue;
      levels.add(row.quality);
      const premium = Boolean(row.paygatedQualityDetails || /premium|enhanced bitrate/i.test(label));
      const fps = Number(match?.[2] ?? 0);
      const offer = { quality: row.quality, formatId: row.formatId, height, premium, fps };
      if (offer.formatId != null) formats.set(String(offer.formatId), offer);
      if (height > maxHeight) continue;
      if (!best || height > best.height || (height === best.height &&
          (premium > best.premium || (premium === best.premium && fps > best.fps)))) {
        best = offer;
      }
    }
    return best && { ...best, formats, levels };
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
    selection = null;
    resetOfferChecks(null);
  }

  function needsCorrection(target) {
    // A player can retain the previous stream's tier before the new video has frames.
    const video = player.querySelector('video');
    const hasFrames = video?.readyState >= 2;
    let actual = hasFrames ? player.getPlaybackQuality?.() : null;
    if (!Object.hasOwn(heights, actual) && !target.levels.has(actual)) {
      actual = null;
      if (hasFrames) {
        const current = target.formats.get(String(player.getVideoStats?.()?.fmt));
        const height = Math.min(video.videoWidth, video.videoHeight);
        // Cropped video can have nonstandard dimensions. Only infer an exact match.
        actual = current?.quality ?? (height === target.height ? target.quality : null);
      }
    }

    if (actual === target.quality) {
      if (target.formatId != null && (target.premium || target.fps > 0)) {
        const current = target.formats.get(String(player.getVideoStats?.()?.fmt));
        // Only a known inferior choice justifies changing a stream at the right resolution.
        if (current?.quality === target.quality &&
            (current.premium !== target.premium || current.fps !== target.fps)) return true;
      }
      selection.playedBest = true;
      return false;
    }

    const preferred = player.getPreferredQuality?.();
    // A correct pin can take time to replace old buffered frames. A later drop
    // after this video reached the target gives us a reason to reapply it once.
    if (preferred === target.quality) return Boolean(actual && selection.playedBest);
    if (actual || Object.hasOwn(heights, preferred) || target.levels.has(preferred)) return true;
    return null;
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
      if (selection?.key !== key) selection = { key, playedBest: false, confirmAt: 0, attempts: 0, nextTry: 0 };
      const correction = needsCorrection(target);
      if (!correction) {
        selection.confirmAt = 0;
        selection.attempts = 0;
        if (correction === null) waitForPlayer();
        return;
      }
      // Give Auto a chance to settle after startup or a brief quality dip.
      if (!selection.confirmAt) selection.confirmAt = now + settleDelay;
      if (now < selection.confirmAt) return schedule(selection.confirmAt - now);
      if (now < selection.nextTry) {
        if (selection.attempts < switchDelays.length) schedule(selection.nextTry - now);
        return;
      }
      // Record the attempt before calling the player. It can emit its change event synchronously.
      const attempt = Math.min(selection.attempts, switchDelays.length - 1);
      selection.playedBest = false;
      selection.attempts = attempt + 1;
      selection.nextTry = now + (selection.attempts === switchDelays.length ? 30000 : switchDelays[attempt]);
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
