// W15-T004 — multi-leg order group state machine, NAPI parity test.

const flox = require('..');
const { OrderGroupPolicy, OrderGroupState } = flox;

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

check('OrderGroupPolicy exposes named constants',
      OrderGroupPolicy.BestEffort === 'BestEffort'
      && OrderGroupPolicy.AllOrNothing === 'AllOrNothing'
      && OrderGroupPolicy.OneSided === 'OneSided');

// --- BestEffort: nothing reverts; leg state reflects events as-is.
{
  const g = new flox.OrderGroup({ parentSignalId: 1, policy: OrderGroupPolicy.BestEffort });
  const a = g.addMarketLeg(1, 0, 0.1);  // BTC buy
  const b = g.addMarketLeg(2, 1, 2.0);  // ETH sell
  check('legCount = 2', g.legCount() === 2);
  check('initial state Pending', g.state() === OrderGroupState.Pending);
  g.recordSubmit(a, 100);
  g.recordSubmit(b, 101);
  check('post-submit state Submitted', g.state() === OrderGroupState.Submitted);
  g.recordFill(a, 0.1);
  g.recordFailure(b);
  check('partial fill + failure → PartiallyFilled', g.state() === OrderGroupState.PartiallyFilled);
  check('BestEffort recommends nothing', g.recommendedActions().length === 0);
  check('legState returns string', g.legState(0) === 'Filled' && g.legState(1) === 'Failed');
}

// --- OneSided via string policy.
{
  const g = new flox.OrderGroup({ parentSignalId: 2, policy: 'OneSided' });
  g.addLimitLeg(1, 0, 50000.0, 0.1);
  g.addLimitLeg(2, 1, 3000.0, 1.5);
  g.recordSubmit(0, 200);
  g.recordSubmit(1, 201);
  g.recordFill(0, 0.1);
  const acts = g.recommendedActions();
  check('OneSided: 1 cancel for unfilled leg', acts.length === 1);
  check('OneSided: cancel kind', acts[0].kind === 'cancel');
  check('OneSided: cancel leg index 1', acts[0].legIndex === 1);
  check('OneSided: cancel order id 201', acts[0].orderId === 201);
}

// --- AllOrNothing using the constant.
{
  const g = new flox.OrderGroup({ parentSignalId: 3, policy: OrderGroupPolicy.AllOrNothing });
  g.addMarketLeg(1, 0, 0.1);
  g.addMarketLeg(2, 1, 2.0);
  g.recordSubmit(0, 300);
  g.recordSubmit(1, 301);
  g.recordFill(0, 0.1);
  g.recordFailure(1);
  check('AllOrNothing: state Reverting', g.state() === OrderGroupState.Reverting);
  const acts = g.recommendedActions();
  check('AllOrNothing: 1 revert action for filled leg', acts.length === 1);
  check('AllOrNothing: revert kind', acts[0].kind === 'revert');
  check('AllOrNothing: revert opposite side', acts[0].side === 1);
  check('AllOrNothing: revert symbol matches', acts[0].symbol === 1);
  check('AllOrNothing: revert qty matches fill', Math.abs(acts[0].qty - 0.1) < 1e-9);
}

// --- Bad policy string throws.
{
  let threw = false;
  try {
    new flox.OrderGroup({ policy: 'Garbage' });
  } catch (e) { threw = true; }
  check('unknown policy string throws', threw);
}

console.log('node order_group test ok');
