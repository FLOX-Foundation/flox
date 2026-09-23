/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The surface contract for the venue-lite build profile.
 *
 * One translation unit that includes exactly what a downstream consumer
 * running the execution venue includes -- the list in venue/lite_surface.txt,
 * nothing added, nothing left out -- plus engine_surface.h, which pins the
 * shape of MatchingEngine's public members.
 *
 * What it catches that the venue's own tests do not: the venue tests include
 * what they happen to need, so a header that moves, splits, or grows a
 * dependency the lite profile forbids can keep every one of them green and
 * still break the consumer's build. This TU is compiled in the lite profile
 * only, where the backtest module, the exchange connectors and the bindings
 * are not configured at all -- so it fails here rather than at the consumer.
 *
 * The list is single-sourced: scripts/lite_closure.py checks that the
 * includes below are exactly the entries of venue/lite_surface.txt, and the
 * lite configure runs it. Removing an entry from the list -- or adding an
 * include here that is not on it -- fails the configure and this target with
 * it.
 *
 * There is nothing to run. The contract is that this compiles.
 */

// sequencer and storage
#include "flox-venue/checkpoint_lane.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/sequenced_shard.h"
#include "flox/util/crc32.h"
#include "flox/util/file_io.h"

// engine and its components
#include "flox-venue/ledger.h"
#include "flox-venue/matcher.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"
#include "flox-venue/reject_reason.h"
#include "flox-venue/session.h"
#include "flox/book/ladder_book.h"

// perimeter
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/metrics.h"
#include "flox-venue/metrics_server.h"
#include "flox-venue/prometheus.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/shard_events.h"
#include "flox-venue/socket_acceptor.h"
#include "flox-venue/tcp_gateway.h"

// FIX initiator
#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_tcp_client.h"
#include "flox/connector/fix/fix_wire.h"

// utilities
#include "flox/common.h"
#include "flox/log/console_logger.h"
#include "flox/net/receive_path.h"
#include "flox/net/socket.h"
#include "flox/util/base/time.h"
#include "flox/util/concurrency/thread_body.h"
#include "flox/util/decimal_wire.h"
#include "flox/util/eventing/wake_set.h"
#include "flox/util/performance/busy_backoff.h"
#include "support/engine_surface.h"

int main()
{
  return 0;
}
