/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/connector/abstract_exchange_connector.h"
#include "flox/connector/connector_factory.h"

#include <gtest/gtest.h>

using namespace flox;

class DummyConnector : public IExchangeConnector
{
 public:
  void start() override {}
  void stop() override {}
  std::string exchangeId() const override { return "dummy"; }
};

TEST(ConnectorFactoryTest, RegisterAndCreateConnector)
{
  ConnectorFactory::instance().registerConnector(
      "dummy", [](const std::string& symbol)
      { return std::make_shared<DummyConnector>(); });

  auto connector = ConnectorFactory::instance().createConnector("dummy", "BTCUSDT");
  ASSERT_NE(connector, nullptr);
  EXPECT_EQ(connector->exchangeId(), "dummy");
}

TEST(ConnectorFactoryTest, UnknownConnectorReturnsNullptr)
{
  auto connector = ConnectorFactory::instance().createConnector("unknown", "BTCUSDT");
  EXPECT_EQ(connector, nullptr);
}
