/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/connector/abstract_exchange_connector.h"
#include "lrvx/connector/connector_factory.h"

#include <gtest/gtest.h>

using namespace lrvx;

class DummyConnector : public IExchangeConnector
{
 public:
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
