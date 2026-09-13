// W6-T021 — multi-feed clock NAPI parity test.

const flox = require('..');
const { FeedClockPolicy, MultiFeedClock } = flox;

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

check('FeedClockPolicy constants',
      FeedClockPolicy.WaitForAll === 'WaitForAll'
      && FeedClockPolicy.FireOnAny === 'FireOnAny'
      && FeedClockPolicy.LeaderFollower === 'LeaderFollower');

const BTC = 1, ETH = 2;
const SECOND_NS = 1_000_000_000;

// --- WaitForAll: fires when both feeds have ticked since last fire.
{
  const c = new MultiFeedClock({
    symbols: [BTC, ETH],
    policy: FeedClockPolicy.WaitForAll,
    timeoutMs: 200,
  });
  const r1 = c.tick(SECOND_NS, BTC);
  check('WaitForAll: BTC alone does not fire', r1.fired === false);
  const r2 = c.tick(SECOND_NS + 50_000_000, ETH);
  check('WaitForAll: ETH after BTC fires', r2.fired === true);
  check('WaitForAll: triggeredBy = ETH', r2.triggeredBy === ETH);
  // After fire, accumulator resets — BTC alone again does not fire.
  const r3 = c.tick(SECOND_NS + 100_000_000, BTC);
  check('WaitForAll: post-fire reset', r3.fired === false);
}

// --- FireOnAny via string policy.
{
  const c = new MultiFeedClock({ symbols: [BTC, ETH], policy: 'FireOnAny' });
  const r1 = c.tick(SECOND_NS, BTC);
  check('FireOnAny: every tick fires', r1.fired === true);
  const r2 = c.tick(SECOND_NS + 1, ETH);
  check('FireOnAny: ETH also fires', r2.fired === true);
}

// --- LeaderFollower with named constant.
{
  const c = new MultiFeedClock({
    symbols: [BTC, ETH],
    policy: FeedClockPolicy.LeaderFollower,
    leaderSymbol: BTC,
    stalenessBudgetMs: 200,
  });
  const r1 = c.tick(SECOND_NS, ETH);
  check('LeaderFollower: follower alone does not fire', r1.fired === false);
  const r2 = c.tick(SECOND_NS + 50_000_000, BTC);
  check('LeaderFollower: leader with fresh follower fires', r2.fired === true);
  check('LeaderFollower: triggeredBy = leader', r2.triggeredBy === BTC);
  const r3 = c.tick(SECOND_NS + 500_000_000, BTC);
  check('LeaderFollower: stale follower blocks fire', r3.fired === false);
}

// --- Snapshot reports staleness.
{
  const c = new MultiFeedClock({ symbols: [BTC, ETH], policy: FeedClockPolicy.WaitForAll });
  c.tick(SECOND_NS, BTC);
  const r2 = c.tick(SECOND_NS + 100_000_000, ETH);
  check('snapshot staleness BTC = 100ms', r2.stalenessNs[BTC] === 100_000_000);
  check('snapshot staleness ETH = 0', r2.stalenessNs[ETH] === 0);
}

// --- Bad policy throws.
{
  let threw = false;
  try { new MultiFeedClock({ symbols: [BTC], policy: 'Garbage' }); } catch (e) { threw = true; }
  check('unknown policy string throws', threw);
}

// --- BOOK-11: the WaitForAll timeout fallback must fire even though the
// second feed never ticks at all (previously required one full fire first).
{
  const c = new MultiFeedClock({
    symbols: [BTC, ETH],
    policy: FeedClockPolicy.WaitForAll,
    timeoutMs: 200,
  });
  let fired = 0;
  for (let i = 0; i < 1000; ++i) {
    if (c.tick(i * (SECOND_NS / 10), BTC).fired) fired++;
  }
  check('WaitForAll: timeout fallback fires before any full fire', fired > 0);
}

// --- BOOK-11: a first full fire landing exactly on ts=0 must not disable
// the timeout permanently.
{
  const c = new MultiFeedClock({
    symbols: [BTC, ETH],
    policy: FeedClockPolicy.WaitForAll,
    timeoutMs: 200,
  });
  check('zero-ts fire: BTC alone does not fire', c.tick(0, BTC).fired === false);
  check('zero-ts fire: ETH completes the fire at ts=0', c.tick(0, ETH).fired === true);

  let fired = 0;
  for (let i = 1; i <= 10; ++i) {
    if (c.tick(i * 300_000_000, BTC).fired) fired++;
  }
  check('WaitForAll: timeout still works after a ts=0 first fire', fired === 10);
}

// --- BOOK-11: an out-of-band symbol never fires and does not affect a
// registered symbol's last-seen timestamp.
{
  const c = new MultiFeedClock({
    symbols: [BTC, ETH],
    policy: FeedClockPolicy.WaitForAll,
    timeoutMs: 200,
  });
  const UNREGISTERED = 99;
  c.tick(SECOND_NS, BTC);
  const oob = c.tick(SECOND_NS + 1, UNREGISTERED);
  check('out-of-band tick never fires', oob.fired === false);
  check('out-of-band triggeredBy echoes the symbol', oob.triggeredBy === UNREGISTERED);
  const after = c.tick(SECOND_NS + 2, BTC);
  check('out-of-band tick does not affect registered last-seen', after.lastTsNs[BTC] === SECOND_NS + 2);
}

console.log('node feed_clock test ok');
