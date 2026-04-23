const path = require('path');

const key = `${process.platform}-${process.arch}`;
const prebuilt = path.join(__dirname, 'prebuilds', key, 'flox_node.node');
const local = path.join(__dirname, 'build', 'Release', 'flox_node.node');

try {
  module.exports = require(prebuilt);
} catch (_) {
  module.exports = require(local);
}
