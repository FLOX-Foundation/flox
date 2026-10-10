/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
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
#include "lrvx-venue/checkpoint_lane.h"
#include "lrvx-venue/event_hash.h"
#include "lrvx-venue/journal.h"
#include "lrvx-venue/sequenced_shard.h"
#include "lrvx/util/crc32.h"
#include "lrvx/util/file_io.h"

// engine and its components
#include "lrvx-venue/ledger.h"
#include "lrvx-venue/matcher.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"
#include "lrvx-venue/messages.h"
#include "lrvx-venue/reject_reason.h"
#include "lrvx-venue/session.h"
#include "lrvx/book/ladder_book.h"

// perimeter
#include "lrvx-venue/control_api.h"
#include "lrvx-venue/control_plane.h"
#include "lrvx-venue/fix_codec.h"
#include "lrvx-venue/fix_session.h"
#include "lrvx-venue/metrics.h"
#include "lrvx-venue/metrics_server.h"
#include "lrvx-venue/prometheus.h"
#include "lrvx-venue/session_registry.h"
#include "lrvx-venue/shard_events.h"
#include "lrvx-venue/socket_acceptor.h"
#include "lrvx-venue/tcp_gateway.h"

// FIX initiator
#include "lrvx/connector/fix/fix_client_codec.h"
#include "lrvx/connector/fix/fix_initiator.h"
#include "lrvx/connector/fix/fix_tcp_client.h"
#include "lrvx/connector/fix/fix_wire.h"

// utilities
#include "lrvx/common.h"
#include "lrvx/log/console_logger.h"
#include "lrvx/net/receive_path.h"
#include "lrvx/net/socket.h"
#include "lrvx/util/base/time.h"
#include "lrvx/util/concurrency/thread_body.h"
#include "lrvx/util/decimal_wire.h"
#include "lrvx/util/eventing/wake_set.h"
#include "lrvx/util/performance/busy_backoff.h"
#include "support/engine_surface.h"

int main()
{
  return 0;
}
