// Smoke test for the OrderGroup state machine in QuickJS strategies.
// Mirrors python/tests/test_order_group.py and node/test/test_order_group.js.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// --- OneSided cancels remaining legs after first fill.
{
    var g = new OrderGroup({ parentSignalId: 1, policy: OrderGroupPolicy.OneSided });
    g.addLimitLeg(1, 0, 50000.0, 0.1);
    g.addLimitLeg(2, 1, 3000.0, 1.5);
    g.recordSubmit(0, 200);
    g.recordSubmit(1, 201);
    g.recordFill(0, 0.1);
    var acts = g.recommendedActions();
    check(acts.length === 1, 'OneSided: 1 cancel for unfilled leg');
    check(acts[0].kind === 'cancel', 'OneSided: cancel kind');
    check(acts[0].legIndex === 1, 'OneSided: cancel leg index 1');
    check(acts[0].orderId === 201, 'OneSided: cancel order id 201');
}

// --- AllOrNothing reverts filled legs on failure.
{
    var g = new OrderGroup({ policy: OrderGroupPolicy.AllOrNothing });
    g.addMarketLeg(1, 0, 0.1);
    g.addMarketLeg(2, 1, 2.0);
    g.recordSubmit(0, 300);
    g.recordSubmit(1, 301);
    g.recordFill(0, 0.1);
    g.recordFailure(1);
    check(g.state() === OrderGroupState.Reverting, 'AllOrNothing: state Reverting');
    var acts = g.recommendedActions();
    check(acts.length === 1 && acts[0].kind === 'revert',
          'AllOrNothing: 1 revert action');
    check(acts[0].symbol === 1 && acts[0].side === 1,
          'AllOrNothing: revert opposite side, same symbol');
    check(Math.abs(acts[0].qty - 0.1) < 1e-9,
          'AllOrNothing: revert qty matches fill');
}

// --- Pair-latency budget (OneSided helper).
{
    var g = new OrderGroup({ policy: OrderGroupPolicy.OneSided });
    g.addLimitLeg(1, 0, 50000.0, 0.1);
    g.addLimitLeg(2, 1, 3000.0, 1.5);

    check(g.pairLatencyDecision({ leaderSubmitTsNs: 0, leaderAckTsNs: 0,
                                   ackReceived: false }) === 'wait',
          'budget unset → wait');

    g.setPairLatencyBudgetNs(50_000_000);
    var submit = 1_000_000_000;

    check(g.pairLatencyDecision({ leaderSubmitTsNs: submit,
                                   leaderAckTsNs: submit + 30_000_000,
                                   ackReceived: true }) === 'submit_follower',
          'ack within budget → submit_follower');
    check(g.pairLatencyDecision({ leaderSubmitTsNs: submit,
                                   leaderAckTsNs: submit + 60_000_000,
                                   ackReceived: true }) === 'cancel_leader',
          'ack over budget → cancel_leader');
    check(g.pairLatencyDecision({ leaderSubmitTsNs: submit,
                                   leaderAckTsNs: submit + 10_000_000,
                                   ackReceived: false }) === 'wait',
          'no ack, still inside budget → wait');
    check(g.pairLatencyDecision({ leaderSubmitTsNs: submit,
                                   leaderAckTsNs: submit + 80_000_000,
                                   ackReceived: false }) === 'cancel_leader',
          'no ack, past budget → cancel_leader (timeout)');
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('order_group smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
