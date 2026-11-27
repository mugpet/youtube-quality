const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const script = readFileSync(join(__dirname, '../extension/quality.js'), 'utf8');

function events() {
  const listeners = new Map();
  return {
    addEventListener(type, listener) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(listener);
    },
    removeEventListener(type, listener) {
      listeners.get(type)?.delete(listener);
    },
    emit(type, event) {
      for (const listener of listeners.get(type) ?? []) listener(event);
    },
  };
}

function quality(quality, qualityLabel, extra = {}) {
  return { quality, qualityLabel, isPlayable: true, ...extra };
}

function createPlayer(offers, prepare = () => {}) {
  let clock = 100_000;
  let nextTimer = 0;
  const timers = new Map();
  const writes = [];
  function later(callback, delay = 0) {
    const id = ++nextTimer;
    timers.set(id, { callback, due: clock + Number(delay) });
    return id;
  }
  function flush() {
    let count = 0;
    while (timers.size) {
      assert.ok(++count < 100, 'timer loop did not settle');
      const [id, job] = [...timers.entries()].sort((a, b) => a[1].due - b[1].due)[0];
      timers.delete(id);
      clock = job.due;
      job.callback();
    }
  }

  const state = { offers, videoId: 'first', actual: 'medium', preferred: 'auto', format: 18 };
  const video = Object.assign(events(), {
    tagName: 'VIDEO', nodeName: 'VIDEO', isConnected: true, readyState: 4,
    paused: false, ended: false, videoHeight: 360, videoWidth: 640,
  });
  const player = Object.assign(events(), {
    id: 'movie_player', tagName: 'DIV', isConnected: true,
    classList: { contains: () => false },
    contains: element => element === video,
    querySelector: selector => selector.includes('video') ? video : null,
    getVideoData: () => ({ video_id: state.videoId }),
    getAvailableQualityData: () => state.offers,
    getAvailableQualityLevels: () => state.offers.map(row => row.quality),
    getPlaybackQuality: () => state.actual,
    getPreferredQuality: () => state.preferred,
    getVideoStats: () => ({ fmt: state.format }),
    getPlayerState: () => 1,
    setPlaybackQualityRange(low, high, format) {
      writes.push({ low, high, format });
      state.actual = low;
      state.preferred = low;
      state.format = format;
      later(() => player.emit('onPlaybackQualityChange', low), 25);
    },
  });
  video.closest = () => player;
  const location = new URL('https://www.youtube.com/watch?v=first');
  const document = Object.assign(events(), {
    readyState: 'complete', visibilityState: 'visible', hidden: false,
    contains: element => element === video || element === player,
    getElementById: id => id === player.id ? player : null,
    querySelector(selector) {
      if (selector.includes('player')) return player;
      return selector.includes('video') ? video : null;
    },
    querySelectorAll(selector) {
      const found = this.querySelector(selector);
      return found ? [found] : [];
    },
  });
  const window = Object.assign(events(), {
    document, location, URL, URLSearchParams,
    setTimeout: later, clearTimeout: id => timers.delete(id),
    performance: { now: () => clock },
    Date: class extends Date { static now() { return clock; } },
  });
  window.window = window;
  window.self = window;
  window.top = window;
  prepare({ state, player, later, writes });
  vm.runInNewContext(script, window, { filename: 'quality.js' });
  function notify(type, target = document) {
    const event = { type, target };
    window.emit(type, event);
    document.emit(type, event);
    if (target !== document) target.emit(type, event);
  }
  flush();
  return { state, player, video, writes, location, flush, notify, timers,
    get scheduledCount() { return nextTimer; } };
}

test('caps a 4K video at the highest playable tier through 1440p', () => {
  const fixture = createPlayer([
    quality('hd1080', '1080p60 HD'), quality('auto', 'Auto'),
    quality('hd2160', '2160p60 4K'), quality('hd1440', '1440p60 HDR'),
  ]);
  assert.equal(fixture.writes.length, 1);
  assert.deepEqual([fixture.writes[0].low, fixture.writes[0].high], ['hd1440', 'hd1440']);
});

test('uses the best lower tier and recognizes SD player tokens', () => {
  for (const tier of ['hd1080', 'hd720', 'large', 'small', 'tiny']) {
    const fixture = createPlayer([
      quality('highres', '4320p60'), quality('auto', 'Auto'), quality(tier, ''),
    ]);
    assert.equal(fixture.writes.at(-1)?.low, tier);
  }
});

const regular = quality('hd1080', '1080p', { formatId: 137 });
const premium = quality('hd1080', '1080p Premium', {
  formatId: 356, paygatedQualityDetails: { paygatedIndicatorText: 'Premium' },
});

test('leaves an existing 1440p pin alone across checks and a new video', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')], ({ state }) => {
    state.preferred = 'hd1440';
    state.actual = 'hd1440';
  });
  for (let i = 0; i < 2; i++) {
    fixture.player.emit('onApiChange');
    fixture.flush();
  }
  fixture.location.search = '?v=second';
  fixture.state.videoId = 'second';
  fixture.state.actual = 'medium';
  fixture.notify('yt-navigate-finish');
  fixture.flush();
  assert.equal(fixture.writes.length, 0);
});

test('pins Auto or a lower preference once even if the target is already decoding', () => {
  for (const preference of ['auto', 'hd720']) {
    const fixture = createPlayer([quality('hd1440', '1440p')], ({ state }) => {
      state.preferred = preference;
      state.actual = 'hd1440';
    });
    fixture.notify('playing', fixture.video);
    fixture.flush();
    assert.equal(fixture.state.preferred, 'hd1440');
    assert.equal(fixture.writes.length, 1);
  }
});

test('keeps one request while the player is still decoding a lower resolution', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')], ({ state, player, writes }) => {
    player.setPlaybackQualityRange = (low, high, format) => {
      writes.push({ low, high, format });
      state.preferred = low;
    };
  });
  fixture.player.emit('onPlaybackQualityChange', 'medium');
  fixture.flush();
  assert.equal(fixture.state.actual, 'medium');
  assert.equal(fixture.writes.length, 1);
  assert.equal(fixture.timers.size, 0);
});

test('distinguishes a correct Premium pin from regular 1080p after buffering ends', () => {
  const fixture = createPlayer([regular, premium], ({ state }) => {
    Object.assign(state, { preferred: 'hd1080', actual: 'hd1080', format: String(premium.formatId) });
  });
  fixture.state.actual = 'medium';
  fixture.state.format = 18;
  fixture.notify('resize', fixture.video);
  fixture.flush();
  assert.equal(fixture.writes.length, 0);
  fixture.state.actual = 'hd1080';
  fixture.state.format = regular.formatId;
  fixture.player.emit('onPlaybackQualityChange', 'hd1080');
  fixture.flush();
  assert.deepEqual(fixture.writes, [{ low: 'hd1080', high: 'hd1080', format: premium.formatId }]);
});

test('leaves an equivalent advertised Premium format alone', () => {
  const alternate = { ...premium, formatId: 721 };
  const fixture = createPlayer([regular, premium, alternate], ({ state }) => {
    Object.assign(state, { preferred: 'hd1080', actual: 'hd1080', format: alternate.formatId });
  });
  assert.equal(fixture.writes.length, 0);
});

test('corrects a lower advertised frame rate once within a pinned resolution', () => {
  const faster = quality('hd1080', '1080p60', { formatId: 299 });
  const fixture = createPlayer([regular, faster], ({ state }) => {
    Object.assign(state, { preferred: 'hd1080', actual: 'hd1080', format: regular.formatId });
  });
  assert.deepEqual(fixture.writes, [{ low: 'hd1080', high: 'hd1080', format: faster.formatId }]);
});

test('uses observed playback when a legacy player cannot report the pin', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')], ({ state, player }) => {
    delete player.getPreferredQuality;
    state.actual = 'hd1440';
  });
  assert.equal(fixture.writes.length, 0);
  fixture.state.actual = 'medium';
  fixture.player.emit('onPlaybackQualityChange', 'medium');
  fixture.flush();
  assert.equal(fixture.writes.length, 1);
});

test('requests once per video if the player exposes no quality readback at all', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')], ({ player }) => {
    delete player.getPreferredQuality;
    delete player.getPlaybackQuality;
  });
  fixture.notify('loadedmetadata', fixture.video);
  fixture.flush();
  assert.equal(fixture.writes.length, 1);
  fixture.location.search = '?v=second';
  fixture.state.videoId = 'second';
  fixture.notify('yt-navigate-finish');
  fixture.flush();
  assert.equal(fixture.writes.length, 2);
});

test('uses legacy level tokens when detailed offers are unavailable', () => {
  const fixture = createPlayer([], ({ player }) => {
    delete player.getAvailableQualityData;
    player.getAvailableQualityLevels = () => ['auto', 'small', 'hd1080', 'hd2160'];
  });
  assert.equal(fixture.writes.at(-1)?.low, 'hd1080');
});

test('discovers higher offers that arrive silently during player startup', () => {
  const fixture = createPlayer([quality('medium', '360p')], ({ state, later }) => {
    later(() => state.offers.push(quality('hd1440', '1440p60 HD')), 1500);
  });
  assert.equal(fixture.writes.at(-1)?.low, 'hd1440');
  assert.equal(fixture.timers.size, 0);
});

test('selects offered enhanced bitrate by its format ID within the 1080p tier', () => {
  const fixture = createPlayer([regular, premium, quality('medium', '360p')]);
  assert.deepEqual(fixture.writes.at(-1), { low: 'hd1080', high: 'hd1080', format: 356 });
});

test('restores the Premium format after a downgrade that keeps the same resolution', () => {
  const fixture = createPlayer([regular, premium]);
  fixture.state.format = regular.formatId;
  fixture.player.emit('onPlaybackQualityChange', 'hd1080');
  fixture.flush();
  assert.equal(fixture.state.format, premium.formatId);
});

test('prioritizes resolution and then frame rate for standard offers', () => {
  const fixture = createPlayer([
    regular, premium, quality('hd1440', '1440p', { formatId: 10 }),
    quality('hd1440', '1440p60 HDR', { formatId: 11 }),
  ]);
  assert.deepEqual(fixture.writes.at(-1), { low: 'hd1440', high: 'hd1440', format: 11 });
});

test('skips a locked Premium offer as well as an unplayable higher resolution', () => {
  const locked = { ...premium, isPlayable: false };
  const fixture = createPlayer([locked, regular, quality('hd1440', '1440p', { isPlayable: false })]);
  assert.equal(fixture.writes.at(-1)?.low, 'hd1080');
  assert.ok(fixture.writes.every(write => write.format !== locked.formatId));
});

test('skips unknown, automatic, locked-only and above-cap renditions', () => {
  const fixture = createPlayer([quality('auto', 'Auto'), quality('future', 'unknown'),
    quality('hd2160', '2160p'), { ...premium, isPlayable: false }]);
  assert.deepEqual(fixture.writes, []);
});

test('stops retrying when the player persistently rejects the selected quality', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')], ({ player, later, writes }) => {
    player.setPlaybackQualityRange = (low, high, format) => {
      writes.push({ low, high, format });
      later(() => player.emit('onPlaybackQualityChange', 'medium'), 25);
    };
  });
  assert.ok(fixture.writes.length > 0 && fixture.writes.length <= 5);
  assert.equal(fixture.timers.size, 0);
});

test('retargets the reused player after in-page video navigation', () => {
  const fixture = createPlayer([quality('hd1440', '1440p')]);
  fixture.location.search = '?v=second';
  Object.assign(fixture.state, { videoId: 'second', preferred: 'auto', actual: 'medium',
    offers: [quality('hd720', '720p60'), quality('small', '240p')] });
  fixture.notify('yt-navigate-finish');
  fixture.notify('loadedmetadata', fixture.video);
  fixture.flush();
  assert.equal(fixture.writes.at(-1)?.low, 'hd720');
});

test('recovers a downgraded preference and then stops scheduling work', () => {
  const fixture = createPlayer([quality('medium', '360p'), quality('hd1440', '1440p')]);
  const initialWrites = fixture.writes.length;
  fixture.state.actual = 'medium';
  fixture.state.preferred = 'auto';
  fixture.player.emit('onPlaybackQualityChange', 'medium');
  fixture.flush();
  assert.ok(fixture.writes.length > initialWrites);
  assert.equal(fixture.state.preferred, 'hd1440');
  const settledWrites = fixture.writes.length;
  const settledTimers = fixture.scheduledCount;
  for (let i = 0; i < 120; i++) fixture.notify('timeupdate', fixture.video);
  fixture.flush();
  assert.equal(fixture.writes.length, settledWrites);
  assert.equal(fixture.scheduledCount, settledTimers);
  assert.equal(fixture.timers.size, 0);
});
